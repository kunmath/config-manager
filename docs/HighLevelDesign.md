# ConfigManager High-Level Design

This document translates `Architecture.md` into an implementable design. It is
intentionally **data-model and API focused**: it defines the concrete C++ types,
the internal storage model behind `ConfigModel`, the public API surface of every
component, and the build/dependency strategy.

It does not restate architectural rationale already captured in the ADRs; it
shows *how* those decisions are realized in code.

---

## 1. Guiding Constraints

These constraints come directly from the architecture and drive the design:

| Constraint | Design consequence |
|---|---|
| Public APIs never throw | Every fallible boundary returns `Result<T>` |
| `ConfigModel` holds data only | No `VersionId` anywhere inside the model |
| `VersionedConfig` is the single source of truth | Migration/sync APIs take only a *target* version |
| Storage agnostic | Backends operate on `std::istream` / `std::ostream` |
| Nodes are stable handles | Internal arena with generation-checked slots |
| Synchronization is transactional | Migration + repair run on a working copy, one commit point |
| No vcpkg dependency for consumers | CMake `FetchContent` + `find_package` fallback, header-only deps, exported package config |

---

## 2. Namespace, Language, and File Layout

* **Language:** C++20.
* **Root namespace:** `configmanager` (alias `cfg` provided in a single header).
* **Header style:** one public header per component, aggregated by `configmanager/configmanager.hpp`.

```text
config-manager/
├── CMakeLists.txt
├── cmake/
│   ├── ConfigManagerConfig.cmake.in     # downstream find_package() entry
│   └── Dependencies.cmake               # FetchContent + find_package fallback
├── include/configmanager/
│   ├── configmanager.hpp                # umbrella include
│   ├── result.hpp                       # Result, Error, ErrorCode
│   ├── version.hpp                      # VersionId, VersionArtifact
│   ├── config_value.hpp                 # ConfigValue, NodeType, scalar variant
│   ├── config_node.hpp                  # ConfigNode handle
│   ├── config_model.hpp                 # ConfigModel (tree + path access)
│   ├── config_path.hpp                  # ConfigPath parser + segments
│   ├── versioned_config.hpp            # VersionedConfig
│   ├── config_interface.hpp             # IConfigInterface
│   ├── version_catalog.hpp              # VersionCatalog
│   ├── migration_registry.hpp           # MigrationRegistry
│   ├── migration_engine.hpp             # MigrationEngine
│   └── config_runtime.hpp               # ConfigRuntime, SyncState, SyncStatus
├── src/                                 # implementation (.cpp) mirroring headers
│   └── ...
├── backends/                            # OPTIONAL, per-format targets
│   ├── json/                            # configmanager::json  (nlohmann/json)
│   ├── yaml/                            # configmanager::yaml  (yaml-cpp)
│   ├── xml/                             # configmanager::xml   (pugixml)
│   └── ini/                             # configmanager::ini   (no external dep)
└── tests/
    └── ...
```

**Core stays dependency-light.** `configmanager::core` depends only on a
header-only `Result` backend (`tl::expected`). Every format parser lives in a
separate optional target so an application that only needs versioning/migration
never pulls a JSON/YAML/XML library.

---

## 3. Foundational Types

### 3.1 Result / Error (`result.hpp`)

```cpp
namespace configmanager {

enum class ErrorCode {
    InvalidPath,
    NodeNotFound,
    InvalidType,
    ParseError,
    SerializationError,
    MigrationFailed,
    MissingMigration,
    InvalidVersion,
};

struct Error {
    ErrorCode   code;
    std::string message;   // owned: richer diagnostics, no lifetime issues
};

template <typename T>
using Result = tl::expected<T, Error>;

// Helpers to keep call sites terse and consistent.
[[nodiscard]] tl::unexpected<Error> fail(ErrorCode code, std::string message);
}
```

* `Result<void>` is used for operations that either succeed or fail.
* A single migration path is provided to swap `tl::expected` for `std::expected`
  later: only `result.hpp` references the underlying type.
* Non-throwing boundary (ADR-018): exceptions other than `std::bad_alloc`
  from user-supplied callbacks (`MigrationFn`, `DefaultFactory`) are caught at
  the invocation site and mapped to `MigrationFailed`. Exceptions derived from
  `std::exception` preserve `what()` in `Error::message`; any other thrown
  type gets the fixed message "unknown exception from user callback". The
  boundary catches `std::bad_alloc` first and rethrows it: memory exhaustion
  is not a recoverable configuration error and may propagate from anywhere,
  including callbacks. The same catch discipline applies at the serialization
  boundary (§6): exceptions from parser libraries or stream operations inside
  `load()` map to `ParseError`, inside `save()` to `SerializationError`.

### 3.2 Versioning (`version.hpp`)

```cpp
namespace configmanager {

using VersionId = std::uint32_t;          // monotonic, application-defined

using DefaultFactory = std::function<ConfigValue()>;

struct VersionArtifact {
    VersionId      version;
    DefaultFactory defaultFactory;        // produces a fully-populated default tree
};
}
```

`DefaultFactory` returns a `ConfigValue` (the value-semantic tree, §4.1);
`VersionCatalog::createDefault` adopts it into a `ConfigModel` via
`ConfigModel::fromValue`, so there is exactly one conversion path between the
two tree representations. The returned tree must be object-rooted (§4.1) and
use path-addressable keys (ADR-021); a factory that throws is caught and
mapped to `MigrationFailed` (ADR-018).

---

## 4. The Configuration Data Model

This is the heart of the library. The model is a JSON-like document tree
(objects, arrays, scalars). The design satisfies two architectural promises:

1. `ConfigModel` *owns all storage*.
2. `ConfigNode` is a *lightweight handle* that survives moves but is invalidated
   by removal.

The root of a model is always an `Object` (ADR-020): `ConfigModel()` starts as
an empty root object, `fromValue` rejects non-object roots with `InvalidType`,
and backends reject non-object document roots with `ParseError`.

### 4.1 Value taxonomy (`config_value.hpp`)

```cpp
namespace configmanager {

enum class NodeType {
    Null,
    Bool,
    Int,        // std::int64_t
    Double,
    String,
    Object,     // ordered key -> child
    Array,      // ordered child list
};

using Scalar = std::variant<
    std::monostate,   // Null
    bool,
    std::int64_t,
    double,
    std::string>;
}
```

`ConfigValue` (the public, copyable, owning representation) is used for default
factories and for inserting subtrees:

```cpp
class ConfigValue {            // recursive, value-semantic
public:
    static ConfigValue object();
    static ConfigValue array();
    template <typename T> static ConfigValue of(T scalar);

    NodeType type() const noexcept;
    // object/array builders used by default factories & migrations
    ConfigValue& set(std::string key, ConfigValue child);   // object
    ConfigValue& push(ConfigValue child);                   // array
    // ...
private:
    Scalar                                            scalar_;
    std::vector<std::pair<std::string, ConfigValue>>  object_;  // insertion-ordered
    std::vector<ConfigValue>                          array_;
};
```

Object members are stored in **insertion order** — the same ordering rule the
arena uses (§4.2) — so there is exactly one member-ordering rule in the library
(ADR-022). A document keeps its author's key order through load → mutate →
save, and a subtree round-tripped through `getValue`/`set` preserves member
order; `ConfigValue::set` on an existing key replaces the value in place
without moving the member. One ordering rule everywhere keeps serialization
and repair **deterministic**, which matters for diffing and reproducible
migrations.

`set`/`push` are builder **preconditions**, not fallible operations: `set`
requires an `Object`, `push` requires an `Array`. Violating a precondition is
a programming error (debug assertion; otherwise undefined behavior), not a
recoverable failure — `ConfigValue` is a builder driven by factory and
migration code, never by external data. `of()`'s two data-dependent
preconditions — an unsigned value above `INT64_MAX`, a null C string — throw
a logic error in **every** build rather than silently corrupting the value:
inside a default factory the catalog boundary maps the exception to
`MigrationFailed` (ADR-018), and `ConfigModel::set` checks both before
calling `of()`, so the throw never crosses the `Result` API. The builder does
not validate keys either; key validity (non-empty, no reserved characters) is
enforced where a value crosses into a model: `fromValue` and subtree `set`
(ADR-021).

### 4.2 Internal storage: a generation-checked arena

`ConfigModel` does **not** store `ConfigValue` directly. It stores a flat arena
of nodes addressed by a stable index. This is what makes `ConfigNode` a cheap,
move-stable handle (see Architecture §"ConfigNode Lifetime").

```cpp
// internal (src/), not part of the public header
using NodeId = std::uint32_t;
struct NodeId_None { static constexpr NodeId value = 0xFFFFFFFF; };

struct Node {
    NodeType                                  type;
    Scalar                                    scalar;     // when scalar type
    std::vector<std::pair<std::string,NodeId>> members;   // when Object (ordered)
    std::vector<NodeId>                        elements;   // when Array
    NodeId                                     parent;
    std::uint32_t                              generation; // bumped on free
    bool                                       alive;
};

class NodeArena {
    std::vector<Node>          nodes_;
    std::vector<NodeId>        freeList_;
    // allocate(): reuse a free slot (bump generation) or append
    // free(node): mark dead, bump generation, return slot to freeList_
};
```

**Why this satisfies the lifetime contract:**

* *Moving / reparenting a node* changes only `parent`/`members`, never its
  `NodeId` → existing handles stay valid. No v1 public operation reparents a
  node; the invariant exists at the storage level and future-proofs
  move/rename APIs (§4.4).
* *Removing a node* frees the slot and bumps `generation` → any handle holding
  the old generation is detectably stale (invalidated).
* The arena lives on the heap (`std::unique_ptr<NodeArena>` member of
  `ConfigModel`), and handles point at the **arena**, never at the
  `ConfigModel` object (§4.3). *Moving the `ConfigModel` object* transfers the
  arena pointer and keeps every handle valid — handles follow the new owner.
* *Destroying* a model — including move-assigning another model onto it —
  destroys the arena and invalidates every handle into it **undetectably**
  (the same contract as container iterators). Because a committing
  `synchronize()` move-assigns the working copy onto the caller's config, it
  invalidates all handles previously obtained from that configuration.

### 4.3 ConfigNode handle (`config_node.hpp`)

```cpp
class ConfigNode {
public:
    bool        valid() const noexcept;      // checks generation against arena
    NodeType    type()  const noexcept;

    template <typename T> Result<T> as() const;           // scalar read
    Result<ConfigNode>     child(std::string_view key) const;  // object
    Result<ConfigNode>     at(std::size_t index)        const; // array
    std::size_t            size() const noexcept;              // object/array
    Result<std::vector<std::string>> keys() const;             // object: member names, in order

private:
    const NodeArena*   arena_    = nullptr;   // stable across ConfigModel moves
    NodeId             id_       = NodeId_None::value;
    std::uint32_t      generation_ = 0;   // must match arena slot to be valid
};
```

A `ConfigNode` is a 16-byte handle. Validity is verified lazily by comparing
`generation_` with the arena slot's current generation.

Accessor contract:

* `valid()` is safe to call while the owning model is alive — it dereferences
  the arena, so it cannot outlive it. After the model is destroyed (including
  by move-assignment onto it or a committing `synchronize()`), no `ConfigNode`
  member is safe, `valid()` included: that lifetime boundary is undetectable
  (§4.2) and use is undefined behavior, as with container iterators.
* The `Result`-returning accessors are total: on a **stale** handle they fail
  with `NodeNotFound` (the node is gone); on a type mismatch — `child()` on a
  non-object, `at()` on a non-array, `keys()` on a non-object, `as<T>()` on a
  non-scalar or unconvertible scalar — they fail with `InvalidType`. A
  missing key or out-of-range index is `NodeNotFound`.
* The two `noexcept` accessors are preconditioned on `valid()`: calling
  `type()` or `size()` on an invalid handle is a programming error (debug
  assertion; otherwise undefined behavior). `size()` returns the
  member/element count for objects/arrays and `0` for scalars.

`ConfigNode` is a **read-only** handle: every accessor is `const`, so const
models can be traversed — repair reads the defaults model exactly this way.
All mutation goes through `ConfigModel`.

The handle references the **arena**, not the `ConfigModel` object. This is
what makes the move-stability contract implementable: the arena's heap address
is stable when the owning model is moved, while the model object's own address
is not. A handle holding a `ConfigModel*` would dangle on every move.

### 4.4 ConfigModel public API (`config_model.hpp`)

`ConfigModel` is move-only (it owns the arena). Path-based access is the primary
ergonomic surface; node handles are the lower-level surface.

```cpp
class ConfigModel {
public:
    ConfigModel();                       // empty root object
    static Result<ConfigModel> fromValue(ConfigValue root);  // non-object root => InvalidType
    ConfigModel(ConfigModel&&) noexcept;
    ConfigModel& operator=(ConfigModel&&) noexcept;

    ConfigNode root() const;

    // ---- Path-based access (primary API) ----
    template <typename T>
    Result<T>    get(std::string_view path) const;        // typed read

    Result<ConfigValue> getValue(std::string_view path) const; // subtree read (deep copy)

    template <typename T>
    Result<void> set(std::string_view path, T value);     // upsert (creates parents)

    bool         contains(std::string_view path) const;
    Result<void> remove(std::string_view path);           // invalidates handles
    Result<ConfigNode> nodeAt(std::string_view path) const;

    // ---- Subtree insertion (for defaults & migrations) ----
    Result<void> set(std::string_view path, ConfigValue subtree);

    // ---- Whole-model ----
    ConfigModel  clone() const;          // deep copy of the arena (working copies)
};
```

Key behaviors:

* `set` uses **upsert semantics** (Architecture §Path Semantics): missing
  intermediate objects/arrays are created. `set("network.timeout", 10)` creates
  `network` first.
* Upsert creates structure but **never changes type** (ADR-019): a path
  segment conflicting with an existing node's type (e.g. `network` exists as
  a string) fails with `InvalidType` and modifies nothing. Migrations that
  change a node's type remove it explicitly first.
* The rule covers the **final** node too: a write to an existing node
  succeeds only when the new value's type equals the node's current type.
  Same-type writes update in place — the target keeps its `NodeId`, so
  handles to it stay valid; a same-type subtree write replaces the node's
  *contents*, removing all previous descendants and detectably invalidating
  their handles. Any cross-type write (`set("a", 1)` while `a` is an object)
  fails with `InvalidType`; remove first.
* Array indices are bounded: a write may target an existing element or one
  past the end (append). Larger indices fail with `NodeNotFound`; missing
  intermediate arrays are created empty. Holes are never fabricated.
* Writes are **atomic** (ADR-019): `set` validates the entire path against
  the existing tree before any mutation, so a failed write — `InvalidPath`,
  `InvalidType`, or `NodeNotFound` — leaves the model unmodified.
  `set("users[2].name", v)` with `users` absent fails without leaving behind
  an empty `users` array.
* `contains()` never fails: it returns `false` for malformed paths as well as
  absent ones.
* `get<T>` returns `InvalidType` if the stored scalar cannot yield `T`,
  `NodeNotFound` if the path is absent, `InvalidPath` if the path is malformed.
* Scalar conversions are **strict and lossless-only**: `Int` → `Double` only
  when the value is exactly representable as a double; `Double` → `Int` only
  when the value is integral and within range of the integer type; `Bool` and
  `String` never convert; every other combination is `InvalidType`. Values are
  never stringified and strings are never parsed into numbers. `Int` is stored
  as `std::int64_t`; reads into narrower or differently signed integer types
  succeed only when the value is exactly representable in the requested type.
* There is **no dedicated rename/move API**. A rename composes from the common
  flow shared by all operations: `getValue(from)` → `set(to, value)` →
  `remove(from)`. The composition **copies**: handles into the removed source
  subtree are invalidated (detectably) and do not carry over to the copy at
  `to`. Arena-level move stability (§4.2) is not observable through this
  composition.
* The scalar `set<T>` template is constrained to exclude `ConfigValue`, so
  subtree insertion unambiguously selects the `ConfigValue` overload.
* `fromValue` rejects a non-object root with `InvalidType` and any object key
  that is not path-addressable — empty, or containing a reserved path
  character — with `InvalidPath` (ADR-021); the subtree `set` overload
  applies the same key check to the inserted value.
* Tree depth is **bounded by `kMaxTreeDepth` (128)**, the root object at
  depth 0. Several operations — subtree writes, extraction, destruction,
  repair, backend serialization — recurse over the tree, so depth must be
  bounded for them to be stack-safe. `fromValue` and the subtree `set` reject
  deeper values with `InvalidPath` (for `set`, the inserted value's depth is
  offset by the target path's segment count); backends reject deeper
  persisted documents with `ParseError` before they reach a model. The
  checks run while descending, so validation itself never recurses past the
  limit even on a hostile input.
* `clone()` provides the deep copy that `ConfigRuntime::synchronize` runs on.

### 4.5 ConfigPath (`config_path.hpp`)

Parsing is isolated so every component shares one grammar implementation
(Architecture §ConfigPath Grammar).

```cpp
struct PathSegment {
    enum class Kind { Key, Index } kind;
    std::string key;        // when Key
    std::size_t index = 0;  // when Index
};

class ConfigPath {
public:
    static Result<ConfigPath> parse(std::string_view text);  // InvalidPath on error
    const std::vector<PathSegment>& segments() const noexcept;
};
```

The complete grammar:

```text
path    := segment ( "." segment )*
segment := key index*
key     := char+            ; char = any byte except "." "[" "]"
index   := "[" digit+ "]"   ; decimal digits only
```

* Supports `object.key`, `arr[0]`, and nesting (`groups[0].users[4].name`).
* Reserved characters: `.` `[` `]`. **No escaping in v1** (explicit non-goal).
* The grammar is total — anything it does not produce is `InvalidPath`:
  empty keys (`""`, `a.`, `.a`, `a..b`), a leading index (`[0]` — the root
  is an object), empty or non-decimal indices (`arr[]`, `arr[-1]`,
  `arr[1x]`), and text directly after `]` other than `.`, `[`, or
  end-of-path (`a[0]b`). Whitespace is never trimmed — it is an ordinary key
  character. Indices are decimal, leading zeros permitted; a value that
  overflows `std::size_t` is `InvalidPath`.
* The empty string is invalid (`InvalidPath`). No string path addresses the
  root node; the root is reached via `ConfigModel::root()`.
* A key that is empty or contains a reserved character is unaddressable: there
  is no escaping, and the empty string is not a valid segment. Such keys are
  rejected at the boundaries (ADR-021): `load()` fails with `ParseError`,
  `fromValue`/subtree `set` fail with `InvalidPath`.
* Traversal logic in `ConfigModel` consumes `ConfigPath::segments()`, applying
  upsert on write and existence checks on read.

---

## 5. VersionedConfig (`versioned_config.hpp`)

```cpp
struct VersionedConfig {
    VersionId   version;     // single source of truth
    ConfigModel model;
};
```

Move-only (because `ConfigModel` is). This is the unit that flows through
`load` → `inspect` → `synchronize` → `save`.

---

## 6. Serialization Boundary (`config_interface.hpp`)

Reproduced from the architecture; the contract is the design.

```cpp
class IConfigInterface {
public:
    virtual ~IConfigInterface() = default;

    virtual Result<VersionedConfig> load(std::istream& in)              = 0;
    virtual Result<void>            save(const VersionedConfig& cfg,
                                         std::ostream& out)             = 0;
};
```

Design rules enforced by the boundary:

* `load()` **only** parses (no repair, no migration) — ADR-013.
* The version is **mandatory**: if a stream carries no version metadata,
  `load()` fails with `InvalidVersion`. Backends never guess or assume a
  version for unversioned data — ADR-014.
* Each backend defines how the version is embedded/extracted in its own format;
  the library imposes **no common envelope** across formats.
* The version carrier is **reserved** (ADR-020): `load()` consumes it — it
  never appears in the resulting model — and `save()` writes it from
  `VersionedConfig::version`. A model that already contains the reserved
  carrier (e.g. a `__version` key) fails `save()` with `SerializationError`.
* The model root is always an object: documents with a non-object root (e.g.
  a top-level JSON array) fail `load()` with `ParseError`.
* Object keys that are not path-addressable — empty, or containing `.` `[`
  `]` — fail `load()` with `ParseError` (ADR-021): v1 paths have no escaping
  and the empty string is not a valid path, so such keys could never be
  addressed, mutated, or repaired.
* Backends are a non-throwing boundary (ADR-018): exceptions from parser
  libraries or stream operations (including streams with exception masks set)
  are caught inside the backend and mapped to `ParseError` in `load()` /
  `SerializationError` in `save()`, with the standard catch order
  (`std::bad_alloc` rethrown first, `std::exception` preserves `what()`,
  catch-all gets a fixed fallback message).
* Backends live in `backends/<fmt>` as independent CMake targets. Errors map to
  `ParseError` / `SerializationError`.

| Target | Library (header-only?) | Version encoding | Reserved model path (`save()` conflict) |
|---|---|---|---|
| `configmanager::json` | nlohmann/json, `ordered_json` (header-only) | top-level `"__version"` field | top-level `__version` key |
| `configmanager::yaml` | yaml-cpp | top-level `version:` key | top-level `version` key |
| `configmanager::xml`  | pugixml (header+small src) | root attribute `version="N"` | none — attributes are outside the model's key space |
| `configmanager::ini`  | none (hand-written) | `[meta] version=N` section | `meta.version` |

The reserved model path is the exact location whose presence in the model
makes `save()` fail with `SerializationError` (ADR-020). The XML carrier is a
root *attribute*, which no model path can express, so its conflict check is
vacuous.

ADR-022 (insertion order end-to-end) constrains parser choice: the JSON
backend must use `nlohmann::ordered_json` — default `nlohmann::json` sorts
object keys alphabetically and would silently reorder documents. yaml-cpp and
pugixml already preserve document order; the hand-written INI backend does the
same.

### 6.1 Format mappings

JSON and YAML express the model natively; XML and INI cannot, so their
mappings are restricted. Two rules close every gap deterministically: a model
unrepresentable in a format fails `save()` with `SerializationError`, and a
document unrepresentable in the model fails `load()` with `ParseError`.
Backends never guess.

Object keys are unique in every format: a document with duplicates —
duplicate JSON/YAML keys, repeated sibling element names under an XML object
element, duplicate INI keys within a section or repeated section headers —
fails `load()` with `ParseError`. Duplicates are never resolved last-wins.

The version carrier itself parses strictly: the value must be an unsigned
decimal integer representable in `VersionId` (`std::uint32_t`). In JSON and
YAML it must be a native unquoted integer scalar; in XML and INI it is
decimal digits only (leading zeros permitted, no sign; INI's usual
whitespace trim applies). Anything else — quoted numbers, floats,
negatives, values above `2^32 - 1` — fails `load()` with `InvalidVersion`,
the same code as missing metadata.

**JSON** — native 1:1. `null`/bool/string map directly; integral JSON numbers
map to `Int`, all others to `Double`; a number outside `std::int64_t`'s range
fails `load()` with `ParseError` (strictness over silent precision loss).
Objects and arrays map directly, insertion-ordered (`nlohmann::ordered_json`).

**YAML** — native 1:1 over the YAML core schema: plain `true`/`false` →
`Bool`, integer literals → `Int`, float literals → `Double`, `~`/`null` →
`Null`, everything else — including quoted scalars — → `String`. Anchors and
aliases are admitted and resolved by **deep copy** into the model (the tree
owns all storage; no sharing survives parsing); cyclic alias structures fail
`load()` with `ParseError`. Merge keys (`<<`) are rejected with `ParseError`
in v1 — their precedence semantics would reintroduce guessing.
Multi-document streams fail `load()` with `ParseError`.

**XML** — elements only. An `Object` maps to an element whose children are
its members in order; an **empty** `Object` carries `type="object"`, keeping
it distinguishable from an empty string. An `Array` maps to an element
carrying `type="array"` whose children are `<item>` elements. Scalars map to
element text with a reserved `type` attribute (`bool`, `int`, `double`,
`null`; absent means `string` — a bare empty element is therefore the empty
string). Elements with a scalar or absent `type` must have no child elements,
and `type="object"`/`type="array"` elements must have no text; violations
fail `load()` with `ParseError`. The document root is
`<config version="N">`. Any attribute other than the reserved `version`
(root) and `type` fails `load()` with `ParseError`; a model key that is not
a valid XML element name fails `save()` with `SerializationError`.

**INI** — two levels. Root members that are `Object`s of scalars map to
sections; root-level scalar members appear before the first section. Deeper
nesting and arrays are unrepresentable (`SerializationError` on `save()`).
Leading/trailing whitespace around keys and values is trimmed; interior
whitespace is preserved. Scalar text is then classified by exact literal,
case-sensitively: `true`/`false` → `Bool`; `null` → `Null`; `-?[0-9]+` →
`Int` (a match that overflows `std::int64_t` fails `load()` with
`ParseError`, mirroring the JSON rule); `-?[0-9]+\.[0-9]+` with an optional
`[eE][+-]?[0-9]+` exponent, or `-?[0-9]+` with a mandatory exponent →
`Double` — no `.5`, `1.`, `nan`, `inf`, hex, or locale-dependent forms;
double-quoted text → `String` (verbatim, quotes stripped); anything else →
`String`. Inside double quotes, `\"` and `\\` are the only escapes; any
other backslash sequence fails `load()` with `ParseError`. `save()` emits
doubles via `std::to_chars` (locale-independent, round-trip exact) and
double-quotes a string — escaping `"` and `\` — when it would re-classify
as another type, has leading/trailing whitespace, or contains `"` or `\`.
Strings containing newlines or carriage returns are unrepresentable in the
line-based format and fail `save()` with `SerializationError`. The `[meta]`
section's `version` key is the reserved carrier.

---

## 7. Version Metadata: VersionCatalog (`version_catalog.hpp`)

```cpp
class VersionCatalog {
public:
    Result<void> registerVersion(VersionArtifact artifact);  // dup => InvalidVersion

    bool                contains(VersionId v) const noexcept;
    Result<VersionId>   latestVersion() const;               // empty catalog => InvalidVersion
    Result<VersionId>   nextVersion(VersionId v) const;      // next registered version
    Result<ConfigModel> createDefault(VersionId v) const;    // factory ConfigValue -> fromValue

    // ordered ascending; used by registry validation
    const std::vector<VersionId>& versions() const noexcept;
};
```

Responsibilities only: version metadata + default factories. It knows nothing
about migrations (ADR-012).

The catalog is also the single source of **version ordering and adjacency**:
`nextVersion()` returns the next *registered* version in catalog order, and is
what registry validation and the migration engine consult. Version ids need not
be consecutive integers — a catalog of v1, v2, v4 makes v2 and v4 adjacent.

`latestVersion()` returns `InvalidVersion` on an empty catalog, keeping the
"recoverable failures use Result" rule intact for applications that call the
catalog directly (`nextVersion()` beside it already returns `Result`). Inside
the library that error path is unreachable: `ConfigRuntime::create()` rejects
an empty catalog (§9.2), so the runtime's own calls unwrap safely. A
`DefaultFactory` that throws inside `createDefault()` is caught and mapped to
`MigrationFailed` (ADR-018).

`registerVersion` rejects an empty `DefaultFactory` with `MigrationFailed` —
the callback family's error code (§10) — so a null callable is caught at
registration, never at first use.

---

## 8. Migrations

### 8.1 MigrationRegistry (`migration_registry.hpp`)

```cpp
// Migrations receive a context, not the model directly (ADR-017). The context
// can grow (logging sink, defaults access, ...) without changing MigrationFn —
// a signature change would break every registered migration in every consumer.
class MigrationContext {
public:
    ConfigModel& model() noexcept;              // the configuration being migrated
    VersionId    fromVersion() const noexcept;  // version this step migrates from
    VersionId    toVersion() const noexcept;    // version this step migrates to

private:
    friend class MigrationEngine;               // only the engine builds contexts
    MigrationContext(ConfigModel& model, VersionId from, VersionId to) noexcept;
};

using MigrationFn = std::function<Result<void>(MigrationContext&)>;  // transforms in place

struct MigrationEdge {
    VersionId   from;
    VersionId   to;        // must equal catalog.nextVersion(from) (adjacent-only)
    MigrationFn apply;
};

class MigrationRegistry {
public:
    Result<void> registerMigration(VersionId from, VersionId to, MigrationFn fn);

    Result<const MigrationEdge*> findMigration(VersionId from, VersionId to) const;

    // Validates registry against the catalog (called by ConfigRuntime::create).
    Result<void> validate(const VersionCatalog& catalog) const;
};
```

`validate()` enforces (Architecture §Registry Validation):

1. Every registered version below the latest has a migration to the next
   registered version.
2. No duplicate `(from,to)` edges.
3. All edge endpoints exist in the catalog.
4. Only adjacent edges (`to == catalog.nextVersion(from)`) are registered.
   Adjacency is catalog order, not numeric `+1`.

Failures return `MissingMigration` / `InvalidVersion`.

`registerMigration` rejects an empty `MigrationFn` with `MigrationFailed`: a
null callable is a registration error, surfaced immediately rather than at
first execution.

### 8.2 MigrationEngine (`migration_engine.hpp`)

```cpp
class MigrationEngine {
public:
    MigrationEngine(const VersionCatalog& catalog,
                    const MigrationRegistry& registry);    // catalog defines adjacency

    // Walks config.version -> target, one adjacent step at a time.
    Result<void> migrate(VersionedConfig& config, VersionId target);
};
```

Algorithm:

```text
current = config.version
while current < target:
    next = catalog.nextVersion(current)                    // InvalidVersion if unknown
    edge = registry.findMigration(current, next)           // MissingMigration if absent
    ctx  = MigrationContext(config.model, current, next)   // built per step (engine is a friend)
    edge->apply(ctx)                                       // MigrationFailed on error
    current = next
    config.version = current                               // version advances per step
```

* `target` must be a registered version; otherwise `InvalidVersion`.
* `config.version == target` is a successful no-op.
* Forward-only; if `config.version > target` the engine fails with
  `InvalidVersion` (downgrade is handled at the runtime layer, not here).
* A migration function that throws is caught and mapped to `MigrationFailed`
  (ADR-018).
* A migration function that **returns** an error is reported identically: the
  engine maps it to `MigrationFailed`, embedding the step and the original
  diagnostic in `Error::message` (e.g. `"migration 3->4 failed: InvalidType:
  ..."`). Callers see one code for any failed step — the underlying cause
  (say, an `InvalidType` from `model().set(...)`) survives in the message,
  not the code.
* `migrate()` is also the **direct migration** entry point (Architecture
  §Direct Migration Workflow, ADR-016) for apps that decide for themselves
  when migration should happen and bypass `ConfigRuntime`.
* Direct semantics are **raw**: in-place mutation, no transactionality, no
  repair, and no forced registry validation (`registry.validate(catalog)` is
  the caller's job). After a mid-chain failure the version reflects the last
  successfully completed step, but the model may additionally contain partial
  mutations from the failed step — migration functions mutate in place before
  returning an error. Treat the configuration as suspect after any failure;
  callers wanting rollback `clone()` first.

---

## 9. ConfigRuntime — the orchestrator (`config_runtime.hpp`)

### 9.1 Inspection types

```cpp
enum class SyncStatus {
    InSync,
    UpgradeRequired,
    DowngradeRequired,
};

struct SyncState {
    VersionId  currentVersion;
    VersionId  targetVersion;
    SyncStatus status;
};
```

There is no failure status: `synchronize()` reports failures through
`Result`'s error channel and guarantees the caller's configuration is
untouched on error.

### 9.2 Construction via validating factory

Because construction can fail (registry validation, ADR-011) and constructors
cannot return `Result`, construction uses a static factory:

```cpp
class ConfigRuntime {
public:
    static Result<ConfigRuntime>
    create(VersionCatalog catalog, MigrationRegistry registry);   // validates here

    SyncState inspect(const VersionedConfig& cfg, VersionId supported) const;

    Result<SyncStatus> synchronize(VersionedConfig& cfg, VersionId supported);
    Result<SyncStatus> synchronize(VersionedConfig& cfg);   // supported = latestVersion()

    Result<VersionedConfig> createDefault(VersionId version) const;

private:
    VersionCatalog    catalog_;
    MigrationRegistry registry_;
    // No MigrationEngine member: the engine holds references to catalog and
    // registry, so a stored engine would dangle once create() moves the
    // runtime out (returned by value). The engine is stateless and cheap;
    // synchronize() constructs one over catalog_/registry_ per call.
};
```

`create()`:

1. Empty catalog (no registered versions) → `Error{InvalidVersion}`: there is
   no latest version to synchronize toward.
2. `registry.validate(catalog)` → on failure, return the error (no runtime built).
3. Move catalog/registry into the runtime and return it. `ConfigRuntime`
   remains freely movable because no member holds references into the object.

### 9.3 inspect()

```text
current = cfg.version
target  = supported
if current == target -> InSync
if current <  target -> UpgradeRequired
if current >  target -> DowngradeRequired
```

`inspect` is pure/const — it never mutates. It performs a plain version
comparison and does not check whether `supported` is registered; that
validation happens in `synchronize()`.

### 9.4 synchronize() — the single transactional pipeline (ADR-009)

```text
if !catalog_.contains(supported) -> Error{InvalidVersion}          // 0. validate

state = inspect(cfg, supported)

DowngradeRequired -> return SyncStatus::DowngradeRequired   (no modification)

UpgradeRequired && !catalog_.contains(cfg.version)
                  -> Error{InvalidVersion}    // unknown persisted version:
                                              // fail before any work, not mid-chain

InSync / UpgradeRequired:
    working = VersionedConfig{ cfg.version, cfg.model.clone() }      // 1. copy
    migrated = false
    if state == UpgradeRequired:
        MigrationEngine{catalog_, registry_}
            .migrate(working, supported)      -> on err return Error // 2. migrate
        migrated = true
    repaired = repair(working.model, supported) -> on err return Error // 3. repair
    if migrated or repaired:
        cfg = std::move(working)                                      // 4. commit
    return SyncStatus::InSync
```

An `InSync` configuration still passes through repair: being at the right
version does not guarantee no keys have drifted away. When neither migration
nor repair changed anything, the commit is skipped and `cfg` is never touched.

A version *newer* than `supported` is reported as `DowngradeRequired` with no
registration check — configurations written by newer applications are expected
to carry versions this catalog does not know.

Guarantees realized:

* All work happens on `working` (a `clone()`); `cfg` is untouched until commit.
* Exactly one migration site, one repair site, one commit (the `std::move`),
  and the commit happens only when something actually changed.
* On failure the `Error` propagates through `Result` and `cfg` is guaranteed
  unmodified.
* The commit move-assigns `working` onto `cfg`, which invalidates every
  `ConfigNode` handle previously obtained from `cfg` (§4.2).
* The library never writes to storage — saving stays with the application.

### 9.5 Repair (private)

Runs once per `synchronize()` — for both `InSync` and `UpgradeRequired` —
after any migration, before commit (ADR-010):

```cpp
// Fill missing keys from the target version's default model. Never overwrite.
// Returns whether any key was added (drives the commit decision).
Result<bool> repair(ConfigModel& model, VersionId targetVersion) {
    ConfigModel defaults = catalog_.createDefault(targetVersion)?;   // factory
    return fillMissing(model, defaults, defaults.root(), /*path=*/"");
}

// Reads the defaults tree through const ConfigNode handles (§4.3) and writes
// through ConfigModel's path API, keeping ConfigNode read-only: all mutation
// goes through the model (§4.4). `path` names defaultsNode's location, which
// is the same in both trees.
Result<bool> fillMissing(ConfigModel& model, const ConfigModel& defaults,
                         ConfigNode defaultsNode, const std::string& path);
```

```text
added = false
for key in defaultsNode.keys():
    childPath = path.empty() ? key : path + "." + key
    if !model.contains(childPath):
        model.set(childPath, defaults.getValue(childPath)?)  // copy default subtree
        added = true
    else if defaultsNode.child(key) and model.nodeAt(childPath) both hold Object:
        added |= fillMissing(model, defaults, defaultsNode.child(key)?, childPath)?
    // else: present in the model -> leave untouched (presence wins)
return added
```

Because writes go through the path API and v1 paths have no escaping (§4.5),
object keys containing reserved characters would be unaddressable during
repair. ADR-021 makes this case unreachable: a default factory producing such
a key already fails at `fromValue` inside `createDefault`.

`fillMissing` rules:

* If a key/path present in `defaults` is **absent** in `model`, copy the default
  subtree in.
* If present, **leave the existing value untouched** (no overwrite, no type
  coercion, no removal of unknown keys, no schema checks).
* **Arrays are atomic**: a default array is copied only when its key is entirely
  absent; existing arrays are never merged element-wise, extended, or truncated.
* Recursion descends only where **both** the model and the defaults hold an
  `Object` at the same key. A key present in the model with a different type
  than the default is left untouched and its default children are not
  backfilled — presence wins over shape.

---

## 10. Error Code Mapping

A single, predictable mapping keeps diagnostics consistent across layers:

| Situation | ErrorCode |
|---|---|
| Malformed path string | `InvalidPath` |
| Path resolves to nothing on read | `NodeNotFound` |
| Scalar cannot convert to requested `T` | `InvalidType` |
| Backend parse failure | `ParseError` |
| Backend serialize failure | `SerializationError` |
| Migration function returned error / engine step failed | `MigrationFailed` |
| Required adjacent migration absent | `MissingMigration` |
| Unknown/duplicate version, bad endpoint | `InvalidVersion` |
| `supported` version not registered in the catalog | `InvalidVersion` |
| Stream carries no version metadata in `load()` | `InvalidVersion` |
| Malformed or out-of-range version carrier in `load()` | `InvalidVersion` |
| Empty catalog (`ConfigRuntime::create()`, `latestVersion()`) | `InvalidVersion` |
| Persisted `config.version` unregistered when an upgrade is required | `InvalidVersion` |
| Engine `target` unregistered or below the current version | `InvalidVersion` |
| User callback threw (`MigrationFn` / `DefaultFactory`) | `MigrationFailed` |
| Empty callable at registration (`MigrationFn` / `DefaultFactory`) | `MigrationFailed` |
| Model contains the format's reserved version carrier in `save()` | `SerializationError` |
| Non-object document root in `load()` | `ParseError` |
| Non-object root passed to `ConfigModel::fromValue` | `InvalidType` |
| Object key not path-addressable (empty or reserved character) in `load()` | `ParseError` |
| Object key not path-addressable (empty or reserved character) in `fromValue` / subtree `set` | `InvalidPath` |
| Duplicate object keys / sections / sibling elements in `load()` | `ParseError` |
| Stale `ConfigNode` handle passed to a `Result`-returning accessor | `NodeNotFound` |
| Backend library / stream exception in `load()` | `ParseError` |
| Backend library / stream exception in `save()` | `SerializationError` |

`synchronize()` reports failures directly through `Result`'s error channel —
there is no failure status — and guarantees the caller's configuration is
unmodified on error.

---

## 11. Build & Dependency Strategy (CMake, no vcpkg for consumers)

Goal: a downstream project consumes ConfigManager with a plain
`find_package(ConfigManager CONFIG REQUIRED)` and **never** needs vcpkg.

### 11.1 Dependency acquisition (`cmake/Dependencies.cmake`)

For each dependency, prefer a system/`find_package` copy, otherwise fetch and
build it as part of our build:

```cmake
# Pattern applied per dependency (tl::expected shown):
find_package(tl-expected QUIET)
if (NOT tl-expected_FOUND)
  include(FetchContent)
  FetchContent_Declare(tl-expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG        v1.1.0)
  FetchContent_MakeAvailable(tl-expected)
endif()
```

* **Core** depends only on `tl::expected` (header-only) → zero runtime deps.
* **Backends** each pull their own header-only-friendly lib via the same pattern
  (nlohmann/json, pugixml). Heavier deps (yaml-cpp) are still fetched/built, not
  required from the consumer's package manager.
* An option `CONFIGMANAGER_USE_SYSTEM_DEPS=ON` forces `find_package`-only for
  distro packagers who want no network fetch.
* Pinned dependency tags: `tl-expected v1.1.0`, `nlohmann/json v3.11.3`,
  `yaml-cpp 0.8.0`, `pugixml v1.14`, `googletest v1.14.0`.

### 11.2 Targets and installation

```cmake
add_library(configmanager_core ...)            # ALIAS configmanager::core
target_link_libraries(configmanager_core PUBLIC tl::expected)

option(CONFIGMANAGER_BUILD_JSON "..." ON)      # each backend opt-in
# add_library(configmanager_json ...) -> ALIAS configmanager::json (links nlohmann_json)

install(TARGETS configmanager_core ... EXPORT ConfigManagerTargets)
install(EXPORT ConfigManagerTargets NAMESPACE configmanager:: ...)
configure_package_config_file(cmake/ConfigManagerConfig.cmake.in ...)
```

* Header-only/transitive deps are re-exported through the generated package
  config so `find_package(ConfigManager)` resolves everything.
* Each format backend is an **opt-in** target: an app needing only versioning
  links `configmanager::core` alone.

---

## 12. Testing Strategy (data-model & API first)

Unit tests target the model and orchestration logic without format backends:

* **ConfigPath**: grammar acceptance/rejection, nested arrays, reserved chars,
  empty segments (`a.`, `.a`, `a..b`), malformed indices (`arr[]`, `arr[-1]`,
  `arr[1x]`), leading index, text after `]`, index overflow.
* **ConfigModel**: upsert creation, typed get/set, `InvalidType`/`NodeNotFound`,
  `remove` semantics, strict scalar conversion rules (lossless-only
  Int↔Double, no Bool/String coercion, no stringification); member order
  preserved across `getValue`/`set` round-trips (ADR-022); `fromValue`
  rejects non-object roots (`InvalidType`) and non-addressable keys — empty
  or reserved-character (`InvalidPath`, ADR-021).
* **Node lifetime**: handle stays valid across reparent/move; becomes invalid
  after `remove` (generation check); handles remain valid across a move of the
  `ConfigModel` object itself (heap arena transfer); `Result`-returning
  accessors on a stale handle fail `NodeNotFound`; `size()` returns 0 for
  scalars.
* **Upsert edge cases**: type-conflict writes fail `InvalidType` and leave the
  model unchanged — including cross-type writes to the final node; same-type
  subtree writes keep the target handle valid while invalidating descendant
  handles; any failed multi-segment write leaves no partially created
  intermediates (write atomicity, ADR-019); array writes may append at one
  past the end while larger indices fail `NodeNotFound`; empty path →
  `InvalidPath`; `contains()` returns `false` on malformed paths.
* **MigrationRegistry::validate**: missing edge, duplicate edge, edge skipping
  a registered version, unknown endpoint; empty callable rejected at
  registration (`MigrationFailed`), for `registerVersion` too.
* **MigrationEngine**: multi-step forward chain (including sparse version ids,
  e.g. v2 → v4), version advances per step, `MissingMigration` surfaced;
  no-op when already at target; unregistered or backward target →
  `InvalidVersion`; throwing migration function mapped to `MigrationFailed`;
  returned migration error wrapped as `MigrationFailed` with the original
  diagnostic preserved in the message.
* **ConfigRuntime**: each `SyncStatus`; unregistered `supported` version →
  `InvalidVersion`; transactional rollback (original unchanged when a mid-chain
  migration fails); repair fills missing only and never overwrites; repair runs
  on `InSync` (drift) with commit skipped when nothing changed; array atomicity
  and type-mismatch (presence-wins) rules in `fillMissing`; empty catalog
  rejected by `create()`; unregistered persisted version on upgrade →
  `InvalidVersion`; newer-unregistered version → `DowngradeRequired`.

A small backend round-trip suite (`load`→`save`→`load`) is added per format once
core is stable, including the mandatory-version failure path (`load()` on
unversioned data → `InvalidVersion`, malformed/out-of-range carrier →
`InvalidVersion`), non-addressable key rejection (empty or
reserved-character keys → `ParseError`, ADR-021), backend exception mapping
(throwing stream/parser → `ParseError`/`SerializationError`, ADR-018),
format-mapping restrictions (§6.1: unrepresentable model → `SerializationError`,
unrepresentable document → `ParseError`, duplicate keys/sections/sibling
elements → `ParseError`, XML empty-object vs empty-string round trip, INI
literal-grammar edges), and member-order preservation across a `load`→`save`
round trip (ADR-022). The test framework is **GoogleTest**
(pinned in §11.1), fetched via the same `FetchContent` pattern so the test
build is self-contained.

---

## 13. Implementation Order (dependency-driven)

The order follows the type dependency graph so each layer is testable in
isolation before the next is built:

1. `result.hpp` (Result/Error) + `version.hpp`.
2. `config_value.hpp`, `config_path.hpp` (parser + tests).
3. `ConfigModel` arena + `ConfigNode` (model + lifetime tests).
4. `VersionedConfig`.
5. `VersionCatalog`.
6. `MigrationRegistry` (+ `validate`).
7. `MigrationEngine` (direct migration usable here).
8. `ConfigRuntime` (`create`/`inspect`/`synchronize`/repair).
9. `IConfigInterface` + first backend (`configmanager::json`).
10. Remaining backends (yaml/xml/ini) as opt-in targets.

Core synchronization (steps 1–8) is fully functional and testable **before** any
serialization backend exists — consistent with the architecture's separation of
the serialization boundary from migration/synchronization.

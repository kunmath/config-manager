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
* Non-throwing boundary (ADR-018): exceptions from user-supplied callbacks
  (`MigrationFn`, `DefaultFactory`) are caught at the invocation site and
  mapped to `MigrationFailed`, preserving `what()` in `Error::message`.
  `std::bad_alloc` is not a recoverable configuration error and may propagate.

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
two tree representations. The returned tree must be object-rooted (§4.1); a
factory that throws is caught and mapped to `MigrationFailed` (ADR-018).

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
    Scalar                                       scalar_;
    std::map<std::string, ConfigValue>           object_;   // ordered => deterministic
    std::vector<ConfigValue>                     array_;
};
```

`std::map` (ordered) is chosen over `unordered_map` so serialization and repair
are **deterministic**, which matters for diffing and reproducible migrations.

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
  `NodeId` → existing handles stay valid.
* *Removing a node* frees the slot and bumps `generation` → any handle holding
  the old generation is detectably stale (invalidated).
* The arena lives on the heap (`std::unique_ptr<NodeArena>` member of
  `ConfigModel`), so *moving the `ConfigModel` object* transfers the arena and
  keeps every handle valid — handles follow the new owner.
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
    const ConfigModel* model_    = nullptr;
    NodeId             id_       = NodeId_None::value;
    std::uint32_t      generation_ = 0;   // must match arena slot to be valid
};
```

A `ConfigNode` is a 16-byte handle. Validity is verified lazily by comparing
`generation_` with the arena slot's current generation.

`ConfigNode` is a **read-only** handle: every accessor is `const` and it
stores a `const ConfigModel*`, so const models can be traversed — repair reads
the defaults model exactly this way. All mutation goes through `ConfigModel`.

### 4.4 ConfigModel public API (`config_model.hpp`)

`ConfigModel` is move-only (it owns the arena). Path-based access is the primary
ergonomic surface; node handles are the lower-level surface.

```cpp
class ConfigModel {
public:
    ConfigModel();                       // empty root object
    static ConfigModel fromValue(ConfigValue root);  // adopt a value tree into the arena
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
* Array indices are bounded: a write may target an existing element or one
  past the end (append). Larger indices fail with `NodeNotFound`; missing
  intermediate arrays are created empty. Holes are never fabricated.
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
  `remove(from)`.
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

* Supports `object.key`, `arr[0]`, and nesting (`groups[0].users[4].name`).
* Reserved characters: `.` `[` `]`. **No escaping in v1** (explicit non-goal).
* The empty string is invalid (`InvalidPath`). No string path addresses the
  root node; the root is reached via `ConfigModel::root()`.
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
* Backends live in `backends/<fmt>` as independent CMake targets. Errors map to
  `ParseError` / `SerializationError`.

| Target | Library (header-only?) | Version encoding example |
|---|---|---|
| `configmanager::json` | nlohmann/json (header-only) | top-level `"__version"` field |
| `configmanager::yaml` | yaml-cpp | top-level `version:` key |
| `configmanager::xml`  | pugixml (header+small src) | root attribute `version="N"` |
| `configmanager::ini`  | none (hand-written) | `[meta] version=N` section |

---

## 7. Version Metadata: VersionCatalog (`version_catalog.hpp`)

```cpp
class VersionCatalog {
public:
    Result<void> registerVersion(VersionArtifact artifact);  // dup => InvalidVersion

    bool                contains(VersionId v) const noexcept;
    VersionId           latestVersion() const;               // max registered
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

`latestVersion()` requires a non-empty catalog. The library only calls it
through `ConfigRuntime`, whose `create()` rejects an empty catalog with
`InvalidVersion` (§9.2). A `DefaultFactory` that throws inside
`createDefault()` is caught and mapped to `MigrationFailed` (ADR-018).

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
    ctx  = MigrationContext{ config.model, current, next } // built per step
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
* `migrate()` is also the **direct migration** entry point (Architecture
  §Direct Migration Workflow, ADR-016) for apps that decide for themselves
  when migration should happen and bypass `ConfigRuntime`.
* Direct semantics are **raw**: in-place mutation, no transactionality (a
  mid-chain failure leaves the config at the last reached version — callers
  wanting rollback `clone()` first), no repair, and no forced registry
  validation (`registry.validate(catalog)` is the caller's job).

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

Because construction can fail (registry validation) and constructors cannot
return `Result`, construction uses a static factory (ADR-011):

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
    return fillMissing(model.root(), defaults.root());   // recursive merge of absent paths
}
```

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
| Empty catalog at `ConfigRuntime::create()` | `InvalidVersion` |
| Persisted `config.version` unregistered when an upgrade is required | `InvalidVersion` |
| Engine `target` unregistered or below the current version | `InvalidVersion` |
| User callback threw (`MigrationFn` / `DefaultFactory`) | `MigrationFailed` |
| Model contains the format's reserved version carrier in `save()` | `SerializationError` |
| Non-object document root in `load()` | `ParseError` |

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

* **ConfigPath**: grammar acceptance/rejection, nested arrays, reserved chars.
* **ConfigModel**: upsert creation, typed get/set, `InvalidType`/`NodeNotFound`,
  `remove` semantics, strict scalar conversion rules (lossless-only
  Int↔Double, no Bool/String coercion, no stringification).
* **Node lifetime**: handle stays valid across reparent/move; becomes invalid
  after `remove` (generation check); handles remain valid across a move of the
  `ConfigModel` object itself (heap arena transfer).
* **Upsert edge cases**: type-conflict writes fail `InvalidType` and leave the
  model unchanged; array writes may append at one past the end while larger
  indices fail `NodeNotFound`; empty path → `InvalidPath`; `contains()`
  returns `false` on malformed paths.
* **MigrationRegistry::validate**: missing edge, duplicate edge, edge skipping
  a registered version, unknown endpoint.
* **MigrationEngine**: multi-step forward chain (including sparse version ids,
  e.g. v2 → v4), version advances per step, `MissingMigration` surfaced;
  no-op when already at target; unregistered or backward target →
  `InvalidVersion`; throwing migration function mapped to `MigrationFailed`.
* **ConfigRuntime**: each `SyncStatus`; unregistered `supported` version →
  `InvalidVersion`; transactional rollback (original unchanged when a mid-chain
  migration fails); repair fills missing only and never overwrites; repair runs
  on `InSync` (drift) with commit skipped when nothing changed; array atomicity
  and type-mismatch (presence-wins) rules in `fillMissing`; empty catalog
  rejected by `create()`; unregistered persisted version on upgrade →
  `InvalidVersion`; newer-unregistered version → `DowngradeRequired`.

A small backend round-trip suite (`load`→`save`→`load`) is added per format once
core is stable, including the mandatory-version failure path (`load()` on
unversioned data → `InvalidVersion`). The test framework is **GoogleTest**
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

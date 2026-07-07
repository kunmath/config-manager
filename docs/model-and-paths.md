# The data model and paths

## The three tree types

| Type | Semantics | Use it for |
|---|---|---|
| `ConfigModel` | Owning, move-only tree; all access via paths or handles | The configuration you operate on |
| `ConfigValue` | Copyable, value-semantic builder | Default factories, subtree insertion in migrations |
| `ConfigNode` | 16-byte read-only handle into a model | Cheap traversal without copying |

A model's root is always an **Object**
([ADR-020](Architecture.md#adr-020)). Node types are `Null`, `Bool`,
`Int` (`std::int64_t`), `Double`, `String`, `Object`, `Array`. Object members
keep **insertion order** end-to-end — through load, mutation, and save
([ADR-022](Architecture.md#adr-022)) — so serialization stays diffable and
migrations reproducible.

## Path grammar

Paths address nodes with dots and indices:

```text
network.hostname
users[0].name
groups[0].users[4].name
```

```text
path    := segment ( "." segment )*
segment := key index*
key     := char+            ; any byte except "." "[" "]"
index   := "[" digit+ "]"   ; decimal digits only
```

Anything the grammar does not produce fails with `InvalidPath`: empty keys
(`a.`, `.a`, `a..b`), a leading index (`[0]`), malformed indices (`arr[]`,
`arr[-1]`, `arr[1x]`), text directly after `]` other than `.`, `[`, or
end-of-path. Whitespace is never trimmed — it is an ordinary key character.
The empty string is not a path; the root is reached via `ConfigModel::root()`.

There is **no escaping** in version 1. Consequently a key that is empty or
contains `.` `[` `]` could never be addressed, so such keys are rejected at
every boundary where data enters a model — `load()`, `fromValue()`, subtree
`set()` ([ADR-021](Architecture.md#adr-021)).

## Reading

```cpp
Result<T>           get(path);       // typed scalar read
Result<ConfigValue> getValue(path);  // deep copy of a subtree
bool                contains(path);  // never fails; false for malformed paths too
Result<ConfigNode>  nodeAt(path);    // handle, no copy
```

Error mapping: malformed path → `InvalidPath`; absent path → `NodeNotFound`;
present but wrong type → `InvalidType`.

### Strict scalar conversion

Typed reads convert only when provably lossless
([ADR-015](Architecture.md#adr-015)):

* `Bool` and `String` never convert to or from anything.
* `Int` → `Double` only when exactly representable as a double.
* `Double` → `Int` only when integral and in range of the target type.
* `Int` is stored as `std::int64_t`; reads into narrower or differently signed
  types succeed only when the value is exactly representable.

Values are never stringified; strings are never parsed into numbers. Everything
else is `InvalidType`.

## Writing

```cpp
Result<void> set(path, T value);           // scalar upsert
Result<void> set(path, ConfigValue tree);  // subtree upsert
Result<void> remove(path);
```

`set()` follows **upsert semantics**: missing intermediate objects and arrays
are created (`set("network.timeout", 10)` creates `network` first). Three
rules bound it ([ADR-019](Architecture.md#adr-019)):

1. **Upsert never changes type.** If any path segment — including the final
   node — conflicts with an existing node's type, the write fails with
   `InvalidType` and nothing changes. A migration that changes a node's type
   removes the node explicitly first.
2. **Array indices are bounded.** A write may target an existing element or
   append at exactly one-past-the-end; larger indices fail with
   `NodeNotFound`. Holes are never fabricated. Missing intermediate arrays are
   created empty, so `set("users[0].name", ...)` with `users` absent creates
   the array and appends.
3. **Writes are atomic.** The whole path is validated before any mutation, so
   a failed write leaves no partially created intermediates.

There is no rename/move API. A rename composes as
`getValue(from)` → `set(to, value)` → `remove(from)` — note this **copies** the
subtree; handles into the removed original are invalidated.

## Handle lifetime

`ConfigNode` is a read-only handle that references the model's heap storage,
not the `ConfigModel` object:

* **Moving the `ConfigModel` object keeps handles valid** — they follow the new
  owner.
* **Removing a node invalidates its handles detectably**: `valid()` returns
  false, and `Result`-returning accessors (`as<T>()`, `child()`, `at()`,
  `keys()`) fail with `NodeNotFound`. The `noexcept` accessors (`type()`,
  `size()`) require a valid handle as a precondition.
* **Destroying a model invalidates handles undetectably** — including
  move-assigning another model onto it, and including a committing
  `synchronize()`, which replaces the caller's model. Using such a handle is
  undefined behavior, exactly like an invalidated container iterator.

Same-type overwrites update a node in place, so handles to it stay valid;
replacing a container's contents removes its previous descendants, whose
handles become detectably invalid.

Full details: [Architecture.md § ConfigNode Lifetime](Architecture.md#confignode-lifetime)
and the storage design in [HighLevelDesign.md §4](HighLevelDesign.md#4-the-configuration-data-model).

## Thread safety

`ConfigModel`, `ConfigNode`, and `ConfigRuntime` are **not thread-safe**.
Synchronize concurrent access in the application.

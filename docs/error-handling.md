# Error handling

## Results, not exceptions

Public APIs never throw ([ADR-005](Architecture.md#adr-005)). Every fallible
operation returns:

```cpp
template <typename T>
using Result = tl::expected<T, Error>;    // Result<void> for succeed-or-fail ops

struct Error {
  ErrorCode   code;
  std::string message;    // owned; rich, human-readable diagnostics
};
```

Usage follows `tl::expected` (a C++20-compatible `std::expected`; only
`result.hpp` references the underlying type, so a later switch is transparent):

```cpp
auto port = model.get<std::int64_t>("server.port");
if (!port) {
  log(port.error().message);            // inspect Error
  return cfg::fail(port.error().code,   // or propagate with the helper
                   std::move(port.error().message));
}
use(*port);                             // dereference on success
```

`cfg::fail(code, message)` builds the `tl::unexpected<Error>` for you at
call sites that produce or forward errors.

## Error codes

| `ErrorCode` | Fires when |
|---|---|
| `InvalidPath` | Malformed path string; non-path-addressable key in `fromValue()`/subtree `set()` |
| `NodeNotFound` | Path/key/index absent on read; out-of-bounds array write; stale `ConfigNode` handle |
| `InvalidType` | Lossy scalar conversion requested; type-conflicting write; non-object root in `fromValue()` |
| `ParseError` | Backend cannot parse a document, or the document is unrepresentable in the model (duplicates, non-object root, bad keys); parser/stream exception in `load()` |
| `SerializationError` | Model unrepresentable in the format; reserved carrier present in the model; stream/parser exception in `save()` |
| `MigrationFailed` | A migration step returned an error or threw; a user callback threw; a null callable at registration |
| `MissingMigration` | A required adjacent migration is not registered |
| `InvalidVersion` | Unknown/duplicate/unregistered version anywhere; missing or malformed version carrier in `load()`; empty catalog |

The exhaustive situation-by-situation table is
[HighLevelDesign.md §10](HighLevelDesign.md#10-error-code-mapping).

Codes are for programmatic dispatch; `message` carries the specifics. A failed
migration step, for example, is always `MigrationFailed`, with the step and the
underlying diagnostic embedded in the message
(`"migration 3->4 failed: InvalidType: ..."`).

## Exception boundaries

The non-throwing guarantee is enforced wherever foreign code runs inside the
library ([ADR-018](Architecture.md#adr-018)):

* **Your callbacks** — migration functions, default factories: a thrown
  exception is caught at the invocation site and mapped to `MigrationFailed`.
* **Parser libraries and streams** inside backends: mapped to `ParseError` in
  `load()`, `SerializationError` in `save()` — including streams with
  exception masks set.

At every boundary the catch order is fixed: `std::bad_alloc` is rethrown
(memory exhaustion is not a recoverable configuration error); exceptions
derived from `std::exception` preserve `what()` in `Error::message`; anything
else gets a fixed fallback message naming the boundary.

## Two error channels to know about

* **`synchronize()` has no failure status.** `SyncStatus` only describes
  version relationships; failures come through the `Result` error channel, and
  on error the caller's configuration is guaranteed unmodified.
* **`contains()` never fails.** It returns `false` for malformed paths as well
  as absent ones — the one deliberately non-`Result` query.

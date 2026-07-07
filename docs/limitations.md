# Limitations

Deliberate scope boundaries and version-1 restrictions, in one place. Most are
design decisions with an ADR behind them, not gaps waiting to be filled.

## Out of scope by design

* **No schema validation** ([ADR-006](Architecture.md#adr-006)). Repair
  backfills missing keys but never checks types, ranges, or shapes. Pair the
  library with any validation library via the
  [direct migration workflow](versioning-and-migration.md#direct-migration-advanced).
* **No storage layer** ([ADR-004](Architecture.md#adr-004)). Backends operate
  on streams; the library never opens, writes, or renames files. Atomic-save
  strategies are the application's responsibility.
* **No downgrades** ([ADR-008](Architecture.md#adr-008)). Migrations are
  forward-only; a newer configuration is reported as `DowngradeRequired` and
  left untouched.
* **No automatic synchronization** ([ADR-002](Architecture.md#adr-002)).
  Nothing migrates unless `synchronize()` (or `migrate()`) is called.

## Version-1 restrictions

* **No path escaping.** `.` `[` `]` cannot appear in object keys, and empty
  keys are invalid — such keys are rejected wherever data enters a model
  ([ADR-021](Architecture.md#adr-021)). Path escaping in a later version may
  lift this.
* **No rename/move API.** Renames compose from
  `getValue` → `set` → `remove`, which copies the subtree and invalidates
  handles into the original.
* **`Int` is `std::int64_t`.** Unsigned values above `INT64_MAX` are not
  storable; JSON numbers outside int64 range fail `load()` with `ParseError`
  rather than losing precision.
* **YAML and INI backends are not yet implemented** (planned; their mappings
  are already specified in
  [HighLevelDesign.md §6.1](HighLevelDesign.md#61-format-mappings)).

## Operational caveats

* **Not thread-safe.** `ConfigModel`, `ConfigNode`, and `ConfigRuntime`
  require external synchronization for concurrent access.
* **Handle invalidation is partly undetectable.** Removing a node invalidates
  its handles *detectably*; destroying a model — including a committing
  `synchronize()` — invalidates them *undetectably* (undefined behavior on
  use), like container iterators. See
  [handle lifetime](model-and-paths.md#handle-lifetime).
* **Direct `MigrationEngine` use is raw**: in-place mutation, no
  transactionality, no repair, no forced registry validation
  ([ADR-016](Architecture.md#adr-016)). `ConfigRuntime` is the safe path.
* **`std::bad_alloc` propagates.** The non-throwing guarantee covers
  recoverable errors; memory exhaustion is not one
  ([ADR-018](Architecture.md#adr-018)).

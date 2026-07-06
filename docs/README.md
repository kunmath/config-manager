# ConfigManager documentation

New to the library? Read the [tutorial](tutorial.md) first, then dip into the
topic guides as needed. The guides are user-facing: they tell you how to *use*
each part of the library. For the *why* behind a rule, they link into the two
design documents at the bottom of this page.

## Guides

| Guide | What it covers |
|---|---|
| [Tutorial](tutorial.md) | From an empty model to the full load → inspect → synchronize → save lifecycle |
| [The data model and paths](model-and-paths.md) | `ConfigModel`, `ConfigValue`, `ConfigNode`, the path grammar, upsert rules, strict scalar conversion, handle lifetime |
| [Versioning and migrations](versioning-and-migration.md) | `VersionCatalog`, `MigrationRegistry`, writing migration functions, direct use of `MigrationEngine` |
| [Synchronization and repair](synchronization.md) | `ConfigRuntime`, `inspect()`/`synchronize()`, the transactional pipeline, what repair does and does not do |
| [Serialization backends](serialization-backends.md) | The `IConfigInterface` contract, per-format mappings and version carriers, writing your own backend |
| [Error handling](error-handling.md) | `Result<T>`, the `ErrorCode` table, exception boundaries |
| [FAQ](faq.md) | Common questions |
| [Limitations](limitations.md) | What the library deliberately does not do |

## Examples

Every example in [`../examples/`](../examples/README.md) is a runnable program
that asserts its own expected outcomes — they are executed in CI, so they
cannot drift from the implementation.

## Design documents

* [Architecture.md](Architecture.md) — architectural principles, component
  responsibilities, and the Architectural Decision Records (ADR-001 … ADR-022)
  the guides cite.
* [HighLevelDesign.md](HighLevelDesign.md) — the implementation contract:
  concrete C++ types, the internal storage model, exact format mappings
  (§6.1), the error-code mapping table (§10), and the build/dependency
  strategy.

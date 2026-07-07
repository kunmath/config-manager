# Versioning and migrations

## The pieces

| Component | Owns | Does not own |
|---|---|---|
| `VersionCatalog` | Version list, ordering/adjacency, default factories | Migrations |
| `MigrationRegistry` | Migration functions, registry validation | Version metadata, defaults |
| `MigrationEngine` | Executing the migration chain | Repair, transactionality |

`VersionedConfig` — `{VersionId version; ConfigModel model;}` — is the single
source of truth for a configuration's current version
([ADR-007](Architecture.md#adr-007)). No API ever asks you for a *source*
version; only *target* versions are passed.

## The version catalog

```cpp
cfg::VersionCatalog catalog;
catalog.registerVersion({1, v1Defaults});   // VersionArtifact{version, defaultFactory}
catalog.registerVersion({2, v2Defaults});
catalog.registerVersion({4, v4Defaults});   // ids need not be consecutive

catalog.latestVersion();    // Result<VersionId>: 4
catalog.nextVersion(2);     // Result<VersionId>: 4 (adjacency is catalog order)
catalog.createDefault(2);   // Result<ConfigModel> from the factory
```

* `VersionId` is `std::uint32_t`, monotonic, application-defined.
* A `DefaultFactory` is `std::function<ConfigValue()>` returning the version's
  **complete** default tree (object-rooted, path-addressable keys). Defaults
  are what repair backfills from, so they are the single place a new setting's
  default value lives.
* **Adjacency is catalog order, not numeric `+1`**: with versions 1, 2, 4
  registered, versions 2 and 4 are adjacent and the required migration is
  `2 → 4` ([ADR-008](Architecture.md#adr-008)).
* Duplicate registration fails with `InvalidVersion`; a null factory fails
  immediately at registration.

## Writing migration functions

```cpp
using MigrationFn = std::function<Result<void>(MigrationContext&)>;

cfg::Result<void> migrateV1ToV2(cfg::MigrationContext& ctx) {
  cfg::ConfigModel& model = ctx.model();
  // ctx.fromVersion() == 1, ctx.toVersion() == 2 for this step
  ...
}

registry.registerMigration(1, 2, migrateV1ToV2);
```

Guidelines that keep migrations small and deterministic:

* **Move user data; don't fill defaults.** Repair runs after the whole chain
  and backfills anything missing from the target version's defaults. A
  migration only restructures what the user actually set. (A migration that
  *needs* a field creates it explicitly — never rely on repair mid-chain.)
* **Type changes are remove-then-set.** Upsert never changes a node's type
  ([ADR-019](Architecture.md#adr-019)); re-expressing `verbose: bool` as
  `logging.level: string` means `remove("verbose")` plus a fresh `set`.
* **Mutation is in place.** Return an error early and the model may be left
  partially modified — that is fine under `ConfigRuntime::synchronize()`,
  which runs the chain on a working copy and discards it on failure.
* **Errors and exceptions are equivalent.** A returned error or a thrown
  exception both surface as `MigrationFailed`, with the step and the original
  diagnostic preserved in `Error::message` (e.g. `"migration 3->4 failed:
  InvalidType: ..."`). Exceptions never escape the library
  ([ADR-018](Architecture.md#adr-018)).

A reusable helper for the most common move
(from [`examples/03`](../examples/03_relaygate_lifecycle.cpp)):

```cpp
cfg::Result<void> moveKey(cfg::ConfigModel& model, std::string_view from,
                          std::string_view to) {
  auto value = model.getValue(from);
  if (!value) return cfg::fail(value.error().code, std::move(value.error().message));
  if (auto put = model.set(to, *std::move(value)); !put) return put;
  return model.remove(from);
}
```

## Registry validation

`ConfigRuntime::create()` validates the registry against the catalog
([ADR-011](Architecture.md#adr-011)); you can also run it yourself with
`registry.validate(catalog)`:

1. Every registered version below the latest has a migration to the **next**
   registered version.
2. No duplicate `(from, to)` edges.
3. All edge endpoints exist in the catalog.
4. Only adjacent edges are registered — an edge that skips a registered
   version (e.g. `1 → 3` when 2 exists) is rejected.

Failures map to `MissingMigration` / `InvalidVersion`. Null callables are
rejected earlier, at `registerMigration()` time.

## Direct migration (advanced)

`MigrationEngine` is a supported standalone entry point
([ADR-016](Architecture.md#adr-016)) for applications that want their own
comparison or validation logic around migration:

```cpp
cfg::MigrationEngine engine(catalog, registry);
cfg::Result<void> r = engine.migrate(config, targetVersion);
```

The engine walks `config.version → target` one adjacent step at a time,
advancing `config.version` after each successful step. Its semantics are
**raw** — everything `synchronize()` layers on top is absent:

* **Not transactional.** Mutation is in place. After a mid-chain failure the
  version reflects the last completed step, and the model may contain partial
  mutations from the failed step. Treat the configuration as suspect after any
  failure; `clone()` first if you need rollback.
* **No repair.** Missing-key backfill is your responsibility.
* **No forced validation.** Call `registry.validate(catalog)` yourself.
* The target must be registered and not below the current version
  (`InvalidVersion` otherwise — downgrades are never performed). Already at
  target is a successful no-op.

Prefer `ConfigRuntime::synchronize()` unless you specifically need these raw
semantics.

# Synchronization and repair

`ConfigRuntime` is the orchestrated, safe path for bringing a persisted
configuration to the version your application supports. Synchronization is
**always explicit** — no read or write ever triggers it
([ADR-002](Architecture.md#adr-002)).

## Construction

```cpp
auto runtime = cfg::ConfigRuntime::create(std::move(catalog), std::move(registry));
if (!runtime) { /* wiring error — see Error::message */ }
```

`create()` is a validating factory: an empty catalog fails with
`InvalidVersion`, and the registry is validated against the catalog
([registry validation](versioning-and-migration.md#registry-validation)) before
a runtime is ever handed out. Bad wiring surfaces at startup, not in the field.

## inspect()

```cpp
cfg::SyncState state = runtime->inspect(config, supportedVersion);
// SyncState{ currentVersion, targetVersion, status }
```

| `SyncStatus` | Meaning |
|---|---|
| `InSync` | `config.version == supported` |
| `UpgradeRequired` | Configuration is older; `synchronize()` will migrate |
| `DowngradeRequired` | Configuration is **newer** than the application supports |

`inspect()` is a pure version comparison — it does not check that either
version is registered in the catalog. That validation happens in
`synchronize()`, which can therefore fail with `InvalidVersion` for a pairing
`inspect()` reported as `UpgradeRequired`.

There is no failure status: failures travel through `Result`'s error channel.

## synchronize()

```cpp
Result<SyncStatus> synchronize(VersionedConfig& config, VersionId supported);
Result<SyncStatus> synchronize(VersionedConfig& config);   // supported = latest
```

The pipeline ([ADR-009](Architecture.md#adr-009)):

```text
validate supported registered        -> InvalidVersion
determine status
DowngradeRequired                    -> return, no modification
validate config.version registered   -> InvalidVersion   (upgrades only)
clone the model                       (working copy)
migrate working copy                  (skipped when InSync)
repair working copy                   (always)
commit iff something changed          (single move-assign)
```

### Guarantees

* **Transactional:** migration and repair run on a clone; on any failure the
  error is returned and your configuration is **guaranteed unmodified**.
* **Single commit point:** the commit happens only when migration or repair
  actually changed something; a clean `InSync` pass leaves the object
  untouched.
* **A commit replaces the model** — every `ConfigNode` handle previously
  obtained from that configuration is invalidated (undetectably; see
  [handle lifetime](model-and-paths.md#handle-lifetime)). Re-acquire handles
  after a successful synchronize.
* **Storage is never touched.** Saving the result is the application's job.

### Downgrades are refused

Migrations are forward-only ([ADR-008](Architecture.md#adr-008)). When the
configuration is newer than `supported`, `synchronize()` returns
`DowngradeRequired` and modifies nothing — regardless of whether that newer
version is registered, since files written by newer application versions are
expected to carry versions this build does not know. The application decides
what to do (refuse to start, run read-only, …); the one thing it should not do
is save over the newer file.

## Repair

Repair runs **once per synchronize()**, after all migrations, using the target
version's defaults — for `UpgradeRequired` *and* `InSync` configurations. A
version-correct file can still have drifted (a user deleted a key); the repair
step is the single place drift is fixed ([ADR-010](Architecture.md#adr-010)).

Repair **will**:

* add missing keys, copying the default value from the target version's
  default factory output.

Repair will **not**:

* replace existing values,
* validate or correct types,
* remove unknown keys,
* enforce schema constraints.

Two structural rules:

* **Arrays are atomic.** A default array is copied only when its key is
  entirely absent. Existing arrays are never merged element-wise, extended, or
  truncated.
* **Presence wins over shape.** Repair descends only where both the
  configuration and the defaults hold an `Object` at the same key. A key
  present with a different type than the default is left untouched and its
  default children are not backfilled.

Because repair runs *after* migration, migration authors never rely on repair
to populate fields mid-chain — see
[writing migration functions](versioning-and-migration.md#writing-migration-functions).

## createDefault()

```cpp
Result<VersionedConfig> fresh = runtime->createDefault(version);
```

Runs the version's default factory and returns a complete `VersionedConfig` —
the first-launch path when no file exists yet.

## Recommended startup shape

```cpp
auto config = loadFile(path);                    // your I/O + a backend
if (!config) {
  config = runtime->createDefault(kSupported);   // first launch (or handle corrupt file)
}
auto status = runtime->synchronize(*config, kSupported);
if (!status) { /* refuse to start; file is untouched */ }
if (*status == cfg::SyncStatus::DowngradeRequired) { /* newer file: don't save */ }
saveFile(*config, path);                         // persist migrations/repairs
```

[`examples/03_relaygate_lifecycle.cpp`](../examples/03_relaygate_lifecycle.cpp)
plays this out across five phases, including the downgrade-refusal branch.

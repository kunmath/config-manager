# Firmware downgrade checkpoints

ConfigManager migrations are forward-only. If firmware supporting config v2
opens a v3 file, `synchronize()` returns `DowngradeRequired` and leaves the
configuration untouched. The optional `configmanager::checkpoint` component
provides an application-invoked recovery path without adding reverse
migrations ([ADR-023](Architecture.md#adr-023)).

This applies to any firmware downgrade, including but not limited to A/B
partition rollback.

## Build and layout

Enable `CONFIGMANAGER_BUILD_CHECKPOINT` and link `configmanager::checkpoint`.
The component works with any `IConfigInterface`; it does not depend on JSON or
XML.

```text
device.json
device.checkpoints/0000000001.json
device.checkpoints/0000000003.json
```

There is one checkpoint per source config version. Capturing the same version
replaces it with fresher bytes. The zero-padded filename is a discovery aid;
the version loaded from the document is authoritative. No manifest is needed.

## Create a store

```cpp
#include <configmanager/checkpoint_store.hpp>

auto store = cfg::cpCreate({
    .canonical_path = "device.json",
    .checkpoint_directory = "device.checkpoints",
    .retention = 2,
});
```

`CheckpointOptions::max_file_bytes` (default 16 MiB) bounds how much of the
canonical file or any checkpoint is read into memory.

`cpCreate()` validates and normalizes paths but performs no writes. All
filesystem changes remain explicit.

## Upgrade

Capture the canonical file before synchronizing it:

```cpp
cfg::SyncState state = runtime.inspect(config, kSupported);
if (state.status == cfg::SyncStatus::UpgradeRequired) {
  if (auto result = cfg::cpCapture(*store, config, backend); !result) {
    return refuseStartup(result.error());
  }
}

auto status = runtime.synchronize(config, kSupported);
if (!status) return refuseStartup(status.error());
if (*status == cfg::SyncStatus::DowngradeRequired) return handleDowngrade();

if (!validate(config)) return refuseStartup(validationError());
if (auto result = cfg::cpCommit(*store, config, backend); !result) {
  return refuseStartup(result.error());
}
auto pruning = cfg::cpPrune(*store, backend);
if (!pruning) return reportStorageFailure(pruning.error());
logRejectedCheckpoints(pruning->rejected);
```

`cpCapture()` loads the canonical file to verify that its embedded version
matches the supplied configuration, then copies the original bytes rather than
reserializing them. It
writes a temporary sibling and atomically replaces that version's checkpoint.
A failure leaves any previous checkpoint and the canonical file unchanged.

`cpCommit()` similarly serializes to a temporary sibling and replaces the
canonical file only after serialization and stream checks succeed. Pruning is
separate and happens after commit so a prune failure cannot make the commit
outcome ambiguous.

## Recover after firmware downgrade

Recovery is explicit application policy:

```cpp
auto search = cfg::cpPrepareRestore(
    *store, backend, runtime, kSupported);
if (!search) return refuseStartup(search.error());
if (!search->candidate) return refuseStartup(noCheckpointError());

cfg::PreparedRestore& restored = *search->candidate;
if (!validate(restored.config)) return refuseStartup(validationError());

auto committed = cfg::cpCommit(*store, restored.config, backend);
if (!committed) return refuseStartup(committed.error());
```

`cpPrepareRestore()`:

1. Verifies that the target is registered without invoking its default factory.
2. Enumerates checkpoint-shaped files.
3. Loads each file through the supplied backend.
4. Rejects embedded versions newer than the firmware supports.
5. Calls `synchronize(candidate, supported)` on every compatible candidate.
6. Accepts only `InSync` candidates whose resulting version equals supported.
7. Tries candidates from highest embedded source version downward and returns
   the first one that synchronizes successfully.

This allows firmware supporting v2 to restore a v1 checkpoint and migrate it
forward through the normal v1-to-v2 migration. Corrupt, unregistered, and
newer candidates are reported in `RestoreSearch::rejected` for logging.

Preparation never modifies the canonical file. The application can validate,
request confirmation, or abort before calling `cpCommit()`.

## Policy and limits

* **Rollback is destructive.** Settings created or changed only under newer
  firmware are not in an older checkpoint. Re-upgrading produces a valid newer
  config, not the previous newer config.
* A normal `load()` failure does not prove firmware downgrade; it may indicate
  corruption. Attempt checkpoint recovery after load failure only when the
  application's firmware/update policy calls for it.
* No compatible checkpoint is a successful search with no candidate. The
  application chooses whether to refuse startup or create defaults.
* Retention counts distinct embedded versions. `cpPrune()` retains the newest
  verified version checkpoints by modification time. Unreadable checkpoints
  are left untouched and returned in `PruneReport::rejected` for logging or
  operator cleanup; they do not block pruning verified files. A retention of
  zero removes all verified checkpoints.
* A checkpoint larger than `max_file_bytes` is left untouched and reported as
  rejected, like a corrupt one. An oversized canonical file fails
  `cpCapture()` with `StorageError`.
* Atomic replacement prevents readers from observing a partial file and
  survives process interruption. It is not a power-loss durability guarantee;
  that also requires platform-specific file and directory synchronization.
* Concurrent writers are unsupported. Use the application's existing config
  or firmware-update lock around capture, synchronization, commit, and prune.
* Multiple firmware versions must not actively share and mutate one canonical
  config directory at the same time.

See [`examples/05_firmware_rollback.cpp`](../examples/05_firmware_rollback.cpp)
for the complete v1 checkpoint, v3 upgrade, and v2 firmware rollback flow.

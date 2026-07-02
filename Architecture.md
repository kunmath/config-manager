# ConfigManager Architecture

## Overview

ConfigManager is a C++ library for configuration versioning, migration, and synchronization.

Its primary goal is:

> Prevent configuration version drift and automatically repair common configuration inconsistencies, ensuring that persisted configuration data can be synchronized to the configuration version expected by an application.

ConfigManager is intentionally not:

* A schema validation framework
* A serialization framework
* A configuration storage framework

ConfigManager focuses exclusively on:

* Configuration version management
* Configuration migration
* Configuration synchronization
* Configuration evolution

---

# Core Principles

## Explicit Synchronization

Configuration synchronization is never automatic.

Applications explicitly decide when synchronization should occur.

Example:

```cpp
auto state =
    runtime.inspect(
        config,
        supportedVersion);

if (state.status == SyncStatus::UpgradeRequired)
{
    runtime.synchronize(
        config,
        supportedVersion);
}
```

Configuration access operations never trigger migrations.

---

## Pure Configuration Data

Configuration data and version metadata are separate concerns.

`ConfigModel` contains only configuration data.

```cpp
ConfigModel model;
```

No version information is stored inside the model.

---

## Single Source Of Truth

The current configuration version is stored exclusively in `VersionedConfig`.

```cpp
struct VersionedConfig
{
    VersionId version;

    ConfigModel model;
};
```

No API accepts a source version parameter.

The current version is always obtained from the configuration itself.

---

## Storage Agnostic

ConfigManager operates on streams.

Supported sources include:

* Files
* Memory streams
* Network streams
* Database streams
* Custom stream implementations

---

## Non-Exception Public API

Public APIs do not throw exceptions.

Recoverable failures are represented through Result types.

```cpp
Result<T>
```

Two boundary rules make this guarantee concrete:

* Exceptions thrown by user-supplied callbacks (migration functions,
  default factories) never escape the library. They are caught at the
  invocation site and converted to a `Result` error.
* `std::bad_alloc` is not a recoverable configuration error. Memory
  exhaustion may propagate.

---

# High-Level Architecture

```text
Application
      |
      v

 ConfigRuntime
      |
      +-------------------+-------------------+
      |                   |                   |
      v                   v                   v

VersionCatalog      MigrationEngine     VersionedConfig
                         |                    |
                         v                    v
                  MigrationRegistry      ConfigModel
                                              |
                                              v
                                        ConfigInterface
```

`ConfigRuntime` owns both `VersionCatalog` and `MigrationRegistry`, and
drives `MigrationEngine`. `MigrationEngine` resolves migrations through
`MigrationRegistry`.

---

# Component Responsibilities

## ConfigModel

Represents configuration data.

Responsibilities:

* Data storage
* Node traversal
* Value access
* Value modification

Example:

```cpp
model.get<int>(
    "server.port");

model.set(
    "server.port",
    8080);
```

The root of a model is always an Object.

Non-responsibilities:

* Version management
* Synchronization
* Migration

---

## VersionedConfig

Represents persisted configuration state.

```cpp
struct VersionedConfig
{
    VersionId version;

    ConfigModel model;
};
```

Responsibilities:

* Store configuration version
* Associate version metadata with configuration data

---

## ConfigInterface

Responsible for serialization and parsing.

```cpp
class IConfigInterface
{
public:
    virtual ~IConfigInterface() = default;

    virtual Result<VersionedConfig>
    load(std::istream& stream) = 0;

    virtual Result<void>
    save(
        const VersionedConfig& config,
        std::ostream& stream) = 0;
};
```

Responsibilities:

* Parse configuration formats
* Serialize configuration formats
* Load version metadata
* Store version metadata

Non-responsibilities:

* Migration
* Synchronization
* Version management
* Repair

`load()` only parses and deserializes. It never modifies, repairs, or
migrates the configuration. Repair and migration belong exclusively to
`ConfigRuntime`.

The configuration version is mandatory. If a stream contains no version
metadata, `load()` fails with `InvalidVersion`; the library never
guesses or assumes a version for unversioned data, because such data
cannot be handled reliably.

Each ConfigInterface implementation is responsible for serializing,
deserializing, and reading/writing the configuration version according
to its storage format.

The library does not impose a common serialization envelope across
different configuration formats.

The version carrier is reserved within each format. load() consumes it
as metadata — it never appears in the resulting ConfigModel — and
save() writes it from `VersionedConfig::version`. If the model itself
already contains the reserved carrier (for example a `__version` key in
JSON), save() fails with `SerializationError` rather than silently
producing ambiguous output.

The model root is always an Object. Documents with a non-object root,
such as a top-level JSON array, fail to load with `ParseError`.

Implementations:

```text
JsonInterface
YamlInterface
XmlInterface
IniInterface
```

---

## VersionCatalog

Authoritative source of version metadata.

Responsibilities:

* Register versions
* Provide default configuration factories
* Provide the latest known version
* Provide version metadata
* Define version ordering and adjacency (the next registered version)

Example:

```cpp
catalog.registerVersion(
{
    .version = 4,
    .defaultFactory =
        createDefaultsV4
});
```

```cpp
catalog.latestVersion();

catalog.createDefault(4);
```

Non-responsibilities:

* Registering migrations
* Resolving migration paths

---

## MigrationRegistry

Authoritative source of migrations.

Responsibilities:

* Register migrations
* Find migrations between adjacent versions
* Validate the registered migration set

Example:

```cpp
registry.registerMigration(
    3,
    4,
    migrateV3ToV4);
```

```cpp
registry.findMigration(3, 4);

registry.validate(catalog);
```

Non-responsibilities:

* Version metadata
* Default configuration factories

---

## MigrationEngine

Performs configuration transformations between versions.

Responsibilities:

* Determine the next version from VersionCatalog order
* Resolve migrations via MigrationRegistry
* Execute migrations
* Update configuration versions

Example:

```cpp
Result<void>
migrate(
    VersionedConfig& config,
    VersionId targetVersion);
```

Current version is determined from:

```cpp
config.version
```

Migration APIs never accept a source version parameter.

---

## ConfigRuntime

High-level synchronization service.

`ConfigRuntime` owns the configuration lifecycle components:

```text
ConfigRuntime
    |
    +-- VersionCatalog
    +-- MigrationRegistry
    +-- MigrationEngine
```

Responsibilities:

* Inspect configuration state
* Determine synchronization requirements
* Own the entire synchronization pipeline
* Invoke MigrationEngine
* Repair configurations using version defaults
* Create default configurations

Example:

```cpp
auto state =
    runtime.inspect(
        config,
        supportedVersion);
```

```cpp
runtime.synchronize(
    config,
    supportedVersion);
```

Internally:

```cpp
migrationEngine.migrate(
    config,
    supportedVersion);
```

---

# Version Catalog

Each version is represented through a VersionArtifact.

```cpp
struct VersionArtifact
{
    VersionId version;

    DefaultFactory defaultFactory;
};
```

Migration relationships form a linear chain over the registered
versions, ordered ascending.

```text
v1 -> v2 -> v3 -> v4
```

Adjacency is defined by catalog order: two versions are adjacent when
no other registered version lies between them. Version numbers do not
need to be consecutive integers; if the catalog registers v1, v2 and
v4, then v2 and v4 are adjacent and the required migration is
v2 -> v4.

Only migrations between adjacent registered versions are supported.

```text
v1 -> v2
v2 -> v3
v3 -> v4
```

Migrations that skip a registered version (v1 -> v4 above) are
intentionally unsupported.

This keeps migrations small, deterministic, and easy to maintain.

---

# Registry Validation

During ConfigRuntime construction, the MigrationRegistry is validated
against the versions declared in the VersionCatalog.

Validation includes:

* Every registered version has a migration to the next registered
  version (catalog order), up to the latest.
* No duplicate migrations exist.
* Migration versions reference versions known to the VersionCatalog.
* Only migrations between adjacent registered versions may be
  registered.

Construction also fails, with `InvalidVersion`, when the catalog
declares no versions: an empty catalog has no latest version to
synchronize toward.

If validation fails, ConfigRuntime construction fails before any
configuration can be synchronized.

This gives developers immediate feedback rather than discovering
problems in the field.

---

# Synchronization Workflow

## Inspection

```cpp
auto state =
    runtime.inspect(
        config,
        supportedVersion);
```

Result:

```cpp
struct SyncState
{
    VersionId currentVersion;

    VersionId targetVersion;

    SyncStatus status;
};
```

The synchronization status is represented explicitly:

```cpp
enum class SyncStatus
{
    InSync,

    UpgradeRequired,

    DowngradeRequired
};
```

* **InSync** – Configuration matches the application's supported version.
* **UpgradeRequired** – Configuration is older and can be migrated.
* **DowngradeRequired** – Configuration is newer than the application supports.

There is no failure status. Failures during synchronization are
reported through the `Result` error channel, and the caller's
configuration is guaranteed unmodified on error.

Because migrations are forward-only, a newer configuration cannot be
downgraded. When the configuration version is newer than the
application's supported version:

* `synchronize()` performs no modifications.
* No repair is attempted.
* `SyncStatus::DowngradeRequired` is returned.
* The application must decide how to proceed.

---

## Synchronization

```cpp
runtime.synchronize(
    config,
    supportedVersion);
```

`supportedVersion` must be a version registered in the VersionCatalog.
If it is not, synchronize() fails with `InvalidVersion` before any other
work is performed.

When an upgrade is required, the configuration's current version must
also be registered: an unregistered persisted version fails with
`InvalidVersion` before any modification, instead of failing mid-chain
inside the migration engine. A version newer than `supportedVersion` is
reported as `DowngradeRequired` regardless of registration —
configurations written by newer applications are expected to carry
versions this catalog does not know.

A convenience overload omits the parameter and targets the latest
registered version:

```cpp
runtime.synchronize(config);
```

Synchronization is a single pipeline owned entirely by `ConfigRuntime`.
There is exactly one place migration happens, exactly one place repair
happens, and exactly one commit point.

```text
                 synchronize()
                       |
         Validate supportedVersion --> Error: InvalidVersion
                       |
              Determine SyncStatus
                       |
        DowngradeRequired -----------> return (no modification)
                       |
        InSync / UpgradeRequired
                       |
  Validate current version registered --> Error: InvalidVersion
                       |
              Copy configuration
                       |
          Apply incremental migrations   (skipped when InSync)
                       |
            Repair missing fields
                       |
            Success? ------------------ No
                 |                       |
                 | Yes                   v
                 v                Error returned,
        Anything changed?        original untouched
                 |
                 +------ No ---------> return InSync (no commit)
                 |
                 | Yes
                 v
          Replace original
                 |
                 v
               InSync
```

An `InSync` configuration still passes through the repair step. A
configuration can be at the correct version and still have missing keys
(drift); synchronization is the single place such drift is repaired.
When neither migration nor repair changed anything, the commit is
skipped and the original configuration is left untouched.

Synchronization is always explicit.

---

## Synchronization Guarantees

Synchronization is transactional with respect to the in-memory VersionedConfig.

* Migrations are executed on a temporary working copy.
* The original VersionedConfig remains unchanged until all migration
  and repair steps complete successfully.
* The caller's configuration is only updated after successful completion.
* On failure, the error is returned through `Result` and the caller's
  configuration is guaranteed unmodified.
* The commit occurs only when migration or repair actually changed
  something; a clean `InSync` pass leaves the original object untouched.
* A commit replaces the caller's model, which invalidates every
  ConfigNode handle previously obtained from that configuration (see
  ConfigNode Lifetime).
* The library never writes to persistent storage. Saving the migrated
  configuration remains the responsibility of the application.

---

## Configuration Repair

Repair is performed by `ConfigRuntime` as a step of the synchronization
pipeline. It is never performed during `load()`.

Repair runs once, after all migrations complete, using the target
version's default configuration, and before the single commit point.

Repair runs for both `InSync` and `UpgradeRequired` configurations. A
configuration at the correct version can still have missing keys;
synchronization is where that drift is repaired.

Repair will:

* Add missing keys using the target version's default values.

Repair will not:

* Replace existing values
* Validate types
* Remove unknown keys
* Enforce schema constraints

Arrays are atomic. A default array is copied only when its key is
entirely absent from the configuration. Existing arrays are never
merged element-wise, extended, or truncated.

Repair descends only where both the configuration and the defaults hold
an Object at the same key. A key present in the configuration with a
different type than the default is left untouched and its default
children are not backfilled: presence always wins over shape.

This allows ConfigManager to recover from common configuration drift
without introducing a full schema validation framework, and keeps the
library from slowly turning into a schema validator.

Because repair runs after migration, migration authors never rely on
repair to populate fields. A migration that needs a field creates it
explicitly. This keeps migrations deterministic.

---

# Direct Migration Workflow

Migration can be used independently of synchronization.

```cpp
migrationEngine.migrate(
    config,
    5);
```

This allows applications to:

* Perform validation before migration
* Perform validation after migration
* Use external schema validators
* Decide when to migrate using their own comparison logic
* Migrate configurations without using ConfigRuntime

Direct migration has raw semantics. Unlike synchronize(), it:

* Mutates the configuration in place and is not transactional. The
  version advances per step, so a mid-chain failure leaves the
  configuration at the last successfully reached version. Callers
  needing rollback should clone the model first.
* Performs no repair. Missing-key backfill is the caller's
  responsibility.
* Does not force registry validation. Callers should invoke
  `registry.validate(catalog)` themselves if they bypass ConfigRuntime.
* Requires the target version to be registered in the catalog and not
  below the current version; otherwise it fails with `InvalidVersion`
  (downgrades are never performed).
* Is a successful no-op when the configuration is already at the target
  version.

---

# Configuration Access

Configuration access never triggers synchronization.

```cpp
auto hostname =
    model.get<std::string>(
        "network.hostname");
```

Behavior:

```text
Read Value
Return Value
```

No migration checks occur.

---

# Scalar Type Conversion

Typed reads are strict. A conversion is performed only when it is
provably lossless:

* `Bool` and `String` never convert to or from any other type.
* `Int` converts to `Double` only when the value is exactly
  representable as a double.
* `Double` converts to `Int` only when the value is integral and within
  the range of the integer type.

All other combinations fail with `InvalidType`. Values are never
stringified and strings are never parsed into numbers.

`Int` is stored as a 64-bit signed integer. Reads into narrower or
differently signed integer types succeed only when the stored value is
exactly representable in the requested type; otherwise they fail with
`InvalidType`.

---

# Error Handling

## Philosophy

Public APIs do not use exceptions.

All recoverable failures are represented using Result.

```cpp
Result<T>
```

---

## C++20 Compatibility

```cpp
template<typename T>
using Result =
    tl::expected<T, Error>;
```

Future implementations may transparently adopt:

```cpp
std::expected
```

when available.

---

## Error Model

```cpp
enum class ErrorCode
{
    InvalidPath,

    NodeNotFound,

    InvalidType,

    ParseError,

    SerializationError,

    MigrationFailed,

    MissingMigration,

    InvalidVersion
};
```

```cpp
struct Error
{
    ErrorCode code;

    std::string message;
};
```

An owned string avoids lifetime issues and allows richer diagnostic
messages.

---

# ConfigPath Grammar

Object traversal:

```text
network.hostname
```

Array traversal:

```text
users[0].name
```

Nested arrays:

```text
groups[0].users[4].name
```

Reserved characters:

```text
.
[
]
```

Version 1 does not support escaping.

The empty string is not a valid path and fails with `InvalidPath`. No
string path addresses the root node; the root is reached through the
model's root handle.

---

## Path Semantics

Path creation follows upsert semantics.

If intermediate nodes do not exist they are automatically created.

Example:

```cpp
set("network.timeout", 10)
```

creates "network" if necessary before writing "timeout".

This makes writing migrations much simpler and more predictable.

Upsert creates structure but never changes type. If an existing node's
type conflicts with what the path requires — for example
`set("network.timeout", 10)` when `network` is currently a string — the
write fails with `InvalidType` and nothing is modified. Migrations that
change a node's type remove the node explicitly first.

Array indices are bounded. A write may target an existing element or
the index one past the end (an append); larger indices fail with
`NodeNotFound`. Missing intermediate arrays are created empty, so a
write to `users[0]` when `users` is absent creates the array and
appends its first element. Holes are never fabricated.

`contains()` never fails: it returns false for malformed paths as well
as absent ones.

---

# ConfigNode Lifetime

ConfigModel owns all storage.

ConfigNode acts as a lightweight, read-only handle. All mutation goes
through ConfigModel.

A ConfigNode remains valid until:

* ConfigModel is destroyed
* Referenced node is removed

Moving nodes within the model does not invalidate handles.

Removing nodes invalidates handles referencing those nodes; such
handles detectably report themselves invalid.

Node storage lives on the heap, owned by the model, so moving the
ConfigModel object itself also keeps handles valid: they follow the new
owner. Destroying a model — including move-assigning another model onto
it — destroys the storage and invalidates every handle into it. This is
not detectable through the handle; using one afterwards is undefined
behavior, as with invalidated container iterators. In particular, a
successful synchronize() that commits replaces the caller's model and
therefore invalidates all handles obtained from it beforehand.

---

# Thread Safety

ConfigRuntime and ConfigModel are not thread-safe.

Concurrent access must be synchronized by the application.

---

# Architectural Decision Records

## ADR-001

Configuration synchronization is the primary responsibility of the framework.

Migration exists to support synchronization.

---

## ADR-002

Synchronization is explicit.

Configuration access operations never trigger migrations.

---

## ADR-003

ConfigModel contains configuration data only.

Version metadata is stored separately.

---

## ADR-004

ConfigInterfaces operate on streams rather than filesystem paths.

---

## ADR-005

Public APIs do not use exceptions.

Recoverable failures are represented using Result types.

---

## ADR-006

Schema validation is outside the scope of the framework.

Applications may integrate any validation library they choose.

---

## ADR-007

VersionedConfig is the authoritative source of the current configuration version.

Migration APIs accept only a target version.

The current version is never supplied by callers.

---

## ADR-008

Migrations are adjacent-only and forward-only.

Adjacency is defined by catalog order: two versions are adjacent when no other
registered version lies between them. Version numbers need not be consecutive
integers.

Only migrations between adjacent registered versions are supported. Migrations
that skip a registered version are intentionally unsupported, keeping migrations
small, deterministic, and easy to maintain.

A configuration newer than the application's supported version cannot be
downgraded. On DowngradeRequired, synchronize() performs no modification and
returns the status; the application decides how to proceed.

---

## ADR-009

Synchronization is a single transactional pipeline owned entirely by
ConfigRuntime: determine status, copy, migrate, repair, then commit.

Migration and repair run on a working copy, and the caller's configuration is
updated only after successful completion. There is exactly one place migration
happens, one place repair happens, and one commit point, making runtime behavior
deterministic.

Failures propagate through the Result error channel; there is no failure
status. The commit occurs only when migration or repair actually changed the
configuration; otherwise the caller's object is left untouched.

---

## ADR-010

ConfigManager performs lightweight repair as a step of the synchronization
pipeline, never during load. Repair runs once, after migrations complete,
backfilling missing fields from the target version's default configuration.

Repair never overwrites existing values, never removes unknown fields, never
corrects types, and does not perform schema validation.

Repair runs for both InSync and UpgradeRequired configurations, so
version-correct drift is repaired too. Arrays are atomic: a default array is
copied only when its key is entirely absent and is never merged element-wise.

---

## ADR-011

The MigrationRegistry is validated during ConfigRuntime construction against
the versions declared in the VersionCatalog.

If validation fails, construction fails before any configuration can be
synchronized, surfacing registration errors immediately rather than in the field.

---

## ADR-012

VersionCatalog and MigrationRegistry are separate components.

VersionCatalog owns version metadata and default factories. MigrationRegistry
owns migrations and their validation. ConfigRuntime owns both. This keeps each
component aligned with a single responsibility.

---

## ADR-013

load() only parses and deserializes. It never modifies, repairs, or migrates
the configuration. This keeps configuration backends free of business logic.

---

## ADR-014

The configuration version is mandatory in persisted data.

load() fails with InvalidVersion when a stream carries no version metadata.
Unversioned configurations cannot be handled reliably, so the library refuses
to guess or assume a version for them.

---

## ADR-015

Scalar type conversions are strict and lossless-only.

Int converts to Double only when the value is exactly representable as a
double. Double converts to Int only when the value is integral and within the
range of the integer type. Bool and String never convert. All other
combinations fail with InvalidType.

---

## ADR-016

MigrationEngine is a supported standalone entry point with raw semantics.

Applications may drive migration directly, using their own comparison and
validation logic. The direct path mutates in place, is not transactional,
performs no repair, and does not force registry validation. ConfigRuntime
remains the safe, orchestrated path.

---

## ADR-017

Migration functions receive a MigrationContext rather than the model directly.

The context exposes the configuration model and the step's source and target
versions. It can grow new capabilities (logging, defaults access) without
changing the migration function signature — a signature change would break
every registered migration in every consuming application.

---

## ADR-018

The non-throwing public API is enforced at two concrete boundaries.

Exceptions thrown by user-supplied callbacks — migration functions and
default factories — never escape the library. They are caught at the
invocation site and mapped to `MigrationFailed`, with the exception
message preserved in `Error::message`.

`std::bad_alloc` is not a recoverable configuration error. Memory
exhaustion may propagate; the Result channel is reserved for failures an
application can meaningfully handle.

---

## ADR-019

Upsert creates structure but never changes type.

A write whose path conflicts with an existing node's type fails with
`InvalidType` and modifies nothing. Migrations that change a node's type
remove the node explicitly first.

Array writes are bounded: an index may address an existing element or
append at one past the end. Larger indices fail with `NodeNotFound`, and
missing intermediate arrays are created empty. Holes are never
fabricated.

---

## ADR-020

The model root is always an Object, and each backend's version carrier
is reserved metadata.

load() consumes the version carrier — it never appears in the resulting
model — and rejects non-object document roots with `ParseError`. save()
writes the carrier from `VersionedConfig::version` and fails with
`SerializationError` if the model itself already contains the reserved
carrier, rather than silently producing ambiguous output.
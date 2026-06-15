# ConfigManager Architecture

## Overview

ConfigManager is a C++ library for configuration versioning, migration, and synchronization.

Its primary goal is:

> Ensure that persisted configuration data can be synchronized to the configuration version expected by an application.

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

if (!state.synchronized)
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

---

# High-Level Architecture

```text
Application
      |
      |
      +--------------------+
      |                    |
      v                    v

 ConfigRuntime      MigrationEngine
      |                    |
      +---------+----------+
                |
                v

         VersionCatalog
                |
                v

        VersionedConfig
                |
                v

           ConfigModel
                |
                v

        ConfigInterface
```

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

Implementations:

```text
JsonInterface
YamlInterface
XmlInterface
IniInterface
```

---

## VersionCatalog

Authoritative source of version artifacts.

Responsibilities:

* Register versions
* Register migrations
* Register default configuration factories
* Resolve migration paths

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
catalog.registerMigration(
    3,
    4,
    migrateV3ToV4);
```

---

## MigrationEngine

Performs configuration transformations between versions.

Responsibilities:

* Build migration paths
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

Responsibilities:

* Inspect configuration state
* Determine synchronization requirements
* Invoke MigrationEngine
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

Migration relationships form a directed graph.

```text
v1 -> v2 -> v3 -> v4
```

Preferred migration strategy:

```text
v1 -> v2
v2 -> v3
v3 -> v4
```

Avoid direct jumps:

```text
v1 -> v4
```

unless explicitly required.

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

    bool synchronized;
};
```

---

## Synchronization

```cpp
runtime.synchronize(
    config,
    supportedVersion);
```

Workflow:

```text
Determine Current Version
          |
Determine Target Version
          |
Resolve Migration Path
          |
Invoke MigrationEngine
          |
Update Version
```

Synchronization is always explicit.

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
* Migrate configurations without using ConfigRuntime

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
class Error
{
public:
    ErrorCode code() const;

    std::string_view message() const;
};
```

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

---

# ConfigNode Lifetime

ConfigModel owns all storage.

ConfigNode acts as a lightweight handle.

A ConfigNode remains valid until:

* ConfigModel is destroyed
* Referenced node is removed

Moving nodes does not invalidate handles.

Removing nodes invalidates handles referencing those nodes.

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
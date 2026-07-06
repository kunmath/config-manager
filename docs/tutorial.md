# Tutorial

This walk-through takes you from an empty configuration model to the full
application lifecycle: load a file, inspect its version, synchronize it to the
version your code expects, use it, save it. Each section corresponds to a
runnable program in [`../examples/`](../examples/README.md).

Everything lives in namespace `configmanager`; the umbrella header provides the
alias `cfg`:

```cpp
#include <configmanager/configmanager.hpp>   // everything in core, alias cfg::
```

## 1. The model is a typed tree, errors are values

*(runnable version: [`examples/01_basic_model.cpp`](../examples/01_basic_model.cpp))*

`ConfigModel` is a JSON-like tree — objects, arrays, scalars — addressed by
dotted paths. Every fallible call returns `Result<T>` (an alias of
`tl::expected<T, Error>`) instead of throwing:

```cpp
cfg::ConfigModel model;                          // starts as an empty root object

// set() has upsert semantics: intermediate objects are created as needed.
model.set("network.host", std::string("127.0.0.1"));
model.set("network.port", 8080);
model.set("network.endpoints[0]", std::string("/health"));   // creates the array

cfg::Result<std::string> host = model.get<std::string>("network.host");
if (host) {
  std::cout << *host << "\n";
} else {
  std::cerr << host.error().message << "\n";     // Error{code, message}
}
```

Reads are strict: `get<int>` on a string is `InvalidType`, not `0`, and a
missing path is `NodeNotFound`. Writes are atomic — a failed `set()` leaves the
model exactly as it was. The details are in
[The data model and paths](model-and-paths.md).

For building whole subtrees (defaults, migration payloads) there is
`ConfigValue`, a plain copyable builder:

```cpp
cfg::ConfigValue server = cfg::ConfigValue::object();
server.set("host", cfg::ConfigValue::of(std::string("127.0.0.1")));
server.set("port", cfg::ConfigValue::of(8080));
model.set("server", std::move(server));          // insert the subtree
```

## 2. Declare your versions

*(runnable version: [`examples/02_versioning_and_migration.cpp`](../examples/02_versioning_and_migration.cpp))*

A `VersionCatalog` records every configuration version your application knows,
each with a **default factory** — a function returning that version's complete
default tree:

```cpp
cfg::ConfigValue v1Defaults() {
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("server_host", cfg::ConfigValue::of(std::string("127.0.0.1")));
  root.set("server_port", cfg::ConfigValue::of(8080));
  root.set("verbose",     cfg::ConfigValue::of(false));
  return root;
}

cfg::VersionCatalog catalog;
catalog.registerVersion({1, v1Defaults});
catalog.registerVersion({2, v2Defaults});
```

The defaults matter twice: `createDefault()` materializes them on first launch,
and **repair** fills missing keys from them during synchronization. They are the
single place a new setting's default value lives.

## 3. Write migrations

A migration is one function per **adjacent** version pair. It restructures the
*user's* data in place — it moves and reshapes what the user already set, and
leaves genuinely new keys to repair:

```cpp
cfg::Result<void> migrateV1ToV2(cfg::MigrationContext& ctx) {
  cfg::ConfigModel& model = ctx.model();

  // v2 groups the flat keys into a "server" section.
  auto host = model.getValue("server_host");                 // deep-copy out
  if (!host) return cfg::fail(host.error().code, std::move(host.error().message));
  if (auto put = model.set("server.host", *std::move(host)); !put) return put;
  return model.remove("server_host");
}

cfg::MigrationRegistry registry;
registry.registerMigration(1, 2, migrateV1ToV2);
```

Migrations receive a `MigrationContext` (the model plus the step's
`fromVersion()`/`toVersion()`), never the raw model — the context can grow
capabilities without breaking every registered migration
([ADR-017](Architecture.md#adr-017)).

## 4. Build the runtime — wiring is validated up front

```cpp
auto runtime = cfg::ConfigRuntime::create(std::move(catalog), std::move(registry));
if (!runtime) {
  // e.g. "missing migration 1 -> 2": schema-evolution mistakes surface at
  // startup, not during an upgrade in the field.
}
```

`create()` fails unless every adjacent version pair in the catalog has exactly
one registered migration and every migration references known, adjacent
versions ([ADR-011](Architecture.md#adr-011)).

## 5. The lifecycle: load → inspect → synchronize → save

*(runnable version: [`examples/03_relaygate_lifecycle.cpp`](../examples/03_relaygate_lifecycle.cpp))*

Serialization backends implement `IConfigInterface` over streams. Using JSON:

```cpp
#include <configmanager/backends/json_interface.hpp>

cfg::JsonInterface json;

std::ifstream in("app.json");
auto config = json.load(in);                 // Result<VersionedConfig>
if (!config) { /* ParseError / InvalidVersion */ }
```

`load()` **only parses**. It consumes the format's version carrier (the
top-level `"__version"` field in JSON) into `config->version` and never
migrates or repairs ([ADR-013](Architecture.md#adr-013)). A file without a
version is refused with `InvalidVersion` — the library never guesses
([ADR-014](Architecture.md#adr-014)).

Now bring the configuration to the version this build supports:

```cpp
constexpr cfg::VersionId kSupported = 2;

cfg::SyncState state = runtime->inspect(*config, kSupported);
// state.status: InSync | UpgradeRequired | DowngradeRequired

auto status = runtime->synchronize(*config, kSupported);
if (!status) {
  // Migration or repair failed; *config is guaranteed unmodified.
}
if (*status == cfg::SyncStatus::DowngradeRequired) {
  // The file was written by a NEWER application. Nothing was touched.
  // Typical responses: refuse to start, or run read-only. Above all: don't save.
}
```

`synchronize()` is one transaction: it clones the model, runs the migration
chain, then repair, and commits only if everything succeeded and something
actually changed. It is worth calling on *every* startup — an `InSync`
configuration still passes through repair, so keys deleted from the file get
their defaults restored. See
[Synchronization and repair](synchronization.md).

Use the configuration, then persist it — saving is always your decision; the
library never touches storage:

```cpp
auto port = config->model.get<std::int64_t>("server.port");
config->model.set("logging.level", std::string("warn"));

std::ofstream out("app.json");
json.save(*config, out);                     // writes "__version" back out
```

## 6. First launch — no file yet

```cpp
cfg::Result<cfg::VersionedConfig> fresh = runtime->createDefault(kSupported);
```

`createDefault()` runs the version's default factory and hands you a complete
`VersionedConfig` ready to save.

## Where to next

* [The data model and paths](model-and-paths.md) — the rules `set`/`get` play by
* [Versioning and migrations](versioning-and-migration.md) — migration
  authoring in depth, sparse version ids, direct `MigrationEngine` use
* [Serialization backends](serialization-backends.md) — XML mapping, writing
  your own backend
* [`examples/04_sensorbridge_xml.cpp`](../examples/04_sensorbridge_xml.cpp) —
  the same lifecycle over the XML backend

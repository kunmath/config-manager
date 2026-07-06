# ConfigManager

A C++20 library for **configuration versioning, migration, and synchronization**.

> Prevent configuration version drift and automatically repair common
> configuration inconsistencies, ensuring that persisted configuration data can
> be synchronized to the configuration version expected by an application.

Your application's configuration schema evolves. Files on disk don't. ConfigManager
closes that gap: it knows every version your application has ever shipped, migrates
old configuration files forward step by step, and backfills missing keys from the
target version's defaults — transactionally, and only when you ask it to.

ConfigManager is intentionally **not**:

* a schema validation framework,
* a serialization framework,
* a configuration storage framework.

It does one job — keeping persisted configuration in sync with the version your
code expects — and composes with whatever validation, storage, or DI layers you
already have.

## Quick start

```cpp
#include <configmanager/configmanager.hpp>              // namespace alias: cfg
#include <configmanager/backends/json_interface.hpp>

// 1. Declare every version your application knows, with its defaults.
cfg::VersionCatalog catalog;
catalog.registerVersion({1, v1Defaults});   // DefaultFactory: () -> ConfigValue
catalog.registerVersion({2, v2Defaults});

// 2. Register one migration per adjacent version pair.
cfg::MigrationRegistry registry;
registry.registerMigration(1, 2, [](cfg::MigrationContext& ctx) -> cfg::Result<void> {
  // Restructure the user's data in place; repair fills new keys later.
  auto host = ctx.model().getValue("server_host");
  if (!host) return cfg::fail(host.error().code, std::move(host.error().message));
  if (auto put = ctx.model().set("server.host", *std::move(host)); !put) return put;
  return ctx.model().remove("server_host");
});

// 3. Build the runtime. create() validates the catalog/registry wiring —
//    a missing or non-adjacent migration fails here, not in the field.
auto runtime = cfg::ConfigRuntime::create(std::move(catalog), std::move(registry));
if (!runtime) { /* runtime.error().message */ }

// 4. Load -> inspect -> synchronize -> use -> save. Nothing is implicit.
cfg::JsonInterface json;
std::ifstream in("app.json");
auto config = json.load(in);                       // Result<VersionedConfig>
if (!config) { /* config.error() */ }

auto status = runtime->synchronize(*config, 2);    // migrate + repair, one transaction
if (!status) { /* config untouched on error */ }
if (*status == cfg::SyncStatus::DowngradeRequired) { /* file is from a newer app */ }

auto port = config->model.get<int>("server.port"); // typed, strict, Result-based
config->model.set("server.port", 9090);

std::ofstream out("app.json");
json.save(*config, out);                           // saving stays your decision
```

Runnable, commented versions of this flow live in [`examples/`](examples/README.md).

## Features

* **Explicit synchronization** — configuration access never triggers migration;
  `synchronize()` runs only when you call it, transactionally: on any failure
  your configuration is guaranteed unmodified.
* **Adjacent-only, forward-only migrations** — one small function per version
  step; the runtime chains them (`v1 → v2 → v3`) and refuses to guess about
  downgrades.
* **Repair without schema validation** — missing keys are backfilled from the
  target version's defaults; existing values are never overwritten, unknown keys
  never removed.
* **Non-throwing public API** — every fallible operation returns
  `Result<T>` (`tl::expected`); exceptions from your callbacks or parser
  libraries are caught at the boundary and mapped to error codes.
* **Typed, strict data model** — a JSON-like tree addressed by dotted paths
  (`server.listeners[0].port`) with lossless-only scalar conversion and
  insertion-order-preserving objects.
* **Storage agnostic** — backends operate on `std::istream`/`std::ostream`;
  files, memory, sockets, databases are all the application's business.
* **Dependency-light** — the core links only header-only `tl::expected`; each
  serialization backend is an opt-in CMake target with its own pinned dependency.

## Serialization backends

| Format | CMake target | Parser dependency | Version carrier | Status |
|---|---|---|---|---|
| JSON | `configmanager::json` | nlohmann/json (`ordered_json`) | top-level `"__version"` | ✅ available |
| XML | `configmanager::xml` | pugixml | root attribute `<config version="N">` | ✅ available |
| YAML | `configmanager::yaml` | yaml-cpp | top-level `version:` | 🚧 planned |
| INI | `configmanager::ini` | none (hand-written) | `[meta] version=N` | 🚧 planned |

An application that only needs versioning/migration links `configmanager::core`
alone and pulls in no parser library at all. You can also implement
[your own backend](docs/serialization-backends.md#writing-your-own-backend)
against `IConfigInterface`.

## Integration

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(ConfigManager
  GIT_REPOSITORY <this repository's URL>
  GIT_TAG        main)
FetchContent_MakeAvailable(ConfigManager)

target_link_libraries(my_app PRIVATE configmanager::core configmanager::json)
```

### add_subdirectory

```cmake
add_subdirectory(third_party/config-manager)
target_link_libraries(my_app PRIVATE configmanager::core configmanager::json)
```

### Installed package

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCONFIGMANAGER_BUILD_TESTS=OFF
cmake --build build && cmake --install build --prefix /your/prefix
```

```cmake
find_package(ConfigManager CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE configmanager::core configmanager::json)
```

When ConfigManager is built (FetchContent, `add_subdirectory`, or the install
build above), dependencies are resolved via `find_package` first and fetched
(pinned tags) as a fallback, so no vcpkg or conan is needed. Distro packagers
can force `find_package`-only resolution with
`-DCONFIGMANAGER_USE_SYSTEM_DEPS=ON`. Consuming an *installed* ConfigManager
via `find_package` requires its dependencies (`tl-expected`; `pugixml` when
the XML backend was built against a system pugixml) to be findable on the
consumer's machine — they are resolved with `find_dependency`, not fetched.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `CONFIGMANAGER_BUILD_JSON` | `ON` | Build the JSON backend (`configmanager::json`) |
| `CONFIGMANAGER_BUILD_XML` | `ON` | Build the XML backend (`configmanager::xml`) |
| `CONFIGMANAGER_BUILD_TESTS` | `ON` | Build the GoogleTest unit tests |
| `CONFIGMANAGER_BUILD_EXAMPLES` | `ON` | Build (and register with CTest) the example programs |
| `CONFIGMANAGER_USE_SYSTEM_DEPS` | `OFF` | Resolve all dependencies via `find_package` only — no network fetch |

### Requirements

* C++20 compiler (CI covers GCC and Clang)
* CMake ≥ 3.21
* No runtime dependencies beyond the (header-only) `tl::expected` for the core

## Building and testing

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build          # unit tests + examples
```

## Documentation

Start with the [documentation index](docs/README.md).

* [Tutorial](docs/tutorial.md) — from an empty model to the full
  load → inspect → synchronize → save lifecycle
* [The data model and paths](docs/model-and-paths.md)
* [Versioning and migrations](docs/versioning-and-migration.md)
* [Synchronization and repair](docs/synchronization.md)
* [Serialization backends](docs/serialization-backends.md) — format mappings and
  how to write your own backend
* [Error handling](docs/error-handling.md)
* [FAQ](docs/faq.md) · [Limitations](docs/limitations.md)

Design rationale lives in [docs/Architecture.md](docs/Architecture.md)
(principles and ADRs) and [docs/HighLevelDesign.md](docs/HighLevelDesign.md)
(concrete types, format mappings, error-code table). The guides link into both
rather than restating them.

## Contributing

Ideas, bug reports, docs, and code are all welcome — see
[CONTRIBUTING.md](CONTRIBUTING.md). The YAML and INI backends are specified
but unclaimed, if you want a self-contained place to start.

## License

[MIT](LICENSE)

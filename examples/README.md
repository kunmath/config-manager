# ConfigManager examples

A graduated tour of the library. Each example is a single self-contained
`.cpp` file (no shared helpers — everything is copy-pasteable), heavily
commented, and asserts its own expected outcomes: exit code 0 means every
step behaved as documented, so the examples double as smoke tests and run in
CI.

All examples include only the umbrella header `configmanager/configmanager.hpp`
(which defines the `cfg` namespace alias); the capstone additionally includes
the JSON backend header.

## 01_basic_model — the model is a typed tree, errors are values

Core only, no versioning or I/O. Builds a `ConfigValue` tree, adopts it into
a `ConfigModel`, then reads (`get<T>`, including an array path), writes
(`set`, atomic type-preserving upsert), and removes through dotted paths.
Deliberately triggers two failures — a cross-type write (`InvalidType`) and
an absent path (`NodeNotFound`) — to show that errors arrive as `Result<T>`
values, never exceptions.

## 02_versioning_and_migration — catalogs, migrations, synchronize

Core only, in memory. A two-version `VersionCatalog` with default factories,
one registered migration, and a `ConfigRuntime` driving
`inspect()`/`synchronize()`. The example is built to show the division of
labor: the **migration** moves a key the user had overridden (the override
survives), while **repair** fills a key that is new in v2 from its default
factory — migrations never hand-copy defaults.

## 03_relaygate_lifecycle — capstone: a daemon's config over its lifetime

Requires the JSON backend (`CONFIGMANAGER_BUILD_JSON=ON`; the example is
skipped otherwise). "relaygate" is a fictional API-gateway daemon whose
config schema evolved through three versions:

| | v1 (flat) | v2 (sectioned) | v3 (multi-listener + TLS + limits) |
|---|---|---|---|
| host | `server_host` | `server.host` | `server.listeners[0].host` |
| port | `server_port` | `server.port` | `server.listeners[0].port` |
| connection cap | `max_connections` | `server.max_connections` | `limits.max_connections` |
| log destination | `log_file` | `logging.file` | `logging.file` |
| log verbosity | `verbose` (bool) | `logging.level` (string) | `logging.level` (string) |
| TLS | — | — | `server.tls.{enabled,cert_file}` (new: repair) |
| request timeout | — | — | `limits.request_timeout_ms` (new: repair) |

One non-interactive run walks five phases: reset, first launch (no file →
defaults → save), steady-state relaunch (load → `InSync` → mutate → save),
upgrading a legacy v1 file whose user overrides must survive the 1→2→3
migration chain plus repair, and refusing a file written by a newer version
(`DowngradeRequired`, file untouched).

Usage: `03_relaygate_lifecycle [data_dir] [out_dir]` (defaults `./data`,
`./out`; CTest runs it from the build-tree `examples/` directory where
`data/` is copied).

## Building and running

Examples build by default (`CONFIGMANAGER_BUILD_EXAMPLES=ON`) and register
with CTest under the `examples.` prefix:

```sh
cmake -S . -B build
cmake --build build --parallel

# run just the examples
ctest --test-dir build --output-on-failure -R '^examples\.'

# or run a binary directly (03 wants to be run from build/examples/)
cd build/examples && ./03_relaygate_lifecycle
```

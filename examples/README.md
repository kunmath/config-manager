# ConfigManager examples

A graduated tour of the library. Each example is a single self-contained
`.cpp` file (no shared helpers — everything is copy-pasteable), heavily
commented, and asserts its own expected outcomes: exit code 0 means every
step behaved as documented, so the examples double as smoke tests and run in
CI.

All examples include only the umbrella header `configmanager/configmanager.hpp`
(which defines the `cfg` namespace alias); 03 additionally includes the JSON
backend header and 04 the XML backend header.

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

## 04_sensorbridge_xml — the XML backend end-to-end, on a real file

Requires the XML backend (`CONFIGMANAGER_BUILD_XML=ON`; the example is
skipped otherwise). "sensorbridge" is a fictional telemetry poller whose
config lives in XML. The runtime plumbing is the same as 02/03 — swapping
backends changes only the code that touches bytes — so this example spends
its phases on what is XML-specific:

* **the wire mapping** — loads the hand-authored `data/sensorbridge_v1.xml`
  (open it next to the code): elements only, a `type` attribute for
  non-string scalars (`bool`/`int`/`double`/`null`, absent means string),
  untyped elements with children as objects, `type="array"` with `<item>`
  children as arrays;
* **an upgrade over XML** — `synchronize()` walks v1→v2 (operator overrides
  survive the migration, the new `polling.jitter_pct` is repaired in) and
  the saved document's root attribute becomes `<config version="2">`;
* **the carrier is an attribute, not a member** — unlike JSON's
  `"__version"`, a config key literally named `version` is plain data and
  round-trips;
* **save-side name validation** — a model key XML cannot spell (`2fast`)
  fails `save()` with a `SerializationError` *value*, writing nothing;
* **order preservation (ADR-022)** — `load(save(x))` re-serializes
  byte-identically, so a load/save cycle never shuffles the operator's file.

Usage: `04_sensorbridge_xml [data_dir] [out_dir]` (defaults `./data`,
`./out`, same convention as 03).

## Building and running

Examples build by default (`CONFIGMANAGER_BUILD_EXAMPLES=ON`) and register
with CTest under the `examples.` prefix:

```sh
cmake -S . -B build
cmake --build build --parallel

# run just the examples
ctest --test-dir build --output-on-failure -R '^examples\.'

# or run a binary directly (03/04 want to be run from build/examples/)
cd build/examples && ./04_sensorbridge_xml
```

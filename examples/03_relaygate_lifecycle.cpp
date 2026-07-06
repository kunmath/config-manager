// Example 03 (capstone) — the full config lifecycle of "relaygate", a
// fictional API-gateway daemon, over the JSON backend.
//
// This wires everything together the way a real application would:
//
//   startup:  load file -> inspect -> synchronize -> use config -> save
//
// relaygate's schema has evolved through three versions:
//
//   v1 (flat):       server_host, server_port, log_file, verbose (bool),
//                    max_connections
//   v2 (sectioned):  server.{host,port,max_connections},
//                    logging.{file,level}   [verbose became logging.level]
//   v3 (multi-listener + TLS + limits):
//                    server.listeners[0].{host,port}, server.tls.{enabled,
//                    cert_file}, limits.{max_connections,request_timeout_ms},
//                    logging.{file,level}
//
// The run is a single non-interactive pass through five phases, each a
// situation a deployed daemon actually meets: first launch with no file,
// steady-state relaunch, upgrading a years-old v1 file (user overrides must
// survive two migrations), and encountering a file written by a *newer*
// relaygate (refuse, do not touch).
//
// Usage: 03_relaygate_lifecycle [data_dir] [out_dir]
//   data_dir (default ./data): ships relaygate_v1.json, relaygate_future.json
//   out_dir  (default ./out):  everything this run writes

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "configmanager/backends/json_interface.hpp"
#include "configmanager/configmanager.hpp"  // umbrella header, defines cfg::

namespace fs = std::filesystem;

namespace {

// Unwrap a Result or exit with the library's diagnostic (see example 01).
template <typename T>
T orDie(cfg::Result<T> result, const std::string& what) {
  if (!result) {
    std::cerr << what << " failed: " << result.error().message << "\n";
    std::exit(EXIT_FAILURE);
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(result).value();
  }
}

// Unwrap-and-compare: the phases below assert concrete outcomes so this
// example doubles as a smoke test — exit 0 means every phase behaved.
template <typename T>
void expectEq(cfg::Result<T> actual, const T& expected,
              const std::string& what) {
  const T value = orDie(std::move(actual), what);
  if (value != expected) {
    std::cerr << what << ": expected '" << expected << "', got '" << value
              << "'\n";
    std::exit(EXIT_FAILURE);
  }
}

// ---- Default factories --------------------------------------------------------
//
// One per catalog version. Each returns the *complete* default tree for its
// version — repair fills from these, so they are the single place a new
// setting's default lives.

// v1: everything flat at the root.
cfg::ConfigValue v1Defaults() {
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("server_host", cfg::ConfigValue::of(std::string("127.0.0.1")));
  root.set("server_port", cfg::ConfigValue::of(8080));
  root.set("log_file",
           cfg::ConfigValue::of(std::string("/var/log/relaygate.log")));
  root.set("verbose", cfg::ConfigValue::of(false));
  root.set("max_connections", cfg::ConfigValue::of(100));
  return root;
}

// v2: grouped into "server" and "logging" sections.
cfg::ConfigValue v2Defaults() {
  cfg::ConfigValue server = cfg::ConfigValue::object();
  server.set("host", cfg::ConfigValue::of(std::string("127.0.0.1")));
  server.set("port", cfg::ConfigValue::of(8080));
  server.set("max_connections", cfg::ConfigValue::of(100));

  cfg::ConfigValue logging = cfg::ConfigValue::object();
  logging.set("file",
              cfg::ConfigValue::of(std::string("/var/log/relaygate.log")));
  logging.set("level", cfg::ConfigValue::of(std::string("info")));

  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("server", std::move(server));
  root.set("logging", std::move(logging));
  return root;
}

// v3: multiple listeners, TLS, and a "limits" section.
cfg::ConfigValue v3Defaults() {
  cfg::ConfigValue listener = cfg::ConfigValue::object();
  listener.set("host", cfg::ConfigValue::of(std::string("127.0.0.1")));
  listener.set("port", cfg::ConfigValue::of(8080));

  cfg::ConfigValue tls = cfg::ConfigValue::object();
  tls.set("enabled", cfg::ConfigValue::of(false));
  tls.set("cert_file", cfg::ConfigValue::of(std::string("")));

  cfg::ConfigValue server = cfg::ConfigValue::object();
  server.set("listeners", cfg::ConfigValue::array().push(std::move(listener)));
  server.set("tls", std::move(tls));

  cfg::ConfigValue limits = cfg::ConfigValue::object();
  limits.set("max_connections", cfg::ConfigValue::of(100));
  limits.set("request_timeout_ms", cfg::ConfigValue::of(30000));

  cfg::ConfigValue logging = cfg::ConfigValue::object();
  logging.set("file",
              cfg::ConfigValue::of(std::string("/var/log/relaygate.log")));
  logging.set("level", cfg::ConfigValue::of(std::string("info")));

  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("server", std::move(server));
  root.set("limits", std::move(limits));
  root.set("logging", std::move(logging));
  return root;
}

// ---- Migrations ----------------------------------------------------------------
//
// Each migration restructures the *user's* data from one version to the
// next. They never fill in brand-new settings — repair does that from the
// target version's defaults — so migrations stay small even as versions grow.

// Relocate the subtree at `from` to `to`. The common move in restructuring
// migrations: deep-copy out, upsert at the new path (intermediate objects are
// created as needed), detach the original.
cfg::Result<void> moveKey(cfg::ConfigModel& model, std::string_view from,
                          std::string_view to) {
  auto value = model.getValue(from);
  if (!value) {
    return cfg::fail(value.error().code, std::move(value.error().message));
  }
  if (auto put = model.set(to, *std::move(value)); !put) {
    return put;
  }
  return model.remove(from);
}

// 1 -> 2: pure restructure. Flat keys move into sections; the boolean
// "verbose" is re-expressed as the richer "logging.level" string.
cfg::Result<void> migrateV1ToV2(cfg::MigrationContext& ctx) {
  cfg::ConfigModel& model = ctx.model();
  if (auto moved = moveKey(model, "server_host", "server.host"); !moved) {
    return moved;
  }
  if (auto moved = moveKey(model, "server_port", "server.port"); !moved) {
    return moved;
  }
  if (auto moved = moveKey(model, "max_connections", "server.max_connections");
      !moved) {
    return moved;
  }
  if (auto moved = moveKey(model, "log_file", "logging.file"); !moved) {
    return moved;
  }

  auto verbose = model.get<bool>("verbose");
  if (!verbose) {
    return cfg::fail(verbose.error().code,
                     std::move(verbose.error().message));
  }
  if (auto put = model.set("logging.level",
                           std::string(*verbose ? "debug" : "info"));
      !put) {
    return put;
  }
  return model.remove("verbose");
}

// 2 -> 3: the single host/port pair becomes listeners[0], and
// max_connections moves under "limits". Deliberately absent: server.tls and
// limits.request_timeout_ms are new in v3 with no v2 data to carry over, so
// this migration leaves them to repair.
cfg::Result<void> migrateV2ToV3(cfg::MigrationContext& ctx) {
  cfg::ConfigModel& model = ctx.model();

  auto host = model.getValue("server.host");
  if (!host) {
    return cfg::fail(host.error().code, std::move(host.error().message));
  }
  auto port = model.getValue("server.port");
  if (!port) {
    return cfg::fail(port.error().code, std::move(port.error().message));
  }
  cfg::ConfigValue listener = cfg::ConfigValue::object();
  listener.set("host", *std::move(host));
  listener.set("port", *std::move(port));
  if (auto put = model.set("server.listeners",
                           cfg::ConfigValue::array().push(std::move(listener)));
      !put) {
    return put;
  }
  if (auto removed = model.remove("server.host"); !removed) {
    return removed;
  }
  if (auto removed = model.remove("server.port"); !removed) {
    return removed;
  }

  return moveKey(model, "server.max_connections", "limits.max_connections");
}

// ---- File I/O over the serialization boundary -----------------------------------
//
// IConfigInterface implementations (here: JsonInterface) work on streams, so
// the application owns file handling. load() only parses — it never migrates
// or repairs — and consumes the "__version" carrier into
// VersionedConfig::version; save() writes the carrier back out first.

cfg::Result<cfg::VersionedConfig> loadFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return cfg::fail(cfg::ErrorCode::ParseError,
                     "cannot open " + path.string() + " for reading");
  }
  cfg::JsonInterface backend;
  return backend.load(in);
}

cfg::Result<void> saveFile(const cfg::VersionedConfig& config,
                           const fs::path& path) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return cfg::fail(cfg::ErrorCode::SerializationError,
                     "cannot open " + path.string() + " for writing");
  }
  cfg::JsonInterface backend;
  return backend.save(config, out);
}

std::string toJsonText(const cfg::VersionedConfig& config) {
  std::ostringstream out;
  cfg::JsonInterface backend;
  orDie(backend.save(config, out), "serialize for printing");
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path dataDir = argc > 1 ? fs::path(argv[1]) : fs::path("data");
  const fs::path outDir = argc > 2 ? fs::path(argv[2]) : fs::path("out");
  const fs::path configFile = outDir / "relaygate.json";
  const fs::path legacyFile = outDir / "legacy.json";

  // The runtime is assembled once at startup. create() validates the whole
  // catalog/registry wiring (every adjacent pair 1->2, 2->3 has exactly one
  // migration), so schema-evolution mistakes surface here, not during an
  // upgrade in the field.
  constexpr cfg::VersionId kLatest = 3;
  cfg::VersionCatalog catalog;
  orDie(catalog.registerVersion({1, v1Defaults}), "register v1");
  orDie(catalog.registerVersion({2, v2Defaults}), "register v2");
  orDie(catalog.registerVersion({3, v3Defaults}), "register v3");
  cfg::MigrationRegistry registry;
  orDie(registry.registerMigration(1, 2, migrateV1ToV2), "register 1->2");
  orDie(registry.registerMigration(2, 3, migrateV2ToV3), "register 2->3");
  cfg::ConfigRuntime runtime = orDie(
      cfg::ConfigRuntime::create(std::move(catalog), std::move(registry)),
      "ConfigRuntime::create");

  // ---- Phase 1: reset -------------------------------------------------------
  // Make the run repeatable: clear this run's outputs, keep the shipped data.
  std::cout << "[phase 1] reset " << outDir << "\n";
  std::error_code ec;
  fs::remove(configFile, ec);
  fs::remove(legacyFile, ec);
  fs::create_directories(outDir);

  // ---- Phase 2: first launch, no config file --------------------------------
  // Nothing to load: materialize the latest version's defaults and persist
  // them, exactly what a daemon does on first install.
  std::cout << "[phase 2] first launch: no " << configFile.filename()
            << ", creating v" << kLatest << " defaults\n";
  if (fs::exists(configFile)) {
    std::cerr << "reset failed to clear " << configFile << "\n";
    return EXIT_FAILURE;
  }
  {
    cfg::VersionedConfig config = orDie(runtime.createDefault(kLatest),
                                        "createDefault(latest)");
    orDie(saveFile(config, configFile), "save first-run config");
  }

  // ---- Phase 3: steady-state relaunch ---------------------------------------
  // Load what we just wrote. inspect() confirms InSync; synchronize() is
  // still worth calling every startup — an InSync config passes through
  // repair, so settings deleted from the file get their defaults restored.
  std::cout << "[phase 3] relaunch: loading " << configFile.filename() << "\n";
  {
    cfg::VersionedConfig config = orDie(loadFile(configFile),
                                        "load own output");
    const cfg::SyncState state = runtime.inspect(config, kLatest);
    if (state.status != cfg::SyncStatus::InSync) {
      std::cerr << "expected InSync when loading our own output\n";
      return EXIT_FAILURE;
    }
    const cfg::SyncStatus status = orDie(runtime.synchronize(config),
                                         "synchronize (steady state)");
    if (status != cfg::SyncStatus::InSync) {
      std::cerr << "expected synchronize to report InSync\n";
      return EXIT_FAILURE;
    }
    // The daemon runs; the operator turns logging up; the change persists.
    orDie(config.model.set("logging.level", std::string("warn")),
          "set logging.level");
    orDie(saveFile(config, configFile), "save steady-state config");
  }

  // ---- Phase 4: upgrade a legacy v1 file -------------------------------------
  // A config written years ago by relaygate v1, with real user overrides
  // (host 0.0.0.0, port 9090, verbose on, 200 connections). synchronize()
  // walks 1->2->3 and then repairs; the overrides must land in their new
  // homes and only the genuinely new keys get defaults.
  std::cout << "[phase 4] upgrade: legacy v1 file with user overrides\n";
  {
    fs::copy_file(dataDir / "relaygate_v1.json", legacyFile,
                  fs::copy_options::overwrite_existing);
    cfg::VersionedConfig config = orDie(loadFile(legacyFile), "load legacy");
    const cfg::SyncState state = runtime.inspect(config, kLatest);
    if (state.status != cfg::SyncStatus::UpgradeRequired) {
      std::cerr << "expected UpgradeRequired for the v1 file\n";
      return EXIT_FAILURE;
    }
    orDie(runtime.synchronize(config), "synchronize (upgrade)");

    // Overrides carried by the migrations:
    expectEq(config.model.get<std::string>("server.listeners[0].host"),
             std::string("0.0.0.0"), "upgraded listener host");
    expectEq(config.model.get<std::int64_t>("server.listeners[0].port"),
             std::int64_t{9090}, "upgraded listener port");
    expectEq(config.model.get<std::string>("logging.level"),
             std::string("debug"), "verbose:true mapped to logging.level");
    expectEq(config.model.get<std::int64_t>("limits.max_connections"),
             std::int64_t{200}, "upgraded max_connections");
    // New-in-v3 keys filled by repair from v3's defaults:
    expectEq(config.model.get<bool>("server.tls.enabled"), false,
             "repaired server.tls.enabled");
    expectEq(config.model.get<std::int64_t>("limits.request_timeout_ms"),
             std::int64_t{30000}, "repaired limits.request_timeout_ms");
    if (config.version != kLatest) {
      std::cerr << "expected the upgraded config to be at v3\n";
      return EXIT_FAILURE;
    }

    orDie(saveFile(config, legacyFile), "save upgraded legacy config");
    std::cout << "upgraded v1 -> v3, result:\n"
              << toJsonText(config) << "\n";
  }

  // ---- Phase 5: file from the future ----------------------------------------
  // A newer relaygate wrote this file (v99). Downgrading would lose data the
  // old code cannot understand, so synchronize() reports DowngradeRequired
  // and guarantees the config is untouched. The right move: refuse to start
  // (or run read-only) and, above all, do not save.
  std::cout << "[phase 5] file written by a newer relaygate\n";
  {
    cfg::VersionedConfig config = orDie(
        loadFile(dataDir / "relaygate_future.json"), "load future file");
    const cfg::SyncStatus status = orDie(
        runtime.synchronize(config, kLatest), "synchronize (future file)");
    if (status != cfg::SyncStatus::DowngradeRequired || config.version != 99) {
      std::cerr << "expected DowngradeRequired with the config untouched\n";
      return EXIT_FAILURE;
    }
    std::cout << "config is v" << config.version << ", we support v" << kLatest
              << ": refusing to downgrade; leaving the file alone\n";
  }

  std::cout << "03_relaygate_lifecycle: all phases behaved as expected\n";
  return EXIT_SUCCESS;
}

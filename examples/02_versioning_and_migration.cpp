// Example 02 — catalogs, migrations, synchronize: versioning in memory.
//
// A configuration schema evolves. The library models that evolution with
// three cooperating pieces:
//
//   * VersionCatalog — every version the application knows, each with a
//     default factory that produces that version's fully-populated defaults.
//   * MigrationRegistry — one function per adjacent version pair (v -> next)
//     that restructures *user data* from the old shape to the new one.
//   * ConfigRuntime — the orchestrator: inspect() reports where a config
//     stands, synchronize() runs migrations and then "repair" (filling keys
//     missing from the target version's defaults) as one transaction.
//
// The division of labor demonstrated below: a migration MOVES what the user
// already set (so overrides survive restructuring), while repair FILLS what
// is genuinely new in the target version (from its default factory).
// Migrations therefore never need to hand-copy defaults.
//
// Everything here is in memory; wiring in file I/O is example 03.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

#include "configmanager/configmanager.hpp"  // umbrella header, defines cfg::

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

// ---- Version 1: a flat schema -----------------------------------------------
// { "app_name": "demo", "timeout": 30 }
cfg::ConfigValue v1Defaults() {
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("app_name", cfg::ConfigValue::of(std::string("demo")));
  root.set("timeout", cfg::ConfigValue::of(30));
  return root;
}

// ---- Version 2: "timeout" moved under "network", plus a new key --------------
// { "app_name": "demo", "network": { "timeout": 30, "retries": 3 } }
cfg::ConfigValue v2Defaults() {
  cfg::ConfigValue network = cfg::ConfigValue::object();
  network.set("timeout", cfg::ConfigValue::of(30));
  network.set("retries", cfg::ConfigValue::of(3));  // new in v2
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("app_name", cfg::ConfigValue::of(std::string("demo")));
  root.set("network", std::move(network));
  return root;
}

// ---- Migration 1 -> 2 ---------------------------------------------------------
// Moves the user's "timeout" to its new home. Note what it does NOT do: it
// does not create "network.retries" — that key is new in v2 and repair fills
// it from v2's defaults after migrations finish. A migration only concerns
// itself with data that existed in the source version.
cfg::Result<void> migrateV1ToV2(cfg::MigrationContext& ctx) {
  cfg::ConfigModel& model = ctx.model();
  // getValue() deep-copies the subtree at a path; set() upserts it at the new
  // location (creating the "network" object on the way); remove() detaches
  // the old node. Returning any error aborts synchronize() transactionally —
  // the caller's config is left exactly as it was.
  auto timeout = model.getValue("timeout");
  if (!timeout) {
    return cfg::fail(timeout.error().code, std::move(timeout.error().message));
  }
  if (auto put = model.set("network.timeout", *std::move(timeout)); !put) {
    return put;
  }
  return model.remove("timeout");
}

const char* statusName(cfg::SyncStatus status) {
  switch (status) {
    case cfg::SyncStatus::InSync:
      return "InSync";
    case cfg::SyncStatus::UpgradeRequired:
      return "UpgradeRequired";
    case cfg::SyncStatus::DowngradeRequired:
      return "DowngradeRequired";
  }
  return "?";
}

}  // namespace

int main() {
  // ---- 1. Register versions and migrations ---------------------------------
  //
  // The catalog defines version ordering; the registry must provide exactly
  // one migration for each adjacent pair. ConfigRuntime::create() validates
  // that (a missing edge or a dangling endpoint fails construction), so a
  // mis-wired catalog is caught at startup, not mid-synchronize.
  cfg::VersionCatalog catalog;
  orDie(catalog.registerVersion({1, v1Defaults}), "register v1");
  orDie(catalog.registerVersion({2, v2Defaults}), "register v2");

  cfg::MigrationRegistry registry;
  orDie(registry.registerMigration(1, 2, migrateV1ToV2), "register 1->2");

  cfg::ConfigRuntime runtime = orDie(
      cfg::ConfigRuntime::create(std::move(catalog), std::move(registry)),
      "ConfigRuntime::create");

  // ---- 2. Start from a v1 config with a user override ----------------------
  //
  // createDefault(1) runs v1's factory. The override below stands in for a
  // value the user changed — the point of migrations is that it survives.
  cfg::VersionedConfig config = orDie(runtime.createDefault(1),
                                      "createDefault(1)");
  orDie(config.model.set("timeout", 45), "set timeout override");

  std::cout << "before: version " << config.version << ", timeout = "
            << orDie(config.model.get<std::int64_t>("timeout"), "get timeout")
            << ", has network.retries: "
            << config.model.contains("network.retries") << "\n";

  // ---- 3. inspect(): where does this config stand? --------------------------
  //
  // inspect() is a pure comparison against the version the application
  // supports — useful for logging or refusing to run — it never mutates.
  const cfg::SyncState state = runtime.inspect(config, 2);
  std::cout << "inspect: current " << state.currentVersion << ", target "
            << state.targetVersion << " -> " << statusName(state.status)
            << "\n";
  if (state.status != cfg::SyncStatus::UpgradeRequired) {
    std::cerr << "expected UpgradeRequired from inspect()\n";
    return EXIT_FAILURE;
  }

  // ---- 4. synchronize(): migrate, repair, commit ----------------------------
  //
  // One call runs the 1->2 migration, then repairs against v2's defaults,
  // and commits the result only if everything succeeded. The returned status
  // is the state *after* the work — InSync on a successful upgrade.
  const cfg::SyncStatus result = orDie(runtime.synchronize(config, 2),
                                       "synchronize");
  std::cout << "synchronize -> " << statusName(result) << "\n";

  // ---- 5. See who did what --------------------------------------------------
  //
  // network.timeout == 45: the MIGRATION moved the user's override.
  // network.retries == 3:  REPAIR filled the new key from v2's defaults.
  const auto movedTimeout = orDie(
      config.model.get<std::int64_t>("network.timeout"), "get moved timeout");
  const auto repairedRetries = orDie(
      config.model.get<std::int64_t>("network.retries"), "get retries");
  std::cout << "after: version " << config.version
            << ", network.timeout = " << movedTimeout << " (moved by migration)"
            << ", network.retries = " << repairedRetries
            << " (filled by repair)\n";

  if (config.version != 2 || movedTimeout != 45 || repairedRetries != 3 ||
      config.model.contains("timeout")) {
    std::cerr << "post-synchronize state is not what this example promises\n";
    return EXIT_FAILURE;
  }

  std::cout << "02_versioning_and_migration: all steps behaved as expected\n";
  return EXIT_SUCCESS;
}

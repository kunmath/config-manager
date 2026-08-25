#include <configmanager/backends/json_interface.hpp>
#include <configmanager/checkpoint_store.hpp>
#include <configmanager/configmanager.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {

template <typename T>
T orDie(cfg::Result<T> result, const std::string& operation) {
  if (!result) {
    std::cerr << operation << ": " << result.error().message << '\n';
    std::exit(EXIT_FAILURE);
  }
  return std::move(*result);
}

void orDie(cfg::Result<void> result, const std::string& operation) {
  if (!result) {
    std::cerr << operation << ": " << result.error().message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

cfg::ConfigValue v1Defaults() {
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("device_name", cfg::ConfigValue::of("sensor"));
  root.set("sample_period_ms", cfg::ConfigValue::of(1000));
  return root;
}

cfg::ConfigValue v2Defaults() {
  cfg::ConfigValue device = cfg::ConfigValue::object();
  device.set("name", cfg::ConfigValue::of("sensor"));
  cfg::ConfigValue sampling = cfg::ConfigValue::object();
  sampling.set("period_ms", cfg::ConfigValue::of(1000));
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("device", std::move(device));
  root.set("sampling", std::move(sampling));
  return root;
}

cfg::ConfigValue v3Defaults() {
  cfg::ConfigValue root = v2Defaults();
  cfg::ConfigValue upload = cfg::ConfigValue::object();
  upload.set("batch_size", cfg::ConfigValue::of(50));
  root.set("upload", std::move(upload));
  return root;
}

cfg::Result<void> moveValue(cfg::ConfigModel& model, const std::string& from,
                            const std::string& to) {
  cfg::Result<cfg::ConfigValue> value = model.getValue(from);
  if (!value) {
    return cfg::fail(value.error().code, std::move(value.error().message));
  }
  cfg::Result<void> set = model.set(to, std::move(*value));
  if (!set) {
    return set;
  }
  return model.remove(from);
}

cfg::Result<void> migrateV1ToV2(cfg::MigrationContext& context) {
  cfg::Result<void> moved =
      moveValue(context.model(), "device_name", "device.name");
  if (!moved) {
    return moved;
  }
  return moveValue(context.model(), "sample_period_ms", "sampling.period_ms");
}

cfg::Result<void> migrateV2ToV3(cfg::MigrationContext&) { return {}; }

cfg::ConfigRuntime makeV2Runtime() {
  cfg::VersionCatalog catalog;
  orDie(catalog.registerVersion({1, v1Defaults}), "register v1");
  orDie(catalog.registerVersion({2, v2Defaults}), "register v2");
  cfg::MigrationRegistry registry;
  orDie(registry.registerMigration(1, 2, migrateV1ToV2),
        "register migration 1->2");
  return orDie(
      cfg::ConfigRuntime::create(std::move(catalog), std::move(registry)),
      "create v2 runtime");
}

cfg::ConfigRuntime makeV3Runtime() {
  cfg::VersionCatalog catalog;
  orDie(catalog.registerVersion({1, v1Defaults}), "register v1");
  orDie(catalog.registerVersion({2, v2Defaults}), "register v2");
  orDie(catalog.registerVersion({3, v3Defaults}), "register v3");
  cfg::MigrationRegistry registry;
  orDie(registry.registerMigration(1, 2, migrateV1ToV2),
        "register migration 1->2");
  orDie(registry.registerMigration(2, 3, migrateV2ToV3),
        "register migration 2->3");
  return orDie(
      cfg::ConfigRuntime::create(std::move(catalog), std::move(registry)),
      "create v3 runtime");
}

cfg::VersionedConfig loadFile(const fs::path& path,
                              cfg::JsonInterface& backend) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "cannot open " << path << '\n';
    std::exit(EXIT_FAILURE);
  }
  return orDie(backend.load(input), "load " + path.string());
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path data_dir = argc > 1 ? fs::path(argv[1]) : fs::path("data");
  const fs::path output_dir = argc > 2 ? fs::path(argv[2]) : fs::path("out");
  const fs::path canonical = output_dir / "device.json";
  const fs::path checkpoints = output_dir / "device.checkpoints";

  std::error_code error;
  fs::remove_all(checkpoints, error);
  fs::create_directories(output_dir, error);
  fs::copy_file(data_dir / "device_v1.json", canonical,
                fs::copy_options::overwrite_existing, error);
  if (error) {
    std::cerr << "cannot prepare example input: " << error.message() << '\n';
    return EXIT_FAILURE;
  }

  cfg::ConfigRuntime v3_runtime = makeV3Runtime();
  cfg::JsonInterface backend;
  cfg::CheckpointStore store =
      orDie(cfg::cpCreate({canonical, checkpoints, 2}), "cpCreate");

  // New firmware supports v3. Capture the original bytes before migration,
  // then commit the synchronized configuration as one final replacement.
  cfg::VersionedConfig config = loadFile(canonical, backend);
  const cfg::SyncState upgrade = v3_runtime.inspect(config, 3);
  if (upgrade.status != cfg::SyncStatus::UpgradeRequired) {
    std::cerr << "expected the shipped v1 config to require an upgrade\n";
    return EXIT_FAILURE;
  }
  orDie(cfg::cpCapture(store, config, backend), "cpCapture(v1)");
  orDie(v3_runtime.synchronize(config, 3), "synchronize to v3");
  orDie(cfg::cpCommit(store, config, backend), "cpCommit(v3)");
  orDie(cfg::cpPrune(store, backend), "cpPrune");

  // Firmware is now downgraded to a build supporting v2. ConfigRuntime
  // correctly refuses to reverse the v3 file; the checkpoint component finds
  // v1 and asks ConfigRuntime to migrate that older state forward to v2.
  cfg::ConfigRuntime v2_runtime = makeV2Runtime();
  config = loadFile(canonical, backend);
  const cfg::SyncStatus downgrade =
      orDie(v2_runtime.synchronize(config, 2), "inspect v3 from v2 firmware");
  if (downgrade != cfg::SyncStatus::DowngradeRequired) {
    std::cerr << "expected DowngradeRequired\n";
    return EXIT_FAILURE;
  }

  cfg::RestoreSearch search =
      orDie(cfg::cpPrepareRestore(store, backend, v2_runtime, 2),
            "cpPrepareRestore(v2)");
  if (!search.candidate) {
    std::cerr << "no compatible checkpoint found\n";
    return EXIT_FAILURE;
  }

  cfg::PreparedRestore& restore = *search.candidate;
  if (restore.source_version != 1 || restore.config.version != 2 ||
      restore.config.model.get<std::string>("device.name").value() !=
          "line-sensor-7" ||
      restore.config.model.get<std::int64_t>("sampling.period_ms").value() !=
          250 ||
      restore.config.model.contains("upload.batch_size")) {
    std::cerr << "restored configuration did not preserve the expected state\n";
    return EXIT_FAILURE;
  }

  // This is where an application can perform schema validation. Only after it
  // accepts the prepared model does it replace the newer canonical file.
  orDie(cfg::cpCommit(store, restore.config, backend), "cpCommit(restored v2)");
  config = loadFile(canonical, backend);
  if (config.version != 2) {
    std::cerr << "expected the canonical config to be v2 after rollback\n";
    return EXIT_FAILURE;
  }

  std::cout << "restored checkpoint v" << restore.source_version
            << " and migrated it forward to firmware config v2\n";
  return EXIT_SUCCESS;
}

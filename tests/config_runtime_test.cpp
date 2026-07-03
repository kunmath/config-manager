#include "configmanager/config_runtime.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace configmanager {
namespace {

DefaultFactory objectFactory() {
  return [] { return ConfigValue::object(); };
}

MigrationFn noopMigration() {
  return [](MigrationContext&) -> Result<void> { return {}; };
}

// v1 defaults: {retries: 3, network: {host: "localhost", ports: [80, 443]}}
ConfigValue v1Defaults() {
  ConfigValue ports = ConfigValue::array();
  ports.push(ConfigValue::of(80)).push(ConfigValue::of(443));
  ConfigValue network = ConfigValue::object();
  network.set("host", ConfigValue::of(std::string("localhost")));
  network.set("ports", std::move(ports));
  ConfigValue root = ConfigValue::object();
  root.set("retries", ConfigValue::of(3));
  root.set("network", std::move(network));
  return root;
}

// v2 defaults: v1 plus network.timeout.
ConfigValue v2Defaults() {
  ConfigValue root = v1Defaults();
  ConfigValue network = ConfigValue::object();
  network.set("host", ConfigValue::of(std::string("localhost")));
  network.set("ports", ConfigValue::array()
                           .push(ConfigValue::of(80))
                           .push(ConfigValue::of(443)));
  network.set("timeout", ConfigValue::of(30));
  root.set("network", std::move(network));
  return root;
}

Result<ConfigRuntime> makeRuntime() {
  VersionCatalog catalog;
  EXPECT_TRUE(catalog.registerVersion({1, [] { return v1Defaults(); }}));
  EXPECT_TRUE(catalog.registerVersion({2, [] { return v2Defaults(); }}));
  MigrationRegistry registry;
  EXPECT_TRUE(registry.registerMigration(
      1, 2, [](MigrationContext& ctx) -> Result<void> {
        return ctx.model().set("migrated", true);
      }));
  return ConfigRuntime::create(std::move(catalog), std::move(registry));
}

// ---- create() -----------------------------------------------------------------

TEST(ConfigRuntimeTest, EmptyCatalogRejectedByCreate) {
  auto runtime = ConfigRuntime::create(VersionCatalog{}, MigrationRegistry{});
  ASSERT_FALSE(runtime);
  EXPECT_EQ(runtime.error().code, ErrorCode::InvalidVersion);
}

TEST(ConfigRuntimeTest, InvalidRegistryRejectedByCreate) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({2, objectFactory()}));
  MigrationRegistry registry;  // missing 1->2
  auto runtime = ConfigRuntime::create(std::move(catalog), std::move(registry));
  ASSERT_FALSE(runtime);
  EXPECT_EQ(runtime.error().code, ErrorCode::MissingMigration);
}

// ---- inspect() ------------------------------------------------------------------

TEST(ConfigRuntimeTest, InspectReportsEachSyncStatus) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);

  VersionedConfig config{2, ConfigModel()};
  EXPECT_EQ(runtime->inspect(config, 2).status, SyncStatus::InSync);
  config.version = 1;
  EXPECT_EQ(runtime->inspect(config, 2).status, SyncStatus::UpgradeRequired);
  config.version = 9;  // not registered: inspect does not care
  EXPECT_EQ(runtime->inspect(config, 2).status, SyncStatus::DowngradeRequired);

  const SyncState state = runtime->inspect(config, 2);
  EXPECT_EQ(state.currentVersion, 9u);
  EXPECT_EQ(state.targetVersion, 2u);
}

// ---- synchronize(): validation and downgrade -------------------------------------

TEST(ConfigRuntimeTest, UnregisteredSupportedVersionIsInvalidVersion) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{1, ConfigModel()};
  auto status = runtime->synchronize(config, 7);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, ErrorCode::InvalidVersion);
}

TEST(ConfigRuntimeTest, NewerUnregisteredVersionIsDowngradeRequired) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{9, ConfigModel()};  // written by a newer application
  ASSERT_TRUE(config.model.set("keep", true));
  auto status = runtime->synchronize(config, 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(*status, SyncStatus::DowngradeRequired);
  EXPECT_EQ(config.version, 9u);  // no modification
  EXPECT_TRUE(config.model.contains("keep"));
}

TEST(ConfigRuntimeTest, UnregisteredPersistedVersionOnUpgradeIsInvalid) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({2, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({3, objectFactory()}));
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(2, 3, noopMigration()));
  auto runtime = ConfigRuntime::create(std::move(catalog), std::move(registry));
  ASSERT_TRUE(runtime);

  VersionedConfig config{1, ConfigModel()};  // 1 was never registered
  auto status = runtime->synchronize(config, 3);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, ErrorCode::InvalidVersion);
  EXPECT_EQ(config.version, 1u);  // failed before any work
}

// ---- synchronize(): upgrade and transactionality ----------------------------------

TEST(ConfigRuntimeTest, UpgradeMigratesRepairsAndCommits) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{1, ConfigModel()};
  ASSERT_TRUE(config.model.set("retries", 5));  // user override

  auto status = runtime->synchronize(config, 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(*status, SyncStatus::InSync);
  EXPECT_EQ(config.version, 2u);
  EXPECT_EQ(config.model.get<bool>("migrated").value(), true);
  // Repair filled v2 defaults around the override without touching it.
  EXPECT_EQ(config.model.get<std::int64_t>("retries").value(), 5);
  EXPECT_EQ(config.model.get<std::int64_t>("network.timeout").value(), 30);
}

TEST(ConfigRuntimeTest, DefaultTargetIsLatestVersion) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{1, ConfigModel()};
  auto status = runtime->synchronize(config);
  ASSERT_TRUE(status);
  EXPECT_EQ(config.version, 2u);
}

TEST(ConfigRuntimeTest, FailedMigrationLeavesOriginalUntouched) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({2, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({3, objectFactory()}));
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(
      1, 2, [](MigrationContext& ctx) -> Result<void> {
        return ctx.model().set("mutatedByStepOne", true);
      }));
  ASSERT_TRUE(registry.registerMigration(
      2, 3, [](MigrationContext&) -> Result<void> {
        return fail(ErrorCode::MigrationFailed, "mid-chain failure");
      }));
  auto runtime = ConfigRuntime::create(std::move(catalog), std::move(registry));
  ASSERT_TRUE(runtime);

  VersionedConfig config{1, ConfigModel()};
  ASSERT_TRUE(config.model.set("original", true));
  auto status = runtime->synchronize(config, 3);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, ErrorCode::MigrationFailed);
  // Transactional rollback: version and model both untouched, even though
  // the first migration step succeeded on the working copy.
  EXPECT_EQ(config.version, 1u);
  EXPECT_TRUE(config.model.contains("original"));
  EXPECT_FALSE(config.model.contains("mutatedByStepOne"));
}

// ---- synchronize(): repair ---------------------------------------------------------

TEST(ConfigRuntimeTest, RepairRunsOnInSyncDrift) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{2, ConfigModel()};  // right version, empty model

  auto status = runtime->synchronize(config, 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(*status, SyncStatus::InSync);
  EXPECT_EQ(config.model.get<std::int64_t>("retries").value(), 3);
  EXPECT_EQ(config.model.get<std::string>("network.host").value(),
            "localhost");
  EXPECT_EQ(config.model.get<std::int64_t>("network.timeout").value(), 30);
}

TEST(ConfigRuntimeTest, CommitSkippedWhenNothingChanged) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  auto config = runtime->createDefault(2);
  ASSERT_TRUE(config);

  // Handles into cfg survive only if the commit (a move-assignment onto the
  // model) is skipped, so a still-valid handle proves cfg was never touched.
  auto handle = config->model.nodeAt("network");
  ASSERT_TRUE(handle);
  auto status = runtime->synchronize(*config, 2);
  ASSERT_TRUE(status);
  EXPECT_EQ(*status, SyncStatus::InSync);
  EXPECT_TRUE(handle->valid());
}

TEST(ConfigRuntimeTest, RepairNeverOverwritesPresentValues) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{2, ConfigModel()};
  ASSERT_TRUE(config.model.set("retries", 99));
  ASSERT_TRUE(config.model.set("network.host", std::string("example.com")));

  ASSERT_TRUE(runtime->synchronize(config, 2));
  EXPECT_EQ(config.model.get<std::int64_t>("retries").value(), 99);
  EXPECT_EQ(config.model.get<std::string>("network.host").value(),
            "example.com");
  // Missing sibling keys were still filled in.
  EXPECT_EQ(config.model.get<std::int64_t>("network.timeout").value(), 30);
}

TEST(ConfigRuntimeTest, RepairArraysAreAtomic) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{2, ConfigModel()};
  // An existing array is never merged, extended, or truncated.
  ASSERT_TRUE(config.model.set("network.ports[0]", 8080));

  ASSERT_TRUE(runtime->synchronize(config, 2));
  auto ports = config.model.nodeAt("network.ports");
  ASSERT_TRUE(ports);
  EXPECT_EQ(ports->size(), 1u);
  EXPECT_EQ(config.model.get<std::int64_t>("network.ports[0]").value(), 8080);
}

TEST(ConfigRuntimeTest, RepairPresenceWinsOverShape) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  VersionedConfig config{2, ConfigModel()};
  // "network" exists with a different type than the default Object: it is
  // left untouched and its default children are not backfilled.
  ASSERT_TRUE(config.model.set("network", std::string("disabled")));

  ASSERT_TRUE(runtime->synchronize(config, 2));
  EXPECT_EQ(config.model.get<std::string>("network").value(), "disabled");
  EXPECT_FALSE(config.model.contains("network.timeout"));
}

// ---- createDefault --------------------------------------------------------------

TEST(ConfigRuntimeTest, CreateDefaultBuildsVersionedConfig) {
  auto runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  auto config = runtime->createDefault(1);
  ASSERT_TRUE(config);
  EXPECT_EQ(config->version, 1u);
  EXPECT_EQ(config->model.get<std::int64_t>("retries").value(), 3);

  auto unknown = runtime->createDefault(9);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().code, ErrorCode::InvalidVersion);
}

}  // namespace
}  // namespace configmanager

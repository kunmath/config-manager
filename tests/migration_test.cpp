#include "configmanager/migration_engine.hpp"
#include "configmanager/migration_registry.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "configmanager/version_catalog.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {
namespace {

DefaultFactory objectFactory() {
  return [] { return ConfigValue::object(); };
}

VersionCatalog makeCatalog(const std::vector<VersionId>& versions) {
  VersionCatalog catalog;
  for (VersionId v : versions) {
    EXPECT_TRUE(catalog.registerVersion({v, objectFactory()}));
  }
  return catalog;
}

MigrationFn noopMigration() {
  return [](MigrationContext&) -> Result<void> { return {}; };
}

// ---- Registration -----------------------------------------------------------

TEST(MigrationRegistryTest, EmptyCallableRejectedAtRegistration) {
  MigrationRegistry registry;
  auto rejected = registry.registerMigration(1, 2, MigrationFn{});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ErrorCode::MigrationFailed);
}

TEST(MigrationRegistryTest, DuplicateEdgeRejectedAtRegistration) {
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));
  auto duplicate = registry.registerMigration(1, 2, noopMigration());
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidVersion);
}

TEST(MigrationRegistryTest, FindMigrationAbsentEdgeIsMissingMigration) {
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));
  ASSERT_TRUE(registry.findMigration(1, 2));
  auto absent = registry.findMigration(2, 3);
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().code, ErrorCode::MissingMigration);
}

// ---- validate() rules ---------------------------------------------------------

TEST(MigrationRegistryTest, ValidateAcceptsCompleteAdjacentChain) {
  VersionCatalog catalog = makeCatalog({1, 2, 4});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));
  ASSERT_TRUE(registry.registerMigration(2, 4, noopMigration()));
  EXPECT_TRUE(registry.validate(catalog));
}

TEST(MigrationRegistryTest, ValidateMissingEdgeIsMissingMigration) {
  VersionCatalog catalog = makeCatalog({1, 2, 3});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));
  auto validated = registry.validate(catalog);
  ASSERT_FALSE(validated);
  EXPECT_EQ(validated.error().code, ErrorCode::MissingMigration);
}

TEST(MigrationRegistryTest, ValidateEdgeSkippingRegisteredVersionIsInvalid) {
  VersionCatalog catalog = makeCatalog({1, 2, 3});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 3, noopMigration()));  // skips 2
  auto validated = registry.validate(catalog);
  ASSERT_FALSE(validated);
  EXPECT_EQ(validated.error().code, ErrorCode::InvalidVersion);
}

TEST(MigrationRegistryTest, ValidateUnknownEndpointIsInvalidVersion) {
  VersionCatalog catalog = makeCatalog({1, 2});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(2, 9, noopMigration()));
  auto validated = registry.validate(catalog);
  ASSERT_FALSE(validated);
  EXPECT_EQ(validated.error().code, ErrorCode::InvalidVersion);
}

TEST(MigrationRegistryTest, ValidateEmptyRegistryAgainstSingleVersionCatalog) {
  VersionCatalog catalog = makeCatalog({1});
  MigrationRegistry registry;
  EXPECT_TRUE(registry.validate(catalog));  // no adjacency to cover
}

// ---- Engine: forward walk -------------------------------------------------------

TEST(MigrationEngineTest, MultiStepChainWithSparseVersionIds) {
  VersionCatalog catalog = makeCatalog({1, 2, 4});
  MigrationRegistry registry;
  std::vector<std::pair<VersionId, VersionId>> steps;
  auto recordingMigration = [&steps](MigrationContext& ctx) -> Result<void> {
    steps.emplace_back(ctx.fromVersion(), ctx.toVersion());
    return ctx.model().set("upgradedTo", static_cast<std::int64_t>(
                                             ctx.toVersion()));
  };
  ASSERT_TRUE(registry.registerMigration(1, 2, recordingMigration));
  ASSERT_TRUE(registry.registerMigration(2, 4, recordingMigration));

  VersionedConfig config{1, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  ASSERT_TRUE(engine.migrate(config, 4));

  EXPECT_EQ(config.version, 4u);
  const std::vector<std::pair<VersionId, VersionId>> expected{{1, 2}, {2, 4}};
  EXPECT_EQ(steps, expected);
  EXPECT_EQ(config.model.get<std::int64_t>("upgradedTo").value(), 4);
}

TEST(MigrationEngineTest, AlreadyAtTargetIsNoOp) {
  VersionCatalog catalog = makeCatalog({1, 2});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(
      1, 2, [](MigrationContext&) -> Result<void> {
        ADD_FAILURE() << "migration must not run for a no-op target";
        return {};
      }));

  VersionedConfig config{2, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  EXPECT_TRUE(engine.migrate(config, 2));
  EXPECT_EQ(config.version, 2u);
}

TEST(MigrationEngineTest, UnregisteredTargetIsInvalidVersion) {
  VersionCatalog catalog = makeCatalog({1, 2});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));

  VersionedConfig config{1, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  auto outcome = engine.migrate(config, 3);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ErrorCode::InvalidVersion);
  EXPECT_EQ(config.version, 1u);
}

TEST(MigrationEngineTest, BackwardTargetIsInvalidVersion) {
  VersionCatalog catalog = makeCatalog({1, 2});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));

  VersionedConfig config{2, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  auto outcome = engine.migrate(config, 1);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ErrorCode::InvalidVersion);
  EXPECT_EQ(config.version, 2u);
}

TEST(MigrationEngineTest, MissingEdgeSurfacesMissingMigration) {
  VersionCatalog catalog = makeCatalog({1, 2, 3});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(1, 2, noopMigration()));
  // 2->3 deliberately unregistered; the engine does not force validate().

  VersionedConfig config{1, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  auto outcome = engine.migrate(config, 3);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ErrorCode::MissingMigration);
  // Raw direct semantics: the version reflects the last completed step.
  EXPECT_EQ(config.version, 2u);
}

// ---- Engine: failure mapping ----------------------------------------------------

TEST(MigrationEngineTest, ThrowingMigrationIsMigrationFailed) {
  VersionCatalog catalog = makeCatalog({1, 2});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(
      1, 2, [](MigrationContext&) -> Result<void> {
        throw std::runtime_error("kaput");
      }));

  VersionedConfig config{1, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  auto outcome = engine.migrate(config, 2);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ErrorCode::MigrationFailed);
  EXPECT_NE(outcome.error().message.find("kaput"), std::string::npos);
  EXPECT_EQ(config.version, 1u);  // failed step never advanced the version
}

TEST(MigrationEngineTest, ReturnedErrorWrappedWithDiagnosticPreserved) {
  VersionCatalog catalog = makeCatalog({3, 4});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(
      3, 4, [](MigrationContext&) -> Result<void> {
        return fail(ErrorCode::InvalidType, "port must stay an Int");
      }));

  VersionedConfig config{3, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  auto outcome = engine.migrate(config, 4);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ErrorCode::MigrationFailed);
  EXPECT_NE(outcome.error().message.find("3->4"), std::string::npos);
  EXPECT_NE(outcome.error().message.find("InvalidType"), std::string::npos);
  EXPECT_NE(outcome.error().message.find("port must stay an Int"),
            std::string::npos);
}

TEST(MigrationEngineTest, MidChainFailureLeavesPartialMutationsInPlace) {
  VersionCatalog catalog = makeCatalog({1, 2, 3});
  MigrationRegistry registry;
  ASSERT_TRUE(registry.registerMigration(
      1, 2, [](MigrationContext& ctx) -> Result<void> {
        return ctx.model().set("stepOne", true);
      }));
  ASSERT_TRUE(registry.registerMigration(
      2, 3, [](MigrationContext& ctx) -> Result<void> {
        // Mutates in place before returning the error (raw semantics).
        auto written = ctx.model().set("partial", true);
        if (!written) {
          return written;
        }
        return fail(ErrorCode::MigrationFailed, "step failed after mutating");
      }));

  VersionedConfig config{1, ConfigModel()};
  MigrationEngine engine(catalog, registry);
  auto outcome = engine.migrate(config, 3);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(config.version, 2u);  // last successfully completed step
  EXPECT_TRUE(config.model.contains("stepOne"));
  EXPECT_TRUE(config.model.contains("partial"));  // suspect after failure
}

}  // namespace
}  // namespace configmanager

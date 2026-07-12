#include "configmanager/version_catalog.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace configmanager {
namespace {

ConfigValue emptyObject() { return ConfigValue::object(); }

DefaultFactory objectFactory() {
  return [] { return ConfigValue::object(); };
}

// ---- Registration -----------------------------------------------------------

TEST(VersionCatalogTest, RegisterAndQueryVersions) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({2, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({4, objectFactory()}));

  EXPECT_TRUE(catalog.contains(1));
  EXPECT_TRUE(catalog.contains(2));
  EXPECT_TRUE(catalog.contains(4));
  EXPECT_FALSE(catalog.contains(3));

  const std::vector<VersionId> expected{1, 2, 4};
  EXPECT_EQ(catalog.versions(), expected);
}

TEST(VersionCatalogTest, DuplicateVersionIsInvalidVersion) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  auto duplicate = catalog.registerVersion({1, objectFactory()});
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidVersion);
}

TEST(VersionCatalogTest, EmptyDefaultFactoryIsMigrationFailed) {
  VersionCatalog catalog;
  auto rejected = catalog.registerVersion({1, DefaultFactory{}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ErrorCode::MigrationFailed);
  EXPECT_FALSE(catalog.contains(1));
}

// ---- Ordering and adjacency ---------------------------------------------------

TEST(VersionCatalogTest, LatestVersionOnEmptyCatalogIsInvalidVersion) {
  VersionCatalog catalog;
  auto latest = catalog.latestVersion();
  ASSERT_FALSE(latest);
  EXPECT_EQ(latest.error().code, ErrorCode::InvalidVersion);
}

TEST(VersionCatalogTest, LatestVersionIsHighestRegistered) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({4, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  EXPECT_EQ(catalog.latestVersion().value(), 4u);
}

TEST(VersionCatalogTest, NextVersionFollowsCatalogOrderNotNumericIncrement) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({2, objectFactory()}));
  ASSERT_TRUE(catalog.registerVersion({4, objectFactory()}));

  EXPECT_EQ(catalog.nextVersion(1).value(), 2u);
  EXPECT_EQ(catalog.nextVersion(2).value(), 4u);  // v2 and v4 are adjacent
}

TEST(VersionCatalogTest, NextVersionOfUnknownVersionIsInvalidVersion) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  auto next = catalog.nextVersion(3);
  ASSERT_FALSE(next);
  EXPECT_EQ(next.error().code, ErrorCode::InvalidVersion);
}

TEST(VersionCatalogTest, NextVersionOfLatestIsInvalidVersion) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  auto next = catalog.nextVersion(1);
  ASSERT_FALSE(next);
  EXPECT_EQ(next.error().code, ErrorCode::InvalidVersion);
}

// ---- createDefault ------------------------------------------------------------

TEST(VersionCatalogTest, CreateDefaultAdoptsFactoryTree) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, [] {
                                         ConfigValue root = emptyObject();
                                         root.set("timeout",
                                                  ConfigValue::of(30));
                                         return root;
                                       }}));
  auto model = catalog.createDefault(1);
  ASSERT_TRUE(model);
  EXPECT_EQ(model->get<std::int64_t>("timeout").value(), 30);
}

// ConfigValue::of throws on its data-dependent preconditions (out-of-range
// unsigned, null C string); inside a factory that maps to MigrationFailed
// like any other callback exception (ADR-018), never a corrupted default.
TEST(VersionCatalogTest, FactoryViolatingOfPreconditionIsMigrationFailed) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion(
      {1, [] {
         ConfigValue root = ConfigValue::object();
         root.set("u",
                  ConfigValue::of(std::numeric_limits<std::uint64_t>::max()));
         return root;
       }}));
  auto model = catalog.createDefault(1);
  ASSERT_FALSE(model);
  EXPECT_EQ(model.error().code, ErrorCode::MigrationFailed);
}

TEST(VersionCatalogTest, CreateDefaultForUnknownVersionIsInvalidVersion) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, objectFactory()}));
  auto model = catalog.createDefault(2);
  ASSERT_FALSE(model);
  EXPECT_EQ(model.error().code, ErrorCode::InvalidVersion);
}

TEST(VersionCatalogTest, ThrowingFactoryIsMigrationFailed) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion(
      {1, []() -> ConfigValue { throw std::runtime_error("boom"); }}));
  auto model = catalog.createDefault(1);
  ASSERT_FALSE(model);
  EXPECT_EQ(model.error().code, ErrorCode::MigrationFailed);
  EXPECT_NE(model.error().message.find("boom"), std::string::npos);
}

TEST(VersionCatalogTest, NonObjectFactoryRootIsInvalidType) {
  VersionCatalog catalog;
  ASSERT_TRUE(catalog.registerVersion({1, [] { return ConfigValue::of(7); }}));
  auto model = catalog.createDefault(1);
  ASSERT_FALSE(model);
  EXPECT_EQ(model.error().code, ErrorCode::InvalidType);
}

}  // namespace
}  // namespace configmanager

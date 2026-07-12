#include "configmanager/config_value.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace configmanager {
namespace {

TEST(ConfigValueTest, DefaultConstructedIsNull) {
  ConfigValue value;
  EXPECT_EQ(value.type(), NodeType::Null);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(value.scalar()));
}

TEST(ConfigValueTest, FactoriesProduceContainers) {
  EXPECT_EQ(ConfigValue::object().type(), NodeType::Object);
  EXPECT_EQ(ConfigValue::array().type(), NodeType::Array);
}

TEST(ConfigValueTest, OfBool) {
  auto value = ConfigValue::of(true);
  EXPECT_EQ(value.type(), NodeType::Bool);
  EXPECT_EQ(std::get<bool>(value.scalar()), true);
}

TEST(ConfigValueTest, OfIntegralTypesStoreInt64) {
  auto fromInt = ConfigValue::of(42);
  EXPECT_EQ(fromInt.type(), NodeType::Int);
  EXPECT_EQ(std::get<std::int64_t>(fromInt.scalar()), 42);

  auto fromLong = ConfigValue::of(std::int64_t{-7});
  EXPECT_EQ(fromLong.type(), NodeType::Int);
  EXPECT_EQ(std::get<std::int64_t>(fromLong.scalar()), -7);
}

TEST(ConfigValueTest, OfFloatingPointStoresDouble) {
  auto value = ConfigValue::of(2.5);
  EXPECT_EQ(value.type(), NodeType::Double);
  EXPECT_EQ(std::get<double>(value.scalar()), 2.5);
}

TEST(ConfigValueTest, OfString) {
  auto value = ConfigValue::of(std::string("hello"));
  EXPECT_EQ(value.type(), NodeType::String);
  EXPECT_EQ(std::get<std::string>(value.scalar()), "hello");
}

TEST(ConfigValueTest, OfStringViewAndCString) {
  auto fromView = ConfigValue::of(std::string_view("view"));
  EXPECT_EQ(fromView.type(), NodeType::String);
  EXPECT_EQ(std::get<std::string>(fromView.scalar()), "view");

  auto fromLiteral = ConfigValue::of("literal");
  EXPECT_EQ(fromLiteral.type(), NodeType::String);
  EXPECT_EQ(std::get<std::string>(fromLiteral.scalar()), "literal");
}

TEST(ConfigValueTest, OfUnsignedBeyondInt64ThrowsOutOfRange) {
  EXPECT_THROW(ConfigValue::of(std::numeric_limits<std::uint64_t>::max()),
               std::out_of_range);
  // The largest storable unsigned value is fine.
  auto max = ConfigValue::of(static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max()));
  EXPECT_EQ(std::get<std::int64_t>(max.scalar()),
            std::numeric_limits<std::int64_t>::max());
}

TEST(ConfigValueTest, OfNullCStringThrowsInvalidArgument) {
  const char* null = nullptr;
  EXPECT_THROW(ConfigValue::of(null), std::invalid_argument);
}

TEST(ConfigValueTest, ObjectMembersKeepInsertionOrder) {
  auto value = ConfigValue::object();
  value.set("zebra", ConfigValue::of(1))
      .set("alpha", ConfigValue::of(2))
      .set("mid", ConfigValue::of(3));

  const auto& members = value.members();
  ASSERT_EQ(members.size(), 3u);
  EXPECT_EQ(members[0].first, "zebra");
  EXPECT_EQ(members[1].first, "alpha");
  EXPECT_EQ(members[2].first, "mid");
}

TEST(ConfigValueTest, SetExistingKeyReplacesInPlace) {
  auto value = ConfigValue::object();
  value.set("first", ConfigValue::of(1))
      .set("second", ConfigValue::of(2))
      .set("first", ConfigValue::of(10));

  const auto& members = value.members();
  ASSERT_EQ(members.size(), 2u);
  EXPECT_EQ(members[0].first, "first");
  EXPECT_EQ(std::get<std::int64_t>(members[0].second.scalar()), 10);
  EXPECT_EQ(members[1].first, "second");
}

TEST(ConfigValueTest, PushAppendsInOrder) {
  auto value = ConfigValue::array();
  value.push(ConfigValue::of(1))
      .push(ConfigValue::of(2))
      .push(ConfigValue::of(3));

  const auto& elements = value.elements();
  ASSERT_EQ(elements.size(), 3u);
  EXPECT_EQ(std::get<std::int64_t>(elements[0].scalar()), 1);
  EXPECT_EQ(std::get<std::int64_t>(elements[2].scalar()), 3);
}

TEST(ConfigValueTest, NestedTreeBuilds) {
  auto root = ConfigValue::object();
  auto network = ConfigValue::object();
  network.set("hostname", ConfigValue::of(std::string("localhost")))
      .set("port", ConfigValue::of(8080));
  root.set("network", std::move(network));

  const auto& members = root.members();
  ASSERT_EQ(members.size(), 1u);
  EXPECT_EQ(members[0].first, "network");
  EXPECT_EQ(members[0].second.type(), NodeType::Object);
  ASSERT_EQ(members[0].second.members().size(), 2u);
  EXPECT_EQ(members[0].second.members()[1].first, "port");
}

}  // namespace
}  // namespace configmanager

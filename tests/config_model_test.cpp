#include "configmanager/config_model.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "configmanager/versioned_config.hpp"

namespace configmanager {
namespace {

// ---- Upsert creation --------------------------------------------------------

TEST(ConfigModelTest, UpsertCreatesIntermediateObjects) {
  ConfigModel model;
  ASSERT_TRUE(model.set("network.timeout", 10));
  EXPECT_TRUE(model.contains("network"));
  auto value = model.get<std::int64_t>("network.timeout");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 10);
}

TEST(ConfigModelTest, UpsertCreatesIntermediateArrays) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a[0].b", 5));
  auto node = model.nodeAt("a");
  ASSERT_TRUE(node);
  EXPECT_EQ(node->type(), NodeType::Array);
  EXPECT_EQ(node->size(), 1u);
  auto value = model.get<std::int64_t>("a[0].b");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 5);
}

TEST(ConfigModelTest, TypedRoundTripForEachScalarType) {
  ConfigModel model;
  ASSERT_TRUE(model.set("b", true));
  ASSERT_TRUE(model.set("i", std::int64_t{-7}));
  ASSERT_TRUE(model.set("d", 2.5));
  ASSERT_TRUE(model.set("s", std::string("hello")));
  ASSERT_TRUE(model.set("n", ConfigValue()));

  EXPECT_EQ(model.get<bool>("b").value(), true);
  EXPECT_EQ(model.get<std::int64_t>("i").value(), -7);
  EXPECT_EQ(model.get<double>("d").value(), 2.5);
  EXPECT_EQ(model.get<std::string>("s").value(), "hello");
  EXPECT_TRUE(model.contains("n"));
  EXPECT_EQ(model.nodeAt("n")->type(), NodeType::Null);
}

// ---- Read error mapping -----------------------------------------------------

TEST(ConfigModelTest, GetAbsentPathIsNodeNotFound) {
  ConfigModel model;
  auto value = model.get<std::int64_t>("missing");
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().code, ErrorCode::NodeNotFound);
}

TEST(ConfigModelTest, GetMalformedPathIsInvalidPath) {
  ConfigModel model;
  auto value = model.get<std::int64_t>("a..b");
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().code, ErrorCode::InvalidPath);

  auto empty = model.get<std::int64_t>("");
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, ErrorCode::InvalidPath);
}

TEST(ConfigModelTest, TraversingThroughScalarIsInvalidType) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  auto value = model.get<std::int64_t>("a.b");
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().code, ErrorCode::InvalidType);
}

// ---- Strict, lossless-only scalar conversions ---------------------------------

TEST(ConfigModelTest, IntToDoubleOnlyWhenExactlyRepresentable) {
  ConfigModel model;
  ASSERT_TRUE(model.set("small", std::int64_t{42}));
  EXPECT_EQ(model.get<double>("small").value(), 42.0);

  // 2^62 + 1 needs 63 bits of mantissa: not representable as a double.
  ASSERT_TRUE(model.set("big", (std::int64_t{1} << 62) + 1));
  auto lossy = model.get<double>("big");
  ASSERT_FALSE(lossy);
  EXPECT_EQ(lossy.error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, DoubleToIntOnlyWhenIntegral) {
  ConfigModel model;
  ASSERT_TRUE(model.set("whole", 2.0));
  EXPECT_EQ(model.get<std::int64_t>("whole").value(), 2);

  ASSERT_TRUE(model.set("frac", 2.5));
  auto lossy = model.get<std::int64_t>("frac");
  ASSERT_FALSE(lossy);
  EXPECT_EQ(lossy.error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, BoolNeverConverts) {
  ConfigModel model;
  ASSERT_TRUE(model.set("b", true));
  ASSERT_TRUE(model.set("i", std::int64_t{1}));
  EXPECT_EQ(model.get<std::int64_t>("b").error().code, ErrorCode::InvalidType);
  EXPECT_EQ(model.get<bool>("i").error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, StringsNeverParseAndValuesNeverStringify) {
  ConfigModel model;
  ASSERT_TRUE(model.set("s", std::string("42")));
  ASSERT_TRUE(model.set("i", std::int64_t{42}));
  EXPECT_EQ(model.get<std::int64_t>("s").error().code, ErrorCode::InvalidType);
  EXPECT_EQ(model.get<std::string>("i").error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, NarrowIntegerReadsAreRangeChecked) {
  ConfigModel model;
  ASSERT_TRUE(model.set("fits", std::int64_t{300}));
  EXPECT_EQ(model.get<std::int16_t>("fits").value(), 300);

  ASSERT_TRUE(model.set("wide", std::int64_t{70000}));
  EXPECT_EQ(model.get<std::int16_t>("wide").error().code,
            ErrorCode::InvalidType);

  ASSERT_TRUE(model.set("negative", std::int64_t{-1}));
  EXPECT_EQ(model.get<std::uint32_t>("negative").error().code,
            ErrorCode::InvalidType);
}

TEST(ConfigModelTest, UnsignedWriteAboveInt64MaxFailsUnchanged) {
  ConfigModel model;
  auto result = model.set("u", std::numeric_limits<std::uint64_t>::max());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidType);
  EXPECT_FALSE(model.contains("u"));

  ASSERT_TRUE(model.set("ok", std::uint64_t{42}));
  EXPECT_EQ(model.get<std::uint64_t>("ok").value(), 42u);
}

TEST(ConfigModelTest, NullCStringWriteFailsUnchanged) {
  ConfigModel model;
  const char* null = nullptr;
  auto result = model.set("s", null);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidType);
  EXPECT_FALSE(model.contains("s"));
}

TEST(ConfigModelTest, DoubleToUnsignedAdmitsExactValuesAboveInt64Max) {
  ConfigModel model;
  ASSERT_TRUE(model.set("big", 9223372036854775808.0));  // 2^63, exact
  EXPECT_EQ(model.get<std::uint64_t>("big").value(), std::uint64_t{1} << 63);
  EXPECT_EQ(model.get<std::int64_t>("big").error().code,
            ErrorCode::InvalidType);

  ASSERT_TRUE(model.set("huge", 1e300));
  EXPECT_EQ(model.get<std::uint64_t>("huge").error().code,
            ErrorCode::InvalidType);
  EXPECT_EQ(model.get<std::int64_t>("huge").error().code,
            ErrorCode::InvalidType);
}

// ---- Member order (ADR-022) ---------------------------------------------------

TEST(ConfigModelTest, MemberOrderPreservedAcrossValueRoundTrip) {
  ConfigModel model;
  auto subtree = ConfigValue::object();
  subtree.set("zebra", ConfigValue::of(1))
      .set("alpha", ConfigValue::of(2))
      .set("mid", ConfigValue::of(3));
  ASSERT_TRUE(model.set("obj", std::move(subtree)));

  auto copied = model.getValue("obj");
  ASSERT_TRUE(copied);
  ASSERT_TRUE(model.set("copy", std::move(*copied)));

  auto keys = model.nodeAt("copy")->keys();
  ASSERT_TRUE(keys);
  ASSERT_EQ(keys->size(), 3u);
  EXPECT_EQ((*keys)[0], "zebra");
  EXPECT_EQ((*keys)[1], "alpha");
  EXPECT_EQ((*keys)[2], "mid");
}

// ---- fromValue (ADR-020, ADR-021) ---------------------------------------------

TEST(ConfigModelTest, FromValueRejectsNonObjectRoot) {
  auto fromArray = ConfigModel::fromValue(ConfigValue::array());
  ASSERT_FALSE(fromArray);
  EXPECT_EQ(fromArray.error().code, ErrorCode::InvalidType);

  auto fromScalar = ConfigModel::fromValue(ConfigValue::of(1));
  ASSERT_FALSE(fromScalar);
  EXPECT_EQ(fromScalar.error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, FromValueRejectsNonAddressableKeys) {
  auto withReserved = ConfigValue::object();
  auto nested = ConfigValue::object();
  nested.set("bad.key", ConfigValue::of(1));
  withReserved.set("ok", std::move(nested));
  auto model = ConfigModel::fromValue(std::move(withReserved));
  ASSERT_FALSE(model);
  EXPECT_EQ(model.error().code, ErrorCode::InvalidPath);

  auto withEmpty = ConfigValue::object();
  withEmpty.set("", ConfigValue::of(1));
  auto emptyKey = ConfigModel::fromValue(std::move(withEmpty));
  ASSERT_FALSE(emptyKey);
  EXPECT_EQ(emptyKey.error().code, ErrorCode::InvalidPath);
}

TEST(ConfigModelTest, FromValueAdoptsTree) {
  auto root = ConfigValue::object();
  auto network = ConfigValue::object();
  network.set("port", ConfigValue::of(8080));
  root.set("network", std::move(network));

  auto model = ConfigModel::fromValue(std::move(root));
  ASSERT_TRUE(model);
  EXPECT_EQ(model->get<std::int64_t>("network.port").value(), 8080);
}

TEST(ConfigModelTest, SubtreeSetRejectsNonAddressableKeysUnchanged) {
  ConfigModel model;
  auto subtree = ConfigValue::object();
  subtree.set("bad[0]", ConfigValue::of(1));
  auto result = model.set("target", std::move(subtree));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidPath);
  EXPECT_FALSE(model.contains("target"));
}

TEST(ConfigModelTest, SubtreeKeyRejectionLeavesExistingTargetUnchanged) {
  ConfigModel model;
  auto good = ConfigValue::object();
  good.set("keep", ConfigValue::of(1));
  ASSERT_TRUE(model.set("t", std::move(good)));

  auto bad = ConfigValue::object();
  bad.set("bad.key", ConfigValue::of(2));
  auto result = model.set("t", std::move(bad));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidPath);
  EXPECT_EQ(model.get<std::int64_t>("t.keep").value(), 1);
}

// ---- getValue -----------------------------------------------------------------

TEST(ConfigModelTest, GetValueIsDeepCopy) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b", 1));
  auto copy = model.getValue("a");
  ASSERT_TRUE(copy);

  ASSERT_TRUE(model.set("a.b", 2));
  ASSERT_EQ(copy->members().size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(copy->members()[0].second.scalar()), 1);
}

// ---- remove -------------------------------------------------------------------

TEST(ConfigModelTest, RemoveDetachesSubtree) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b.c", 1));
  ASSERT_TRUE(model.remove("a.b"));
  EXPECT_FALSE(model.contains("a.b"));
  EXPECT_TRUE(model.contains("a"));
}

TEST(ConfigModelTest, RemoveAbsentIsNodeNotFoundMalformedIsInvalidPath) {
  ConfigModel model;
  auto absent = model.remove("missing");
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().code, ErrorCode::NodeNotFound);

  auto malformed = model.remove("a..b");
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().code, ErrorCode::InvalidPath);
}

TEST(ConfigModelTest, RemoveArrayElementShiftsSuccessors) {
  ConfigModel model;
  ASSERT_TRUE(model.set("arr[0]", 1));
  ASSERT_TRUE(model.set("arr[1]", 2));
  ASSERT_TRUE(model.set("arr[2]", 3));
  ASSERT_TRUE(model.remove("arr[1]"));
  EXPECT_EQ(model.nodeAt("arr")->size(), 2u);
  EXPECT_EQ(model.get<std::int64_t>("arr[1]").value(), 3);
}

// ---- Upsert edge cases (ADR-019) ----------------------------------------------

TEST(ConfigModelTest, CrossTypeWriteToFinalNodeFailsUnchanged) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", std::int64_t{1}));
  auto result = model.set("a", std::string("str"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidType);
  EXPECT_EQ(model.get<std::int64_t>("a").value(), 1);
}

TEST(ConfigModelTest, CrossTypeWriteOverContainerFailsUnchanged) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b", 1));
  auto result = model.set("a", 5);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidType);
  EXPECT_EQ(model.get<std::int64_t>("a.b").value(), 1);
}

TEST(ConfigModelTest, NullFollowsTheSameTypeRules) {
  ConfigModel model;
  ASSERT_TRUE(model.set("n", ConfigValue()));
  ASSERT_TRUE(model.set("n", ConfigValue()));  // Null over Null: same type
  auto crossType = model.set("n", 1);
  ASSERT_FALSE(crossType);
  EXPECT_EQ(crossType.error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, SameTypeSubtreeWriteReplacesContents) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b", 1));
  auto replacement = ConfigValue::object();
  replacement.set("c", ConfigValue::of(2));
  ASSERT_TRUE(model.set("a", std::move(replacement)));
  EXPECT_FALSE(model.contains("a.b"));
  EXPECT_EQ(model.get<std::int64_t>("a.c").value(), 2);
}

TEST(ConfigModelTest, FailedWriteLeavesNoPartialIntermediates) {
  ConfigModel model;
  // users is absent and the index is not [0]: nothing may be created.
  auto holey = model.set("users[2].name", std::string("x"));
  ASSERT_FALSE(holey);
  EXPECT_EQ(holey.error().code, ErrorCode::NodeNotFound);
  EXPECT_FALSE(model.contains("users"));

  auto fresh = model.set("x.y[1]", 1);
  ASSERT_FALSE(fresh);
  EXPECT_EQ(fresh.error().code, ErrorCode::NodeNotFound);
  EXPECT_FALSE(model.contains("x"));

  ASSERT_TRUE(model.set("a", 1));
  auto conflict = model.set("a.b.c", 1);
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code, ErrorCode::InvalidType);
  EXPECT_FALSE(model.contains("a.b"));
}

TEST(ConfigModelTest, ArrayWritesAppendOnlyAtOnePastTheEnd) {
  ConfigModel model;
  ASSERT_TRUE(model.set("arr[0]", 1));
  ASSERT_TRUE(model.set("arr[1]", 2));  // append at one past the end
  auto hole = model.set("arr[3]", 4);
  ASSERT_FALSE(hole);
  EXPECT_EQ(hole.error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(model.nodeAt("arr")->size(), 2u);
}

TEST(ConfigModelTest, ExistingArrayElementFollowsTypeRules) {
  ConfigModel model;
  ASSERT_TRUE(model.set("arr[0]", 1));
  ASSERT_TRUE(model.set("arr[0]", 9));  // same-type update
  EXPECT_EQ(model.get<std::int64_t>("arr[0]").value(), 9);

  auto crossType = model.set("arr[0]", std::string("s"));
  ASSERT_FALSE(crossType);
  EXPECT_EQ(crossType.error().code, ErrorCode::InvalidType);
}

TEST(ConfigModelTest, ContainsIsTotalOverMalformedPaths) {
  ConfigModel model;
  EXPECT_FALSE(model.contains(""));
  EXPECT_FALSE(model.contains("a..b"));
  EXPECT_FALSE(model.contains("arr[-1]"));
  EXPECT_FALSE(model.contains("missing"));
}

// ---- clone --------------------------------------------------------------------

TEST(ConfigModelTest, CloneIsIndependentDeepCopy) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b", 1));
  ConfigModel copy = model.clone();

  ASSERT_TRUE(model.set("a.b", 2));
  EXPECT_EQ(copy.get<std::int64_t>("a.b").value(), 1);
  ASSERT_TRUE(copy.set("c", 3));
  EXPECT_FALSE(model.contains("c"));
}

// ---- Maximum nesting depth (HLD §4.4) --------------------------------------------

// A value tree whose deepest node sits `wraps` levels below its root.
ConfigValue nestedValue(std::size_t wraps) {
  ConfigValue value = ConfigValue::of(1);
  for (std::size_t i = 0; i < wraps; ++i) {
    ConfigValue wrapper = ConfigValue::object();
    wrapper.set("a", std::move(value));
    value = std::move(wrapper);
  }
  return value;
}

TEST(ConfigModelTest, FromValueAcceptsExactlyMaxTreeDepth) {
  auto model = ConfigModel::fromValue(nestedValue(kMaxTreeDepth));
  ASSERT_TRUE(model) << model.error().message;
}

TEST(ConfigModelTest, FromValueBeyondMaxTreeDepthIsInvalidPath) {
  auto model = ConfigModel::fromValue(nestedValue(kMaxTreeDepth + 1));
  ASSERT_FALSE(model);
  EXPECT_EQ(model.error().code, ErrorCode::InvalidPath);
}

TEST(ConfigModelTest, SetCountsPathDepthAgainstMaxTreeDepth) {
  ConfigModel model;
  // The subtree's root would sit at depth 1, pushing its deepest node one
  // past the limit.
  auto tooDeep = model.set("x", nestedValue(kMaxTreeDepth));
  ASSERT_FALSE(tooDeep);
  EXPECT_EQ(tooDeep.error().code, ErrorCode::InvalidPath);
  EXPECT_FALSE(model.contains("x"));  // failed writes stay atomic

  ASSERT_TRUE(model.set("x", nestedValue(kMaxTreeDepth - 1)));
}

TEST(ConfigModelTest, DeepPathWritesRespectMaxTreeDepth) {
  std::string path = "a";
  for (std::size_t i = 1; i < kMaxTreeDepth; ++i) {
    path += ".a";
  }
  // kMaxTreeDepth segments: the scalar lands exactly at the limit.
  ConfigModel model;
  ASSERT_TRUE(model.set(path, 1));

  ConfigModel deeper;
  auto tooDeep = deeper.set(path + ".a", 1);
  ASSERT_FALSE(tooDeep);
  EXPECT_EQ(tooDeep.error().code, ErrorCode::InvalidPath);
}

// ---- VersionedConfig (HLD §5) ---------------------------------------------------

TEST(VersionedConfigTest, MoveOnlyUnitCarriesVersionAndModel) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  VersionedConfig config{3, std::move(model)};
  EXPECT_EQ(config.version, 3u);

  VersionedConfig moved = std::move(config);
  EXPECT_EQ(moved.version, 3u);
  EXPECT_EQ(moved.model.get<std::int64_t>("a").value(), 1);
}

}  // namespace
}  // namespace configmanager

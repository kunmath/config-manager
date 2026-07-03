#include "configmanager/config_node.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "configmanager/config_model.hpp"

namespace configmanager {
namespace {

TEST(ConfigNodeTest, DefaultConstructedHandleIsInvalid) {
  ConfigNode node;
  EXPECT_FALSE(node.valid());
  EXPECT_EQ(node.as<std::int64_t>().error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(node.child("a").error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(node.at(0).error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(node.keys().error().code, ErrorCode::NodeNotFound);
}

TEST(ConfigNodeTest, RootIsAlwaysAnObject) {
  ConfigModel model;
  ConfigNode root = model.root();
  ASSERT_TRUE(root.valid());
  EXPECT_EQ(root.type(), NodeType::Object);
  EXPECT_EQ(root.size(), 0u);
}

TEST(ConfigNodeTest, HandleSurvivesModelMove) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  ConfigNode handle = *model.nodeAt("a");

  ConfigModel moved = std::move(model);
  ASSERT_TRUE(handle.valid());
  EXPECT_EQ(handle.as<std::int64_t>().value(), 1);
}

TEST(ConfigNodeTest, HandleDetectablyInvalidAfterRemove) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b", 1));
  ConfigNode handle = *model.nodeAt("a.b");
  ASSERT_TRUE(handle.valid());

  ASSERT_TRUE(model.remove("a.b"));
  EXPECT_FALSE(handle.valid());
  EXPECT_EQ(handle.as<std::int64_t>().error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(handle.child("x").error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(handle.at(0).error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(handle.keys().error().code, ErrorCode::NodeNotFound);
}

TEST(ConfigNodeTest, RecycledSlotDoesNotResurrectOldHandle) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  ConfigNode stale = *model.nodeAt("a");
  ASSERT_TRUE(model.remove("a"));

  // The freed slot is reused; the generation bump keeps the old handle stale.
  ASSERT_TRUE(model.set("b", 2));
  EXPECT_FALSE(stale.valid());
  EXPECT_EQ(stale.as<std::int64_t>().error().code, ErrorCode::NodeNotFound);
}

TEST(ConfigNodeTest, HandleValidAcrossUnrelatedMutations) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  ConfigNode handle = *model.nodeAt("a");
  ASSERT_TRUE(model.set("b.c", 2));
  ASSERT_TRUE(model.remove("b"));
  EXPECT_TRUE(handle.valid());
  EXPECT_EQ(handle.as<std::int64_t>().value(), 1);
}

TEST(ConfigNodeTest, SubtreeReplaceKeepsTargetInvalidatesDescendants) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a.b", 1));
  ConfigNode target = *model.nodeAt("a");
  ConfigNode descendant = *model.nodeAt("a.b");

  auto replacement = ConfigValue::object();
  replacement.set("c", ConfigValue::of(2));
  ASSERT_TRUE(model.set("a", std::move(replacement)));

  EXPECT_TRUE(target.valid());
  EXPECT_FALSE(descendant.valid());
  auto child = target.child("c");
  ASSERT_TRUE(child);
  EXPECT_EQ(child->as<std::int64_t>().value(), 2);
}

TEST(ConfigNodeTest, SameTypeScalarWriteKeepsHandleValid) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  ConfigNode handle = *model.nodeAt("a");
  ASSERT_TRUE(model.set("a", 2));
  ASSERT_TRUE(handle.valid());
  EXPECT_EQ(handle.as<std::int64_t>().value(), 2);
}

TEST(ConfigNodeTest, ChildAccessorContract) {
  ConfigModel model;
  ASSERT_TRUE(model.set("obj.x", 1));
  ASSERT_TRUE(model.set("arr[0]", 1));

  ConfigNode obj = *model.nodeAt("obj");
  EXPECT_EQ(obj.child("missing").error().code, ErrorCode::NodeNotFound);
  EXPECT_TRUE(obj.child("x"));

  ConfigNode arr = *model.nodeAt("arr");
  EXPECT_EQ(arr.child("x").error().code, ErrorCode::InvalidType);
}

TEST(ConfigNodeTest, AtAccessorContract) {
  ConfigModel model;
  ASSERT_TRUE(model.set("arr[0]", 1));
  ASSERT_TRUE(model.set("obj.x", 1));

  ConfigNode arr = *model.nodeAt("arr");
  EXPECT_EQ(arr.at(0)->as<std::int64_t>().value(), 1);
  EXPECT_EQ(arr.at(1).error().code, ErrorCode::NodeNotFound);

  ConfigNode obj = *model.nodeAt("obj");
  EXPECT_EQ(obj.at(0).error().code, ErrorCode::InvalidType);
}

TEST(ConfigNodeTest, KeysReturnsMemberNamesInOrder) {
  ConfigModel model;
  ASSERT_TRUE(model.set("obj.zebra", 1));
  ASSERT_TRUE(model.set("obj.alpha", 2));

  auto keys = model.nodeAt("obj")->keys();
  ASSERT_TRUE(keys);
  ASSERT_EQ(keys->size(), 2u);
  EXPECT_EQ((*keys)[0], "zebra");
  EXPECT_EQ((*keys)[1], "alpha");

  ConfigNode scalar = *model.nodeAt("obj.zebra");
  EXPECT_EQ(scalar.keys().error().code, ErrorCode::InvalidType);
}

TEST(ConfigNodeTest, SizeCountsChildrenAndIsZeroForScalars) {
  ConfigModel model;
  ASSERT_TRUE(model.set("obj.a", 1));
  ASSERT_TRUE(model.set("obj.b", 2));
  ASSERT_TRUE(model.set("arr[0]", 1));

  EXPECT_EQ(model.nodeAt("obj")->size(), 2u);
  EXPECT_EQ(model.nodeAt("arr")->size(), 1u);
  EXPECT_EQ(model.nodeAt("obj.a")->size(), 0u);
}

TEST(ConfigNodeTest, AsOnContainerIsInvalidType) {
  ConfigModel model;
  ASSERT_TRUE(model.set("obj.a", 1));
  ASSERT_TRUE(model.set("arr[0]", 1));
  EXPECT_EQ(model.nodeAt("obj")->as<std::int64_t>().error().code,
            ErrorCode::InvalidType);
  EXPECT_EQ(model.nodeAt("arr")->as<std::string>().error().code,
            ErrorCode::InvalidType);
}

TEST(ConfigNodeTest, HandlesWorkOnClonedModelIndependently) {
  ConfigModel model;
  ASSERT_TRUE(model.set("a", 1));
  ConfigModel copy = model.clone();

  ConfigNode original = *model.nodeAt("a");
  ConfigNode cloned = *copy.nodeAt("a");
  ASSERT_TRUE(model.remove("a"));

  EXPECT_FALSE(original.valid());
  ASSERT_TRUE(cloned.valid());
  EXPECT_EQ(cloned.as<std::int64_t>().value(), 1);
}

}  // namespace
}  // namespace configmanager

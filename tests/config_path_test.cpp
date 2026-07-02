#include "configmanager/config_path.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace configmanager {
namespace {

std::vector<PathSegment> parseOk(std::string_view text) {
  auto path = ConfigPath::parse(text);
  EXPECT_TRUE(path.has_value()) << "expected \"" << text << "\" to parse";
  return path.has_value() ? path->segments() : std::vector<PathSegment>{};
}

void expectInvalid(std::string_view text) {
  auto path = ConfigPath::parse(text);
  ASSERT_FALSE(path.has_value()) << "expected \"" << text << "\" to fail";
  EXPECT_EQ(path.error().code, ErrorCode::InvalidPath);
}

TEST(ConfigPathTest, SingleKey) {
  auto segments = parseOk("hostname");
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].kind, PathSegment::Kind::Key);
  EXPECT_EQ(segments[0].key, "hostname");
}

TEST(ConfigPathTest, ObjectTraversal) {
  auto segments = parseOk("network.hostname");
  ASSERT_EQ(segments.size(), 2u);
  EXPECT_EQ(segments[0].key, "network");
  EXPECT_EQ(segments[1].key, "hostname");
}

TEST(ConfigPathTest, ArrayTraversal) {
  auto segments = parseOk("users[0].name");
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0].kind, PathSegment::Kind::Key);
  EXPECT_EQ(segments[0].key, "users");
  EXPECT_EQ(segments[1].kind, PathSegment::Kind::Index);
  EXPECT_EQ(segments[1].index, 0u);
  EXPECT_EQ(segments[2].kind, PathSegment::Kind::Key);
  EXPECT_EQ(segments[2].key, "name");
}

TEST(ConfigPathTest, NestedArrays) {
  auto segments = parseOk("groups[0].users[4].name");
  ASSERT_EQ(segments.size(), 5u);
  EXPECT_EQ(segments[0].key, "groups");
  EXPECT_EQ(segments[1].index, 0u);
  EXPECT_EQ(segments[2].key, "users");
  EXPECT_EQ(segments[3].index, 4u);
  EXPECT_EQ(segments[4].key, "name");
}

TEST(ConfigPathTest, ConsecutiveIndices) {
  auto segments = parseOk("matrix[2][7]");
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0].key, "matrix");
  EXPECT_EQ(segments[1].index, 2u);
  EXPECT_EQ(segments[2].index, 7u);
}

TEST(ConfigPathTest, IndexAllowsLeadingZeros) {
  auto segments = parseOk("arr[007]");
  ASSERT_EQ(segments.size(), 2u);
  EXPECT_EQ(segments[1].index, 7u);
}

TEST(ConfigPathTest, WhitespaceIsOrdinaryKeyCharacter) {
  auto segments = parseOk(" spaced key ");
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].key, " spaced key ");
}

TEST(ConfigPathTest, EmptyPathIsInvalid) { expectInvalid(""); }

TEST(ConfigPathTest, EmptyKeysAreInvalid) {
  expectInvalid("a.");
  expectInvalid(".a");
  expectInvalid("a..b");
  expectInvalid(".");
}

TEST(ConfigPathTest, LeadingIndexIsInvalid) {
  expectInvalid("[0]");
  expectInvalid("[0].a");
}

TEST(ConfigPathTest, MalformedIndicesAreInvalid) {
  expectInvalid("arr[]");
  expectInvalid("arr[-1]");
  expectInvalid("arr[1x]");
  expectInvalid("arr[0x1]");
  expectInvalid("arr[1");
}

TEST(ConfigPathTest, TextAfterClosingBracketIsInvalid) {
  expectInvalid("a[0]b");
}

TEST(ConfigPathTest, StrayClosingBracketIsInvalid) {
  expectInvalid("a]b");
  expectInvalid("a]");
}

TEST(ConfigPathTest, IndexAfterDotIsInvalid) { expectInvalid("a.[0]"); }

TEST(ConfigPathTest, IndexOverflowIsInvalid) {
  expectInvalid("a[99999999999999999999999999999]");
}

TEST(ConfigPathTest, IndexFollowedByDotAndSegmentIsValid) {
  auto segments = parseOk("a[0].b[1]");
  ASSERT_EQ(segments.size(), 4u);
  EXPECT_EQ(segments[0].key, "a");
  EXPECT_EQ(segments[1].index, 0u);
  EXPECT_EQ(segments[2].key, "b");
  EXPECT_EQ(segments[3].index, 1u);
}

}  // namespace
}  // namespace configmanager

#include "configmanager/result.hpp"

#include <string>

#include <gtest/gtest.h>

namespace configmanager {
namespace {

TEST(ResultTest, SuccessCarriesValue) {
  Result<int> result = 42;
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(ResultTest, FailProducesErrorWithCodeAndMessage) {
  Result<int> result = fail(ErrorCode::NodeNotFound, "no such node");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::NodeNotFound);
  EXPECT_EQ(result.error().message, "no such node");
}

TEST(ResultTest, VoidResultSupportsBothChannels) {
  Result<void> ok{};
  EXPECT_TRUE(ok.has_value());

  Result<void> bad = fail(ErrorCode::InvalidVersion, "unknown version");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, ErrorCode::InvalidVersion);
}

}  // namespace
}  // namespace configmanager

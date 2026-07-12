#include "configmanager/backends/json_interface.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "configmanager/config_model.hpp"
#include "configmanager/config_value.hpp"

namespace configmanager {
namespace {

Result<VersionedConfig> loadText(const std::string& text) {
  std::istringstream in(text);
  JsonInterface backend;
  return backend.load(in);
}

VersionedConfig makeConfig(VersionId version, ConfigValue root) {
  Result<ConfigModel> model = ConfigModel::fromValue(std::move(root));
  return VersionedConfig{version, std::move(model).value()};
}

std::string saveToText(const VersionedConfig& config) {
  std::ostringstream out;
  JsonInterface backend;
  auto saved = backend.save(config, out);
  EXPECT_TRUE(saved) << (saved ? "" : saved.error().message);
  return out.str();
}

// ---- Round trip ---------------------------------------------------------------

TEST(JsonInterfaceTest, RoundTripPreservesValuesTypesAndOrder) {
  const std::string document = R"({
    "__version": 3,
    "zebra": "stripes",
    "flag": true,
    "nothing": null,
    "count": -42,
    "ratio": 2.5,
    "nested": {"b": 1, "a": [1, "two", false, {"deep": null}]},
    "empty_object": {},
    "empty_array": []
  })";

  auto first = loadText(document);
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_EQ(first->version, 3u);

  JsonInterface backend;
  std::ostringstream out;
  ASSERT_TRUE(backend.save(*first, out));
  auto second = loadText(out.str());
  ASSERT_TRUE(second) << second.error().message;

  EXPECT_EQ(second->version, 3u);
  EXPECT_EQ(second->model.get<std::string>("zebra").value(), "stripes");
  EXPECT_EQ(second->model.get<bool>("flag").value(), true);
  EXPECT_EQ(second->model.nodeAt("nothing")->type(), NodeType::Null);
  EXPECT_EQ(second->model.get<std::int64_t>("count").value(), -42);
  EXPECT_EQ(second->model.get<double>("ratio").value(), 2.5);
  EXPECT_EQ(second->model.get<std::int64_t>("nested.b").value(), 1);
  EXPECT_EQ(second->model.get<std::string>("nested.a[1]").value(), "two");
  EXPECT_EQ(second->model.nodeAt("nested.a[3].deep")->type(), NodeType::Null);

  // Empty containers keep their type through the round trip.
  EXPECT_EQ(second->model.nodeAt("empty_object")->type(), NodeType::Object);
  EXPECT_EQ(second->model.nodeAt("empty_object")->size(), 0u);
  EXPECT_EQ(second->model.nodeAt("empty_array")->type(), NodeType::Array);
  EXPECT_EQ(second->model.nodeAt("empty_array")->size(), 0u);

  // Member insertion order survives load -> save -> load (ADR-022).
  const std::vector<std::string> expectedOrder{
      "zebra", "flag", "nothing", "count", "ratio",
      "nested", "empty_object", "empty_array"};
  EXPECT_EQ(second->model.root().keys().value(), expectedOrder);
  EXPECT_EQ(second->model.nodeAt("nested")->keys().value(),
            (std::vector<std::string>{"b", "a"}));
}

TEST(JsonInterfaceTest, SaveWritesCarrierFirstFromVersionedConfig) {
  const std::string text =
      saveToText(makeConfig(9, ConfigValue::object().set(
                                   "key", ConfigValue::of(std::int64_t{1}))));
  const auto carrierPos = text.find("\"__version\": 9");
  const auto keyPos = text.find("\"key\"");
  ASSERT_NE(carrierPos, std::string::npos);
  ASSERT_NE(keyPos, std::string::npos);
  EXPECT_LT(carrierPos, keyPos);
}

TEST(JsonInterfaceTest, EmptyDocumentRoundTrip) {
  auto loaded = loadText(R"({"__version": 7})");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->version, 7u);
  EXPECT_EQ(loaded->model.root().size(), 0u);

  auto reloaded = loadText(saveToText(*loaded));
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->version, 7u);
  EXPECT_EQ(reloaded->model.root().size(), 0u);
}

// ---- Version carrier (ADR-014, ADR-020) ----------------------------------------

TEST(JsonInterfaceTest, LoadConsumesVersionCarrier) {
  auto loaded = loadText(R"({"a": 1, "__version": 5, "b": 2})");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->version, 5u);
  EXPECT_FALSE(loaded->model.contains("__version"));
  EXPECT_EQ(loaded->model.root().keys().value(),
            (std::vector<std::string>{"a", "b"}));
}

TEST(JsonInterfaceTest, NestedVersionKeyIsPlainData) {
  auto loaded = loadText(R"({"__version": 1, "a": {"__version": 2}})");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->version, 1u);
  EXPECT_EQ(loaded->model.get<std::int64_t>("a.__version").value(), 2);
}

TEST(JsonInterfaceTest, MissingVersionIsInvalidVersion) {
  auto loaded = loadText(R"({"a": 1})");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::InvalidVersion);
}

TEST(JsonInterfaceTest, MalformedCarrierIsInvalidVersion) {
  const std::vector<std::string> carriers{
      R"("1")",                     // quoted number
      R"(1.0)",                     // float
      R"(1e0)",                     // exponent form
      R"(-1)",                      // negative
      R"(4294967296)",              // 2^32, above VersionId
      R"(18446744073709551616)",    // 2^64, overflows every integer type
      R"(99999999999999999999)",    // integral overflow to float callback
      R"(true)",
      R"(null)",
      R"({})",
      R"([1])",
  };
  for (const std::string& carrier : carriers) {
    auto loaded = loadText(R"({"__version": )" + carrier + "}");
    ASSERT_FALSE(loaded) << "carrier accepted: " << carrier;
    EXPECT_EQ(loaded.error().code, ErrorCode::InvalidVersion)
        << "carrier: " << carrier << " -> " << loaded.error().message;
  }
}

TEST(JsonInterfaceTest, CarrierAcceptsFullVersionIdRange) {
  auto zero = loadText(R"({"__version": 0})");
  ASSERT_TRUE(zero) << zero.error().message;
  EXPECT_EQ(zero->version, 0u);

  auto max = loadText(R"({"__version": 4294967295})");
  ASSERT_TRUE(max) << max.error().message;
  EXPECT_EQ(max->version, 4294967295u);
}

TEST(JsonInterfaceTest, SaveRejectsReservedCarrierInModel) {
  const VersionedConfig config = makeConfig(
      1, ConfigValue::object().set("__version", ConfigValue::of(
                                                    std::int64_t{2})));
  std::ostringstream out;
  JsonInterface backend;
  auto saved = backend.save(config, out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

TEST(JsonInterfaceTest, SaveAllowsNestedVersionKey) {
  const VersionedConfig config = makeConfig(
      1, ConfigValue::object().set(
             "a", ConfigValue::object().set("__version",
                                            ConfigValue::of(std::int64_t{2}))));
  auto reloaded = loadText(saveToText(config));
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->model.get<std::int64_t>("a.__version").value(), 2);
}

// ---- Document shape rules (§6, §6.1) --------------------------------------------

TEST(JsonInterfaceTest, NonObjectRootIsParseError) {
  for (const std::string& document :
       {std::string(R"([1, 2])"), std::string(R"(42)"),
        std::string(R"("text")"), std::string(R"(null)"),
        std::string(R"(true)")}) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted root: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "root: " << document;
  }
}

TEST(JsonInterfaceTest, NonAddressableKeysAreParseError) {
  for (const std::string& document : {
           std::string(R"({"__version": 1, "a.b": 2})"),
           std::string(R"({"__version": 1, "a[0]": 2})"),
           std::string(R"({"__version": 1, "a]b": 2})"),
           std::string(R"({"__version": 1, "": 2})"),
           std::string(R"({"__version": 1, "ok": {"bad.key": 2}})"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

TEST(JsonInterfaceTest, DuplicateKeysAreParseError) {
  for (const std::string& document : {
           std::string(R"({"__version": 1, "a": 1, "a": 2})"),
           std::string(R"({"__version": 1, "o": {"x": 1, "x": 2}})"),
           std::string(R"({"__version": 1, "__version": 1})"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

// ---- Number mapping (§6.1) -------------------------------------------------------

TEST(JsonInterfaceTest, IntegralNumbersMapToIntOthersToDouble) {
  auto loaded = loadText(
      R"({"__version": 1, "i": 7, "n": -3, "min": -9223372036854775808,)"
      R"( "max": 9223372036854775807, "d": 1.0, "e": 1e2})");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.nodeAt("i")->type(), NodeType::Int);
  EXPECT_EQ(loaded->model.nodeAt("n")->type(), NodeType::Int);
  EXPECT_EQ(loaded->model.get<std::int64_t>("min").value(),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(loaded->model.get<std::int64_t>("max").value(),
            std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(loaded->model.nodeAt("d")->type(), NodeType::Double);
  EXPECT_EQ(loaded->model.nodeAt("e")->type(), NodeType::Double);
  EXPECT_EQ(loaded->model.get<double>("e").value(), 100.0);
}

TEST(JsonInterfaceTest, IntegralNumberOutsideInt64IsParseError) {
  for (const std::string& number : {
           std::string("9223372036854775808"),     // INT64_MAX + 1
           std::string("18446744073709551615"),    // UINT64_MAX
           std::string("-9223372036854775809"),    // INT64_MIN - 1
           std::string("99999999999999999999"),    // > UINT64_MAX
       }) {
    auto loaded = loadText(R"({"__version": 1, "n": )" + number + "}");
    ASSERT_FALSE(loaded) << "accepted number: " << number;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "number: " << number;
  }
}

// ---- Malformed input and exception mapping (ADR-018) -----------------------------

TEST(JsonInterfaceTest, MalformedJsonIsParseError) {
  for (const std::string& document : {
           std::string(""),
           std::string("{"),
           std::string(R"({"__version": 1,})"),
           std::string(R"({"__version": 1} trailing)"),
           std::string(R"({"__version": 1, "d": 1e400})"),  // double overflow
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

class ThrowingInBuf : public std::streambuf {
 protected:
  int_type underflow() override {
    throw std::runtime_error("simulated read failure");
  }
};

TEST(JsonInterfaceTest, ThrowingInputStreamIsParseError) {
  ThrowingInBuf buffer;
  std::istream in(&buffer);
  JsonInterface backend;
  auto loaded = backend.load(in);
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

class FailingOutBuf : public std::streambuf {
 protected:
  int_type overflow(int_type) override { return traits_type::eof(); }
};

TEST(JsonInterfaceTest, FailingOutputStreamIsSerializationError) {
  FailingOutBuf buffer;
  std::ostream out(&buffer);
  JsonInterface backend;
  auto saved = backend.save(makeConfig(1, ConfigValue::object()), out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

TEST(JsonInterfaceTest, ThrowingOutputStreamIsSerializationError) {
  FailingOutBuf buffer;
  std::ostream out(&buffer);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  JsonInterface backend;
  auto saved = backend.save(makeConfig(1, ConfigValue::object()), out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

TEST(JsonInterfaceTest, InvalidUtf8StringFailsSaveWithSerializationError) {
  const VersionedConfig config = makeConfig(
      1, ConfigValue::object().set("s", ConfigValue::of(std::string(
                                            "\xFF\xFE invalid utf-8"))));
  std::ostringstream out;
  JsonInterface backend;
  auto saved = backend.save(config, out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

// {"a": {"a": ... 1 ...}} with `wraps` nested objects: placed as the value
// of a root member, the innermost scalar sits at depth wraps + 1.
std::string nestedObjectDocument(std::size_t wraps) {
  std::string document = R"({"__version": 1, "a": )";
  for (std::size_t i = 0; i < wraps; ++i) {
    document += R"({"a": )";
  }
  document += "1";
  document.append(wraps, '}');
  document += "}";
  return document;
}

TEST(JsonInterfaceTest, DocumentBeyondMaxTreeDepthIsParseError) {
  auto loaded = loadText(nestedObjectDocument(kMaxTreeDepth));
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
  EXPECT_NE(loaded.error().message.find("nesting depth"), std::string::npos);
}

TEST(JsonInterfaceTest, DocumentAtMaxTreeDepthLoads) {
  auto loaded = loadText(nestedObjectDocument(kMaxTreeDepth - 1));
  ASSERT_TRUE(loaded) << loaded.error().message;
}

TEST(JsonInterfaceTest, DeepArrayNestingIsParseError) {
  std::string document = R"({"__version": 1, "a": )";
  document.append(kMaxTreeDepth + 1, '[');
  document.append(kMaxTreeDepth + 1, ']');
  document += "}";
  auto loaded = loadText(document);
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

TEST(JsonInterfaceTest, NonFiniteDoubleFailsSaveWithSerializationError) {
  const double nonFinite[] = {std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()};
  for (const double value : nonFinite) {
    const VersionedConfig config = makeConfig(
        1, ConfigValue::object().set("d", ConfigValue::of(value)));
    std::ostringstream out;
    JsonInterface backend;
    auto saved = backend.save(config, out);
    ASSERT_FALSE(saved);
    EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
    EXPECT_NE(saved.error().message.find("'d'"), std::string::npos);
  }
}

TEST(JsonInterfaceTest, NonFiniteDoubleInArrayFailsSave) {
  const VersionedConfig config = makeConfig(
      1, ConfigValue::object().set(
             "a", ConfigValue::array().push(ConfigValue::of(
                      std::numeric_limits<double>::infinity()))));
  std::ostringstream out;
  JsonInterface backend;
  auto saved = backend.save(config, out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

}  // namespace
}  // namespace configmanager

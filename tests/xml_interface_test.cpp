#include "configmanager/backends/xml_interface.hpp"

#include <cmath>
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
  XmlInterface backend;
  return backend.load(in);
}

VersionedConfig makeConfig(VersionId version, ConfigValue root) {
  Result<ConfigModel> model = ConfigModel::fromValue(std::move(root));
  return VersionedConfig{version, std::move(model).value()};
}

std::string saveToText(const VersionedConfig& config) {
  std::ostringstream out;
  XmlInterface backend;
  auto saved = backend.save(config, out);
  EXPECT_TRUE(saved) << (saved ? "" : saved.error().message);
  return out.str();
}

// ---- Round trip ---------------------------------------------------------------

TEST(XmlInterfaceTest, RoundTripPreservesValuesTypesAndOrder) {
  const std::string document = R"(<config version="3">
    <zebra>stripes</zebra>
    <flag type="bool">true</flag>
    <nothing type="null"/>
    <count type="int">-42</count>
    <ratio type="double">2.5</ratio>
    <nested>
      <b type="int">1</b>
      <a type="array">
        <item type="int">1</item>
        <item>two</item>
        <item type="bool">false</item>
        <item><deep type="null"/></item>
      </a>
    </nested>
    <empty_object type="object"/>
    <empty_array type="array"/>
  </config>)";

  auto first = loadText(document);
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_EQ(first->version, 3u);

  auto second = loadText(saveToText(*first));
  ASSERT_TRUE(second) << second.error().message;

  EXPECT_EQ(second->version, 3u);
  EXPECT_EQ(second->model.get<std::string>("zebra").value(), "stripes");
  EXPECT_EQ(second->model.get<bool>("flag").value(), true);
  EXPECT_EQ(second->model.nodeAt("nothing")->type(), NodeType::Null);
  EXPECT_EQ(second->model.get<std::int64_t>("count").value(), -42);
  EXPECT_EQ(second->model.get<double>("ratio").value(), 2.5);
  EXPECT_EQ(second->model.get<std::int64_t>("nested.b").value(), 1);
  EXPECT_EQ(second->model.get<std::string>("nested.a[1]").value(), "two");
  EXPECT_EQ(second->model.get<bool>("nested.a[2]").value(), false);
  EXPECT_EQ(second->model.nodeAt("nested.a[3].deep")->type(), NodeType::Null);

  // Empty containers keep their type through the round trip (§6.1: an empty
  // Object carries type="object", distinguishable from the empty string).
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

TEST(XmlInterfaceTest, SaveWritesConfigRootWithVersionAttribute) {
  const std::string text =
      saveToText(makeConfig(9, ConfigValue::object().set(
                                   "key", ConfigValue::of(std::int64_t{1}))));
  EXPECT_NE(text.find("<config version=\"9\">"), std::string::npos) << text;
  EXPECT_NE(text.find("<key type=\"int\">1</key>"), std::string::npos)
      << text;
}

TEST(XmlInterfaceTest, EmptyDocumentRoundTrip) {
  auto loaded = loadText(R"(<config version="7"/>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->version, 7u);
  EXPECT_EQ(loaded->model.root().size(), 0u);

  auto reloaded = loadText(saveToText(*loaded));
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->version, 7u);
  EXPECT_EQ(reloaded->model.root().size(), 0u);
}

TEST(XmlInterfaceTest, EmptyStringIsBareElement) {
  auto loaded = loadText(R"(<config version="1"><s/></config>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.nodeAt("s")->type(), NodeType::String);
  EXPECT_EQ(loaded->model.get<std::string>("s").value(), "");

  auto reloaded = loadText(saveToText(*loaded));
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->model.get<std::string>("s").value(), "");
}

TEST(XmlInterfaceTest, WhitespaceInStringsIsPreserved) {
  auto loaded = loadText(
      "<config version=\"1\"><blank>  </blank>"
      "<padded>  x  </padded></config>");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.get<std::string>("blank").value(), "  ");
  EXPECT_EQ(loaded->model.get<std::string>("padded").value(), "  x  ");

  auto reloaded = loadText(saveToText(*loaded));
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->model.get<std::string>("blank").value(), "  ");
  EXPECT_EQ(reloaded->model.get<std::string>("padded").value(), "  x  ");
}

TEST(XmlInterfaceTest, SpecialCharacterStringsRoundTrip) {
  const std::string markup = "<tag> & \"quotes\" 'apos' ]]>";
  const std::string control = "line1\nline2\ttabbed";
  const VersionedConfig config = makeConfig(
      1, ConfigValue::object()
             .set("markup", ConfigValue::of(markup))
             .set("control", ConfigValue::of(control)));

  auto reloaded = loadText(saveToText(config));
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->model.get<std::string>("markup").value(), markup);
  EXPECT_EQ(reloaded->model.get<std::string>("control").value(), control);
}

// ---- Version carrier (ADR-014, ADR-020) ----------------------------------------

TEST(XmlInterfaceTest, CarrierAllowsLeadingZeros) {
  auto loaded = loadText(R"(<config version="007"/>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->version, 7u);
}

TEST(XmlInterfaceTest, MissingVersionIsInvalidVersion) {
  for (const std::string& document : {
           std::string(R"(<config/>)"),
           std::string(R"(<config><a type="int">1</a></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::InvalidVersion)
        << "document: " << document;
  }
}

TEST(XmlInterfaceTest, MalformedCarrierIsInvalidVersion) {
  const std::vector<std::string> carriers{
      "",                          // empty
      " 1",                        // leading whitespace
      "1 ",                        // trailing whitespace
      "-1",                        // negative
      "+1",                        // sign
      "1.0",                       // float
      "1e2",                       // exponent form
      "0x10",                      // hex
      "abc",                       // not a number
      "4294967296",                // 2^32, above VersionId
      "18446744073709551616",      // 2^64, overflows every integer type
      "99999999999999999999999",   // far beyond any integer type
  };
  for (const std::string& carrier : carriers) {
    auto loaded = loadText("<config version=\"" + carrier + "\"/>");
    ASSERT_FALSE(loaded) << "carrier accepted: '" << carrier << "'";
    EXPECT_EQ(loaded.error().code, ErrorCode::InvalidVersion)
        << "carrier: '" << carrier << "' -> " << loaded.error().message;
  }
}

TEST(XmlInterfaceTest, CarrierAcceptsFullVersionIdRange) {
  auto zero = loadText(R"(<config version="0"/>)");
  ASSERT_TRUE(zero) << zero.error().message;
  EXPECT_EQ(zero->version, 0u);

  auto max = loadText(R"(<config version="4294967295"/>)");
  ASSERT_TRUE(max) << max.error().message;
  EXPECT_EQ(max->version, 4294967295u);
}

TEST(XmlInterfaceTest, DuplicateVersionAttributeIsParseError) {
  // pugixml accepts duplicate attribute names; the backend must not resolve
  // them last-wins (§6.1: duplicates are never resolved).
  auto loaded = loadText(R"(<config version="1" version="2"/>)");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

TEST(XmlInterfaceTest, DocumentBeyondMaxTreeDepthIsParseError) {
  std::string document = R"(<config version="1">)";
  for (std::size_t i = 0; i <= kMaxTreeDepth; ++i) {
    document += "<a>";
  }
  document += "1";
  for (std::size_t i = 0; i <= kMaxTreeDepth; ++i) {
    document += "</a>";
  }
  document += "</config>";
  auto loaded = loadText(document);
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
  EXPECT_NE(loaded.error().message.find("nesting depth"), std::string::npos);
}

TEST(XmlInterfaceTest, DocumentAtMaxTreeDepthLoads) {
  std::string document = R"(<config version="1">)";
  for (std::size_t i = 1; i < kMaxTreeDepth; ++i) {
    document += "<a>";
  }
  document += "<a>1</a>";
  for (std::size_t i = 1; i < kMaxTreeDepth; ++i) {
    document += "</a>";
  }
  document += "</config>";
  auto loaded = loadText(document);
  ASSERT_TRUE(loaded) << loaded.error().message;
}

TEST(XmlInterfaceTest, DuplicateTypeAttributeOnRootIsParseError) {
  auto loaded =
      loadText(R"(<config version="1" type="object" type="object"/>)");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

TEST(XmlInterfaceTest, TextOutsideRootElementIsParseError) {
  auto leading = loadText("junk<config version=\"1\"/>");
  ASSERT_FALSE(leading);
  EXPECT_EQ(leading.error().code, ErrorCode::ParseError);

  auto trailing = loadText("<config version=\"1\"/>trailing");
  ASSERT_FALSE(trailing);
  EXPECT_EQ(trailing.error().code, ErrorCode::ParseError);

  // Whitespace around the root stays legal.
  auto padded = loadText("\n  <config version=\"1\"/>\n");
  EXPECT_TRUE(padded) << padded.error().message;
}

TEST(XmlInterfaceTest, VersionAttributeOnNonRootIsParseError) {
  auto loaded = loadText(
      R"(<config version="1"><a version="2" type="int">1</a></config>)");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

// The carrier is a root attribute, outside the model's key space, so the
// ADR-020 save() conflict check is vacuous: a member named "version" is
// plain data in both directions.
TEST(XmlInterfaceTest, ModelKeyNamedVersionIsPlainData) {
  auto loaded = loadText(
      R"(<config version="1"><version type="int">42</version></config>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->version, 1u);
  EXPECT_EQ(loaded->model.get<std::int64_t>("version").value(), 42);

  const std::string text = saveToText(makeConfig(
      5, ConfigValue::object().set("version",
                                   ConfigValue::of(std::int64_t{9}))));
  EXPECT_NE(text.find("<config version=\"5\">"), std::string::npos) << text;
  auto reloaded = loadText(text);
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->version, 5u);
  EXPECT_EQ(reloaded->model.get<std::int64_t>("version").value(), 9);
}

// ---- Document shape rules (§6, §6.1) --------------------------------------------

TEST(XmlInterfaceTest, RootMustBeConfigElement) {
  auto loaded = loadText(R"(<settings version="1"/>)");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

TEST(XmlInterfaceTest, RootTypeMustBeObject) {
  auto object = loadText(R"(<config version="1" type="object"/>)");
  ASSERT_TRUE(object) << object.error().message;

  for (const std::string& type : {"array", "int", "string"}) {
    auto loaded =
        loadText("<config version=\"1\" type=\"" + type + "\"/>");
    ASSERT_FALSE(loaded) << "accepted root type: " << type;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError) << "type: " << type;
  }
}

TEST(XmlInterfaceTest, UnknownAttributesAreParseError) {
  for (const std::string& document : {
           std::string(R"(<config version="1" extra="x"/>)"),
           std::string(R"(<config version="1" xmlns="urn:x"/>)"),
           std::string(R"(<config version="1"><a foo="1">x</a></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

TEST(XmlInterfaceTest, UnknownTypeValuesAreParseError) {
  // "string" is deliberately not a reserved value: absent means string.
  for (const std::string& type : {"string", "float", "Object", "list"}) {
    auto loaded = loadText("<config version=\"1\"><a type=\"" + type +
                           "\">x</a></config>");
    ASSERT_FALSE(loaded) << "accepted type: " << type;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError) << "type: " << type;
  }
}

TEST(XmlInterfaceTest, RepeatedSiblingNamesAreParseError) {
  for (const std::string& document : {
           std::string(R"(<config version="1"><a>1</a><a>2</a></config>)"),
           std::string(
               R"(<config version="1"><o><x>1</x><x>2</x></o></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

TEST(XmlInterfaceTest, NonItemChildUnderArrayIsParseError) {
  auto loaded = loadText(
      R"(<config version="1"><a type="array"><element>1</element></a></config>)");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

TEST(XmlInterfaceTest, ScalarWithChildElementsIsParseError) {
  for (const std::string& document : {
           std::string(
               R"(<config version="1"><a type="int"><b/>1</a></config>)"),
           std::string(
               R"(<config version="1"><a type="null"><b/></a></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

TEST(XmlInterfaceTest, MixedContentIsParseError) {
  for (const std::string& document : {
           // Untyped element with both text and children.
           std::string(
               R"(<config version="1"><a>text<b type="int">1</b></a></config>)"),
           std::string(R"(<config version="1">text</config>)"),
           std::string(
               R"(<config version="1"><a type="object">text</a></config>)"),
           std::string(
               R"(<config version="1"><a type="array">text</a></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

TEST(XmlInterfaceTest, NonAddressableElementNamesAreParseError) {
  // '.' is a valid XML name character but reserved by the path grammar
  // (ADR-021).
  auto loaded =
      loadText(R"(<config version="1"><a.b type="int">1</a.b></config>)");
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

TEST(XmlInterfaceTest, UntypedElementWithChildrenIsObject) {
  auto loaded = loadText(R"(<config version="1">
    <server>
      <host>relay.example</host>
      <port type="int">8080</port>
    </server>
    <tagged type="object"><x type="int">1</x></tagged>
  </config>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.nodeAt("server")->type(), NodeType::Object);
  EXPECT_EQ(loaded->model.get<std::string>("server.host").value(),
            "relay.example");
  EXPECT_EQ(loaded->model.get<std::int64_t>("server.port").value(), 8080);
  // An explicit type="object" on a non-empty element is also admitted.
  EXPECT_EQ(loaded->model.get<std::int64_t>("tagged.x").value(), 1);
}

TEST(XmlInterfaceTest, NullElementMustBeEmpty) {
  for (const std::string& document : {
           std::string(
               R"(<config version="1"><a type="null">null</a></config>)"),
           std::string(R"(<config version="1"><a type="null"> </a></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "document: " << document;
  }
}

// ---- Scalar literal grammar (§6.1) -----------------------------------------------

TEST(XmlInterfaceTest, BoolTextIsStrict) {
  for (const std::string& text :
       {"True", "TRUE", "1", "", " true", "true "}) {
    auto loaded = loadText("<config version=\"1\"><b type=\"bool\">" + text +
                           "</b></config>");
    ASSERT_FALSE(loaded) << "accepted bool text: '" << text << "'";
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError)
        << "text: '" << text << "'";
  }
}

TEST(XmlInterfaceTest, IntTextEdges) {
  auto loaded = loadText(R"(<config version="1">
    <min type="int">-9223372036854775808</min>
    <max type="int">9223372036854775807</max>
    <padded type="int">007</padded>
  </config>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.get<std::int64_t>("min").value(),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(loaded->model.get<std::int64_t>("max").value(),
            std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(loaded->model.get<std::int64_t>("padded").value(), 7);

  for (const std::string& text : {
           std::string(""),
           std::string("1x"),
           std::string("1.0"),
           std::string(" 1"),
           std::string("+1"),
           std::string("9223372036854775808"),   // INT64_MAX + 1
           std::string("18446744073709551615"),  // UINT64_MAX
       }) {
    auto bad = loadText("<config version=\"1\"><i type=\"int\">" + text +
                        "</i></config>");
    ASSERT_FALSE(bad) << "accepted int text: '" << text << "'";
    EXPECT_EQ(bad.error().code, ErrorCode::ParseError)
        << "text: '" << text << "'";
  }
}

TEST(XmlInterfaceTest, DoubleTextEdges) {
  auto loaded = loadText(R"(<config version="1">
    <plain type="double">2.5</plain>
    <exponent type="double">1e2</exponent>
    <negative type="double">-0.5</negative>
    <integral type="double">100</integral>
  </config>)");
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.get<double>("plain").value(), 2.5);
  EXPECT_EQ(loaded->model.get<double>("exponent").value(), 100.0);
  EXPECT_EQ(loaded->model.get<double>("negative").value(), -0.5);
  EXPECT_EQ(loaded->model.get<double>("integral").value(), 100.0);
  EXPECT_EQ(loaded->model.nodeAt("integral")->type(), NodeType::Double);

  for (const std::string& text :
       {"", "nan", "inf", "-inf", "1e400", "0x1", "abc", " 2.5"}) {
    auto bad = loadText("<config version=\"1\"><d type=\"double\">" + text +
                        "</d></config>");
    ASSERT_FALSE(bad) << "accepted double text: '" << text << "'";
    EXPECT_EQ(bad.error().code, ErrorCode::ParseError)
        << "text: '" << text << "'";
  }
}

// ---- Save-side restrictions (§6.1) ------------------------------------------------

TEST(XmlInterfaceTest, InvalidElementNameFailsSaveWithSerializationError) {
  // "\xC3\x97" is U+00D7 (multiplication sign), excluded from the XML
  // NameStartChar production; "a\xE2\x80\xB0" ends in U+2030 (per mille),
  // not a NameChar (regression: its low byte is 0x30, ASCII '0');
  // "k\xFFy" is not valid UTF-8 at all.
  for (const std::string& key : {"1bad", "bad key", "a:b", "-lead",
                                 "\xC3\x97ratio", "a\xE2\x80\xB0", "k\xFFy"}) {
    const VersionedConfig config = makeConfig(
        1, ConfigValue::object().set(key, ConfigValue::of(std::int64_t{1})));
    std::ostringstream out;
    XmlInterface backend;
    auto saved = backend.save(config, out);
    ASSERT_FALSE(saved) << "accepted key: '" << key << "'";
    EXPECT_EQ(saved.error().code, ErrorCode::SerializationError)
        << "key: '" << key << "'";
  }
}

TEST(XmlInterfaceTest, NonAsciiElementNameRoundTrips) {
  // §6.1 admits every valid XML element name, not just ASCII: "caf\xC3\xA9"
  // is "café" (é = U+00E9, a NameStartChar).
  const VersionedConfig config = makeConfig(
      1, ConfigValue::object().set("caf\xC3\xA9",
                                   ConfigValue::of(std::int64_t{7})));
  auto loaded = loadText(saveToText(config));
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.get<std::int64_t>("caf\xC3\xA9").value(), 7);
}

TEST(XmlInterfaceTest, NonXmlNameElementFailsLoad) {
  // pugixml admits any byte >= 0x80 in names; the backend must still reject
  // names outside the XML Name production (else save() could not rewrite
  // them): U+00D7 is not a name character, "a\xFF" is not valid UTF-8.
  for (const std::string& document : {
           std::string("<config version=\"1\"><\xC3\x97r>1</\xC3\x97r>"
                       "</config>"),
           std::string("<config version=\"1\"><a\xFF>1</a\xFF></config>"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
  }
}

TEST(XmlInterfaceTest, Utf8StringRoundTrips) {
  const std::string value = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C "
                            "\xF0\x9F\x9A\x80";  // héllo 世界 🚀
  const VersionedConfig config =
      makeConfig(1, ConfigValue::object().set("s", ConfigValue::of(value)));
  auto loaded = loadText(saveToText(config));
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->model.get<std::string>("s").value(), value);
}

TEST(XmlInterfaceTest, InvalidUtf8StringFailsSave) {
  // XML must be valid UTF-8: a stray continuation byte, a truncated
  // sequence, an overlong encoding, and the non-character U+FFFF would all
  // produce a document a conformant parser rejects.
  for (const std::string& value :
       {std::string("bad\xFF"), std::string("caf\xC3"),
        std::string("over\xC0\xAFlong"), std::string("\xEF\xBF\xBF")}) {
    const VersionedConfig config =
        makeConfig(1, ConfigValue::object().set("s", ConfigValue::of(value)));
    std::ostringstream out;
    XmlInterface backend;
    auto saved = backend.save(config, out);
    ASSERT_FALSE(saved) << "accepted string: '" << value << "'";
    EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
  }
}

TEST(XmlInterfaceTest, UnrepresentableStringTextFailsLoad) {
  // pugixml passes raw bytes and expanded character references through
  // unvalidated; the backend must reject text save() could not rewrite:
  // invalid UTF-8, a control character via reference, and a carriage
  // return via reference (a literal CR would be normalized away, but
  // &#13; survives expansion).
  for (const std::string& document : {
           std::string("<config version=\"1\"><s>ab\xFF</s></config>"),
           std::string(R"(<config version="1"><s>a&#11;b</s></config>)"),
           std::string(R"(<config version="1"><s>a&#13;b</s></config>)"),
       }) {
    auto loaded = loadText(document);
    ASSERT_FALSE(loaded) << "accepted document: " << document;
    EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
  }
}

TEST(XmlInterfaceTest, ControlCharacterStringFailsSave) {
  // A literal carriage return is included: it cannot survive the mandatory
  // line-ending normalization of a conformant XML parse.
  for (const std::string& value :
       {std::string("bad\x01byte"), std::string("line1\rline2")}) {
    const VersionedConfig config =
        makeConfig(1, ConfigValue::object().set("s", ConfigValue::of(value)));
    std::ostringstream out;
    XmlInterface backend;
    auto saved = backend.save(config, out);
    ASSERT_FALSE(saved) << "accepted string: '" << value << "'";
    EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
  }
}

TEST(XmlInterfaceTest, NonFiniteDoubleFailsSave) {
  for (const double value : {std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    const VersionedConfig config =
        makeConfig(1, ConfigValue::object().set("d", ConfigValue::of(value)));
    std::ostringstream out;
    XmlInterface backend;
    auto saved = backend.save(config, out);
    ASSERT_FALSE(saved) << "accepted double: " << value;
    EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
  }
}

// ---- Malformed input and exception mapping (ADR-018) -----------------------------

TEST(XmlInterfaceTest, MalformedXmlIsParseError) {
  for (const std::string& document : {
           std::string(""),
           std::string("<config"),
           std::string(R"(<config version="1"><a></config>)"),
           std::string("plain text"),
           std::string(R"(<config version="1"/><extra/>)"),  // two roots
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

TEST(XmlInterfaceTest, ThrowingInputStreamIsParseError) {
  ThrowingInBuf buffer;
  std::istream in(&buffer);
  XmlInterface backend;
  auto loaded = backend.load(in);
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code, ErrorCode::ParseError);
}

class FailingOutBuf : public std::streambuf {
 protected:
  int_type overflow(int_type) override { return traits_type::eof(); }
};

TEST(XmlInterfaceTest, FailingOutputStreamIsSerializationError) {
  FailingOutBuf buffer;
  std::ostream out(&buffer);
  XmlInterface backend;
  auto saved = backend.save(makeConfig(1, ConfigValue::object()), out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

TEST(XmlInterfaceTest, ThrowingOutputStreamIsSerializationError) {
  FailingOutBuf buffer;
  std::ostream out(&buffer);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  XmlInterface backend;
  auto saved = backend.save(makeConfig(1, ConfigValue::object()), out);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::SerializationError);
}

}  // namespace
}  // namespace configmanager

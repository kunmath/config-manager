#include "configmanager/backends/xml_interface.hpp"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <istream>
#include <limits>
#include <new>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "configmanager/config_model.hpp"
#include "configmanager/config_node.hpp"
#include "configmanager/config_value.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {
namespace {

constexpr char kRootName[] = "config";
constexpr char kVersionAttr[] = "version";
constexpr char kTypeAttr[] = "type";
constexpr char kItemName[] = "item";
constexpr std::uint64_t kMaxVersion = std::numeric_limits<VersionId>::max();

// parse_ws_pcdata keeps whitespace-only text nodes: they are formatting
// inside containers but significant inside string scalars (e.g. <s>  </s>).
// parse_fragment keeps document-level text too — without it pugixml
// silently discards text outside the root element, which load() must
// reject as malformed. The relaxations fragment mode brings (no root,
// several roots) are all checked explicitly below.
constexpr unsigned int kParseFlags =
    pugi::parse_default | pugi::parse_ws_pcdata | pugi::parse_fragment;

bool isPathAddressable(std::string_view key) {
  return !key.empty() && key.find_first_of(".[]") == std::string_view::npos;
}

bool isXmlWhitespace(std::string_view text) {
  return text.find_first_not_of(" \t\n\r") == std::string_view::npos;
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Strict UTF-8 decode of the code point starting at text[i]; advances i past
// it. Overlong forms, surrogates, values above U+10FFFF, and truncated or
// malformed sequences yield nullopt (i is left unspecified).
std::optional<char32_t> decodeUtf8(std::string_view text, std::size_t& i) {
  const unsigned char lead = static_cast<unsigned char>(text[i]);
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  std::size_t length = 0;
  char32_t value = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    value = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    value = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    value = lead & 0x07;
  } else {
    return std::nullopt;  // continuation byte as lead, or 0xF8-0xFF
  }
  if (i + length > text.size()) return std::nullopt;
  for (std::size_t k = 1; k < length; ++k) {
    const unsigned char cont = static_cast<unsigned char>(text[i + k]);
    if ((cont & 0xC0) != 0x80) return std::nullopt;
    value = (value << 6) | (cont & 0x3F);
  }
  constexpr char32_t kMinForLength[] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < kMinForLength[length]) return std::nullopt;       // overlong
  if (value >= 0xD800 && value <= 0xDFFF) return std::nullopt;  // surrogate
  if (value > 0x10FFFF) return std::nullopt;
  i += length;
  return value;
}

// XML 1.0 §2.3 NameStartChar, without ':' (a colon would imply namespaces,
// which this restricted mapping does not model).
bool isNameStartChar(char32_t c) {
  return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) ||
         (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
         (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) ||
         (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) ||
         (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
         (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

bool isNameChar(char32_t c) {
  return isNameStartChar(c) || c == '-' || c == '.' ||
         (c >= '0' && c <= '9') || c == 0xB7 ||
         (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

// The XML 1.0 Name production over UTF-8 (minus ':'). Enforced symmetrically:
// save() rejects model keys outside it (they would emit a malformed
// document), and load() rejects element names outside it (pugixml's parser
// is more permissive than the spec — it admits any byte >= 0x80 in names
// unvalidated), so every document load() accepts can be re-saved.
bool isValidElementName(std::string_view name) {
  if (name.empty()) return false;
  std::size_t i = 0;
  bool first = true;
  while (i < name.size()) {
    const std::optional<char32_t> c = decodeUtf8(name, i);
    if (!c) return false;
    if (first ? !isNameStartChar(*c) : !isNameChar(*c)) return false;
    first = false;
  }
  return true;
}

// XML 1.0 carries only valid UTF-8 encoding the Char production: among the
// C0 controls just tab and newline (a literal carriage return is also legal
// but never survives the mandatory line-ending normalization of a conformant
// parse), no U+FFFE/U+FFFF. Enforced symmetrically: save() rejects strings
// outside it, and load() rejects text outside it (pugixml passes raw bytes
// and expanded character references through unvalidated), so every string
// load() accepts can be re-saved.
bool isXmlRepresentable(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const std::optional<char32_t> c = decodeUtf8(text, i);
    if (!c) return false;
    if (*c < 0x20 && *c != '\t' && *c != '\n') return false;
    if (*c == 0xFFFE || *c == 0xFFFF) return false;
  }
  return true;
}

Result<VersionId> parseVersionCarrier(std::string_view text) {
  if (text.empty()) {
    return fail(ErrorCode::InvalidVersion,
                "version attribute must not be empty");
  }
  for (const char c : text) {
    if (!isAsciiDigit(c)) {
      return fail(ErrorCode::InvalidVersion,
                  "version attribute must be decimal digits only, got '" +
                      std::string(text) + "'");
    }
  }
  std::uint64_t value = 0;
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size() ||
      value > kMaxVersion) {
    return fail(ErrorCode::InvalidVersion,
                "version attribute value '" + std::string(text) +
                    "' is not representable in VersionId");
  }
  return static_cast<VersionId>(value);
}

Result<ConfigValue> parseValue(const pugi::xml_node& element,
                               std::size_t depth);

bool hasElementChildren(const pugi::xml_node& element) {
  for (const pugi::xml_node& child : element.children()) {
    if (child.type() == pugi::node_element) return true;
  }
  return false;
}

// Concatenated pcdata/cdata text of a scalar element; child elements violate
// §6.1 ("elements with a scalar or absent type must have no child elements").
Result<std::string> scalarText(const pugi::xml_node& element) {
  std::string text;
  for (const pugi::xml_node& child : element.children()) {
    switch (child.type()) {
      case pugi::node_pcdata:
      case pugi::node_cdata:
        text += child.value();
        break;
      case pugi::node_element:
        return fail(ErrorCode::ParseError,
                    "scalar element '" + std::string(element.name()) +
                        "' must have no child elements");
      default:
        break;  // comments/PIs/doctype are not parsed under kParseFlags
    }
  }
  return text;
}

// `depth` is the element's node depth in the model, the <config> root at 0.
Result<ConfigValue> parseObjectMembers(const pugi::xml_node& element,
                                       std::size_t depth) {
  ConfigValue object = ConfigValue::object();
  for (const pugi::xml_node& child : element.children()) {
    switch (child.type()) {
      case pugi::node_element: {
        const std::string key = child.name();
        if (!isPathAddressable(key)) {
          return fail(ErrorCode::ParseError,
                      "element name '" + key +
                          "' is not path-addressable (contains '.', '[', "
                          "or ']')");
        }
        if (!isValidElementName(key)) {
          return fail(ErrorCode::ParseError,
                      "element name '" + key +
                          "' is not a valid XML name in this mapping");
        }
        for (const auto& member : object.members()) {
          if (member.first == key) {
            return fail(ErrorCode::ParseError,
                        "repeated sibling element '" + key + "' under '" +
                            std::string(element.name()) + "'");
          }
        }
        Result<ConfigValue> value = parseValue(child, depth + 1);
        if (!value) {
          return value;
        }
        object.set(key, std::move(*value));
        break;
      }
      case pugi::node_pcdata:
      case pugi::node_cdata:
        if (!isXmlWhitespace(child.value())) {
          return fail(ErrorCode::ParseError,
                      "object element '" + std::string(element.name()) +
                          "' must have no text");
        }
        break;
      default:
        break;
    }
  }
  return object;
}

Result<ConfigValue> parseArrayItems(const pugi::xml_node& element,
                                    std::size_t depth) {
  ConfigValue array = ConfigValue::array();
  for (const pugi::xml_node& child : element.children()) {
    switch (child.type()) {
      case pugi::node_element: {
        if (std::string_view(child.name()) != kItemName) {
          return fail(ErrorCode::ParseError,
                      "array element '" + std::string(element.name()) +
                          "' must contain only <item> children, got '" +
                          child.name() + "'");
        }
        Result<ConfigValue> value = parseValue(child, depth + 1);
        if (!value) {
          return value;
        }
        array.push(std::move(*value));
        break;
      }
      case pugi::node_pcdata:
      case pugi::node_cdata:
        if (!isXmlWhitespace(child.value())) {
          return fail(ErrorCode::ParseError,
                      "array element '" + std::string(element.name()) +
                          "' must have no text");
        }
        break;
      default:
        break;
    }
  }
  return array;
}

Result<ConfigValue> parseBoolText(const std::string& text,
                                  const pugi::xml_node& element) {
  if (text == "true") return ConfigValue::of(true);
  if (text == "false") return ConfigValue::of(false);
  return fail(ErrorCode::ParseError,
              "bool element '" + std::string(element.name()) +
                  "' must be exactly 'true' or 'false', got '" + text + "'");
}

Result<ConfigValue> parseIntText(const std::string& text,
                                 const pugi::xml_node& element) {
  std::int64_t value = 0;
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec == std::errc::result_out_of_range) {
    return fail(ErrorCode::ParseError,
                "int element text '" + text +
                    "' is outside std::int64_t's range");
  }
  if (ec != std::errc() || ptr != text.data() + text.size()) {
    return fail(ErrorCode::ParseError,
                "int element '" + std::string(element.name()) +
                    "' text '" + text + "' is not a decimal integer");
  }
  return ConfigValue::of(value);
}

Result<ConfigValue> parseDoubleText(const std::string& text,
                                    const pugi::xml_node& element) {
  double value = 0.0;
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size() ||
      !std::isfinite(value)) {
    return fail(ErrorCode::ParseError,
                "double element '" + std::string(element.name()) +
                    "' text '" + text +
                    "' is not a finite decimal floating-point literal");
  }
  return ConfigValue::of(value);
}

Result<ConfigValue> parseValue(const pugi::xml_node& element,
                               std::size_t depth) {
  // Checked on the way down, so the parse recursion itself stays bounded
  // even for a hostile document (§4.4).
  if (depth > kMaxTreeDepth) {
    return fail(ErrorCode::ParseError,
                "document exceeds the maximum nesting depth (" +
                    std::to_string(kMaxTreeDepth) + ")");
  }
  const char* type = nullptr;
  for (const pugi::xml_attribute& attr : element.attributes()) {
    if (std::string_view(attr.name()) == kTypeAttr) {
      if (type != nullptr) {
        return fail(ErrorCode::ParseError,
                    "duplicate 'type' attribute on element '" +
                        std::string(element.name()) + "'");
      }
      type = attr.value();
    } else {
      return fail(ErrorCode::ParseError,
                  "attribute '" + std::string(attr.name()) +
                      "' on element '" + element.name() +
                      "' — only 'type' (and 'version' on the document "
                      "root) is reserved");
    }
  }

  if (type == nullptr) {
    // Untyped: child elements make it an Object (only an *empty* Object
    // carries type="object", §6.1); otherwise its text is a String and a
    // bare empty element is the empty string.
    if (hasElementChildren(element)) {
      return parseObjectMembers(element, depth);
    }
    Result<std::string> text = scalarText(element);
    if (!text) {
      return fail(text.error().code, std::move(text.error().message));
    }
    if (!isXmlRepresentable(*text)) {
      return fail(ErrorCode::ParseError,
                  "text of element '" + std::string(element.name()) +
                      "' is not valid UTF-8 XML character data");
    }
    return ConfigValue::of(std::move(*text));
  }

  const std::string_view typeName(type);
  if (typeName == "object") return parseObjectMembers(element, depth);
  if (typeName == "array") return parseArrayItems(element, depth);
  if (typeName == "null" || typeName == "bool" || typeName == "int" ||
      typeName == "double") {
    Result<std::string> text = scalarText(element);
    if (!text) {
      return fail(text.error().code, std::move(text.error().message));
    }
    if (typeName == "null") {
      if (!text->empty()) {
        return fail(ErrorCode::ParseError,
                    "null element '" + std::string(element.name()) +
                        "' must be empty");
      }
      return ConfigValue();
    }
    if (typeName == "bool") return parseBoolText(*text, element);
    if (typeName == "int") return parseIntText(*text, element);
    return parseDoubleText(*text, element);
  }
  return fail(ErrorCode::ParseError,
              "unknown type attribute value '" + std::string(typeName) +
                  "' on element '" + element.name() +
                  "' (expected bool, int, double, null, object, or array)");
}

// pugixml signals allocation failure with empty handles or false returns
// instead of exceptions. Convert to std::bad_alloc so the memory-exhaustion
// policy (ADR-018: bad_alloc propagates) holds, rather than save() reporting
// success for a silently truncated document.
template <typename HandleOrBool>
HandleOrBool checkAlloc(HandleOrBool result) {
  if (!result) {
    throw std::bad_alloc();
  }
  return result;
}

Result<void> appendValueElement(pugi::xml_node parent, const std::string& name,
                                const ConfigNode& node);

// The Result unwraps below are internal invariants, not fallible boundaries:
// the traversal only requests the type the node reports, on handles taken
// from a live model. value() throwing would be a library bug and is caught
// by save()'s catch-all (ADR-018).
Result<void> appendMembers(pugi::xml_node element, const ConfigNode& object) {
  const std::vector<std::string> keys = object.keys().value();
  for (const std::string& key : keys) {
    Result<void> appended =
        appendValueElement(element, key, object.child(key).value());
    if (!appended) {
      return appended;
    }
  }
  return {};
}

Result<void> appendItems(pugi::xml_node element, const ConfigNode& array) {
  for (std::size_t i = 0; i < array.size(); ++i) {
    Result<void> appended =
        appendValueElement(element, kItemName, array.at(i).value());
    if (!appended) {
      return appended;
    }
  }
  return {};
}

void setElementText(pugi::xml_node element, const std::string& text) {
  checkAlloc(
      checkAlloc(element.append_child(pugi::node_pcdata)).set_value(
          text.c_str()));
}

void setTypeAttribute(pugi::xml_node element, const char* type) {
  checkAlloc(checkAlloc(element.append_attribute(kTypeAttr)).set_value(type));
}

Result<void> appendValueElement(pugi::xml_node parent, const std::string& name,
                                const ConfigNode& node) {
  if (!isValidElementName(name)) {
    return fail(ErrorCode::SerializationError,
                "model key '" + name +
                    "' is not a valid XML element name (this backend admits "
                    "ASCII names matching [A-Za-z_][A-Za-z0-9_-]*)");
  }
  pugi::xml_node element = checkAlloc(parent.append_child(name.c_str()));
  switch (node.type()) {
    case NodeType::Null:
      setTypeAttribute(element, "null");
      return {};
    case NodeType::Bool:
      setTypeAttribute(element, "bool");
      setElementText(element, node.as<bool>().value() ? "true" : "false");
      return {};
    case NodeType::Int:
      setTypeAttribute(element, "int");
      setElementText(element, std::to_string(node.as<std::int64_t>().value()));
      return {};
    case NodeType::Double: {
      const double value = node.as<double>().value();
      if (!std::isfinite(value)) {
        return fail(ErrorCode::SerializationError,
                    "non-finite double at key '" + name +
                        "' is unrepresentable in the XML mapping");
      }
      char buffer[64];
      const auto [ptr, ec] =
          std::to_chars(buffer, buffer + sizeof buffer, value);
      assert(ec == std::errc() && "to_chars cannot fail for finite doubles");
      setTypeAttribute(element, "double");
      setElementText(element, std::string(buffer, ptr));
      return {};
    }
    case NodeType::String: {
      const std::string value = node.as<std::string>().value();
      if (!isXmlRepresentable(value)) {
        return fail(ErrorCode::SerializationError,
                    "string at key '" + name +
                        "' contains a control character that XML 1.0 "
                        "cannot represent");
      }
      if (!value.empty()) {
        setElementText(element, value);  // bare empty element == empty string
      }
      return {};
    }
    case NodeType::Object:
      if (node.size() == 0) {
        // Distinguishes an empty Object from the empty string (§6.1).
        setTypeAttribute(element, "object");
        return {};
      }
      return appendMembers(element, node);
    case NodeType::Array:
      setTypeAttribute(element, "array");
      return appendItems(element, node);
  }
  assert(false && "unreachable: exhaustive NodeType switch");
  return {};
}

}  // namespace

Result<VersionedConfig> XmlInterface::load(std::istream& in) {
  try {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load(in, kParseFlags);
    if (!parsed) {
      return fail(ErrorCode::ParseError,
                  std::string("XML parse failed: ") + parsed.description());
    }
    const pugi::xml_node root = doc.document_element();
    // pugixml is lenient about extra roots and stray text outside the root
    // element; both are well-formedness violations.
    for (const pugi::xml_node& child : doc.children()) {
      switch (child.type()) {
        case pugi::node_element:
          if (child != root) {
            return fail(ErrorCode::ParseError,
                        "document has more than one root element");
          }
          break;
        case pugi::node_pcdata:
        case pugi::node_cdata:
          if (!isXmlWhitespace(child.value())) {
            return fail(ErrorCode::ParseError,
                        "document has text outside the root element");
          }
          break;
        default:
          break;
      }
    }
    if (std::string_view(root.name()) != kRootName) {
      return fail(ErrorCode::ParseError,
                  "document root must be <config>, got <" +
                      std::string(root.name()) + ">");
    }

    std::optional<VersionId> version;
    bool sawTypeAttr = false;
    for (const pugi::xml_attribute& attr : root.attributes()) {
      const std::string_view name(attr.name());
      if (name == kVersionAttr) {
        if (version.has_value()) {
          return fail(ErrorCode::ParseError,
                      "duplicate 'version' attribute on the document root");
        }
        Result<VersionId> carrier = parseVersionCarrier(attr.value());
        if (!carrier) {
          return fail(carrier.error().code,
                      std::move(carrier.error().message));
        }
        version = *carrier;
      } else if (name == kTypeAttr) {
        if (sawTypeAttr) {
          return fail(ErrorCode::ParseError,
                      "duplicate 'type' attribute on the document root");
        }
        sawTypeAttr = true;
        if (std::string_view(attr.value()) != "object") {
          return fail(ErrorCode::ParseError,
                      "document root must be an object, got type=\"" +
                          std::string(attr.value()) + "\"");
        }
      } else {
        return fail(ErrorCode::ParseError,
                    "attribute '" + std::string(name) +
                        "' on the document root — only 'version' and "
                        "'type' are reserved");
      }
    }
    if (!version.has_value()) {
      return fail(ErrorCode::InvalidVersion,
                  "stream carries no version metadata (root 'version' "
                  "attribute)");
    }

    Result<ConfigValue> rootValue = parseObjectMembers(root, /*depth=*/0);
    if (!rootValue) {
      return fail(rootValue.error().code,
                  std::move(rootValue.error().message));
    }
    Result<ConfigModel> model = ConfigModel::fromValue(std::move(*rootValue));
    if (!model) {
      // Unreachable: the traversal already enforced fromValue's
      // preconditions.
      return fail(ErrorCode::ParseError, std::move(model.error().message));
    }
    return VersionedConfig{*version, std::move(*model)};
  } catch (const std::bad_alloc&) {
    throw;  // memory exhaustion is not a recoverable config error (ADR-018)
  } catch (const std::exception& e) {
    return fail(ErrorCode::ParseError,
                std::string("XML load failed: ") + e.what());
  } catch (...) {
    return fail(ErrorCode::ParseError,
                "XML load failed with a non-standard exception");
  }
}

Result<void> XmlInterface::save(const VersionedConfig& config,
                                std::ostream& out) {
  try {
    // ADR-020's reserved-carrier conflict check is vacuous here: the carrier
    // is a root *attribute*, outside the model's key space, so a model
    // member named "version" is a plain child element.
    pugi::xml_document doc;
    pugi::xml_node root = checkAlloc(doc.append_child(kRootName));
    checkAlloc(checkAlloc(root.append_attribute(kVersionAttr))
                   .set_value(static_cast<unsigned long long>(config.version)));
    Result<void> appended = appendMembers(root, config.model.root());
    if (!appended) {
      return appended;
    }
    doc.save(out, "  ");
    if (!out) {
      return fail(ErrorCode::SerializationError,
                  "failed to write XML document to stream");
    }
    return {};
  } catch (const std::bad_alloc&) {
    throw;  // memory exhaustion is not a recoverable config error (ADR-018)
  } catch (const std::exception& e) {
    return fail(ErrorCode::SerializationError,
                std::string("XML save failed: ") + e.what());
  } catch (...) {
    return fail(ErrorCode::SerializationError,
                "XML save failed with a non-standard exception");
  }
}

}  // namespace configmanager

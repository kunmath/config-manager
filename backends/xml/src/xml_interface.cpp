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
constexpr unsigned int kParseFlags =
    pugi::parse_default | pugi::parse_ws_pcdata;

bool isPathAddressable(std::string_view key) {
  return !key.empty() && key.find_first_of(".[]") == std::string_view::npos;
}

bool isXmlWhitespace(std::string_view text) {
  return text.find_first_not_of(" \t\n\r") == std::string_view::npos;
}

bool isAsciiAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Conservative ASCII subset of the XML Name production (no ':', which would
// imply namespaces; no non-ASCII name characters). Rejecting an exotic but
// technically valid name only fails save(); accepting an invalid one would
// emit a malformed document.
bool isValidElementName(std::string_view name) {
  if (name.empty()) return false;
  if (!isAsciiAlpha(name.front()) && name.front() != '_') return false;
  for (const char c : name.substr(1)) {
    if (!isAsciiAlpha(c) && !isAsciiDigit(c) && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

// XML 1.0 cannot express control characters below 0x20 other than tab and
// newline: most are illegal even as character references, and a literal
// carriage return never survives the mandatory line-ending normalization of
// a conformant parse (pugixml's writer emits CR unescaped in text).
bool isXmlRepresentable(std::string_view text) {
  for (const unsigned char c : text) {
    if (c < 0x20 && c != '\t' && c != '\n') return false;
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

Result<ConfigValue> parseValue(const pugi::xml_node& element);

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

Result<ConfigValue> parseObjectMembers(const pugi::xml_node& element) {
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
        for (const auto& member : object.members()) {
          if (member.first == key) {
            return fail(ErrorCode::ParseError,
                        "repeated sibling element '" + key + "' under '" +
                            std::string(element.name()) + "'");
          }
        }
        Result<ConfigValue> value = parseValue(child);
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

Result<ConfigValue> parseArrayItems(const pugi::xml_node& element) {
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
        Result<ConfigValue> value = parseValue(child);
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

Result<ConfigValue> parseValue(const pugi::xml_node& element) {
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
      return parseObjectMembers(element);
    }
    Result<std::string> text = scalarText(element);
    if (!text) {
      return fail(text.error().code, std::move(text.error().message));
    }
    return ConfigValue::of(std::move(*text));
  }

  const std::string_view typeName(type);
  if (typeName == "object") return parseObjectMembers(element);
  if (typeName == "array") return parseArrayItems(element);
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
  element.append_child(pugi::node_pcdata).set_value(text.c_str());
}

Result<void> appendValueElement(pugi::xml_node parent, const std::string& name,
                                const ConfigNode& node) {
  if (!isValidElementName(name)) {
    return fail(ErrorCode::SerializationError,
                "model key '" + name +
                    "' is not a valid XML element name (this backend admits "
                    "ASCII names matching [A-Za-z_][A-Za-z0-9_-]*)");
  }
  pugi::xml_node element = parent.append_child(name.c_str());
  switch (node.type()) {
    case NodeType::Null:
      element.append_attribute(kTypeAttr).set_value("null");
      return {};
    case NodeType::Bool:
      element.append_attribute(kTypeAttr).set_value("bool");
      setElementText(element, node.as<bool>().value() ? "true" : "false");
      return {};
    case NodeType::Int:
      element.append_attribute(kTypeAttr).set_value("int");
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
      element.append_attribute(kTypeAttr).set_value("double");
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
        element.append_attribute(kTypeAttr).set_value("object");
        return {};
      }
      return appendMembers(element, node);
    case NodeType::Array:
      element.append_attribute(kTypeAttr).set_value("array");
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
    // pugixml is lenient about trailing content after the root element.
    for (const pugi::xml_node& child : doc.children()) {
      if (child.type() == pugi::node_element && child != root) {
        return fail(ErrorCode::ParseError,
                    "document has more than one root element");
      }
    }
    if (std::string_view(root.name()) != kRootName) {
      return fail(ErrorCode::ParseError,
                  "document root must be <config>, got <" +
                      std::string(root.name()) + ">");
    }

    std::optional<VersionId> version;
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

    Result<ConfigValue> rootValue = parseObjectMembers(root);
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
    pugi::xml_node root = doc.append_child(kRootName);
    root.append_attribute(kVersionAttr)
        .set_value(static_cast<unsigned long long>(config.version));
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

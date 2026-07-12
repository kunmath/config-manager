#include "configmanager/backends/json_interface.hpp"

#include <cassert>
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
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "configmanager/config_model.hpp"
#include "configmanager/config_node.hpp"
#include "configmanager/config_value.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {
namespace {

using OrderedJson = nlohmann::ordered_json;

constexpr char kVersionKey[] = "__version";
constexpr std::uint64_t kMaxVersion =
    std::numeric_limits<VersionId>::max();
constexpr std::uint64_t kMaxInt =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

bool isPathAddressable(const std::string& key) {
  return !key.empty() && key.find_first_of(".[]") == std::string::npos;
}

// SAX handler that builds a ConfigValue tree directly. A DOM parse cannot
// enforce the boundary's rules — nlohmann silently drops duplicate keys and
// widens overflowing integer literals — so the backend intercepts events:
// duplicate keys and non-addressable keys (ADR-021) fail ParseError, numbers
// outside std::int64_t fail ParseError (§6.1), and the top-level "__version"
// carrier is consumed in place (ADR-020) rather than entering the tree.
class ValueBuilder : public nlohmann::json_sax<OrderedJson> {
 public:
  bool null() override { return onScalar(ConfigValue()); }

  bool boolean(bool val) override { return onScalar(ConfigValue::of(val)); }

  bool number_integer(number_integer_t val) override {
    // nlohmann routes non-negative integers to number_unsigned; only
    // negative values arrive here, and they always fit std::int64_t.
    if (stack_.empty()) return rejectNonObjectRoot();
    if (consumeCarrierFlag()) {
      return setError(ErrorCode::InvalidVersion,
                      "version carrier must not be negative");
    }
    return attach(ConfigValue::of(val));
  }

  bool number_unsigned(number_unsigned_t val) override {
    if (stack_.empty()) return rejectNonObjectRoot();
    if (consumeCarrierFlag()) {
      if (val > kMaxVersion) {
        return setError(ErrorCode::InvalidVersion,
                        "version carrier value " + std::to_string(val) +
                            " is not representable in VersionId");
      }
      version_ = static_cast<VersionId>(val);
      return true;
    }
    if (val > kMaxInt) {
      return setError(ErrorCode::ParseError,
                      "JSON number " + std::to_string(val) +
                          " is outside std::int64_t's range");
    }
    return attach(ConfigValue::of(static_cast<std::int64_t>(val)));
  }

  bool number_float(number_float_t val, const string_t& s) override {
    if (stack_.empty()) return rejectNonObjectRoot();
    if (consumeCarrierFlag()) {
      return setError(ErrorCode::InvalidVersion,
                      "version carrier must be a native unsigned integer, "
                      "got number '" +
                          s + "'");
    }
    if (s.find_first_of(".eE") == std::string::npos) {
      // An integral literal reaches the float callback only by overflowing
      // the integer types.
      return setError(ErrorCode::ParseError,
                      "integral JSON number '" + s +
                          "' is outside std::int64_t's range");
    }
    return attach(ConfigValue::of(val));
  }

  bool string(string_t& val) override {
    return onScalar(ConfigValue::of(std::move(val)));
  }

  bool binary(binary_t& /*val*/) override {
    return true;  // unreachable for text JSON input
  }

  bool start_object(std::size_t /*elements*/) override {
    if (consumeCarrierFlag()) {
      return setError(ErrorCode::InvalidVersion,
                      "version carrier must be a native unsigned integer, "
                      "got an object");
    }
    if (!checkContainerDepth()) return false;
    stack_.push_back(Frame{ConfigValue::object(), {}});
    return true;
  }

  bool key(string_t& val) override {
    if (!isPathAddressable(val)) {
      return setError(ErrorCode::ParseError,
                      "object key '" + val +
                          "' is not path-addressable (empty or contains "
                          "'.', '[', ']')");
    }
    Frame& top = stack_.back();
    if (stack_.size() == 1 && val == kVersionKey) {
      if (sawVersionKey_) {
        return setError(ErrorCode::ParseError,
                        "duplicate object key '__version'");
      }
      sawVersionKey_ = true;
      nextIsCarrier_ = true;
      return true;
    }
    for (const auto& member : top.container.members()) {
      if (member.first == val) {
        return setError(ErrorCode::ParseError,
                        "duplicate object key '" + val + "'");
      }
    }
    top.pendingKey = std::move(val);
    return true;
  }

  bool end_object() override { return popFrame(); }

  bool start_array(std::size_t /*elements*/) override {
    if (stack_.empty()) return rejectNonObjectRoot();
    if (consumeCarrierFlag()) {
      return setError(ErrorCode::InvalidVersion,
                      "version carrier must be a native unsigned integer, "
                      "got an array");
    }
    if (!checkContainerDepth()) return false;
    stack_.push_back(Frame{ConfigValue::array(), {}});
    return true;
  }

  bool end_array() override { return popFrame(); }

  bool parse_error(std::size_t /*position*/, const std::string& /*last_token*/,
                   const nlohmann::detail::exception& ex) override {
    return setError(ErrorCode::ParseError, ex.what());
  }

  bool sawVersion() const { return version_.has_value(); }
  VersionId version() const { return *version_; }

  ConfigValue takeRoot() {
    assert(haveRoot_ && "takeRoot() requires a successful parse");
    return std::move(root_);
  }

  Error takeError() {
    if (error_) {
      return std::move(*error_);
    }
    return Error{ErrorCode::ParseError, "JSON parse failed"};
  }

 private:
  struct Frame {
    ConfigValue container;
    std::string pendingKey;  // objects: set by key(), consumed by attach()
  };

  bool setError(ErrorCode code, std::string message) {
    error_ = Error{code, std::move(message)};
    return false;
  }

  bool rejectNonObjectRoot() {
    return setError(ErrorCode::ParseError,
                    "JSON document root is not an object");
  }

  // The carrier flag covers exactly the next value event after the top-level
  // "__version" key; every value callback consumes it.
  bool consumeCarrierFlag() {
    const bool wasCarrier = nextIsCarrier_;
    nextIsCarrier_ = false;
    return wasCarrier;
  }

  bool onScalar(ConfigValue value) {
    if (stack_.empty()) return rejectNonObjectRoot();
    if (consumeCarrierFlag()) {
      return setError(ErrorCode::InvalidVersion,
                      "version carrier must be a native unsigned integer");
    }
    return attach(std::move(value));
  }

  // A container about to be opened sits at depth stack_.size() (the root
  // object opens at 0). Depth-limit errors fire on the way down, before the
  // deep value is ever built (§4.4).
  bool checkContainerDepth() {
    if (stack_.size() > kMaxTreeDepth) {
      return setError(ErrorCode::ParseError,
                      "document exceeds the maximum nesting depth (" +
                          std::to_string(kMaxTreeDepth) + ")");
    }
    return true;
  }

  // An attached node sits at depth stack_.size(); completed containers were
  // already depth-checked when they opened.
  bool attach(ConfigValue value) {
    if (stack_.size() > kMaxTreeDepth) {
      return setError(ErrorCode::ParseError,
                      "document exceeds the maximum nesting depth (" +
                          std::to_string(kMaxTreeDepth) + ")");
    }
    Frame& top = stack_.back();
    if (top.container.type() == NodeType::Object) {
      top.container.set(std::move(top.pendingKey), std::move(value));
    } else {
      top.container.push(std::move(value));
    }
    return true;
  }

  bool popFrame() {
    ConfigValue done = std::move(stack_.back().container);
    stack_.pop_back();
    if (stack_.empty()) {
      root_ = std::move(done);
      haveRoot_ = true;
      return true;
    }
    return attach(std::move(done));
  }

  std::vector<Frame> stack_;
  ConfigValue root_;
  bool haveRoot_ = false;
  std::optional<VersionId> version_;
  bool sawVersionKey_ = false;
  bool nextIsCarrier_ = false;
  std::optional<Error> error_;
};

// The Result unwraps below are internal invariants, not fallible boundaries:
// the traversal only requests the type the node reports, on handles taken
// from a live model. value() throwing would be a library bug and is caught
// by save()'s catch-all (ADR-018). The one fallible case is a non-finite
// Double: JSON has no literal for it and nlohmann would silently emit null,
// so it fails SerializationError instead. `name` is the member key or array
// index, for diagnostics only.
Result<OrderedJson> toJson(const ConfigNode& node, const std::string& name) {
  switch (node.type()) {
    case NodeType::Null:
      return OrderedJson(nullptr);
    case NodeType::Bool:
      return OrderedJson(node.as<bool>().value());
    case NodeType::Int:
      return OrderedJson(node.as<std::int64_t>().value());
    case NodeType::Double: {
      const double value = node.as<double>().value();
      if (!std::isfinite(value)) {
        return fail(ErrorCode::SerializationError,
                    "non-finite double at '" + name +
                        "' is unrepresentable in JSON");
      }
      return OrderedJson(value);
    }
    case NodeType::String:
      return OrderedJson(node.as<std::string>().value());
    case NodeType::Object: {
      OrderedJson object = OrderedJson::object();
      const std::vector<std::string> keys = node.keys().value();
      for (const std::string& key : keys) {
        Result<OrderedJson> child = toJson(node.child(key).value(), key);
        if (!child) {
          return child;
        }
        object[key] = std::move(*child);
      }
      return object;
    }
    case NodeType::Array: {
      OrderedJson array = OrderedJson::array();
      for (std::size_t i = 0; i < node.size(); ++i) {
        Result<OrderedJson> element =
            toJson(node.at(i).value(), std::to_string(i));
        if (!element) {
          return element;
        }
        array.push_back(std::move(*element));
      }
      return array;
    }
  }
  assert(false && "unreachable: exhaustive NodeType switch");
  return OrderedJson(nullptr);
}

}  // namespace

Result<VersionedConfig> JsonInterface::load(std::istream& in) {
  try {
    ValueBuilder builder;
    if (!OrderedJson::sax_parse(in, &builder)) {
      Error error = builder.takeError();
      return fail(error.code, std::move(error.message));
    }
    if (!builder.sawVersion()) {
      return fail(ErrorCode::InvalidVersion,
                  "stream carries no version metadata ('__version')");
    }
    Result<ConfigModel> model = ConfigModel::fromValue(builder.takeRoot());
    if (!model) {
      // Unreachable: the builder already enforced fromValue's preconditions.
      return fail(ErrorCode::ParseError, std::move(model.error().message));
    }
    return VersionedConfig{builder.version(), std::move(*model)};
  } catch (const std::bad_alloc&) {
    throw;  // memory exhaustion is not a recoverable config error (ADR-018)
  } catch (const std::exception& e) {
    return fail(ErrorCode::ParseError,
                std::string("JSON load failed: ") + e.what());
  } catch (...) {
    return fail(ErrorCode::ParseError,
                "JSON load failed with a non-standard exception");
  }
}

Result<void> JsonInterface::save(const VersionedConfig& config,
                                 std::ostream& out) {
  try {
    if (config.model.contains(kVersionKey)) {
      return fail(ErrorCode::SerializationError,
                  "model contains the reserved version carrier '__version' "
                  "(ADR-020)");
    }
    OrderedJson doc = OrderedJson::object();
    doc[kVersionKey] = config.version;
    const ConfigNode root = config.model.root();
    const std::vector<std::string> keys = root.keys().value();
    for (const std::string& key : keys) {
      Result<OrderedJson> child = toJson(root.child(key).value(), key);
      if (!child) {
        return fail(child.error().code, std::move(child.error().message));
      }
      doc[key] = std::move(*child);
    }
    out << doc.dump(2) << '\n';
    if (!out) {
      return fail(ErrorCode::SerializationError,
                  "failed to write JSON document to stream");
    }
    return {};
  } catch (const std::bad_alloc&) {
    throw;  // memory exhaustion is not a recoverable config error (ADR-018)
  } catch (const std::exception& e) {
    return fail(ErrorCode::SerializationError,
                std::string("JSON save failed: ") + e.what());
  } catch (...) {
    return fail(ErrorCode::SerializationError,
                "JSON save failed with a non-standard exception");
  }
}

}  // namespace configmanager

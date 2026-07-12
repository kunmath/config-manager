#ifndef CONFIGMANAGER_CONFIG_VALUE_HPP_
#define CONFIGMANAGER_CONFIG_VALUE_HPP_

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace configmanager {

enum class NodeType {
  Null,
  Bool,
  Int,     // std::int64_t
  Double,
  String,
  Object,  // ordered key -> child
  Array,   // ordered child list
};

using Scalar = std::variant<std::monostate,  // Null
                            bool,
                            std::int64_t,
                            double,
                            std::string>;

// Recursive, value-semantic configuration tree. Used by default factories and
// for inserting subtrees into a ConfigModel; the model itself stores nodes in
// an arena, not ConfigValue.
//
// set()/push() are builder preconditions, not fallible operations: set()
// requires an Object, push() requires an Array. Violating a precondition is a
// programming error (debug assertion), not a recoverable failure. of()'s two
// data-dependent preconditions throw a logic error in every build (see
// below). Key validity (ADR-021) is enforced where a value crosses into a
// model, not here.
class ConfigValue {
 public:
  ConfigValue() = default;  // Null

  static ConfigValue object();
  static ConfigValue array();

  // Accepts an explicit supported set only: an unconstrained fallback would
  // silently stringify arbitrary types (or narrow long double), and any
  // failure of these constructions is a compile error or a thrown logic
  // error, never a silently corrupted value. The two data-dependent
  // preconditions — an unsigned value beyond Int's range, a null C string —
  // throw in every build: inside a default factory the catalog boundary maps
  // the exception to MigrationFailed (ADR-018), and ConfigModel::set checks
  // both before calling of(), so neither throw crosses the Result API.
  template <typename T>
  static ConfigValue of(T scalar) {
    constexpr bool kIsString =
        std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
        std::is_same_v<T, const char*> || std::is_same_v<T, char*>;
    static_assert(std::is_integral_v<T> || std::is_same_v<T, float> ||
                      std::is_same_v<T, double> || kIsString,
                  "ConfigValue::of supports bool, standard integer types, "
                  "float, double, std::string, std::string_view, and C "
                  "strings only");
    ConfigValue value;
    if constexpr (std::is_same_v<T, bool>) {
      value.type_ = NodeType::Bool;
      value.scalar_ = scalar;
    } else if constexpr (std::is_integral_v<T>) {
      if constexpr (std::is_unsigned_v<T>) {
        // Int is stored as std::int64_t: a larger value would silently wrap.
        if (static_cast<std::uint64_t>(scalar) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
          throw std::out_of_range(
              "ConfigValue::of: unsigned value exceeds Int range");
        }
      }
      value.type_ = NodeType::Int;
      value.scalar_ = static_cast<std::int64_t>(scalar);
    } else if constexpr (std::is_floating_point_v<T>) {
      value.type_ = NodeType::Double;
      value.scalar_ = static_cast<double>(scalar);
    } else {
      if constexpr (std::is_pointer_v<T>) {
        if (scalar == nullptr) {
          throw std::invalid_argument(
              "ConfigValue::of: C string must not be null");
        }
      }
      value.type_ = NodeType::String;
      value.scalar_ = std::string(std::move(scalar));
    }
    return value;
  }

  NodeType type() const noexcept { return type_; }

  // Object builder. Replaces an existing key's value in place, preserving the
  // member's position (ADR-022); otherwise appends in insertion order.
  ConfigValue& set(std::string key, ConfigValue child);

  // Array builder. Appends.
  ConfigValue& push(ConfigValue child);

  const Scalar& scalar() const noexcept { return scalar_; }
  const std::vector<std::pair<std::string, ConfigValue>>& members()
      const noexcept {
    return object_;
  }
  const std::vector<ConfigValue>& elements() const noexcept { return array_; }

 private:
  NodeType type_ = NodeType::Null;
  Scalar scalar_;
  std::vector<std::pair<std::string, ConfigValue>> object_;  // insertion-ordered
  std::vector<ConfigValue> array_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_CONFIG_VALUE_HPP_

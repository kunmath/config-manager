#ifndef CONFIGMANAGER_CONFIG_NODE_HPP_
#define CONFIGMANAGER_CONFIG_NODE_HPP_

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "configmanager/config_value.hpp"
#include "configmanager/result.hpp"

namespace configmanager {

class NodeArena;  // internal storage (src/node_arena.hpp)
class ConfigModel;

namespace detail {

template <typename T>
bool fitsInt(std::int64_t value) {
  if constexpr (std::is_signed_v<T>) {
    return value >= static_cast<std::int64_t>(std::numeric_limits<T>::min()) &&
           value <= static_cast<std::int64_t>(std::numeric_limits<T>::max());
  } else {
    return value >= 0 &&
           static_cast<std::uint64_t>(value) <= std::numeric_limits<T>::max();
  }
}

// Strict, lossless-only scalar conversion (docs/HighLevelDesign.md §4.4): Int and
// Double interconvert only when the exact value survives the round trip;
// Bool and String never convert; values are never stringified and strings
// are never parsed. Everything else is InvalidType.
template <typename T>
Result<T> convertScalar(const Scalar& scalar) {
  static_assert(std::is_same_v<T, std::string> || std::is_same_v<T, bool> ||
                    std::is_integral_v<T> || std::is_floating_point_v<T>,
                "as<T> supports bool, integral, floating-point, and "
                "std::string only");
  // The int64 range as doubles: -2^63 is exactly representable, 2^63 is the
  // first double above the maximum. Casting a double back to int64 is
  // defined only inside [-2^63, 2^63).
  constexpr double kInt64Lo = -9223372036854775808.0;
  constexpr double kInt64Hi = 9223372036854775808.0;

  if constexpr (std::is_same_v<T, bool>) {
    if (const bool* value = std::get_if<bool>(&scalar)) {
      return *value;
    }
    return fail(ErrorCode::InvalidType, "scalar does not hold a Bool");
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (const std::string* value = std::get_if<std::string>(&scalar)) {
      return *value;
    }
    return fail(ErrorCode::InvalidType, "scalar does not hold a String");
  } else if constexpr (std::is_integral_v<T>) {
    if (const std::int64_t* value = std::get_if<std::int64_t>(&scalar)) {
      if (fitsInt<T>(*value)) {
        return static_cast<T>(*value);
      }
      return fail(ErrorCode::InvalidType,
                  "Int value is not representable in the requested type");
    }
    if (const double* value = std::get_if<double>(&scalar)) {
      if constexpr (std::is_unsigned_v<T>) {
        // Unsigned targets admit exact values up to 2^64: [2^63, 2^64) is
        // out of Int's range but within "the range of the integer type".
        constexpr double kUint64Hi = 18446744073709551616.0;  // 2^64
        if (*value >= 0.0 && *value < kUint64Hi) {
          const auto integral = static_cast<std::uint64_t>(*value);
          if (static_cast<double>(integral) == *value &&
              integral <=
                  static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
            return static_cast<T>(integral);
          }
        }
      } else {
        if (*value >= kInt64Lo && *value < kInt64Hi) {
          const auto integral = static_cast<std::int64_t>(*value);
          if (static_cast<double>(integral) == *value && fitsInt<T>(integral)) {
            return static_cast<T>(integral);
          }
        }
      }
      return fail(ErrorCode::InvalidType,
                  "Double value is not an exactly representable integer in "
                  "the requested type");
    }
    return fail(ErrorCode::InvalidType, "scalar does not hold a number");
  } else {  // floating point
    if (const double* value = std::get_if<double>(&scalar)) {
      if constexpr (std::is_same_v<T, double>) {
        return *value;  // identity, including NaN and infinities
      }
      if (std::isnan(*value)) {
        return static_cast<T>(*value);  // NaN converts to NaN losslessly
      }
      // Out-of-range finite values must not reach the cast: narrowing a
      // double outside T's finite range is undefined behavior. Compared in
      // long double so T's limits only ever widen (converting a wider T's
      // limits to double would itself be out of range).
      if (std::isfinite(*value) &&
          (static_cast<long double>(*value) <
               static_cast<long double>(std::numeric_limits<T>::lowest()) ||
           static_cast<long double>(*value) >
               static_cast<long double>(std::numeric_limits<T>::max()))) {
        return fail(ErrorCode::InvalidType,
                    "Double value is outside the requested type's range");
      }
      const T narrowed = static_cast<T>(*value);
      if (static_cast<double>(narrowed) == *value) {
        return narrowed;
      }
      return fail(ErrorCode::InvalidType,
                  "Double value is not exactly representable in the "
                  "requested type");
    }
    if (const std::int64_t* value = std::get_if<std::int64_t>(&scalar)) {
      const T converted = static_cast<T>(*value);
      if (converted >= static_cast<T>(kInt64Lo) &&
          converted < static_cast<T>(kInt64Hi) &&
          static_cast<std::int64_t>(converted) == *value) {
        return converted;
      }
      return fail(ErrorCode::InvalidType,
                  "Int value is not exactly representable in the requested "
                  "type");
    }
    return fail(ErrorCode::InvalidType, "scalar does not hold a number");
  }
}

}  // namespace detail

// Lightweight read-only handle into a ConfigModel's node arena
// (docs/HighLevelDesign.md §4.3). Handles survive moves of the owning ConfigModel
// (they reference the heap arena, never the model object) and are detectably
// invalidated when their node is removed. Destroying the owning model —
// including move-assigning onto it — invalidates handles *undetectably*: no
// member, valid() included, may be called afterwards (the same contract as
// container iterators).
//
// The Result-returning accessors are total: a stale handle fails with
// NodeNotFound, a type mismatch with InvalidType. type() and size() are
// preconditioned on valid().
class ConfigNode {
 public:
  ConfigNode() = default;  // invalid handle: valid() returns false

  bool valid() const noexcept;

  // Preconditioned on valid() (debug assertion; otherwise undefined).
  NodeType type() const noexcept;
  // Member/element count for objects/arrays, 0 for scalars.
  std::size_t size() const noexcept;

  template <typename T>
  Result<T> as() const {
    Result<Scalar> scalar = scalarValue();
    if (!scalar) {
      return fail(scalar.error().code, std::move(scalar.error().message));
    }
    return detail::convertScalar<T>(*scalar);
  }

  Result<ConfigNode> child(std::string_view key) const;  // object member
  Result<ConfigNode> at(std::size_t index) const;        // array element
  Result<std::vector<std::string>> keys() const;         // object: in order

 private:
  friend class ConfigModel;

  ConfigNode(const NodeArena* arena, std::uint32_t id,
             std::uint32_t generation) noexcept
      : arena_(arena), id_(id), generation_(generation) {}

  // Stale handle -> NodeNotFound; Object/Array -> InvalidType.
  Result<Scalar> scalarValue() const;

  const NodeArena* arena_ = nullptr;  // stable across ConfigModel moves
  std::uint32_t id_ = 0xFFFFFFFFu;
  std::uint32_t generation_ = 0;  // must match the arena slot to be valid
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_CONFIG_NODE_HPP_

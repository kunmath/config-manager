#ifndef CONFIGMANAGER_CONFIG_MODEL_HPP_
#define CONFIGMANAGER_CONFIG_MODEL_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "configmanager/config_node.hpp"
#include "configmanager/config_path.hpp"
#include "configmanager/config_value.hpp"
#include "configmanager/result.hpp"

namespace configmanager {

class NodeArena;  // internal storage (src/node_arena.hpp)

// Maximum node depth in a model, with the root object at depth 0 (docs/
// HighLevelDesign.md §4.4). Several operations — subtree writes, extraction,
// destruction, repair, backend serialization — recurse over the tree, so
// depth must be bounded for them to be stack-safe: a deeper write fails with
// InvalidPath at the model boundary, and backends reject deeper persisted
// documents with ParseError before they reach a model.
inline constexpr std::size_t kMaxTreeDepth = 128;

// Owning configuration tree (docs/HighLevelDesign.md §4.4). The root is always an
// Object (ADR-020). Move-only: the model owns a heap arena that ConfigNode
// handles reference, so moving the model keeps every handle valid, while
// destroying it (or move-assigning onto it) invalidates them undetectably.
//
// Path-based access is the primary API; writes use upsert semantics with two
// invariants:
//   * Atomic (ADR-019): the whole path is validated before any mutation, so
//     a failed set() leaves the model unmodified.
//   * Type-preserving (ADR-019): upsert creates structure but never changes
//     an existing node's type — cross-type writes, including to the final
//     node, fail with InvalidType. Same-type writes update in place, keeping
//     handles to the target valid; a same-type subtree write replaces the
//     node's contents, detectably invalidating descendant handles.
class ConfigModel {
 public:
  ConfigModel();  // empty root object
  ~ConfigModel();
  ConfigModel(ConfigModel&&) noexcept;
  ConfigModel& operator=(ConfigModel&&) noexcept;
  ConfigModel(const ConfigModel&) = delete;
  ConfigModel& operator=(const ConfigModel&) = delete;

  // Adopts a value tree. A non-object root fails with InvalidType; an object
  // key that is not path-addressable — empty, or containing "." "[" "]" —
  // fails with InvalidPath (ADR-021).
  static Result<ConfigModel> fromValue(ConfigValue root);

  ConfigNode root() const;

  // ---- Path-based access (primary API) ----

  template <typename T>
  Result<T> get(std::string_view path) const {
    Result<ConfigNode> node = nodeAt(path);
    if (!node) {
      return fail(node.error().code, std::move(node.error().message));
    }
    return node->as<T>();
  }

  // Deep copy of the subtree at path.
  Result<ConfigValue> getValue(std::string_view path) const;

  // Scalar upsert. Constrained to exclude ConfigValue so subtree insertion
  // unambiguously selects the ConfigValue overload. ConfigValue::of's
  // preconditions are checked first and fail with InvalidType, so its throw
  // never crosses the Result API.
  template <typename T,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<T>, ConfigValue>>>
  Result<void> set(std::string_view path, T value) {
    if (const char* violation = ConfigValue::ofPreconditionViolation(value)) {
      return fail(ErrorCode::InvalidType, violation);
    }
    return set(path, ConfigValue::of(std::move(value)));
  }

  // Subtree upsert (defaults & migrations). Applies the same key
  // addressability check as fromValue to the inserted value (ADR-021).
  Result<void> set(std::string_view path, ConfigValue subtree);

  // Never fails: returns false for malformed paths as well as absent ones.
  bool contains(std::string_view path) const;

  // Detaches and frees the subtree at path, detectably invalidating handles
  // into it. Absent path -> NodeNotFound.
  Result<void> remove(std::string_view path);

  Result<ConfigNode> nodeAt(std::string_view path) const;

  // Deep copy of the arena (working copies for synchronize()).
  ConfigModel clone() const;

 private:
  Result<std::uint32_t> resolve(const ConfigPath& path) const;

  std::unique_ptr<NodeArena> arena_;
  std::uint32_t rootId_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_CONFIG_MODEL_HPP_

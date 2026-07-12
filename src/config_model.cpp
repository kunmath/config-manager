#include "configmanager/config_model.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "node_arena.hpp"

namespace configmanager {

namespace {

NodeId findMember(const Node& node, std::string_view key) {
  for (const auto& member : node.members) {
    if (member.first == key) {
      return member.second;
    }
  }
  return kNodeIdNone;
}

bool isAddressableKey(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  for (const char c : key) {
    if (c == '.' || c == '[' || c == ']') {
      return false;
    }
  }
  return true;
}

// ADR-021: keys that the path grammar cannot address (empty, or containing a
// reserved character) are rejected wherever a value crosses into a model.
// `depth` is where the value's root would sit in the model; nodes deeper
// than kMaxTreeDepth are rejected too. The depth check runs on the way
// down, so this validation itself never recurses past the limit, even on a
// hostile value tree.
Result<void> validateTree(const ConfigValue& value, std::size_t depth) {
  if (depth > kMaxTreeDepth) {
    return fail(ErrorCode::InvalidPath,
                "value exceeds the maximum nesting depth (" +
                    std::to_string(kMaxTreeDepth) + ")");
  }
  switch (value.type()) {
    case NodeType::Object:
      for (const auto& member : value.members()) {
        if (!isAddressableKey(member.first)) {
          return fail(ErrorCode::InvalidPath,
                      "object key \"" + member.first +
                          "\" is not path-addressable");
        }
        if (Result<void> nested = validateTree(member.second, depth + 1);
            !nested) {
          return nested;
        }
      }
      return {};
    case NodeType::Array:
      for (const auto& element : value.elements()) {
        if (Result<void> nested = validateTree(element, depth + 1); !nested) {
          return nested;
        }
      }
      return {};
    default:
      return {};
  }
}

void freeSubtree(NodeArena& arena, NodeId id);

NodeId buildFromValue(NodeArena& arena, const ConfigValue& value,
                      NodeId parent) {
  const NodeId id = arena.allocate(value.type());
  arena.get(id).parent = parent;
  try {
    switch (value.type()) {
      case NodeType::Object:
        arena.get(id).members.reserve(value.members().size());
        for (const auto& member : value.members()) {
          // Copy the key before building the child: with capacity reserved,
          // the emplace is then non-throwing, so a failed key copy cannot
          // strand an already-built subtree outside id's child list.
          std::string key = member.first;
          // allocate may grow the arena, so re-fetch the node by id each
          // time.
          const NodeId child = buildFromValue(arena, member.second, id);
          arena.get(id).members.emplace_back(std::move(key), child);
        }
        break;
      case NodeType::Array:
        arena.get(id).elements.reserve(value.elements().size());
        for (const auto& element : value.elements()) {
          const NodeId child = buildFromValue(arena, element, id);
          arena.get(id).elements.push_back(child);
        }
        break;
      default:
        arena.get(id).scalar = value.scalar();
        break;
    }
  } catch (...) {
    // std::bad_alloc mid-build (ADR-018 lets it propagate): release the
    // partial subtree so a failed set() cannot strand nodes in the arena.
    freeSubtree(arena, id);
    throw;
  }
  return id;
}

// Frees all descendants, detectably invalidating their handles; the node
// itself stays alive (its generation is untouched).
void freeChildren(NodeArena& arena, NodeId id) {
  const auto members = std::move(arena.get(id).members);
  const auto elements = std::move(arena.get(id).elements);
  arena.get(id).members.clear();
  arena.get(id).elements.clear();
  for (const auto& member : members) {
    freeSubtree(arena, member.second);
  }
  for (const NodeId element : elements) {
    freeSubtree(arena, element);
  }
}

void freeSubtree(NodeArena& arena, NodeId id) {
  freeChildren(arena, id);
  arena.release(id);
}

// Replaces target's contents with value's. Precondition: same NodeType. The
// target keeps its slot and generation, so handles to it stay valid;
// descendant handles are detectably invalidated.
//
// Replacements are built completely before the old contents are freed, so a
// std::bad_alloc mid-build (ADR-018 lets it propagate) leaves the tree
// exactly as it was — the atomic-write guarantee (ADR-019) holds under
// allocation failure too.
void assignContents(NodeArena& arena, NodeId target, const ConfigValue& value) {
  assert(arena.get(target).type == value.type());
  switch (value.type()) {
    case NodeType::Object: {
      std::vector<std::pair<std::string, NodeId>> members;
      members.reserve(value.members().size());
      try {
        for (const auto& member : value.members()) {
          // Copy the key before building the child: with capacity reserved,
          // the emplace itself is then non-throwing, so a failed key copy
          // cannot strand an already-built subtree.
          std::string key = member.first;
          const NodeId child = buildFromValue(arena, member.second, target);
          members.emplace_back(std::move(key), child);
        }
      } catch (...) {
        for (const auto& member : members) {
          freeSubtree(arena, member.second);
        }
        throw;
      }
      freeChildren(arena, target);
      arena.get(target).members = std::move(members);
      break;
    }
    case NodeType::Array: {
      std::vector<NodeId> elements;
      elements.reserve(value.elements().size());
      try {
        for (const auto& element : value.elements()) {
          elements.push_back(buildFromValue(arena, element, target));
        }
      } catch (...) {
        for (const NodeId element : elements) {
          freeSubtree(arena, element);
        }
        throw;
      }
      freeChildren(arena, target);
      arena.get(target).elements = std::move(elements);
      break;
    }
    default: {
      Scalar replacement = value.scalar();  // may throw; target untouched
      arena.get(target).scalar = std::move(replacement);
      break;
    }
  }
}

ConfigValue buildValue(const NodeArena& arena, NodeId id) {
  const Node& node = arena.get(id);
  switch (node.type) {
    case NodeType::Object: {
      ConfigValue value = ConfigValue::object();
      for (const auto& member : node.members) {
        value.set(member.first, buildValue(arena, member.second));
      }
      return value;
    }
    case NodeType::Array: {
      ConfigValue value = ConfigValue::array();
      for (const NodeId element : node.elements) {
        value.push(buildValue(arena, element));
      }
      return value;
    }
    case NodeType::Bool:
      return ConfigValue::of(std::get<bool>(node.scalar));
    case NodeType::Int:
      return ConfigValue::of(std::get<std::int64_t>(node.scalar));
    case NodeType::Double:
      return ConfigValue::of(std::get<double>(node.scalar));
    case NodeType::String:
      return ConfigValue::of(std::get<std::string>(node.scalar));
    case NodeType::Null:
      break;
  }
  return ConfigValue();  // Null
}

// Where a validated write mutates: when createFrom == segment count, `node`
// is the existing target (same-type replace); otherwise `node` is the
// deepest existing node and creation starts at segment createFrom.
struct WritePlan {
  NodeId node;
  std::size_t createFrom;
};

// Segments below the creation point descend through freshly created empty
// containers, so any index among them can only be 0 (holes are never
// fabricated).
Result<WritePlan> checkCreatable(const std::vector<PathSegment>& segments,
                                 std::size_t from, NodeId parent) {
  for (std::size_t j = from + 1; j < segments.size(); ++j) {
    const PathSegment& segment = segments[j];
    if (segment.kind == PathSegment::Kind::Index && segment.index != 0) {
      return fail(ErrorCode::NodeNotFound,
                  "index " + std::to_string(segment.index) +
                      " into a newly created array; only [0] can be appended");
    }
  }
  return WritePlan{parent, from};
}

// Validates the entire write against the existing tree before any mutation
// (ADR-019): a returned error guarantees the model is untouched.
Result<WritePlan> planWrite(const NodeArena& arena, NodeId rootId,
                            const std::vector<PathSegment>& segments,
                            NodeType finalType) {
  NodeId current = rootId;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const PathSegment& segment = segments[i];
    const Node& node = arena.get(current);
    NodeId child = kNodeIdNone;
    if (segment.kind == PathSegment::Kind::Key) {
      if (node.type != NodeType::Object) {
        return fail(ErrorCode::InvalidType,
                    "segment \"" + segment.key +
                        "\" traverses a non-object node");
      }
      child = findMember(node, segment.key);
      if (child == kNodeIdNone) {
        return checkCreatable(segments, i, current);
      }
    } else {
      if (node.type != NodeType::Array) {
        return fail(ErrorCode::InvalidType,
                    "index segment traverses a non-array node");
      }
      if (segment.index > node.elements.size()) {
        return fail(ErrorCode::NodeNotFound,
                    "array write index " + std::to_string(segment.index) +
                        " is past the end (size " +
                        std::to_string(node.elements.size()) + ")");
      }
      if (segment.index == node.elements.size()) {
        return checkCreatable(segments, i, current);  // append
      }
      child = node.elements[segment.index];
    }
    if (i + 1 == segments.size()) {
      // Final node exists: a write succeeds only when the new value's type
      // equals the node's current type (ADR-019). Cross-type writes must
      // remove first.
      if (arena.get(child).type != finalType) {
        return fail(ErrorCode::InvalidType,
                    "write would change the type of the existing node; "
                    "remove it first");
      }
      return WritePlan{child, segments.size()};
    }
    current = child;
  }
  // Unreachable: the grammar guarantees at least one segment.
  return WritePlan{current, segments.size()};
}

// The created chain is built detached and joined to the existing tree only
// as the final, non-throwing step, so a std::bad_alloc mid-build (ADR-018
// lets it propagate) leaves the tree exactly as it was (ADR-019).
void applyWrite(NodeArena& arena, const WritePlan& plan,
                const std::vector<PathSegment>& segments,
                const ConfigValue& value) {
  if (plan.createFrom == segments.size()) {
    assignContents(arena, plan.node, value);
    return;
  }
  NodeId head = kNodeIdNone;  // first created node, still detached
  try {
    NodeId tail = kNodeIdNone;
    NodeId parent = plan.node;
    for (std::size_t j = plan.createFrom; j < segments.size(); ++j) {
      const bool isFinal = (j + 1 == segments.size());
      // For links inside the chain, copy the key before building the child
      // (see assignContents): the emplace below is then non-throwing.
      std::string key;
      if (tail != kNodeIdNone &&
          segments[j].kind == PathSegment::Kind::Key) {
        key = segments[j].key;
        arena.get(tail).members.reserve(1);
      } else if (tail != kNodeIdNone) {
        arena.get(tail).elements.reserve(1);
      }
      NodeId child;
      if (isFinal) {
        child = buildFromValue(arena, value, parent);
      } else {
        // Intermediate type is determined by the next segment: a key needs
        // an Object to live in, an index needs an Array.
        const NodeType type = segments[j + 1].kind == PathSegment::Kind::Key
                                  ? NodeType::Object
                                  : NodeType::Array;
        child = arena.allocate(type);
        arena.get(child).parent = parent;
      }
      if (tail == kNodeIdNone) {
        head = child;
      } else if (segments[j].kind == PathSegment::Kind::Key) {
        arena.get(tail).members.emplace_back(std::move(key), child);
      } else {
        arena.get(tail).elements.push_back(child);
      }
      tail = child;
      parent = child;
    }
    // Join point: everything fallible happens before the emplace, which
    // runs with reserved capacity and a prebuilt key.
    Node& anchor = arena.get(plan.node);
    if (segments[plan.createFrom].kind == PathSegment::Kind::Key) {
      std::string key = segments[plan.createFrom].key;
      anchor.members.reserve(anchor.members.size() + 1);
      anchor.members.emplace_back(std::move(key), head);
    } else {
      anchor.elements.reserve(anchor.elements.size() + 1);
      anchor.elements.push_back(head);
    }
  } catch (...) {
    if (head != kNodeIdNone) {
      freeSubtree(arena, head);
    }
    throw;
  }
}

}  // namespace

ConfigModel::ConfigModel() : arena_(std::make_unique<NodeArena>()) {
  rootId_ = arena_->allocate(NodeType::Object);
}

ConfigModel::~ConfigModel() = default;
ConfigModel::ConfigModel(ConfigModel&&) noexcept = default;
ConfigModel& ConfigModel::operator=(ConfigModel&&) noexcept = default;

Result<ConfigModel> ConfigModel::fromValue(ConfigValue root) {
  if (root.type() != NodeType::Object) {
    return fail(ErrorCode::InvalidType, "model root must be an Object");
  }
  if (Result<void> keys = validateTree(root, /*depth=*/0); !keys) {
    return fail(keys.error().code, std::move(keys.error().message));
  }
  ConfigModel model;
  assignContents(*model.arena_, model.rootId_, root);
  return model;
}

ConfigNode ConfigModel::root() const {
  return ConfigNode(arena_.get(), rootId_, arena_->get(rootId_).generation);
}

Result<ConfigValue> ConfigModel::getValue(std::string_view path) const {
  Result<ConfigPath> parsed = ConfigPath::parse(path);
  if (!parsed) {
    return fail(parsed.error().code, std::move(parsed.error().message));
  }
  Result<std::uint32_t> id = resolve(*parsed);
  if (!id) {
    return fail(id.error().code, std::move(id.error().message));
  }
  return buildValue(*arena_, *id);
}

Result<void> ConfigModel::set(std::string_view path, ConfigValue subtree) {
  Result<ConfigPath> parsed = ConfigPath::parse(path);
  if (!parsed) {
    return fail(parsed.error().code, std::move(parsed.error().message));
  }
  // The written subtree's root lands at depth segments-count, so the whole
  // write stays within kMaxTreeDepth exactly when the value tree, offset by
  // that depth, does.
  if (Result<void> keys = validateTree(subtree, parsed->segments().size());
      !keys) {
    return keys;
  }
  Result<WritePlan> plan =
      planWrite(*arena_, rootId_, parsed->segments(), subtree.type());
  if (!plan) {
    return fail(plan.error().code, std::move(plan.error().message));
  }
  applyWrite(*arena_, *plan, parsed->segments(), subtree);
  return {};
}

bool ConfigModel::contains(std::string_view path) const {
  Result<ConfigPath> parsed = ConfigPath::parse(path);
  if (!parsed) {
    return false;
  }
  return resolve(*parsed).has_value();
}

Result<void> ConfigModel::remove(std::string_view path) {
  Result<ConfigPath> parsed = ConfigPath::parse(path);
  if (!parsed) {
    return fail(parsed.error().code, std::move(parsed.error().message));
  }
  Result<std::uint32_t> target = resolve(*parsed);
  if (!target) {
    return fail(target.error().code, std::move(target.error().message));
  }
  // The grammar guarantees at least one segment, so the target is never the
  // root and always has a live parent.
  const NodeId parent = arena_->get(*target).parent;
  Node& parentNode = arena_->get(parent);
  const PathSegment& last = parsed->segments().back();
  if (last.kind == PathSegment::Kind::Key) {
    for (auto it = parentNode.members.begin(); it != parentNode.members.end();
         ++it) {
      if (it->second == *target) {
        parentNode.members.erase(it);
        break;
      }
    }
  } else {
    parentNode.elements.erase(parentNode.elements.begin() +
                              static_cast<std::ptrdiff_t>(last.index));
  }
  freeSubtree(*arena_, *target);
  return {};
}

Result<ConfigNode> ConfigModel::nodeAt(std::string_view path) const {
  Result<ConfigPath> parsed = ConfigPath::parse(path);
  if (!parsed) {
    return fail(parsed.error().code, std::move(parsed.error().message));
  }
  Result<std::uint32_t> id = resolve(*parsed);
  if (!id) {
    return fail(id.error().code, std::move(id.error().message));
  }
  return ConfigNode(arena_.get(), *id, arena_->get(*id).generation);
}

ConfigModel ConfigModel::clone() const {
  ConfigModel copy;
  copy.arena_ = std::make_unique<NodeArena>(*arena_);
  copy.rootId_ = rootId_;
  return copy;
}

Result<std::uint32_t> ConfigModel::resolve(const ConfigPath& path) const {
  NodeId current = rootId_;
  for (const PathSegment& segment : path.segments()) {
    const Node& node = arena_->get(current);
    if (segment.kind == PathSegment::Kind::Key) {
      if (node.type != NodeType::Object) {
        return fail(ErrorCode::InvalidType,
                    "segment \"" + segment.key +
                        "\" traverses a non-object node");
      }
      const NodeId child = findMember(node, segment.key);
      if (child == kNodeIdNone) {
        return fail(ErrorCode::NodeNotFound,
                    "no member \"" + segment.key + "\"");
      }
      current = child;
    } else {
      if (node.type != NodeType::Array) {
        return fail(ErrorCode::InvalidType,
                    "index segment traverses a non-array node");
      }
      if (segment.index >= node.elements.size()) {
        return fail(ErrorCode::NodeNotFound,
                    "index " + std::to_string(segment.index) +
                        " out of range (size " +
                        std::to_string(node.elements.size()) + ")");
      }
      current = node.elements[segment.index];
    }
  }
  return current;
}

}  // namespace configmanager

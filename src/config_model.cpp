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

// Owns a detached subtree until dismiss(): if building throws (ADR-018 lets
// std::bad_alloc propagate), the destructor frees the partial subtree so a
// failed write cannot strand nodes in the arena.
class SubtreeGuard {
 public:
  SubtreeGuard(NodeArena& arena, NodeId id) : arena_(&arena), id_(id) {}
  SubtreeGuard(const SubtreeGuard&) = delete;
  SubtreeGuard& operator=(const SubtreeGuard&) = delete;
  ~SubtreeGuard() {
    if (id_ != kNodeIdNone) {
      freeSubtree(*arena_, id_);
    }
  }

  void reset(NodeId id) noexcept { id_ = id; }
  void dismiss() noexcept { id_ = kNodeIdNone; }

 private:
  NodeArena* arena_;
  NodeId id_;
};

NodeId buildFromValue(NodeArena& arena, const ConfigValue& value,
                      NodeId parent) {
  const NodeId id = arena.allocate(value.type());
  arena.get(id).parent = parent;
  SubtreeGuard guard(arena, id);
  // allocate may grow the arena, so re-fetch nodes by id after every build.
  switch (value.type()) {
    case NodeType::Object:
      arena.get(id).members.reserve(value.members().size());
      for (const auto& member : value.members()) {
        const NodeId child = buildFromValue(arena, member.second, id);
        SubtreeGuard childGuard(arena, child);  // owns child until it is linked
        arena.get(id).members.emplace_back(member.first, child);
        childGuard.dismiss();
      }
      break;
    case NodeType::Array:
      arena.get(id).elements.reserve(value.elements().size());
      for (const auto& element : value.elements()) {
        const NodeId child = buildFromValue(arena, element, id);
        SubtreeGuard childGuard(arena, child);
        arena.get(id).elements.push_back(child);
        childGuard.dismiss();
      }
      break;
    default:
      arena.get(id).scalar = value.scalar();
      break;
  }
  guard.dismiss();
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
// descendant handles are detectably invalidated. The replacement is built
// fully detached first, and every splice step after it is non-throwing, so a
// std::bad_alloc mid-build (ADR-018 lets it propagate) leaves the tree
// exactly as it was — the atomic-write guarantee (ADR-019) holds under
// allocation failure too.
void assignContents(NodeArena& arena, NodeId target, const ConfigValue& value) {
  assert(arena.get(target).type == value.type());
  const NodeId built = buildFromValue(arena, value, kNodeIdNone);
  freeChildren(arena, target);
  Node& builtNode = arena.get(built);
  Node& targetNode = arena.get(target);
  targetNode.scalar = std::move(builtNode.scalar);
  targetNode.members = std::move(builtNode.members);
  targetNode.elements = std::move(builtNode.elements);
  for (const auto& member : targetNode.members) {
    arena.get(member.second).parent = target;
  }
  for (const NodeId element : targetNode.elements) {
    arena.get(element).parent = target;
  }
  arena.release(built);
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

// The created chain is assembled detached, bottom-up, and joined to the
// existing tree as the final step. A std::bad_alloc anywhere (ADR-018 lets
// it propagate) unwinds through SubtreeGuard, leaving the tree exactly as it
// was (ADR-019): the join emplace has the strong guarantee, so the anchor is
// untouched on failure.
void applyWrite(NodeArena& arena, const WritePlan& plan,
                const std::vector<PathSegment>& segments,
                const ConfigValue& value) {
  if (plan.createFrom == segments.size()) {
    assignContents(arena, plan.node, value);
    return;
  }
  NodeId chain = buildFromValue(arena, value, kNodeIdNone);
  SubtreeGuard guard(arena, chain);
  for (std::size_t j = segments.size() - 1; j > plan.createFrom; --j) {
    // The container addressed by segments[j - 1]: a key segment needs an
    // Object to live in, an index segment an Array.
    const NodeType type = segments[j].kind == PathSegment::Kind::Key
                              ? NodeType::Object
                              : NodeType::Array;
    const NodeId container = arena.allocate(type);
    SubtreeGuard containerGuard(arena, container);
    arena.get(chain).parent = container;
    if (segments[j].kind == PathSegment::Kind::Key) {
      arena.get(container).members.emplace_back(segments[j].key, chain);
    } else {
      arena.get(container).elements.push_back(chain);
    }
    containerGuard.dismiss();
    chain = container;
    guard.reset(chain);
  }
  arena.get(chain).parent = plan.node;
  Node& anchor = arena.get(plan.node);
  if (segments[plan.createFrom].kind == PathSegment::Kind::Key) {
    anchor.members.emplace_back(segments[plan.createFrom].key, chain);
  } else {
    anchor.elements.push_back(chain);
  }
  guard.dismiss();
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

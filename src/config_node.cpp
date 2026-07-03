#include "configmanager/config_node.hpp"

#include <cassert>
#include <string>

#include "node_arena.hpp"

namespace configmanager {

namespace {

const Node* findLive(const NodeArena* arena, std::uint32_t id,
                     std::uint32_t generation) {
  if (arena == nullptr || !arena->matches(id, generation)) {
    return nullptr;
  }
  return &arena->get(id);
}

}  // namespace

bool ConfigNode::valid() const noexcept {
  return arena_ != nullptr && arena_->matches(id_, generation_);
}

NodeType ConfigNode::type() const noexcept {
  assert(valid() && "ConfigNode::type requires a valid handle");
  return arena_->get(id_).type;
}

std::size_t ConfigNode::size() const noexcept {
  assert(valid() && "ConfigNode::size requires a valid handle");
  const Node& node = arena_->get(id_);
  switch (node.type) {
    case NodeType::Object:
      return node.members.size();
    case NodeType::Array:
      return node.elements.size();
    default:
      return 0;
  }
}

Result<ConfigNode> ConfigNode::child(std::string_view key) const {
  const Node* node = findLive(arena_, id_, generation_);
  if (node == nullptr) {
    return fail(ErrorCode::NodeNotFound, "stale ConfigNode handle");
  }
  if (node->type != NodeType::Object) {
    return fail(ErrorCode::InvalidType, "child() requires an Object node");
  }
  for (const auto& member : node->members) {
    if (member.first == key) {
      return ConfigNode(arena_, member.second,
                        arena_->get(member.second).generation);
    }
  }
  return fail(ErrorCode::NodeNotFound,
              "no member \"" + std::string(key) + "\"");
}

Result<ConfigNode> ConfigNode::at(std::size_t index) const {
  const Node* node = findLive(arena_, id_, generation_);
  if (node == nullptr) {
    return fail(ErrorCode::NodeNotFound, "stale ConfigNode handle");
  }
  if (node->type != NodeType::Array) {
    return fail(ErrorCode::InvalidType, "at() requires an Array node");
  }
  if (index >= node->elements.size()) {
    return fail(ErrorCode::NodeNotFound,
                "index " + std::to_string(index) + " out of range (size " +
                    std::to_string(node->elements.size()) + ")");
  }
  const NodeId element = node->elements[index];
  return ConfigNode(arena_, element, arena_->get(element).generation);
}

Result<std::vector<std::string>> ConfigNode::keys() const {
  const Node* node = findLive(arena_, id_, generation_);
  if (node == nullptr) {
    return fail(ErrorCode::NodeNotFound, "stale ConfigNode handle");
  }
  if (node->type != NodeType::Object) {
    return fail(ErrorCode::InvalidType, "keys() requires an Object node");
  }
  std::vector<std::string> names;
  names.reserve(node->members.size());
  for (const auto& member : node->members) {
    names.push_back(member.first);
  }
  return names;
}

Result<Scalar> ConfigNode::scalarValue() const {
  const Node* node = findLive(arena_, id_, generation_);
  if (node == nullptr) {
    return fail(ErrorCode::NodeNotFound, "stale ConfigNode handle");
  }
  if (node->type == NodeType::Object || node->type == NodeType::Array) {
    return fail(ErrorCode::InvalidType, "node is not a scalar");
  }
  return node->scalar;
}

}  // namespace configmanager

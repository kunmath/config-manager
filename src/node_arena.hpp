#ifndef CONFIGMANAGER_SRC_NODE_ARENA_HPP_
#define CONFIGMANAGER_SRC_NODE_ARENA_HPP_

// Internal storage for ConfigModel (docs/HighLevelDesign.md §4.2). Not installed:
// public headers see NodeArena only as a forward declaration, so the storage
// layout can change without touching the API surface.

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "configmanager/config_value.hpp"

namespace configmanager {

using NodeId = std::uint32_t;
inline constexpr NodeId kNodeIdNone = 0xFFFFFFFFu;

struct Node {
  NodeType type = NodeType::Null;
  Scalar scalar;                                       // when scalar type
  std::vector<std::pair<std::string, NodeId>> members;  // when Object (ordered)
  std::vector<NodeId> elements;                         // when Array
  NodeId parent = kNodeIdNone;
  std::uint32_t generation = 0;  // bumped on free: stale handles are detectable
  bool alive = false;
};

// Flat arena of nodes addressed by stable NodeId. Slots are recycled through
// a free list; freeing a slot bumps its generation so any ConfigNode holding
// the old generation is detectably stale.
class NodeArena {
 public:
  NodeId allocate(NodeType type) {
    if (!freeList_.empty()) {
      // The generation was already bumped by release(), so every handle
      // into the slot's previous life is stale; reuse does not bump again.
      const NodeId id = freeList_.back();
      freeList_.pop_back();
      Node& node = nodes_[id];
      node.type = type;
      node.scalar = Scalar{};
      node.parent = kNodeIdNone;
      node.alive = true;
      return id;
    }
    Node node;
    node.type = type;
    node.alive = true;
    nodes_.push_back(std::move(node));
    return static_cast<NodeId>(nodes_.size() - 1);
  }

  void release(NodeId id) {
    Node& node = nodes_[id];
    assert(node.alive && "NodeArena::release requires a live node");
    node.alive = false;
    ++node.generation;
    node.scalar = Scalar{};
    node.members.clear();
    node.elements.clear();
    node.parent = kNodeIdNone;
    freeList_.push_back(id);
  }

  Node& get(NodeId id) {
    assert(id < nodes_.size() && nodes_[id].alive);
    return nodes_[id];
  }
  const Node& get(NodeId id) const {
    assert(id < nodes_.size() && nodes_[id].alive);
    return nodes_[id];
  }

  bool matches(NodeId id, std::uint32_t generation) const noexcept {
    return id < nodes_.size() && nodes_[id].alive &&
           nodes_[id].generation == generation;
  }

 private:
  std::vector<Node> nodes_;
  std::vector<NodeId> freeList_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_SRC_NODE_ARENA_HPP_

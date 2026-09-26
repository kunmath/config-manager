#include "configmanager/config_value.hpp"

#include <cassert>

namespace configmanager {

ConfigValue ConfigValue::object() {
  ConfigValue value;
  value.type_ = NodeType::Object;
  return value;
}

ConfigValue ConfigValue::array() {
  ConfigValue value;
  value.type_ = NodeType::Array;
  return value;
}

ConfigValue& ConfigValue::set(std::string key, ConfigValue child) {
  assert(type_ == NodeType::Object && "ConfigValue::set requires an Object");
  const auto existing = object_index_.find(key);
  if (existing != object_index_.end()) {
    object_[existing->second].second = std::move(child);
    return *this;
  }
  const auto inserted = object_index_.emplace(key, object_.size()).first;
  try {
    object_.emplace_back(std::move(key), std::move(child));
  } catch (...) {
    object_index_.erase(inserted);
    throw;
  }
  return *this;
}

bool ConfigValue::contains(const std::string& key) const {
  return object_index_.find(key) != object_index_.end();
}

ConfigValue& ConfigValue::push(ConfigValue child) {
  assert(type_ == NodeType::Array && "ConfigValue::push requires an Array");
  array_.push_back(std::move(child));
  return *this;
}

}  // namespace configmanager

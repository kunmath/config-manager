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
  for (auto& member : object_) {
    if (member.first == key) {
      member.second = std::move(child);
      return *this;
    }
  }
  object_.emplace_back(std::move(key), std::move(child));
  return *this;
}

ConfigValue& ConfigValue::push(ConfigValue child) {
  assert(type_ == NodeType::Array && "ConfigValue::push requires an Array");
  array_.push_back(std::move(child));
  return *this;
}

}  // namespace configmanager

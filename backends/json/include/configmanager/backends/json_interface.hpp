#ifndef CONFIGMANAGER_BACKENDS_JSON_INTERFACE_HPP_
#define CONFIGMANAGER_BACKENDS_JSON_INTERFACE_HPP_

#include "configmanager/config_interface.hpp"

namespace configmanager {

// JSON backend (HighLevelDesign.md §6.1): native 1:1 mapping via
// nlohmann::ordered_json, preserving member insertion order end-to-end
// (ADR-022). The version carrier is the top-level "__version" field; it must
// be a native unquoted unsigned integer representable in VersionId, anything
// else fails load() with InvalidVersion. Integral JSON numbers map to Int and
// all others to Double; a number outside std::int64_t's range fails load()
// with ParseError. Duplicate keys and non-path-addressable keys (ADR-021)
// fail load() with ParseError.
class JsonInterface : public IConfigInterface {
 public:
  Result<VersionedConfig> load(std::istream& in) override;
  Result<void> save(const VersionedConfig& config,
                    std::ostream& out) override;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_BACKENDS_JSON_INTERFACE_HPP_

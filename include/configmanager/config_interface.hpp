#ifndef CONFIGMANAGER_CONFIG_INTERFACE_HPP_
#define CONFIGMANAGER_CONFIG_INTERFACE_HPP_

#include <iosfwd>

#include "configmanager/result.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {

// Serialization boundary (HighLevelDesign.md §6). Backends only parse and
// serialize: load() never repairs or migrates (ADR-013). The version is
// mandatory — a stream without version metadata fails load() with
// InvalidVersion (ADR-014) — and the format's version carrier is reserved
// (ADR-020): load() consumes it (it never appears in the model) and save()
// writes it from VersionedConfig::version; a model that already contains the
// carrier fails save() with SerializationError. Backends are a non-throwing
// boundary (ADR-018): parser and stream exceptions map to ParseError in
// load() and SerializationError in save(), with std::bad_alloc rethrown.
class IConfigInterface {
 public:
  virtual ~IConfigInterface() = default;

  virtual Result<VersionedConfig> load(std::istream& in) = 0;
  virtual Result<void> save(const VersionedConfig& config,
                            std::ostream& out) = 0;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_CONFIG_INTERFACE_HPP_

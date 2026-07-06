#ifndef CONFIGMANAGER_VERSIONED_CONFIG_HPP_
#define CONFIGMANAGER_VERSIONED_CONFIG_HPP_

#include "configmanager/config_model.hpp"
#include "configmanager/version.hpp"

namespace configmanager {

// The unit that flows through load -> inspect -> synchronize -> save
// (docs/HighLevelDesign.md §5). `version` is the single source of truth for the
// configuration's version; the model never carries version metadata.
// Move-only (because ConfigModel is).
struct VersionedConfig {
  VersionId version;
  ConfigModel model;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_VERSIONED_CONFIG_HPP_

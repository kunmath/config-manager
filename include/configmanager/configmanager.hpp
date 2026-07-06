#ifndef CONFIGMANAGER_CONFIGMANAGER_HPP_
#define CONFIGMANAGER_CONFIGMANAGER_HPP_

// Umbrella include. Components are added here as they land
// (HighLevelDesign.md §13 implementation order).

#include "configmanager/config_interface.hpp"
#include "configmanager/config_model.hpp"
#include "configmanager/config_node.hpp"
#include "configmanager/config_path.hpp"
#include "configmanager/config_runtime.hpp"
#include "configmanager/config_value.hpp"
#include "configmanager/migration_engine.hpp"
#include "configmanager/migration_registry.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/version_catalog.hpp"
#include "configmanager/versioned_config.hpp"

namespace cfg = configmanager;

#endif  // CONFIGMANAGER_CONFIGMANAGER_HPP_

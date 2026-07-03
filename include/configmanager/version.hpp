#ifndef CONFIGMANAGER_VERSION_HPP_
#define CONFIGMANAGER_VERSION_HPP_

#include <cstdint>
#include <functional>

#include "configmanager/config_value.hpp"

namespace configmanager {

using VersionId = std::uint32_t;  // monotonic, application-defined

// Produces a fully-populated default tree for a version. The returned tree
// must be object-rooted and use path-addressable keys (ADR-021);
// VersionCatalog::createDefault adopts it via ConfigModel::fromValue. A
// factory that throws is caught and mapped to MigrationFailed (ADR-018).
using DefaultFactory = std::function<ConfigValue()>;

struct VersionArtifact {
  VersionId version;
  DefaultFactory defaultFactory;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_VERSION_HPP_

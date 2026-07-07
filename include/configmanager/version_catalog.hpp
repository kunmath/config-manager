#ifndef CONFIGMANAGER_VERSION_CATALOG_HPP_
#define CONFIGMANAGER_VERSION_CATALOG_HPP_

#include <vector>

#include "configmanager/config_model.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"

namespace configmanager {

// Version metadata and default factories (docs/HighLevelDesign.md §7). Knows
// nothing about migrations (ADR-012).
//
// The catalog is the single source of version ordering and adjacency:
// nextVersion() returns the next *registered* version in catalog order, and
// is what registry validation and the migration engine consult. Version ids
// need not be consecutive — a catalog of v1, v2, v4 makes v2 and v4 adjacent.
class VersionCatalog {
 public:
  // Duplicate version -> InvalidVersion. An empty DefaultFactory is a
  // registration error, caught here with MigrationFailed (the callback
  // family's error code, §10) rather than at first use.
  Result<void> registerVersion(VersionArtifact artifact);

  bool contains(VersionId v) const noexcept;

  // Empty catalog -> InvalidVersion. Unreachable through ConfigRuntime,
  // which rejects an empty catalog at create() (§9.2).
  Result<VersionId> latestVersion() const;

  // Next registered version after v in catalog order. Unknown v ->
  // InvalidVersion; v == latest -> InvalidVersion (nothing follows it).
  Result<VersionId> nextVersion(VersionId v) const;

  // Runs the version's factory and adopts the result via
  // ConfigModel::fromValue. A throwing factory is caught and mapped to
  // MigrationFailed (ADR-018).
  Result<ConfigModel> createDefault(VersionId v) const;

  // Ordered ascending; used by registry validation.
  const std::vector<VersionId>& versions() const noexcept { return versions_; }

 private:
  // versions_ is kept sorted; factories_[i] belongs to versions_[i].
  std::vector<VersionId> versions_;
  std::vector<DefaultFactory> factories_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_VERSION_CATALOG_HPP_

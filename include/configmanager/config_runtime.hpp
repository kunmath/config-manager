#ifndef CONFIGMANAGER_CONFIG_RUNTIME_HPP_
#define CONFIGMANAGER_CONFIG_RUNTIME_HPP_

#include <utility>

#include "configmanager/config_model.hpp"
#include "configmanager/migration_registry.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/version_catalog.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {

enum class SyncStatus {
  InSync,
  UpgradeRequired,
  DowngradeRequired,
};

// There is no failure status: synchronize() reports failures through
// Result's error channel and guarantees the caller's configuration is
// untouched on error.
struct SyncState {
  VersionId currentVersion;
  VersionId targetVersion;
  SyncStatus status;
};

// The orchestrator (docs/HighLevelDesign.md §9): validating construction,
// inspection, and the single transactional synchronize() pipeline
// (clone -> migrate -> repair -> single commit, ADR-009).
class ConfigRuntime {
 public:
  // Construction can fail (registry validation, ADR-011) and constructors
  // cannot return Result, so construction is a static factory. Rejects an
  // empty catalog with InvalidVersion (no latest version to synchronize
  // toward), then runs registry.validate(catalog).
  static Result<ConfigRuntime> create(VersionCatalog catalog,
                                      MigrationRegistry registry);

  // Pure version comparison; never mutates and does not check whether
  // `supported` is registered (synchronize() does).
  SyncState inspect(const VersionedConfig& cfg, VersionId supported) const;

  // Transactional: all work happens on a clone; cfg is untouched until the
  // single commit, which happens only when migration or repair actually
  // changed something. The commit move-assigns onto cfg, invalidating every
  // ConfigNode handle previously obtained from it (§4.2). On error cfg is
  // guaranteed unmodified. A version newer than `supported` returns
  // SyncStatus::DowngradeRequired without modification (configurations
  // written by newer applications are expected to carry versions this
  // catalog does not know); an InSync configuration still passes through
  // repair, since being at the right version does not guarantee no keys
  // have drifted away.
  Result<SyncStatus> synchronize(VersionedConfig& cfg, VersionId supported);
  Result<SyncStatus> synchronize(VersionedConfig& cfg);  // supported = latest

  // Default-constructs a configuration at `version` from its registered
  // factory.
  Result<VersionedConfig> createDefault(VersionId version) const;

 private:
  ConfigRuntime(VersionCatalog catalog, MigrationRegistry registry)
      : catalog_(std::move(catalog)), registry_(std::move(registry)) {}

  // Fills keys missing from the target version's default model; never
  // overwrites (§9.5). Returns whether any key was added (drives the commit
  // decision).
  Result<bool> repair(ConfigModel& model, VersionId targetVersion) const;

  VersionCatalog catalog_;
  MigrationRegistry registry_;
  // No MigrationEngine member: the engine holds references to catalog and
  // registry, so a stored engine would dangle once create() moves the
  // runtime out (returned by value). The engine is stateless and cheap;
  // synchronize() constructs one over catalog_/registry_ per call.
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_CONFIG_RUNTIME_HPP_

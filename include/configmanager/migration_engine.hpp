#ifndef CONFIGMANAGER_MIGRATION_ENGINE_HPP_
#define CONFIGMANAGER_MIGRATION_ENGINE_HPP_

#include "configmanager/migration_registry.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/version_catalog.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {

// Stepwise forward migration walker (docs/HighLevelDesign.md §8.2). Stateless and
// cheap: holds references only, so construct one over a catalog/registry pair
// per use and let it go out of scope.
//
// migrate() is also the *direct migration* entry point (ADR-016) for apps
// that bypass ConfigRuntime. Direct semantics are raw: in-place mutation, no
// transactionality, no repair, and no forced registry validation
// (registry.validate(catalog) is the caller's job). After a mid-chain failure
// config.version reflects the last successfully completed step, but the model
// may additionally contain partial mutations from the failed step — treat the
// configuration as suspect after any failure; callers wanting rollback
// clone() first.
class MigrationEngine {
 public:
  // The catalog defines adjacency. Both references must outlive the engine.
  MigrationEngine(const VersionCatalog& catalog,
                  const MigrationRegistry& registry) noexcept
      : catalog_(catalog), registry_(registry) {}

  // Walks config.version -> target, one adjacent step at a time, advancing
  // config.version after each successful step. target must be a registered
  // version at or above config.version, else InvalidVersion (downgrade is
  // handled at the runtime layer, not here); already-at-target is a
  // successful no-op. A migration function that throws or returns an error is
  // reported as MigrationFailed with the step and original diagnostic in the
  // message.
  //
  // Migration functions run through the registry's stored callables (state
  // persists across steps and runs) and must not mutate the registry —
  // registering an edge from inside a migration can destroy the running
  // callable (see MigrationRegistry::registerMigration).
  Result<void> migrate(VersionedConfig& config, VersionId target);

 private:
  const VersionCatalog& catalog_;
  const MigrationRegistry& registry_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_MIGRATION_ENGINE_HPP_

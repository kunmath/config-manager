#ifndef CONFIGMANAGER_MIGRATION_REGISTRY_HPP_
#define CONFIGMANAGER_MIGRATION_REGISTRY_HPP_

#include <functional>
#include <vector>

#include "configmanager/config_model.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/version_catalog.hpp"

namespace configmanager {

class MigrationEngine;

// Migrations receive a context, not the model directly (ADR-017). The context
// can grow (logging sink, defaults access, ...) without changing MigrationFn —
// a signature change would break every registered migration in every
// consumer.
class MigrationContext {
 public:
  ConfigModel& model() noexcept { return *model_; }
  VersionId fromVersion() const noexcept { return from_; }
  VersionId toVersion() const noexcept { return to_; }

 private:
  friend class MigrationEngine;  // only the engine builds contexts
  MigrationContext(ConfigModel& model, VersionId from, VersionId to) noexcept
      : model_(&model), from_(from), to_(to) {}

  ConfigModel* model_;
  VersionId from_;
  VersionId to_;
};

// Transforms the context's model in place.
using MigrationFn = std::function<Result<void>(MigrationContext&)>;

struct MigrationEdge {
  VersionId from;
  VersionId to;  // must equal catalog.nextVersion(from) (adjacent-only)
  MigrationFn apply;
};

// Migration edge store (docs/HighLevelDesign.md §8.1). Registration keeps only the
// checks that need no catalog (duplicate edge, empty callable); adjacency and
// completeness are validate()'s job, since the catalog may still be growing
// while edges are registered.
class MigrationRegistry {
 public:
  // Duplicate (from,to) -> InvalidVersion. An empty MigrationFn is a
  // registration error, surfaced immediately with MigrationFailed rather
  // than at first execution.
  //
  // Must not be called while MigrationEngine::migrate() is executing over
  // this registry — including from inside a migration function: the engine
  // invokes the stored callable in place, and registration may reallocate
  // the edge storage and destroy it mid-execution (undefined behavior).
  Result<void> registerMigration(VersionId from, VersionId to, MigrationFn fn);

  // Absent edge -> MissingMigration. The pointer stays valid until the next
  // registerMigration.
  Result<const MigrationEdge*> findMigration(VersionId from,
                                             VersionId to) const;

  // Validates against the catalog (called by ConfigRuntime::create):
  //   1. every registered version below the latest has an edge to the next
  //      registered version (else MissingMigration);
  //   2. no duplicate (from,to) edges (enforced at registration);
  //   3. all edge endpoints exist in the catalog (else InvalidVersion);
  //   4. only adjacent edges, adjacency in catalog order (else
  //      InvalidVersion).
  Result<void> validate(const VersionCatalog& catalog) const;

 private:
  std::vector<MigrationEdge> edges_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_MIGRATION_REGISTRY_HPP_

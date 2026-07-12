#include "configmanager/migration_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace configmanager {

namespace {

std::string edgeName(VersionId from, VersionId to) {
  return std::to_string(from) + "->" + std::to_string(to);
}

}  // namespace

Result<void> MigrationRegistry::registerMigration(VersionId from, VersionId to,
                                                  MigrationFn fn) {
  if (!fn) {
    return fail(ErrorCode::MigrationFailed,
                "migration " + edgeName(from, to) + " is an empty callable");
  }
  const bool duplicate =
      std::any_of(edges_.begin(), edges_.end(), [&](const MigrationEdge& e) {
        return e.from == from && e.to == to;
      });
  if (duplicate) {
    return fail(ErrorCode::InvalidVersion,
                "migration " + edgeName(from, to) + " is already registered");
  }
  edges_.push_back(MigrationEdge{from, to, std::move(fn)});
  return {};
}

Result<const MigrationEdge*> MigrationRegistry::findMigration(
    VersionId from, VersionId to) const {
  const auto pos =
      std::find_if(edges_.begin(), edges_.end(), [&](const MigrationEdge& e) {
        return e.from == from && e.to == to;
      });
  if (pos == edges_.end()) {
    return fail(ErrorCode::MissingMigration,
                "no migration registered for " + edgeName(from, to));
  }
  return &*pos;
}

Result<void> MigrationRegistry::validate(const VersionCatalog& catalog) const {
  // Rules 2 and 3: endpoints exist, and every edge is catalog-adjacent.
  // Duplicates need no re-check here: registration rejects them.
  for (const MigrationEdge& edge : edges_) {
    if (!catalog.contains(edge.from) || !catalog.contains(edge.to)) {
      return fail(ErrorCode::InvalidVersion,
                  "migration " + edgeName(edge.from, edge.to) +
                      " references a version not in the catalog");
    }
    const Result<VersionId> next = catalog.nextVersion(edge.from);
    if (!next || *next != edge.to) {
      return fail(ErrorCode::InvalidVersion,
                  "migration " + edgeName(edge.from, edge.to) +
                      " is not adjacent in catalog order");
    }
  }
  // Rule 1: the chain is complete from every version up to the latest.
  const std::vector<VersionId> versions = catalog.versions();
  for (std::size_t i = 0; i + 1 < versions.size(); ++i) {
    const VersionId from = versions[i];
    const VersionId to = versions[i + 1];
    const bool present =
        std::any_of(edges_.begin(), edges_.end(), [&](const MigrationEdge& e) {
          return e.from == from && e.to == to;
        });
    if (!present) {
      return fail(ErrorCode::MissingMigration,
                  "no migration registered for " + edgeName(from, to));
    }
  }
  return {};
}

}  // namespace configmanager

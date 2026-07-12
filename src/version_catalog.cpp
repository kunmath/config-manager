#include "configmanager/version_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace configmanager {

namespace {

std::vector<VersionArtifact>::const_iterator findEntry(
    const std::vector<VersionArtifact>& entries, VersionId v) {
  return std::lower_bound(entries.begin(), entries.end(), v,
                          [](const VersionArtifact& entry, VersionId value) {
                            return entry.version < value;
                          });
}

}  // namespace

Result<void> VersionCatalog::registerVersion(VersionArtifact artifact) {
  if (!artifact.defaultFactory) {
    return fail(ErrorCode::MigrationFailed,
                "default factory for version " +
                    std::to_string(artifact.version) + " is empty");
  }
  const auto pos = findEntry(entries_, artifact.version);
  if (pos != entries_.end() && pos->version == artifact.version) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(artifact.version) +
                    " is already registered");
  }
  entries_.insert(pos, std::move(artifact));
  return {};
}

bool VersionCatalog::contains(VersionId v) const noexcept {
  const auto pos = findEntry(entries_, v);
  return pos != entries_.end() && pos->version == v;
}

Result<VersionId> VersionCatalog::latestVersion() const {
  if (entries_.empty()) {
    return fail(ErrorCode::InvalidVersion, "catalog has no registered versions");
  }
  return entries_.back().version;
}

Result<VersionId> VersionCatalog::nextVersion(VersionId v) const {
  const auto pos = findEntry(entries_, v);
  if (pos == entries_.end() || pos->version != v) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(v) + " is not registered");
  }
  const auto next = pos + 1;
  if (next == entries_.end()) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(v) +
                    " is the latest registered version");
  }
  return next->version;
}

Result<ConfigModel> VersionCatalog::createDefault(VersionId v) const {
  const auto pos = findEntry(entries_, v);
  if (pos == entries_.end() || pos->version != v) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(v) + " is not registered");
  }
  ConfigValue root;
  try {
    root = pos->defaultFactory();
  } catch (const std::bad_alloc&) {
    throw;  // memory exhaustion is not a recoverable config error (ADR-018)
  } catch (const std::exception& e) {
    return fail(ErrorCode::MigrationFailed,
                "default factory for version " + std::to_string(v) +
                    " threw: " + e.what());
  } catch (...) {
    return fail(ErrorCode::MigrationFailed,
                "default factory for version " + std::to_string(v) +
                    " threw a non-standard exception");
  }
  return ConfigModel::fromValue(std::move(root));
}

std::vector<VersionId> VersionCatalog::versions() const {
  std::vector<VersionId> ids;
  ids.reserve(entries_.size());
  for (const VersionArtifact& entry : entries_) {
    ids.push_back(entry.version);
  }
  return ids;
}

}  // namespace configmanager

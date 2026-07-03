#include "configmanager/version_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace configmanager {

Result<void> VersionCatalog::registerVersion(VersionArtifact artifact) {
  if (!artifact.defaultFactory) {
    return fail(ErrorCode::MigrationFailed,
                "default factory for version " +
                    std::to_string(artifact.version) + " is empty");
  }
  const auto pos =
      std::lower_bound(versions_.begin(), versions_.end(), artifact.version);
  if (pos != versions_.end() && *pos == artifact.version) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(artifact.version) +
                    " is already registered");
  }
  const auto index = static_cast<std::size_t>(pos - versions_.begin());
  versions_.insert(pos, artifact.version);
  factories_.insert(factories_.begin() + static_cast<std::ptrdiff_t>(index),
                    std::move(artifact.defaultFactory));
  return {};
}

bool VersionCatalog::contains(VersionId v) const noexcept {
  return std::binary_search(versions_.begin(), versions_.end(), v);
}

Result<VersionId> VersionCatalog::latestVersion() const {
  if (versions_.empty()) {
    return fail(ErrorCode::InvalidVersion, "catalog has no registered versions");
  }
  return versions_.back();
}

Result<VersionId> VersionCatalog::nextVersion(VersionId v) const {
  const auto pos = std::lower_bound(versions_.begin(), versions_.end(), v);
  if (pos == versions_.end() || *pos != v) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(v) + " is not registered");
  }
  const auto next = pos + 1;
  if (next == versions_.end()) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(v) +
                    " is the latest registered version");
  }
  return *next;
}

Result<ConfigModel> VersionCatalog::createDefault(VersionId v) const {
  const auto pos = std::lower_bound(versions_.begin(), versions_.end(), v);
  if (pos == versions_.end() || *pos != v) {
    return fail(ErrorCode::InvalidVersion,
                "version " + std::to_string(v) + " is not registered");
  }
  const auto index = static_cast<std::size_t>(pos - versions_.begin());
  ConfigValue root;
  try {
    root = factories_[index]();
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

}  // namespace configmanager

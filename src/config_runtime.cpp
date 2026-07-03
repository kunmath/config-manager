#include "configmanager/config_runtime.hpp"

#include <string>
#include <utility>
#include <vector>

#include "configmanager/config_node.hpp"
#include "configmanager/migration_engine.hpp"

namespace configmanager {

namespace {

// Reads the defaults tree through const ConfigNode handles (§4.3) and writes
// through ConfigModel's path API, keeping ConfigNode read-only. `path` names
// defaultsNode's location, which is the same in both trees. Rules (§9.5):
// copy a default subtree only where its key is entirely absent; presence wins
// over shape (no overwrite, no coercion, no default children under a
// type-mismatched key); arrays are atomic; recursion descends only where
// both trees hold an Object at the same key.
Result<bool> fillMissing(ConfigModel& model, const ConfigModel& defaults,
                         ConfigNode defaultsNode, const std::string& path) {
  bool added = false;
  Result<std::vector<std::string>> keys = defaultsNode.keys();
  if (!keys) {
    return fail(keys.error().code, std::move(keys.error().message));
  }
  for (const std::string& key : *keys) {
    const std::string childPath = path.empty() ? key : path + "." + key;
    Result<ConfigNode> defaultsChild = defaultsNode.child(key);
    if (!defaultsChild) {
      return fail(defaultsChild.error().code,
                  std::move(defaultsChild.error().message));
    }
    if (!model.contains(childPath)) {
      Result<ConfigValue> subtree = defaults.getValue(childPath);
      if (!subtree) {
        return fail(subtree.error().code, std::move(subtree.error().message));
      }
      Result<void> written = model.set(childPath, std::move(*subtree));
      if (!written) {
        return fail(written.error().code, std::move(written.error().message));
      }
      added = true;
    } else if (defaultsChild->type() == NodeType::Object) {
      Result<ConfigNode> modelChild = model.nodeAt(childPath);
      if (!modelChild) {
        return fail(modelChild.error().code,
                    std::move(modelChild.error().message));
      }
      if (modelChild->type() == NodeType::Object) {
        Result<bool> childAdded =
            fillMissing(model, defaults, *defaultsChild, childPath);
        if (!childAdded) {
          return fail(childAdded.error().code,
                      std::move(childAdded.error().message));
        }
        added = added || *childAdded;
      }
    }
    // else: present in the model -> leave untouched (presence wins).
  }
  return added;
}

}  // namespace

Result<ConfigRuntime> ConfigRuntime::create(VersionCatalog catalog,
                                            MigrationRegistry registry) {
  if (catalog.versions().empty()) {
    return fail(ErrorCode::InvalidVersion,
                "catalog has no registered versions");
  }
  Result<void> validated = registry.validate(catalog);
  if (!validated) {
    return fail(validated.error().code, std::move(validated.error().message));
  }
  return ConfigRuntime(std::move(catalog), std::move(registry));
}

SyncState ConfigRuntime::inspect(const VersionedConfig& cfg,
                                 VersionId supported) const {
  SyncStatus status = SyncStatus::InSync;
  if (cfg.version < supported) {
    status = SyncStatus::UpgradeRequired;
  } else if (cfg.version > supported) {
    status = SyncStatus::DowngradeRequired;
  }
  return SyncState{cfg.version, supported, status};
}

Result<SyncStatus> ConfigRuntime::synchronize(VersionedConfig& cfg,
                                              VersionId supported) {
  if (!catalog_.contains(supported)) {
    return fail(ErrorCode::InvalidVersion,
                "supported version " + std::to_string(supported) +
                    " is not registered");
  }
  const SyncState state = inspect(cfg, supported);
  if (state.status == SyncStatus::DowngradeRequired) {
    return SyncStatus::DowngradeRequired;  // no modification
  }
  if (state.status == SyncStatus::UpgradeRequired &&
      !catalog_.contains(cfg.version)) {
    // Unknown persisted version: fail before any work, not mid-chain.
    return fail(ErrorCode::InvalidVersion,
                "persisted version " + std::to_string(cfg.version) +
                    " is not registered");
  }

  VersionedConfig working{cfg.version, cfg.model.clone()};  // 1. copy
  bool migrated = false;
  if (state.status == SyncStatus::UpgradeRequired) {
    Result<void> outcome =
        MigrationEngine(catalog_, registry_).migrate(working, supported);
    if (!outcome) {  // 2. migrate
      return fail(outcome.error().code, std::move(outcome.error().message));
    }
    migrated = true;
  }
  Result<bool> repaired = repair(working.model, supported);  // 3. repair
  if (!repaired) {
    return fail(repaired.error().code, std::move(repaired.error().message));
  }
  if (migrated || *repaired) {
    cfg = std::move(working);  // 4. commit — the single mutation of cfg
  }
  return SyncStatus::InSync;
}

Result<SyncStatus> ConfigRuntime::synchronize(VersionedConfig& cfg) {
  // create() rejected an empty catalog, so latestVersion() cannot fail here.
  Result<VersionId> latest = catalog_.latestVersion();
  if (!latest) {
    return fail(latest.error().code, std::move(latest.error().message));
  }
  return synchronize(cfg, *latest);
}

Result<VersionedConfig> ConfigRuntime::createDefault(VersionId version) const {
  Result<ConfigModel> model = catalog_.createDefault(version);
  if (!model) {
    return fail(model.error().code, std::move(model.error().message));
  }
  return VersionedConfig{version, std::move(*model)};
}

Result<bool> ConfigRuntime::repair(ConfigModel& model,
                                   VersionId targetVersion) const {
  Result<ConfigModel> defaults = catalog_.createDefault(targetVersion);
  if (!defaults) {
    return fail(defaults.error().code, std::move(defaults.error().message));
  }
  return fillMissing(model, *defaults, defaults->root(), /*path=*/"");
}

}  // namespace configmanager

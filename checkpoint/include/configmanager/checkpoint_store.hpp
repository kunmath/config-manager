#ifndef CONFIGMANAGER_CHECKPOINT_STORE_HPP_
#define CONFIGMANAGER_CHECKPOINT_STORE_HPP_

#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include "configmanager/config_interface.hpp"
#include "configmanager/config_runtime.hpp"
#include "configmanager/result.hpp"
#include "configmanager/version.hpp"
#include "configmanager/versioned_config.hpp"

namespace configmanager {

struct CheckpointOptions {
  std::filesystem::path canonical_path;
  std::filesystem::path checkpoint_directory;
  std::size_t retention = 2;
  // Canonical and checkpoint files larger than this are not read: capture
  // fails and restore/prune report the file as rejected.
  std::size_t max_file_bytes = 16 * 1024 * 1024;
};

class CheckpointStore {
 public:
  const CheckpointOptions& options() const { return options_; }

 private:
  explicit CheckpointStore(CheckpointOptions options)
      : options_(std::move(options)) {}

  CheckpointOptions options_;

  friend Result<CheckpointStore> cpCreate(CheckpointOptions options);
};

struct CheckpointRejection {
  std::filesystem::path path;
  Error error;
};

struct PreparedRestore {
  std::filesystem::path checkpoint_path;
  VersionId source_version;
  VersionedConfig config;
};

struct RestoreSearch {
  std::optional<PreparedRestore> candidate;
  std::vector<CheckpointRejection> rejected;
};

struct PruneReport {
  std::vector<std::filesystem::path> removed;
  std::vector<CheckpointRejection> rejected;
};

// Validates options and creates a handle. Filesystem changes remain explicit in
// the other checkpoint operations.
Result<CheckpointStore> cpCreate(CheckpointOptions options);

// Atomically captures the canonical file's original bytes under config.version.
Result<void> cpCapture(const CheckpointStore& store,
                       const VersionedConfig& config,
                       IConfigInterface& backend);

// Tries compatible checkpoints from highest embedded version downward and
// returns the first that synchronizes successfully. The canonical file is
// never modified.
Result<RestoreSearch> cpPrepareRestore(const CheckpointStore& store,
                                       IConfigInterface& backend,
                                       ConfigRuntime& runtime,
                                       VersionId supported_version);

// Serializes config to a sibling temporary file and atomically replaces the
// canonical file as the final operation.
Result<void> cpCommit(const CheckpointStore& store,
                      const VersionedConfig& config, IConfigInterface& backend);

// Retains the newest verified checkpoints by file modification time. Unreadable
// checkpoints remain untouched and are returned in the report. Call only after
// a successful canonical commit.
Result<PruneReport> cpPrune(const CheckpointStore& store,
                            IConfigInterface& backend);

}  // namespace configmanager

#endif  // CONFIGMANAGER_CHECKPOINT_STORE_HPP_

#include "configmanager/checkpoint_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace configmanager {
namespace {

namespace fs = std::filesystem;

std::atomic<std::uint64_t> g_temp_sequence{0};

std::string storageErrorMessage(std::string operation, const fs::path& path,
                                const std::error_code& error = {}) {
  std::string message = std::move(operation) + " '" + path.string() + "'";
  if (error) {
    message += ": " + error.message();
  }
  return message;
}

fs::path parentOrCurrent(const fs::path& path) {
  return path.parent_path().empty() ? fs::path(".") : path.parent_path();
}

fs::path temporaryCandidate(const fs::path& destination) {
  const std::uint64_t sequence =
      g_temp_sequence.fetch_add(1, std::memory_order_relaxed);
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return parentOrCurrent(destination) /
         (destination.filename().string() + ".tmp." + std::to_string(ticks) +
          "." + std::to_string(sequence));
}

void removeTemporary(const fs::path& path) {
  std::error_code ignored;
  fs::remove(path, ignored);
}

Result<std::string> readFileBytes(const fs::path& path, std::size_t max_bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return fail(ErrorCode::StorageError,
                "cannot open '" + path.string() + "' for reading");
  }

  std::string bytes;
  std::array<char, 16 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::size_t count = static_cast<std::size_t>(input.gcount());
    if (count > max_bytes - bytes.size()) {
      return fail(ErrorCode::StorageError,
                  "file '" + path.string() + "' exceeds size limit (" +
                      std::to_string(max_bytes) + " bytes)");
    }
    bytes.append(buffer.data(), count);
  }
  if (!input.eof()) {
    return fail(ErrorCode::StorageError,
                "failed while reading '" + path.string() + "'");
  }
  return bytes;
}

#ifdef _WIN32
Result<fs::path> writeTemporary(const fs::path& destination,
                                const std::string& bytes,
                                const fs::path& permission_source) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const fs::path candidate = temporaryCandidate(destination);
    std::error_code source_error;
    const bool source_exists = fs::exists(permission_source, source_error);
    if (source_error) {
      return fail(ErrorCode::StorageError,
                  storageErrorMessage("cannot inspect permission source",
                                      permission_source, source_error));
    }

    if (source_exists &&
        !CopyFileW(permission_source.c_str(), candidate.c_str(), TRUE)) {
      if (GetLastError() == ERROR_FILE_EXISTS) {
        continue;
      }
      return fail(ErrorCode::StorageError,
                  "cannot create temporary file '" + candidate.string() +
                      "' (Windows error " + std::to_string(GetLastError()) +
                      ")");
    }

    HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                              source_exists ? TRUNCATE_EXISTING : CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (!source_exists && error == ERROR_FILE_EXISTS) {
        continue;
      }
      if (source_exists) {
        removeTemporary(candidate);
      }
      return fail(ErrorCode::StorageError,
                  "cannot open temporary file '" + candidate.string() +
                      "' (Windows error " + std::to_string(error) + ")");
    }

    std::size_t offset = 0;
    bool write_ok = true;
    while (offset < bytes.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
      DWORD written = 0;
      if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
          written == 0) {
        write_ok = false;
        break;
      }
      offset += written;
    }
    if (!CloseHandle(file)) {
      write_ok = false;
    }
    if (!write_ok) {
      removeTemporary(candidate);
      return fail(ErrorCode::StorageError, "failed to write temporary file '" +
                                               candidate.string() + "'");
    }
    return candidate;
  }
  return fail(ErrorCode::StorageError,
              "cannot allocate a unique temporary file beside '" +
                  destination.string() + "'");
}
#else
Result<fs::path> writeTemporary(const fs::path& destination,
                                const std::string& bytes,
                                const fs::path& permission_source) {
  mode_t final_mode = S_IRUSR | S_IWUSR;
  struct stat source_stat {};
  if (::stat(permission_source.c_str(), &source_stat) == 0) {
    final_mode = source_stat.st_mode & 07777;
  } else if (errno != ENOENT) {
    return fail(
        ErrorCode::StorageError,
        "cannot inspect permissions of '" + permission_source.string() +
            "': " + std::error_code(errno, std::generic_category()).message());
  }

  for (int attempt = 0; attempt < 100; ++attempt) {
    const fs::path candidate = temporaryCandidate(destination);
    const int descriptor =
        ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
               S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      if (errno == EEXIST) {
        continue;
      }
      return fail(
          ErrorCode::StorageError,
          "cannot create temporary file '" + candidate.string() + "': " +
              std::error_code(errno, std::generic_category()).message());
    }

    std::size_t offset = 0;
    int write_error = 0;
    while (offset < bytes.size()) {
      const ssize_t written =
          ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        write_error = written < 0 ? errno : EIO;
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
    if (write_error == 0 && ::fchmod(descriptor, final_mode) != 0) {
      write_error = errno;
    }
    if (::close(descriptor) != 0 && write_error == 0) {
      write_error = errno;
    }
    if (write_error != 0) {
      removeTemporary(candidate);
      return fail(
          ErrorCode::StorageError,
          "failed to write temporary file '" + candidate.string() + "': " +
              std::error_code(write_error, std::generic_category()).message());
    }
    return candidate;
  }
  return fail(ErrorCode::StorageError,
              "cannot allocate a unique temporary file beside '" +
                  destination.string() + "'");
}
#endif

Result<void> replaceFile(const fs::path& temporary,
                         const fs::path& destination) {
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return fail(ErrorCode::StorageError,
                "cannot atomically replace '" + destination.string() +
                    "' (Windows error " + std::to_string(GetLastError()) + ")");
  }
#else
  if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
    return fail(ErrorCode::StorageError,
                "cannot atomically replace '" + destination.string() + "': " +
                    std::error_code(errno, std::generic_category()).message());
  }
#endif
  return {};
}

std::string checkpointFilename(VersionId version, const fs::path& extension) {
  std::ostringstream name;
  name << std::setw(10) << std::setfill('0') << version << extension.string();
  return name.str();
}

bool isCheckpointFilename(const fs::path& path, const fs::path& extension) {
  if (path.extension() != extension) {
    return false;
  }
  const std::string stem = path.stem().string();
  return stem.size() == 10 &&
         std::all_of(stem.begin(), stem.end(),
                     [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

Result<std::vector<fs::path>> checkpointFiles(const CheckpointStore& store) {
  const CheckpointOptions& options = store.options();
  std::error_code error;
  const bool exists = fs::exists(options.checkpoint_directory, error);
  if (error) {
    return fail(ErrorCode::StorageError,
                storageErrorMessage("cannot inspect checkpoint directory",
                                    options.checkpoint_directory, error));
  }
  if (!exists) {
    return std::vector<fs::path>{};
  }

  std::vector<fs::path> paths;
  fs::directory_iterator iterator(options.checkpoint_directory, error);
  const fs::directory_iterator end;
  while (!error && iterator != end) {
    const fs::directory_entry& entry = *iterator;
    std::error_code type_error;
    const bool regular = entry.is_regular_file(type_error);
    if (type_error) {
      return fail(ErrorCode::StorageError,
                  storageErrorMessage("cannot inspect checkpoint entry",
                                      entry.path(), type_error));
    }
    if (regular && isCheckpointFilename(entry.path(),
                                        options.canonical_path.extension())) {
      paths.push_back(entry.path());
    }
    iterator.increment(error);
  }
  if (error) {
    return fail(ErrorCode::StorageError,
                storageErrorMessage("cannot enumerate checkpoint directory",
                                    options.checkpoint_directory, error));
  }
  return paths;
}

Result<VersionedConfig> loadBytes(const std::string& bytes,
                                  IConfigInterface& backend) {
  try {
    std::istringstream input(bytes);
    return backend.load(input);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return fail(ErrorCode::ParseError,
                std::string("checkpoint backend load failed: ") + error.what());
  } catch (...) {
    return fail(ErrorCode::ParseError,
                "checkpoint backend load failed with a non-standard exception");
  }
}

Result<std::string> saveBytes(const VersionedConfig& config,
                              IConfigInterface& backend) {
  try {
    std::ostringstream output;
    Result<void> saved = backend.save(config, output);
    if (!saved) {
      return fail(saved.error().code, std::move(saved.error().message));
    }
    if (!output) {
      return fail(ErrorCode::SerializationError,
                  "checkpoint backend failed to serialize the configuration");
    }
    return output.str();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return fail(ErrorCode::SerializationError,
                std::string("checkpoint backend save failed: ") + error.what());
  } catch (...) {
    return fail(ErrorCode::SerializationError,
                "checkpoint backend save failed with a non-standard exception");
  }
}

template <typename T>
Result<T> storageException(const char* operation, const std::exception& error) {
  return fail(ErrorCode::StorageError,
              std::string(operation) + ": " + error.what());
}

Result<CheckpointOptions> validateOptions(CheckpointOptions options) {
  if (options.canonical_path.empty()) {
    return fail(ErrorCode::StorageError, "canonical path must not be empty");
  }
  if (options.checkpoint_directory.empty()) {
    return fail(ErrorCode::StorageError,
                "checkpoint directory must not be empty");
  }

  options.canonical_path = fs::weakly_canonical(options.canonical_path);
  options.checkpoint_directory =
      fs::weakly_canonical(options.checkpoint_directory);
  if (parentOrCurrent(options.canonical_path) == options.checkpoint_directory) {
    return fail(ErrorCode::StorageError,
                "canonical file must not be inside the checkpoint directory");
  }
  return options;
}

Result<void> cpCaptureImpl(const CheckpointStore& store,
                           const VersionedConfig& config,
                           IConfigInterface& backend) {
  const CheckpointOptions& options = store.options();
  Result<std::string> bytes =
      readFileBytes(options.canonical_path, options.max_file_bytes);
  if (!bytes) {
    return fail(bytes.error().code, std::move(bytes.error().message));
  }
  Result<VersionedConfig> loaded = loadBytes(*bytes, backend);
  if (!loaded) {
    return fail(loaded.error().code, std::move(loaded.error().message));
  }
  if (loaded->version != config.version) {
    return fail(ErrorCode::InvalidVersion,
                "canonical version " + std::to_string(loaded->version) +
                    " does not match supplied config version " +
                    std::to_string(config.version));
  }

  std::error_code error;
  fs::create_directories(options.checkpoint_directory, error);
  if (error) {
    return fail(ErrorCode::StorageError,
                storageErrorMessage("cannot create checkpoint directory",
                                    options.checkpoint_directory, error));
  }
  const fs::path destination =
      options.checkpoint_directory /
      checkpointFilename(config.version, options.canonical_path.extension());
  std::error_code exists_error;
  const bool destination_exists = fs::exists(destination, exists_error);
  if (exists_error) {
    return fail(ErrorCode::StorageError,
                storageErrorMessage("cannot inspect checkpoint", destination,
                                    exists_error));
  }
  const fs::path& permission_source =
      destination_exists ? destination : options.canonical_path;
  Result<fs::path> temporary =
      writeTemporary(destination, *bytes, permission_source);
  if (!temporary) {
    return fail(temporary.error().code, std::move(temporary.error().message));
  }
  Result<void> replaced = replaceFile(*temporary, destination);
  if (!replaced) {
    removeTemporary(*temporary);
    return replaced;
  }
  return {};
}

Result<RestoreSearch> cpPrepareRestoreImpl(const CheckpointStore& store,
                                           IConfigInterface& backend,
                                           ConfigRuntime& runtime,
                                           VersionId supported_version) {
  if (!runtime.supportsVersion(supported_version)) {
    return fail(ErrorCode::InvalidVersion,
                "supported version " + std::to_string(supported_version) +
                    " is not registered");
  }
  Result<std::vector<fs::path>> paths = checkpointFiles(store);
  if (!paths) {
    return fail(paths.error().code, std::move(paths.error().message));
  }

  struct LoadedCandidate {
    fs::path path;
    VersionId source_version;
    fs::file_time_type modified;
    VersionedConfig config;
  };
  RestoreSearch search;
  std::vector<LoadedCandidate> candidates;
  for (const fs::path& path : *paths) {
    Result<std::string> bytes =
        readFileBytes(path, store.options().max_file_bytes);
    if (!bytes) {
      search.rejected.push_back({path, std::move(bytes.error())});
      continue;
    }
    Result<VersionedConfig> loaded = loadBytes(*bytes, backend);
    if (!loaded) {
      search.rejected.push_back({path, std::move(loaded.error())});
      continue;
    }
    const VersionId source_version = loaded->version;
    if (source_version > supported_version) {
      search.rejected.push_back(
          {path, Error{ErrorCode::InvalidVersion,
                       "checkpoint version " + std::to_string(source_version) +
                           " is newer than supported version " +
                           std::to_string(supported_version)}});
      continue;
    }
    std::error_code time_error;
    const fs::file_time_type modified = fs::last_write_time(path, time_error);
    if (time_error) {
      search.rejected.push_back(
          {path,
           Error{ErrorCode::StorageError,
                 storageErrorMessage("cannot read checkpoint modification time",
                                     path, time_error)}});
      continue;
    }
    candidates.push_back({path, source_version, modified, std::move(*loaded)});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const LoadedCandidate& left, const LoadedCandidate& right) {
              if (left.source_version != right.source_version) {
                return left.source_version > right.source_version;
              }
              if (left.modified != right.modified) {
                return left.modified > right.modified;
              }
              return left.path.string() < right.path.string();
            });
  for (LoadedCandidate& candidate : candidates) {
    Result<SyncStatus> synchronized =
        runtime.synchronize(candidate.config, supported_version);
    if (!synchronized) {
      search.rejected.push_back(
          {candidate.path, std::move(synchronized.error())});
      continue;
    }
    if (*synchronized != SyncStatus::InSync ||
        candidate.config.version != supported_version) {
      search.rejected.push_back(
          {candidate.path,
           Error{ErrorCode::InvalidVersion,
                 "checkpoint did not synchronize to supported version " +
                     std::to_string(supported_version)}});
      continue;
    }
    search.candidate = PreparedRestore{candidate.path, candidate.source_version,
                                       std::move(candidate.config)};
    break;
  }
  return search;
}

Result<void> cpCommitImpl(const CheckpointStore& store,
                          const VersionedConfig& config,
                          IConfigInterface& backend) {
  Result<std::string> bytes = saveBytes(config, backend);
  if (!bytes) {
    return fail(bytes.error().code, std::move(bytes.error().message));
  }
  const fs::path& destination = store.options().canonical_path;
  std::error_code error;
  fs::create_directories(parentOrCurrent(destination), error);
  if (error) {
    return fail(ErrorCode::StorageError,
                storageErrorMessage("cannot create canonical directory",
                                    parentOrCurrent(destination), error));
  }
  Result<fs::path> temporary = writeTemporary(destination, *bytes, destination);
  if (!temporary) {
    return fail(temporary.error().code, std::move(temporary.error().message));
  }
  Result<void> replaced = replaceFile(*temporary, destination);
  if (!replaced) {
    removeTemporary(*temporary);
    return replaced;
  }
  return {};
}

Result<PruneReport> cpPruneImpl(const CheckpointStore& store,
                                IConfigInterface& backend) {
  Result<std::vector<fs::path>> paths = checkpointFiles(store);
  if (!paths) {
    return fail(paths.error().code, std::move(paths.error().message));
  }

  struct CheckpointFile {
    fs::path path;
    fs::file_time_type modified;
  };
  PruneReport report;
  std::map<VersionId, std::vector<CheckpointFile>> versions;
  for (const fs::path& path : *paths) {
    Result<std::string> bytes =
        readFileBytes(path, store.options().max_file_bytes);
    if (!bytes) {
      report.rejected.push_back({path, std::move(bytes.error())});
      continue;
    }
    Result<VersionedConfig> loaded = loadBytes(*bytes, backend);
    if (!loaded) {
      report.rejected.push_back({path, std::move(loaded.error())});
      continue;
    }
    std::error_code error;
    const fs::file_time_type modified = fs::last_write_time(path, error);
    if (error) {
      report.rejected.push_back(
          {path,
           Error{ErrorCode::StorageError,
                 storageErrorMessage("cannot read checkpoint modification time",
                                     path, error)}});
      continue;
    }
    versions[loaded->version].push_back({path, modified});
  }

  std::vector<CheckpointFile> retained_versions;
  std::vector<fs::path> removals;
  for (auto& [version, files] : versions) {
    static_cast<void>(version);
    std::sort(files.begin(), files.end(),
              [](const CheckpointFile& left, const CheckpointFile& right) {
                if (left.modified != right.modified) {
                  return left.modified > right.modified;
                }
                return left.path.string() < right.path.string();
              });
    retained_versions.push_back(files.front());
    for (std::size_t index = 1; index < files.size(); ++index) {
      removals.push_back(files[index].path);
    }
  }
  std::sort(retained_versions.begin(), retained_versions.end(),
            [](const CheckpointFile& left, const CheckpointFile& right) {
              if (left.modified != right.modified) {
                return left.modified > right.modified;
              }
              return left.path.string() < right.path.string();
            });
  for (std::size_t index = store.options().retention;
       index < retained_versions.size(); ++index) {
    removals.push_back(retained_versions[index].path);
  }
  for (const fs::path& path : removals) {
    std::error_code error;
    fs::remove(path, error);
    if (error) {
      return fail(ErrorCode::StorageError,
                  storageErrorMessage("cannot prune checkpoint", path, error));
    }
    report.removed.push_back(path);
  }
  return report;
}

}  // namespace

Result<CheckpointStore> cpCreate(CheckpointOptions options) {
  try {
    Result<CheckpointOptions> validated = validateOptions(std::move(options));
    if (!validated) {
      return fail(validated.error().code, std::move(validated.error().message));
    }
    return CheckpointStore(std::move(*validated));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return storageException<CheckpointStore>("cpCreate failed", error);
  } catch (...) {
    return fail(ErrorCode::StorageError,
                "cpCreate failed with a non-standard exception");
  }
}

Result<void> cpCapture(const CheckpointStore& store,
                       const VersionedConfig& config,
                       IConfigInterface& backend) {
  try {
    return cpCaptureImpl(store, config, backend);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return storageException<void>("cpCapture failed", error);
  } catch (...) {
    return fail(ErrorCode::StorageError,
                "cpCapture failed with a non-standard exception");
  }
}

Result<RestoreSearch> cpPrepareRestore(const CheckpointStore& store,
                                       IConfigInterface& backend,
                                       ConfigRuntime& runtime,
                                       VersionId supported_version) {
  try {
    return cpPrepareRestoreImpl(store, backend, runtime, supported_version);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return storageException<RestoreSearch>("cpPrepareRestore failed", error);
  } catch (...) {
    return fail(ErrorCode::StorageError,
                "cpPrepareRestore failed with a non-standard exception");
  }
}

Result<void> cpCommit(const CheckpointStore& store,
                      const VersionedConfig& config,
                      IConfigInterface& backend) {
  try {
    return cpCommitImpl(store, config, backend);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return storageException<void>("cpCommit failed", error);
  } catch (...) {
    return fail(ErrorCode::StorageError,
                "cpCommit failed with a non-standard exception");
  }
}

Result<PruneReport> cpPrune(const CheckpointStore& store,
                            IConfigInterface& backend) {
  try {
    return cpPruneImpl(store, backend);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return storageException<PruneReport>("cpPrune failed", error);
  } catch (...) {
    return fail(ErrorCode::StorageError,
                "cpPrune failed with a non-standard exception");
  }
}

}  // namespace configmanager

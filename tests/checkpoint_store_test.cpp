#include "configmanager/checkpoint_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace configmanager {
namespace {

namespace fs = std::filesystem;

class TextBackend : public IConfigInterface {
 public:
  Result<VersionedConfig> load(std::istream& input) override {
    VersionId version = 0;
    std::int64_t value = 0;
    char separator = '\0';
    if (!(input >> version >> separator >> value) || separator != ':') {
      return fail(ErrorCode::ParseError, "invalid test document");
    }
    ConfigModel model;
    Result<void> set = model.set("value", value);
    if (!set) {
      return fail(set.error().code, std::move(set.error().message));
    }
    return VersionedConfig{version, std::move(model)};
  }

  Result<void> save(const VersionedConfig& config,
                    std::ostream& output) override {
    if (fail_save_) {
      return fail(ErrorCode::SerializationError, "injected save failure");
    }
    Result<std::int64_t> value = config.model.get<std::int64_t>("value");
    if (!value) {
      return fail(value.error().code, std::move(value.error().message));
    }
    output << config.version << ':' << *value << '\n';
    if (!output) {
      return fail(ErrorCode::SerializationError, "test stream write failed");
    }
    return {};
  }

  void setFailSave(bool fail) { fail_save_ = fail; }

 private:
  bool fail_save_ = false;
};

class ThrowingBackend : public IConfigInterface {
 public:
  Result<VersionedConfig> load(std::istream&) override {
    throw std::runtime_error("injected load exception");
  }

  Result<void> save(const VersionedConfig&, std::ostream&) override {
    throw std::runtime_error("injected save exception");
  }
};

ConfigValue defaults(std::int64_t value) {
  ConfigValue root = ConfigValue::object();
  root.set("value", ConfigValue::of(value));
  return root;
}

Result<ConfigRuntime> makeRuntime() {
  VersionCatalog catalog;
  Result<void> registered =
      catalog.registerVersion({1, [] { return defaults(10); }});
  if (!registered) {
    return fail(registered.error().code, std::move(registered.error().message));
  }
  registered = catalog.registerVersion({2, [] { return defaults(20); }});
  if (!registered) {
    return fail(registered.error().code, std::move(registered.error().message));
  }
  registered = catalog.registerVersion({3, [] { return defaults(30); }});
  if (!registered) {
    return fail(registered.error().code, std::move(registered.error().message));
  }

  MigrationRegistry registry;
  Result<void> migration = registry.registerMigration(
      1, 2, [](MigrationContext&) -> Result<void> { return {}; });
  if (!migration) {
    return fail(migration.error().code, std::move(migration.error().message));
  }
  migration = registry.registerMigration(
      2, 3, [](MigrationContext&) -> Result<void> { return {}; });
  if (!migration) {
    return fail(migration.error().code, std::move(migration.error().message));
  }
  return ConfigRuntime::create(std::move(catalog), std::move(registry));
}

Result<ConfigRuntime> makeFailingV3Runtime() {
  VersionCatalog catalog;
  Result<void> registered =
      catalog.registerVersion({1, [] { return defaults(10); }});
  if (!registered) {
    return fail(registered.error().code, std::move(registered.error().message));
  }
  registered = catalog.registerVersion({2, [] { return defaults(20); }});
  if (!registered) {
    return fail(registered.error().code, std::move(registered.error().message));
  }
  registered = catalog.registerVersion({3, [] { return defaults(30); }});
  if (!registered) {
    return fail(registered.error().code, std::move(registered.error().message));
  }
  MigrationRegistry registry;
  Result<void> migration = registry.registerMigration(
      1, 2, [](MigrationContext&) -> Result<void> { return {}; });
  if (!migration) {
    return fail(migration.error().code, std::move(migration.error().message));
  }
  migration = registry.registerMigration(
      2, 3, [](MigrationContext& context) -> Result<void> {
        Result<std::int64_t> value = context.model().get<std::int64_t>("value");
        if (!value) {
          return fail(value.error().code, std::move(value.error().message));
        }
        if (*value == 22) {
          return fail(ErrorCode::MigrationFailed, "injected migration failure");
        }
        return {};
      });
  if (!migration) {
    return fail(migration.error().code, std::move(migration.error().message));
  }
  return ConfigRuntime::create(std::move(catalog), std::move(registry));
}

void writeText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output << text;
  ASSERT_TRUE(output.good());
}

std::string readText(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

class CheckpointStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::uint64_t sequence = 0;
    root_ = fs::temp_directory_path() /
            ("configmanager-checkpoint-test-" + std::to_string(++sequence));
    std::error_code error;
    fs::remove_all(root_, error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(fs::create_directories(root_));

    Result<CheckpointStore> store =
        cpCreate({root_ / "config.cfg", root_ / "checkpoints", 2});
    ASSERT_TRUE(store) << store.error().message;
    store_.emplace(std::move(*store));
  }

  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  fs::path checkpoint(VersionId version) const {
    std::ostringstream name;
    name.width(10);
    name.fill('0');
    name << version << ".cfg";
    return root_ / "checkpoints" / name.str();
  }

  VersionedConfig configAt(VersionId version) const {
    return VersionedConfig{version, ConfigModel()};
  }

  fs::path root_;
  std::optional<CheckpointStore> store_;
};

TEST(CheckpointCreateTest, RejectsEmptyPaths) {
  Result<CheckpointStore> no_canonical = cpCreate({{}, "checkpoints", 2});
  ASSERT_FALSE(no_canonical);
  EXPECT_EQ(no_canonical.error().code, ErrorCode::StorageError);

  Result<CheckpointStore> no_directory = cpCreate({"config.cfg", {}, 2});
  ASSERT_FALSE(no_directory);
  EXPECT_EQ(no_directory.error().code, ErrorCode::StorageError);
}

TEST(CheckpointCreateTest, RejectsCanonicalInsideCheckpointDirectory) {
  Result<CheckpointStore> store =
      cpCreate({"checkpoints/0000000001.cfg", "checkpoints", 2});
  ASSERT_FALSE(store);
  EXPECT_EQ(store.error().code, ErrorCode::StorageError);
}

TEST_F(CheckpointStoreTest, CapturePreservesExactBytes) {
  const std::string original = "1:42\n# spacing and comments stay byte-exact\n";
  writeText(store_->options().canonical_path, original);

  TextBackend backend;
  Result<void> captured = cpCapture(*store_, configAt(1), backend);
  ASSERT_TRUE(captured) << captured.error().message;
  EXPECT_EQ(readText(checkpoint(1)), original);
  EXPECT_EQ(readText(store_->options().canonical_path), original);
}

TEST_F(CheckpointStoreTest, CaptureRejectsCanonicalVersionMismatch) {
  writeText(store_->options().canonical_path, "2:42\n");
  TextBackend backend;

  Result<void> captured = cpCapture(*store_, configAt(1), backend);
  ASSERT_FALSE(captured);
  EXPECT_EQ(captured.error().code, ErrorCode::InvalidVersion);
  EXPECT_FALSE(fs::exists(checkpoint(1)));
}

TEST_F(CheckpointStoreTest, BackendExceptionsAreMappedToErrors) {
  writeText(store_->options().canonical_path, "1:42\n");
  ThrowingBackend backend;
  Result<void> captured = cpCapture(*store_, configAt(1), backend);
  ASSERT_FALSE(captured);
  EXPECT_EQ(captured.error().code, ErrorCode::ParseError);

  ConfigModel model;
  ASSERT_TRUE(model.set("value", 42));
  Result<void> committed =
      cpCommit(*store_, VersionedConfig{1, std::move(model)}, backend);
  ASSERT_FALSE(committed);
  EXPECT_EQ(committed.error().code, ErrorCode::SerializationError);
}

TEST_F(CheckpointStoreTest, RecaptureAtomicallyReplacesSameVersion) {
  writeText(store_->options().canonical_path, "1:10\n");
  TextBackend backend;
  ASSERT_TRUE(cpCapture(*store_, configAt(1), backend));
  writeText(store_->options().canonical_path, "1:99\n");

  ASSERT_TRUE(cpCapture(*store_, configAt(1), backend));
  EXPECT_EQ(readText(checkpoint(1)), "1:99\n");
}

TEST_F(CheckpointStoreTest, CaptureFailureDoesNotChangeExistingCheckpoint) {
  writeText(store_->options().canonical_path, "1:10\n");
  TextBackend backend;
  ASSERT_TRUE(cpCapture(*store_, configAt(1), backend));
  ASSERT_TRUE(fs::remove(store_->options().canonical_path));

  Result<void> captured = cpCapture(*store_, configAt(1), backend);
  ASSERT_FALSE(captured);
  EXPECT_EQ(captured.error().code, ErrorCode::StorageError);
  EXPECT_EQ(readText(checkpoint(1)), "1:10\n");
}

TEST_F(CheckpointStoreTest, OlderCheckpointMigratesForwardInMemory) {
  writeText(checkpoint(1), "1:77\n");
  Result<ConfigRuntime> runtime = makeRuntime();
  ASSERT_TRUE(runtime) << runtime.error().message;
  TextBackend backend;

  Result<RestoreSearch> search =
      cpPrepareRestore(*store_, backend, *runtime, 2);
  ASSERT_TRUE(search) << search.error().message;
  ASSERT_TRUE(search->candidate);
  EXPECT_EQ(search->candidate->source_version, 1u);
  EXPECT_EQ(search->candidate->config.version, 2u);
  EXPECT_EQ(search->candidate->config.model.get<std::int64_t>("value").value(),
            77);
  EXPECT_FALSE(fs::exists(store_->options().canonical_path));
}

TEST_F(CheckpointStoreTest,
       EmbeddedVersionOverridesFilenameAndBestVersionWins) {
  writeText(checkpoint(1), "2:22\n");
  writeText(checkpoint(2), "1:11\n");
  Result<ConfigRuntime> runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  TextBackend backend;

  Result<RestoreSearch> search =
      cpPrepareRestore(*store_, backend, *runtime, 2);
  ASSERT_TRUE(search);
  ASSERT_TRUE(search->candidate);
  EXPECT_EQ(search->candidate->checkpoint_path, checkpoint(1));
  EXPECT_EQ(search->candidate->source_version, 2u);
  EXPECT_EQ(search->candidate->config.model.get<std::int64_t>("value").value(),
            22);
}

TEST_F(CheckpointStoreTest, CorruptAndNewerCheckpointsAreRejected) {
  writeText(checkpoint(1), "not a document\n");
  writeText(checkpoint(3), "3:33\n");
  Result<ConfigRuntime> runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  TextBackend backend;

  Result<RestoreSearch> search =
      cpPrepareRestore(*store_, backend, *runtime, 2);
  ASSERT_TRUE(search);
  EXPECT_FALSE(search->candidate);
  ASSERT_EQ(search->rejected.size(), 2u);
}

TEST_F(CheckpointStoreTest, FailedHigherCandidateFallsBackToOlderCheckpoint) {
  writeText(checkpoint(1), "1:11\n");
  writeText(checkpoint(2), "2:22\n");
  Result<ConfigRuntime> runtime = makeFailingV3Runtime();
  ASSERT_TRUE(runtime);
  TextBackend backend;

  Result<RestoreSearch> search =
      cpPrepareRestore(*store_, backend, *runtime, 3);
  ASSERT_TRUE(search);
  ASSERT_TRUE(search->candidate);
  EXPECT_EQ(search->candidate->source_version, 1u);
  EXPECT_EQ(search->candidate->config.version, 3u);
  ASSERT_EQ(search->rejected.size(), 1u);
  EXPECT_EQ(search->rejected.front().path, checkpoint(2));
  EXPECT_EQ(search->rejected.front().error.code, ErrorCode::MigrationFailed);
}

TEST_F(CheckpointStoreTest, EmptyCheckpointDirectoryIsNotAnError) {
  Result<ConfigRuntime> runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  TextBackend backend;

  Result<RestoreSearch> search =
      cpPrepareRestore(*store_, backend, *runtime, 2);
  ASSERT_TRUE(search);
  EXPECT_FALSE(search->candidate);
  EXPECT_TRUE(search->rejected.empty());
}

TEST_F(CheckpointStoreTest, InvalidSupportedVersionFailsEmptySearch) {
  Result<ConfigRuntime> runtime = makeRuntime();
  ASSERT_TRUE(runtime);
  TextBackend backend;

  Result<RestoreSearch> search =
      cpPrepareRestore(*store_, backend, *runtime, 99);
  ASSERT_FALSE(search);
  EXPECT_EQ(search.error().code, ErrorCode::InvalidVersion);
}

TEST_F(CheckpointStoreTest, CommitReplacesCanonicalOnlyAfterSerialization) {
  writeText(store_->options().canonical_path, "3:300\n");
  ConfigModel model;
  ASSERT_TRUE(model.set("value", 25));
  VersionedConfig restored{2, std::move(model)};
  TextBackend backend;

  Result<void> committed = cpCommit(*store_, restored, backend);
  ASSERT_TRUE(committed) << committed.error().message;
  EXPECT_EQ(readText(store_->options().canonical_path), "2:25\n");
}

TEST_F(CheckpointStoreTest, SerializationFailureLeavesCanonicalUntouched) {
  const std::string newer = "3:300\n";
  writeText(store_->options().canonical_path, newer);
  ConfigModel model;
  ASSERT_TRUE(model.set("value", 25));
  VersionedConfig restored{2, std::move(model)};
  TextBackend backend;
  backend.setFailSave(true);

  Result<void> committed = cpCommit(*store_, restored, backend);
  ASSERT_FALSE(committed);
  EXPECT_EQ(committed.error().code, ErrorCode::SerializationError);
  EXPECT_EQ(readText(store_->options().canonical_path), newer);
}

TEST_F(CheckpointStoreTest, PruneKeepsNewestConfiguredCount) {
  writeText(checkpoint(1), "1:10\n");
  writeText(checkpoint(2), "2:20\n");
  writeText(checkpoint(3), "3:30\n");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(checkpoint(1), now - std::chrono::seconds(3));
  fs::last_write_time(checkpoint(2), now - std::chrono::seconds(2));
  fs::last_write_time(checkpoint(3), now - std::chrono::seconds(1));

  TextBackend backend;
  Result<PruneReport> pruned = cpPrune(*store_, backend);
  ASSERT_TRUE(pruned) << pruned.error().message;
  ASSERT_EQ(pruned->removed.size(), 1u);
  EXPECT_EQ(pruned->removed.front(), checkpoint(1));
  EXPECT_TRUE(pruned->rejected.empty());
  EXPECT_FALSE(fs::exists(checkpoint(1)));
  EXPECT_TRUE(fs::exists(checkpoint(2)));
  EXPECT_TRUE(fs::exists(checkpoint(3)));
}

TEST_F(CheckpointStoreTest, CorruptCheckpointDoesNotBlockVerifiedPruning) {
  writeText(checkpoint(1), "1:10\n");
  writeText(checkpoint(2), "2:20\n");
  writeText(checkpoint(3), "corrupt\n");
  writeText(checkpoint(4), "4:40\n");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(checkpoint(1), now - std::chrono::seconds(4));
  fs::last_write_time(checkpoint(2), now - std::chrono::seconds(3));
  fs::last_write_time(checkpoint(3), now - std::chrono::seconds(2));
  fs::last_write_time(checkpoint(4), now - std::chrono::seconds(1));
  TextBackend backend;

  Result<PruneReport> pruned = cpPrune(*store_, backend);
  ASSERT_TRUE(pruned) << pruned.error().message;
  ASSERT_EQ(pruned->rejected.size(), 1u);
  EXPECT_EQ(pruned->rejected.front().path, checkpoint(3));
  EXPECT_EQ(pruned->rejected.front().error.code, ErrorCode::ParseError);
  ASSERT_EQ(pruned->removed.size(), 1u);
  EXPECT_EQ(pruned->removed.front(), checkpoint(1));
  EXPECT_FALSE(fs::exists(checkpoint(1)));
  EXPECT_TRUE(fs::exists(checkpoint(2)));
  EXPECT_TRUE(fs::exists(checkpoint(3)));
  EXPECT_TRUE(fs::exists(checkpoint(4)));
}

TEST_F(CheckpointStoreTest, ZeroRetentionRemovesVerifiedCheckpoints) {
  Result<CheckpointStore> zero_retention =
      cpCreate({root_ / "config.cfg", root_ / "zero-retention", 0});
  ASSERT_TRUE(zero_retention);
  writeText(root_ / "zero-retention" / "0000000001.cfg", "1:10\n");
  TextBackend backend;

  Result<PruneReport> pruned = cpPrune(*zero_retention, backend);
  ASSERT_TRUE(pruned);
  ASSERT_EQ(pruned->removed.size(), 1u);
  EXPECT_FALSE(fs::exists(root_ / "zero-retention" / "0000000001.cfg"));
}

TEST_F(CheckpointStoreTest, CommitPreservesCanonicalPermissions) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX mode bits are not available on Windows";
#else
  writeText(store_->options().canonical_path, "3:300\n");
  ASSERT_EQ(::chmod(store_->options().canonical_path.c_str(), 0600), 0);
  ConfigModel model;
  ASSERT_TRUE(model.set("value", 25));
  TextBackend backend;
  ASSERT_TRUE(cpCommit(*store_, VersionedConfig{2, std::move(model)}, backend));

  struct stat file_stat {};
  ASSERT_EQ(::stat(store_->options().canonical_path.c_str(), &file_stat), 0);
  EXPECT_EQ(file_stat.st_mode & 0777, 0600);
#endif
}

}  // namespace
}  // namespace configmanager

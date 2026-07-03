#include "configmanager/migration_engine.hpp"

#include <exception>
#include <new>
#include <string>

namespace configmanager {

namespace {

std::string stepName(VersionId from, VersionId to) {
  return std::to_string(from) + "->" + std::to_string(to);
}

const char* errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::InvalidPath:
      return "InvalidPath";
    case ErrorCode::NodeNotFound:
      return "NodeNotFound";
    case ErrorCode::InvalidType:
      return "InvalidType";
    case ErrorCode::ParseError:
      return "ParseError";
    case ErrorCode::SerializationError:
      return "SerializationError";
    case ErrorCode::MigrationFailed:
      return "MigrationFailed";
    case ErrorCode::MissingMigration:
      return "MissingMigration";
    case ErrorCode::InvalidVersion:
      return "InvalidVersion";
  }
  return "UnknownError";
}

}  // namespace

Result<void> MigrationEngine::migrate(VersionedConfig& config,
                                      VersionId target) {
  if (!catalog_.contains(target)) {
    return fail(ErrorCode::InvalidVersion,
                "migration target version " + std::to_string(target) +
                    " is not registered");
  }
  if (config.version > target) {
    return fail(ErrorCode::InvalidVersion,
                "cannot migrate backward from version " +
                    std::to_string(config.version) + " to " +
                    std::to_string(target));
  }
  VersionId current = config.version;
  while (current < target) {
    const Result<VersionId> next = catalog_.nextVersion(current);
    if (!next) {
      return fail(next.error().code, std::string(next.error().message));
    }
    const Result<const MigrationEdge*> edge =
        registry_.findMigration(current, *next);
    if (!edge) {
      return fail(edge.error().code, std::string(edge.error().message));
    }
    // Built per step; only the engine may construct contexts (§8.1).
    MigrationContext ctx(config.model, current, *next);
    // Copied out of the registry: a migration that (perversely) registers
    // new edges would reallocate the registry's storage and destroy the
    // std::function mid-execution.
    const MigrationFn apply = (*edge)->apply;
    Result<void> applied;
    try {
      applied = apply(ctx);
    } catch (const std::bad_alloc&) {
      throw;  // memory exhaustion is not a recoverable config error (ADR-018)
    } catch (const std::exception& e) {
      return fail(ErrorCode::MigrationFailed,
                  "migration " + stepName(current, *next) +
                      " threw: " + e.what());
    } catch (...) {
      return fail(ErrorCode::MigrationFailed,
                  "migration " + stepName(current, *next) +
                      " threw a non-standard exception");
    }
    if (!applied) {
      // One code for any failed step; the underlying cause survives in the
      // message, not the code (§8.2).
      return fail(ErrorCode::MigrationFailed,
                  "migration " + stepName(current, *next) +
                      " failed: " + errorCodeName(applied.error().code) +
                      ": " + applied.error().message);
    }
    current = *next;
    config.version = current;  // version advances per step
  }
  return {};
}

}  // namespace configmanager

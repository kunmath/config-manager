#ifndef CONFIGMANAGER_RESULT_HPP_
#define CONFIGMANAGER_RESULT_HPP_

#include <string>
#include <utility>

#include <tl/expected.hpp>

namespace configmanager {

enum class ErrorCode {
  InvalidPath,
  NodeNotFound,
  InvalidType,
  ParseError,
  SerializationError,
  MigrationFailed,
  MissingMigration,
  InvalidVersion,
};

struct Error {
  ErrorCode code;
  std::string message;  // owned: richer diagnostics, no lifetime issues
};

// Only this header references the underlying expected implementation, so a
// later migration to std::expected touches a single file.
template <typename T>
using Result = tl::expected<T, Error>;

[[nodiscard]] inline tl::unexpected<Error> fail(ErrorCode code,
                                                std::string message) {
  return tl::unexpected<Error>(Error{code, std::move(message)});
}

}  // namespace configmanager

#endif  // CONFIGMANAGER_RESULT_HPP_

#ifndef CONFIGMANAGER_CONFIG_PATH_HPP_
#define CONFIGMANAGER_CONFIG_PATH_HPP_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "configmanager/result.hpp"

namespace configmanager {

struct PathSegment {
  enum class Kind { Key, Index };

  Kind kind;
  std::string key;        // when Kind::Key
  std::size_t index = 0;  // when Kind::Index
};

// One grammar implementation shared by every component:
//
//   path    := segment ( "." segment )*
//   segment := key index*
//   key     := char+            ; char = any byte except "." "[" "]"
//   index   := "[" digit+ "]"   ; decimal digits only
//
// The grammar is total — anything it does not produce fails with
// InvalidPath: empty paths and keys, a leading index, empty or non-decimal
// indices, text directly after "]" other than ".", "[", or end-of-path, and
// indices that overflow std::size_t. Whitespace is never trimmed; it is an
// ordinary key character. No escaping in v1.
class ConfigPath {
 public:
  static Result<ConfigPath> parse(std::string_view text);

  const std::vector<PathSegment>& segments() const noexcept {
    return segments_;
  }

 private:
  ConfigPath() = default;

  std::vector<PathSegment> segments_;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_CONFIG_PATH_HPP_

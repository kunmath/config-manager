#include "configmanager/config_path.hpp"

#include <charconv>
#include <system_error>

namespace configmanager {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isReserved(char c) { return c == '.' || c == '[' || c == ']'; }

std::string quoted(std::string_view text) {
  return "\"" + std::string(text) + "\"";
}

}  // namespace

Result<ConfigPath> ConfigPath::parse(std::string_view text) {
  if (text.empty()) {
    return fail(ErrorCode::InvalidPath, "empty path");
  }

  ConfigPath path;
  std::size_t pos = 0;
  const std::size_t size = text.size();

  while (true) {
    // key := char+ ; any byte except "." "[" "]"
    const std::size_t keyStart = pos;
    while (pos < size && !isReserved(text[pos])) {
      ++pos;
    }
    if (pos == keyStart) {
      return fail(ErrorCode::InvalidPath,
                  "empty key at offset " + std::to_string(pos) + " in " +
                      quoted(text));
    }
    PathSegment key;
    key.kind = PathSegment::Kind::Key;
    key.key = std::string(text.substr(keyStart, pos - keyStart));
    path.segments_.push_back(std::move(key));

    // index* := ( "[" digit+ "]" )*
    while (pos < size && text[pos] == '[') {
      ++pos;  // consume '['
      const std::size_t digitsStart = pos;
      while (pos < size && isDigit(text[pos])) {
        ++pos;
      }
      if (pos == digitsStart) {
        return fail(ErrorCode::InvalidPath,
                    "index must be decimal digits at offset " +
                        std::to_string(digitsStart) + " in " + quoted(text));
      }
      if (pos == size || text[pos] != ']') {
        return fail(ErrorCode::InvalidPath,
                    "unterminated or non-decimal index at offset " +
                        std::to_string(digitsStart) + " in " + quoted(text));
      }

      PathSegment index;
      index.kind = PathSegment::Kind::Index;
      const char* first = text.data() + digitsStart;
      const char* last = text.data() + pos;
      const auto conversion = std::from_chars(first, last, index.index);
      if (conversion.ec != std::errc()) {
        return fail(ErrorCode::InvalidPath,
                    "index overflows std::size_t in " + quoted(text));
      }
      path.segments_.push_back(std::move(index));
      ++pos;  // consume ']'
    }

    if (pos == size) {
      break;
    }
    if (text[pos] == '.') {
      ++pos;  // consume '.'; the grammar requires a segment to follow
      continue;
    }
    // A "]" without a matching "[", or any byte directly after "]" other
    // than ".", "[", or end-of-path.
    return fail(ErrorCode::InvalidPath,
                "unexpected '" + std::string(1, text[pos]) + "' at offset " +
                    std::to_string(pos) + " in " + quoted(text));
  }

  return path;
}

}  // namespace configmanager

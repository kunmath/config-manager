#ifndef CONFIGMANAGER_LOAD_LIMITS_HPP_
#define CONFIGMANAGER_LOAD_LIMITS_HPP_

#include <cstddef>

namespace configmanager {

// Resource limits applied while loading an untrusted serialized document.
// maxInputBytes is checked before any parsing and bounds every string, key,
// and container; maxNodes (which includes the model's root) bounds the model
// a small input can expand into. Callers with a tighter memory budget can pass
// a smaller instance to a backend's constructor. A DOM-based parser can use
// many times the input size before maxNodes is checked (the XML backend peaks
// at roughly 20x), so maxInputBytes is the primary memory bound.
struct LoadLimits {
  std::size_t maxInputBytes = 4 * 1024 * 1024;
  std::size_t maxNodes = 100'000;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_LOAD_LIMITS_HPP_

// Example 01 — the model is a typed tree, errors are values.
//
// Introduces the two core types every consumer touches first:
//
//   * ConfigValue — a plain, copyable builder for describing a configuration
//     tree (objects, arrays, scalars). It has no path access and no error
//     handling; it only describes shape and content.
//   * ConfigModel — the owning tree the library operates on. All access goes
//     through dotted paths ("network.host", "network.ports[0]"), and every
//     fallible call returns Result<T> (tl::expected) instead of throwing.
//
// No versioning and no file I/O yet — those are examples 02 and 03.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

#include "configmanager/configmanager.hpp"  // umbrella header, defines cfg::

namespace {

// The standard consumer idiom for calls that are expected to succeed: unwrap
// the Result or exit with the library's diagnostic. Errors are ordinary
// values — `result.error()` carries an ErrorCode plus a human-readable
// message — so "handling" one is a branch, not a try/catch.
template <typename T>
T orDie(cfg::Result<T> result, const std::string& what) {
  if (!result) {
    std::cerr << what << " failed: " << result.error().message << "\n";
    std::exit(EXIT_FAILURE);
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(result).value();
  }
}

}  // namespace

int main() {
  // ---- 1. Describe the tree with ConfigValue -------------------------------
  //
  // ConfigValue::object()/array() make containers, ConfigValue::of<T>() makes
  // scalars (bool, integers -> Int, floating point -> Double, strings).
  // set() and push() return the value itself, so building can be chained.
  cfg::ConfigValue ports = cfg::ConfigValue::array();
  ports.push(cfg::ConfigValue::of(8080)).push(cfg::ConfigValue::of(8443));

  cfg::ConfigValue network = cfg::ConfigValue::object();
  network.set("host", cfg::ConfigValue::of(std::string("localhost")));
  network.set("ports", std::move(ports));

  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("app_name", cfg::ConfigValue::of(std::string("demo")));
  root.set("retries", cfg::ConfigValue::of(3));
  root.set("network", std::move(network));

  // ---- 2. Adopt it into a ConfigModel ---------------------------------------
  //
  // fromValue() is a fallible boundary: it rejects a non-object root and keys
  // that would not be addressable by the path grammar (empty, or containing
  // '.', '[', ']'). The model owns its storage; it is move-only.
  cfg::ConfigModel model =
      orDie(cfg::ConfigModel::fromValue(std::move(root)), "fromValue");

  // ---- 3. Read through paths ------------------------------------------------
  //
  // get<T>() resolves the path and converts the scalar in one call. Array
  // elements are addressed with [index]. Conversions are strict and lossless:
  // an Int reads back as any integer type it fits in, but never as a string.
  const auto host = orDie(model.get<std::string>("network.host"),
                          "get network.host");
  const auto firstPort = orDie(model.get<std::int64_t>("network.ports[0]"),
                               "get network.ports[0]");
  std::cout << "host = " << host << ", first port = " << firstPort << "\n";

  // ---- 4. Write through paths (upsert) --------------------------------------
  //
  // set() updates an existing node or creates the path — including missing
  // intermediate objects — in one atomic step. Here "retries" exists and is
  // updated; "network.timeout_ms" does not exist yet and is created.
  orDie(model.set("retries", 5), "set retries");
  std::cout << "network.timeout_ms present before set: "
            << model.contains("network.timeout_ms") << "\n";
  orDie(model.set("network.timeout_ms", 2500), "set network.timeout_ms");
  std::cout << "network.timeout_ms present after set:  "
            << model.contains("network.timeout_ms") << "\n";

  // remove() detaches a subtree; contains() never fails (a malformed or
  // absent path is simply "not there").
  orDie(model.remove("network.timeout_ms"), "remove network.timeout_ms");
  std::cout << "network.timeout_ms present after remove: "
            << model.contains("network.timeout_ms") << "\n";

  // ---- 5. Errors are values, part one: cross-type writes --------------------
  //
  // Upsert is type-preserving: it creates structure but never changes an
  // existing node's type. "retries" is an Int, so writing a string to it must
  // fail with InvalidType — and, because writes are atomic, the model is
  // untouched. Success here would be a bug, hence the inverted check.
  auto crossType = model.set("retries", std::string("many"));
  if (crossType.has_value() ||
      crossType.error().code != cfg::ErrorCode::InvalidType) {
    std::cerr << "expected the cross-type set to fail with InvalidType\n";
    return EXIT_FAILURE;
  }
  std::cout << "cross-type set rejected: " << crossType.error().message << "\n";
  std::cout << "retries still an Int: "
            << orDie(model.get<std::int64_t>("retries"), "get retries") << "\n";

  // ---- 6. Errors are values, part two: absent paths -------------------------
  //
  // Reading a path that does not exist is not exceptional either — it is a
  // NodeNotFound value the caller can branch on (e.g. to apply a fallback).
  auto absent = model.get<std::int64_t>("network.proxy.port");
  if (absent.has_value() ||
      absent.error().code != cfg::ErrorCode::NodeNotFound) {
    std::cerr << "expected the absent-path get to fail with NodeNotFound\n";
    return EXIT_FAILURE;
  }
  std::cout << "absent path rejected: " << absent.error().message << "\n";

  std::cout << "01_basic_model: all steps behaved as expected\n";
  return EXIT_SUCCESS;
}

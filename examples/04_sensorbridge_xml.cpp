// Example 04 — the XML backend end-to-end, on a real file.
//
// "sensorbridge" is a fictional telemetry poller whose config lives in XML.
// The runtime pieces (catalog, migrations, synchronize) are exactly the ones
// from examples 02/03 — IConfigInterface implementations are interchangeable,
// so swapping JSON for XML changes only the two functions that touch bytes.
// What is new here is everything XML-specific:
//
//   * the wire mapping (docs/HighLevelDesign.md §6.1): elements only; a `type`
//     attribute marks non-string scalars (absent means string); objects are
//     untyped elements with children; arrays carry type="array" and hold
//     <item> children;
//   * the version carrier is a root *attribute* (<config version="N">), not
//     a member of the model — so unlike JSON's "__version", a config key
//     literally named "version" is plain data;
//   * save-side validation: XML element names are narrower than model keys,
//     so a key like "2fast" (fine in JSON) fails save() with
//     SerializationError — as a Result value, never an exception;
//   * member order is preserved end-to-end (ADR-022), so load -> save is
//     byte-stable.
//
// sensorbridge's schema has two versions:
//
//   v1 (flat):      device_name, poll_interval_ms, notes, last_error,
//                   sensors[] of {id, enabled, scale}
//   v2 (sectioned): device.name, polling.{interval_ms,jitter_pct},
//                   notes, last_error, sensors[]   [jitter_pct is new in v2]
//
// The run: load a hand-authored v1 file with operator overrides, upgrade it
// to v2 (overrides survive, the new key is repaired in), save and print the
// result, then walk the XML-specific corners above.
//
// Usage: 04_sensorbridge_xml [data_dir] [out_dir]
//   data_dir (default ./data): ships sensorbridge_v1.xml
//   out_dir  (default ./out):  everything this run writes

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "configmanager/backends/xml_interface.hpp"
#include "configmanager/configmanager.hpp"  // umbrella header, defines cfg::

namespace fs = std::filesystem;

namespace {

// Unwrap a Result or exit with the library's diagnostic (see example 01).
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

// Unwrap-and-compare: the phases below assert concrete outcomes so this
// example doubles as a smoke test — exit 0 means every phase behaved.
template <typename T>
void expectEq(cfg::Result<T> actual, const T& expected,
              const std::string& what) {
  const T value = orDie(std::move(actual), what);
  if (value != expected) {
    std::cerr << what << ": expected '" << expected << "', got '" << value
              << "'\n";
    std::exit(EXIT_FAILURE);
  }
}

// ---- Default factories --------------------------------------------------------
//
// One complete default tree per version; repair fills missing keys from
// these. The sensors array's default is a single built-in sensor — an array
// the file overrides wholesale.

cfg::ConfigValue defaultSensors() {
  cfg::ConfigValue sensor = cfg::ConfigValue::object();
  sensor.set("id", cfg::ConfigValue::of(std::string("internal")));
  sensor.set("enabled", cfg::ConfigValue::of(true));
  sensor.set("scale", cfg::ConfigValue::of(1.0));
  return cfg::ConfigValue::array().push(std::move(sensor));
}

// v1: everything flat at the root. last_error defaults to Null — on the wire
// that is <last_error type="null"/>.
cfg::ConfigValue v1Defaults() {
  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("device_name", cfg::ConfigValue::of(std::string("sensorbridge")));
  root.set("poll_interval_ms", cfg::ConfigValue::of(1000));
  root.set("notes", cfg::ConfigValue::of(std::string("")));
  root.set("last_error", cfg::ConfigValue());  // Null
  root.set("sensors", defaultSensors());
  return root;
}

// v2: "device" and "polling" sections; polling.jitter_pct is new in v2.
cfg::ConfigValue v2Defaults() {
  cfg::ConfigValue device = cfg::ConfigValue::object();
  device.set("name", cfg::ConfigValue::of(std::string("sensorbridge")));

  cfg::ConfigValue polling = cfg::ConfigValue::object();
  polling.set("interval_ms", cfg::ConfigValue::of(1000));
  polling.set("jitter_pct", cfg::ConfigValue::of(5));  // new in v2

  cfg::ConfigValue root = cfg::ConfigValue::object();
  root.set("device", std::move(device));
  root.set("polling", std::move(polling));
  root.set("notes", cfg::ConfigValue::of(std::string("")));
  root.set("last_error", cfg::ConfigValue());  // Null
  root.set("sensors", defaultSensors());
  return root;
}

// ---- Migration 1 -> 2 ----------------------------------------------------------
//
// Moves the operator's data into the new sections; polling.jitter_pct is new
// in v2 with no v1 data to carry over, so repair fills it (see example 02).

cfg::Result<void> moveKey(cfg::ConfigModel& model, std::string_view from,
                          std::string_view to) {
  auto value = model.getValue(from);
  if (!value) {
    return cfg::fail(value.error().code, std::move(value.error().message));
  }
  if (auto put = model.set(to, *std::move(value)); !put) {
    return put;
  }
  return model.remove(from);
}

cfg::Result<void> migrateV1ToV2(cfg::MigrationContext& ctx) {
  cfg::ConfigModel& model = ctx.model();
  if (auto moved = moveKey(model, "device_name", "device.name"); !moved) {
    return moved;
  }
  return moveKey(model, "poll_interval_ms", "polling.interval_ms");
}

// ---- File I/O over the serialization boundary -----------------------------------
//
// Identical to example 03 with XmlInterface in place of JsonInterface —
// that one-word diff is the point of the IConfigInterface boundary.

cfg::Result<cfg::VersionedConfig> loadFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return cfg::fail(cfg::ErrorCode::ParseError,
                     "cannot open " + path.string() + " for reading");
  }
  cfg::XmlInterface backend;
  return backend.load(in);
}

cfg::Result<void> saveFile(const cfg::VersionedConfig& config,
                           const fs::path& path) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return cfg::fail(cfg::ErrorCode::SerializationError,
                     "cannot open " + path.string() + " for writing");
  }
  cfg::XmlInterface backend;
  return backend.save(config, out);
}

std::string toXmlText(const cfg::VersionedConfig& config) {
  std::ostringstream out;
  cfg::XmlInterface backend;
  orDie(backend.save(config, out), "serialize for printing");
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path dataDir = argc > 1 ? fs::path(argv[1]) : fs::path("data");
  const fs::path outDir = argc > 2 ? fs::path(argv[2]) : fs::path("out");
  const fs::path configFile = outDir / "sensorbridge.xml";

  constexpr cfg::VersionId kLatest = 2;
  cfg::VersionCatalog catalog;
  orDie(catalog.registerVersion({1, v1Defaults}), "register v1");
  orDie(catalog.registerVersion({2, v2Defaults}), "register v2");
  cfg::MigrationRegistry registry;
  orDie(registry.registerMigration(1, 2, migrateV1ToV2), "register 1->2");
  cfg::ConfigRuntime runtime = orDie(
      cfg::ConfigRuntime::create(std::move(catalog), std::move(registry)),
      "ConfigRuntime::create");

  fs::create_directories(outDir);

  // ---- Phase 1: load a hand-authored XML file --------------------------------
  // sensorbridge_v1.xml was written by hand (open it alongside this code).
  // load() consumes the root's version attribute into config.version and
  // maps elements by their `type` attribute: absent -> string, and the
  // typed reads below only succeed because the file said type="int",
  // type="bool", type="double" where it meant those.
  std::cout << "[phase 1] load hand-authored "
            << (dataDir / "sensorbridge_v1.xml") << "\n";
  cfg::VersionedConfig config =
      orDie(loadFile(dataDir / "sensorbridge_v1.xml"), "load v1 file");
  if (config.version != 1) {
    std::cerr << "expected the file's root attribute to say version 1\n";
    return EXIT_FAILURE;
  }
  expectEq(config.model.get<std::string>("device_name"),
           std::string("bench-rig-07"), "device_name (string: no type attr)");
  expectEq(config.model.get<std::int64_t>("poll_interval_ms"),
           std::int64_t{250}, "poll_interval_ms (type=\"int\")");
  // The sensors array arrived as <item> children, addressable by index.
  expectEq(config.model.get<std::string>("sensors[1].id"),
           std::string("thermo-b"), "sensors[1].id");
  expectEq(config.model.get<bool>("sensors[1].enabled"), false,
           "sensors[1].enabled (type=\"bool\")");
  expectEq(config.model.get<double>("sensors[1].scale"), 2.5,
           "sensors[1].scale (type=\"double\")");

  // ---- Phase 2: upgrade v1 -> v2 ----------------------------------------------
  // Backend-agnostic from here: the runtime never sees XML. The migration
  // moves the operator's overrides; repair fills the new-in-v2 key.
  std::cout << "[phase 2] synchronize v1 -> v" << kLatest << "\n";
  const cfg::SyncState state = runtime.inspect(config, kLatest);
  if (state.status != cfg::SyncStatus::UpgradeRequired) {
    std::cerr << "expected UpgradeRequired for the v1 file\n";
    return EXIT_FAILURE;
  }
  orDie(runtime.synchronize(config, kLatest), "synchronize");
  expectEq(config.model.get<std::string>("device.name"),
           std::string("bench-rig-07"), "device.name moved by migration");
  expectEq(config.model.get<std::int64_t>("polling.interval_ms"),
           std::int64_t{250}, "polling.interval_ms moved by migration");
  expectEq(config.model.get<std::int64_t>("polling.jitter_pct"),
           std::int64_t{5}, "polling.jitter_pct filled by repair");

  // ---- Phase 3: save, and read the emitted XML --------------------------------
  // save() writes the carrier back as the root attribute — the document now
  // opens with <config version="2"> — and re-emits each scalar with the
  // `type` attribute its ConfigValue type dictates.
  std::cout << "[phase 3] save " << configFile << "\n";
  orDie(saveFile(config, configFile), "save upgraded config");
  const std::string emitted = toXmlText(config);
  std::cout << emitted << "\n";
  if (emitted.find("<config version=\"2\">") == std::string::npos ||
      emitted.find("<jitter_pct type=\"int\">5</jitter_pct>") ==
          std::string::npos) {
    std::cerr << "emitted XML is missing the carrier or the repaired key\n";
    return EXIT_FAILURE;
  }

  // ---- Phase 4: a key named "version" is plain data ---------------------------
  // JSON smuggles the carrier through the model as "__version", which makes
  // that name reserved there. XML's carrier is an attribute, outside the
  // element namespace, so "version" is an ordinary key that round-trips as
  // data while config.version rides the attribute.
  std::cout << "[phase 4] a member named \"version\" is not special in XML\n";
  {
    orDie(config.model.set("version", std::string("the operator wrote this")),
          "set data key named version");
    std::istringstream in(toXmlText(config));
    cfg::XmlInterface backend;
    cfg::VersionedConfig reloaded = orDie(backend.load(in),
                                          "reload with version member");
    expectEq(reloaded.model.get<std::string>("version"),
             std::string("the operator wrote this"),
             "\"version\" member survives as data");
    if (reloaded.version != kLatest) {
      std::cerr << "the carrier attribute should still say v2\n";
      return EXIT_FAILURE;
    }
    orDie(config.model.remove("version"), "remove demo key");
  }

  // ---- Phase 5: save-side element-name validation ------------------------------
  // Model keys are freer than XML element names: "2fast" is a perfectly
  // addressable key (example 01's paths would take it) but cannot open an
  // XML tag. The failure is a SerializationError Result from save() — the
  // model itself accepts the key, and nothing is written.
  std::cout << "[phase 5] a key XML cannot spell fails save(), as a value\n";
  {
    orDie(config.model.set("2fast", cfg::ConfigValue::of(true)),
          "set key that is not an XML name");
    std::ostringstream sink;
    cfg::XmlInterface backend;
    cfg::Result<void> saved = backend.save(config, sink);
    if (saved || saved.error().code != cfg::ErrorCode::SerializationError) {
      std::cerr << "expected save() to fail with SerializationError\n";
      return EXIT_FAILURE;
    }
    std::cout << "save() said: " << saved.error().message << "\n";
    orDie(config.model.remove("2fast"), "remove demo key");
  }

  // ---- Phase 6: order-preserving round-trip ------------------------------------
  // Member order is part of the model (ADR-022) and both directions of the
  // XML mapping keep it, so serialize -> load -> serialize is byte-stable:
  // an operator's file layout is not shuffled by a load/save cycle.
  std::cout << "[phase 6] load(save(x)) re-serializes byte-identically\n";
  {
    cfg::VersionedConfig reloaded = orDie(loadFile(configFile),
                                          "reload own output");
    if (toXmlText(reloaded) != toXmlText(config)) {
      std::cerr << "round-trip changed the serialized document\n";
      return EXIT_FAILURE;
    }
  }

  std::cout << "04_sensorbridge_xml: all phases behaved as expected\n";
  return EXIT_SUCCESS;
}

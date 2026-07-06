# Serialization backends

Backends translate between streams and `VersionedConfig`. They implement one
interface:

```cpp
class IConfigInterface {
 public:
  virtual ~IConfigInterface() = default;
  virtual Result<VersionedConfig> load(std::istream& in) = 0;
  virtual Result<void> save(const VersionedConfig& config, std::ostream& out) = 0;
};
```

Backends operate on streams, never filesystem paths
([ADR-004](Architecture.md#adr-004)) — opening files, sockets, or memory
buffers is the application's business.

## Available backends

| Format | Header | Target | Version carrier | Reserved model path |
|---|---|---|---|---|
| JSON | `configmanager/backends/json_interface.hpp` | `configmanager::json` | top-level `"__version"` | `__version` |
| XML | `configmanager/backends/xml_interface.hpp` | `configmanager::xml` | root attribute `<config version="N">` | none (attributes are outside the key space) |
| YAML *(planned)* | — | `configmanager::yaml` | top-level `version:` | `version` |
| INI *(planned)* | — | `configmanager::ini` | `[meta] version=N` | `meta.version` |

All backends are interchangeable: swapping JSON for XML in
[`examples/03`](../examples/03_relaygate_lifecycle.cpp) vs
[`examples/04`](../examples/04_sensorbridge_xml.cpp) changes only the two
functions that touch bytes.

## The contract every backend honors

These rules are what make backends interchangeable; each links to its
rationale.

* **`load()` only parses** — no repair, no migration, no modification
  ([ADR-013](Architecture.md#adr-013)).
* **The version is mandatory.** No version metadata in the stream →
  `InvalidVersion`. The carrier must parse as an unsigned decimal integer
  representable in `VersionId` (`std::uint32_t`); malformed or out-of-range
  values are `InvalidVersion` too. Backends never guess
  ([ADR-014](Architecture.md#adr-014)).
* **The version carrier is reserved** ([ADR-020](Architecture.md#adr-020)).
  `load()` consumes it — it never appears in the model — and `save()` writes it
  from `VersionedConfig::version`. If the model itself contains the reserved
  path (see table above), `save()` fails with `SerializationError` rather than
  emitting ambiguous output.
* **The document root must be an object**; anything else (e.g. a top-level
  JSON array) fails `load()` with `ParseError`.
* **Object keys are unique.** Duplicate keys / repeated sibling elements /
  duplicate INI sections fail `load()` with `ParseError` — never last-wins.
* **Keys must be path-addressable** — non-empty, no `.` `[` `]` — or `load()`
  fails with `ParseError` ([ADR-021](Architecture.md#adr-021)).
* **Formats differ in power; backends never guess.** A model the format cannot
  represent fails `save()` with `SerializationError`; a document the model
  cannot represent fails `load()` with `ParseError`.
* **Backends never throw** ([ADR-018](Architecture.md#adr-018)). Exceptions
  from parser libraries or stream operations (including streams with exception
  masks set) are caught inside and mapped to `ParseError` in `load()` /
  `SerializationError` in `save()`. Only `std::bad_alloc` propagates.
* **Member order is preserved** end-to-end
  ([ADR-022](Architecture.md#adr-022)): a document keeps its author's key
  order through load → mutate → save.

## Format mappings

The authoritative, normative mapping for every format is
[HighLevelDesign.md §6.1](HighLevelDesign.md#61-format-mappings). Highlights:

### JSON

Native 1:1 mapping via `nlohmann::ordered_json`. Integral numbers map to
`Int`, all others to `Double`; a number outside `std::int64_t`'s range fails
`load()` with `ParseError` (strictness over silent precision loss).

### XML

Restricted, elements-only mapping (XML has no native arrays or types):

```xml
<config version="3">
  <host>127.0.0.1</host>                 <!-- string: no type attribute -->
  <port type="int">8080</port>
  <tls type="object">                    <!-- only *empty* objects are marked -->
  </tls>
  <endpoints type="array">
    <item>/health</item>
    <item>/metrics</item>
  </endpoints>
</config>
```

* Scalars are element text with a reserved `type` attribute (`bool`, `int`,
  `double`, `null`; absent means `string`). Literals are strict and untrimmed.
* An untyped element with children is an `Object`; an empty object carries
  `type="object"` to stay distinguishable from the empty string.
* Arrays carry `type="array"` and hold `<item>` children.
* Any other attribute, mixed content, unknown `type` value, or a root not
  named `<config>` fails `load()` with `ParseError`. A model key that is not a
  valid XML element name (e.g. `2fast`) fails `save()` with
  `SerializationError`.
* Because the carrier is a root *attribute*, a model member literally named
  `version` is plain data — unlike JSON's `__version`.

## Writing your own backend

Any format (TOML, protobuf text, a database row format, …) can join the
library by implementing `IConfigInterface`. The checklist:

1. **Decide the version carrier** — a reserved location in your format that
   `load()` consumes and `save()` writes from `VersionedConfig::version`.
   Document its reserved model path, and make `save()` fail with
   `SerializationError` when the model already contains it (skip this only if,
   like an XML attribute, the carrier lives outside the model's key space).
2. **Define the mapping** to the model's types (`Null`, `Bool`,
   `Int`/`std::int64_t`, `Double`, `String`, `Object`, `Array`). Close every
   expressiveness gap in both directions with an error — `SerializationError`
   for unrepresentable models, `ParseError` for unrepresentable documents.
   Never guess, never coerce.
3. **Build the tree** with `ConfigValue` (insertion-ordered by construction)
   and adopt it via `ConfigModel::fromValue()`, which enforces the
   object-root and path-addressable-key rules for you (`InvalidType` /
   `InvalidPath` — map these to `ParseError` diagnostics at your boundary).
4. **Enforce, on load:** mandatory strictly-parsed version carrier
   (`InvalidVersion`), object document root (`ParseError`), unique keys
   (`ParseError` — your parser may resolve duplicates silently; detect them
   yourself, e.g. the JSON backend parses SAX-style for exactly this reason),
   preserved member order (pick an order-preserving parser mode).
5. **Wrap everything in the non-throwing boundary.** Catch in this order:
   `std::bad_alloc` — rethrow; `std::exception` — map to
   `ParseError`/`SerializationError` preserving `what()`; anything else — same
   code with a fixed message. No exception other than `std::bad_alloc` may
   escape `load()`/`save()`, including from streams with exception masks set.
6. **Test the round trip.** The per-backend suite in
   [`../tests/`](../tests/) (`json_interface_test.cpp`,
   `xml_interface_test.cpp`) shows the expected coverage:
   `load → save → load` equality, missing/malformed carrier, duplicate keys,
   non-addressable keys, throwing streams, unrepresentable values in both
   directions, member-order preservation.

For a template, read the JSON backend (`backends/json/`) for a native mapping
or the XML backend (`backends/xml/`) for a restricted one, plus each backend's
CMake wiring (opt-in option, dependency in `cmake/Dependencies.cmake`, install
into `ConfigManagerTargets`).

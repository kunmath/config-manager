# Contributing to ConfigManager

Thank you for considering a contribution! Ideas, bug reports, documentation
fixes, examples, and code are all welcome — you don't need to write C++ to
make this library better.

This guide covers the mechanics. The short version: **discuss before you build,
align with the design docs, bring tests.**

## Ways to contribute

* **Report a bug** — even without a fix, a good report is a contribution.
* **Propose an idea or feature** — open an issue first (see below); design
  discussion up front saves everyone a rewritten PR.
* **Improve the documentation** — unclear guide sections, missing FAQ entries,
  and better examples are always fair game.
* **Pick up planned work** — the YAML and INI backends are specified but not
  yet implemented (see
  [docs/HighLevelDesign.md §6.1](docs/HighLevelDesign.md#61-format-mappings)
  and the
  [writing-your-own-backend checklist](docs/serialization-backends.md#writing-your-own-backend));
  the JSON and XML backends are the structural templates.

## Before proposing a feature: read the design docs

This project is design-driven. Two documents are normative:

* [docs/Architecture.md](docs/Architecture.md) — principles and the
  Architectural Decision Records (ADR-001 … ADR-022). **ADRs are binding**:
  a proposal that contradicts one (say, adding downgrade migrations against
  ADR-008, or implicit synchronization against ADR-002) needs to argue for
  changing the ADR itself, not just add code.
* [docs/HighLevelDesign.md](docs/HighLevelDesign.md) — the implementation
  contract: concrete types, format mappings, and the error-code table.

Also note what ConfigManager is intentionally **not** (schema validation,
serialization framework, storage framework — see the
[README](README.md) and [docs/limitations.md](docs/limitations.md)). Features
in those directions will be declined regardless of code quality, so ask first.

**Process:** open an issue describing the problem you're solving (not just the
API you'd like), how it fits or changes the ADRs, and a sketch of the public
surface. Once there's agreement, PRs go smoothly.

## Reporting bugs

Please include:

* A **minimal reproduction** — ideally a short `main()` against the public
  API, or a failing GoogleTest case.
* What you expected vs. what happened (including the `Error::code` and
  `Error::message` you got, if any).
* Compiler and version, platform, CMake version, and the ConfigManager
  commit/tag.
* For backend issues: the exact input document (trimmed as far as possible).

If the behavior contradicts the documentation, say which document — a doc bug
is a valid outcome.

## Pull requests

* **Keep PRs focused.** One concern per PR; mechanical refactors separate from
  behavior changes.
* **Base on `main`** unless you're stacking on an open feature branch, and say
  so if you are.
* **Tests are required** for behavior changes. The suite is GoogleTest, one
  file per component under `tests/`; backend changes extend the per-format
  round-trip suite (see `tests/json_interface_test.cpp` /
  `tests/xml_interface_test.cpp` for the expected coverage, and
  [docs/HighLevelDesign.md §12](docs/HighLevelDesign.md#12-testing-strategy-data-model--api-first)
  for the test matrix).
* **Update the docs you touch.** A behavior change usually lands in one of the
  [docs/ guides](docs/README.md); a contract change must also update
  Architecture.md / HighLevelDesign.md — flag that explicitly in the PR, since
  it needs design review.
* **Examples must keep passing** — they assert their own outcomes and run in
  CI via CTest.
* CI builds GCC and Clang, Debug and Release; PRs need a green matrix.

### Building and testing

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure     # unit tests + examples
```

Useful option matrix checks when touching CMake or backends:
`-DCONFIGMANAGER_BUILD_JSON=OFF`, `-DCONFIGMANAGER_BUILD_XML=OFF`,
`-DCONFIGMANAGER_BUILD_TESTS=OFF -DCONFIGMANAGER_BUILD_EXAMPLES=ON`, and
`-DCONFIGMANAGER_USE_SYSTEM_DEPS=ON` if you have the deps installed.

### Code style

* [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html),
  C++20, `-Wall -Wextra -Wpedantic` clean on GCC and Clang.
* **Public APIs never throw.** Fallible operations return `Result<T>`; foreign
  code (callbacks, parser libraries, streams) is wrapped at the boundary with
  the fixed catch discipline of
  [ADR-018](docs/Architecture.md#adr-018) — `std::bad_alloc` rethrown first,
  `std::exception` preserves `what()`, catch-all gets a fixed message.
* Error codes follow the mapping table in
  [docs/HighLevelDesign.md §10](docs/HighLevelDesign.md#10-error-code-mapping);
  don't invent new codes without a design issue.
* Match the surrounding code: one public header per component, tests mirror
  component names, comments explain contracts rather than restating code.

### Commit messages

Concise: an imperative subject line, then a few bullets for the *what/why*
that isn't obvious from the diff. No exhaustive file-by-file narration.

## Adding a serialization backend

Backends are the most self-contained contribution. Follow the
[checklist](docs/serialization-backends.md#writing-your-own-backend) and mirror
the existing structure: a directory under `backends/<fmt>/` with its own
CMake target (`configmanager::<fmt>`), an opt-in `CONFIGMANAGER_BUILD_<FMT>`
option wired in the root `CMakeLists.txt`, the dependency pinned in
`cmake/Dependencies.cmake` (find_package first, FetchContent fallback),
install/export into `ConfigManagerTargets`, and the round-trip test suite.
For YAML and INI, the normative mapping already exists in
[docs/HighLevelDesign.md §6.1](docs/HighLevelDesign.md#61-format-mappings) —
implement that, don't design a new one.

## License

ConfigManager is [MIT-licensed](LICENSE). By contributing, you agree that your
contributions are licensed under the same terms.

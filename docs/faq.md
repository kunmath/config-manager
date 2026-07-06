# FAQ

### Why does `load()` refuse a file without a version?

Unversioned data cannot be handled reliably: guessing a version risks running
the wrong migrations against user data. The library refuses to guess
([ADR-014](Architecture.md#adr-014)). If you are adopting ConfigManager for an
application with existing unversioned files, stamp them once at adoption time
(your code knows what version they are; the library cannot).

### Why won't `synchronize()` downgrade a newer file?

A newer file can contain structures the old code has no migration knowledge
of; downgrading would silently destroy them. `synchronize()` returns
`DowngradeRequired`, touches nothing, and leaves the decision to you
([ADR-008](Architecture.md#adr-008)). Typical responses: refuse to start, or
run read-only — and never save over the newer file.

### Why did `get<int>` fail on a value that's clearly `8080`?

Check the stored type. Conversions are strict and lossless-only
([ADR-015](Architecture.md#adr-015)): `"8080"` (a string) never converts to a
number, `8080.5` is not integral, and an `Int` too large for the requested
narrower type fails. This is deliberate — silent coercion is how configuration
bugs hide.

### Why did my `set()` fail with `InvalidType`?

An existing node on the path has a different type than the write requires —
e.g. `set("network.timeout", 10)` while `network` is currently a string, or
writing a scalar over an object. Upsert creates structure but never changes
type ([ADR-019](Architecture.md#adr-019)); `remove()` the node first if a type
change is intended.

### Should I call `synchronize()` on every startup, even when versions match?

Yes. An `InSync` configuration still passes through repair, so keys a user
deleted from the file get their defaults restored. When nothing changed, the
commit is skipped and the object is untouched — a clean pass is cheap.

### Why do migrations have to go one version at a time?

Adjacent-only migrations stay small, deterministic, and testable
([ADR-008](Architecture.md#adr-008)). A `v1 → v4` shortcut would have to
duplicate the logic of `1→2`, `2→3`, `3→4` and would need updating every time
any of them changes. The engine chains the small steps for you.

### Do version numbers have to be consecutive?

No. Adjacency is catalog order: registering versions 1, 2, 4 makes 2 and 4
adjacent, and the required migration is `2 → 4`. Date-like ids
(`20260101`, …) work fine.

### Where do I put the default for a brand-new setting?

In the target version's default factory — only there. Repair backfills it
after migration; migrations never hand-copy defaults. One place per default
keeps upgrades reproducible.

### Why is there no `save()` to a file path?

Backends operate on streams ([ADR-004](Architecture.md#adr-004)). The
application owns file handling — paths, permissions, atomic-rename strategies,
encryption — and the same backend then works for sockets and memory buffers,
and is trivial to test.

### Can I validate my configuration against a schema?

Not with this library — schema validation is explicitly out of scope
([ADR-006](Architecture.md#adr-006)). The direct
[`MigrationEngine` workflow](versioning-and-migration.md#direct-migration-advanced)
exists partly for this: run your validator before/after migration, with any
schema library you like.

### Is any of this thread-safe?

No. `ConfigModel` and `ConfigRuntime` require external synchronization; see
[Limitations](limitations.md).

### My `ConfigNode` handles stopped working after `synchronize()`. Why?

A committing `synchronize()` replaces the model, which invalidates every
handle obtained from it — undetectably, like container iterators after
reallocation. Re-acquire handles afterwards. Details:
[handle lifetime](model-and-paths.md#handle-lifetime).

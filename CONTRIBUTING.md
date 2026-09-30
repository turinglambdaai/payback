# Contributing

Payback follows the Rivet application architecture: Racket owns domain
logic, native hosts own UI, and the contract between them lives in
`docs/data-format.md`. Keep that split intact.

## Before opening a pull request

```bash
raco test tests/                      # backend tests must stay green
raco rivet build                      # backend + current platform host
node scripts/gen-strings.js --check   # strings parity
```

On macOS, `raco rivet package` should also succeed for UI-touching changes.

## Ground rules

- **User-visible numbers are computed in Racket** (`app/domain.rkt`). Never
  reimplement payback math in Swift or C++ — hosts render, they do not
  calculate.
- **Contract changes update the contract first.** Data model, RPC, milestone
  rules, or validation limits: edit `docs/data-format.md` in the same PR,
  with tests on both sides.
- **UI strings come from `shared/strings/strings.json`.** Run
  `node scripts/gen-strings.js` and commit the regenerated tables; never edit
  `L10n.swift` or `Strings.h` directly.
- **Update behavior is verified, not assumed.** Anything touching
  `app/updater.rkt` or the native install adapters needs a test in
  `tests/updater-test.rkt` and, where relevant, a note in
  `docs/updates.md`.
- **Validation limits and error messages are user-facing.** The native hosts
  show backend messages verbatim, so write them for end users.
- Prefer small, focused pull requests with a clear failure mode and a
  regression test. Avoid new dependencies when Racket or the platform
  libraries already provide the primitive.

## Releasing

Releases are tag-driven; the mechanics (signing, manifest, upload layout)
are in `docs/updates.md`. Bump `version`/`build` in `rivet.rktd` and mirror
them in `app/version.rkt` — both must match in the same release commit.

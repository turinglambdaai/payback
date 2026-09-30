# Online updates

Payback ships with online updates from day one, built on `rivet/distribution`.
The Racket backend fetches, verifies, and downloads signed artifacts; the
native host owns installation. HTTPS protects transport but is never the root
of trust — a signed manifest is.

## Trust chain

1. The backend downloads `update-stable.json` from the update base URL and
   verifies its Ed25519 signature against the public key embedded in
   `app/version.rkt` (expected `key_id` included).
2. `select-update` enforces application identity, channel, SemVer precedence,
   minimum updatable version, and the staged-rollout bucket.
3. The artifact download is size-capped (800 MB), hash-checked (SHA-256 from
   the signed manifest) before being trusted, and streamed with progress.
4. The native host installs only a fully verified artifact.

RVT1 has no role in any of this: the updater deliberately lives outside the
application protocol.

## UX flow

- Launch: if `updateAutoCheck` is on, the host checks at most once per 24 h
  (the backend throttles via `lastUpdateCheckAt`) and stays silent on failure.
  A failed check is not recorded, so the next launch retries.
- 「检查更新」(⌘U) forces a check. An available update offers a download with
  a progress bar; after verification the user installs explicitly.

## Installation per platform

- **macOS** — the SwiftUI host mounts the DMG (`hdiutil attach`), copies the
  new `.app` over the installed one, and relaunches. The previous bundle is
  kept as `Payback.app.old` until the replacement works; a failed copy is
  rolled back in place.
- **Windows** — the WinUI host launches the downloaded MSI with `msiexec /i`.
  The MSI supplies transactional rollback.

## Publishing a release

```bash
export RIVET_UPDATE_BASE_URL=https://downloads.jrtx.site/payback
export RIVET_UPDATE_PRIVATE_KEY="$PWD/keys/update-ed25519-private.der"
export RIVET_UPDATE_KEY_ID=payback-2026-09
raco rivet release            # signed DMG + dist/update-stable.json + SBOM
```

Then upload `dist/payback.dmg` and `dist/update-stable.json` to the base URL
so that `<base>/update-stable.json` and `<base>/<installer-file>` resolve over
HTTPS. Windows releases are produced the same way on a Windows runner
(`Payback-<version>.msi`). If both platforms are published, merge their
`artifacts` arrays into one manifest and sign the merged file with
`rivet/distribution` (`write-signed-manifest`) so a single channel manifest
carries every platform.

The manifest is signed **outside** the Apple/Microsoft signing identities;
its key is separate on purpose. `RIVET_UPDATE_PRIVATE_KEY` never enters the
repository — `keys/` is gitignored except the public half, and CI receives
the private key as a secret.

## Keys

Generate a fresh pair with `scripts/gen-update-keys.sh` (Racket crypto
generates them because macOS LibreSSL lacks Ed25519; the round-trip through
rivet's own readers is verified at generation time).

Key rotation follows rivet's rule: `key_id` identifies the signing key.
Ship a build that trusts the *next* public key before signing releases
exclusively with it — that is, embed the new public key (old key kept as
fallback if the rotation is staged), release, then flip `RIVET_UPDATE_KEY_ID`.

## Local end-to-end test without publishing

The full manifest path (sign → fetch → verify → select → download) is unit
tested offline in `tests/updater-test.rkt` with a throwaway keypair and
tamper checks. To exercise the real HTTP path, serve `dist/` with any static
file server, point `updateBaseUrl` (settings file or the `updateBaseUrl`
settings key) at `https://localhost/...`, and run 「检查更新」.

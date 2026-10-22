# Data format

Payback speaks one RPC vocabulary over a storage layer built for multi-device
sync. Both the Racket backend and every native host agree on the shapes
below; this document is the contract. The on-disk documents and the RPC
payloads use the same encoding, so a support engineer can read the data
directory and know exactly what the app shows.

## Storage (format v2 — per-machine shards)

Every machine writes exactly one **shard** file — `devices-<machineId>.json`,
its claim set — into the **active directory**: the user-chosen sync folder
when sync is on, the platform default directory when it is off. Reading is
merging: union device records by id, decide each id and each whitelisted
settings key by last-writer-wins over `(updatedAt, editedBy)`, hide winners
carrying a tombstone (`deletedAt`). The full design, invariants and the
"why" live in [`sync-design.md`](sync-design.md).

| Platform | Default directory (active when sync is off) |
|---|---|
| macOS | `~/Library/Application Support/Payback/` |
| Windows | `%APPDATA%\Payback\` |
| Linux (experimental) | `$XDG_DATA_HOME/payback/` |

Directory contents:

| File | Writer | Content |
|---|---|---|
| `local.json` | this machine only | machine identity (`machine.id`/`machine.name`), `syncDir` choice, per-machine settings (update checks, digest stamp, milestone acks). Never synced, never leaves the default directory. |
| `devices-<machineId>.json` | one per machine | that machine's claim set: devices it created or last edited, plus its synced settings claims. Read-only for everyone else. |
| `payback.json` | nobody (legacy) | v1 single document, imported once on first v2 launch, then kept untouched as a backup. |

Shards carry `version: 2`; device records add `editedBy` and `deletedAt`
to the v1 fields. Synced settings claims are
`{"value": …, "updatedAt": …, "editedBy": …}` under the whitelist keys
`currency` and `licenseKey` only. Unparseable or shape-broken `devices-*.json`
files (half-uploads, cloud conflict copies) are skipped and reported through
the `sync-state` RPC — never renamed, never deleted.

Writes go through atomic file replacement under a semaphore; a corrupt
*local* document is moved aside as `*.corrupt-<unix-seconds>` and the app
starts fresh rather than crashing (the Taskly 0.6.1 lesson). Downloaded
update artifacts land under the **default** directory's `updates/` — never
inside the sync folder.

## Free tier and license

The free tier holds up to **10 devices** (`add-device` rejects the 11th
with a user-facing message; existing libraries are never touched). A
Payback Pro license lifts the cap.

A license is an offline `PB1.<b64url(canonical claims JSON)>.<b64url(sig)>`
token, Ed25519-signed by `keys/license-ed25519-private.der` (separate
keypair from update signing — one lock, one job). Claims: `product`,
`subject`, `type: "pro"`, `key_id`, optional `expiry` `YYYY-MM-DD`
(expiry day inclusive). Verification happens entirely in the backend; no
server, no call-home. This is an honesty scheme, not DRM. Vendoring:
`scripts/issue-license.rkt`; rotation: ship a build trusting the next
`license-public-key-b64` before issuing exclusively with it. The token
lives in settings as `licenseKey`; `license-state` re-verifies it on every
call, so an expired license downgrades to the free tier without user
action.

## Document

The RPC-facing document (`load-all`) is the **merged view**, shaped exactly
like the v1 document — hosts never needed to learn the shard layout:

```json
{
  "version": 1,
  "devices": [ ...live device records, merged across shards... ],
  "settings": { ...merged settings... }
}
```

`version` refers to the view schema; the backend merges missing keys from
defaults so older payloads keep working.

### Settings

```json
{
  "currency": "CNY",
  "licenseKey": null,
  "updateAutoCheck": true,
  "updateBaseUrl": null,
  "lastUpdateCheckAt": null,
  "rolloutBucket": null
}
```

`currency` and `licenseKey` are **synced** (LWW settings claims); the rest
are per-machine local state:

- `currency` — ISO 4217 code, the default for new devices. Synced: one
  machine's choice becomes every machine's default.
- `licenseKey` — the stored PB1 token. Synced: activate once, unlocked
  everywhere.
- `updateBaseUrl` — `null` uses the built-in feed URL; may be overridden
  with an `https://` URL for testing.
- `lastUpdateCheckAt` — epoch seconds of the last **successful** update check;
  a failed check stays unrecorded so the next launch retries. Drives the 4 h
  auto-check throttle (family baseline). Local per machine.
- `rolloutBucket` — sticky random 0–99 assigned on first check, used for
  staged-rollout comparison (bucket `<` rollout wins). Local per machine.
- `lastDigestAt` — `yyyy-MM-dd` of the last daily digest; the
  `daily-digest` RPC is throttled to once per calendar day. Local per
  machine — every device gets its own first-hand 快报.
- `seenMilestones` — map of device id → milestone keys already celebrated;
  `load-all` flags un-seen achieved milestones with `"new": true` and
  acks them in the same pass (ack-on-read). Local per machine — milestone
  celebrations are each device's own first-hand experience.

## Money

All stored amounts are **integer minor units** (fen/cents) in an `Int64`.
Computed rates (`costPerDayMinor`, `earnedMinor`, …) are floating-point
minor units. Display formatting (symbols, separators) is the native host's
job. Mixing currencies is allowed per device but summary totals add minor
units naively — the UI steers to a single currency per library.

## Device record

Stored fields (native hosts must send every editable field on update):

| Key | Type | Rules |
|---|---|---|
| `id` | string | `d-` + 10 hex, assigned by backend, immutable |
| `name` | string | 1–100 chars after trim |
| `icon` | string | emoji, 1–16 chars, default `📦` |
| `category` | string | one of `computer phone tablet watch audio camera gaming appliance accessory other` |
| `priceMinor` | integer | 1 … 1,000,000,000 (≤ 10,000,000.00) |
| `currency` | string | `^[A-Z]{3}$` |
| `purchaseDate` | string | `yyyy-MM-dd`, 1990-01-01 … today |
| `willingPerDayMinor` | integer \| null | 心理价位：每天愿意付的钱，1 … 100,000,000 |
| `notes` | string | ≤ 2000 chars |
| `createdAt` / `updatedAt` | string | `yyyy-MM-dd HH:mm:ss` local, backend-owned |
| `editedBy` | string | machineId of the last writer (v2 LWW key, hosts may ignore) |
| `deletedAt` | string \| null | tombstone timestamp; a record with a non-null `deletedAt` never appears in the merged view |

The merged view only ever contains live records; `editedBy`/`deletedAt`
travel with the winner and matter to the sync engine
([`sync-design.md`](sync-design.md)). Hosts render neither.

### Computed block

Added by the backend on every read; hosts never store it:

```json
{
  "daysHeld": 487,
  "costPerDayMinor": 2669.4,
  "willingSet": false,
  "earnedMinor": null,
  "paybackProgress": null,
  "paybackEta": null,
  "paidBack": false,
  "milestones": [ {"key": "days-100", "achieved": true}, ... ]
}
```

- `daysHeld` = `max(1, today − purchaseDate + 1)` — the purchase day counts
  as day one.
- `costPerDayMinor` = `priceMinor / daysHeld`.
- With a willingness-to-pay set: `earnedMinor = daysHeld × willing − price`,
  `paybackProgress = daysHeld × willing / price`, `paidBack = earned ≥ 0`,
  and `paybackEta` = `purchaseDate + ⌈price / willing⌉` days (`null` once
  paid back). All `null` when `willingPerDayMinor` is not set.

### Milestones

Fixed ladder, always reported in this order with an `achieved` flag. Keys are
stable identifiers; hosts localize the labels.

| Key | Achieved when |
|---|---|
| `days-100` / `days-365` / `days-1000` | `daysHeld ≥ 100 / 365 / 1000` |
| `cpd-10` / `cpd-5` / `cpd-2` / `cpd-1` / `cpd-05` | `costPerDayMinor < 1000 / 500 / 200 / 100 / 50` |
| `paid-back` | willingness set and `paidBack` |

## Summary

```json
{
  "deviceCount": 5,
  "totalSpentMinor": 4299600,
  "totalDaysHeld": 2341,
  "avgCostPerDayMinor": 1836.7,
  "earnedTotalMinor": 152300.0,
  "paidBackCount": 2,
  "bestDeviceId": "d-…",
  "toughestDeviceId": "d-…"
}
```

`avgCostPerDayMinor` = total spent / total days held across all devices.
`earnedTotalMinor` sums the positive `earnedMinor` values — 「已赚回」.
`bestDeviceId` / `toughestDeviceId` have the lowest / highest daily cost
(`null` when there are no devices).

## RPC surface (RVT1 protocol 1)

Bytes arguments/results are UTF-8 JSON. Errors arrive as RVT1 error frames
with the validation message; the backend never crashes on bad input.

| RPC | Signature | Result JSON |
|---|---|---|
| `load-all` | `() → Bytes` | `{app, settings, devices[], summary}`; milestone objects carry a transient `"new"` flag |
| `daily-digest` | `() → Bytes` | `{status: ok \| already, earnedTotalMinor, deviceCount, bestDeviceName, bestDeviceCostPerDayMinor}` |
| `add-device` | `(Bytes) → Bytes` | stored device incl. `computed` |
| `update-device` | `(Bytes) → Bytes` | full replace of editable fields; `id` required |
| `delete-device` | `(String) → Void` | unknown id is an error |
| `save-settings` | `(Bytes) → Bytes` | patch of `currency` (synced claim) / `updateAutoCheck` / `updateBaseUrl` (local); returns merged settings |
| `activate-license` | `(Bytes {key}) → Bytes` | `LicenseState`; invalid/expired tokens are errors; the key is stored as a synced settings claim |
| `license-state` | `() → Bytes` | `{licensed, type, subject, expiry, deviceLimit, reason}` (`deviceLimit` 10 on free, null on Pro) |
| `check-updates` | `(Bool force) → Bytes` | `{status: available \| up-to-date \| throttled \| error, …}` |
| `start-download` | `() → Void` | errors when nothing is available |
| `update-state` | `() → Bytes` | `{phase: idle \| checking \| downloading \| downloaded \| error, percent, message, downloadedPath, availableVersion}` |
| `sync-state` | `() → Bytes` | `{enabled, dir, defaultDir, machine{id,name}, machines[{id,name}], lastMergeAt, revision, ignoredFiles[]}`; also the poll entry point — stats the shard files and re-merges if the directory changed |
| `configure-sync` | `(Bytes {dir}) → Bytes` | `dir` = absolute folder path (created if missing) or null to turn sync off; returns the new `sync-state` payload |
| `rename-machine` | `(Bytes {name}) → Bytes` | 1–100 chars, trimmed; returns the new `sync-state` payload |

Hosts poll `sync-state` on a timer (5 s) and call `load-all` whenever
`revision` moves — that is how an edit made on another machine shows up
without any push channel.

`check-updates` with `force=false` is the throttled auto-check. Downloads run
on a background thread; hosts poll `update-state` for progress (RVT1 events
are thread-local, so progress is polled rather than pushed).

## Strings

User-visible strings live in `shared/strings/strings.json` (zh + en) and are
generated into the native tables by `scripts/gen-strings.js` — CI runs it
with `--check`. Do not edit `L10n.swift` / `Strings.h` by hand.

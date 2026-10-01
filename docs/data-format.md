# Data format

Payback stores one JSON document and speaks one RPC vocabulary. Both the
Racket backend and every native host agree on the shapes below; this document
is the contract. The on-disk document and the RPC payloads use the same
encoding, so a support engineer can read `payback.json` and know exactly what
the app shows.

## Storage

| Platform | Path |
|---|---|
| macOS | `~/Library/Application Support/Payback/payback.json` |
| Windows | `%APPDATA%\Payback\payback.json` |
| Linux (experimental) | `$XDG_DATA_HOME/payback/payback.json` |

Writes go through atomic file replacement under a semaphore; a corrupt file
is moved aside as `payback.json.corrupt-<unix-seconds>` and the app starts
fresh rather than crashing (the Taskly 0.6.1 lesson). "Corrupt" covers more
than invalid JSON: a file whose JSON parses but whose shape is wrong (e.g.
`devices` is not a list, or a device lacks `id`/`name`/`priceMinor`/
`purchaseDate`) takes the same path. Reads are also cheap by design —
`load-all` only rewrites the file when it newly observes a milestone
achievement; otherwise the store stays untouched. Downloaded update
artifacts land under `<data-dir>/updates/`.

## Document

```json
{
  "version": 1,
  "devices": [ ...device records... ],
  "settings": { ...settings... }
}
```

`version` bumps only with a breaking change; the backend merges missing keys
from defaults so older files keep working.

### Settings

```json
{
  "currency": "CNY",
  "updateAutoCheck": true,
  "updateBaseUrl": null,
  "lastUpdateCheckAt": null,
  "rolloutBucket": null
}
```

- `currency` — ISO 4217 code, the default for new devices.
- `updateBaseUrl` — `null` uses the built-in channel URL; may be overridden
  with an `https://` URL for testing.
- `lastUpdateCheckAt` — epoch seconds of the last **successful** update check;
  a failed check stays unrecorded so the next launch retries. Drives the 24 h
  auto-check throttle.
- `rolloutBucket` — sticky random 0–99 assigned on first check, used for
  staged-rollout comparison (bucket `<` rollout wins).
- `lastDigestAt` — `yyyy-MM-dd` of the last daily digest; the
  `daily-digest` RPC is throttled to once per calendar day.
- `seenMilestones` — map of device id → milestone keys already celebrated;
  `load-all` flags un-seen achieved milestones with `"new": true` and
  acks them in the same pass (ack-on-read).

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
| `category` | string | one of `computer phone tablet audio camera gaming appliance accessory other` |
| `priceMinor` | integer | 1 … 1,000,000,000 (≤ 10,000,000.00) |
| `currency` | string | `^[A-Z]{3}$` |
| `purchaseDate` | string | `yyyy-MM-dd`, 1990-01-01 … today |
| `willingPerDayMinor` | integer \| null | 心理价位：每天愿意付的钱，1 … 100,000,000 |
| `notes` | string | ≤ 2000 chars |
| `createdAt` / `updatedAt` | string | `yyyy-MM-dd HH:mm:ss` local, backend-owned |

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
| `save-settings` | `(Bytes) → Bytes` | patch of `currency` / `updateAutoCheck` / `updateBaseUrl` |
| `check-updates` | `(Bool force) → Bytes` | `{status: available \| up-to-date \| throttled \| error, …}` |
| `start-download` | `() → Void` | errors when nothing is available |
| `update-state` | `() → Bytes` | `{phase: idle \| checking \| downloading \| downloaded \| error, percent, message, downloadedPath, availableVersion}` |

`check-updates` with `force=false` is the throttled auto-check. Downloads run
on a background thread; hosts poll `update-state` for progress (RVT1 events
are thread-local, so progress is polled rather than pushed).

## Strings

User-visible strings live in `shared/strings/strings.json` (zh + en) and are
generated into the native tables by `scripts/gen-strings.js` — CI runs it
with `--check`. Do not edit `L10n.swift` / `Strings.h` by hand.

# Save/draft lifecycle contract

The device form owns one explicit save lifecycle on every host: the form
stays open with identical fields until the backend confirms persistence,
surfaces its own failure with a retry, and closes exactly once on success.
This document is the interaction contract for that lifecycle; PR #28
implemented it and this table was re-verified line by line against the
implementation at main `0d2e9ee`. The storage-side rule lives in
[`data-format.md`](data-format.md) (validation bounds) and
[`sync-design.md`](sync-design.md) (shard semantics); this page owns what
the user sees and can do while a save is in flight.

## Scenario: add a device under a failing backend, then recover

| Column | Contract (verified against implementation) |
|---|---|
| initial state | The Add Device sheet is open over the main window with an empty new draft: name and price empty, date prefilled with today, willing disabled. No field takes automatic keyboard focus on presentation (SwiftUI default); every field is reachable in the Tab loop. |
| user action | Type into fields (Tab walks name → price → date → notes), press **Save** (or Return). With a backend/ storage failure injected, press **Retry** after correcting the field. |
| visible result | While saving: Save and Cancel are disabled and the sheet cannot be dismissed. On backend rejection: the sheet stays open, every field keeps exactly what was typed, a form-level **Save failed** banner renders the backend message (never the window-level alert), and the Save button becomes **Retry**. On success: the sheet closes exactly once, the device list shows the new record, and the detail pane selects the freshly added device (`pendingSelection`). |
| persisted effect | The backend commits the shard to disk **first** and adopts the in-memory claim only after the write succeeds (the unknown-outcome rule). A failed save leaves zero in-memory residue, so the retry records exactly one device. Memory equals disk after every failed attempt. |
| failure/recovery | Two injected classes: backend rejection (price above the `priceMinor` cap → validation error shown at the form) and storage failure (unwritable data directory → shard write error shown at the form). Esc and drag-to-close cannot dismiss the sheet while a save is in flight. After failure, Esc cancels again (close without retry) and fields are preserved until the form is dismissed or the save lands. |
| keyboard | Tab walks the text fields (real event path, verified by typed characters landing). Return triggers Save (`defaultAction`). Esc maps to Cancel (`cancelAction`): dismisses in idle and failed states, blocked while saving. Save label swaps Save → Retry → Save across the lifecycle. |
| native presentation | macOS: SwiftUI `.sheet` with `interactiveDismissDisabled(saving)`, Save/Retry as the form's default-action button, failure rendered inline above the button row. Windows/Linux parity is declared in PR #28 and owned by their hosts; this table's macOS leg is acceptance-verified, the other two legs compile and run in CI. |
| acceptance evidence | AX snapshots at each lifecycle state, state screenshots, an interaction trace with per-step assertions, a relaunch of the packaged app against the same data root proving persistence, and a shard-file check proving exactly one record and zero residue from the failed attempt. Produced by `scripts/e2e/e2e-macos.sh` (suite `macos-native-ui`). |

## Implementation anchors

| Contract fact | Where |
|---|---|
| `idle → saving → failed(String)` state machine | `macos-host/Sources/RivetHost/DeviceForm.swift` (`SavePhase`) |
| Save disabled while `!isValid || saving`; label Save/Retry | `DeviceForm.swift` (button row) |
| Return = Save, Esc = Cancel, Cancel disabled while saving | `DeviceForm.swift` (`keyboardShortcut(.defaultAction/.cancelAction)`) |
| Sheet non-dismissible while saving | `DeviceForm.swift` (`interactiveDismissDisabled(saving)`) |
| Failure banner at the form, not the window alert | `DeviceForm.swift` (failed-phase block), `RivetHostApp.swift` (`addDevice` rethrows, never alerts) |
| One save in flight (double-tap guard) | `DeviceForm.swift` (`save()` guard) |
| Close exactly once after confirmed persistence | `DeviceForm.swift` (`dismiss()` on the success path only) |
| New device selected after the reload lands | `RivetHostApp.swift` (`pendingSelection`), `ContentView.swift` (`.onChange`) |
| Validation bounds (price ≤ 10,000,000.00, willing ≤ 1,000,000.00/day, name ≤ 100 chars) | `app/wire.rkt`, `docs/data-format.md` |
| Commit-on-disk-first, zero residue, retry = exactly one | `app/sync.rkt` (`sync-mutate!`), `tests/sync-test.rkt` (durability case) |
| User-visible strings (Save/Retry/Cancel/Save failed) | `shared/strings/strings.json` |

## Evidence map (suite `macos-native-ui`, macOS leg)

The suite drives two launches: **run 1 (full)** carries every interactive
contract line in the first app instance (the only instance whose keyboard
routing is reliable), and **run 2 (verify)** relaunches for the
persistence proof with accessibility reads only.

| Contract row | Evidence artifact under `.rivet/acceptance/macos/` |
|---|---|
| initial state | `accessibility-run1-idle.json` (sheet with empty draft), `form-idle.png` |
| user action (typed event path) | trace assertions `typed:name-field`, `tab-walk:price-focused-and-typed` (Tab focus readback + typed characters landing) |
| visible result: saving disables buttons | trace `saving-state-observation` (opportunistic probe — see note) |
| visible result: rejection keeps fields | `accessibility-run1-failed.json`, `form-run1-failed.png`, trace `run1-name-field-preserved` / `run1-price-field-preserved` / `run1-retry-offered` |
| backend rejection message visible | trace `backend-message-visible` |
| correction on the failed form keeps the sheet open (#30) | trace `correction-keeps-form-open`, `correction-name-preserved`, `correction-typed` |
| corrected retry persists and closes exactly once | trace `corrected-save-closes-sheet-once`, `new-device-listed`, `detail-follows-new-device`, `accessibility-run1.json` |
| keyboard: Esc in idle cancels without persisting | trace `esc-idle-cancels`, `esc-idle-persists-nothing` |
| keyboard: Esc after a failure cancels again | trace `esc-after-failure-cancels`, `rejection-cancels-persists-nothing` |
| failure/recovery: storage failure keeps form open | `accessibility-run2-failed.json`, `form-run2-failed.png`, trace `storage-failure-keeps-form-open`, `storage-failure-persists-nothing` |
| Retry (after the injection is removed) persists and closes exactly once | trace `retry-closes-sheet-once`, `storage-new-device-listed`, `storage-detail-follows-new-device`, `accessibility-run2-saved.json`, `form-saved.png` |
| persisted effect: exactly one record per story | shard-file checks in `scripts/e2e/e2e-macos.sh` (rejected save + corrected retry = one `E2E Save Probe`; storage-failed attempt + retry = one `E2E Storage Probe`) |
| persisted effect: relaunch proof | run 2 (verify) assertions `seed-persisted-across-restart`, `correction-recorded-exactly-one`, `retry-recorded-exactly-one`, `accessibility.json` |
| native presentation: sheet + Save→Retry | screenshots + AX snapshots above |

Note on the saving-state observation: with local storage the in-flight
window is milliseconds, so the suite records whether the disabled-Save
moment was observed rather than failing on the race; the deterministic
guarantees are the blocked dismissal (a failure keeps the sheet open) and
the exactly-once close. The blocking itself is enforced by three
independent mechanisms (disabled Save, disabled Cancel, `guard phase !=
.saving`), and the run-trace records the observation honestly either way.

## Platform-forced dismissal on current macOS hosts (#30): re-assert

Current macOS hosts (CI macos-15 runners and a macOS 26 desktop — the
acceptance suite isolated it on CI) can dismiss the sheet while the user
edits a field on a failed form: the keystroke whose result is a valid
value closes the sheet, losing the typed correction — even when no
structural change rides on the transition and even when the price goes
from one valid value straight to another. Earlier product-side
mitigations (per-sheet anchor views, hint stabilization) did not help;
the trigger is platform-layer.

The hosts now defend in kind (`DeviceFormRescue` in
`RivetHostApp.swift`): the form mirrors `(draft, phase)` to the app
model after every change, and when the sheet disappears while the phase
is still `.failed` — no Save/Retry/Esc path ran, those all reset the
phase to idle first — the dismissal was the platform, not the user, and
the form re-presents within a beat with exactly what was typed and the
failure banner intact. Re-assertion is bounded at three attempts so a
host that refuses every re-presentation degrades to the old behavior
instead of looping. The acceptance suite's `correction-keeps-form-open`
row is the regression gate: it performs the exact discriminating
keystroke sequence (select-all + retype to a valid value) that used to
destroy the sheet.

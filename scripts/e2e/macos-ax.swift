import AppKit
import ApplicationServices
import Carbon.HIToolbox
import Foundation

// Save/draft-lifecycle acceptance driver for the packaged macOS app
// (docs/save-lifecycle.md). Three runs against one disposable data root:
//
//   full     Esc cancels an idle form; a typed over-cap price is rejected
//            by the backend with every field preserved; correcting it and
//            pressing Retry persists exactly one device and closes the
//            sheet once with the new record selected.
//   storage  relaunch proves persistence; a read-only data directory turns
//            the next save into a storage failure that keeps the form open
//            with fields intact; Esc cancels again afterwards.
//   final    relaunch proves the failed storage save left zero residue and
//            the successful retry kept exactly one record; writes the
//            canonical accessibility.json / interaction-trace.json.
//
// Two platform disciplines this driver follows:
//
//   1. The keyboard path is real end to end: focus is asserted through
//      kAXFocusedAttribute and then proven by typed CGEvents landing in
//      the field's AXValue (an AXUIElementSetValue shortcut would skip
//      the user path); Return is keycode 36, Escape 53, Tab 48.
//   2. Text matching is explicit about scope: hasText is node-local
//      (title/value/description/help of that one element), so any check
//      that means "somewhere in this subtree" must go through
//      subtreeHasText/firstWith — a node-local check on a container
//      silently never matches its own content. Probes also re-resolve
//      their trees from a freshly created application element each time,
//      so no wait loop leans on element identity across state changes.

enum E: Error { case failure(String) }

// ---------- element helpers ----------

func attr(_ e: AXUIElement, _ n: String) -> CFTypeRef? {
    var v: CFTypeRef?
    return AXUIElementCopyAttributeValue(e, n as CFString, &v) == .success ? v : nil
}
func str(_ e: AXUIElement, _ n: String) -> String { attr(e, n) as? String ?? "" }
func bool(_ e: AXUIElement, _ n: String) -> Bool { attr(e, n) as? Bool ?? false }
func role(_ e: AXUIElement) -> String { str(e, kAXRoleAttribute) }
func children(_ e: AXUIElement) -> [AXUIElement] {
    attr(e, kAXChildrenAttribute) as? [AXUIElement] ?? []
}
func walk(_ e: AXUIElement, _ depth: Int = 0) -> [AXUIElement] {
    depth >= 14 ? [e] : [e] + children(e).flatMap { walk($0, depth + 1) }
}

/// the visible text of an element, across the attributes SwiftUI populates
func labels(_ e: AXUIElement) -> [String] {
    [str(e, kAXTitleAttribute), str(e, kAXValueAttribute),
     str(e, kAXDescriptionAttribute), str(e, kAXHelpAttribute)]
}
func hasText(_ e: AXUIElement, _ needle: String) -> Bool {
    labels(e).contains { $0.contains(needle) }
}
func firstWith(_ root: AXUIElement, _ texts: [String], wantedRole: String? = nil) -> AXUIElement? {
    walk(root).first { e in
        (wantedRole == nil || role(e) == wantedRole) && texts.contains { hasText(e, $0) }
    }
}
func formFields(_ root: AXUIElement) -> [AXUIElement] {
    walk(root).filter { ["AXTextField", "AXTextArea"].contains(role($0)) }
}
func listElement(_ root: AXUIElement, anchor: String) -> AXUIElement? {
    walk(root).first {
        ["AXList", "AXTable", "AXOutline"].contains(role($0))
            && subtreeHasText($0, anchor)   // the anchor lives in descendants
    }
}
func rowCount(_ list: AXUIElement, name: String) -> Int {
    walk(list).filter { $0 !== list && role($0) == "AXStaticText"
        && str($0, kAXValueAttribute) == name }.count
}

func wait<T>(_ probe: @escaping () -> T?, timeout: TimeInterval = 30) throws -> T {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if let v = probe() { return v }
        Thread.sleep(forTimeInterval: 0.1)
    }
    throw E.failure("timeout")
}
func waitGone(_ probe: @escaping () -> AXUIElement?) throws {
    let deadline = Date().addingTimeInterval(30)
    while Date() < deadline {
        if probe() == nil { return }
        Thread.sleep(forTimeInterval: 0.1)
    }
    throw E.failure("timeout waiting for the element to disappear")
}

func snap(_ e: AXUIElement, _ depth: Int = 0) -> [String: Any] {
    var r: [String: Any] = ["role": role(e),
                            "title": str(e, kAXTitleAttribute),
                            "value": str(e, kAXValueAttribute),
                            "label": str(e, kAXDescriptionAttribute),
                            "help": str(e, kAXHelpAttribute)]
    if depth < 10 { r["children"] = children(e).map { snap($0, depth + 1) } }
    return r
}
func writeJSON(_ v: Any, _ out: URL, _ name: String) throws {
    try JSONSerialization.data(withJSONObject: v, options: [.prettyPrinted, .sortedKeys])
        .write(to: out.appendingPathComponent(name))
}
func shoot(_ out: URL, _ name: String) -> Bool {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
    p.arguments = ["-x", out.appendingPathComponent(name).path]
    do { try p.run() } catch { return false }
    p.waitUntilExit()
    return p.terminationStatus == 0
}

// ---------- keyboard: the real user path ----------

func focus(_ e: AXUIElement, _ what: String) throws {
    let r = AXUIElementSetAttributeValue(e, kAXFocusedAttribute as CFString, kCFBooleanTrue)
    guard r == .success else { throw E.failure("AX focus failed for \(what): \(r.rawValue)") }
    guard bool(e, kAXFocusedAttribute) else { throw E.failure("\(what) did not take focus") }
}

func postKey(_ keyCode: CGKeyCode, flags: CGEventFlags = []) {
    for down in [true, false] {
        guard let ev = CGEvent(keyboardEventSource: nil, virtualKey: keyCode, keyDown: down)
        else { fatalError("could not create key event \(keyCode)") }
        if !flags.isEmpty { ev.flags = flags }
        ev.post(tap: .cghidEventTap)
        usleep(20_000)
    }
}
func typeText(_ text: String) {
    for ch in text.unicodeScalars {
        guard let down = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: true),
              let up = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: false)
        else { fatalError("could not create typing events") }
        let utf16 = Array(String(ch).utf16)
        down.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
        up.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
        down.post(tap: .cghidEventTap)
        up.post(tap: .cghidEventTap)
        usleep(20_000)
    }
}

func press(_ e: AXUIElement, _ what: String) throws {
    let r = AXUIElementPerformAction(e, kAXPressAction as CFString)
    guard r == .success else { throw E.failure("AXPress failed for \(what): \(r.rawValue)") }
}

// ---------- lifecycle vocabulary (both shipped languages) ----------

let saveLabel = ["Save", "保存"]
let retryLabel = ["Retry", "重试"]
let saveFailedLabel = ["Save failed", "保存失败"]
// the toolbar + button's accessibility label comes from the SF Symbol
// ("Add" / "添加"), not from the .help() tooltip — match that; the other
// toolbar buttons ("Hide Sidebar", "Date added", "Edit", "Trash") don't
// collide with these needles case-sensitively
let addDeviceLabel = ["Add", "添加"]

let seededDevice = "Codex Acceptance Device"
let probeName = "E2E Save Probe"
let storageName = "E2E Storage Probe"

// ---------- context ----------

let args = CommandLine.arguments
func argValue(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}
guard let pid = argValue("--pid").flatMap({ Int32($0) }),
      let outDir = argValue("--output").map({ URL(fileURLWithPath: $0, isDirectory: true) }),
      let phase = argValue("--phase") else {
    FileHandle.standardError.write(Data(
        "usage: macos-ax --pid PID --output DIR --phase full|storage|final [--data-dir DIR]\n".utf8))
    exit(2)
}
let dataDir = argValue("--data-dir")
var actions: [String] = []
var assertions: [String: Any] = [:]
var observations: [String: Any] = [:]
func act(_ s: String) { actions.append(s) }
func note(_ key: String, _ v: Any) { observations[key] = v }
func assertHolds(_ key: String, _ cond: Bool, _ why: String) throws {
    assertions[key] = cond
    guard cond else { throw E.failure("assertion failed: \(key) — \(why)") }
}

func writeTrace() {
    let trace: [String: Any] = ["platform": "macos", "phase": phase,
                                "actions": actions, "assertions": assertions,
                                "observations": observations]
    let name = phase == "final" ? "interaction-trace.json" : "trace-\(phase).json"
    try? writeJSON(trace, outDir, name)
}

/// every probe resolves its tree from a fresh application element, so no
/// wait loop can pin the client-side AX cache to a stale snapshot
func appEl() -> AXUIElement { AXUIElementCreateApplication(pid) }
/// both window sources, deduplicated: kAXWindowsAttribute and the app
/// element's children each intermittently report nothing on macOS 26
/// while the other source carries the live window
func windows() -> [AXUIElement] {
    var ws = (attr(appEl(), kAXWindowsAttribute) as? [AXUIElement]) ?? []
    for w in children(appEl()) where role(w) == "AXWindow"
        && !ws.contains(where: { CFEqual($0, w) }) {
        ws.append(w)
    }
    return ws
}
func mainWindow() -> AXUIElement? { windows().first }
/// subtree text search — hasText itself is node-local, so callers that
/// need descendants must go through this
func subtreeHasText(_ root: AXUIElement?, _ needle: String) -> Bool {
    guard let root else { return false }
    return walk(root).contains { hasText($0, needle) }
}
// the Add Device sheet: prefer an AXSheet node, but its content is not
// guaranteed to sit under it (an empty AXSheet shell with the form fields
// elsewhere in the window subtree has been observed) — so fall back to
// identifying the form container by shape: 3+ text fields and the primary
// button, which is labeled Save in idle/saving and Retry after a failure
let formButtonLabel = saveLabel + retryLabel
func sheet() -> AXUIElement? {
    let tree = walk(appEl())
    for s in tree where role(s) == "AXSheet" {
        if formFields(s).count >= 3
            && firstWith(s, formButtonLabel, wantedRole: "AXButton") != nil {
            return s
        }
    }
    return tree.first { s in
        role(s) != "AXWindow" && formFields(s).count >= 3
            && firstWith(s, formButtonLabel, wantedRole: "AXButton") != nil
    }
}
func deviceList() -> AXUIElement? {
    mainWindow().flatMap { listElement($0, anchor: seededDevice) }
}
func openForm() throws -> AXUIElement {
    guard let win = mainWindow(),
          let plus = firstWith(win, addDeviceLabel, wantedRole: "AXButton") else {
        throw E.failure("Add Device toolbar button not found")
    }
    try press(plus, "Add Device")
    return try wait { sheet() }
}

/// fire Save via Return and opportunistically observe the in-flight state
/// (Save disabled) without fixed sleeps; the saving window is milliseconds
/// on local storage, so this is recorded as an observation, never an
/// assertion; each read re-resolves the button from a fresh tree
func fireSave() throws {
    try keyboardGuard("Return-to-Save")
    postKey(36)   // Return = defaultAction = Save
    act("press:return-saves")
    var sawDisabled = false
    var sawStale = false
    let deadline = Date().addingTimeInterval(1.2)
    while Date() < deadline {
        guard let form = sheet(),
              let button = firstWith(form, saveLabel, wantedRole: "AXButton") else {
            sawStale = true   // the label swap rebuilt the button
            break
        }
        if !bool(button, kAXEnabledAttribute) { sawDisabled = true; break }
        usleep(10_000)
    }
    note("saving-state-observation",
         ["saveButtonDisabledSeen": sawDisabled,
          "buttonReferenceWentStale": sawStale])
}

func assertFailureRendered(keepName: String, keepPrice: String, tag: String) throws {
    _ = try wait { sheet().flatMap { firstWith($0, saveFailedLabel) } }
    _ = try wait { sheet().flatMap { firstWith($0, retryLabel, wantedRole: "AXButton") } }
    try assertHolds("\(tag)-sheet-still-open", true, "failure must keep the form open")
    func currentFields() -> [AXUIElement]? { sheet().map { formFields($0) } }
    let name = try wait { currentFields()?.first }
    try assertHolds("\(tag)-name-field-preserved",
                    str(name, kAXValueAttribute) == keepName,
                    "the typed name must survive the failed save untouched")
    let price = try wait { currentFields().flatMap { $0.dropFirst().first } }
    try assertHolds("\(tag)-price-field-preserved",
                    str(price, kAXValueAttribute) == keepPrice,
                    "the typed price must survive the failed save untouched")
    try assertHolds("\(tag)-retry-offered",
                    sheet().flatMap { firstWith($0, retryLabel, wantedRole: "AXButton") } != nil,
                    "Save must relabel to Retry after a failure")
    if let form = sheet() {
        try writeJSON(snap(form), outDir, "accessibility-\(tag)-failed.json")
    }
    note("screenshot-\(tag)-failed", shoot(outDir, "form-\(tag)-failed.png"))
    act("failure-rendered-at-form:\(tag)")
}

/// focus a field, type, and require the typed characters to land in the
/// field's AXValue — the anti-fake-focus proof: a stale or misrouted focus
/// readback fails here because the value never changes; the field is
/// re-resolved from a fresh tree on every value probe
func typeAndVerify(_ index: Int, _ text: String, _ what: String) throws {
    guard let form = sheet(), formFields(form).count > index else {
        throw E.failure("\(what) not found before typing")
    }
    try keyboardGuard("typing into \(what)")
    try focus(formFields(form)[index], what)
    typeText(text)
    _ = try wait { () -> String? in
        guard let f = sheet(), formFields(f).count > index else { return nil }
        let v = str(formFields(f)[index], kAXValueAttribute)
        return v.contains(text) ? v : nil
    }
}

/// replace a field's content through the user path: select all, then type
/// over the selection. The intermediate clear is NOT awaited separately —
/// the value is asserted exactly once at the end, so a select-all that
/// silently missed (focus churn after a failed-save rebuild) surfaces as
/// an exact-value failure with the field's actual content in the trace
func replaceText(_ index: Int, _ text: String, _ what: String) throws {
    guard let form = sheet(), formFields(form).count > index else {
        throw E.failure("\(what) not found before replacing")
    }
    try keyboardGuard("replacing \(what)")
    try focus(formFields(form)[index], what)
    postKey(0, flags: .maskCommand)   // cmd+A selects the field's text
    postKey(51)                       // delete clears the selection
    typeText(text)                    // types over the (now empty) field
    _ = try wait { () -> String? in
        guard let f = sheet(), formFields(f).count > index else { return nil }
        let v = str(formFields(f)[index], kAXValueAttribute)
        return v == text ? v : nil
    }
}

/// Tab walks from the name field to the price field; the focus claim is
/// read back from a fresh tree
func focusSecondField() throws {
    try keyboardGuard("Tab walk to the price field")
    postKey(48)
    _ = try wait { () -> Bool? in
        guard let f = sheet(), formFields(f).count >= 2 else { return nil }
        return bool(formFields(f)[1], kAXFocusedAttribute) ? true : nil
    }
}

/// quit through the user path (Cmd+Q → quit Apple Event): the run loop
/// drives the orderly backend shutdown. NSRunningApplication.terminate()
/// and raw SIGTERM are unreliable for the packaged app on newer macOS
/// hosts (the signal is swallowed and the process idles forever), while
/// the quit Apple Event works everywhere; the suite owns launching and
/// this driver owns the real-user quit.
func quitOrderly() {
    _ = AXUIElementSetAttributeValue(appEl(), kAXFrontmostAttribute as CFString,
                                     kCFBooleanTrue)
    // SAFETY: if the app could not take frontmost (shared desktop, sick
    // instance), a posted Cmd+Q would land in the user's focused app —
    // skip the keys and let the suite's bounded TERM/KILL reap us instead
    guard (attr(appEl(), kAXFrontmostAttribute) as? Bool) == true else { return }
    for down in [true, false] {
        guard let ev = CGEvent(keyboardEventSource: nil, virtualKey: 12, keyDown: down)
        else { return }
        ev.flags = .maskCommand
        ev.post(tap: .cghidEventTap)
        usleep(50_000)
    }
}

/// bring the app frontmost and PROVE it: posted CGEvents route to the
/// frontmost application, so every keyboard step below depends on this.
/// This is also a SAFETY gate: on a shared desktop, keystrokes aimed at a
/// non-frontmost app would land in whatever the user is typing into.
func ensureKeyboardTarget() throws {
    let app = appEl()
    _ = AXUIElementSetAttributeValue(app, kAXFrontmostAttribute as CFString,
                                     kCFBooleanTrue)
    if (attr(app, kAXFrontmostAttribute) as? Bool) != true {
        NSRunningApplication(processIdentifier: pid)?.activate()
    }
    _ = try wait({ () -> Bool? in
        if (attr(appEl(), kAXFrontmostAttribute) as? Bool) == true { return true }
        NSRunningApplication(processIdentifier: pid)?.activate()
        return nil
    }, timeout: 10)
}

/// re-verify immediately before emitting keys; fail fast instead of
/// letting keystrokes loose into another application
func keyboardGuard(_ what: String) throws {
    guard (attr(appEl(), kAXFrontmostAttribute) as? Bool) == true else {
        throw E.failure("Payback lost frontmost before \(what); keyboard phases need an exclusive session (shared desktops reroute posted keys to the user's focused app)")
    }
}

// ---------- input source: third-party IMEs swallow posted keys ----------

// Third-party input methods (e.g. WeChat IME) intercept key events on the
// text-input path, so typed characters and even Escape never reach the
// app. Temporarily selecting an ASCII-capable keyboard layout for the
// keyboard phases and restoring it afterwards is the established
// workaround; the saved source is restored on every exit path.
var savedInputSource: TISInputSource?

func asciiCapable(_ src: TISInputSource) -> Bool {
    guard let prop = TISGetInputSourceProperty(src, kTISPropertyInputSourceIsASCIICapable)
    else { return false }
    let value = Unmanaged<CFTypeRef>.fromOpaque(prop).takeUnretainedValue()
    return (value as? Bool) == true   // CFBoolean bridges to Bool
}

func inputSourceID(_ src: TISInputSource) -> String {
    guard let prop = TISGetInputSourceProperty(src, kTISPropertyInputSourceID)
    else { return "" }
    return Unmanaged<CFString>.fromOpaque(prop).takeUnretainedValue() as String
}

func selectASCIILayout() {
    savedInputSource = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue()
    guard let list = TISCreateInputSourceList(nil, false)?.takeRetainedValue()
        as? [TISInputSource] else { return }
    var pick: TISInputSource?
    for src in list where asciiCapable(src) {
        if inputSourceID(src) == "com.apple.keylayout.ABC" { pick = src; break }
        if pick == nil { pick = src }
    }
    if let pick { TISSelectInputSource(pick) }
}

func restoreInputSource() {
    if let savedInputSource { TISSelectInputSource(savedInputSource) }
    savedInputSource = nil
}

do {
    try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)
    guard AXIsProcessTrusted() else {
        throw E.failure("accessibility unavailable; the test host must hold AX trust")
    }
    _ = try wait({ () -> Bool? in
        subtreeHasText(mainWindow(), seededDevice) ? true : nil
    }, timeout: 60)
    act("launch:seeded-device-visible")
    try ensureKeyboardTarget()
    selectASCIILayout()

    switch phase {

    // ---------------- run 1: the full rejection/retry cycle ----------------
    case "full":
        let seededRows = try wait { deviceList().map { rowCount($0, name: seededDevice) } }
        try assertHolds("seeded-list-exactly-one", seededRows == 1,
                        "the seed must present exactly one row")

        // Esc on an idle form cancels without persisting anything
        let form = try openForm()
        let idleFields = formFields(form)
        try assertHolds("form-fields-present", idleFields.count >= 3,
                        "name/price/date fields must exist in the sheet")
        try assertHolds("initial-name-empty", str(idleFields[0], kAXValueAttribute) == "",
                        "a new draft starts with an empty name")
        try assertHolds("initial-price-empty", str(idleFields[1], kAXValueAttribute) == "",
                        "a new draft starts with an empty price")
        try writeJSON(snap(form), outDir, "accessibility-run1-idle.json")
        note("screenshot-idle", shoot(outDir, "form-idle.png"))
        try keyboardGuard("Esc on the idle form")
        postKey(53)   // Esc
        try waitGone { sheet() }
        try assertHolds("esc-idle-cancels", true, "Esc must dismiss an idle form")
        try assertHolds("esc-idle-persists-nothing",
                        (deviceList().map { rowCount($0, name: seededDevice) } ?? 0) == 1
                            && (deviceList().map { rowCount($0, name: probeName) } ?? -1) == 0,
                        "canceling the idle form must not create a record")
        act("esc:idle-form-cancelled")

        // reopen and walk the real keyboard path into a backend rejection
        _ = try openForm()
        try typeAndVerify(0, probeName, "name field")
        act("typed:name-field")
        try focusSecondField()
        try typeAndVerify(1, "20000000.00", "price field")
        act("tab-walk:price-focused-and-typed")
        note("tab-walk-price-focus", true)

        // over-cap price: the client accepts the shape, the backend
        // rejects the value (cap: priceMinor <= 1,000,000,000)
        try fireSave()
        try assertFailureRendered(keepName: probeName, keepPrice: "20000000.00",
                                  tag: "run1")
        try assertHolds("backend-message-visible",
                        subtreeHasText(sheet(), "priceMinor"),
                        "the backend validation message must render at the form")
        try assertHolds("rejection-persists-nothing",
                        (deviceList().map { rowCount($0, name: probeName) } ?? -1) == 0,
                        "a rejected save must leave no record")

        // correct the price and press Retry: persist, close once, select
        try replaceText(1, "12999.00", "price field")
        act("corrected:price")
        let correctedPrice = sheet().flatMap { formFields($0).dropFirst().first }
            .map { str($0, kAXValueAttribute) }
        note("corrected-price-value", correctedPrice ?? "fields not AX-visible")
        guard let retryButton = sheet().flatMap({ firstWith($0, retryLabel,
                                                            wantedRole: "AXButton") }) else {
            throw E.failure("the Retry button vanished before the retry")
        }
        try press(retryButton, "Retry")
        act("press:retry")
        try waitGone { sheet() }
        try assertHolds("retry-closes-sheet-once", sheet() == nil,
                        "the confirmed save closes the sheet exactly once")
        let savedRows = try wait { deviceList().map { rowCount($0, name: probeName) } }
        try assertHolds("new-device-listed", savedRows >= 1,
                        "the persisted device must appear in the list")
        try assertHolds("detail-follows-new-device",
                        subtreeHasText(mainWindow(), probeName),
                        "the detail pane must show the freshly added device")
        try assertHolds("seeded-row-intact",
                        (deviceList().map { rowCount($0, name: seededDevice) } ?? 0) == 1,
                        "the seeded device must survive untouched")
        if let win = mainWindow() {
            try writeJSON(snap(win), outDir, "accessibility-run1-saved.json")
            try writeJSON(snap(appEl()), outDir, "accessibility-run1.json")
        }
        note("screenshot-saved", shoot(outDir, "form-saved.png"))
        act("retry:persisted-and-closed")

    // ---------- run 2: persistence proof + storage failure ----------
    case "storage":
        let persistedRows = try wait { deviceList().map { rowCount($0, name: probeName) } }
        try assertHolds("persistence-across-restart", persistedRows >= 1,
                        "the device saved in run 1 must survive a relaunch")
        act("relaunch:persisted-device-visible")

        _ = try openForm()
        try typeAndVerify(0, storageName, "name field")
        try focusSecondField()
        try typeAndVerify(1, "99.00", "price field")

        // inject the storage failure: the data directory refuses new
        // files, so the shard write fails after validation passed
        guard let dataDir else {
            throw E.failure("--data-dir is required for the storage phase")
        }
        do {
            try FileManager.default.setAttributes([.posixPermissions: 0o555],
                                                  ofItemAtPath: dataDir)
        } catch {
            throw E.failure("could not make the data directory read-only: \(error)")
        }
        defer {
            try? FileManager.default.setAttributes([.posixPermissions: 0o755],
                                                   ofItemAtPath: dataDir)
        }
        try fireSave()
        try assertFailureRendered(keepName: storageName, keepPrice: "99.00",
                                  tag: "run2")
        try assertHolds("storage-failure-keeps-form-open", sheet() != nil,
                        "a failed shard write must keep the form open")

        // after the failure Esc cancels again, and nothing was persisted
        try keyboardGuard("Esc after the storage failure")
        postKey(53)
        try waitGone { sheet() }
        try assertHolds("esc-after-failure-cancels", sheet() == nil,
                        "Esc must cancel the failed form")
        try assertHolds("storage-failure-persists-nothing",
                        (deviceList().map { rowCount($0, name: storageName) } ?? -1) == 0,
                        "the failed storage save must leave no record")
        try writeJSON(snap(appEl()), outDir, "accessibility-run2.json")
        act("storage:failed-and-cancelled")

    // ---------------- run 3: residue-free final state ----------------
    case "final":
        let list = try wait { deviceList() }
        try assertHolds("retry-recorded-exactly-one",
                        rowCount(list, name: probeName) == 1,
                        "the retry must present exactly one record, never two")
        try assertHolds("failed-save-left-zero-residue",
                        rowCount(list, name: storageName) == 0,
                        "the storage-failed attempt must not appear after relaunch")
        try assertHolds("seeded-row-still-intact",
                        rowCount(list, name: seededDevice) == 1,
                        "the seeded device must still be exactly one row")
        try writeJSON(snap(appEl()), outDir, "accessibility.json")
        note("screenshot-final", shoot(outDir, "screen.png"))
        act("final:exactly-one-record-zero-residue")

    default:
        throw E.failure("unknown phase \(phase)")
    }

    writeTrace()
    restoreInputSource()
    quitOrderly()
} catch {
    if let dataDir {  // never leave the disposable root read-only
        try? FileManager.default.setAttributes([.posixPermissions: 0o755],
                                               ofItemAtPath: dataDir)
    }
    writeTrace()
    try? writeJSON(snap(appEl()), outDir, "accessibility-failure-\(phase).json")
    _ = shoot(outDir, "failure-\(phase).png")   // visual ground truth at abort
    restoreInputSource()
    quitOrderly()   // best effort; the suite reaps us regardless
    FileHandle.standardError.write(Data("Payback acceptance failed [\(phase)]: \(error)\n".utf8))
    exit(1)
}

import SwiftUI

// Add / edit form. The backend re-validates everything; client-side checks
// only keep obvious typos from a round trip. Invalid fields show a red hint
// inline so the disabled Save is never a puzzle.

struct DeviceForm: View {
    /// The save lifecycle the issue #24 contract requires: the form stays
    /// open with identical fields until persistence is confirmed, surfaces
    /// its own failure with a retry, and closes exactly once on success.
    enum SavePhase: Equatable {
        case idle
        case saving
        case failed(String)
    }

    struct Draft: Equatable {
        var id: String?
        var name: String
        var icon: String
        var category: String
        var priceText: String
        var dateText: String
        var willingEnabled: Bool
        var willingText: String
        var notes: String
        var currency: String

        static func new(currency: String) -> Draft {
            Draft(id: nil, name: "", icon: "📦", category: "other",
                  priceText: "", dateText: Self.text(from: Date()),
                  willingEnabled: false, willingText: "", notes: "",
                  currency: currency)
        }

        static func from(_ device: Device) -> Draft {
            Draft(id: device.id,
                  name: device.name,
                  icon: device.icon,
                  category: device.category,
                  priceText: String(format: "%.2f", Double(device.priceMinor) / 100),
                  dateText: device.purchaseDate,
                  willingEnabled: device.willingPerDayMinor != nil,
                  willingText: device.willingPerDayMinor.map {
                      String(format: "%.2f", Double($0) / 100)
                  } ?? "",
                  notes: device.notes ?? "",
                  currency: device.currency)
        }

        func toDeviceDraft() -> DeviceDraft? {
            let trimmed = name.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty else { return nil }
            let price = Self.parseMinor(priceText)
            guard price > 0 else { return nil }
            guard let purchaseDate = Self.normalizedDate(dateText) else { return nil }
            // enabled with a clean amount = set; enabled but empty = not set;
            // enabled with garbage = refuse the save (the hint says why)
            var willing: Int64?
            if willingEnabled {
                let parsed = Self.parseMinor(willingText)
                if willingText.trimmingCharacters(in: .whitespaces).isEmpty {
                    willing = nil
                } else {
                    guard parsed > 0 else { return nil }
                    willing = parsed
                }
            }

            return DeviceDraft(name: trimmed,
                               icon: icon.isEmpty ? "📦" : String(icon.prefix(4)),
                               category: category,
                               priceMinor: price,
                               currency: currency,
                               purchaseDate: purchaseDate,
                               willingPerDayMinor: willing,
                               notes: notes)
        }

        static func parseMinor(_ text: String) -> Int64 {
            var cleaned = foldFullWidth(text)
                .trimmingCharacters(in: .whitespaces)
            // "5,999" (every comma group exactly 3 digits) reads as a
            // thousands separator; anything else treats the comma as a
            // decimal point ("5,99" = 5.99)
            let groups = cleaned.components(separatedBy: ",")
            if groups.count > 1,
               groups.dropFirst().allSatisfy({ $0.count == 3 && Int($0) != nil }) {
                cleaned = groups.joined()
            } else {
                cleaned = cleaned.replacingOccurrences(of: ",", with: ".")
            }
            guard let value = Double(cleaned), value.isFinite else { return 0 }
            return Int64((value * 100).rounded())
        }

        // Chinese IMEs emit full-width digits and punctuation; fold them so
        // "５９９９" and "5，999" parse like their half-width equivalents.
        static func foldFullWidth(_ text: String) -> String {
            let scalars = text.unicodeScalars.map { scalar -> Unicode.Scalar in
                switch scalar.value {
                case 0xFF10...0xFF19:
                    return Unicode.Scalar(scalar.value - 0xFF10 + 0x30)!
                case 0xFF0C:
                    return ","
                case 0xFF0D:
                    return "-"
                case 0xFF0E:
                    return "."
                case 0xFF0F:
                    return "/"
                default:
                    return scalar
                }
            }
            return String(String.UnicodeScalarView(scalars))
        }

        // The backend contract is yyyy-MM-dd within 1990-01-01…today
        // (docs/data-format.md). The field is plain text, so accept the
        // shapes people actually type: 2026-10-15, 2026/10/15, 2026.10.15,
        // 20261015, 2026年10月15日. Returns the canonical yyyy-MM-dd form.
        static func normalizedDate(_ text: String) -> String? {
            var cleaned = foldFullWidth(text)
                .trimmingCharacters(in: .whitespaces)
            for (glyph, ascii) in [("年", "-"), ("月", "-"), ("日", "")] {
                cleaned = cleaned.replacingOccurrences(of: glyph, with: ascii)
            }
        let parts = cleaned
            .components(separatedBy: CharacterSet(charactersIn: "-/."))
            .filter { !$0.isEmpty }
        let y: Int, m: Int, d: Int
        if parts.count == 3, let yy = Int(parts[0]), let mm = Int(parts[1]),
           let dd = Int(parts[2]) {
            (y, m, d) = (yy, mm, dd)
        } else if parts.count == 1, let only = parts.first, only.count == 8,
                  let compact = Int(only) {
            (y, m, d) = (compact / 10000, compact / 100 % 100, compact % 100)
        } else {
            return nil
        }
            // Calendar normalizes overflow (Feb 30 → Mar 1); round-trip to
            // reject it instead of silently shifting the day.
            let date = Calendar.current.date(from: DateComponents(year: y, month: m, day: d))
            let back = date.map { Calendar.current.dateComponents([.year, .month, .day], from: $0) }
            guard back?.year == y, back?.month == m, back?.day == d else { return nil }
            guard y >= 1990 else { return nil }
            let today = Calendar.current.dateComponents([.year, .month, .day], from: Date())
            guard (y, m, d) <= (today.year ?? y, today.month ?? m, today.day ?? d) else {
                return nil
            }
            return String(format: "%04d-%02d-%02d", y, m, d)
        }

        static func text(from date: Date) -> String {
            let parts = Calendar.current.dateComponents([.year, .month, .day], from: date)
            return String(format: "%04d-%02d-%02d",
                          parts.year ?? 0, parts.month ?? 0, parts.day ?? 0)
        }

        static func date(from text: String) -> Date? {
            let parts = normalizedDate(text) ?? ""
            let numbers = parts.split(separator: "-").compactMap { Int($0) }
            guard numbers.count == 3 else { return nil }
            return Calendar.current.date(from: DateComponents(year: numbers[0],
                                                              month: numbers[1],
                                                              day: numbers[2]))
        }
    }

    let draft: Draft
    /// persists the payload and returns the saved device's id; throws so
    /// the form itself can report the failure and offer a retry
    let onSave: (DeviceDraft) async throws -> String
    /// live snapshot of (draft, phase) after every change: the parent keeps
    /// it so a platform forced dismissal (#30) can re-present the sheet
    /// with exactly what the user typed and the failure banner intact
    let onSnapshot: ((Draft, SavePhase) -> Void)?
    /// a re-presented (rescued) form: keyboard focus lands in the price
    /// field — the field the user was editing when the platform killed
    /// the sheet. Fresh presentations stay unfocused per the contract.
    let rescued: Bool

    @Environment(\.dismiss) private var dismiss
    @State private var form: Draft
    @State private var phase: SavePhase

    init(draft: Draft,
         initialPhase: SavePhase = .idle,
         rescued: Bool = false,
         onSave: @escaping (DeviceDraft) async throws -> String,
         onSnapshot: ((Draft, SavePhase) -> Void)? = nil) {
        self.draft = draft
        self.onSave = onSave
        self.onSnapshot = onSnapshot
        self.rescued = rescued
        _form = State(initialValue: draft)
        _phase = State(initialValue: initialPhase)
    }

    private let icons = ["💻", "📱", "🖥", "⌚️", "🎧", "🔊", "📷", "🎮",
                         "📺", "⌨️", "🖱", "🖊", "🖨", "🚲", "📦"]
    private let categories: [(String, L10n.Key)] = [
        ("computer", .categoryComputer), ("phone", .categoryPhone),
        ("tablet", .categoryTablet), ("watch", .categoryWatch),
        ("audio", .categoryAudio),
        ("camera", .categoryCamera), ("gaming", .categoryGaming),
        ("appliance", .categoryAppliance), ("accessory", .categoryAccessory),
        ("other", .categoryOther)
    ]

    var body: some View {
        VStack(spacing: 0) {
            Text(draft.id == nil ? L10n.t(.addDevice) : L10n.t(.edit))
                .font(.headline)
                .padding(.top, 16)

            Form {
                Section {
                    TextField(L10n.t(.name), text: $form.name)

                    Picker(L10n.t(.icon), selection: $form.icon) {
                        ForEach(uniqueIcons, id: \.self) { icon in
                            Text(icon).tag(icon)
                        }
                    }

                    Picker(L10n.t(.category), selection: $form.category) {
                        ForEach(categories, id: \.0) { category in
                            Text(L10n.t(category.1)).tag(category.0)
                        }
                    }
                }

                Section(L10n.t(.price)) {
                    HStack {
                        TextField("0.00", text: $form.priceText)
                            .multilineTextAlignment(.trailing)
                            .focused($priceFieldFocused)
                        Text(form.currency)
                            .foregroundStyle(.secondary)
                    }
                    // the whole row — currency suffix included — starts editing
                    .contentShape(Rectangle())
                    .onTapGesture { priceFieldFocused = true }
                    if !priceValid {
                        hint(L10n.t(.priceRequiredHint))
                    }
                    HStack {
                        TextField(Draft.text(from: Date()), text: $form.dateText)
                        Spacer()
                        Button {
                            calendarPresented = true
                        } label: {
                            Image(systemName: "calendar")
                        }
                        .buttonStyle(.borderless)
                        .popover(isPresented: $calendarPresented, arrowEdge: .bottom) {
                            DateCalendarPopover(selection: calendarSelection)
                        }
                    }
                    if !dateValid {
                        hint(L10n.t(.dateInvalidHint))
                    }
                }

                Section {
                    Toggle(L10n.t(.willingLabel), isOn: $form.willingEnabled)
                    if form.willingEnabled {
                        HStack {
                            TextField("0.00", text: $form.willingText)
                                .multilineTextAlignment(.trailing)
                                .focused($willingFieldFocused)
                            Text(form.currency + L10n.t(.perDay))
                                .foregroundStyle(.secondary)
                        }
                        .contentShape(Rectangle())
                        .onTapGesture { willingFieldFocused = true }
                        if willingInvalid {
                            hint(L10n.t(.priceRequiredHint))
                        }
                    }
                    Text(L10n.t(.willingHint))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section(L10n.t(.notes)) {
                    TextField("", text: $form.notes, axis: .vertical)
                        .lineLimit(2...4)
                }
            }
            .formStyle(.grouped)
            .tint(PaybackTheme.accent)

            // the failure lives at the form with a relevant title, never
            // in the window-level generic alert (#24)
            if case .failed(let message) = phase {
                VStack(alignment: .leading, spacing: 4) {
                    Text(L10n.t(.saveFailed)).font(.callout.bold()).foregroundStyle(.red)
                    Text(message).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 16)
            }

            HStack {
                Button(L10n.t(.cancel)) {
                    phase = .idle  // an intended close: the parent must not re-assert (#30)
                    dismiss()
                }
                .keyboardShortcut(.cancelAction)
                .disabled(saving)  // the outcome must be known before leaving
                Spacer()
                Button(failed ? L10n.t(.retry) : L10n.t(.save)) {
                    save()
                }
                .keyboardShortcut(.defaultAction)
                .disabled(!isValid || saving)
            }
            .padding(16)
        }
        .frame(width: 420)
        // a failed form must not go down under the user's edits (#30):
        // current hosts dismiss the sheet when an edit on a failed form
        // results in a valid value, routed as an interactive dismissal —
        // block that path entirely while the outcome is unknown or the
        // form is failed. Esc still cancels through the cancel-action
        // button, and the explicit dismiss() calls ignore this modifier.
        .interactiveDismissDisabled(saving || failed)
        .onChange(of: form) { onSnapshot?(form, phase) }
        .onChange(of: phase) { onSnapshot?(form, phase) }
        .onAppear {
            guard rescued else { return }
            // next runloop: the FocusState needs the view installed; AX
            // focus alone does not establish a real field editor on
            // current macOS hosts, SwiftUI's own focus engine does
            DispatchQueue.main.async {
                priceFieldFocused = true
                FileHandle.standardError.write(
                    Data("payback: rescue: price field auto-focused active=\(NSApp.isActive)\n".utf8))
            }
        }
    }

    @State private var calendarPresented = false
    @FocusState private var priceFieldFocused: Bool
    @FocusState private var willingFieldFocused: Bool

    // The calendar writes back into the text field through a computed
    // binding: no extra state, and opening it never clobbers typed text.
    private var calendarSelection: Binding<Date> {
        Binding(get: { Draft.date(from: form.dateText) ?? Date() },
                set: { form.dateText = Draft.text(from: $0) })
    }

    private func hint(_ text: String) -> some View {
        Text(text)
            .font(.caption)
            .foregroundStyle(.red)
    }

    private var priceValid: Bool {
        Draft.parseMinor(form.priceText) > 0
    }

    private var dateValid: Bool {
        Draft.normalizedDate(form.dateText) != nil
    }

    // enabled + something typed that is not a positive amount = fix it;
    // enabled + empty = legitimately "not set"
    private var willingInvalid: Bool {
        form.willingEnabled
            && !form.willingText.trimmingCharacters(in: .whitespaces).isEmpty
            && Draft.parseMinor(form.willingText) <= 0
    }

    private var isValid: Bool {
        !form.name.trimmingCharacters(in: .whitespaces).isEmpty
            && priceValid && dateValid && !willingInvalid
    }

    private func save() {
        // one save in flight: the guard plus the disabled button makes a
        // double tap create exactly one device (#24)
        guard phase != .saving, let payload = form.toDeviceDraft() else { return }
        phase = .saving
        Task {
            do {
                _ = try await onSave(payload)
                phase = .idle  // an intended close: the parent must not re-assert (#30)
                dismiss()  // once, after persistence is confirmed
            } catch {
                // fields stay exactly as typed; Save turns into Retry
                phase = .failed(AppModel.cleanError(error))
            }
        }
    }

    private var saving: Bool { phase == .saving }

    private var failed: Bool {
        if case .failed = phase { return true }
        return false
    }

    private var uniqueIcons: [String] {
        var seen = Set<String>()
        return icons.filter { seen.insert($0).inserted }
    }
}

// The graphical date picker's chevrons step a month at a time and purchase
// dates often live years back, so the popover adds direct year/month fields
// and a jump-to-today button around it. `visible` steers which month the
// picker shows; picking a day (or steering) writes through `selection`.
private struct DateCalendarPopover: View {
    let selection: Binding<Date>

    @State private var visible: Date = Date()
    @State private var yearText: String = ""
    @State private var monthText: String = ""

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 6) {
                TextField(L10n.t(.yearField), text: $yearText)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 64)
                    .multilineTextAlignment(.trailing)
                Text(L10n.t(.yearField)).foregroundStyle(.secondary)
                TextField(L10n.t(.monthField), text: $monthText)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 40)
                    .multilineTextAlignment(.trailing)
                Text(L10n.t(.monthField)).foregroundStyle(.secondary)
                Spacer()
                Button(L10n.t(.jumpToToday)) { visible = Date() }
                    .buttonStyle(.borderless)
            }
            .font(.caption)
            .onChange(of: yearText) { steerIfValid() }
            .onChange(of: monthText) { steerIfValid() }

            DatePicker(L10n.t(.purchaseDate), selection: $visible,
                       displayedComponents: .date)
                .datePickerStyle(.graphical)
                .labelsHidden()
        }
        .padding(12)
        .onAppear {
            visible = selection.wrappedValue
            syncTexts()
        }
        .onChange(of: visible) { _, newValue in
            if Calendar.current.isDate(newValue, inSameDayAs: selection.wrappedValue) == false {
                selection.wrappedValue = newValue
            }
            syncTexts()
        }
    }

    // typing a complete year (or month) jumps the calendar; partial input
    // stays untouched because it cannot parse into the valid range
    private func steerIfValid() {
        guard let y = Int(yearText), (1990...9999).contains(y),
              let m = Int(monthText), (1...12).contains(m) else { return }
        let calendar = Calendar.current
        let day = min(calendar.component(.day, from: visible),
                      calendar.range(of: .day, in: .month, for: visible)?.count ?? 28)
        if let date = calendar.date(from: DateComponents(year: y, month: m, day: day)) {
            visible = date
        }
    }

    private func syncTexts() {
        let parts = Calendar.current.dateComponents([.year, .month], from: visible)
        if Int(yearText) != parts.year { yearText = String(parts.year ?? 2026) }
        if Int(monthText) != parts.month { monthText = String(parts.month ?? 1) }
    }
}

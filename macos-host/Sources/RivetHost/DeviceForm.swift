import SwiftUI

// Add / edit form. The backend re-validates everything; client-side checks
// only keep obvious typos from a round trip.

struct DeviceForm: View {
    struct Draft {
        var id: String?
        var name: String
        var icon: String
        var category: String
        var priceText: String
        var purchaseDate: Date
        var willingEnabled: Bool
        var willingText: String
        var notes: String
        var currency: String

        static func new(currency: String) -> Draft {
            Draft(id: nil, name: "", icon: "📦", category: "other",
                  priceText: "", purchaseDate: Date(),
                  willingEnabled: false, willingText: "", notes: "",
                  currency: currency)
        }

        static func from(_ device: Device) -> Draft {
            Draft(id: device.id,
                  name: device.name,
                  icon: device.icon,
                  category: device.category,
                  priceText: String(format: "%.2f", Double(device.priceMinor) / 100),
                  purchaseDate: Self.date(from: device.purchaseDate) ?? Date(),
                  willingEnabled: device.willingPerDayMinor != nil,
                  willingText: device.willingPerDayMinor.map {
                      String(format: "%.2f", Double($0) / 100)
                  } ?? "",
                  notes: device.notes ?? "",
                  currency: device.currency)
        }

        static func date(from string: String) -> Date? {
            let formatter = DateFormatter()
            formatter.dateFormat = "yyyy-MM-dd"
            formatter.locale = Locale(identifier: "en_US_POSIX")
            return formatter.date(from: string)
        }

        func toDeviceDraft() -> DeviceDraft? {
            let trimmed = name.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty else { return nil }
            let price = Self.parseMinor(priceText)
            guard price > 0 else { return nil }
            let willing = willingEnabled ? Self.parseMinor(willingText) : nil

            let formatter = DateFormatter()
            formatter.dateFormat = "yyyy-MM-dd"
            formatter.locale = Locale(identifier: "en_US_POSIX")

            return DeviceDraft(name: trimmed,
                               icon: icon.isEmpty ? "📦" : String(icon.prefix(4)),
                               category: category,
                               priceMinor: price,
                               currency: currency,
                               purchaseDate: formatter.string(from: purchaseDate),
                               willingPerDayMinor: (willing ?? 0) > 0 ? willing : nil,
                               notes: notes)
        }

        static func parseMinor(_ text: String) -> Int64 {
            let cleaned = text.replacingOccurrences(of: ",", with: ".")
                .trimmingCharacters(in: .whitespaces)
            guard let value = Double(cleaned), value.isFinite else { return 0 }
            return Int64((value * 100).rounded())
        }
    }

    let draft: Draft
    let onSave: (DeviceDraft) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var form: Draft

    init(draft: Draft, onSave: @escaping (DeviceDraft) -> Void) {
        self.draft = draft
        self.onSave = onSave
        _form = State(initialValue: draft)
    }

    private let icons = ["💻", "📱", "📱", "🖥", "⌚️", "🎧", "🔊", "📷", "🎮",
                         "📺", "⌨️", "🖱", "🖊", "🖨", "🚲", "📦"]
    private let categories: [(String, L10n.Key)] = [
        ("computer", .categoryComputer), ("phone", .categoryPhone),
        ("tablet", .categoryTablet), ("audio", .categoryAudio),
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
                        Text(form.currency)
                            .foregroundStyle(.secondary)
                    }
                    DatePicker(L10n.t(.purchaseDate),
                               selection: $form.purchaseDate,
                               displayedComponents: .date)
                }

                Section {
                    Toggle(L10n.t(.willingLabel), isOn: $form.willingEnabled)
                    if form.willingEnabled {
                        HStack {
                            TextField("0.00", text: $form.willingText)
                                .multilineTextAlignment(.trailing)
                            Text(form.currency + L10n.t(.perDay))
                                .foregroundStyle(.secondary)
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

            HStack {
                Button(L10n.t(.cancel)) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button(L10n.t(.save)) { save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
            }
            .padding(16)
        }
        .frame(width: 420)
    }

    private var uniqueIcons: [String] {
        var seen = Set<String>()
        return icons.filter { seen.insert($0).inserted }
    }

    private var isValid: Bool {
        !form.name.trimmingCharacters(in: .whitespaces).isEmpty
            && Draft.parseMinor(form.priceText) > 0
    }

    private func save() {
        if let payload = form.toDeviceDraft() {
            onSave(payload)
            dismiss()
        }
    }
}

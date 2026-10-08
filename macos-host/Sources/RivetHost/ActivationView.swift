import SwiftUI

// Activation sheet for Payback Pro: paste the PB1 token from the purchase
// email; verification is offline (app/license.rkt). Shows the current
// license state, so the sheet doubles as the "about licensing" surface.

struct ActivationView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var key = ""
    @State private var state: LicenseState?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 10) {
                Image(systemName: "lock.open")
                    .font(.system(size: 26))
                    .foregroundStyle(PaybackTheme.accent)
                Text(L10n.t(.activateProMenu))
                    .font(.title3.bold())
            }

            if let state, state.licensed {
                Label {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(L10n.t(.proActiveTitle)).bold()
                        if let subject = state.subject {
                            Text(L10n.t(.proLicensedTo)
                                .replacingOccurrences(of: "{name}", with: subject))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        if let expiry = state.expiry {
                            Text("\(L10n.t(.paybackEta)): \(expiry)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                } icon: {
                    Image(systemName: "checkmark.seal.fill")
                        .foregroundStyle(PaybackTheme.earn)
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(
                    RoundedRectangle(cornerRadius: PaybackTheme.cardCorner, style: .continuous)
                        .fill(PaybackTheme.earnSoft)
                )
            } else {
                Text(L10n.t(.freeDeviceLimitNote))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                TextEditor(text: $key)
                    .font(.system(.callout, design: .monospaced))
                    .frame(width: 360, height: 84)
                    .overlay(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .strokeBorder(.quaternary)
                    )
                Text(L10n.t(.licenseHint))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Spacer()
                    Button(L10n.t(.activateButton)) { model.activateLicense(key) }
                        .buttonStyle(.borderedProminent)
                        .tint(PaybackTheme.accent)
                        .disabled(key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        .keyboardShortcut(.defaultAction)
                }
            }

            HStack {
                Spacer()
                Button(L10n.t(.close)) { dismiss() }
            }
        }
        .padding(24)
        .frame(width: 410)
        .background(PaybackTheme.paper)
        .tint(PaybackTheme.accent)
        .task { await loadState() }
    }

    private func loadState() async {
        guard let api = model.api else { return }
        do {
            state = try JSONDecoder().decode(
                LicenseState.self, from: try await api.license_state())
        } catch {
            // the sheet still works without the state line
        }
    }
}

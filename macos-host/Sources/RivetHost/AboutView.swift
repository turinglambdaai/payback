import SwiftUI

// About sheet (Help ▸ About Payback): version, product line, license line,
// and the two links that route feedback — website and issue tracker.

struct AboutSheet: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss

    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? RivetGeneratedConfig.version
    }

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "banknote")
                .font(.system(size: 44))
                .foregroundStyle(PaybackTheme.accent)
            Text("Payback v\(version)")
                .font(.system(size: 15, weight: .semibold))
            Text("© 2026 Payback Team")
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(L10n.t(.aboutContent))
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Text(L10n.t(.aboutLicense))
                .font(.caption2)
                .foregroundStyle(.secondary)
            HStack(spacing: 20) {
                Link(L10n.t(.aboutWebsite),
                     destination: URL(string: "https://payback.jrtx.site")!)
                Link(L10n.t(.aboutReportIssue),
                     destination: URL(string: "https://github.com/turinglambdaai/payback/issues")!)
            }
            .font(.caption)
            .tint(PaybackTheme.accent)
            Button(L10n.t(.close)) { dismiss() }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
                .tint(PaybackTheme.accent)
        }
        .padding(28)
        .frame(width: 320)
    }
}

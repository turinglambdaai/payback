import Foundation

// Money formatting from integer minor units. Symbols follow the record's
// currency code; grouping and decimals follow the user's locale.

enum Money {
    private static func formatter(code: String) -> NumberFormatter {
        let formatter = NumberFormatter()
        formatter.numberStyle = .currency
        formatter.currencyCode = code
        formatter.maximumFractionDigits = 2
        formatter.minimumFractionDigits = 2
        return formatter
    }

    static func major(_ minor: Int64, code: String) -> String {
        formatter(code: code).string(from: NSNumber(value: Double(minor) / 100))
            ?? String(format: "%.2f", Double(minor) / 100)
    }

    static func major(_ minor: Double, code: String) -> String {
        formatter(code: code).string(from: NSNumber(value: minor / 100))
            ?? String(format: "%.2f", minor / 100)
    }

    /// daily cost: at least two decimals, three when under 0.10
    static func perDay(_ minor: Double, code: String) -> String {
        let value = minor / 100
        let formatter = formatter(code: code)
        if value > 0 && value < 0.10 {
            formatter.maximumFractionDigits = 3
            formatter.minimumFractionDigits = 3
        }
        return formatter.string(from: NSNumber(value: value))
            ?? String(format: "%.2f", value)
    }

    static func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

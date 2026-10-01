import Foundation

// Money formatting from integer minor units. Symbols follow the record's
// currency code; grouping and decimals follow the user's locale.

enum Money {
    // NumberFormatter allocation is expensive and this runs for every tile,
    // card, and curve label on every render — one instance per currency code,
    // guarded by a lock because Money is called from view bodies and from
    // background decode tasks alike. nonisolated(unsafe) matches L10n: the
    // generated types stay usable without concurrency annotations.
    private static let lock = NSLock()
    private nonisolated(unsafe) static var cache: [String: NumberFormatter] = [:]

    private static func baseFormatter(code: String) -> NumberFormatter {
        lock.lock()
        defer { lock.unlock() }
        if let cached = cache[code] {
            return cached
        }
        let formatter = NumberFormatter()
        formatter.numberStyle = .currency
        formatter.currencyCode = code
        formatter.maximumFractionDigits = 2
        formatter.minimumFractionDigits = 2
        cache[code] = formatter
        return formatter
    }

    /// The cached formatter is shared and never mutated; callers needing
    /// different fraction digits work on a copy.
    private static func string(_ value: Double, code: String, fractionDigits: Int) -> String {
        let formatter: NumberFormatter
        if fractionDigits == 2 {
            formatter = baseFormatter(code: code)
        } else {
            formatter = baseFormatter(code: code).copy() as! NumberFormatter
            formatter.maximumFractionDigits = fractionDigits
            formatter.minimumFractionDigits = fractionDigits
        }
        return formatter.string(from: NSNumber(value: value))
            ?? String(format: "%.\(fractionDigits)f", value)
    }

    static func major(_ minor: Int64, code: String) -> String {
        string(Double(minor) / 100, code: code, fractionDigits: 2)
    }

    static func major(_ minor: Double, code: String) -> String {
        string(minor / 100, code: code, fractionDigits: 2)
    }

    /// daily cost: at least two decimals, three when under 0.10
    static func perDay(_ minor: Double, code: String) -> String {
        let value = minor / 100
        let digits = (value > 0 && value < 0.10) ? 3 : 2
        return string(value, code: code, fractionDigits: digits)
    }

    static func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

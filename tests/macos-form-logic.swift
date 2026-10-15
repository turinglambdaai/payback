// Form input logic tests for the macOS host, run standalone by CI:
//   swift tests/macos-form-logic.swift
// The functions under test live in DeviceForm.Draft (the app executable has
// no test target), so the implementations are mirrored here — keep them in
// sync with macos-host/Sources/RivetHost/DeviceForm.swift. These tests exist
// because the first draft of normalizedDate rejected "20261015" and the
// full-width separators ／ － that Chinese IMEs actually emit.

import Foundation

// ---------- mirror of DeviceForm.Draft (keep in sync) ----------

struct Draft {
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
}

// ---------- harness ----------

var failures = 0

func checkEqual<T: Equatable>(_ got: T, _ want: T, _ label: String) {
    if got != want {
        failures += 1
        print("FAIL \(label): got=\(got) want=\(want)")
    } else {
        print("ok   \(label)")
    }
}

// ---------- amounts ----------

checkEqual(Draft.parseMinor("5999"), 599_900, "plain integer")
checkEqual(Draft.parseMinor("5999.50"), 599_950, "decimal point")
checkEqual(Draft.parseMinor("5,999"), 599_900, "thousands separator")
checkEqual(Draft.parseMinor("1,234,567"), 123_456_700, "nested thousands")
checkEqual(Draft.parseMinor("5,99"), 599, "comma as decimal point")
checkEqual(Draft.parseMinor("５９９９"), 599_900, "full-width digits")
checkEqual(Draft.parseMinor("５，９９９"), 599_900, "full-width thousands comma")
checkEqual(Draft.parseMinor("5999abc"), 0, "trailing junk rejected")
checkEqual(Draft.parseMinor(""), 0, "empty rejected")
checkEqual(Draft.parseMinor("abc"), 0, "garbage rejected")

// ---------- dates ----------

checkEqual(Draft.normalizedDate("2026-10-15"), "2026-10-15", "iso")
checkEqual(Draft.normalizedDate("2026/10/15"), "2026-10-15", "slashes")
checkEqual(Draft.normalizedDate("2026.10.15"), "2026-10-15", "dots")
checkEqual(Draft.normalizedDate("20261015"), "2026-10-15", "compact")
checkEqual(Draft.normalizedDate("２０２６／１０／１５"), "2026-10-15", "full-width slashes+digits")
checkEqual(Draft.normalizedDate("２０２６－１０－１５"), "2026-10-15", "full-width hyphens")
checkEqual(Draft.normalizedDate("２０２６１０１５"), "2026-10-15", "full-width compact")
checkEqual(Draft.normalizedDate("2024年2月4日"), "2024-02-04", "年月日")
checkEqual(Draft.normalizedDate("2024/2/4"), "2024-02-04", "single-digit parts")
checkEqual(Draft.normalizedDate("  2026-10-15  "), "2026-10-15", "whitespace trimmed")
checkEqual(Draft.normalizedDate("abc"), nil, "garbage rejected")
checkEqual(Draft.normalizedDate("５９９９"), nil, "amount in date field rejected")
checkEqual(Draft.normalizedDate("2026-13-01"), nil, "month 13 rejected")
checkEqual(Draft.normalizedDate("2026-02-30"), nil, "feb 30 rejected")
checkEqual(Draft.normalizedDate("1989-12-31"), nil, "before 1990 rejected")
checkEqual(Draft.normalizedDate("2077-01-01"), nil, "future rejected")
checkEqual(Draft.normalizedDate("2026-10-15 extra"), nil, "trailing junk rejected")

let today = Calendar.current.dateComponents([.year, .month, .day], from: Date())
let todayText = String(format: "%04d-%02d-%02d", today.year!, today.month!, today.day!)
checkEqual(Draft.normalizedDate(todayText), todayText, "today itself accepted")

if failures == 0 {
    print("\nALL PASS")
    exit(0)
}
print("\n\(failures) FAILURES")
exit(1)

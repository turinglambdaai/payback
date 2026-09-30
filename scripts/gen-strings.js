#!/usr/bin/env node
// Generate platform UI string tables from shared/strings/strings.json.
//
//   node scripts/gen-strings.js          regenerate L10n.swift + Strings.h
//   node scripts/gen-strings.js --check  exit 1 when the checked-in files
//                                        differ from the generated output
//
// shared/strings/strings.json is the single source of truth; the generated
// files are committed so building an app never requires Node.

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const source = JSON.parse(
  fs.readFileSync(path.join(root, "shared/strings/strings.json"), "utf8"));

const check = process.argv.includes("--check");

function fail(message) {
  console.error(`gen-strings: ${message}`);
  process.exit(1);
}

for (const [key, translations] of Object.entries(source)) {
  if (!/^[a-z][A-Za-z0-9]*$/.test(key)) {
    fail(`key "${key}" must be lowerCamelCase`);
  }
  for (const language of ["zh", "en"]) {
    if (typeof translations[language] !== "string" || !translations[language]) {
      fail(`key "${key}" is missing a "${language}" string`);
    }
  }
  const extra = Object.keys(translations).filter((l) => l !== "zh" && l !== "en");
  if (extra.length) {
    fail(`key "${key}" has unexpected languages: ${extra.join(", ")}`);
  }
}

function escapeSwift(text) {
  return text.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
}

function escapeCpp(text) {
  return text.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
}

function lowerSnake(camel) {
  return camel.replace(/([A-Z])/g, "_$1").toLowerCase();
}

const cppKeywords = new Set(["delete", "new", "class", "template", "operator",
  "private", "public", "protected", "return", "switch", "case", "default",
  "if", "else", "for", "while", "namespace", "struct", "union", "auto"]);

const cppMethodAliases = { delete: "remove" };

function cppMethod(camel) {
  const snake = lowerSnake(camel);
  if (cppMethodAliases[snake]) return cppMethodAliases[snake];
  return cppKeywords.has(snake) ? snake + "_key" : snake;
}

// ---------- Swift -----------------------------------------------------------

const swiftKeys = Object.keys(source).map((key) => `        case ${key}`);

const swiftCases = Object.entries(source)
  .map(([key, translations]) => `            case .${key}: return "${escapeSwift(translations.zh)}"`)
  .join("\n");

const swiftEn = Object.entries(source)
  .map(([key, translations]) => `            case .${key}: return "${escapeSwift(translations.en)}"`)
  .join("\n");

const swift = `import SwiftUI

// GENERATED from shared/strings/strings.json by scripts/gen-strings.js.
// Do not edit by hand: change strings.json and re-run the generator.
// Language resolution: an in-app override ("zh" / "en") wins; otherwise
// the system language decides, with zh as the fallback.

enum L10n {
    /// in-app language override; nil = follow the system. UI-set on the main
    /// actor only; nonisolated(unsafe) keeps the generated type usable from
    /// any view body without concurrency annotations.
    nonisolated(unsafe) static var override: String?

    static var prefersChinese: Bool {
        switch override {
        case "zh": return true
        case "en": return false
        default: return Locale.preferredLanguages.first?.hasPrefix("zh") ?? true
        }
    }

    static func t(_ key: Key) -> String {
        prefersChinese ? key.zh : key.en
    }

    enum Key {
${swiftKeys.join("\n")}

        var zh: String {
            switch self {
${swiftCases}
            }
        }

        var en: String {
            switch self {
${swiftEn}
            }
        }
    }
}
`;

// ---------- C++ -------------------------------------------------------------

const cppMethods = Object.entries(source)
  .map(([key, translations]) => {
    const method = cppMethod(key);
    const zh = `L"${escapeCpp(translations.zh)}"`;
    const en = `L"${escapeCpp(translations.en)}"`;
    return `  std::wstring ${method}() const { return zh ? ${zh} : ${en}; }`;
  })
  .join("\n");

const cpp = `// GENERATED from shared/strings/strings.json by scripts/gen-strings.js.
// Do not edit by hand: change strings.json and re-run the generator.
#pragma once

#include <windows.h>
#include <string>

namespace payback::strings {

inline bool chinese_ui() {
  // WinAppSDK's cppwinrt projection does not carry Windows.System.Profile;
  // the plain Win32 language query covers zh-Hans and zh-Hant alike.
  return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
}

// zh is the primary audience; English is selected automatically for
// non-Chinese systems.
struct Strings {
  bool zh;

  explicit Strings(bool chinese) : zh(chinese) {}

${cppMethods}
};

inline Strings const& strings() {
  static Strings const value{chinese_ui()};
  return value;
}

}  // namespace payback::strings
`;

// ---------- write or verify --------------------------------------------------

const targets = [
  {
    file: path.join(root, "macos-host/Sources/RivetHost/L10n.swift"),
    content: swift,
  },
  {
    file: path.join(root, "windows/Strings.h"),
    content: cpp,
  },
];

let dirty = 0;
for (const target of targets) {
  const existing = fs.existsSync(target.file)
    ? fs.readFileSync(target.file, "utf8")
    : null;
  if (existing !== target.content) {
    if (check) {
      console.error(`gen-strings: out of date: ${path.relative(root, target.file)}`);
      dirty += 1;
    } else {
      fs.writeFileSync(target.file, target.content);
      console.log(`gen-strings: wrote ${path.relative(root, target.file)}`);
    }
  }
}

if (dirty > 0) {
  process.exit(1);
}
console.log("gen-strings: strings tables are up to date");

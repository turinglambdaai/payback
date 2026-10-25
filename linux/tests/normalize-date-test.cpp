// Regression harness for normalize_date (issue #22): the canonical
// yyyy-MM-dd shape used to be read as 2026, -10, -15 and rejected by the
// form. Includes the host source with main() renamed so the parser and its
// glib calendar checks run exactly as shipped, then exercises the shapes
// named in the wire contract (docs/data-format.md) plus the rejection set.
#define main rivet_host_main
#include "../src/main.cpp"
#undef main

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect_date(std::string const& label, std::string const& input,
                 std::string const& want) {
  std::string const got = normalize_date(input);
  if (got == want) {
    std::printf("ok   %-28s %-28s -> %s\n", label.c_str(), input.c_str(),
                got.c_str());
    return;
  }
  ++failures;
  std::printf("FAIL %-28s %-28s -> %s (want %s)\n", label.c_str(),
              input.c_str(), got.c_str(), want.c_str());
}

}  // namespace

int main() {
  // Accepted shapes from the contract comment, canonical first — that is
  // the wire format the form used to reject.
  expect_date("canonical hyphen", "2026-10-15", "2026-10-15");
  expect_date("slash", "2026/10/15", "2026-10-15");
  expect_date("dot", "2026.10.15", "2026-10-15");
  expect_date("compact", "20261015", "2026-10-15");
  expect_date("chinese", "2026年10月15日", "2026-10-15");
  expect_date("full-width", "２０２６－１０－１５", "2026-10-15");
  expect_date("single-digit fields", "2026/1/5", "2026-01-05");
  expect_date("surrounding spaces", "  2026-10-15  ", "2026-10-15");

  // Out-of-range and calendar-invalid input stays rejected.
  expect_date("feb 30", "2026-02-30", "");
  expect_date("month 13", "2026-13-01", "");
  expect_date("before 1990", "1989-12-31", "");
  expect_date("far future", "9999-12-31", "");

  // After today: computed from the same local clock normalize_date uses,
  // so this stays meaningful without injecting a reference date.
  GDateTime* now = g_date_time_new_now_local();
  GDateTime* tomorrow = g_date_time_add_days(now, 1);
  char* tomorrow_text = g_date_time_format(tomorrow, "%Y-%m-%d");
  expect_date("tomorrow", tomorrow_text, "");
  g_free(tomorrow_text);
  g_date_time_unref(tomorrow);
  g_date_time_unref(now);

  // Garbage, partial entries and parse artifacts stay rejected; they are
  // form error states, never zero or a silently different date.
  expect_date("letters", "abc", "");
  expect_date("empty", "", "");
  expect_date("spaces only", "   ", "");
  expect_date("partial year-month", "2026-10", "");
  expect_date("trailing field", "2026 10 15 15", "");
  expect_date("dangling separator", "2026-10-15-", "");
  expect_date("sign prefix", "-2026-10-15", "");

  if (failures != 0) {
    std::printf("%d case(s) failed\n", failures);
    return 1;
  }
  std::printf("all normalize_date cases passed\n");
  return 0;
}

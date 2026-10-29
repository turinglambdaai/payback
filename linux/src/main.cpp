// Payback Linux host — a GTK4 window over one embedded Racket CS backend
// (rivet architecture, parity with the SwiftUI and WinUI hosts).
//
// Every user-visible number comes from the backend over RVT1; this file only
// renders and collects input. RPC completions arrive on RVT1 reader threads,
// so each one is flattened (pb::unpack) and posted to the GTK main loop via
// g_idle_add (pb::post_to_main) before any widget is touched.
//
// GTK4 notes: gtk_dialog_run is gone, so every dialog is response-signal
// driven with a heap context struct freed in the handler; dialogs destroy
// themselves before follow-up flows fire (via post_to_main hops).
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "GeneratedBackend.hpp"
#include "Strings.h"
#include "system_services.hpp"  // rivet::system — single-instance lease

namespace {

// ------------------------------------------------------------- dispatch

namespace pb {

namespace detail {

template <typename T>
int run_on_main(gpointer data) {
  std::unique_ptr<std::pair<std::function<void(T&)>, T>> task(
      static_cast<std::pair<std::function<void(T&)>, T>*>(data));
  task->first(task->second);
  return G_SOURCE_REMOVE;
}

}  // namespace detail

template <typename T>
void post_to_main(std::function<void(T&)> run, T payload) {
  auto* task = new std::pair<std::function<void(T&)>, T>(std::move(run),
                                                         std::move(payload));
  g_idle_add(detail::run_on_main<T>, task);
}

template <typename T>
struct Unpacked {
  bool ok{false};
  T value{};
  std::string error;
};

template <>
struct Unpacked<void> {
  bool ok{false};
  std::string error;
};

template <typename T>
Unpacked<T> unpack(rivet_app::Result<T> result) {
  Unpacked<T> out;
  try {
    out.value = result.get();
    out.ok = true;
  } catch (std::exception const& e) {
    out.error = e.what();
  } catch (...) {
    out.error = "unknown backend failure";
  }
  return out;
}

inline Unpacked<void> unpack(rivet_app::Result<void> result) {
  Unpacked<void> out;
  try {
    result.get();
    out.ok = true;
  } catch (std::exception const& e) {
    out.error = e.what();
  } catch (...) {
    out.error = "unknown backend failure";
  }
  return out;
}

}  // namespace pb

// ------------------------------------------------------------ i18n state

// -1 follow system, 0 zh, 1 en; persisted beside the backend data dir.
int g_language_override = -1;

payback::linux_strings::Strings strings() {
  if (g_language_override == 0) return payback::linux_strings::Strings(true);
  if (g_language_override == 1) return payback::linux_strings::Strings(false);
  bool const system_zh = [] {
    char const* const* languages = g_get_language_names();
    for (int i = 0; languages[i] != nullptr; ++i) {
      std::string const lang = languages[i];
      if (lang.rfind("zh", 0) == 0) return true;
      if (lang.rfind("en", 0) == 0) return false;
    }
    return true;
  }();
  return payback::linux_strings::Strings(system_zh);
}

std::filesystem::path language_settings_path() {
  return std::filesystem::path(g_get_user_data_dir()) / "payback" /
         "ui-language.txt";
}

void load_language_override() {
  std::ifstream file(language_settings_path());
  std::string line;
  if (std::getline(file, line)) {
    g_language_override = line == "zh" ? 0 : line == "en" ? 1 : -1;
  }
}

void save_language_override(int value) {
  std::filesystem::path const path = language_settings_path();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::trunc);
  file << (value == 0 ? "zh" : value == 1 ? "en" : "system") << '\n';
}

// ---------------------------------------------------------- json helpers

// RAII parser holder: json-glib nodes are owned by the parser.
class JsonDoc {
 public:
  explicit JsonDoc(std::string const& text) {
    parser_ = json_parser_new();
    loaded_ = json_parser_load_from_data(parser_, text.data(),
                                         static_cast<gssize>(text.size()),
                                         nullptr) != 0;
  }
  ~JsonDoc() {
    if (parser_ != nullptr) g_object_unref(parser_);
  }
  JsonDoc(JsonDoc const&) = delete;
  JsonDoc& operator=(JsonDoc const&) = delete;

  bool ok() const { return loaded_; }
  JsonObject* root() const {
    if (!loaded_) return nullptr;
    JsonNode* node = json_parser_get_root(parser_);
    if (node == nullptr || !JSON_NODE_HOLDS_OBJECT(node)) return nullptr;
    return json_node_get_object(node);
  }

 private:
  JsonParser* parser_{nullptr};
  bool loaded_{false};
};

JsonNode* field(JsonObject* object, char const* key) {
  if (object == nullptr) return nullptr;
  if (!json_object_has_member(object, key)) return nullptr;
  return json_object_get_member(object, key);
}

bool jnull(JsonObject* object, char const* key) {
  JsonNode* node = field(object, key);
  return node == nullptr || json_node_is_null(node);
}

std::string jstr(JsonObject* object, char const* key, std::string fallback = "") {
  JsonNode* node = field(object, key);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    return fallback;
  }
  char const* value = json_node_get_string(node);
  return value == nullptr ? fallback : std::string(value);
}

gint64 jint(JsonObject* object, char const* key, gint64 fallback = 0) {
  JsonNode* node = field(object, key);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
  GType const type = json_node_get_value_type(node);
  if (type == G_TYPE_INT64) return json_node_get_int(node);
  if (type == G_TYPE_DOUBLE) return static_cast<gint64>(json_node_get_double(node));
  return fallback;
}

double jdouble(JsonObject* object, char const* key, double fallback = 0.0) {
  JsonNode* node = field(object, key);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
  GType const type = json_node_get_value_type(node);
  if (type == G_TYPE_INT64) return static_cast<double>(json_node_get_int(node));
  if (type == G_TYPE_DOUBLE) return json_node_get_double(node);
  return fallback;
}

bool jbool(JsonObject* object, char const* key, bool fallback = false) {
  JsonNode* node = field(object, key);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
    return fallback;
  }
  return json_node_get_boolean(node) != 0;
}

bool jhas(JsonObject* object, char const* key) {
  return object != nullptr && json_object_has_member(object, key) &&
         !json_node_is_null(json_object_get_member(object, key));
}

JsonObject* jobj(JsonObject* object, char const* key) {
  JsonNode* node = field(object, key);
  if (node == nullptr || !JSON_NODE_HOLDS_OBJECT(node)) return nullptr;
  return json_node_get_object(node);
}

JsonArray* jarray(JsonObject* object, char const* key) {
  JsonNode* node = field(object, key);
  if (node == nullptr || !JSON_NODE_HOLDS_ARRAY(node)) return nullptr;
  return json_node_get_array(node);
}

guint jlen(JsonArray* array) {
  return array == nullptr ? 0 : json_array_get_length(array);
}

JsonObject* jat(JsonArray* array, guint index) {
  JsonNode* node = json_array_get_element(array, index);
  if (node == nullptr || !JSON_NODE_HOLDS_OBJECT(node)) return nullptr;
  return json_node_get_object(node);
}

// JSON payloads are built with a JsonBuilder (json-glib handles escaping).
std::string build_json(std::function<void(JsonBuilder*)> build) {
  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  build(builder);
  json_builder_end_object(builder);
  JsonGenerator* generator = json_generator_new();
  json_generator_set_root(generator, json_builder_get_root(builder));
  gchar* text = json_generator_to_data(generator, nullptr);
  std::string const out(text == nullptr ? "" : text);
  g_free(text);
  g_object_unref(generator);
  g_object_unref(builder);
  return out;
}

void builder_string(JsonBuilder* builder, char const* key,
                    std::string const& value) {
  json_builder_set_member_name(builder, key);
  json_builder_add_string_value(builder, value.c_str());
}

void builder_int(JsonBuilder* builder, char const* key, gint64 value) {
  json_builder_set_member_name(builder, key);
  json_builder_add_int_value(builder, value);
}

void builder_null(JsonBuilder* builder, char const* key) {
  json_builder_set_member_name(builder, key);
  json_builder_add_null_value(builder);
}

rivet::Bytes to_bytes(std::string const& text) {
  return rivet::Bytes(text.begin(), text.end());
}

std::string bytes_to_string(rivet::Bytes const& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// ---------------------------------------------------------- money & dates

std::string currency_symbol(std::string const& code) {
  if (code == "CNY") return "¥";
  if (code == "USD") return "$";
  if (code == "EUR") return "€";
  if (code == "GBP") return "£";
  if (code == "JPY") return "¥";
  if (code == "HKD") return "HK$";
  if (code == "TWD") return "NT$";
  if (code == "KRW") return "₩";
  return code + " ";
}

std::string money(double minor, std::string const& code) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.2f", minor / 100.0);
  return currency_symbol(code) + buffer;
}

std::string format_size(double bytes) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.1f MB", bytes / (1024.0 * 1024.0));
  return buffer;
}

int day_of_year() {
  GDateTime* now = g_date_time_new_now_local();
  int const day = g_date_time_get_day_of_year(now);
  g_date_time_unref(now);
  return day;
}

std::string today_string() {
  GDateTime* now = g_date_time_new_now_local();
  std::string const today = g_date_time_format(now, "%Y-%m-%d");
  g_date_time_unref(now);
  return today;
}

// Chinese IMEs emit full-width digits and punctuation; fold them so
// "５９９９" and "5，999" read like their half-width equivalents.
std::string fold_full_width(std::string const& text) {
  std::string folded;
  folded.reserve(text.size());
  for (const char* p = text.c_str(); *p != '\0'; p = g_utf8_next_char(p)) {
    gunichar const ch = g_utf8_get_char(p);
    switch (ch) {
      case 0xFF10 ... 0xFF19:
        folded.push_back(static_cast<char>(ch - 0xFF10 + '0'));
        break;
      case 0xFF0C:
        folded.push_back(',');
        break;
      case 0xFF0D:
        folded.push_back('-');
        break;
      case 0xFF0E:
        folded.push_back('.');
        break;
      case 0xFF0F:
        folded.push_back('/');
        break;
      default:
        folded.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
    }
  }
  return folded;
}

// Strict amount parsing in minor units (x100, rounded): the whole text must
// be one finite number after folding; anything else is 0 (mirrors the mac
// host, so a bad field never rounds-trips to the backend).
gint64 parse_amount_minor(std::string const& text) {
  std::string cleaned = fold_full_width(text);
  // "5,999" (every comma group exactly 3 digits) reads as a thousands
  // separator; anything else treats the comma as a decimal point
  {
    std::vector<std::string> groups;
    std::size_t start = 0;
    for (;;) {
      std::size_t const at = cleaned.find(',', start);
      if (at == std::string::npos) {
        groups.push_back(cleaned.substr(start));
        break;
      }
      groups.push_back(cleaned.substr(start, at - start));
      start = at + 1;
    }
    bool thousands = groups.size() > 1;
    for (std::size_t i = 1; i < groups.size() && thousands; ++i) {
      thousands = groups[i].size() == 3 &&
                  groups[i].find_first_not_of("0123456789") ==
                      std::string::npos;
    }
    if (thousands) {
      cleaned.clear();
      for (std::string const& group : groups) cleaned += group;
    } else {
      std::replace(cleaned.begin(), cleaned.end(), ',', '.');
    }
  }
  char* end = nullptr;
  double const value = std::strtod(cleaned.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
    return 0;
  }
  return static_cast<gint64>(value * 100.0 + 0.5);
}

// The wire contract is yyyy-MM-dd within 1990-01-01…today
// (docs/data-format.md). The entry is plain text, so accept the shapes
// people actually type: 2026-10-15, 2026/10/15, 2026.10.15, 20261015,
// 2026年10月15日. Returns the canonical yyyy-MM-dd form, or "" when the
// text is not a usable in-range date.
std::string normalize_date(std::string const& text) {
  std::string cleaned = fold_full_width(text);
  auto replace_all = [](std::string& target, std::string const& from,
                        std::string const& to) {
    std::size_t at;
    while ((at = target.find(from)) != std::string::npos) {
      target.replace(at, from.size(), to);
    }
  };
  replace_all(cleaned, "年", "-");
  replace_all(cleaned, "月", "-");
  replace_all(cleaned, "日", "");
  for (char& c : cleaned) {
    if (c == '/' || c == '.') c = ' ';
  }
  std::size_t const first = cleaned.find_first_not_of(' ');
  if (first == std::string::npos) {
    return "";
  }
  cleaned.erase(0, first);
  cleaned.erase(cleaned.find_last_not_of(' ') + 1);
  if (cleaned.find_first_not_of("0123456789- ") != std::string::npos) {
    return "";
  }
  // A leading '-' is a sign, not a date shape; reject it before folding.
  if (cleaned[0] == '-') {
    return "";
  }
  // Every supported shape ends in a day digit; a dangling separator or a
  // trailing fragment is garbage, not a date.
  if (cleaned.back() < '0' || cleaned.back() > '9') {
    return "";
  }
  int y = 0;
  int m = 0;
  int d = 0;
  if (cleaned.size() == 8 &&
      cleaned.find_first_not_of("0123456789") == std::string::npos) {
    y = std::stoi(cleaned.substr(0, 4));
    m = std::stoi(cleaned.substr(4, 2));
    d = std::stoi(cleaned.substr(6, 2));
  } else {
    // Hyphen is a separator in every supported shape; fold it to a space
    // so the stream reads 2026-10-15 as three fields, not 2026, -10, -15.
    std::string fields = cleaned;
    std::replace(fields.begin(), fields.end(), '-', ' ');
    std::istringstream stream(fields);
    if (!(stream >> y >> m >> d)) {
      return "";
    }
    stream >> std::ws;
    if (!stream.eof()) {
      return "";  // trailing garbage is not a date
    }
  }
  // g_date_new_dmy validates the calendar (Feb 30 is rejected, not shifted)
  if (y < 1990 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
    return "";
  }
  GDate* parsed = g_date_new_dmy(static_cast<GDateDay>(d),
                                 static_cast<GDateMonth>(m),
                                 static_cast<GDateYear>(y));
  if (parsed == nullptr) {
    return "";
  }
  GDateTime* now = g_date_time_new_now_local();
  GDate* today = g_date_new_dmy(
      static_cast<GDateDay>(g_date_time_get_day_of_month(now)),
      static_cast<GDateMonth>(g_date_time_get_month(now)),
      static_cast<GDateYear>(g_date_time_get_year(now)));
  bool const in_range = g_date_compare(parsed, today) <= 0;
  char buffer[16]{};
  g_date_strftime(buffer, sizeof buffer, "%Y-%m-%d", parsed);
  g_date_time_unref(now);
  g_date_free(today);
  g_date_free(parsed);
  return in_range ? std::string(buffer) : std::string();
}

// ------------------------------------------------------------ wire state

struct Milestone {
  std::string key;
  bool achieved{false};
  bool is_new{false};  // ack-on-read celebration flag from load-all
};

struct Computed {
  gint64 days_held{1};
  double cost_per_day_minor{0.0};
  bool willing_set{false};
  double earned_minor{0.0};
  bool has_earned{false};
  double payback_progress{0.0};
  bool has_progress{false};
  std::string payback_eta;
  bool paid_back{false};
  std::vector<Milestone> milestones;
};

struct DeviceRow {
  std::string id;
  std::string name;
  std::string icon = "📦";
  std::string category;
  gint64 price_minor{0};
  std::string currency;
  std::string purchase_date;
  std::string created_at;
  gint64 willing_per_day_minor{0};
  bool has_willing{false};
  std::string notes;
  Computed computed;
};

// one currency's aggregate (issue #23); every number is computed in the
// backend, the host only formats
struct CurrencyGroup {
  std::string currency;
  gint64 total_spent_minor{0};
  double earned_total_minor{0.0};
  double avg_cost_per_day_minor{0.0};
};

struct Document {
  std::string currency = "CNY";
  bool update_auto_check = true;
  std::vector<DeviceRow> devices;
  gint64 total_spent_minor{0};
  double earned_total_minor{0.0};
  double avg_cost_per_day_minor{0.0};
  int device_count{0};
  // mixed portfolios: the combined money scalars above are null and the
  // summary tiles render these groups instead (issue #23)
  std::vector<CurrencyGroup> currency_groups;
  bool mixed_currency() const { return currency_groups.size() > 1; }
};

// Parsed `sync-state` payload (issue #9 M1); refreshed by the settings
// dialog and the 5s poll timer.
struct SyncSnapshot {
  bool valid{false};
  bool enabled{false};
  std::string dir;
  std::string machine_id;
  std::string machine_name;
  std::vector<std::pair<std::string, std::string>> machines;  // id, name
  std::string last_merge_at;  // "" when null on the wire
  gint64 revision{0};
  std::vector<std::string> ignored_files;
};

// ------------------------------------------------------------- app state

struct AppState {
  GtkApplication* app{nullptr};
  GtkWindow* window{nullptr};

  GtkLabel* total_spent{nullptr};
  GtkLabel* earned_back{nullptr};
  GtkLabel* overall_daily{nullptr};
  GtkLabel* device_count{nullptr};
  GtkLabel* quip{nullptr};
  GtkLabel* total_spent_label{nullptr};
  GtkLabel* earned_back_label{nullptr};
  GtkLabel* overall_daily_label{nullptr};
  GtkLabel* device_count_label{nullptr};

  GtkRevealer* status_bar{nullptr};
  GtkLabel* status_label{nullptr};
  GtkCssProvider* css_provider{nullptr};

  // split layout (issue #8): draggable divider between sidebar and detail
  GtkPaned* split{nullptr};
  GtkBox* detail_container{nullptr};
  GtkBox* empty_state{nullptr};
  GtkLabel* empty_title{nullptr};
  GtkLabel* empty_hint{nullptr};
  GtkButton* empty_add_button{nullptr};
  GtkComboBoxText* sort_box{nullptr};
  GtkComboBoxText* language_box{nullptr};
  GtkButton* add_button{nullptr};
  GtkButton* edit_button{nullptr};
  GtkButton* delete_button{nullptr};
  GtkMenuButton* help_menu_button{nullptr};
  GMenu* help_menu{nullptr};
  GtkListBox* device_list{nullptr};
  GtkWidget* ctx_popover{nullptr};  // row context menu, closed on list rebuild

  // update download progress; owned here so poll callbacks stay simple
  GtkWindow* update_dialog{nullptr};
  GtkProgressBar* update_bar{nullptr};
  guint poll_source{0};

  // Data & Sync settings dialog (issue #9 M1); widget pointers live only
  // while the dialog is up and are nulled from its destroy handler
  GtkWindow* sync_dialog{nullptr};
  GtkLabel* sync_status_label{nullptr};
  GtkButton* sync_toggle_button{nullptr};
  GtkBox* sync_enabled_box{nullptr};
  GtkLabel* sync_dir_label{nullptr};
  GtkLabel* sync_machines_value{nullptr};
  GtkLabel* sync_last_merge_value{nullptr};
  GtkBox* sync_warning_row{nullptr};
  GtkLabel* sync_warning_label{nullptr};
  GtkEntry* sync_machine_entry{nullptr};
  GtkButton* sync_machine_save{nullptr};
  GtkComboBoxText* currency_box{nullptr};
  bool currency_box_updating{false};

  // sync poll timer: revision bumps when another machine (or this one)
  // rewrote the shards; the first snapshot only primes the baseline
  guint sync_poll_source{0};
  gint64 sync_revision{-1};

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::unique_ptr<rivet_app::API> api;

  std::mutex startup_mutex;
  std::thread startup_thread;
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  std::string startup_error;
  std::atomic<bool> shutting_down{false};

  Document document;
  SyncSnapshot sync;
  std::string selected_id;  // "" falls back to the first device (mac parity)
  int sort_mode{0};  // 0 added, 1 daily cost, 2 payback progress
  int quip_day{1};
  bool auto_check_done{false};
  bool celebration_open{false};
  bool dark_theme{false};  // cairo draw funcs cannot read css classes
};

AppState g_state;

// ------------------------------------------------------------ forward decls

void apply_language();
void load_all_async();
void delete_device_async(std::string const& id);
void render_detail_pane();
void open_device_form(bool update, std::string const& id);
void show_delete_confirm(std::string const& id);
void check_updates(bool force, bool silent);
void run_daily_digest();
void maybe_celebrate();
void open_settings_dialog();
void update_sync_dialog_rows();
void request_sync_state();
void on_sync_machine_entry_changed(GtkEditable* editable, gpointer user_data);
void begin_enable_sync();
void open_sync_folder_chooser();
void configure_sync_async(std::optional<std::string> dir);
void rename_machine_async(std::string const& name);
void save_currency_async(std::string const& code);
void on_currency_changed(GtkComboBox* box, gpointer user_data);

// -------------------------------------------------------------- ui helpers

std::string clean_error(std::string const& raw) {
  // RVT1 failures arrive as "...error: <message>"; keep the message
  std::size_t const at = raw.find("error: ");
  if (at != std::string::npos) return raw.substr(at + 7);
  return raw;
}

void set_status(std::string const& message, bool reveal) {
  gtk_label_set_text(g_state.status_label, message.c_str());
  gtk_revealer_set_reveal_child(g_state.status_bar, reveal ? TRUE : FALSE);
}

void show_error(std::string const& message) {
  set_status(clean_error(message), true);
}

void show_info(std::string const& message) { set_status(message, true); }

void show_success(std::string const& message) { set_status(message, true); }

void replace_all_in(std::string& text, std::string const& from,
                    std::string const& to);

std::string milestone_emoji(std::string const& key) {
  if (key == "days-100") return "🌱";
  if (key == "days-365") return "📅";
  if (key == "days-1000") return "🏆";
  if (key == "cpd-10") return "💸";
  if (key == "cpd-5") return "🌤";
  if (key == "cpd-2") return "🍃";
  if (key == "cpd-1") return "🪶";
  if (key == "cpd-05") return "✨";
  if (key == "paid-back") return "🎉";
  return "🏅";
}

std::string milestone_label(std::string const& key) {
  payback::linux_strings::Strings const s = strings();
  if (key == "days-100") return s.milestone_days100();
  if (key == "days-365") return s.milestone_days365();
  if (key == "days-1000") return s.milestone_days1000();
  if (key == "cpd-10") return s.milestone_cpd10();
  if (key == "cpd-5") return s.milestone_cpd5();
  if (key == "cpd-2") return s.milestone_cpd2();
  if (key == "cpd-1") return s.milestone_cpd1();
  if (key == "cpd-05") return s.milestone_cpd05();
  if (key == "paid-back") return s.milestone_paid_back();
  return key;
}

std::string daily_quip(int day_index) {
  payback::linux_strings::Strings const s = strings();
  std::string const quips[] = {s.quip1(), s.quip2(), s.quip3(),
                               s.quip4(), s.quip5(), s.quip6()};
  int const index = day_index % 6;
  return quips[index < 0 ? index + 6 : index];
}

void apply_quip() {
  if (g_state.document.devices.empty()) {
    gtk_label_set_text(g_state.quip, "");
    return;
  }
  gtk_label_set_text(g_state.quip,
                     ("✦ " + daily_quip(g_state.quip_day)).c_str());
}

void clear_list() {
  GtkWidget* child =
      gtk_widget_get_first_child(GTK_WIDGET(g_state.device_list));
  while (child != nullptr) {
    GtkWidget* next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(g_state.device_list, child);
    child = next;
  }
}

std::vector<DeviceRow> sorted_devices() {
  std::vector<DeviceRow> rows = g_state.document.devices;
  std::sort(rows.begin(), rows.end(),
            [](DeviceRow const& a, DeviceRow const& b) {
              if (g_state.sort_mode == 1) {
                return a.computed.cost_per_day_minor <
                       b.computed.cost_per_day_minor;
              }
              if (g_state.sort_mode == 2) {
                return a.computed.payback_progress >
                       b.computed.payback_progress;
              }
              // "yyyy-MM-dd HH:mm:ss" compares chronologically as text
              return a.created_at > b.created_at;
            });
  return rows;
}

DeviceRow const* find_device(std::string const& id) {
  for (DeviceRow const& row : g_state.document.devices) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

// nil selection (fresh launch, deleted device) falls back to the first
// device so the detail pane is never blank while devices exist — the mac
// split view's `selectedDevice` rule
std::string resolved_selection_id() {
  if (g_state.document.devices.empty()) return std::string();
  if (find_device(g_state.selected_id) != nullptr) return g_state.selected_id;
  return g_state.document.devices.front().id;
}

// cairo cannot read css classes; the draw funcs read this flag instead
GdkRGBA theme_color(char const* light, char const* dark) {
  GdkRGBA color = {0.0, 0.0, 0.0, 1.0};
  gdk_rgba_parse(&color, g_state.dark_theme ? dark : light);
  return color;
}

// -------------------------------------------------------------- css theming

// category whitelist shared by the add/edit form and the sidebar badges; the
// css class is derived from it so data can never inject a rogue class
std::string const kCategories[] = {"computer", "phone",    "tablet",
                                   "watch",    "audio",    "camera",
                                   "gaming",   "appliance", "accessory",
                                   "other"};

std::string category_css_class(std::string const& category) {
  for (std::string const& known : kCategories) {
    if (category == known) return "cat-" + category;
  }
  return "cat-other";
}

// Warm paper in both schemes; the dark palette mirrors the Windows host's
// ThemeDictionaries (docs/design.md tokens). Label foreground stays on the
// GTK theme, so swapping our backgrounds is what keeps dark mode legible —
// a single light stylesheet under a dark GTK theme renders light-on-light.
char const* const kLightCss = R"CSS(
.payback-paper { background: #FAF7F2; }
.payback-card { background: #FFFFFF; border: 1px solid #E8E2D9; border-radius: 12px; padding: 12px 14px; }
.payback-tile { background: #FFFFFF; border: 1px solid #E8E2D9; border-radius: 14px; padding: 10px 14px; }
.payback-earned { color: #38964D; }
.payback-accent { color: #C15F3C; }
.payback-dim { opacity: 0.65; font-size: 12px; }
.payback-meta { opacity: 0.65; font-size: 12px; }
.payback-cost { font-size: 20px; font-weight: bold; }
.payback-cost-label { opacity: 0.65; font-size: 11px; }
.payback-name { font-size: 16px; font-weight: 600; }
.payback-badges { font-size: 12px; }
.payback-quip { opacity: 0.65; font-size: 12px; }
.payback-section { font-weight: bold; opacity: 0.8; }
.payback-detail-name { font-size: 22px; font-weight: 700; }
.payback-tile-value { font-size: 17px; font-weight: bold; }
.payback-mono { font-family: monospace; font-size: 11px; }
.payback-warning { color: #B25E09; }
.payback-empty-emoji { font-size: 40px; }
.payback-empty-title { font-size: 20px; font-weight: bold; }
.payback-capsule { background: rgba(56,150,77,0.14); border-radius: 999px; padding: 6px 12px; }
.payback-capsule-text { color: #38964D; font-weight: bold; }
.payback-card-earn { background: rgba(56,150,77,0.10); }
.payback-hint-card { background: rgba(193,95,60,0.10); border-radius: 12px; padding: 12px 14px; }
.payback-list row { padding: 7px 8px; border-radius: 10px; }
.payback-badge { font-size: 17px; border-radius: 10px; }
.payback-badge-lg { font-size: 28px; border-radius: 14px; }
/* the primary action is the brand accent, never the GTK theme's suggested
   color (docs/design.md: orange = in-progress value; Yaru renders
   suggested-action in the user's accent hue, which violates the palette) */
button.suggested-action { background: #C15F3C; color: #FFFFFF; border: none; }
button.suggested-action:hover { background: #B05536; }
button.suggested-action:active { background: #9E4B30; }
button.suggested-action > label { color: #FFFFFF; }
.cat-computer { background-color: rgba(10,132,255,0.14); }
.cat-phone { background-color: rgba(48,176,199,0.14); }
.cat-tablet { background-color: rgba(94,92,230,0.14); }
.cat-watch { background-color: rgba(0,199,190,0.18); }
.cat-audio { background-color: rgba(191,90,242,0.14); }
.cat-camera { background-color: rgba(255,159,10,0.16); }
.cat-gaming { background-color: rgba(255,69,58,0.13); }
.cat-appliance { background-color: rgba(50,173,230,0.15); }
.cat-accessory { background-color: rgba(0,199,190,0.16); }
.cat-other { background-color: rgba(193,95,60,0.14); }
.payback-bar-accent > trough > progress { background-color: #C15F3C; border-radius: 3px; }
.payback-bar-earn > trough > progress { background-color: #38964D; border-radius: 3px; }
)CSS";

char const* const kDarkCss = R"CSS(
.payback-paper { background: #1F1D1A; }
.payback-card { background: #2B2926; border: 1px solid #3A3835; border-radius: 12px; padding: 12px 14px; }
.payback-tile { background: #2B2926; border: 1px solid #3A3835; border-radius: 14px; padding: 10px 14px; }
.payback-earned { color: #57B26B; }
.payback-accent { color: #D98A66; }
.payback-dim { opacity: 0.65; font-size: 12px; }
.payback-meta { opacity: 0.65; font-size: 12px; }
.payback-cost { font-size: 20px; font-weight: bold; }
.payback-cost-label { opacity: 0.65; font-size: 11px; }
.payback-name { font-size: 16px; font-weight: 600; }
.payback-badges { font-size: 12px; }
.payback-quip { opacity: 0.65; font-size: 12px; }
.payback-section { font-weight: bold; opacity: 0.8; }
.payback-detail-name { font-size: 22px; font-weight: 700; }
.payback-tile-value { font-size: 17px; font-weight: bold; }
.payback-mono { font-family: monospace; font-size: 11px; }
.payback-warning { color: #E8A33D; }
.payback-empty-emoji { font-size: 40px; }
.payback-empty-title { font-size: 20px; font-weight: bold; }
.payback-capsule { background: rgba(87,178,107,0.16); border-radius: 999px; padding: 6px 12px; }
.payback-capsule-text { color: #57B26B; font-weight: bold; }
.payback-card-earn { background: rgba(87,178,107,0.12); }
.payback-hint-card { background: rgba(217,138,102,0.12); border-radius: 12px; padding: 12px 14px; }
.payback-list row { padding: 7px 8px; border-radius: 10px; }
.payback-badge { font-size: 17px; border-radius: 10px; }
.payback-badge-lg { font-size: 28px; border-radius: 14px; }
button.suggested-action { background: #D98A66; color: #1F1D1A; border: none; }
button.suggested-action:hover { background: #E29A78; }
button.suggested-action:active { background: #C97B5B; }
button.suggested-action > label { color: #1F1D1A; }
.cat-computer { background-color: rgba(10,132,255,0.20); }
.cat-phone { background-color: rgba(48,176,199,0.20); }
.cat-tablet { background-color: rgba(94,92,230,0.20); }
.cat-watch { background-color: rgba(0,199,190,0.24); }
.cat-audio { background-color: rgba(191,90,242,0.20); }
.cat-camera { background-color: rgba(255,159,10,0.22); }
.cat-gaming { background-color: rgba(255,69,58,0.19); }
.cat-appliance { background-color: rgba(50,173,230,0.21); }
.cat-accessory { background-color: rgba(0,199,190,0.22); }
.cat-other { background-color: rgba(217,138,102,0.18); }
.payback-bar-accent > trough > progress { background-color: #D98A66; border-radius: 3px; }
.payback-bar-earn > trough > progress { background-color: #57B26B; border-radius: 3px; }
)CSS";

// same heuristic as the prefer-dark setup: a theme whose name contains
// "Dark"/"dark" is a dark scheme
bool system_dark_theme() {
  GtkSettings* settings = gtk_settings_get_default();
  gchar* theme_name = nullptr;
  g_object_get(settings, "gtk-theme-name", &theme_name, nullptr);
  bool const dark = theme_name != nullptr &&
                    std::string(theme_name).find("ark") != std::string::npos;
  g_free(theme_name);
  return dark;
}

void load_css_variant() {
  bool const dark = system_dark_theme();
  g_state.dark_theme = dark;
  gtk_css_provider_load_from_string(
      g_state.css_provider, dark ? kDarkCss : kLightCss);
  // follow the system's dark preference for the stock widgets too
  g_object_set(gtk_settings_get_default(),
               "gtk-application-prefer-dark-theme", dark ? TRUE : FALSE,
               nullptr);
}

void on_theme_changed(GObject*, GParamSpec*, gpointer) { load_css_variant(); }

void apply_css() {
  auto* provider = gtk_css_provider_new();
  g_state.css_provider = provider;
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  load_css_variant();
  // hot-swap when the user flips the system theme while we run
  g_signal_connect(gtk_settings_get_default(), "notify::gtk-theme-name",
                   G_CALLBACK(on_theme_changed), nullptr);
}

// -------------------------------------------------------------- rendering

// Mixed portfolios never show one combined money value: the backend nulls
// the combined scalars and the tiles join the per-currency groups (#23).
template <typename Format>
std::string joined_group_money(Document const& d, Format format) {
  std::string joined;
  for (CurrencyGroup const& group : d.currency_groups) {
    if (!joined.empty()) joined += " · ";
    joined += format(group);
  }
  return joined;
}

void render_summary() {
  Document const& d = g_state.document;
  gtk_label_set_text(
      g_state.total_spent,
      (d.mixed_currency()
           ? joined_group_money(
                 d, [](CurrencyGroup const& g) {
                   return money(static_cast<double>(g.total_spent_minor),
                                g.currency);
                 })
           : money(static_cast<double>(d.total_spent_minor), d.currency))
          .c_str());
  gtk_label_set_text(
      g_state.earned_back,
      (d.mixed_currency()
           ? joined_group_money(d, [](CurrencyGroup const& g) {
               return money(g.earned_total_minor, g.currency);
             })
           : money(d.earned_total_minor, d.currency))
          .c_str());
  gtk_label_set_text(
      g_state.overall_daily,
      (d.mixed_currency()
           ? joined_group_money(d, [](CurrencyGroup const& g) {
               return money(g.avg_cost_per_day_minor, g.currency);
             })
           : money(d.avg_cost_per_day_minor, d.currency))
          .c_str());
  gtk_label_set_text(g_state.device_count,
                     std::to_string(d.device_count).c_str());
}

// Rounded icon badge whose backdrop hue follows the device category —
// same mapping as the mac DeviceIconBadge (alpha tints read on both paper
// palettes, so one table serves light and dark).
GtkWidget* category_badge(std::string const& icon, std::string const& category,
                          bool large) {
  auto* badge = gtk_label_new(icon.c_str());
  int const size = large ? 54 : 34;
  gtk_widget_set_size_request(badge, size, size);
  gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(badge, large ? "payback-badge-lg" : "payback-badge");
  gtk_widget_add_css_class(badge, category_css_class(category).c_str());
  return badge;
}

// last `limit` achieved milestones as an emoji run (mac suffix(2) rule)
std::string achieved_emoji_suffix(DeviceRow const& row, std::size_t limit) {
  std::vector<std::string> emojis;
  for (Milestone const& milestone : row.computed.milestones) {
    if (milestone.achieved) emojis.push_back(milestone_emoji(milestone.key));
  }
  std::string out;
  std::size_t const start =
      emojis.size() > limit ? emojis.size() - limit : 0;
  for (std::size_t i = start; i < emojis.size(); ++i) out += emojis[i];
  return out;
}

// ------------------------------------------------------------ cairo drawing

constexpr double kPi = 3.14159265358979323846;

struct RingSpec {
  double progress{0.0};
  bool paid_back{false};
  double line_width{3.0};
  double font_size{7.0};
};

// accent ring with a track, drawn from 12 o'clock clockwise; the center
// carries the percent (or a check once paid back) like mac PaybackRingView
void draw_ring(GtkDrawingArea*, cairo_t* cr, int width, int height,
               gpointer data) {
  auto const* spec = static_cast<RingSpec const*>(data);
  double const cx = width / 2.0;
  double const cy = height / 2.0;
  double const radius =
      std::min(width, height) / 2.0 - spec->line_width / 2.0 - 0.5;
  if (radius <= 0) return;
  double const progress = std::clamp(spec->progress, 0.0, 1.0);

  GdkRGBA const track = theme_color("#3C3C43", "#EBEBF5");
  cairo_set_source_rgba(cr, track.red, track.green, track.blue, 0.15);
  cairo_set_line_width(cr, spec->line_width);
  cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * kPi);
  cairo_stroke(cr);

  if (progress > 0.0005) {
    GdkRGBA const tone = spec->paid_back
                             ? theme_color("#38964D", "#57B26B")
                             : theme_color("#C15F3C", "#D98A66");
    cairo_set_source_rgba(cr, tone.red, tone.green, tone.blue, 1.0);
    cairo_set_line_width(cr, spec->line_width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    // GTK's cairo y axis points down, so 12 o'clock is -pi/2 and progress
    // sweeps clockwise — the same direction the mac ring animates
    cairo_arc(cr, cx, cy, radius, -kPi / 2.0,
              -kPi / 2.0 + progress * 2.0 * kPi);
    cairo_stroke(cr);
  }

  GdkRGBA const ink = theme_color("#3A3630", "#E8E4DD");
  cairo_set_source_rgba(cr, ink.red, ink.green, ink.blue, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  std::string const center = spec->paid_back
                                 ? std::string("✓")
                                 : std::to_string(static_cast<int>(
                                       progress * 100.0 + 0.5));
  cairo_set_font_size(cr, spec->paid_back ? spec->font_size * 1.2
                                          : spec->font_size);
  cairo_text_extents_t extents;
  cairo_text_extents(cr, center.c_str(), &extents);
  cairo_move_to(cr, cx - extents.width / 2.0 - extents.x_bearing,
                cy - extents.height / 2.0 - extents.y_bearing);
  cairo_show_text(cr, center.c_str());
}

struct CurveSpec {
  gint64 price_minor{0};
  int days_held{0};
};

// price/day for every day since purchase: day 1 carries the whole price at
// the top edge and the line only ever falls (docs/design.md: 永不上扬) —
// gradient stroke + soft fill below, mirroring mac CostCurve
void draw_cost_curve(GtkDrawingArea*, cairo_t* cr, int width, int height,
                     gpointer data) {
  auto const* spec = static_cast<CurveSpec const*>(data);
  if (spec->days_held <= 1 || spec->price_minor <= 0) return;

  int const max_days = std::max(spec->days_held, 30);
  double const max_value = static_cast<double>(spec->price_minor);
  double const pad_x = 2.0;
  double const pad_y = 4.0;
  double const plot_w = std::max(static_cast<double>(width) - 2 * pad_x, 1.0);
  double const plot_h = std::max(static_cast<double>(height) - 2 * pad_y, 1.0);
  auto point_at = [&](int day) {
    double const x = pad_x + static_cast<double>(day - 1) /
                               static_cast<double>(max_days - 1) * plot_w;
    double const value =
        static_cast<double>(spec->price_minor) / static_cast<double>(day);
    double const y = pad_y + (1.0 - value / max_value) * plot_h;
    return std::pair<double, double>{std::min(x, static_cast<double>(width)),
                                     y};
  };

  auto const first = point_at(1);
  auto const last = point_at(spec->days_held);
  GdkRGBA const accent = theme_color("#C15F3C", "#D98A66");
  auto set_accent = [&](double alpha) {
    cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, alpha);
  };

  // fill: curve down to the bottom edge, fading toward the paper
  cairo_move_to(cr, first.first, first.second);
  for (int day = 2; day <= spec->days_held; ++day) {
    auto const point = point_at(day);
    cairo_line_to(cr, point.first, point.second);
  }
  cairo_line_to(cr, last.first, static_cast<double>(height));
  cairo_line_to(cr, first.first, static_cast<double>(height));
  cairo_close_path(cr);
  cairo_pattern_t* fill = cairo_pattern_create_linear(0.0, 0.0, 0.0, height);
  cairo_pattern_add_color_stop_rgba(fill, 0.0, accent.red, accent.green,
                                    accent.blue, 0.22);
  cairo_pattern_add_color_stop_rgba(fill, 1.0, accent.red, accent.green,
                                    accent.blue, 0.02);
  cairo_set_source(cr, fill);
  cairo_fill(cr);
  cairo_pattern_destroy(fill);

  // stroke: horizontal fade from a quiet accent into the full brand orange
  cairo_move_to(cr, first.first, first.second);
  for (int day = 2; day <= spec->days_held; ++day) {
    auto const point = point_at(day);
    cairo_line_to(cr, point.first, point.second);
  }
  cairo_pattern_t* stroke = cairo_pattern_create_linear(0.0, 0.0, width, 0.0);
  cairo_pattern_add_color_stop_rgba(stroke, 0.0, accent.red, accent.green,
                                    accent.blue, 0.5);
  cairo_pattern_add_color_stop_rgba(stroke, 1.0, accent.red, accent.green,
                                    accent.blue, 1.0);
  cairo_set_source(cr, stroke);
  cairo_set_line_width(cr, 2.5);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);
  cairo_pattern_destroy(stroke);

  // endpoint dot: today's position on the curve
  cairo_arc(cr, last.first, last.second, 3.5, 0.0, 2.0 * kPi);
  set_accent(1.0);
  cairo_fill(cr);
}

// --------------------------------------------------------- detail pane

GtkWidget* detail_label(char const* text, char const* css_class) {
  auto* label = gtk_label_new(text);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  if (css_class != nullptr) gtk_widget_add_css_class(label, css_class);
  return label;
}

GtkWidget* stat_tile(char const* label_text, std::string const& value,
                     bool earned_tint) {
  auto* tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_add_css_class(tile, "payback-tile");
  gtk_widget_set_hexpand(tile, TRUE);
  auto* label = gtk_label_new(label_text);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_add_css_class(label, "payback-dim");
  gtk_box_append(GTK_BOX(tile), label);
  auto* value_label = gtk_label_new(value.c_str());
  gtk_widget_set_halign(value_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(value_label, "payback-tile-value");
  if (earned_tint) gtk_widget_add_css_class(value_label, "payback-earned");
  gtk_box_append(GTK_BOX(tile), value_label);
  return tile;
}

void render_detail_pane() {
  if (g_state.detail_container == nullptr) return;
  GtkWidget* child =
      gtk_widget_get_first_child(GTK_WIDGET(g_state.detail_container));
  while (child != nullptr) {
    GtkWidget* next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(g_state.detail_container, child);
    child = next;
  }

  std::string const id = resolved_selection_id();
  DeviceRow const* found = id.empty() ? nullptr : find_device(id);
  if (found == nullptr) return;
  DeviceRow const row = *found;  // copy: the document may reload underneath
  payback::linux_strings::Strings const s = strings();

  // header: badge + name + price · date + 🎉 paid-back capsule
  auto* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_box_append(GTK_BOX(header),
                 category_badge(row.icon, row.category, true));
  auto* title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  auto* name = gtk_label_new(row.name.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(name, GTK_ALIGN_START);
  gtk_widget_add_css_class(name, "payback-detail-name");
  gtk_box_append(GTK_BOX(title_box), name);
  std::string const facts =
      money(static_cast<double>(row.price_minor), row.currency) + " · " +
      row.purchase_date;
  gtk_box_append(GTK_BOX(title_box), detail_label(facts.c_str(), "payback-meta"));
  gtk_box_append(GTK_BOX(header), title_box);
  auto* spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spring, TRUE);
  gtk_box_append(GTK_BOX(header), spring);
  if (row.computed.paid_back) {
    auto* capsule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(capsule, "payback-capsule");
    gtk_widget_set_valign(capsule, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(capsule), gtk_label_new("🎉"));
    gtk_box_append(GTK_BOX(capsule),
                   detail_label(s.paid_back().c_str(), "payback-capsule-text"));
    gtk_box_append(GTK_BOX(header), capsule);
  }
  gtk_box_append(g_state.detail_container, header);

  // stat tiles: daily cost, held-for, and (with a willing price) per-day worth
  auto* tiles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_append(
      GTK_BOX(tiles),
      stat_tile(s.daily_cost().c_str(),
                money(row.computed.cost_per_day_minor, row.currency) + " " +
                    s.per_day(),
                row.computed.paid_back));
  gtk_box_append(GTK_BOX(tiles),
                 stat_tile(s.held_for().c_str(),
                           std::to_string(row.computed.days_held) + " " +
                               s.days(),
                           false));
  if (row.computed.willing_set) {
    gtk_box_append(
        GTK_BOX(tiles),
        stat_tile(s.willing_per_day().c_str(),
                  money(static_cast<double>(row.willing_per_day_minor),
                        row.currency),
                  false));
  }
  gtk_box_append(g_state.detail_container, tiles);

  // cost curve card: the hero chart, height-locked like the mac 64pt canvas
  auto* curve_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(curve_card, "payback-card");
  auto* curve_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(curve_header),
                 detail_label(s.daily_cost().c_str(), "payback-dim"));
  auto* curve_spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(curve_spring, TRUE);
  gtk_box_append(GTK_BOX(curve_header), curve_spring);
  std::string const today_cost =
      money(static_cast<double>(row.price_minor) /
                static_cast<double>(std::max<gint64>(row.computed.days_held, 1)),
            row.currency) +
      " " + s.per_day();
  auto* today_label = gtk_label_new(today_cost.c_str());
  gtk_widget_set_halign(today_label, GTK_ALIGN_END);
  gtk_widget_add_css_class(today_label, "payback-accent");
  gtk_widget_add_css_class(today_label, "payback-tile-value");
  gtk_box_append(GTK_BOX(curve_header), today_label);
  gtk_box_append(GTK_BOX(curve_card), curve_header);

  auto* curve = gtk_drawing_area_new();
  gtk_widget_set_size_request(curve, -1, 64);
  gtk_widget_set_hexpand(curve, TRUE);
  auto* curve_spec = g_new0(CurveSpec, 1);
  curve_spec->price_minor = row.price_minor;
  curve_spec->days_held = static_cast<int>(row.computed.days_held);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(curve), draw_cost_curve,
                                 curve_spec, g_free);
  gtk_box_append(GTK_BOX(curve_card), curve);

  auto* axis = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  std::string day_one = s.curve_day_label();
  replace_all_in(day_one, "{day}", "1");
  gtk_box_append(GTK_BOX(axis), detail_label(day_one.c_str(), "payback-dim"));
  auto* axis_spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(axis_spring, TRUE);
  gtk_box_append(GTK_BOX(axis), axis_spring);
  std::string day_now = s.curve_day_label();
  replace_all_in(day_now, "{day}", std::to_string(row.computed.days_held));
  auto* day_now_label = gtk_label_new(day_now.c_str());
  gtk_widget_set_halign(day_now_label, GTK_ALIGN_END);
  gtk_widget_add_css_class(day_now_label, "payback-dim");
  gtk_box_append(GTK_BOX(axis), day_now_label);
  gtk_box_append(GTK_BOX(curve_card), axis);
  gtk_box_append(g_state.detail_container, curve_card);

  // payback progress card: ring + bar + ETA, or the willing-price hint
  if (row.computed.willing_set) {
    auto* payback_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(payback_card, "payback-card");
    if (row.computed.paid_back) {
      gtk_widget_add_css_class(payback_card, "payback-card-earn");
    }
    auto* progress_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    auto* ring = gtk_drawing_area_new();
    gtk_widget_set_size_request(ring, 38, 38);
    auto* ring_spec = g_new0(RingSpec, 1);
    ring_spec->progress = row.computed.payback_progress;
    ring_spec->paid_back = row.computed.paid_back;
    ring_spec->line_width = 5.0;
    ring_spec->font_size = 10.5;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ring), draw_ring,
                                   ring_spec, g_free);
    gtk_box_append(GTK_BOX(progress_header), ring);
    gtk_box_append(GTK_BOX(progress_header),
                   detail_label(s.payback_progress().c_str(), "payback-name"));
    auto* progress_spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(progress_spring, TRUE);
    gtk_box_append(GTK_BOX(progress_header), progress_spring);
    std::string const headline =
        row.computed.paid_back
            ? s.paid_back()
            : std::to_string(static_cast<int>(
                  row.computed.payback_progress * 100.0 + 0.5)) + "%";
    auto* headline_label = gtk_label_new(headline.c_str());
    gtk_widget_set_halign(headline_label, GTK_ALIGN_END);
    gtk_widget_add_css_class(headline_label, "payback-tile-value");
    gtk_widget_add_css_class(headline_label, row.computed.paid_back
                                                 ? "payback-earned"
                                                 : "payback-accent");
    gtk_box_append(GTK_BOX(progress_header), headline_label);
    gtk_box_append(GTK_BOX(payback_card), progress_header);

    auto* bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(bar),
        std::clamp(row.computed.payback_progress, 0.0, 1.0));
    gtk_widget_add_css_class(bar, row.computed.paid_back ? "payback-bar-earn"
                                                         : "payback-bar-accent");
    gtk_box_append(GTK_BOX(payback_card), bar);

    std::string note;
    if (row.computed.paid_back) {
      if (row.computed.has_earned) {
        note = s.earned_so_far() + " " +
               money(row.computed.earned_minor, row.currency);
      }
    } else if (!row.computed.payback_eta.empty()) {
      note = s.payback_eta() + ": " + row.computed.payback_eta;
    }
    if (!note.empty()) {
      gtk_box_append(GTK_BOX(payback_card),
                     detail_label(note.c_str(), "payback-meta"));
    }
    gtk_box_append(g_state.detail_container, payback_card);
  } else {
    auto* hint_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(hint_card, "payback-hint-card");
    gtk_box_append(GTK_BOX(hint_card), gtk_label_new("💡"));
    gtk_box_append(GTK_BOX(hint_card),
                   detail_label(s.willing_hint().c_str(), "payback-meta"));
    gtk_box_append(g_state.detail_container, hint_card);
  }

  // milestone ladder: every rung with its emoji, dimmed until reached
  auto* ladder_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(ladder_card, "payback-card");
  gtk_box_append(GTK_BOX(ladder_card),
                 detail_label(s.milestones().c_str(), "payback-section"));
  for (Milestone const& milestone : row.computed.milestones) {
    auto* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    auto* emoji = gtk_label_new(milestone_emoji(milestone.key).c_str());
    if (!milestone.achieved) gtk_widget_set_opacity(emoji, 0.3);
    gtk_box_append(GTK_BOX(line), emoji);
    gtk_box_append(GTK_BOX(line),
                   detail_label(milestone_label(milestone.key).c_str(),
                                nullptr));
    auto* line_spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(line_spring, TRUE);
    gtk_box_append(GTK_BOX(line), line_spring);
    if (milestone.achieved) {
      auto* check = gtk_label_new("✓");
      gtk_widget_add_css_class(check, "payback-earned");
      gtk_box_append(GTK_BOX(line), check);
    }
    gtk_box_append(GTK_BOX(ladder_card), line);
  }
  gtk_box_append(g_state.detail_container, ladder_card);
}

// ---------------------------------------------------------- sidebar list

// the context popover is parented to a list row, which the next rebuild
// destroys — close it deterministically instead of leaving a dangling child
void close_ctx_popover() {
  if (g_state.ctx_popover == nullptr) return;
  GtkWidget* popover = g_state.ctx_popover;
  g_state.ctx_popover = nullptr;
  gtk_widget_unparent(popover);  // drops the parent's ref; a late "closed"
                                 // is a no-op thanks to the parent check
}

void update_action_sensitivity() {
  bool const have = !g_state.document.devices.empty();
  if (g_state.edit_button != nullptr) {
    gtk_widget_set_sensitive(GTK_WIDGET(g_state.edit_button), have);
  }
  if (g_state.delete_button != nullptr) {
    gtk_widget_set_sensitive(GTK_WIDGET(g_state.delete_button), have);
  }
}

void on_row_right_clicked(GtkGestureClick*, int, double x, double y,
                          gpointer user_data) {
  auto* card = GTK_WIDGET(user_data);
  auto* row = gtk_widget_get_ancestor(card, GTK_TYPE_LIST_BOX_ROW);
  if (row == nullptr) return;
  auto const* id = static_cast<char const*>(
      g_object_get_data(G_OBJECT(row), "device-id"));
  if (id == nullptr) return;  // the empty-state hint row

  // acting on a row also focuses it: detail pane and menu stay in sync
  gtk_list_box_select_row(g_state.device_list, GTK_LIST_BOX_ROW(row));

  payback::linux_strings::Strings const s = strings();
  auto* menu = g_menu_new();
  auto* edit_item = g_menu_item_new(s.edit().c_str(), nullptr);
  g_menu_item_set_action_and_target(edit_item, "win.ctx-edit", "s", id);
  g_menu_append_item(menu, edit_item);
  g_object_unref(edit_item);
  // delete rides a custom child so it can wear the destructive style;
  // menu-model items cannot carry css classes
  auto* delete_item = g_menu_item_new(nullptr, nullptr);
  g_menu_item_set_attribute(delete_item, "custom", "s", "payback-delete");
  g_menu_append_item(menu, delete_item);
  g_object_unref(delete_item);

  close_ctx_popover();
  auto* popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  auto* delete_button = gtk_button_new_with_label(s.remove().c_str());
  gtk_widget_add_css_class(delete_button, "flat");
  gtk_widget_add_css_class(delete_button, "destructive-action");
  g_object_set_data_full(G_OBJECT(delete_button), "device-id",
                         g_strdup(id), g_free);
  g_signal_connect(
      delete_button, "clicked",
      G_CALLBACK(+[](GtkButton* button, gpointer) {
        auto const* row_id = static_cast<char const*>(
            g_object_get_data(G_OBJECT(button), "device-id"));
        if (g_state.window != nullptr && row_id != nullptr) {
          g_action_group_activate_action(G_ACTION_GROUP(g_state.window),
                                         "ctx-delete",
                                         g_variant_new_string(row_id));
        }
      }),
      nullptr);
  gtk_popover_menu_add_child(GTK_POPOVER_MENU(popover), delete_button,
                             "payback-delete");
  gtk_widget_set_parent(popover, card);
  g_state.ctx_popover = popover;
  GdkRectangle rect = {static_cast<int>(x), static_cast<int>(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
  g_signal_connect(popover, "closed",
                   G_CALLBACK(+[](GtkPopover* popover, gpointer) {
                     g_state.ctx_popover = nullptr;
                     // close_ctx_popover may have unparented first; a
                     // second unparent would warn (or worse)
                     if (gtk_widget_get_parent(GTK_WIDGET(popover)) !=
                         nullptr) {
                       gtk_widget_unparent(GTK_WIDGET(popover));
                     }
                   }),
                   nullptr);
  gtk_popover_popup(GTK_POPOVER(popover));
}

GtkWidget* device_row(DeviceRow const& row) {
  payback::linux_strings::Strings const s = strings();

  // light identity strip like mac SidebarDeviceRow: badge + name + per-day
  // cost, then the payback ring (or the newest milestone emojis)
  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(box, 2);
  gtk_widget_set_margin_bottom(box, 2);
  gtk_box_append(GTK_BOX(box), category_badge(row.icon, row.category, false));

  auto* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
  auto* name = gtk_label_new(row.name.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(name, GTK_ALIGN_START);
  gtk_widget_add_css_class(name, "payback-name");
  gtk_box_append(GTK_BOX(left), name);

  std::string const subtitle =
      money(row.computed.cost_per_day_minor, row.currency) + " " + s.per_day();
  gtk_box_append(GTK_BOX(left),
                 detail_label(subtitle.c_str(), "payback-meta"));
  gtk_box_append(GTK_BOX(box), left);

  auto* right_spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(right_spring, TRUE);
  gtk_box_append(GTK_BOX(box), right_spring);

  if (row.computed.willing_set) {
    auto* ring = gtk_drawing_area_new();
    gtk_widget_set_size_request(ring, 26, 26);
    gtk_widget_set_valign(ring, GTK_ALIGN_CENTER);
    auto* spec = g_new0(RingSpec, 1);
    spec->progress = row.computed.payback_progress;
    spec->paid_back = row.computed.paid_back;
    spec->line_width = 3.0;
    spec->font_size = 7.0;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ring), draw_ring, spec,
                                   g_free);
    gtk_box_append(GTK_BOX(box), ring);
  } else {
    std::string const emojis = achieved_emoji_suffix(row, 2);
    if (!emojis.empty()) {
      gtk_box_append(GTK_BOX(box),
                     detail_label(emojis.c_str(), "payback-badges"));
    }
  }

  // right-click opens Edit/Delete; single-button mice get the headerbar
  auto* gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                GDK_BUTTON_SECONDARY);
  g_signal_connect(gesture, "pressed", G_CALLBACK(on_row_right_clicked), box);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(gesture));

  return box;
}

void render_device_list() {
  close_ctx_popover();
  clear_list();
  payback::linux_strings::Strings const s = strings();
  std::vector<DeviceRow> const rows = sorted_devices();
  if (rows.empty()) {
    auto* hint = gtk_label_new(s.empty_hint().c_str());
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hint, 24);
    gtk_widget_add_css_class(hint, "payback-dim");
    auto* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hint);
    gtk_list_box_append(g_state.device_list, row);
    gtk_widget_set_sensitive(row, FALSE);
    return;
  }
  for (DeviceRow const& row_data : rows) {
    GtkWidget* card = device_row(row_data);
    auto* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    g_object_set_data_full(G_OBJECT(row), "device-id",
                           g_strdup(row_data.id.c_str()), g_free);
    gtk_list_box_append(g_state.device_list, row);
  }

  // restore the resolved selection so the detail pane keeps its subject
  // after every rebuild (sort switch, document reload, delete)
  std::string const selected = resolved_selection_id();
  GtkListBoxRow* target = nullptr;
  for (guint i = 0;; ++i) {
    GtkListBoxRow* row =
        gtk_list_box_get_row_at_index(g_state.device_list, static_cast<gint>(i));
    if (row == nullptr) break;
    auto const* id = static_cast<char const*>(
        g_object_get_data(G_OBJECT(row), "device-id"));
    if (id != nullptr && selected == id) target = row;
  }
  gtk_list_box_select_row(g_state.device_list, target);
}

void render_document_from_cache() {
  bool const empty = g_state.document.devices.empty();
  if (g_state.split != nullptr) {
    gtk_widget_set_visible(GTK_WIDGET(g_state.split), empty ? FALSE : TRUE);
  }
  if (g_state.empty_state != nullptr) {
    gtk_widget_set_visible(GTK_WIDGET(g_state.empty_state),
                           empty ? TRUE : FALSE);
  }
  render_summary();
  render_device_list();
  apply_quip();
  update_action_sensitivity();
  render_detail_pane();
}

// --------------------------------------------------------------- flows

void load_all_async() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->load_all_async(
      [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) {
                show_error(r.error);
                return;
              }
              JsonDoc doc(bytes_to_string(r.value));
              JsonObject* root = doc.root();
              if (root == nullptr) {
                show_error("malformed load-all document");
                return;
              }
              Document& d = g_state.document;
              if (JsonObject* settings = jobj(root, "settings")) {
                d.currency = jstr(settings, "currency", "CNY");
                d.update_auto_check = jbool(settings, "updateAutoCheck", true);
              }
              if (JsonObject* summary = jobj(root, "summary")) {
                d.total_spent_minor = jint(summary, "totalSpentMinor");
                d.earned_total_minor = jdouble(summary, "earnedTotalMinor");
                d.avg_cost_per_day_minor =
                    jdouble(summary, "avgCostPerDayMinor");
                d.device_count =
                    static_cast<int>(jint(summary, "deviceCount"));
                d.currency_groups.clear();
                JsonArray* groups = jarray(summary, "currencyGroups");
                guint const group_count = jlen(groups);
                for (guint i = 0; i < group_count; ++i) {
                  JsonObject* g = json_array_get_object_element(groups, i);
                  if (g == nullptr) continue;
                  d.currency_groups.push_back(CurrencyGroup{
                      jstr(g, "currency"), jint(g, "totalSpentMinor"),
                      jdouble(g, "earnedTotalMinor"),
                      jdouble(g, "avgCostPerDayMinor")});
                }
              }
              d.devices.clear();
              JsonArray* devices = jarray(root, "devices");
              guint const count = jlen(devices);
              for (guint i = 0; i < count; ++i) {
                JsonObject* item = jat(devices, i);
                if (item == nullptr) continue;
                DeviceRow row;
                row.id = jstr(item, "id");
                row.name = jstr(item, "name");
                row.icon = jstr(item, "icon", "📦");
                row.category = jstr(item, "category");
                row.price_minor = jint(item, "priceMinor");
                row.currency = jstr(item, "currency", d.currency);
                row.purchase_date = jstr(item, "purchaseDate");
                row.created_at = jstr(item, "createdAt");
                row.has_willing = jhas(item, "willingPerDayMinor");
                row.willing_per_day_minor = jint(item, "willingPerDayMinor");
                row.notes = jstr(item, "notes");
                if (JsonObject* computed = jobj(item, "computed")) {
                  row.computed.days_held = jint(computed, "daysHeld", 1);
                  row.computed.cost_per_day_minor =
                      jdouble(computed, "costPerDayMinor");
                  row.computed.willing_set =
                      jbool(computed, "willingSet");
                  row.computed.has_earned = jhas(computed, "earnedMinor");
                  row.computed.earned_minor = jdouble(computed, "earnedMinor");
                  row.computed.has_progress =
                      jhas(computed, "paybackProgress");
                  row.computed.payback_progress =
                      jdouble(computed, "paybackProgress");
                  row.computed.payback_eta = jstr(computed, "paybackEta");
                  row.computed.paid_back = jbool(computed, "paidBack");
                  JsonArray* milestones = jarray(computed, "milestones");
                  guint const mcount = jlen(milestones);
                  for (guint m = 0; m < mcount; ++m) {
                    JsonObject* entry = jat(milestones, m);
                    if (entry == nullptr) continue;
                    Milestone milestone;
                    milestone.key = jstr(entry, "key");
                    milestone.achieved = jbool(entry, "achieved");
                    milestone.is_new = jbool(entry, "new");
                    row.computed.milestones.push_back(milestone);
                  }
                }
                d.devices.push_back(row);
              }

              render_document_from_cache();
              maybe_celebrate();

              if (!g_state.auto_check_done) {
                g_state.auto_check_done = true;
                run_daily_digest();
                if (d.update_auto_check) {
                  check_updates(false, true);
                }
              }
            },
            std::move(unpacked));
      });
}

// --------------------------------------------------------- dialogs: delete

void show_delete_confirm(std::string const& id) {
  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_dialog_new_with_buttons(
      s.delete_confirm_title().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.cancel().c_str(), GTK_RESPONSE_CANCEL,
      s.remove().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* text = gtk_label_new(s.delete_confirm_text().c_str());
  gtk_widget_set_margin_top(text, 8);
  gtk_widget_set_margin_bottom(text, 8);
  gtk_widget_set_margin_start(text, 16);
  gtk_widget_set_margin_end(text, 16);
  gtk_box_append(GTK_BOX(area), text);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);

  auto* id_copy = new std::string(id);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer user_data) {
        std::unique_ptr<std::string> id(static_cast<std::string*>(user_data));
        bool const confirmed = response == GTK_RESPONSE_ACCEPT;
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (confirmed) {
          pb::post_to_main<std::string>(
              [](std::string& p) { delete_device_async(p); }, *id);
        }
      }),
      id_copy);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ---------------------------------------------------------- dialogs: form

struct FormContext {
  GtkEntry* name{nullptr};
  GtkEntry* icon{nullptr};
  GtkComboBoxText* category{nullptr};
  GtkEntry* price{nullptr};
  GtkEntry* date_entry{nullptr};
  GtkSwitch* willing_on{nullptr};
  GtkEntry* willing{nullptr};
  GtkEntry* notes{nullptr};
  GtkLabel* price_error{nullptr};
  GtkLabel* date_error{nullptr};
  GtkLabel* willing_error{nullptr};
  GtkWidget* save_button{nullptr};
  // save lifecycle (issue #24): the dialog stays open until persistence
  // is confirmed; failures land in form_error and Save acts as Retry
  GtkLabel* form_error{nullptr};
  GtkLabel* form_status{nullptr};
  bool saving{false};
  std::string currency;
  bool update{false};
  std::string id;
  // values captured when the dialog closes — the widgets die with it, so
  // the payload is built from these, never from the (dangling) pointers
  std::string name_text;
  std::string icon_text;
  std::string price_text;
  std::string date_text;
  std::string willing_text;
  std::string notes_text;
  int category_index{-1};
  bool willing_active{false};
};

GtkWidget* form_row(GtkWidget* into, char const* label, GtkWidget* widget) {
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  auto* header = gtk_label_new(label);
  gtk_widget_set_halign(header, GTK_ALIGN_START);
  gtk_widget_add_css_class(header, "payback-dim");
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), widget);
  gtk_box_append(GTK_BOX(into), box);
  return widget;
}

void on_calendar_day_selected(GtkCalendar* calendar, gpointer user_data) {
  auto* entry = GTK_ENTRY(user_data);
  GDateTime* date = gtk_calendar_get_date(calendar);
  std::string const text = g_date_time_format(date, "%Y-%m-%d");
  g_date_time_unref(date);
  gtk_editable_set_text(GTK_EDITABLE(entry), text.c_str());
}

std::string entry_text(GtkEntry* entry) {
  return entry == nullptr
             ? std::string()
             : std::string(gtk_editable_get_text(GTK_EDITABLE(entry)));
}

bool form_willing_bad(FormContext const& c) {
  return c.willing_on != nullptr && gtk_switch_get_active(c.willing_on) &&
         !entry_text(c.willing).empty() &&
         parse_amount_minor(entry_text(c.willing)) <= 0;
}

bool form_valid(FormContext const& c) {
  return parse_amount_minor(entry_text(c.price)) > 0 &&
         !normalize_date(entry_text(c.date_entry)).empty() &&
         !form_willing_bad(c);
}

// live validation: the hints track their fields and Save reflects them
void refresh_form_validity(FormContext* c) {
  gtk_widget_set_visible(
      GTK_WIDGET(c->price_error),
      parse_amount_minor(entry_text(c->price)) <= 0);
  gtk_widget_set_visible(GTK_WIDGET(c->date_error),
                         normalize_date(entry_text(c->date_entry)).empty());
  gtk_widget_set_visible(GTK_WIDGET(c->willing_error),
                         form_willing_bad(*c));
  if (c->save_button != nullptr) {
    gtk_widget_set_sensitive(c->save_button, form_valid(*c) && !c->saving);
  }
}

void on_form_field_changed(GtkEditable*, gpointer user_data) {
  refresh_form_validity(static_cast<FormContext*>(user_data));
}

void on_form_toggled(GObject*, GParamSpec*, gpointer user_data) {
  refresh_form_validity(static_cast<FormContext*>(user_data));
}

// Fires the add/update RPC on behalf of an open form dialog. Success
// closes the dialog exactly once and reloads; failure keeps the dialog
// (and every typed field) open for an in-place retry (#24). Both `dialog`
// and `ctx` stay alive until the outcome lands, so the completion may
// touch them.
void save_device_payload(std::string const& json, bool update,
                         GtkDialog* dialog, FormContext* ctx) {
  if (g_state.api == nullptr) return;
  auto done = [dialog, ctx](pb::Unpacked<rivet::Bytes>& r) {
    if (r.ok) {
      gtk_window_destroy(GTK_WINDOW(dialog));
      delete ctx;
      load_all_async();
      return;
    }
    ctx->saving = false;
    if (ctx->save_button != nullptr) {
      gtk_widget_set_sensitive(ctx->save_button, TRUE);
    }
    if (ctx->form_status != nullptr) {
      gtk_widget_set_visible(GTK_WIDGET(ctx->form_status), FALSE);
    }
    if (ctx->form_error != nullptr) {
      gtk_label_set_text(ctx->form_error, clean_error(r.error).c_str());
      gtk_widget_set_visible(GTK_WIDGET(ctx->form_error), TRUE);
    }
  };
  if (update) {
    (void)g_state.api->update_device_async(
        to_bytes(json),
        [done](rivet_app::Result<rivet::Bytes> result) {
          auto unpacked = pb::unpack(result);
          pb::post_to_main<pb::Unpacked<rivet::Bytes>>(done, std::move(unpacked));
        });
  } else {
    (void)g_state.api->add_device_async(
        to_bytes(json),
        [done](rivet_app::Result<rivet::Bytes> result) {
          auto unpacked = pb::unpack(result);
          pb::post_to_main<pb::Unpacked<rivet::Bytes>>(done, std::move(unpacked));
        });
  }
}

void delete_device_async(std::string const& id) {
  if (g_state.api == nullptr) return;
  (void)g_state.api->delete_device_async(
      id, [](rivet_app::Result<void> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<void>>(
            [](pb::Unpacked<void>& r) {
              if (!r.ok) {
                show_error(r.error);
                return;
              }
              load_all_async();
            },
            std::move(unpacked));
      });
}

// Issue #24: the dialog stays open until persistence is confirmed. A
// response while a save is in flight (double Save, cancel, window close)
// is ignored; a failure keeps every typed field in place and re-enables
// Save as the retry. The widgets stay alive through the RPC, so the
// payload is captured at click time and the context dies only with the
// dialog (cancel path or confirmed success).
void on_form_response(GtkDialog* dialog, int response, gpointer user_data) {
  FormContext* ctx = static_cast<FormContext*>(user_data);
  if (ctx->saving) {
    return;  // the outcome must be known before anything happens
  }
  bool const accepted = response == GTK_RESPONSE_ACCEPT;
  if (accepted && !form_valid(*ctx)) {
    // Enter can trigger Accept while the button is insensitive; keep the
    // form open under its hints
    refresh_form_validity(ctx);
    return;
  }
  if (!accepted) {
    delete ctx;
    gtk_window_destroy(GTK_WINDOW(dialog));
    return;
  }

  // capture the values now — edits made during the flight belong to the
  // next retry, not to this request
  ctx->name_text = entry_text(ctx->name);
  ctx->icon_text = entry_text(ctx->icon);
  ctx->category_index =
      ctx->category != nullptr
          ? gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->category))
          : -1;
  ctx->price_text = entry_text(ctx->price);
  ctx->date_text = entry_text(ctx->date_entry);
  ctx->willing_active =
      ctx->willing_on != nullptr && gtk_switch_get_active(ctx->willing_on);
  ctx->willing_text = entry_text(ctx->willing);
  ctx->notes_text = entry_text(ctx->notes);
  ctx->saving = true;
  if (ctx->save_button != nullptr) {
    gtk_widget_set_sensitive(ctx->save_button, FALSE);
  }
  if (ctx->form_error != nullptr) {
    gtk_widget_set_visible(GTK_WIDGET(ctx->form_error), FALSE);
  }
  if (ctx->form_status != nullptr) {
    gtk_widget_set_visible(GTK_WIDGET(ctx->form_status), TRUE);
  }

  std::string const category =
      (ctx->category_index >= 0 &&
       ctx->category_index < static_cast<gint>(std::size(kCategories)))
          ? kCategories[ctx->category_index]
          : std::string("other");

  auto const price_minor = parse_amount_minor(ctx->price_text);
  auto const willing_minor = parse_amount_minor(ctx->willing_text);

  std::string const json = build_json([&](JsonBuilder* b) {
    builder_string(b, "name", ctx->name_text);
    builder_string(b, "icon", ctx->icon_text.empty() ? "📦" : ctx->icon_text);
    builder_string(b, "category", category);
    builder_int(b, "priceMinor", price_minor);
    builder_string(b, "currency", ctx->currency);
    builder_string(b, "purchaseDate", normalize_date(ctx->date_text));
    if (ctx->willing_active && willing_minor > 0) {
      builder_int(b, "willingPerDayMinor", willing_minor);
    } else {
      builder_null(b, "willingPerDayMinor");
    }
    builder_string(b, "notes", ctx->notes_text);
    if (ctx->update) builder_string(b, "id", ctx->id);
  });
  save_device_payload(json, ctx->update, dialog, ctx);
}

void open_device_form(bool update, std::string const& id) {
  DeviceRow existing;
  if (update) {
    bool found = false;
    for (DeviceRow const& row : g_state.document.devices) {
      if (row.id == id) {
        existing = row;
        found = true;
      }
    }
    if (!found) return;
  }

  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_dialog_new_with_buttons(
      (update ? s.edit() : s.add_device()).c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.cancel().c_str(), GTK_RESPONSE_CANCEL,
      s.save().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(panel, 8);
  gtk_widget_set_margin_bottom(panel, 8);
  gtk_widget_set_margin_start(panel, 16);
  gtk_widget_set_margin_end(panel, 16);
  gtk_box_append(GTK_BOX(area), panel);

  auto* context = new FormContext();
  context->currency = update ? existing.currency : g_state.document.currency;
  context->update = update;
  context->id = id;

  auto* name = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(name), existing.name.c_str());
  context->name = GTK_ENTRY(name);
  form_row(panel, s.name().c_str(), name);

  auto* icon = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(icon), existing.icon.c_str());
  context->icon = GTK_ENTRY(icon);
  form_row(panel, (s.icon() + " (💻📱🎧…)").c_str(), icon);

  auto* category = gtk_combo_box_text_new();
  int selected = static_cast<int>(std::size(kCategories)) - 1;  // "other"
  for (int i = 0; i < static_cast<int>(std::size(kCategories)); ++i) {
    // localized display name; the id travels to the backend
    std::string const label_text = [&s, i] {
      switch (i) {
        case 0: return s.category_computer();
        case 1: return s.category_phone();
        case 2: return s.category_tablet();
        case 3: return s.category_watch();
        case 4: return s.category_audio();
        case 5: return s.category_camera();
        case 6: return s.category_gaming();
        case 7: return s.category_appliance();
        case 8: return s.category_accessory();
        default: return s.category_other();
      }
    }();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(category),
                                   label_text.c_str());
    if (kCategories[i] == existing.category) selected = i;
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(category), selected);
  context->category = GTK_COMBO_BOX_TEXT(category);
  form_row(panel, s.category().c_str(), category);

  auto* price = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(price), "0.00");
  if (update) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", existing.price_minor / 100.0);
    gtk_editable_set_text(GTK_EDITABLE(price), buffer);
  }
  context->price = GTK_ENTRY(price);
  form_row(panel,
           (s.price() + " (" + context->currency + ")").c_str(), price);

  auto* price_error = gtk_label_new(s.price_required_hint().c_str());
  gtk_widget_set_halign(price_error, GTK_ALIGN_START);
  gtk_widget_add_css_class(price_error, "error");
  gtk_widget_set_visible(price_error, FALSE);
  gtk_box_append(GTK_BOX(panel), price_error);
  context->price_error = GTK_LABEL(price_error);

  auto* date_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto* date_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(date_entry),
                        (update ? existing.purchase_date : today_string())
                            .c_str());
  gtk_widget_set_hexpand(date_entry, TRUE);
  context->date_entry = GTK_ENTRY(date_entry);
  gtk_box_append(GTK_BOX(date_row), date_entry);

  auto* calendar_button = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(calendar_button), "📅");
  auto* popover = gtk_popover_new();
  auto* calendar = gtk_calendar_new();
  gtk_popover_set_child(GTK_POPOVER(popover), calendar);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(calendar_button), popover);
  g_signal_connect(calendar, "day-selected",
                   G_CALLBACK(on_calendar_day_selected), date_entry);
  gtk_box_append(GTK_BOX(date_row), calendar_button);
  form_row(panel, s.purchase_date().c_str(), date_row);

  auto* date_error = gtk_label_new(s.date_invalid_hint().c_str());
  gtk_widget_set_halign(date_error, GTK_ALIGN_START);
  gtk_widget_add_css_class(date_error, "error");
  gtk_widget_set_visible(date_error, FALSE);
  gtk_box_append(GTK_BOX(panel), date_error);
  context->date_error = GTK_LABEL(date_error);

  auto* willing_on = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(willing_on), existing.has_willing);
  context->willing_on = GTK_SWITCH(willing_on);
  form_row(panel, s.willing_label().c_str(), willing_on);

  auto* willing = gtk_entry_new();
  if (update && existing.has_willing) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f",
                  existing.willing_per_day_minor / 100.0);
    gtk_editable_set_text(GTK_EDITABLE(willing), buffer);
  }
  context->willing = GTK_ENTRY(willing);
  form_row(panel, s.willing_per_day().c_str(), willing);

  // enabled with garbage would silently drop the field — surface that live
  auto* willing_error = gtk_label_new(s.price_required_hint().c_str());
  gtk_widget_set_halign(willing_error, GTK_ALIGN_START);
  gtk_widget_add_css_class(willing_error, "error");
  gtk_widget_set_visible(willing_error, FALSE);
  gtk_box_append(GTK_BOX(panel), willing_error);
  context->willing_error = GTK_LABEL(willing_error);

  auto* hint = gtk_label_new(s.willing_hint().c_str());
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_widget_add_css_class(hint, "payback-dim");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(panel), hint);

  auto* notes = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(notes), existing.notes.c_str());
  context->notes = GTK_ENTRY(notes);
  form_row(panel, s.notes().c_str(), notes);

  // save lifecycle surface (issue #24): the status line covers the flight,
  // the error line survives a failure so Save acts as Retry in place
  auto* form_status = gtk_label_new(s.saving().c_str());
  gtk_widget_set_halign(form_status, GTK_ALIGN_START);
  gtk_widget_add_css_class(form_status, "payback-dim");
  gtk_widget_set_visible(form_status, FALSE);
  gtk_box_append(GTK_BOX(panel), form_status);
  context->form_status = GTK_LABEL(form_status);

  auto* form_error = gtk_label_new("");
  gtk_widget_set_halign(form_error, GTK_ALIGN_START);
  gtk_widget_add_css_class(form_error, "error");
  gtk_label_set_wrap(GTK_LABEL(form_error), TRUE);
  gtk_widget_set_visible(form_error, FALSE);
  gtk_box_append(GTK_BOX(panel), form_error);
  context->form_error = GTK_LABEL(form_error);

  g_signal_connect(price, "changed", G_CALLBACK(on_form_field_changed),
                   context);
  g_signal_connect(date_entry, "changed", G_CALLBACK(on_form_field_changed),
                   context);
  g_signal_connect(willing, "changed", G_CALLBACK(on_form_field_changed),
                   context);
  g_signal_connect(willing_on, "notify::active",
                   G_CALLBACK(on_form_toggled), context);
  context->save_button = gtk_dialog_get_widget_for_response(
      GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  refresh_form_validity(context);
  g_signal_connect(dialog, "response", G_CALLBACK(on_form_response), context);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ------------------------------------------------------- updates (consent)

void stop_update_poll() {
  if (g_state.poll_source != 0) {
    g_source_remove(g_state.poll_source);
    g_state.poll_source = 0;
  }
}

int poll_tick(gpointer) {
  if (g_state.api == nullptr) return G_SOURCE_REMOVE;
  (void)g_state.api->update_state_async(
      [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) return;
              JsonDoc doc(bytes_to_string(r.value));
              JsonObject* root = doc.root();
              if (root == nullptr) return;
              std::string const phase = jstr(root, "phase");
              if (g_state.update_bar != nullptr) {
                gtk_progress_bar_set_fraction(
                    g_state.update_bar,
                    std::clamp(static_cast<double>(jint(root, "percent")),
                               0.0, 100.0) /
                        100.0);
              }
              if (phase == "downloaded" || phase == "error") {
                if (g_state.poll_source != 0) {
                  g_source_remove(g_state.poll_source);
                  g_state.poll_source = 0;
                }
                std::string const path = jstr(root, "downloadedPath");
                std::string const message = jstr(root, "message");
                if (g_state.update_dialog != nullptr) {
                  gtk_window_destroy(g_state.update_dialog);
                  g_state.update_dialog = nullptr;
                  g_state.update_bar = nullptr;
                }
                if (phase == "error") {
                  show_error(message);
                  return;
                }
                // downloaded: consent to install (Linux = replace manually)
                payback::linux_strings::Strings const s = strings();
                std::string body = s.update_ready_body();
                std::string const version = jstr(root, "availableVersion");
                {
                  std::size_t at;
                  while ((at = body.find("{version}")) !=
                         std::string::npos) {
                    body.replace(at, 9, version.empty() ? "?" : version);
                  }
                }
                auto* dialog = gtk_dialog_new_with_buttons(
                    s.update_available().c_str(), g_state.window,
                    static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                    s.close().c_str(), GTK_RESPONSE_CLOSE,
                    s.install_now().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
                GtkWidget* area =
                    gtk_dialog_get_content_area(GTK_DIALOG(dialog));
                auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
                gtk_widget_set_margin_top(content, 8);
                gtk_widget_set_margin_bottom(content, 8);
                gtk_widget_set_margin_start(content, 16);
                gtk_widget_set_margin_end(content, 16);
                gtk_box_append(GTK_BOX(area), content);
                auto* text = gtk_label_new(body.c_str());
                gtk_label_set_wrap(GTK_LABEL(text), TRUE);
                gtk_box_append(GTK_BOX(content), text);
                auto* path_label = gtk_label_new(path.c_str());
                gtk_widget_add_css_class(path_label, "payback-dim");
                gtk_label_set_selectable(GTK_LABEL(path_label), TRUE);
                gtk_box_append(GTK_BOX(content), path_label);
                auto* note = gtk_label_new(s.update_install_note().c_str());
                gtk_widget_add_css_class(note, "payback-dim");
                gtk_label_set_wrap(GTK_LABEL(note), TRUE);
                gtk_box_append(GTK_BOX(content), note);
                auto* open = gtk_button_new_with_label(s.update_open_folder().c_str());
                g_signal_connect(
                    open, "clicked",
                    G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                      auto const* p =
                          static_cast<std::string*>(user_data);
                      std::filesystem::path const dir =
                          std::filesystem::path(*p).parent_path();
                      gtk_show_uri(g_state.window,
                                   ("file://" + dir.string()).c_str(),
                                   GDK_CURRENT_TIME);
                    }),
                    new std::string(path));
                gtk_box_append(GTK_BOX(content), open);
                gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                             g_state.window);
                gtk_window_present(GTK_WINDOW(dialog));
              }
            },
            std::move(unpacked));
      });
  return G_SOURCE_CONTINUE;
}

void start_update_download() {
  if (g_state.api == nullptr) return;
  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_dialog_new_with_buttons(
      s.update_available().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.cancel().c_str(), GTK_RESPONSE_CLOSE, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 8);
  gtk_widget_set_margin_bottom(content, 8);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_box_append(GTK_BOX(area), content);
  auto* bar = gtk_progress_bar_new();
  gtk_widget_set_size_request(bar, 300, -1);
  gtk_box_append(GTK_BOX(content), bar);
  auto* label = gtk_label_new(s.downloading().c_str());
  gtk_box_append(GTK_BOX(content), label);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int, gpointer) {
        stop_update_poll();
        g_state.update_dialog = nullptr;
        g_state.update_bar = nullptr;
        gtk_window_destroy(GTK_WINDOW(dialog));
      }),
      nullptr);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
  gtk_window_present(GTK_WINDOW(dialog));
  g_state.update_dialog = GTK_WINDOW(dialog);
  g_state.update_bar = GTK_PROGRESS_BAR(bar);
  g_state.poll_source = g_timeout_add(400, poll_tick, nullptr);

  (void)g_state.api->start_download_async(
      [](rivet_app::Result<void> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<void>>(
            [](pb::Unpacked<void>& r) {
              if (!r.ok) show_error(r.error);
            },
            std::move(unpacked));
      });
}

void replace_all_in(std::string& text, std::string const& from,
                    std::string const& to) {
  std::size_t at = 0;
  while ((at = text.find(from, at)) != std::string::npos) {
    text.replace(at, from.size(), to);
    at += to.size();
  }
}

void show_update_consent(std::string const& version, double size_bytes) {
  payback::linux_strings::Strings const s = strings();
  std::string body = s.update_available_body();
  replace_all_in(body, "{version}", version);
  replace_all_in(body, "{size}", format_size(size_bytes));

  auto* dialog = gtk_dialog_new_with_buttons(
      s.update_available().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.cancel().c_str(), GTK_RESPONSE_CANCEL,
      s.download_update().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* text = gtk_label_new(body.c_str());
  gtk_label_set_wrap(GTK_LABEL(text), TRUE);
  gtk_widget_set_margin_top(text, 8);
  gtk_widget_set_margin_bottom(text, 8);
  gtk_widget_set_margin_start(text, 16);
  gtk_widget_set_margin_end(text, 16);
  gtk_box_append(GTK_BOX(area), text);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);

  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer) {
        bool const go = response == GTK_RESPONSE_ACCEPT;
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (go) {
          pb::post_to_main<int>(
              [](int&) { start_update_download(); }, 0);
        }
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

void check_updates(bool force, bool silent) {
  if (g_state.api == nullptr) return;
  if (!silent) show_info(strings().update_check_title());
  (void)g_state.api->check_updates_async(
      force, [silent](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [silent](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) {
                if (!silent) show_error(r.error);
                return;
              }
              JsonDoc doc(bytes_to_string(r.value));
              JsonObject* root = doc.root();
              if (root == nullptr) return;
              std::string const status = jstr(root, "status");
              if (status == "available") {
                show_update_consent(jstr(root, "availableVersion"),
                                    jdouble(root, "sizeBytes"));
              } else if (!silent) {
                payback::linux_strings::Strings const s = strings();
                std::string const body =
                    status == "up-to-date" ? s.up_to_date()
                                           : s.update_error();
                auto* dialog = gtk_dialog_new_with_buttons(
                    s.updates().c_str(), g_state.window,
                    static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                    s.close().c_str(), GTK_RESPONSE_CLOSE, nullptr);
                GtkWidget* area =
                    gtk_dialog_get_content_area(GTK_DIALOG(dialog));
                auto* text = gtk_label_new(body.c_str());
                gtk_widget_set_margin_top(text, 8);
                gtk_widget_set_margin_bottom(text, 8);
                gtk_widget_set_margin_start(text, 16);
                gtk_widget_set_margin_end(text, 16);
                gtk_box_append(GTK_BOX(area), text);
                gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                             g_state.window);
                g_signal_connect(
                    dialog, "response",
                    G_CALLBACK(
                        +[](GtkDialog* dialog, int, gpointer) {
                          gtk_window_destroy(GTK_WINDOW(dialog));
                        }),
                    nullptr);
                gtk_window_present(GTK_WINDOW(dialog));
              }
            },
            std::move(unpacked));
      });
}

// -------------------------------------------------- sync settings (#9 M1)

// same list and order as the mac settings picker
std::string const kCurrencies[] = {"CNY", "USD", "EUR", "JPY", "GBP", "HKD",
                                   "TWD", "KRW", "SGD", "AUD", "CAD", "CHF"};

std::string currency_option_label(std::string const& code) {
  std::string const symbol = currency_symbol(code);
  std::string const fallback = code + " ";
  return symbol == fallback ? code : code + " · " + symbol;
}

SyncSnapshot parse_sync_snapshot(JsonObject* root) {
  SyncSnapshot snapshot;
  snapshot.valid = root != nullptr;
  if (root == nullptr) return snapshot;
  snapshot.enabled = jbool(root, "enabled");
  snapshot.dir = jstr(root, "dir");
  if (JsonObject* machine = jobj(root, "machine")) {
    snapshot.machine_id = jstr(machine, "id");
    snapshot.machine_name = jstr(machine, "name");
  }
  JsonArray* machines = jarray(root, "machines");
  guint const count = jlen(machines);
  for (guint i = 0; i < count; ++i) {
    JsonObject* entry = jat(machines, i);
    if (entry == nullptr) continue;
    snapshot.machines.emplace_back(jstr(entry, "id"), jstr(entry, "name"));
  }
  snapshot.last_merge_at =
      jnull(root, "lastMergeAt") ? std::string() : jstr(root, "lastMergeAt");
  snapshot.revision = jint(root, "revision");
  JsonArray* ignored = jarray(root, "ignoredFiles");
  guint const ignored_count = jlen(ignored);
  for (guint i = 0; i < ignored_count; ++i) {
    JsonNode* node = json_array_get_element(ignored, i);
    if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) {
      continue;
    }
    char const* value = json_node_get_string(node);
    if (value != nullptr) snapshot.ignored_files.emplace_back(value);
  }
  return snapshot;
}

void update_sync_dialog_rows() {
  if (g_state.sync_dialog == nullptr || !g_state.sync.valid) return;
  payback::linux_strings::Strings const s = strings();

  gtk_label_set_text(g_state.sync_status_label,
                     (g_state.sync.enabled ? s.sync_status_on()
                                           : s.sync_status_off())
                         .c_str());
  // swap tint classes wholesale; stale classes outlive their meaning
  gtk_widget_remove_css_class(GTK_WIDGET(g_state.sync_status_label),
                              "payback-earned");
  gtk_widget_remove_css_class(GTK_WIDGET(g_state.sync_status_label),
                              "payback-dim");
  gtk_widget_add_css_class(GTK_WIDGET(g_state.sync_status_label),
                           g_state.sync.enabled ? "payback-earned"
                                                : "payback-dim");

  gtk_button_set_label(g_state.sync_toggle_button,
                       (g_state.sync.enabled ? s.sync_disable()
                                             : s.sync_enable())
                           .c_str());
  gtk_widget_set_visible(GTK_WIDGET(g_state.sync_enabled_box),
                         g_state.sync.enabled ? TRUE : FALSE);

  gtk_label_set_text(g_state.sync_dir_label, g_state.sync.dir.c_str());

  std::string machines_text;
  for (auto const& machine : g_state.sync.machines) {
    machines_text += "• " + machine.second;
    if (machine.first == g_state.sync.machine_id) {
      machines_text += "  (" + g_state.sync.machine_name + ")";
    }
    machines_text += "\n";
  }
  gtk_label_set_text(g_state.sync_machines_value, machines_text.c_str());

  std::string last_merge = "—";
  if (!g_state.sync.last_merge_at.empty()) {
    last_merge = s.sync_last_merge_label() + ": " + g_state.sync.last_merge_at;
  }
  gtk_label_set_text(g_state.sync_last_merge_value, last_merge.c_str());

  std::string warning;
  if (!g_state.sync.ignored_files.empty()) {
    warning = s.sync_ignored_warning();
    replace_all_in(warning, "{count}",
                   std::to_string(g_state.sync.ignored_files.size()));
  }
  gtk_label_set_text(g_state.sync_warning_label, warning.c_str());
  gtk_widget_set_visible(GTK_WIDGET(g_state.sync_warning_row),
                         warning.empty() ? FALSE : TRUE);

  // never stomp a name the user is halfway through typing
  if (g_state.sync_machine_entry != nullptr &&
      !gtk_widget_has_focus(GTK_WIDGET(g_state.sync_machine_entry))) {
    gtk_editable_set_text(GTK_EDITABLE(g_state.sync_machine_entry),
                          g_state.sync.machine_name.c_str());
  }
  if (g_state.sync_machine_entry != nullptr) {
    on_sync_machine_entry_changed(nullptr, nullptr);
  }

  if (g_state.currency_box != nullptr) {
    // mirror a currency that arrived through sync without re-saving it
    g_state.currency_box_updating = true;
    int active = 0;
    for (int i = 0; i < static_cast<int>(std::size(kCurrencies)); ++i) {
      if (kCurrencies[i] == g_state.document.currency) active = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_state.currency_box), active);
    g_state.currency_box_updating = false;
  }
}

void request_sync_state() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->sync_state_async(
      [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) {
                show_error(r.error);
                return;
              }
              JsonDoc doc(bytes_to_string(r.value));
              g_state.sync = parse_sync_snapshot(doc.root());
              update_sync_dialog_rows();
            },
            std::move(unpacked));
      });
}

// 5s poll: the backend re-merges when the sync folder changed on disk, so a
// revision bump means another machine rewrote the shards — refresh the whole
// document through the normal load-all path. Errors stay silent: the poll is
// a background courtesy, not a user action.
int sync_poll_tick(gpointer) {
  if (g_state.api == nullptr) return G_SOURCE_CONTINUE;
  (void)g_state.api->sync_state_async(
      [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) return;
              JsonDoc doc(bytes_to_string(r.value));
              JsonObject* root = doc.root();
              if (root == nullptr) return;
              g_state.sync = parse_sync_snapshot(root);
              if (g_state.sync_revision < 0) {
                // first snapshot only primes the baseline
                g_state.sync_revision = g_state.sync.revision;
              } else if (g_state.sync_revision != g_state.sync.revision) {
                g_state.sync_revision = g_state.sync.revision;
                load_all_async();
              }
              update_sync_dialog_rows();
            },
            std::move(unpacked));
      });
  return G_SOURCE_CONTINUE;
}

void configure_sync_async(std::optional<std::string> dir) {
  if (g_state.api == nullptr) return;
  std::string const json = build_json([&](JsonBuilder* b) {
    if (dir.has_value()) {
      builder_string(b, "dir", *dir);
    } else {
      builder_null(b, "dir");
    }
  });
  (void)g_state.api->configure_sync_async(
      to_bytes(json), [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) {
                show_error(strings().sync_enable_failed() + ": " + r.error);
                return;
              }
              JsonDoc doc(bytes_to_string(r.value));
              g_state.sync = parse_sync_snapshot(doc.root());
              update_sync_dialog_rows();
            },
            std::move(unpacked));
      });
}

void rename_machine_async(std::string const& name) {
  if (g_state.api == nullptr) return;
  std::string const json = build_json(
      [&](JsonBuilder* b) { builder_string(b, "name", name); });
  (void)g_state.api->rename_machine_async(
      to_bytes(json), [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) {
                show_error(r.error);
                return;
              }
              JsonDoc doc(bytes_to_string(r.value));
              g_state.sync = parse_sync_snapshot(doc.root());
              update_sync_dialog_rows();
            },
            std::move(unpacked));
      });
}

void on_sync_machine_save_clicked(GtkButton*, gpointer) {
  if (g_state.sync_machine_entry == nullptr) return;
  std::string const name = entry_text(g_state.sync_machine_entry);
  // trim like the mac host; the backend rejects empty and >100 names
  std::size_t const first = name.find_first_not_of(" \t\n\r");
  if (first == std::string::npos) return;
  std::size_t const last = name.find_last_not_of(" \t\n\r");
  rename_machine_async(name.substr(first, last - first + 1));
}

// Save lights up only for a non-blank name, mirroring the mac host
void on_sync_machine_entry_changed(GtkEditable*, gpointer) {
  if (g_state.sync_machine_save == nullptr ||
      g_state.sync_machine_entry == nullptr) {
    return;
  }
  std::string const name = entry_text(g_state.sync_machine_entry);
  bool const valid = name.find_first_not_of(" \t\n\r") != std::string::npos;
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.sync_machine_save),
                           valid ? TRUE : FALSE);
}

void on_sync_open_folder_clicked(GtkButton*, gpointer) {
  if (g_state.sync.dir.empty()) return;
  gtk_show_uri(g_state.window,
               ("file://" + g_state.sync.dir).c_str(), GDK_CURRENT_TIME);
}

// confirm-first enable flow: probe the common sync homes (a `Payback`
// subfolder inside any of them), fall back to the folder picker
void begin_enable_sync() {
  std::vector<std::string> candidates;
  std::string const home = g_get_home_dir();
  char const* const bases[] = {"iCloudDrive", "Dropbox",      "OneDrive",
                               "Nutstore Files", "坚果云",     "Sync",
                               "Syncthing"};
  for (char const* base : bases) {
    std::error_code ec;
    auto const candidate =
        std::filesystem::path(home) / base / "Payback";
    if (std::filesystem::is_directory(candidate, ec)) {
      candidates.push_back(candidate.string());
    }
  }
  if (candidates.empty()) {
    open_sync_folder_chooser();
    return;
  }

  payback::linux_strings::Strings const s = strings();
  std::string body = s.sync_detected_body();
  replace_all_in(body, "{folder}",
                 std::filesystem::path(candidates.front()).filename().string());
  auto* dialog = gtk_dialog_new_with_buttons(
      s.sync_detected_title().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      s.cancel().c_str(), GTK_RESPONSE_CANCEL,
      s.sync_choose_folder().c_str(), 1,
      s.sync_use_detected().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* text = gtk_label_new(body.c_str());
  gtk_label_set_wrap(GTK_LABEL(text), TRUE);
  gtk_widget_set_margin_top(text, 8);
  gtk_widget_set_margin_bottom(text, 8);
  gtk_widget_set_margin_start(text, 16);
  gtk_widget_set_margin_end(text, 16);
  gtk_box_append(GTK_BOX(area), text);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
  auto* folder = new std::string(candidates.front());
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer user_data) {
        std::unique_ptr<std::string> folder(
            static_cast<std::string*>(user_data));
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (response == GTK_RESPONSE_ACCEPT) {
          pb::post_to_main<std::string>(
              [](std::string& p) { configure_sync_async(p); }, *folder);
        } else if (response == 1) {
          pb::post_to_main<int>(
              [](int&) { open_sync_folder_chooser(); }, 0);
        }
      }),
      folder);
  gtk_window_present(GTK_WINDOW(dialog));
}

void on_sync_toggle_clicked(GtkButton*, gpointer) {
  if (!g_state.sync.valid) return;
  if (g_state.sync.enabled) {
    // turning sync off keeps local data; one confirmation, then null dir
    payback::linux_strings::Strings const s = strings();
    auto* dialog = gtk_dialog_new_with_buttons(
        s.sync_disable_confirm_title().c_str(), g_state.window,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                    GTK_DIALOG_DESTROY_WITH_PARENT),
        s.cancel().c_str(), GTK_RESPONSE_CANCEL,
        s.sync_disable().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    auto* text = gtk_label_new(s.sync_disable_confirm_body().c_str());
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_widget_set_margin_top(text, 8);
    gtk_widget_set_margin_bottom(text, 8);
    gtk_widget_set_margin_start(text, 16);
    gtk_widget_set_margin_end(text, 16);
    gtk_box_append(GTK_BOX(area), text);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
    g_signal_connect(
        dialog, "response",
        G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer) {
          bool const confirmed = response == GTK_RESPONSE_ACCEPT;
          gtk_window_destroy(GTK_WINDOW(dialog));
          if (confirmed) {
            pb::post_to_main<int>(
                [](int&) { configure_sync_async(std::nullopt); }, 0);
          }
        }),
        nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
  } else {
    begin_enable_sync();
  }
}

void open_settings_dialog() {
  if (g_state.sync_dialog != nullptr) {
    gtk_window_present(g_state.sync_dialog);
    return;
  }
  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_dialog_new_with_buttons(
      s.settings_title().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      s.close().c_str(), GTK_RESPONSE_CLOSE, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(content, 10);
  gtk_widget_set_margin_bottom(content, 12);
  gtk_widget_set_margin_start(content, 18);
  gtk_widget_set_margin_end(content, 18);
  gtk_box_append(GTK_BOX(area), content);

  gtk_box_append(GTK_BOX(content),
                 detail_label(s.data_sync_section().c_str(), "payback-section"));
  auto* how = gtk_label_new(s.sync_how_it_works().c_str());
  gtk_label_set_wrap(GTK_LABEL(how), TRUE);
  gtk_widget_set_halign(how, GTK_ALIGN_START);
  gtk_widget_add_css_class(how, "payback-dim");
  gtk_box_append(GTK_BOX(content), how);

  auto* status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* status_label = gtk_label_new("…");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(status_label, TRUE);
  g_state.sync_status_label = GTK_LABEL(status_label);
  gtk_box_append(GTK_BOX(status_row), status_label);
  auto* toggle = gtk_button_new_with_label("…");
  g_state.sync_toggle_button = GTK_BUTTON(toggle);
  g_signal_connect(toggle, "clicked", G_CALLBACK(on_sync_toggle_clicked),
                   nullptr);
  gtk_box_append(GTK_BOX(status_row), toggle);
  gtk_box_append(GTK_BOX(content), status_row);

  auto* enabled_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  g_state.sync_enabled_box = GTK_BOX(enabled_box);
  gtk_widget_set_visible(enabled_box, FALSE);

  // sync folder row: the path plus a one-click reveal in the file manager
  auto* folder_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_append(GTK_BOX(folder_box),
                 detail_label(s.sync_folder_label().c_str(), "payback-dim"));
  auto* dir_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* dir_label = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(dir_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable(GTK_LABEL(dir_label), TRUE);
  gtk_widget_set_halign(dir_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(dir_label, TRUE);
  gtk_widget_add_css_class(dir_label, "payback-mono");
  g_state.sync_dir_label = GTK_LABEL(dir_label);
  gtk_box_append(GTK_BOX(dir_row), dir_label);
  auto* open_button = gtk_button_new_with_label(s.sync_open_folder().c_str());
  g_signal_connect(open_button, "clicked",
                   G_CALLBACK(on_sync_open_folder_clicked), nullptr);
  gtk_box_append(GTK_BOX(dir_row), open_button);
  gtk_box_append(GTK_BOX(folder_box), dir_row);
  gtk_box_append(GTK_BOX(enabled_box), folder_box);

  // machine name: saved via rename-machine, one Save click or Enter
  auto* machine_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(machine_row),
                 detail_label(s.sync_machine_name_label().c_str(), "payback-dim"));
  auto* machine_entry = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(machine_entry), 100);
  gtk_widget_set_hexpand(machine_entry, TRUE);
  g_state.sync_machine_entry = GTK_ENTRY(machine_entry);
  g_signal_connect(machine_entry, "changed",
                   G_CALLBACK(on_sync_machine_entry_changed), nullptr);
  gtk_box_append(GTK_BOX(machine_row), machine_entry);
  auto* machine_save = gtk_button_new_with_label(s.save().c_str());
  g_state.sync_machine_save = GTK_BUTTON(machine_save);
  g_signal_connect(machine_save, "clicked",
                   G_CALLBACK(on_sync_machine_save_clicked), nullptr);
  gtk_box_append(GTK_BOX(machine_row), machine_save);
  gtk_box_append(GTK_BOX(enabled_box), machine_row);

  gtk_box_append(GTK_BOX(enabled_box),
                 detail_label(s.sync_machines_label().c_str(), "payback-dim"));
  auto* machines_value = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(machines_value), TRUE);
  gtk_widget_set_halign(machines_value, GTK_ALIGN_START);
  gtk_widget_add_css_class(machines_value, "payback-meta");
  g_state.sync_machines_value = GTK_LABEL(machines_value);
  gtk_box_append(GTK_BOX(enabled_box), machines_value);
  auto* last_merge = gtk_label_new("");
  gtk_widget_set_halign(last_merge, GTK_ALIGN_START);
  gtk_widget_add_css_class(last_merge, "payback-dim");
  g_state.sync_last_merge_value = GTK_LABEL(last_merge);
  gtk_box_append(GTK_BOX(enabled_box), last_merge);

  auto* warning_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(warning_row), gtk_label_new("⚠️"));
  auto* warning_label = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(warning_label), TRUE);
  gtk_widget_set_halign(warning_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(warning_label, TRUE);
  gtk_widget_add_css_class(warning_label, "payback-warning");
  g_state.sync_warning_label = GTK_LABEL(warning_label);
  gtk_box_append(GTK_BOX(warning_row), warning_label);
  gtk_widget_set_visible(warning_row, FALSE);
  g_state.sync_warning_row = GTK_BOX(warning_row);
  gtk_box_append(GTK_BOX(enabled_box), warning_row);
  gtk_box_append(GTK_BOX(content), enabled_box);

  // default currency: saved through the existing save-settings RPC, whose
  // result is the merged settings — fold it back into the in-memory doc
  gtk_box_append(GTK_BOX(content),
                 detail_label(s.currency().c_str(), "payback-section"));
  auto* currency_box = gtk_combo_box_text_new();
  for (std::string const& code : kCurrencies) {
    std::string const label = currency_option_label(code);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(currency_box),
                                   label.c_str());
  }
  g_state.currency_box = GTK_COMBO_BOX_TEXT(currency_box);
  g_signal_connect(currency_box, "changed", G_CALLBACK(on_currency_changed),
                   nullptr);
  gtk_box_append(GTK_BOX(content), currency_box);
  auto* hint = gtk_label_new(s.currency_hint().c_str());
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_widget_add_css_class(hint, "payback-dim");
  gtk_box_append(GTK_BOX(content), hint);

  // the dialog owns the row widgets; drop our pointers when it dies
  g_signal_connect(
      dialog, "destroy",
      G_CALLBACK(+[](GtkWidget*, gpointer) {
        g_state.sync_dialog = nullptr;
        g_state.sync_status_label = nullptr;
        g_state.sync_toggle_button = nullptr;
        g_state.sync_enabled_box = nullptr;
        g_state.sync_dir_label = nullptr;
        g_state.sync_machines_value = nullptr;
        g_state.sync_last_merge_value = nullptr;
        g_state.sync_warning_row = nullptr;
        g_state.sync_warning_label = nullptr;
        g_state.sync_machine_entry = nullptr;
        g_state.sync_machine_save = nullptr;
        g_state.currency_box = nullptr;
      }),
      nullptr);

  g_state.sync_dialog = GTK_WINDOW(dialog);
  gtk_window_present(GTK_WINDOW(dialog));
  request_sync_state();
  update_sync_dialog_rows();
}

void on_settings_clicked(GtkButton*, gpointer) { open_settings_dialog(); }

// native folder picker; the fallback when nothing familiar was probed
void open_sync_folder_chooser() {
  payback::linux_strings::Strings const s = strings();
  auto* chooser = gtk_file_chooser_dialog_new(
      s.sync_pick_folder_title().c_str(), g_state.window,
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
      s.cancel().c_str(), GTK_RESPONSE_CANCEL,
      s.sync_choose_folder().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  gtk_window_set_transient_for(GTK_WINDOW(chooser), g_state.window);
  g_signal_connect(
      chooser, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer) {
        std::string path;
        if (response == GTK_RESPONSE_ACCEPT) {
          GFile* folder = gtk_file_chooser_get_file(
              GTK_FILE_CHOOSER(dialog));
          if (folder != nullptr) {
            gchar* native = g_file_get_path(folder);
            if (native != nullptr) path = native;
            g_free(native);
            g_object_unref(folder);
          }
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (!path.empty()) {
          pb::post_to_main<std::string>(
              [](std::string& p) { configure_sync_async(p); }, path);
        }
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(chooser));
}

void save_currency_async(std::string const& code) {
  if (g_state.api == nullptr) return;
  std::string const json = build_json(
      [&](JsonBuilder* b) { builder_string(b, "currency", code); });
  (void)g_state.api->save_settings_async(
      to_bytes(json), [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) {
                show_error(r.error);
                return;
              }
              // save-settings returns the merged settings hash (the same
              // shape as load-all's "settings" member)
              JsonDoc doc(bytes_to_string(r.value));
              JsonObject* settings = doc.root();
              if (settings == nullptr) return;
              g_state.document.currency =
                  jstr(settings, "currency", g_state.document.currency);
              g_state.document.update_auto_check =
                  jbool(settings, "updateAutoCheck",
                        g_state.document.update_auto_check);
              render_document_from_cache();
              update_sync_dialog_rows();
            },
            std::move(unpacked));
      });
}

void on_currency_changed(GtkComboBox* box, gpointer) {
  if (g_state.currency_box_updating) return;
  gchar* active = gtk_combo_box_text_get_active_text(
      GTK_COMBO_BOX_TEXT(box));
  if (active == nullptr) return;
  std::string const selected(active);
  g_free(active);
  // "CNY · ¥" display labels fold back to the bare code for the backend
  std::string const code = selected.substr(0, 3);
  if (code == g_state.document.currency) return;
  save_currency_async(code);
}

// ------------------------------------------------------- emotional layer

void run_daily_digest() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->daily_digest_async(
      [](rivet_app::Result<rivet::Bytes> result) {
        auto unpacked = pb::unpack(result);
        pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
            [](pb::Unpacked<rivet::Bytes>& r) {
              if (!r.ok) return;  // the digest is a delight, never an error
              try {
                JsonDoc doc(bytes_to_string(r.value));
                JsonObject* root = doc.root();
                if (root == nullptr || jstr(root, "status") != "ok") return;
                payback::linux_strings::Strings const s = strings();
                std::string body = s.digest_body();
                // mixed currencies: the combined scalar is null and the
                // per-currency list renders instead (#23)
                std::string earned_text;
                if (!jnull(root, "earnedTotalMinor")) {
                  earned_text = money(jdouble(root, "earnedTotalMinor"),
                                      g_state.document.currency);
                } else if (JsonArray* per =
                               jarray(root, "earnedPerCurrency")) {
                  guint const earned_count = jlen(per);
                  for (guint i = 0; i < earned_count; ++i) {
                    JsonObject* entry =
                        json_array_get_object_element(per, i);
                    if (entry == nullptr) continue;
                    if (!earned_text.empty()) earned_text += " · ";
                    earned_text +=
                        money(jdouble(entry, "earnedTotalMinor"),
                              jstr(entry, "currency",
                                   g_state.document.currency.c_str()));
                  }
                }
                if (earned_text.empty()) {
                  earned_text = money(0.0, g_state.document.currency);
                }
                replace_all_in(body, "{earned}", earned_text);
                std::string const best = jstr(root, "bestDeviceName");
                if (!best.empty()) {
                  std::string best_line = s.digest_body_best();
                  replace_all_in(best_line, "{name}", best);
                  replace_all_in(
                      best_line, "{cost}",
                      money(jdouble(root, "bestDeviceCostPerDayMinor"),
                            jstr(root, "bestDeviceCurrency",
                                 g_state.document.currency.c_str())));
                  body += "\n" + best_line;
                }
                show_info(s.digest_title() + "  " + body);
              } catch (...) {
              }
            },
            std::move(unpacked));
      });
}

void maybe_celebrate() {
  if (g_state.celebration_open) return;
  payback::linux_strings::Strings const s = strings();
  std::vector<std::string> lines;
  for (DeviceRow const& row : g_state.document.devices) {
    for (Milestone const& milestone : row.computed.milestones) {
      if (milestone.achieved && milestone.is_new) {
        lines.push_back(milestone_emoji(milestone.key) + " " + row.name +
                        " — " + milestone_label(milestone.key));
      }
    }
  }
  if (lines.empty()) return;

  g_state.celebration_open = true;
  auto* dialog = gtk_dialog_new_with_buttons(
      s.celebrate_title().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.celebrate_keep().c_str(), GTK_RESPONSE_CLOSE, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(content, 8);
  gtk_widget_set_margin_bottom(content, 8);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_box_append(GTK_BOX(area), content);
  for (std::string const& line : lines) {
    auto* item = gtk_label_new(line.c_str());
    gtk_widget_set_halign(item, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(content), item);
  }
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int, gpointer) {
        g_state.celebration_open = false;
        gtk_window_destroy(GTK_WINDOW(dialog));
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ----------------------------------------------------------- about dialog

void show_about_dialog() {
  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "Payback");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), rivet_app::kVersion);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                s.about_content().c_str());
  gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog),
                               s.about_license().c_str());
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                               "https://payback.jrtx.site");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                     s.about_website().c_str());
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ----------------------------------------------------------- language menu

void on_language_changed(GtkComboBox* box, gpointer) {
  int const index = gtk_combo_box_get_active(box);
  if (index < 0) return;
  int const mapped = index == 0 ? -1 : index - 1;
  if (mapped == g_language_override) return;
  g_language_override = mapped;
  save_language_override(mapped);
  apply_language();
}

void apply_language() {
  payback::linux_strings::Strings const s = strings();
  gtk_label_set_text(g_state.total_spent_label, s.total_spent().c_str());
  gtk_label_set_text(g_state.earned_back_label, s.earned_back().c_str());
  gtk_label_set_text(g_state.overall_daily_label, s.overall_daily().c_str());
  gtk_label_set_text(g_state.device_count_label, s.device_count().c_str());
  gtk_button_set_label(g_state.add_button, ("＋ " + s.add_device()).c_str());
  // detail actions live in the headerbar (mac toolbar parity); tooltips
  // carry the labels since the buttons are icon-only
  gtk_widget_set_tooltip_text(GTK_WIDGET(g_state.edit_button),
                              s.edit().c_str());
  gtk_widget_set_tooltip_text(GTK_WIDGET(g_state.delete_button),
                              s.remove().c_str());
  gtk_widget_set_tooltip_text(GTK_WIDGET(g_state.add_button),
                              s.add_device().c_str());
  if (g_state.empty_title != nullptr) {
    gtk_label_set_text(g_state.empty_title, s.empty_title().c_str());
  }
  if (g_state.empty_hint != nullptr) {
    gtk_label_set_text(g_state.empty_hint, s.empty_hint().c_str());
  }
  if (g_state.empty_add_button != nullptr) {
    gtk_button_set_label(g_state.empty_add_button,
                         ("＋ " + s.add_device()).c_str());
  }
  if (g_state.help_menu != nullptr) {
    g_menu_remove_all(g_state.help_menu);
    g_menu_append(g_state.help_menu, s.check_updates_menu().c_str(),
                  "win.check-updates");
    g_menu_append(g_state.help_menu, s.about_menu().c_str(), "win.about");
  }
  if (g_state.help_menu_button != nullptr) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(g_state.help_menu_button),
                                s.help_menu().c_str());
  }
  // rebuild sort items; suppressing the handler with the guard is enough
  GtkComboBoxText* sort = g_state.sort_box;
  gtk_combo_box_text_remove(sort, 0);
  gtk_combo_box_text_remove(sort, 0);
  gtk_combo_box_text_remove(sort, 0);
  gtk_combo_box_text_insert_text(sort, 0, s.sort_added().c_str());
  gtk_combo_box_text_insert_text(sort, 1, s.sort_daily_cost().c_str());
  gtk_combo_box_text_insert_text(sort, 2, s.sort_payback().c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(sort), g_state.sort_mode);
  render_document_from_cache();
  // the settings dialog is rebuilt wholesale so no stale caption survives a
  // language flip (mac rebuilds its whole tree the same way); null the
  // pointer first so reopen cannot present the dying window
  if (g_state.sync_dialog != nullptr) {
    GtkWindow* dialog = g_state.sync_dialog;
    g_state.sync_dialog = nullptr;
    gtk_window_destroy(dialog);
    open_settings_dialog();
  }
}

// ----------------------------------------------------------- boot + startup

int on_backend_finished(gpointer) {
  if (g_state.startup_thread.joinable()) {
    g_state.startup_thread.join();
  }
  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::string error;
  {
    std::lock_guard lock(g_state.startup_mutex);
    backend = std::move(g_state.startup_backend);
    error = std::move(g_state.startup_error);
  }
  if (g_state.shutting_down.load(std::memory_order_acquire)) {
    if (backend != nullptr) backend->stop();
    return G_SOURCE_REMOVE;
  }
  if (!error.empty() || backend == nullptr) {
    show_error(error.empty() ? "backend startup completed without a backend"
                             : error);
    return G_SOURCE_REMOVE;
  }
  g_state.backend = std::move(backend);
  g_state.api = std::make_unique<rivet_app::API>(*g_state.backend);
  show_success(strings().backend_ready());
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.add_button), TRUE);
  if (g_state.empty_add_button != nullptr) {
    gtk_widget_set_sensitive(GTK_WIDGET(g_state.empty_add_button), TRUE);
  }
  // background refresh: another machine's shard writes reach this window
  // within one poll tick (docs/sync-design.md M1)
  if (g_state.sync_poll_source == 0) {
    g_state.sync_poll_source =
        g_timeout_add_seconds(5, sync_poll_tick, nullptr);
  }
  load_all_async();
  return G_SOURCE_REMOVE;
}

std::string executable_path() {
  try {
    return std::filesystem::read_symlink("/proc/self/exe").string();
  } catch (...) {
    return {};
  }
}

struct RuntimeLayout {
  std::filesystem::path petite_boot;
  std::filesystem::path scheme_boot;
  std::filesystem::path racket_boot;
  std::filesystem::path core;
};

std::optional<RuntimeLayout> discover_runtime_layout() {
  std::filesystem::path const exe = executable_path();
  if (exe.empty()) return std::nullopt;
  std::filesystem::path const root = exe.parent_path();
  RuntimeLayout layout{
      root / "runtime" / "petite.boot",
      root / "runtime" / "scheme.boot",
      root / "runtime" / "racket.boot",
      root / "res" / "core.zo",
  };
  if (std::filesystem::exists(layout.petite_boot) &&
      std::filesystem::exists(layout.scheme_boot) &&
      std::filesystem::exists(layout.racket_boot) &&
      std::filesystem::exists(layout.core)) {
    return layout;
  }
  return std::nullopt;
}

void start_backend() {
  set_status(strings().starting_backend(), true);
  auto layout = discover_runtime_layout();
  if (!layout.has_value()) {
    show_error("Missing Rivet runtime layout (runtime/*.boot, res/core.zo) "
               "next to the executable. Build with raco rivet build/dev.");
    return;
  }
  rivet::linux_runtime::RacketRuntimeConfig config;
  config.executable_path = executable_path();
  config.petite_boot = layout->petite_boot.string();
  config.scheme_boot = layout->scheme_boot.string();
  config.racket_boot = layout->racket_boot.string();
  config.backend_bundle = layout->core.string();
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;

  g_state.startup_thread =
      std::thread([config = std::move(config)]() mutable {
        auto backend = std::make_unique<rivet::linux_runtime::Backend>(
            std::move(config));
        try {
          backend->start();
          {
            std::lock_guard lock(g_state.startup_mutex);
            g_state.startup_backend = std::move(backend);
          }
        } catch (std::exception const& e) {
          std::lock_guard lock(g_state.startup_mutex);
          g_state.startup_error = e.what();
        }
        g_idle_add(on_backend_finished, nullptr);
      });
}

// -------------------------------------------------------------- ui build

GtkWidget* summary_tile(GtkLabel** value_out, GtkLabel** label_out) {
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  auto* label = gtk_label_new("");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_add_css_class(label, "payback-dim");
  auto* value = gtk_label_new("—");
  gtk_widget_set_halign(value, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), value);
  *value_out = GTK_LABEL(value);
  *label_out = GTK_LABEL(label);
  return box;
}

void on_sort_changed(GtkComboBox* box, gpointer) {
  int const index = gtk_combo_box_get_active(box);
  if (index >= 0) {
    g_state.sort_mode = index;
    render_device_list();
  }
}

void on_add_clicked(GtkButton*, gpointer) { open_device_form(false, ""); }

// Help-menu actions (the retired header buttons routed to the same entry
// points).
void on_check_updates_action(GSimpleAction*, GVariant*, gpointer) {
  check_updates(true, false);
}

void on_about_action(GSimpleAction*, GVariant*, gpointer) {
  show_about_dialog();
}

void on_status_close_clicked(GtkButton*, gpointer) {
  gtk_revealer_set_reveal_child(g_state.status_bar, FALSE);
}

// selection drives the detail pane in place (mac split-view behavior);
// row-activated no longer opens a modal between the list and the detail
void on_selection_changed(GtkListBox*, gpointer) {
  GtkListBoxRow* row = gtk_list_box_get_selected_row(g_state.device_list);
  if (row != nullptr) {
    auto const* id = static_cast<char const*>(
        g_object_get_data(G_OBJECT(row), "device-id"));
    if (id != nullptr) g_state.selected_id = id;
  }
  update_action_sensitivity();
  render_detail_pane();
}

void on_edit_clicked(GtkButton*, gpointer) {
  std::string const id = resolved_selection_id();
  if (!id.empty()) open_device_form(true, id);
}

void on_delete_clicked(GtkButton*, gpointer) {
  std::string const id = resolved_selection_id();
  if (!id.empty()) show_delete_confirm(id);
}

// context-menu targets (row right-click); the parameter carries the id
void on_ctx_edit_action(GSimpleAction*, GVariant* parameter, gpointer) {
  open_device_form(true, g_variant_get_string(parameter, nullptr));
}

void on_ctx_delete_action(GSimpleAction*, GVariant* parameter, gpointer) {
  show_delete_confirm(g_variant_get_string(parameter, nullptr));
}

void on_activate(GtkApplication* app, gpointer) {
  if (g_state.window != nullptr) {
    gtk_window_present(g_state.window);
    return;
  }
  g_state.app = app;

  auto* window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Payback");
  gtk_window_set_default_size(GTK_WINDOW(window), 1020, 680);
  g_state.window = GTK_WINDOW(window);

  // Help-menu actions (backed by the header menu button built below).
  auto* check_updates_action = g_simple_action_new("check-updates", nullptr);
  g_signal_connect(check_updates_action, "activate",
                   G_CALLBACK(on_check_updates_action), nullptr);
  g_action_map_add_action(G_ACTION_MAP(window),
                          G_ACTION(check_updates_action));
  g_object_unref(check_updates_action);
  auto* about_action = g_simple_action_new("about", nullptr);
  g_signal_connect(about_action, "activate", G_CALLBACK(on_about_action),
                   nullptr);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(about_action));
  g_object_unref(about_action);
  // row context menu targets; the parameter carries the device id so the
  // menu acts on the row it was opened from
  auto* ctx_edit_action =
      g_simple_action_new("ctx-edit", G_VARIANT_TYPE_STRING);
  g_signal_connect(ctx_edit_action, "activate",
                   G_CALLBACK(on_ctx_edit_action), nullptr);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(ctx_edit_action));
  g_object_unref(ctx_edit_action);
  auto* ctx_delete_action =
      g_simple_action_new("ctx-delete", G_VARIANT_TYPE_STRING);
  g_signal_connect(ctx_delete_action, "activate",
                   G_CALLBACK(on_ctx_delete_action), nullptr);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(ctx_delete_action));
  g_object_unref(ctx_delete_action);

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(root, "payback-paper");
  // same floor as the macOS host; the child's minimum propagates to the
  // toplevel so the window cannot be shrunk into a broken layout
  gtk_widget_set_size_request(root, 980, 600);
  gtk_window_set_child(GTK_WINDOW(window), root);

  auto* header = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(window), header);

  auto* sort_box = gtk_combo_box_text_new();
  // items are placeholders; apply_language fills them from strings
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort_box), "-");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort_box), "-");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort_box), "-");
  g_signal_connect(sort_box, "changed", G_CALLBACK(on_sort_changed), nullptr);
  g_state.sort_box = GTK_COMBO_BOX_TEXT(sort_box);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), sort_box);

  auto* add_button = gtk_button_new_with_label("＋");
  gtk_widget_set_sensitive(add_button, FALSE);
  g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_clicked), nullptr);
  g_state.add_button = GTK_BUTTON(add_button);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), add_button);

  // the detail pane itself carries zero buttons — its actions live here,
  // sensitive only while a device is selected (mac toolbar parity)
  auto* edit_button = gtk_button_new_with_label("✏️");
  gtk_widget_set_sensitive(edit_button, FALSE);
  g_signal_connect(edit_button, "clicked", G_CALLBACK(on_edit_clicked),
                   nullptr);
  g_state.edit_button = GTK_BUTTON(edit_button);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), edit_button);

  auto* delete_button = gtk_button_new_with_label("🗑️");
  gtk_widget_set_sensitive(delete_button, FALSE);
  g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_clicked),
                   nullptr);
  g_state.delete_button = GTK_BUTTON(delete_button);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), delete_button);

  // Family menu convention: Check for Updates… and About live together in
  // the Help menu instead of as always-visible header buttons. Labels are
  // placeholders; apply_language refills them from strings.
  auto* help_menu = g_menu_new();
  g_menu_append(help_menu, "Check for Updates…", "win.check-updates");
  g_menu_append(help_menu, "About Payback", "win.about");
  g_state.help_menu = help_menu;
  auto* help_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(help_button),
                                "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(help_button),
                                 G_MENU_MODEL(help_menu));
  g_state.help_menu_button = GTK_MENU_BUTTON(help_button);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), help_button);
  g_object_unref(help_menu);

  auto* language_box = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(language_box), "🌐");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(language_box), "中文");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(language_box), "English");
  gtk_combo_box_set_active(GTK_COMBO_BOX(language_box),
                           g_language_override < 0 ? 0
                                                   : g_language_override + 1);
  g_signal_connect(language_box, "changed",
                   G_CALLBACK(on_language_changed), nullptr);
  g_state.language_box = GTK_COMBO_BOX_TEXT(language_box);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), language_box);

  // settings: the Data & Sync dialog (issue #9 M1)
  auto* settings_button = gtk_button_new_with_label("⚙️");
  g_signal_connect(settings_button, "clicked",
                   G_CALLBACK(on_settings_clicked), nullptr);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), settings_button);

  // split layout (issue #8): horizontal GtkPaned — sidebar on the left,
  // detail pane on the right, divider draggable like mac's NavigationSplitView
  auto* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  g_state.split = GTK_PANED(paned);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_box_append(GTK_BOX(root), paned);

  // ----- sidebar: summary + quip pinned on top, device list below
  auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(sidebar, 300, -1);

  // summary strip: 2×2 so a narrow window reflows the tiles instead of
  // clipping them off the right edge (matches the compact grid the macOS
  // sidebar uses)
  auto* summary = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(summary), 24);
  gtk_grid_set_row_spacing(GTK_GRID(summary), 8);
  gtk_grid_set_column_homogeneous(GTK_GRID(summary), TRUE);
  gtk_widget_set_margin_top(summary, 14);
  gtk_widget_set_margin_bottom(summary, 2);
  gtk_widget_set_margin_start(summary, 16);
  gtk_widget_set_margin_end(summary, 16);
  gtk_grid_attach(GTK_GRID(summary),
                  summary_tile(&g_state.total_spent, &g_state.total_spent_label),
                  0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(summary),
                  summary_tile(&g_state.earned_back, &g_state.earned_back_label),
                  1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(summary),
                  summary_tile(&g_state.overall_daily,
                               &g_state.overall_daily_label),
                  0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(summary),
                  summary_tile(&g_state.device_count, &g_state.device_count_label),
                  1, 1, 1, 1);
  gtk_box_append(GTK_BOX(sidebar), summary);

  auto* quip = gtk_label_new("");
  gtk_widget_set_halign(quip, GTK_ALIGN_START);
  gtk_widget_set_margin_start(quip, 20);
  gtk_widget_set_margin_bottom(quip, 6);
  gtk_widget_add_css_class(quip, "payback-quip");
  g_state.quip = GTK_LABEL(quip);
  gtk_box_append(GTK_BOX(sidebar), quip);

  auto* list_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(list_scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* list = gtk_list_box_new();
  // single selection drives the detail pane; no modal in between
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(list, "payback-list");
  gtk_widget_set_margin_start(list, 12);
  gtk_widget_set_margin_end(list, 12);
  gtk_widget_set_margin_bottom(list, 16);
  g_state.device_list = GTK_LIST_BOX(list);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scrolled), list);
  g_signal_connect(list, "selected-rows-changed",
                   G_CALLBACK(on_selection_changed), nullptr);
  gtk_box_append(GTK_BOX(sidebar), list_scrolled);

  gtk_paned_set_start_child(GTK_PANED(paned), sidebar);
  // mac's ideal column width; the user can drag from there
  gtk_paned_set_position(GTK_PANED(paned), 330);

  // ----- detail pane: rebuilt in place around the selected device
  auto* detail_scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* detail_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_top(detail_container, 24);
  gtk_widget_set_margin_bottom(detail_container, 24);
  gtk_widget_set_margin_start(detail_container, 24);
  gtk_widget_set_margin_end(detail_container, 24);
  g_state.detail_container = GTK_BOX(detail_container);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detail_scrolled),
                                detail_container);
  gtk_paned_set_end_child(GTK_PANED(paned), detail_scrolled);

  // status text doubles as the update-failure surface; it spans the window
  // under both panes
  auto* status_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(status_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_revealer_set_reveal_child(GTK_REVEALER(status_revealer), FALSE);
  auto* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* status_label = gtk_label_new("");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_widget_set_hexpand(status_label, TRUE);
  // Status text doubles as the update-failure surface; making it selectable
  // lets a bug report start from the exact error string.
  gtk_label_set_selectable(GTK_LABEL(status_label), TRUE);
  g_state.status_label = GTK_LABEL(status_label);
  auto* status_close = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(status_close, "flat");
  g_signal_connect(status_close, "clicked",
                   G_CALLBACK(on_status_close_clicked), nullptr);
  gtk_box_append(GTK_BOX(status_box), status_label);
  gtk_box_append(GTK_BOX(status_box), status_close);
  gtk_revealer_set_child(GTK_REVEALER(status_revealer), status_box);
  g_state.status_bar = GTK_REVEALER(status_revealer);
  gtk_box_append(GTK_BOX(root), status_revealer);

  // empty state replaces the split entirely while no devices exist (mac
  // emptyState parity)
  auto* empty_state = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_halign(empty_state, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty_state, GTK_ALIGN_CENTER);
  auto* empty_emoji = gtk_label_new("💰");
  gtk_widget_add_css_class(empty_emoji, "payback-empty-emoji");
  gtk_box_append(GTK_BOX(empty_state), empty_emoji);
  auto* empty_title = gtk_label_new("");
  gtk_widget_add_css_class(empty_title, "payback-empty-title");
  g_state.empty_title = GTK_LABEL(empty_title);
  gtk_box_append(GTK_BOX(empty_state), empty_title);
  auto* empty_hint = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(empty_hint), TRUE);
  gtk_label_set_justify(GTK_LABEL(empty_hint), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(empty_hint, GTK_ALIGN_CENTER);
  gtk_label_set_max_width_chars(GTK_LABEL(empty_hint), 38);
  gtk_widget_add_css_class(empty_hint, "payback-dim");
  g_state.empty_hint = GTK_LABEL(empty_hint);
  gtk_box_append(GTK_BOX(empty_state), empty_hint);
  auto* empty_add = gtk_button_new_with_label("＋");
  gtk_widget_set_sensitive(empty_add, FALSE);
  gtk_widget_add_css_class(empty_add, "suggested-action");
  g_signal_connect(empty_add, "clicked", G_CALLBACK(on_add_clicked), nullptr);
  g_state.empty_add_button = GTK_BUTTON(empty_add);
  gtk_box_append(GTK_BOX(empty_state), empty_add);
  gtk_widget_set_visible(empty_state, FALSE);
  g_state.empty_state = GTK_BOX(empty_state);
  gtk_box_append(GTK_BOX(root), empty_state);

  apply_css();
  apply_language();
  gtk_window_present(GTK_WINDOW(window));
  g_state.quip_day = day_of_year();
  start_backend();
}

void on_shutdown(GApplication*, gpointer) {
  g_state.shutting_down.store(true, std::memory_order_release);
  stop_update_poll();
  if (g_state.sync_poll_source != 0) {
    g_source_remove(g_state.sync_poll_source);
    g_state.sync_poll_source = 0;
  }
  if (g_state.startup_thread.joinable()) {
    g_state.startup_thread.join();
  }
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  {
    std::lock_guard lock(g_state.startup_mutex);
    startup_backend = std::move(g_state.startup_backend);
  }
  if (startup_backend != nullptr) startup_backend->stop();
  if (g_state.backend != nullptr) g_state.backend->stop();
}

}  // namespace

int main(int argc, char** argv) {
  std::unique_ptr<rivet::system::SingleInstanceLease> lease;
  try {
    lease = std::make_unique<rivet::system::SingleInstanceLease>(
        rivet_app::kIdentifier);
    if (!lease->is_primary()) {
      (void)lease->forward_arguments(
          std::vector<std::string>(argv + 1, argv + argc));
      return 0;
    }
  } catch (...) {
    lease.reset();
  }
  (void)lease;

  auto* app =
      gtk_application_new(rivet_app::kIdentifier, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), nullptr);
  int const status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}

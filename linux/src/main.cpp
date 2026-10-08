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
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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

struct Document {
  std::string currency = "CNY";
  bool update_auto_check = true;
  std::vector<DeviceRow> devices;
  gint64 total_spent_minor{0};
  double earned_total_minor{0.0};
  double avg_cost_per_day_minor{0.0};
  int device_count{0};
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

  GtkComboBoxText* sort_box{nullptr};
  GtkComboBoxText* language_box{nullptr};
  GtkButton* add_button{nullptr};
  GtkButton* check_updates_button{nullptr};
  GtkButton* pro_button{nullptr};
  GtkListBox* device_list{nullptr};

  // update download progress; owned here so poll callbacks stay simple
  GtkWindow* update_dialog{nullptr};
  GtkProgressBar* update_bar{nullptr};
  guint poll_source{0};

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::unique_ptr<rivet_app::API> api;

  std::mutex startup_mutex;
  std::thread startup_thread;
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  std::string startup_error;
  std::atomic<bool> shutting_down{false};

  Document document;
  int sort_mode{0};  // 0 added, 1 daily cost, 2 payback progress
  int quip_day{1};
  bool auto_check_done{false};
  bool celebration_open{false};
};

AppState g_state;

// ------------------------------------------------------------ forward decls

void apply_language();
void load_all_async();
void delete_device_async(std::string const& id);
void open_detail_dialog(std::string const& id);
void open_device_form(bool update, std::string const& id);
void show_delete_confirm(std::string const& id);
void check_updates(bool force, bool silent);
void open_activation_dialog();
void run_daily_digest();
void maybe_celebrate();

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

// -------------------------------------------------------------- css theming

void apply_css() {
  auto* provider = gtk_css_provider_new();
  // Warm paper light surfaces; the dark preference flips GTK palettes and
  // this stylesheet swaps card/text colors (docs/design.md tokens).
  std::string const css = R"CSS(
.payback-paper { background: #FAF7F2; }
.payback-card { background: #FFFFFF; border: 1px solid #E8E2D9; border-radius: 12px; padding: 12px 14px; }
.payback-earned { color: #1F7A33; }
.payback-accent { color: #C15F3C; }
.payback-dim { opacity: 0.65; font-size: 12px; }
.payback-meta { opacity: 0.65; font-size: 12px; }
.payback-cost { font-size: 20px; font-weight: bold; }
.payback-cost-label { opacity: 0.65; font-size: 11px; }
.payback-name { font-size: 16px; font-weight: 600; }
.payback-badges { font-size: 12px; }
.payback-quip { opacity: 0.65; font-size: 12px; }
)CSS";
  gtk_css_provider_load_from_string(provider, css.c_str());
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  // follow the system's dark preference
  GtkSettings* settings = gtk_settings_get_default();
  gchar* theme_name = nullptr;
  g_object_get(settings, "gtk-theme-name", &theme_name, nullptr);
  bool const dark = theme_name != nullptr &&
                    std::string(theme_name).find("ark") != std::string::npos;
  g_free(theme_name);
  g_object_set(settings, "gtk-application-prefer-dark-theme",
               dark ? TRUE : FALSE, nullptr);
}

// -------------------------------------------------------------- rendering

void render_summary() {
  Document const& d = g_state.document;
  gtk_label_set_text(
      g_state.total_spent,
      money(static_cast<double>(d.total_spent_minor), d.currency).c_str());
  gtk_label_set_text(g_state.earned_back,
                     money(d.earned_total_minor, d.currency).c_str());
  gtk_label_set_text(g_state.overall_daily,
                     money(d.avg_cost_per_day_minor, d.currency).c_str());
  gtk_label_set_text(g_state.device_count,
                     std::to_string(d.device_count).c_str());
}

GtkWidget* device_card(DeviceRow const& row) {
  payback::linux_strings::Strings const s = strings();

  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

  auto* icon = gtk_label_new(row.icon.c_str());
  gtk_widget_set_size_request(icon, 40, 40);
  gtk_box_append(GTK_BOX(box), icon);

  auto* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
  auto* name = gtk_label_new(row.name.c_str());
  gtk_widget_set_halign(name, GTK_ALIGN_START);
  gtk_widget_add_css_class(name, "payback-name");
  gtk_box_append(GTK_BOX(left), name);

  std::string const subtitle =
      s.held_for() + " " + std::to_string(row.computed.days_held) + " " +
      s.days() + " · " +
      money(static_cast<double>(row.price_minor), row.currency);
  auto* meta = gtk_label_new(subtitle.c_str());
  gtk_widget_set_halign(meta, GTK_ALIGN_START);
  gtk_widget_add_css_class(meta, "payback-meta");
  gtk_box_append(GTK_BOX(left), meta);

  std::string badges;
  for (Milestone const& milestone : row.computed.milestones) {
    if (milestone.achieved) badges += milestone_emoji(milestone.key) + " ";
  }
  if (!badges.empty()) {
    auto* badge = gtk_label_new(badges.c_str());
    gtk_widget_set_halign(badge, GTK_ALIGN_START);
    gtk_widget_add_css_class(badge, "payback-badges");
    gtk_box_append(GTK_BOX(left), badge);
  }
  gtk_box_append(GTK_BOX(box), left);

  auto* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(right, GTK_ALIGN_END);
  gtk_widget_set_hexpand(right, TRUE);
  auto* cost = gtk_label_new(
      money(row.computed.cost_per_day_minor, row.currency).c_str());
  gtk_widget_set_halign(cost, GTK_ALIGN_END);
  gtk_widget_add_css_class(cost, "payback-cost");
  if (row.computed.paid_back) {
    gtk_widget_add_css_class(cost, "payback-earned");
  }
  gtk_box_append(GTK_BOX(right), cost);
  auto* cost_label =
      gtk_label_new((s.daily_cost() + " " + s.per_day()).c_str());
  gtk_widget_set_halign(cost_label, GTK_ALIGN_END);
  gtk_widget_add_css_class(cost_label, "payback-cost-label");
  gtk_box_append(GTK_BOX(right), cost_label);
  gtk_box_append(GTK_BOX(box), right);

  return box;
}

void render_device_list() {
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
    GtkWidget* card = device_card(row_data);
    auto* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    g_object_set_data_full(G_OBJECT(row), "device-id",
                           g_strdup(row_data.id.c_str()), g_free);
    gtk_list_box_append(g_state.device_list, row);
  }
}

void render_document_from_cache() {
  render_summary();
  render_device_list();
  apply_quip();
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

void save_device_payload(std::string const& json, bool update,
                         std::string const& id) {
  if (g_state.api == nullptr) return;
  auto done = [](pb::Unpacked<rivet::Bytes>& r) {
    if (!r.ok) {
      show_error(r.error);
      return;
    }
    load_all_async();
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
  (void)id;
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

// ------------------------------------------------------- dialogs: detail

void open_detail_dialog(std::string const& id) {
  DeviceRow const* found = nullptr;
  for (DeviceRow const& row : g_state.document.devices) {
    if (row.id == id) found = &row;
  }
  if (found == nullptr) return;
  DeviceRow const row = *found;  // copy: the document may reload underneath

  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_dialog_new_with_buttons(
      row.name.c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.close().c_str(), GTK_RESPONSE_CLOSE,
      s.remove().c_str(), GTK_RESPONSE_REJECT,
      s.edit().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 8);
  gtk_widget_set_margin_bottom(content, 8);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_box_append(GTK_BOX(area), content);

  auto* title = gtk_label_new((row.icon + " " + row.name).c_str());
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_add_css_class(title, "payback-name");
  gtk_box_append(GTK_BOX(content), title);

  std::string const facts =
      money(static_cast<double>(row.price_minor), row.currency) + " · " +
      row.purchase_date + " · " + s.held_for() + " " +
      std::to_string(row.computed.days_held) + " " + s.days();
  auto* facts_label = gtk_label_new(facts.c_str());
  gtk_widget_set_halign(facts_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(facts_label, "payback-meta");
  gtk_label_set_wrap(GTK_LABEL(facts_label), TRUE);
  gtk_box_append(GTK_BOX(content), facts_label);

  if (row.computed.willing_set) {
    std::string line = s.payback_progress() + ": ";
    line += row.computed.paid_back
                ? "✓ " + s.paid_back()
                : std::to_string(static_cast<int>(
                      row.computed.payback_progress * 100 + 0.5)) + "%";
    if (!row.computed.paid_back && !row.computed.payback_eta.empty()) {
      line += " · " + s.payback_eta() + " " + row.computed.payback_eta;
    }
    auto* progress = gtk_label_new(line.c_str());
    gtk_widget_set_halign(progress, GTK_ALIGN_START);
    gtk_widget_add_css_class(progress, "payback-accent");
    gtk_box_append(GTK_BOX(content), progress);
  }

  auto* ladder_title = gtk_label_new(s.milestones().c_str());
  gtk_widget_set_halign(ladder_title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(content), ladder_title);

  for (Milestone const& milestone : row.computed.milestones) {
    std::string const line_text = milestone_emoji(milestone.key) + " " +
                                  milestone_label(milestone.key) +
                                  (milestone.achieved ? "  ✓" : "");
    auto* item = gtk_label_new(line_text.c_str());
    gtk_widget_set_halign(item, GTK_ALIGN_START);
    if (!milestone.achieved) gtk_widget_set_opacity(item, 0.4);
    gtk_box_append(GTK_BOX(content), item);
  }

  auto* id_copy = new std::string(id);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer user_data) {
        std::unique_ptr<std::string> id(static_cast<std::string*>(user_data));
        std::string const action = response == GTK_RESPONSE_ACCEPT
                                       ? "edit"
                                       : response == GTK_RESPONSE_REJECT
                                             ? "delete"
                                             : "";
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (action == "edit") {
          pb::post_to_main<std::string>(
              [](std::string& p) { open_device_form(true, p); }, *id);
        } else if (action == "delete") {
          pb::post_to_main<std::string>(
              [](std::string& p) { show_delete_confirm(p); }, *id);
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
  std::string currency;
  bool update{false};
  std::string id;
};

std::string const kCategories[] = {"computer", "phone",     "tablet",
                                   "audio",    "camera",    "gaming",
                                   "appliance", "accessory", "other"};

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

void on_form_response(GtkDialog* dialog, int response, gpointer user_data) {
  std::unique_ptr<FormContext> ctx(static_cast<FormContext*>(user_data));
  bool const accepted = response == GTK_RESPONSE_ACCEPT;
  FormContext const* snapshot = new FormContext(*ctx);
  gtk_window_destroy(GTK_WINDOW(dialog));
  if (!accepted) {
    delete snapshot;
    return;
  }
  pb::post_to_main<FormContext>(
      [](FormContext& c) {
        auto const text_of = [](GtkEntry* entry) {
          return entry == nullptr
                     ? std::string()
                     : std::string(gtk_editable_get_text(GTK_EDITABLE(entry)));
        };
        std::string const name = text_of(c.name);
        std::string const icon = text_of(c.icon);
        gint const category_index = gtk_combo_box_get_active(
            GTK_COMBO_BOX(c.category));
        std::string const category =
            (category_index >= 0 &&
             category_index < static_cast<gint>(std::size(kCategories)))
                ? kCategories[category_index]
                : std::string("other");
        std::string const price_text = text_of(c.price);
        std::string const date = text_of(c.date_entry);
        bool const willing_on =
            c.willing_on != nullptr && gtk_switch_get_active(c.willing_on);
        std::string const willing_text = text_of(c.willing);
        std::string const notes = text_of(c.notes);

        double const price_value = std::wcstod(
            std::wstring(price_text.begin(), price_text.end()).c_str(),
            nullptr);
        auto const price_minor =
            static_cast<gint64>(price_value * 100.0 + 0.5);
        double const willing_value = std::wcstod(
            std::wstring(willing_text.begin(), willing_text.end()).c_str(),
            nullptr);
        auto const willing_minor =
            static_cast<gint64>(willing_value * 100.0 + 0.5);

        std::string const json = build_json([&](JsonBuilder* b) {
          builder_string(b, "name", name);
          builder_string(b, "icon", icon.empty() ? "📦" : icon);
          builder_string(b, "category", category);
          builder_int(b, "priceMinor", price_minor);
          builder_string(b, "currency", c.currency);
          builder_string(b, "purchaseDate", date);
          if (willing_on && willing_minor > 0) {
            builder_int(b, "willingPerDayMinor", willing_minor);
          } else {
            builder_null(b, "willingPerDayMinor");
          }
          builder_string(b, "notes", notes);
          if (c.update) builder_string(b, "id", c.id);
        });
        save_device_payload(json, c.update, c.id);
      },
      *snapshot);
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
  int selected = 8;
  for (int i = 0; i < static_cast<int>(std::size(kCategories)); ++i) {
    // localized display name; the id travels to the backend
    std::string const label_text = [&s, i] {
      switch (i) {
        case 0: return s.category_computer();
        case 1: return s.category_phone();
        case 2: return s.category_tablet();
        case 3: return s.category_audio();
        case 4: return s.category_camera();
        case 5: return s.category_gaming();
        case 6: return s.category_appliance();
        case 7: return s.category_accessory();
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

  auto* hint = gtk_label_new(s.willing_hint().c_str());
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_widget_add_css_class(hint, "payback-dim");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(panel), hint);

  auto* notes = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(notes), existing.notes.c_str());
  context->notes = GTK_ENTRY(notes);
  form_row(panel, s.notes().c_str(), notes);

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

// ------------------------------------------------- license + emotional layer

void open_activation_dialog() {
  payback::linux_strings::Strings const s = strings();
  auto* dialog = gtk_dialog_new_with_buttons(
      s.activate_pro_menu().c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      s.cancel().c_str(), GTK_RESPONSE_CANCEL,
      s.activate_button().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 8);
  gtk_widget_set_margin_bottom(content, 8);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_box_append(GTK_BOX(area), content);

  auto* key = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(key), "");
  gtk_entry_set_placeholder_text(GTK_ENTRY(key), "PB1.…");
  gtk_box_append(GTK_BOX(content), key);

  auto* hint = gtk_label_new(s.license_hint().c_str());
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_widget_add_css_class(hint, "payback-dim");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(content), hint);

  auto* key_copy = new GtkEntry*(GTK_ENTRY(key));
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer user_data) {
        auto* entry = static_cast<GtkEntry**>(user_data);
        std::string const key_text =
            response == GTK_RESPONSE_ACCEPT
                ? std::string(gtk_editable_get_text(GTK_EDITABLE(*entry)))
                : std::string();
        delete entry;
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (!key_text.empty()) {
          pb::post_to_main<std::string>(
              [](std::string& p) {
                if (g_state.api == nullptr) return;
                std::string const json = build_json(
                    [&](JsonBuilder* b) { builder_string(b, "key", p); });
                (void)g_state.api->activate_license_async(
                    to_bytes(json),
                    [](rivet_app::Result<rivet::Bytes> result) {
                      auto unpacked = pb::unpack(result);
                      pb::post_to_main<pb::Unpacked<rivet::Bytes>>(
                          [](pb::Unpacked<rivet::Bytes>& r) {
                            if (!r.ok) {
                              show_error(
                                  strings().activation_failed() + ": " +
                                  r.error);
                              return;
                            }
                            show_success(strings().pro_active_title());
                            load_all_async();
                          },
                          std::move(unpacked));
                    });
              },
              key_text);
        }
      }),
      key_copy);
  gtk_window_present(GTK_WINDOW(dialog));
}

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
                replace_all_in(
                    body, "{earned}",
                    money(jdouble(root, "earnedTotalMinor"),
                          g_state.document.currency));
                std::string const best = jstr(root, "bestDeviceName");
                if (!best.empty()) {
                  std::string best_line = s.digest_body_best();
                  replace_all_in(best_line, "{name}", best);
                  replace_all_in(
                      best_line, "{cost}",
                      money(jdouble(root, "bestDeviceCostPerDayMinor"),
                            g_state.document.currency));
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
  gtk_button_set_label(g_state.check_updates_button,
                       s.check_updates_menu().c_str());
  gtk_button_set_label(g_state.pro_button,
                       ("✦ " + s.activate_pro_menu()).c_str());
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

void on_check_updates_clicked(GtkButton*, gpointer) {
  check_updates(true, false);
}

void on_pro_clicked(GtkButton*, gpointer) { open_activation_dialog(); }

void on_status_close_clicked(GtkButton*, gpointer) {
  gtk_revealer_set_reveal_child(g_state.status_bar, FALSE);
}

void on_row_activated(GtkListBox*, GtkListBoxRow* row, gpointer) {
  if (row == nullptr) return;
  auto const* id = static_cast<char const*>(
      g_object_get_data(G_OBJECT(row), "device-id"));
  if (id != nullptr) open_detail_dialog(id);
}

void on_activate(GtkApplication* app, gpointer) {
  if (g_state.window != nullptr) {
    gtk_window_present(g_state.window);
    return;
  }
  g_state.app = app;

  auto* window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Payback");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 640);
  g_state.window = GTK_WINDOW(window);

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(root, "payback-paper");
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

  auto* check_button = gtk_button_new_with_label("⟳");
  g_signal_connect(check_button, "clicked",
                   G_CALLBACK(on_check_updates_clicked), nullptr);
  g_state.check_updates_button = GTK_BUTTON(check_button);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), check_button);

  auto* pro_button = gtk_button_new_with_label("✦");
  g_signal_connect(pro_button, "clicked", G_CALLBACK(on_pro_clicked), nullptr);
  g_state.pro_button = GTK_BUTTON(pro_button);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), pro_button);

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

  // summary strip
  auto* summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
  gtk_widget_set_margin_top(summary, 14);
  gtk_widget_set_margin_bottom(summary, 2);
  gtk_widget_set_margin_start(summary, 20);
  gtk_widget_set_margin_end(summary, 20);
  gtk_box_append(GTK_BOX(summary), summary_tile(&g_state.total_spent,
                                                &g_state.total_spent_label));
  gtk_box_append(GTK_BOX(summary), summary_tile(&g_state.earned_back,
                                                &g_state.earned_back_label));
  gtk_box_append(GTK_BOX(summary), summary_tile(&g_state.overall_daily,
                                                &g_state.overall_daily_label));
  gtk_box_append(GTK_BOX(summary), summary_tile(&g_state.device_count,
                                                &g_state.device_count_label));
  gtk_box_append(GTK_BOX(root), summary);

  auto* quip = gtk_label_new("");
  gtk_widget_set_halign(quip, GTK_ALIGN_START);
  gtk_widget_set_margin_start(quip, 24);
  gtk_widget_set_margin_bottom(quip, 6);
  gtk_widget_add_css_class(quip, "payback-quip");
  g_state.quip = GTK_LABEL(quip);
  gtk_box_append(GTK_BOX(root), quip);

  auto* status_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(status_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_revealer_set_reveal_child(GTK_REVEALER(status_revealer), FALSE);
  auto* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* status_label = gtk_label_new("");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_widget_set_hexpand(status_label, TRUE);
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

  auto* scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_set_margin_start(list, 20);
  gtk_widget_set_margin_end(list, 20);
  gtk_widget_set_margin_bottom(list, 16);
  g_state.device_list = GTK_LIST_BOX(list);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
  g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated),
                   nullptr);
  gtk_box_append(GTK_BOX(root), scrolled);

  apply_css();
  apply_language();
  gtk_window_present(GTK_WINDOW(window));
  g_state.quip_day = day_of_year();
  start_backend();
}

void on_shutdown(GApplication*, gpointer) {
  g_state.shutting_down.store(true, std::memory_order_release);
  stop_update_poll();
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

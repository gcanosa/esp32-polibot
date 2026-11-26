#include "config_manager.h"
#include <esp_spiffs.h>
#include <esp_log.h>
#include <cstdio>
#include <sys/stat.h>

static const char* TAG = "CONFIG";

static const char* DEFAULT_CONFIG_STR = R"JSON({
  "device_name": "PoliBOT-Sentry",
  "wifi": {
    "ssid": "YOUR_WIFI_SSID",
    "password": "YOUR_WIFI_PASSWORD",
    "ap_fallback": true,
    "ap_ssid": "PoliBOT-Setup",
    "ap_password": "polibotpassword"
  },
  "irc": {
    "cmd_prefix": "!",
    "reconnect_delay": 5,
    "max_reconnect_delay": 60,
    "ping_timeout": 120,
    "networks": [
      {
        "name": "SekurNet",
        "host": "irc.SekurNet.org",
        "port": 6697,
        "ssl": true,
        "ssl_verify": true,
        "nick": "PoliBOT",
        "ident": "polibot",
        "realname": "ESP32-S3 Guardian Sentry",
        "nickserv_password": "",
        "channels": ["#polibot"]
      }
    ]
  },
  "guardian": {
    "anti_flood": {
      "enabled": true,
      "max_lines": 4,
      "time_window": 4,
      "action": "kick",
      "kick_reason": "Flood protection triggered"
    },
    "anti_caps": {
      "enabled": false,
      "min_length": 15,
      "caps_threshold": 75,
      "action": "warn",
      "kick_reason": "Excessive caps detected"
    },
    "anti_badwords": {
      "enabled": true,
      "words": ["badword1", "spammer", "attack"],
      "action": "kick",
      "kick_reason": "Disallowed word"
    },
    "auto_op_admins": true,
    "channel_settings": {
      "#polibot": {
        "dynamic_limit": {
          "enabled": true,
          "offset": 3,
          "min_limit": 5
        },
        "entrymsg": {
          "enabled": true,
          "notice": true,
          "message": "Welcome to #polibot! Channel guarded by PoliBOT. Please follow the rules."
        },
        "badwords": ["badword1", "spammer"],
        "lockdown": false
      }
    }
  },
  "users": [
    {
      "username": "admin",
      "salt": "b991f51f",
      "password_hash": "4b882ab6cbdd3526d216e6344e790f42239ea570943539fd29844014b37773b5",
      "global_role": "owner",
      "channel_roles": {
        "#polibot": "owner"
      },
      "hostmasks": ["*!*@*"]
    }
  ],
  "web": {
    "port": 80
  },
  "ntp": {
    "enabled": true,
    "server": "pool.ntp.org",
    "timezone": "CET-1CEST,M3.5.0,M10.5.0/3",
    "location": "Europe/Madrid"
  }
})JSON";

ConfigManager::ConfigManager(const std::string& filepath)
    : filepath_(filepath), root_(nullptr) {}

ConfigManager::~ConfigManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (root_) {
        cJSON_Delete(root_);
        root_ = nullptr;
    }
}

bool ConfigManager::init_spiffs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount or format SPIFFS (%s)", esp_err_to_name(ret));
        return false;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS Partition size: total: %d, used: %d", total, used);
    return true;
}

cJSON* ConfigManager::create_default_config() {
    return cJSON_Parse(DEFAULT_CONFIG_STR);
}

static cJSON* read_json_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return nullptr;
    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return cJSON_Parse(text.c_str()); // nullptr for a truncated file
}

// Files written by single-network versions: expose those settings as the first entry of irc.networks.
// The old keys are left in place, so rolling back to older firmware still finds its settings.
static void upgrade_config(cJSON* root) {
    cJSON* irc = cJSON_GetObjectItemCaseSensitive(root, "irc");
    if (!cJSON_IsObject(irc) || cJSON_GetObjectItemCaseSensitive(irc, "networks")) return;
    cJSON* net = cJSON_CreateObject();
    cJSON_AddStringToObject(net, "name", "default");
    for (const char* key : {"host", "port", "ssl", "ssl_verify", "nick", "ident", "realname", "nickserv_password", "channels"}) {
        cJSON* value = cJSON_GetObjectItemCaseSensitive(irc, key);
        if (value) cJSON_AddItemToObject(net, key, cJSON_Duplicate(value, 1));
    }
    if (!cJSON_GetObjectItemCaseSensitive(net, "host")) cJSON_AddStringToObject(net, "host", "irc.SekurNet.org");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(irc, "networks"), net);
}

static thread_local bool t_defers_writes = false;

void ConfigManager::defer_writes_on_this_task() { t_defers_writes = true; }

bool ConfigManager::load() {
    std::lock_guard<std::mutex> writer(write_mutex_); // not while the file is being replaced
    std::lock_guard<std::mutex> lock(mutex_);
    // reading the file again must not undo a change that is still on its way into it
    if (pending_ && !write_both_locked()) return false;
    if (root_) {
        cJSON_Delete(root_);
        root_ = nullptr;
    }

    const std::string tmp = filepath_ + ".tmp";
    root_ = read_json_file(filepath_);
    if (root_) {
        upgrade_config(root_);
        ESP_LOGI(TAG, "Configuration loaded successfully from %s", filepath_.c_str());
        return true;
    }

    root_ = read_json_file(tmp);
    if (root_) {
        // Power failed during a save: the temporary file is the newest complete copy
        remove(filepath_.c_str());
        rename(tmp.c_str(), filepath_.c_str());
        ESP_LOGW(TAG, "Recovered configuration from an interrupted save.");
        upgrade_config(root_);
        return true;
    }

    root_ = create_default_config();
    struct stat st;
    if (stat(filepath_.c_str(), &st) == 0) {
        // Leave the damaged file alone so it can still be inspected
        ESP_LOGE(TAG, "%s is unreadable. Running on defaults.", filepath_.c_str());
    } else {
        ESP_LOGW(TAG, "Config file not found (%s). Creating default config...", filepath_.c_str());
        write_both_locked();
    }
    return (root_ != nullptr);
}

bool ConfigManager::save() {
    if (t_defers_writes && writer_) {
        pending_ = true;
        xTaskNotifyGive(writer_);
        return true;
    }
    std::lock_guard<std::mutex> writer(write_mutex_);
    return write_now();
}

bool ConfigManager::flush() {
    std::lock_guard<std::mutex> writer(write_mutex_); // a write under way finishes first
    return !pending_ || write_now();
}

bool ConfigManager::write_now() {
    char* printed;
    {
        // readers wait only while the text is made, not while it is written
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = false; // what changes from here on needs another write
        printed = root_ ? cJSON_Print(root_) : nullptr;
    }
    const bool ok = printed && write_file(printed);
    free(printed);
    if (!ok) pending_ = true;
    return ok;
}

bool ConfigManager::write_both_locked() {
    char* printed = root_ ? cJSON_Print(root_) : nullptr;
    const bool ok = printed && write_file(printed);
    free(printed);
    pending_ = !ok;
    return ok;
}

// Never truncate the only good copy: write a sibling file, then swap it in.
bool ConfigManager::write_file(const char* text) {
    const std::string tmp = filepath_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    bool ok = false;
    if (f) {
        ok = fputs(text, f) >= 0;
        ok = (fclose(f) == 0) && ok;
    }

    if (ok) {
        remove(filepath_.c_str()); // SPIFFS rename() refuses to overwrite
        ok = rename(tmp.c_str(), filepath_.c_str()) == 0;
    }
    if (ok) {
        ESP_LOGI(TAG, "Configuration saved to %s", filepath_.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to save %s", filepath_.c_str());
    }
    return ok;
}

bool ConfigManager::rehash() {
    bool ok = load();
    if (!ok) return false;

    std::vector<RehashCallback> callbacks_copy;
    cJSON* root_copy = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy = listeners_;
        root_copy = cJSON_Duplicate(root_, 1);
    }

    for (auto& cb : callbacks_copy) {
        if (cb) cb(root_copy);
    }
    if (root_copy) cJSON_Delete(root_copy);

    ESP_LOGI(TAG, "Rehash completed across all listeners.");
    return true;
}

void ConfigManager::register_rehash_listener(RehashCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(cb);
}

cJSON* ConfigManager::get_root() {
    std::lock_guard<std::mutex> lock(mutex_);
    return root_ ? cJSON_Duplicate(root_, 1) : nullptr;
}

cJSON* ConfigManager::get_item(const char* key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!root_) return nullptr;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root_, key);
    return item ? cJSON_Duplicate(item, 1) : nullptr;
}

std::string ConfigManager::get_string(const char* parent_key, const char* child_key, const std::string& default_val) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!root_) return default_val;
    cJSON* parent = cJSON_GetObjectItemCaseSensitive(root_, parent_key);
    if (!parent) return default_val;
    cJSON* child = cJSON_GetObjectItemCaseSensitive(parent, child_key);
    if (child && cJSON_IsString(child) && child->valuestring) {
        return child->valuestring;
    }
    return default_val;
}

int ConfigManager::get_int(const char* parent_key, const char* child_key, int default_val) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!root_) return default_val;
    cJSON* parent = cJSON_GetObjectItemCaseSensitive(root_, parent_key);
    if (!parent) return default_val;
    cJSON* child = cJSON_GetObjectItemCaseSensitive(parent, child_key);
    if (child && cJSON_IsNumber(child)) {
        return child->valueint;
    }
    return default_val;
}

bool ConfigManager::get_bool(const char* parent_key, const char* child_key, bool default_val) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!root_) return default_val;
    cJSON* parent = cJSON_GetObjectItemCaseSensitive(root_, parent_key);
    if (!parent) return default_val;
    cJSON* child = cJSON_GetObjectItemCaseSensitive(parent, child_key);
    if (child && cJSON_IsBool(child)) {
        return cJSON_IsTrue(child);
    }
    return default_val;
}

bool ConfigManager::set_item(const char* key, cJSON* item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!root_ || !item) return false;
    while (cJSON_GetObjectItemCaseSensitive(root_, key)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root_, key);
    }
    cJSON_AddItemToObject(root_, key, item);
    return true;
}

bool ConfigManager::update(const std::function<bool(cJSON* root)>& change) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!root_ || !change(root_)) return false;
    }
    return save();
}

bool ConfigManager::replace_root(cJSON* next) {
    if (!next) return false;
    std::lock_guard<std::mutex> writer(write_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* previous = root_;
    const bool was_pending = pending_;
    root_ = next;
    if (!write_both_locked()) {
        root_ = previous;
        pending_ = was_pending;
        cJSON_Delete(next);
        return false;
    }
    cJSON_Delete(previous);
    return true;
}

bool ConfigManager::replace_from_json_string(const std::string& json_str) {
    cJSON* new_root = cJSON_Parse(json_str.c_str());
    if (!new_root) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (root_) cJSON_Delete(root_);
        root_ = new_root;
    }
    save();
    return true;
}

std::string ConfigManager::to_string(bool formatted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!root_) return "{}";
    char* s = formatted ? cJSON_Print(root_) : cJSON_PrintUnformatted(root_);
    if (!s) return "{}";
    std::string res(s);
    free(s);
    return res;
}

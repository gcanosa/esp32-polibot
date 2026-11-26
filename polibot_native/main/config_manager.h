#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class ConfigManager {
public:
    using RehashCallback = std::function<void(cJSON* root)>;

    explicit ConfigManager(const std::string& filepath = "/spiffs/config.json");
    ~ConfigManager();

    bool init_spiffs();
    bool load();
    bool save();
    bool rehash();

    // A task that must stay responsive (an IRC connection) never waits for the flash: its changes hold at
    // once, save() and update() report success, and the writer task is woken to write them with flush().
    static void defer_writes_on_this_task();
    void set_writer(TaskHandle_t writer) { writer_ = writer; }
    // Waits for a write under way and writes what is still waiting; false if that failed (it stays waiting)
    bool flush();

    void register_rehash_listener(RehashCallback cb);

    // Read access with cJSON duplication (caller owns returned pointer or copy)
    cJSON* get_root();
    cJSON* get_item(const char* key);
    std::string get_string(const char* parent_key, const char* child_key, const std::string& default_val = "");
    int get_int(const char* parent_key, const char* child_key, int default_val = 0);
    bool get_bool(const char* parent_key, const char* child_key, bool default_val = false);

    // Updates
    bool set_item(const char* key, cJSON* item);
    bool replace_from_json_string(const std::string& json_str);
    // Saves `next` as the whole configuration and takes ownership of it; on failure nothing changes
    bool replace_root(cJSON* next);
    // Edits the configuration in place and saves it; nothing is saved if `change` returns false
    bool update(const std::function<bool(cJSON* root)>& change);

    std::string to_string(bool formatted = false);

private:
    std::string filepath_;
    cJSON* root_;
    std::mutex mutex_;       // the settings in memory
    std::mutex write_mutex_; // the file: one writer at a time. Taken before mutex_, never while holding it.
    std::atomic<bool> pending_{false}; // a change has not reached the file
    std::atomic<TaskHandle_t> writer_{nullptr};
    std::vector<RehashCallback> listeners_;

    cJSON* create_default_config();
    bool write_now();                  // needs write_mutex_
    bool write_file(const char* text); // needs write_mutex_
    bool write_both_locked();          // needs both mutexes
};

extern ConfigManager g_config_mgr;

#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_desc.h>

struct OtaStatus {
    bool in_progress = false;
    int progress_percent = 0;
    size_t bytes_transferred = 0;
    size_t total_size = 0;
    std::string status_message = "Idle";
    std::string running_partition = "unknown";
    std::string next_partition = "unknown";
    std::string app_version = "1.0.0";
    std::string compile_time = "";
    bool success = false;
    bool requires_reboot = false;
};

class OtaManager {
public:
    OtaManager();
    ~OtaManager();

    void init();

    // Call once the new firmware has proven healthy; until then any reset rolls back.
    void confirm_running_image();

    // Partition & App details
    std::string get_running_partition_name();
    std::string get_next_partition_name();
    OtaStatus get_status();

    // Stream-based upload (from Web POST)
    esp_err_t begin_stream_update(size_t image_size = OTA_SIZE_UNKNOWN);
    esp_err_t write_stream_chunk(const char* data, size_t length);
    esp_err_t finish_stream_update();
    void abort_stream_update();

    // URL-based OTA (HTTP / HTTPS background download task)
    bool start_url_update(const std::string& url,
                          std::function<void(bool success, const std::string& msg)> on_complete = nullptr,
                          std::function<void(int percent, size_t read_bytes, size_t total_bytes)> on_progress = nullptr);

    bool is_in_progress() const { return in_progress_; }

private:
    std::mutex mutex_;
    std::atomic<bool> in_progress_{false};
    OtaStatus current_status_;

    // Stream OTA state
    esp_ota_handle_t ota_handle_ = 0;
    const esp_partition_t* update_partition_ = nullptr;
    size_t stream_bytes_written_ = 0;
    size_t stream_total_size_ = 0;

    // Background task parameters
    static void url_ota_task_entry(void* pv);
    void run_url_ota();

    std::string task_url_;
    std::function<void(bool, const std::string&)> task_on_complete_;
    std::function<void(int, size_t, size_t)> task_on_progress_;
};

extern OtaManager g_ota_mgr;

#include "ota_manager.h"
#include "log_buffer.h"
#include <esp_log.h>
#include <esp_system.h>
#include <esp_crt_bundle.h>
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

static const char* TAG = "OTA";

OtaManager g_ota_mgr;

OtaManager::OtaManager() {}

OtaManager::~OtaManager() {
    abort_stream_update();
}

void OtaManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Query running and target partitions
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);

    if (running) {
        current_status_.running_partition = running->label;
    }
    if (next) {
        current_status_.next_partition = next->label;
    }

    // 2. Query app version and build timestamp
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc) {
        current_status_.app_version = app_desc->version;
        current_status_.compile_time = std::string(app_desc->date) + " " + app_desc->time;
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "OTA Ready: Running '%s' (v%s, %s), Next Target '%s'",
             current_status_.running_partition.c_str(),
             current_status_.app_version.c_str(),
             current_status_.compile_time.c_str(),
             current_status_.next_partition.c_str());
    g_log_buffer.log(msg);
    ESP_LOGI(TAG, "%s", msg);
}

void OtaManager::confirm_running_image() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; // Not a freshly installed OTA image
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        g_log_buffer.log("New firmware confirmed healthy; rollback cancelled.");
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }
}

std::string OtaManager::get_running_partition_name() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_status_.running_partition;
}

std::string OtaManager::get_next_partition_name() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_status_.next_partition;
}

OtaStatus OtaManager::get_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_status_;
}

// -------------------------------------------------------------
// Stream-based upload (Web Control Panel POST)
// -------------------------------------------------------------

esp_err_t OtaManager::begin_stream_update(size_t image_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (in_progress_) {
        ESP_LOGE(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    update_partition_ = esp_ota_get_next_update_partition(NULL);
    if (!update_partition_) {
        ESP_LOGE(TAG, "No valid OTA target partition found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Beginning stream OTA flashing to partition '%s' (size limit: %u bytes)",
             update_partition_->label, (unsigned int)image_size);

    esp_err_t err = esp_ota_begin(update_partition_, image_size, &ota_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    in_progress_ = true;
    stream_bytes_written_ = 0;
    stream_total_size_ = (image_size != OTA_SIZE_UNKNOWN) ? image_size : 0;

    current_status_.in_progress = true;
    current_status_.progress_percent = 0;
    current_status_.bytes_transferred = 0;
    current_status_.total_size = stream_total_size_;
    current_status_.status_message = "Receiving and flashing firmware...";
    current_status_.success = false;
    current_status_.requires_reboot = false;

    g_log_buffer.log("Firmware upload initiated. Flashing to " + std::string(update_partition_->label) + "...");
    return ESP_OK;
}

esp_err_t OtaManager::write_stream_chunk(const char* data, size_t length) {
    if (!in_progress_ || !data || length == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_ota_write(ota_handle_, data, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        abort_stream_update();
        return err;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stream_bytes_written_ += length;
    current_status_.bytes_transferred = stream_bytes_written_;

    if (stream_total_size_ > 0) {
        current_status_.progress_percent = (int)((stream_bytes_written_ * 100) / stream_total_size_);
    }

    return ESP_OK;
}

esp_err_t OtaManager::finish_stream_update() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!in_progress_) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Finalizing stream OTA. Total written: %u bytes", (unsigned int)stream_bytes_written_);
    current_status_.status_message = "Verifying image integrity...";

    esp_err_t err = esp_ota_end(ota_handle_);
    ota_handle_ = 0;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (checksum/signature validation error): %s", esp_err_to_name(err));
        in_progress_ = false;
        current_status_.in_progress = false;
        current_status_.status_message = "Verification failed: " + std::string(esp_err_to_name(err));
        g_log_buffer.log("OTA verification failed: " + std::string(esp_err_to_name(err)));
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        in_progress_ = false;
        current_status_.in_progress = false;
        current_status_.status_message = "Set boot partition failed: " + std::string(esp_err_to_name(err));
        return err;
    }

    // in_progress_ stays set until reboot so nothing can erase the new boot slot
    current_status_.in_progress = false;
    current_status_.progress_percent = 100;
    current_status_.status_message = "Firmware updated successfully! Rebooting...";
    current_status_.success = true;
    current_status_.requires_reboot = true;

    char log_str[160];
    snprintf(log_str, sizeof(log_str), "OTA Successful! Boot partition swapped to '%s'. Rebooting...", update_partition_->label);
    g_log_buffer.log(log_str);
    ESP_LOGI(TAG, "%s", log_str);

    return ESP_OK;
}

void OtaManager::abort_stream_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ota_handle_) return; // No upload active; never clobber a running URL update
    esp_ota_abort(ota_handle_);
    ota_handle_ = 0;
    in_progress_ = false;
    current_status_.in_progress = false;
    current_status_.status_message = "Upload aborted";
}

// -------------------------------------------------------------
// URL-based OTA (HTTP / HTTPS Background Task)
// -------------------------------------------------------------

void OtaManager::url_ota_task_entry(void* pv) {
    OtaManager* self = static_cast<OtaManager*>(pv);
    self->run_url_ota();
    vTaskDelete(NULL);
}

bool OtaManager::start_url_update(const std::string& url,
                                  std::function<void(bool success, const std::string& msg)> on_complete,
                                  std::function<void(int percent, size_t read_bytes, size_t total_bytes)> on_progress) {
    if (url.empty() || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
        if (on_complete) on_complete(false, "Invalid URL (must start with http:// or https://)");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_progress_) {
            if (on_complete) on_complete(false, "Another OTA update is currently in progress");
            return false;
        }
        in_progress_ = true;
        task_url_ = url;
        task_on_complete_ = on_complete;
        task_on_progress_ = on_progress;

        current_status_.in_progress = true;
        current_status_.progress_percent = 0;
        current_status_.bytes_transferred = 0;
        current_status_.total_size = 0;
        current_status_.status_message = "Connecting to " + url + "...";
        current_status_.success = false;
        current_status_.requires_reboot = false;
    }

    g_log_buffer.log("Starting OTA download from: " + url);

    BaseType_t res = xTaskCreatePinnedToCore(
        url_ota_task_entry,
        "ota_url_task",
        8192,
        this,
        5,
        NULL,
        1 // Core 1 (leave Core 0 for Wi-Fi and IRC)
    );

    if (res != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        in_progress_ = false;
        current_status_.in_progress = false;
        current_status_.status_message = "Failed to create FreeRTOS task";
        if (on_complete) on_complete(false, "FreeRTOS memory error: cannot create task");
        return false;
    }

    return true;
}

void OtaManager::run_url_ota() {
    std::string url;
    std::function<void(bool, const std::string&)> on_complete;
    std::function<void(int, size_t, size_t)> on_progress;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = task_url_;
        on_complete = task_on_complete_;
        on_progress = task_on_progress_;
    }

    ESP_LOGI(TAG, "Connecting to OTA URL: %s", url.c_str());

    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.timeout_ms = 25000;
    http_config.keep_alive_enable = true;
    // GitHub release downloads redirect to very long signed URLs
    http_config.buffer_size = 4096;
    http_config.buffer_size_tx = 4096;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);

    if (err != ESP_OK || !https_ota_handle) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "OTA connection failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", err_buf);
        g_log_buffer.log(err_buf);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_progress_ = false;
            current_status_.in_progress = false;
            current_status_.status_message = err_buf;
        }
        if (on_complete) on_complete(false, err_buf);
        return;
    }

    int last_reported_percent = -1;

    while (true) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int read_len = esp_https_ota_get_image_len_read(https_ota_handle);
        int total_len = esp_https_ota_get_image_size(https_ota_handle);
        int percent = (total_len > 0) ? (read_len * 100 / total_len) : 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_status_.bytes_transferred = read_len;
            current_status_.total_size = total_len;
            current_status_.progress_percent = percent;
            char msg[96];
            snprintf(msg, sizeof(msg), "Downloading & Flashing: %d%% (%u KB / %u KB)",
                     percent, (unsigned int)(read_len / 1024), (unsigned int)(total_len / 1024));
            current_status_.status_message = msg;
        }

        if (percent != last_reported_percent && (percent % 10 == 0 || percent == 100)) {
            last_reported_percent = percent;
            ESP_LOGI(TAG, "OTA Progress: %d%% (%d bytes)", percent, read_len);
            if (on_progress) on_progress(percent, read_len, total_len);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "OTA transmission error: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", err_buf);
        g_log_buffer.log(err_buf);
        esp_https_ota_abort(https_ota_handle);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_progress_ = false;
            current_status_.in_progress = false;
            current_status_.status_message = err_buf;
        }
        if (on_complete) on_complete(false, err_buf);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(https_ota_handle)) {
        const char* msg = "OTA failed: download incomplete";
        g_log_buffer.log(msg);
        esp_https_ota_abort(https_ota_handle);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_progress_ = false;
            current_status_.in_progress = false;
            current_status_.status_message = msg;
        }
        if (on_complete) on_complete(false, msg);
        return;
    }

    esp_err_t finish_err = esp_https_ota_finish(https_ota_handle);
    if (finish_err == ESP_OK) {
        char ok_msg[160];
        snprintf(ok_msg, sizeof(ok_msg), "OTA Upgrade Successful! Rebooting from new partition...");
        ESP_LOGI(TAG, "%s", ok_msg);
        g_log_buffer.log(ok_msg);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_status_.in_progress = false;
            current_status_.progress_percent = 100;
            current_status_.status_message = ok_msg;
            current_status_.success = true;
            current_status_.requires_reboot = true;
        }

        if (on_complete) on_complete(true, ok_msg);

        // Allow network response/IRC message to transmit cleanly before restart
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "OTA validation failed: %s", esp_err_to_name(finish_err));
        ESP_LOGE(TAG, "%s", err_buf);
        g_log_buffer.log(err_buf);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_progress_ = false;
            current_status_.in_progress = false;
            current_status_.status_message = err_buf;
        }
        if (on_complete) on_complete(false, err_buf);
    }
}

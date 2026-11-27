#include "update_checker.h"
#include "bot_settings.h"
#include "log_buffer.h"
#include "ota_manager.h"
#include "releases.h"
#include "wifi_manager.h"
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

UpdateChecker g_updates;

extern const char update_key_start[] asm("_binary_update_key_pem_start");
extern const char update_key_end[] asm("_binary_update_key_pem_end"); // after the closing zero byte

namespace {

// Where tools/release.sh publishes
const char* const RELEASES = "https://raw.githubusercontent.com/gcanosa/esp32-polibot/main/firmware/";
const char* const BOARD = "esp32s3-n16r8";
constexpr size_t MANIFEST_LIMIT = 2048;
constexpr size_t SIGNATURE_LIMIT = 128;
constexpr int64_t FIRST_CHECK_AFTER = 120; // seconds after a start: the connections settle first
constexpr int64_t RETRY_AFTER = 900;

int64_t seconds() { return esp_timer_get_time() / 1000000; }

// A whole small file. False if there is no connection, no such file, or more than `limit` bytes.
bool fetch(const std::string& url, size_t limit, std::string& out, std::string& problem) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.buffer_size = 2048; // GitHub answers with long headers
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        problem = "Not enough memory to connect.";
        return false;
    }
    bool ok = false;
    out.clear();
    if (esp_http_client_open(client, 0) != ESP_OK) {
        problem = "Could not connect to the release server.";
    } else {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        char buffer[256];
        int n = 0;
        while (status == 200 && out.size() <= limit && (n = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) out.append(buffer, n);
        ok = status == 200 && n == 0 && out.size() <= limit && esp_http_client_is_complete_data_received(client);
        if (status != 200) problem = "The release server answered " + std::to_string(status) + ".";
        else if (!ok) problem = out.size() > limit ? "The release list is too large." : "The download was cut short.";
    }
    esp_http_client_cleanup(client);
    return ok;
}

bool signed_by_the_project(const std::string& data, const std::string& signature) {
    unsigned char hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash, 0);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const bool genuine =
        mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char*>(update_key_start), update_key_end - update_key_start) == 0 &&
        mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                          reinterpret_cast<const unsigned char*>(signature.data()), signature.size()) == 0;
    mbedtls_pk_free(&key);
    return genuine;
}

std::string text_of(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

} // namespace

std::string UpdateChecker::fetch_release(Release& out) {
    std::string manifest, signature, problem;
    if (!fetch(std::string(RELEASES) + "latest.json", MANIFEST_LIMIT, manifest, problem) ||
        !fetch(std::string(RELEASES) + "latest.json.sig", SIGNATURE_LIMIT, signature, problem)) {
        return problem;
    }
    if (!signed_by_the_project(manifest, signature)) return "The release list does not carry a genuine signature. Ignored.";

    cJSON* root = cJSON_Parse(manifest.c_str());
    const cJSON* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    out.version = text_of(root, "version");
    out.file = text_of(root, "file");
    out.sha256 = text_of(root, "sha256");
    out.notes = text_of(root, "notes");
    out.size = cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble < 0x1000000 ? static_cast<size_t>(size->valuedouble) : 0;
    const bool this_board = text_of(root, "board") == BOARD;
    cJSON_Delete(root);
    if (!this_board) return "The newest release is for another board.";
    if (out.version.empty() || !release_file_name(out.file) || !sha256_text(out.sha256) || !out.size) return "The release list is incomplete.";
    return "";
}

void UpdateChecker::run() {
    Release release;
    const std::string problem = fetch_release(release);
    const std::string running = esp_app_get_description()->version;
    std::string announce;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        checked_at_ = seconds();
        failed_ = !problem.empty();
        if (failed_) {
            message_ = problem; // what an earlier check found stays on offer
        } else if (version_newer(release.version, running)) {
            found_ = release;
            message_ = "Version " + release.version + " is available.";
            if (announced_ != release.version) announce = announced_ = release.version;
        } else {
            found_ = Release();
            message_ = "You have the latest version.";
        }
    }
    if (!announce.empty()) g_log_buffer.log("A new version, " + announce + ", is available: see Firmware & Backup in the web panel.");
    else if (!problem.empty()) g_log_buffer.log("Could not check for a new version: " + problem);
}

void UpdateChecker::task_entry(void* self) {
    static_cast<UpdateChecker*>(self)->run(); // everything it holds is freed on return
    static_cast<UpdateChecker*>(self)->checking_ = false;
    vTaskDelete(nullptr);
}

bool UpdateChecker::check_now() {
    if (checking_.exchange(true)) return false;
    // lives only while checking; the stack must hold a TLS handshake
    if (xTaskCreatePinnedToCore(task_entry, "update_check", 9216, this, 2, nullptr, 1) == pdPASS) return true;
    checking_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = "Not enough memory to check now.";
    return false;
}

void UpdateChecker::tick() {
    const auto settings = g_settings.get();
    const int64_t now = seconds();
    if (!settings->update_auto_check || now < FIRST_CHECK_AFTER || !g_wifi_mgr.is_connected() || g_ota_mgr.is_in_progress()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t wait = failed_ ? RETRY_AFTER : settings->update_check_hours * 3600LL;
        if (checked_at_ >= 0 && now - checked_at_ < wait) return;
    }
    check_now();
}

bool UpdateChecker::install(std::string& problem) {
    Release release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release = found_;
    }
    if (release.version.empty()) {
        problem = "No newer version is known. Check first.";
        return false;
    }
    const ImageCheck expected{release.size, release.sha256};
    if (!g_ota_mgr.start_url_update(RELEASES + release.file, nullptr, nullptr, &expected)) {
        problem = "An update is already in progress.";
        return false;
    }
    return true;
}

std::string UpdateChecker::available() {
    std::lock_guard<std::mutex> lock(mutex_);
    return found_.version;
}

cJSON* UpdateChecker::state_json() {
    const auto settings = g_settings.get();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "current", esp_app_get_description()->version);
    cJSON_AddBoolToObject(o, "auto_check", settings->update_auto_check);
    cJSON_AddNumberToObject(o, "check_hours", settings->update_check_hours);
    cJSON_AddBoolToObject(o, "checking", checking_);
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON_AddStringToObject(o, "message", message_.c_str());
    cJSON_AddNumberToObject(o, "checked_ago", checked_at_ < 0 ? -1 : seconds() - checked_at_);
    if (!found_.version.empty()) {
        cJSON* offer = cJSON_AddObjectToObject(o, "available");
        cJSON_AddStringToObject(offer, "version", found_.version.c_str());
        cJSON_AddStringToObject(offer, "notes", found_.notes.c_str());
        cJSON_AddNumberToObject(offer, "size", found_.size);
    }
    return o;
}

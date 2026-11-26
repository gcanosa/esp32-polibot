// Firmware update (OTA), owner only
#include "web_server.h"
#include "ota_manager.h"
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <vector>

// 12. GET /api/ota/status
esp_err_t WebServer::api_ota_status_handler(httpd_req_t* req) {
    if (!require_owner(req)) return ESP_OK;
    OtaStatus s = g_ota_mgr.get_status();
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);

    cJSON* res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "in_progress", s.in_progress);
    cJSON_AddNumberToObject(res, "progress_percent", s.progress_percent);
    cJSON_AddNumberToObject(res, "bytes_transferred", s.bytes_transferred);
    cJSON_AddNumberToObject(res, "total_size", s.total_size);
    cJSON_AddStringToObject(res, "status_message", s.status_message.c_str());
    cJSON_AddStringToObject(res, "running_partition", s.running_partition.c_str());
    cJSON_AddStringToObject(res, "next_partition", s.next_partition.c_str());
    cJSON_AddNumberToObject(res, "max_image_size", next ? next->size : 0);
    cJSON_AddStringToObject(res, "app_version", s.app_version.c_str());
    cJSON_AddStringToObject(res, "compile_time", s.compile_time.c_str());
    cJSON_AddBoolToObject(res, "success", s.success);
    cJSON_AddBoolToObject(res, "requires_reboot", s.requires_reboot);
    return send_json(req, res, 200);
}

// 13. POST /api/ota/upload  (request body = raw firmware .bin)
esp_err_t WebServer::api_ota_upload_handler(httpd_req_t* req) {
    // ESP_FAIL closes the socket, so an unauthorised body is never drained
    if (!require_owner(req)) return ESP_FAIL;

    std::vector<char> buf(4096);
    size_t remaining = req->content_len;
    // Owner errors: swallow the rest of the body first, otherwise the browser sees a connection reset instead of the reason
    auto reject = [&](int status, const std::string& why) -> esp_err_t {
        while (remaining > 0) {
            int n = httpd_req_recv(req, buf.data(), std::min(remaining, buf.size()));
            if (n <= 0) return ESP_FAIL;
            remaining -= n;
        }
        return send_message(req, status, why);
    };

    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return reject(400, "Firmware image is empty or larger than the OTA partition.");
    }

    esp_err_t err = g_ota_mgr.begin_stream_update(req->content_len);
    if (err != ESP_OK) {
        std::string why = (err == ESP_ERR_INVALID_STATE) ? "Another update is in progress or awaiting reboot."
                        : (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) ? "Running firmware is still being verified. Retry in 30 seconds."
                        : std::string("Cannot start update: ") + esp_err_to_name(err);
        return reject(409, why);
    }

    int timeouts = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf.data(), std::min(remaining, buf.size()));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 6) continue; // ~30 s of silence
        if (n <= 0) {
            g_ota_mgr.abort_stream_update();
            g_log_buffer.log("Firmware upload aborted: connection lost.");
            return ESP_FAIL;
        }
        timeouts = 0;
        remaining -= n;
        err = g_ota_mgr.write_stream_chunk(buf.data(), n);
        if (err != ESP_OK) {
            return reject(400, std::string("Flash write rejected: ") + esp_err_to_name(err));
        }
    }

    err = g_ota_mgr.finish_stream_update();
    if (err != ESP_OK) {
        return send_message(req, 400, std::string("Image verification failed: ") + esp_err_to_name(err));
    }

    send_message(req, 200, "Firmware updated. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1500)); // let the response leave the socket
    esp_restart();
    return ESP_OK;
}

// 14. POST /api/ota/url  {"url": "https://..."}
esp_err_t WebServer::api_ota_url_handler(httpd_req_t* req) {
    if (!require_owner(req)) return ESP_OK;
    cJSON* p = cJSON_Parse(read_post_body(req).c_str());
    cJSON* url_i = cJSON_GetObjectItemCaseSensitive(p, "url");
    std::string url = (url_i && url_i->valuestring) ? url_i->valuestring : "";
    cJSON_Delete(p);

    if (!g_ota_mgr.start_url_update(url)) {
        return send_message(req, 400, "Invalid URL (http:// or https://), or an update is already in progress.");
    }
    return send_message(req, 200, "Download started.");
}

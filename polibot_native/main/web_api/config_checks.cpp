// Refusing settings that cannot work, whether typed in the panel or restored
#include "config_checks.h"
#include "guardian.h"
#include "irc_config.h"
#include "irc_masks.h"
#include "mdns_reply.h"
#include "proxy.h"
#include <cstring>

std::string validate_wifi_config(const cJSON* wifi) {
    auto length = [&](const char* key) -> int {
        cJSON* v = cJSON_GetObjectItemCaseSensitive(wifi, key);
        return cJSON_IsString(v) ? (int)strlen(v->valuestring) : -1; // -1: not sent
    };
    if (length("ssid") > 32) return "The WiFi name can have 32 characters at most.";
    if (length("password") > 64) return "The WiFi password can have 64 characters at most.";
    if (length("ap_ssid") == 0 || length("ap_ssid") > 32) return "The access point name needs 1-32 characters.";
    const int ap = length("ap_password");
    if (ap > 63 || (ap > 0 && ap < 8)) return "The access point password needs 8-63 characters, or none for an open network.";
    return "";
}

// Auto-op, half-op and voice entries must each belong to a channel and identify somebody
static std::string validate_access(const cJSON* guardian) {
    cJSON* list = cJSON_GetObjectItemCaseSensitive(guardian, "access");
    if (!list) return "";
    if (!cJSON_IsArray(list) || (size_t)cJSON_GetArraySize(list) > AccessList::MAX) {
        return "The auto-op and voice lists hold at most " + std::to_string(AccessList::MAX) + " entries in all.";
    }
    AccessList accepted;
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        auto text = [&](const char* key) -> std::string {
            cJSON* v = cJSON_GetObjectItemCaseSensitive(entry, key);
            return cJSON_IsString(v) ? v->valuestring : "";
        };
        const std::string mode = text("mode"), who = text("who"), channel = text("channel");
        if (mode.size() != 1 || !is_channel_name(channel) || !accepted.add(text("network"), channel, mode[0], who)) {
            const std::string why = access_problem(who);
            return "List entry '" + who + "': " + (why.empty() ? "it needs a network, a channel and a mode of o, h or v." : why);
        }
    }
    return "";
}

// An auto-kick entry that matches everybody would empty a channel
std::string validate_guardian_config(const cJSON* guardian) {
    const std::string access = validate_access(guardian);
    if (!access.empty()) return access;
    cJSON* list = cJSON_GetObjectItemCaseSensitive(guardian, "akick");
    if (!list) return "";
    if (!cJSON_IsArray(list) || (size_t)cJSON_GetArraySize(list) > MAX_AKICKS) {
        return "The auto-kick list holds at most " + std::to_string(MAX_AKICKS) + " entries.";
    }
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        cJSON* mask = cJSON_GetObjectItemCaseSensitive(entry, "mask");
        if (!cJSON_IsString(mask) || !mask_is_specific(mask->valuestring) || strchr(mask->valuestring, ' ')) {
            return std::string("Auto-kick mask '") + (cJSON_IsString(mask) ? mask->valuestring : "") + "' must name a nick or host, without spaces.";
        }
    }
    return "";
}

std::string validate_bot_config(const cJSON* config) {
    const cJSON* url = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(config, "alerts"), "url");
    if (cJSON_IsString(url) && url->valuestring[0] &&
        (strlen(url->valuestring) > 256 || strpbrk(url->valuestring, " \r\n") ||
         (strncmp(url->valuestring, "http://", 7) != 0 && strncmp(url->valuestring, "https://", 8) != 0))) {
        return "The alert address starts with http:// or https:// and has no spaces (256 characters at most).";
    }
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(config, "network"), "mdns_name");
    if (cJSON_IsString(name) && name->valuestring[0] && !valid_host_name(name->valuestring)) {
        return "The local name uses 1-32 letters, digits or hyphens, without \".local\".";
    }
    const cJSON* proxies = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(config, "network"), "trusted_proxies");
    if (cJSON_IsString(proxies) && !valid_proxy_list(proxies->valuestring)) {
        return "Trusted proxies are up to 8 IPv4 addresses, such as 192.168.1.10, separated by commas.";
    }
    return "";
}

// GuardianEngine: the auto-op, half-op and voice lists, kept under guardian.access in config.json
#include "guardian.h"
#include "irc_masks.h"

void GuardianEngine::load_access_locked(const cJSON* guardian) {
    access_.clear();
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(guardian, "access")) {
        auto text = [&](const char* key) -> std::string {
            cJSON* v = cJSON_GetObjectItemCaseSensitive(entry, key);
            return cJSON_IsString(v) ? v->valuestring : "";
        };
        const std::string mode = text("mode");
        // what cannot work in a hand-edited file is left out
        if (mode.size() == 1) access_.add(text("network"), text("channel"), mode[0], text("who"));
    }
}

bool GuardianEngine::save_access(const PsramVector<AccessEntry>& entries) {
    return config_mgr_.update([&](cJSON* root) {
        cJSON* g = cJSON_GetObjectItemCaseSensitive(root, "guardian");
        if (!g) g = cJSON_AddObjectToObject(root, "guardian");
        cJSON_DeleteItemFromObjectCaseSensitive(g, "access");
        cJSON* list = cJSON_AddArrayToObject(g, "access");
        for (const auto& e : entries) {
            const char mode[2] = {e.mode, '\0'};
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "network", e.network);
            cJSON_AddStringToObject(o, "channel", e.channel);
            cJSON_AddStringToObject(o, "mode", mode);
            cJSON_AddStringToObject(o, "who", e.who);
            cJSON_AddItemToArray(list, o);
        }
        return true;
    });
}

uint8_t GuardianEngine::listed_modes(const std::string& network, const std::string& channel, const std::string& hostmask,
                                     const std::string& account, const std::string& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    return access_.modes_for(network, channel, hostmask, account, user);
}

std::vector<std::string> GuardianEngine::access_entries(const std::string& network, const std::string& channel, char mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    return access_.list(network, channel, mode);
}

std::string GuardianEngine::add_access(const std::string& network, const std::string& channel, char mode, const std::string& who) {
    const std::string problem = access_problem(who);
    if (!problem.empty()) return problem;
    if (!is_channel_name(channel) || !is_list_mode(mode)) return "Name a channel and one of operator, half-operator or voice.";
    std::lock_guard<std::mutex> saving(access_saving_);
    AccessList next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = access_;
    }
    if (!next.add(network, channel, mode, who)) {
        return "The lists hold " + std::to_string(AccessList::MAX) + " entries in all, and they are full.";
    }
    if (!save_access(next.all())) return "Could not save to flash.";
    std::lock_guard<std::mutex> lock(mutex_);
    access_ = next;
    return "";
}

bool GuardianEngine::remove_access(const std::string& network, const std::string& channel, char mode, const std::string& who) {
    std::lock_guard<std::mutex> saving(access_saving_);
    AccessList next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = access_;
    }
    if (!next.remove(network, channel, mode, who) || !save_access(next.all())) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    access_ = next;
    return true;
}

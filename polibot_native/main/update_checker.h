#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <cJSON.h>

// Looks for newer releases in the project's public repository. The list of releases there (latest.json) is
// signed: only what carries the signature of the built-in key (update_key.pem) is offered, and a download is
// installed only if its size and SHA-256 are the ones that were signed.
class UpdateChecker {
public:
    void tick();                       // every few seconds: starts the automatic check when it is due
    bool check_now();                  // false if no check could be started (one is already running)
    bool install(std::string& problem); // downloads and installs the newer release that was found
    std::string available();           // its version, "" if none is known
    cJSON* state_json();               // for the panel; the caller owns it

private:
    struct Release {
        std::string version, file, sha256, notes;
        size_t size = 0;
    };
    static void task_entry(void* self);
    void run();
    std::string fetch_release(Release& out); // "" or why there is none to trust

    std::mutex mutex_; // everything below but checking_
    std::atomic<bool> checking_{false};
    Release found_;             // newer than the running firmware; version "" = none
    std::string message_ = "Not checked yet.";
    std::string announced_;     // the version already written to the event log
    int64_t checked_at_ = -1;   // seconds since boot
    bool failed_ = false;
};

extern UpdateChecker g_updates;

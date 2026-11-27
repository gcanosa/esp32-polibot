#pragma once

// Keeping an eye on the device: tasks that stop responding, restarts and low memory

// A task that must keep showing signs of life. Too long without one and the device restarts.
int watch_start(const char* name); // -1 if there is no room: that task goes unwatched
void watch_beat(int slot);
void watch_stop(int slot);
void watch_limit(int minutes); // 0 = never restart
void watch_hangs();            // starts the timer that checks

void report_restart();
void check_memory();

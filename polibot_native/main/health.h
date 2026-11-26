#pragma once

// The device's own condition, told to the report channels
void report_restart(); // once, after start up
void check_memory();   // from the heartbeat

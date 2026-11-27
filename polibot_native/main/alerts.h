#pragma once

// Messages sent outside IRC, to a webhook or an ntfy topic: for what the report channel cannot tell
#include <string>

// Queued, and sent once the network allows. Nothing happens while no address is set.
void alert(const std::string& message);
// Every few seconds: notices networks that stay away, and sends what waits
void alerts_tick();

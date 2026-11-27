#pragma once

// Answers "<name>.local" on the local network, so the panel opens without knowing the address

// Every few seconds: starts the responder when a name is set (it ends by itself when none is)
void mdns_tick();

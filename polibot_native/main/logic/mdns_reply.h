#pragma once

// Just enough multicast DNS to answer "what is the address of <name>.local?", and to say it has no other kind
#include <cstddef>
#include <cstdint>
#include <string>

// 1-32 letters, digits or hyphens, not starting or ending with a hyphen
bool valid_host_name(const std::string& name);

struct MdnsReply {
    size_t length = 0;    // 0: nothing to say
    bool unicast = false; // for the asker only, not the whole network
};

// Reads a query from the network and writes the answer, if it asks for `name`, into `out`.
// `legacy`: it came from another port than 5353 (an ordinary resolver), which wants its id and question back.
MdnsReply mdns_reply(const uint8_t* query, size_t length, const std::string& name, const uint8_t ip[4], bool legacy,
                     uint8_t* out, size_t room);

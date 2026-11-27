#include "mdns_reply.h"
#include <cctype>
#include <cstring>
#include <strings.h>

namespace {

uint16_t number(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Reads the name at `at`, which then points past it. True if it is exactly <host>.local.
// False with `at` set to 0 if the packet is broken. Everything comes from the network: check every step.
bool name_is(const uint8_t* q, size_t length, size_t& at, const std::string& host) {
    const char* wanted[] = {host.c_str(), "local"};
    size_t pos = at, end = 0;
    int label = 0, jumps = 0;
    bool match = true;
    at = 0;
    while (pos < length) {
        const uint8_t size = q[pos];
        if (!size) {
            at = end ? end : pos + 1;
            return match && label == 2;
        }
        if ((size & 0xc0) == 0xc0) { // a pointer to the rest of the name, which only ever points back
            if (pos + 1 >= length || ++jumps > 4) return false;
            const size_t target = (size_t)(size & 0x3f) << 8 | q[pos + 1];
            if (!end) end = pos + 2;
            if (target >= pos) return false;
            pos = target;
            continue;
        }
        if (size > 63 || pos + 1 + size > length) return false;
        match = match && label < 2 && std::strlen(wanted[label]) == size &&
                strncasecmp(reinterpret_cast<const char*>(q + pos + 1), wanted[label], size) == 0;
        label++;
        pos += 1 + size;
    }
    return false;
}

struct Writer {
    uint8_t* out;
    size_t at = 0;
    void byte(uint8_t b) { out[at++] = b; }
    void word(uint16_t w) {
        byte(w >> 8);
        byte(w & 0xff);
    }
    void name(const std::string& host) {
        byte(host.size());
        for (char c : host) byte(c);
        byte(5);
        for (char c : {'l', 'o', 'c', 'a', 'l'}) byte(c);
        byte(0);
    }
    // what every record starts with
    void record(const std::string& host, uint16_t type, bool legacy, uint16_t data) {
        name(host);
        word(type);
        word(legacy ? 0x0001 : 0x8001); // IN, replacing what is cached
        word(0);
        word(legacy ? 10 : 120); // seconds it may be remembered
        word(data);
    }
};

} // namespace

bool valid_host_name(const std::string& name) {
    if (name.empty() || name.size() > 32 || name.front() == '-' || name.back() == '-') return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '-') return false;
    }
    return true;
}

MdnsReply mdns_reply(const uint8_t* query, size_t length, const std::string& name, const uint8_t ip[4], bool legacy,
                     uint8_t* out, size_t room) {
    MdnsReply reply;
    // a query (not a response), opcode 0
    if (!valid_host_name(name) || length < 12 || (number(query + 2) & 0xf800)) return reply;
    // The question to answer: the one for the address if it is there, else any other about this name
    const int questions = number(query + 4);
    size_t at = 12;
    uint16_t asked = 0, asked_class = 0;
    for (int i = 0; i < questions && i < 16; i++) {
        const bool ours = name_is(query, length, at, name);
        if (!at || at + 4 > length) return reply;
        const uint16_t type = number(query + at), klass = number(query + at + 2);
        at += 4;
        if (!ours || ((klass & 0x7fff) != 1 && (klass & 0x7fff) != 255)) continue;
        asked = type;
        asked_class = klass;
        if (type == 1 || type == 255) break;
    }
    if (!asked) return reply;

    // Whoever also asks for an IPv6 address waits seconds unless told that there is none: an NSEC record says
    // the name has an address of the first kind only (RFC 6762, 6.1). An ordinary resolver takes an empty answer.
    const bool address = asked == 1 || asked == 255;
    const size_t encoded = name.size() + 8;
    const size_t needed = 12 + (legacy ? encoded + 4 : 0) + (address ? encoded + 14 : 0) + (legacy ? 0 : 2 * encoded + 13);
    if (needed > room) return reply;
    Writer w{out};
    w.word(legacy ? number(query) : 0);
    w.word(0x8400); // an authoritative answer
    w.word(legacy ? 1 : 0);
    w.word(legacy && !address ? 0 : 1);  // answers
    w.word(0);
    w.word(!legacy && address ? 1 : 0); // additional records
    if (legacy) {
        w.name(name);
        w.word(asked);
        w.word(1);
    }
    if (address) {
        w.record(name, 1, legacy, 4); // A
        for (int b = 0; b < 4; b++) w.byte(ip[b]);
    }
    if (!legacy) {
        w.record(name, 47, false, encoded + 3); // NSEC
        w.name(name);
        w.byte(0); // the first 256 types,
        w.byte(1); // one byte of them:
        w.byte(0x40); // only type 1, A
    }
    reply.length = w.at;
    reply.unicast = legacy || (asked_class & 0x8000);
    return reply;
}

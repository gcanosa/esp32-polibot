// Runs on a PC:  c++ -std=c++17 -I main test/status_led_test.cpp -o /tmp/status_led_test && /tmp/status_led_test
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "status_led.h"

static bool same(Rgb c, int r, int g, int b) { return c.r == r && c.g == g && c.b == b; }

int main() {
    assert(link_status(false, 0, 0) == LinkStatus::NoWifi);
    assert(link_status(false, 2, 2) == LinkStatus::NoWifi); // WiFi gone, IRC not yet timed out
    assert(link_status(true, 0, 0) == LinkStatus::NoIrc);   // no network configured
    assert(link_status(true, 0, 3) == LinkStatus::NoIrc);
    assert(link_status(true, 1, 3) == LinkStatus::SomeIrc);
    assert(link_status(true, 2, 3) == LinkStatus::SomeIrc);
    assert(link_status(true, 1, 1) == LinkStatus::AllIrc);
    assert(link_status(true, 4, 4) == LinkStatus::AllIrc);

    LedSettings s;
    s.brightness = 100;
    const auto dark = [](Rgb c) { return same(c, 0, 0, 0); };
    const auto alone = [&](LinkStatus status, unsigned tick) { return led_colour(s, status, WebUse::Nobody, tick); };
    assert(same(alone(LinkStatus::NoWifi, 0), 255, 0, 0));
    assert(same(alone(LinkStatus::NoIrc, 0), 255, 50, 0));
    assert(same(alone(LinkStatus::SomeIrc, 0), 255, 180, 0));
    assert(same(alone(LinkStatus::AllIrc, 0), 0, 255, 0));
    // alone: half a second of the status colour every two seconds, dark otherwise
    for (unsigned tick = 0; tick < 60; tick++) {
        const Rgb c = alone(LinkStatus::AllIrc, tick);
        assert(tick % 20 < 5 ? same(c, 0, 255, 0) : dark(c));
    }
    // signed in: the same blink, with quick white flashes in the gap and a pause on both sides of it
    int flashes = 0;
    for (unsigned tick = 0; tick < 20; tick++) {
        const Rgb c = led_colour(s, LinkStatus::AllIrc, WebUse::Panel, tick);
        if (tick < 5) assert(same(c, 0, 255, 0));
        else if (tick >= 7 && tick <= 17 && tick % 2 == 1) { assert(same(c, 255, 255, 255)); flashes++; }
        else assert(dark(c));
    }
    assert(flashes == 6);
    assert(same(led_colour(s, LinkStatus::NoWifi, WebUse::Visitor, 7), 150, 0, 255) && dark(led_colour(s, LinkStatus::NoWifi, WebUse::Visitor, 8)));
    s.period = 5;
    assert(dark(alone(LinkStatus::AllIrc, 20)) && same(alone(LinkStatus::AllIrc, 50), 0, 255, 0));
    s.period = 0; // out of range: the shortest
    assert(same(alone(LinkStatus::AllIrc, 10), 0, 255, 0) && same(led_colour(s, LinkStatus::AllIrc, WebUse::Panel, 7), 255, 255, 255));
    s.period = 2;

    // own colours
    assert(parse_colour("#1A2b3C", s.colour[3]) && same(alone(LinkStatus::AllIrc, 0), 26, 43, 60));
    assert(parse_colour("#0000ff", s.colour[LED_PANEL]) && same(led_colour(s, LinkStatus::AllIrc, WebUse::Panel, 7), 0, 0, 255));
    for (const char* bad : {"", "#fff", "ff0000", "#ff00000", "#gg0000", "#-10000", "# ff000", "red"}) {
        Rgb keep{1, 2, 3};
        assert(!parse_colour(bad, keep) && same(keep, 1, 2, 3));
    }
    assert(!parse_colour(nullptr, s.colour[0]));
    char text[8];
    format_colour({0, 160, 5}, text);
    assert(std::strcmp(text, "#00a005") == 0);

    LedSettings d;
    const auto first = [&](LinkStatus status) { return led_colour(d, status, WebUse::Nobody, 0); };
    assert(d.enabled && d.period == 2 && d.brightness == 12 && same(first(LinkStatus::NoWifi), 30, 0, 0));
    d.brightness = 1; // dim orange stays orange
    assert(same(first(LinkStatus::NoIrc), 2, 1, 0));
    d.brightness = 0; // off, flashes included
    assert(same(first(LinkStatus::AllIrc), 0, 0, 0) && same(led_colour(d, LinkStatus::AllIrc, WebUse::Panel, 7), 0, 0, 0));
    d.brightness = 500;
    assert(same(first(LinkStatus::NoWifi), 255, 0, 0));
    d.brightness = -3;
    assert(same(first(LinkStatus::NoWifi), 0, 0, 0));
    d.brightness = 100;
    d.enabled = false; // switched off
    for (unsigned tick = 0; tick < 20; tick++) assert(same(led_colour(d, LinkStatus::AllIrc, WebUse::Panel, tick), 0, 0, 0));
    std::puts("status led: all checks passed");
}

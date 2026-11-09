#include "cyc_host_extras.h"
#include "cyc_tcp.h"
#include "imgui.h"
#include <cstdio>

static bool open;
static int selected;
static bool is_open(void*) { return open; }
static void draw(void*) {
    ImGui::Begin("Game overlay");
    if (ImGui::Button("First")) selected = 1;
    ImGui::SetItemDefaultFocus();
    if (ImGui::Button("Second")) selected = 2;
    ImGui::End();
}
static void command(int id, const char* line) {
    bool value;
    if (cyc_tcp_bool(line, "open", &value)) open = value;
    char fields[80];
    std::snprintf(fields, sizeof fields, "\"open\":%s,\"selected\":%d", open ? "true" : "false", selected);
    cyc_tcp_ok(id, fields);
}
static void setup(void*) { cyc_tcp_register("overlay", "test game overlay", command); }
extern "C" const CycHostExtras* cyc_host_extras(void) {
    static CycHostExtras extras{};
    extras.tcp_setup = setup;
    extras.ui_is_open = is_open;
    extras.ui_render = draw;
    return &extras;
}

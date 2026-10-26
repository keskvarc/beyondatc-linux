#include "menu.hpp"

#include <cassert>

struct Item {
    std::string name;
    void* ref = nullptr;
    bool enabled = true;
    XPLMMenuCheck checked = xplm_Menu_NoCheck;
};
struct Menu {
    std::vector<Item> items;
    XPLMMenuHandler_f handler = nullptr;
    void* ref = nullptr;
} plugins, submenu;
bool fail_create = false;
int fail_append = -1, destroyed = 0, removed = 0;
std::vector<batc::MenuAction> actions;
static void action(batc::MenuAction value) { actions.push_back(value); }
static Menu& as_menu(XPLMMenuID id) { assert(id == &plugins || id == &submenu); return *static_cast<Menu*>(id); }
static Item& named(const std::string& text) {
    for (auto& item : submenu.items) if (item.name.find(text) == 0) return item;
    throw std::runtime_error("missing menu item: " + text);
}
extern "C" {
XPLMMenuID XPLMFindPluginsMenu() { return &plugins; }
int XPLMAppendMenuItem(XPLMMenuID id, const char* name, void* ref, int) {
    auto& menu = as_menu(id);
    if (id == &submenu && int(menu.items.size()) == fail_append) return -1;
    menu.items.push_back({name, ref});
    return int(menu.items.size() - 1);
}
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID parent, int index, XPLMMenuHandler_f handler, void* ref) {
    assert(parent == &plugins && plugins.items.at(index).name == "BeyondATC Native");
    if (fail_create) return nullptr;
    submenu = {};
    submenu.handler = handler; submenu.ref = ref;
    return &submenu;
}
void XPLMDestroyMenu(XPLMMenuID menu) { assert(menu == &submenu); ++destroyed; submenu = {}; }
void XPLMRemoveMenuItem(XPLMMenuID menu, int index) {
    assert(menu == &plugins && plugins.items.at(index).name == "BeyondATC Native");
    ++removed; plugins.items.erase(plugins.items.begin() + index);
}
void XPLMAppendMenuSeparator(XPLMMenuID menu) { as_menu(menu).items.push_back({"separator"}); }
void XPLMSetMenuItemName(XPLMMenuID menu, int index, const char* name, int) { as_menu(menu).items.at(index).name = name; }
void XPLMCheckMenuItem(XPLMMenuID menu, int index, XPLMMenuCheck check) { as_menu(menu).items.at(index).checked = check; }
void XPLMEnableMenuItem(XPLMMenuID menu, int index, int enabled) { as_menu(menu).items.at(index).enabled = enabled; }
}
int main() {
    plugins.items.push_back({"existing plugin"});
    {
        batc::PluginMenu menu(action);
        menu.create();
        assert(plugins.items.size() == 2);
        batc::DiagnosticStatus status;
        batc::RecordingState recording;
        menu.refresh(status, recording);
        assert(!named("Connection: plugin disabled").enabled);
        assert(!named("Record flight diagnostics").enabled);
        assert(named("Record flight diagnostics").checked == xplm_Menu_Unchecked);
        assert(named("Write status snapshot").enabled);
        assert(!named("Logs: Output/").enabled);
        status.enabled = status.connected = status.tcas = status.aircraft_loaded = true;
        status.heartbeat_age = 0.1; status.models = 2027; status.aircraft = 60;
        status.last_issue = "test\nissue";
        recording.active = true; recording.dropped = 7;
        menu.refresh(status, recording);
        assert(!named("Connection: connected; heartbeat 0.1 s ago").enabled);
        assert(!named("Traffic: 60/63; TCAS: owned").enabled);
        assert(!named("Last issue: test?issue").enabled);
        assert(named("Record flight diagnostics").enabled);
        assert(named("Record flight diagnostics").checked == xplm_Menu_Checked);
        assert(!named("Recording: ON; dropped records: 7").enabled);
        submenu.handler(submenu.ref, named("Record flight diagnostics").ref);
        submenu.handler(submenu.ref, named("Write status snapshot").ref);
        submenu.handler(submenu.ref, nullptr);
        assert((actions == std::vector{batc::MenuAction::recording, batc::MenuAction::snapshot}));
        recording.active = false; recording.error = "disk full";
        menu.refresh(status, recording);
        assert(named("Record flight diagnostics").checked == xplm_Menu_Unchecked);
        assert(!named("Recording: ERROR (see Log.txt)").enabled);
        status.connected = false;
        menu.refresh(status, recording);
        assert(!named("Connection: waiting for BeyondATC").enabled);
        status.enabled = false;
        menu.refresh(status, recording);
        assert(!named("Connection: plugin disabled").enabled);
        assert(!named("Record flight diagnostics").enabled);
        menu.destroy();
        menu.destroy();
    }
    assert(destroyed == 1 && removed == 1 && plugins.items.size() == 1);
    fail_create = true;
    {
        batc::PluginMenu menu(action);
        bool failed = false;
        try { menu.create(); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed);
    }
    assert(destroyed == 1 && removed == 2 && plugins.items.size() == 1);
    fail_create = false; fail_append = 3;
    {
        batc::PluginMenu menu(action);
        bool failed = false;
        try { menu.create(); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed);
    }
    assert(destroyed == 2 && removed == 3 && plugins.items.size() == 1);
    assert(plugins.items[0].name == "existing plugin");
}

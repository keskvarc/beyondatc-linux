#pragma once

#include "diagnostics.hpp"
#include "XPLMMenus.h"

namespace batc {
enum class MenuAction { recording, snapshot };

class PluginMenu {
    XPLMMenuID parent = nullptr, menu = nullptr;
    int parent_item = -1, recording_item = -1, recording_status = -1;
    std::array<int, 8> information{};
    void (*action)(MenuAction);
    MenuAction recording_action = MenuAction::recording, snapshot_action = MenuAction::snapshot;
    static void selected(void* ref, void* item) {
        auto& self = *static_cast<PluginMenu*>(ref);
        if (item == &self.recording_action) self.action(MenuAction::recording);
        else if (item == &self.snapshot_action) self.action(MenuAction::snapshot);
    }
    int append(const char* name, void* ref = nullptr) {
        int item = XPLMAppendMenuItem(menu, name, ref, 0);
        if (item < 0) throw std::runtime_error("cannot create BeyondATC menu item");
        return item;
    }
public:
    explicit PluginMenu(void (*handler)(MenuAction)) : action(handler) {}
    PluginMenu(const PluginMenu&) = delete;
    PluginMenu& operator=(const PluginMenu&) = delete;
    ~PluginMenu() { destroy(); }
    void create() {
        parent = XPLMFindPluginsMenu();
        if (!parent) throw std::runtime_error("X-Plane Plugins menu is unavailable");
        parent_item = XPLMAppendMenuItem(parent, "BeyondATC Native", nullptr, 0);
        if (parent_item < 0) throw std::runtime_error("cannot add BeyondATC Plugins submenu");
        menu = XPLMCreateMenu("BeyondATC Native", parent, parent_item, selected, this);
        if (!menu) throw std::runtime_error("cannot create BeyondATC Plugins submenu");
        for (auto& item : information) {
            item = append("Starting...");
            XPLMEnableMenuItem(menu, item, 0);
        }
        XPLMAppendMenuSeparator(menu);
        recording_item = append("Record flight diagnostics (5 Hz; max 32 MiB)", &recording_action);
        recording_status = append("Recording: off");
        XPLMEnableMenuItem(menu, recording_status, 0);
        append("Write status snapshot to Log.txt", &snapshot_action);
        int path = append("Logs: Output/beyondatc-native/diagnostics");
        XPLMEnableMenuItem(menu, path, 0);
    }
    void refresh(const DiagnosticStatus& status, const RecordingState& recording) {
        if (!menu) return;
        auto lines = status_lines(status);
        for (size_t i = 0; i < lines.size(); ++i)
            XPLMSetMenuItemName(menu, information[i], lines[i].c_str(), 0);
        XPLMCheckMenuItem(menu, recording_item, recording.active ? xplm_Menu_Checked : xplm_Menu_Unchecked);
        XPLMEnableMenuItem(menu, recording_item, status.enabled);
        std::string text = "Recording: " + std::string(!recording.error.empty() ? "ERROR (see Log.txt)" :
                                                     recording.active ? "ON" : "off") +
            "; dropped records: " + std::to_string(recording.dropped);
        XPLMSetMenuItemName(menu, recording_status, text.c_str(), 0);
    }
    void destroy() {
        if (menu) XPLMDestroyMenu(menu);
        menu = nullptr;
        if (parent && parent_item >= 0) XPLMRemoveMenuItem(parent, parent_item);
        parent = nullptr;
        parent_item = -1;
    }
};
}

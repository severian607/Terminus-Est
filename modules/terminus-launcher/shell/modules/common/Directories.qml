pragma Singleton
import QtQuick
import Quickshell

// === shell paths ===
// Where the shell's own files and the user's files live. Every file that points at a script or a user file goes
// through here, so moving a folder around only needs a change in this one place.
Singleton {
    id: root

    readonly property string shell: Quickshell.shellDir
    readonly property string scripts: shell + "/scripts"

    // The user's files, honouring XDG: config (scale.json, theme.json, hidden-apps.json, launcher.json) and state
    // (launcher-slots.json, launch-state.json).
    readonly property string config: xdg("XDG_CONFIG_HOME", "/.config") + "/terminus-launcher"
    readonly property string state: xdg("XDG_STATE_HOME", "/.local/state") + "/terminus-launcher"

    function xdg(variable, fallback) {
        const v = Quickshell.env(variable)
        return (v && v.length > 0) ? v : Quickshell.env("HOME") + fallback
    }

    // "probe/memory.sh" -> /path/to/shell/scripts/probe/memory.sh
    function script(rel) { return scripts + "/" + rel }
}

pragma Singleton
import Quickshell
import Quickshell.Io
import QtQuick

// Pointer position in global layout coordinates, fed by cursorpos.py, plus the theme accent.
Singleton {
    id: root

    property real x: -100000
    property real y: -100000
    readonly property color accent: Quickshell.env("POINTERFX_ACCENT") || "#ffcccd"

    Process {
        running: true
        command: ["python3", Quickshell.env("POINTERFX_DIR") + "/cursorpos.py"]
        stdout: SplitParser {
            onRead: line => {
                const p = line.split(" ");
                root.x = parseFloat(p[0]);
                root.y = parseFloat(p[1]);
            }
        }
    }
}

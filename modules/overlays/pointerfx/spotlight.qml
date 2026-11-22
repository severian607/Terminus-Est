import Quickshell
import Quickshell.Wayland
import QtQuick

// Dims every monitor except a soft circle around the pointer. Click-through.
ShellRoot {
    Variants {
        model: Quickshell.screens

        PanelWindow {
            id: win
            required property var modelData
            readonly property real radius: 190
            readonly property real size: radius * 2
            readonly property real hx: Cursor.x - modelData.x - radius
            readonly property real hy: Cursor.y - modelData.y - radius
            readonly property color dim: Qt.rgba(Cursor.accent.r * 0.05, Cursor.accent.g * 0.05, Cursor.accent.b * 0.05, 0.62)

            function clamp(v, hi) { return Math.max(0, Math.min(hi, v)); }

            screen: modelData
            anchors { top: true; bottom: true; left: true; right: true }
            exclusionMode: ExclusionMode.Ignore
            WlrLayershell.layer: WlrLayer.Overlay
            WlrLayershell.namespace: "terminus-spotlight"
            color: "transparent"
            mask: Region {}

            Rectangle {
                x: 0; y: 0
                width: win.width
                height: win.clamp(win.hy, win.height)
                color: win.dim
            }
            Rectangle {
                x: 0
                y: win.clamp(win.hy + win.size, win.height)
                width: win.width
                height: win.height - y
                color: win.dim
            }
            Rectangle {
                x: 0
                y: win.clamp(win.hy, win.height)
                width: win.clamp(win.hx, win.width)
                height: win.clamp(win.hy + win.size, win.height) - y
                color: win.dim
            }
            Rectangle {
                x: win.clamp(win.hx + win.size, win.width)
                y: win.clamp(win.hy, win.height)
                width: win.width - x
                height: win.clamp(win.hy + win.size, win.height) - y
                color: win.dim
            }
            Canvas {
                x: win.hx
                y: win.hy
                width: win.size
                height: win.size
                onPaint: {
                    const ctx = getContext("2d");
                    const r = win.radius;
                    const g = ctx.createRadialGradient(r, r, 0, r, r, r);
                    const d = win.dim;
                    g.addColorStop(0.0, Qt.rgba(d.r, d.g, d.b, 0));
                    g.addColorStop(0.55, Qt.rgba(d.r, d.g, d.b, 0));
                    g.addColorStop(0.8, Qt.rgba(d.r, d.g, d.b, d.a * 0.55));
                    g.addColorStop(1.0, Qt.rgba(d.r, d.g, d.b, d.a));
                    ctx.clearRect(0, 0, width, height);
                    ctx.fillStyle = g;
                    ctx.fillRect(0, 0, width, height);
                }
            }
        }
    }
}

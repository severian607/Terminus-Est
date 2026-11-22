import Quickshell
import Quickshell.Wayland
import QtQuick
import QtQuick.Shapes

// Thin dashed lines through the pointer across the monitor it is on. Click-through.
ShellRoot {
    Variants {
        model: Quickshell.screens

        PanelWindow {
            id: win
            required property var modelData
            readonly property real lx: Cursor.x - modelData.x
            readonly property real ly: Cursor.y - modelData.y
            readonly property color line: Qt.rgba(Cursor.accent.r, Cursor.accent.g, Cursor.accent.b, 0.85)

            screen: modelData
            anchors { top: true; bottom: true; left: true; right: true }
            exclusionMode: ExclusionMode.Ignore
            WlrLayershell.layer: WlrLayer.Overlay
            WlrLayershell.namespace: "terminus-crosshair"
            color: "transparent"
            mask: Region {}

            Shape {
                visible: win.lx >= 0 && win.lx < win.width && win.ly >= 0 && win.ly < win.height
                x: 0
                y: Math.round(win.ly) - 1
                width: win.width
                height: 3
                ShapePath {
                    strokeColor: win.line
                    strokeWidth: 1
                    fillColor: "transparent"
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [8, 6]
                    startX: 0; startY: 1.5
                    PathLine { x: win.width; y: 1.5 }
                }
            }
            Shape {
                visible: win.lx >= 0 && win.lx < win.width && win.ly >= 0 && win.ly < win.height
                x: Math.round(win.lx) - 1
                y: 0
                width: 3
                height: win.height
                ShapePath {
                    strokeColor: win.line
                    strokeWidth: 1
                    fillColor: "transparent"
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [8, 6]
                    startX: 1.5; startY: 0
                    PathLine { x: 1.5; y: win.height }
                }
            }
        }
    }
}

import Quickshell
import Quickshell.Wayland
import QtQuick

// A pair of googly eyes in the bottom right corner of the main monitor, looking at the pointer. Click-through.
ShellRoot {
    id: root

    readonly property var screenOf: {
        const all = Quickshell.screens;
        for (let i = 0; i < all.length; i++)
            if (all[i].x === 0 && all[i].y === 0)
                return all[i];
        return all[0];
    }

    PanelWindow {
        id: win
        readonly property real margin: 28
        readonly property real eyeW: 96
        readonly property real eyeH: 120
        readonly property real pupil: 38
        // Window origin in global layout coordinates.
        readonly property real ox: screen.x + screen.width - margin - width
        readonly property real oy: screen.y + screen.height - margin - height

        screen: root.screenOf
        anchors { bottom: true; right: true }
        margins { bottom: margin; right: margin }
        implicitWidth: eyeW * 2 + 16
        implicitHeight: eyeH
        exclusionMode: ExclusionMode.Ignore
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "terminus-eyes"
        color: "transparent"
        mask: Region {}

        Repeater {
            model: 2

            Rectangle {
                id: eye
                required property int index
                x: index * (win.eyeW + 16)
                width: win.eyeW
                height: win.eyeH
                radius: width / 2
                color: "#f4f4f4"
                border.width: 4
                border.color: Cursor.accent

                readonly property real tx: (width - win.pupil) / 2 - 8
                readonly property real ty: (height - win.pupil) / 2 - 8
                readonly property real dx: Cursor.x - (win.ox + x + width / 2)
                readonly property real dy: Cursor.y - (win.oy + y + height / 2)
                readonly property real dist: Math.sqrt(dx * dx + dy * dy)
                readonly property real reach: Math.min(1, dist / 160)

                Rectangle {
                    width: win.pupil
                    height: win.pupil
                    radius: width / 2
                    color: "#111111"
                    x: eye.width / 2 - width / 2 + (eye.dist > 0 ? eye.dx / eye.dist * eye.tx * eye.reach : 0)
                    y: eye.height / 2 - height / 2 + (eye.dist > 0 ? eye.dy / eye.dist * eye.ty * eye.reach : 0)
                    Behavior on x { NumberAnimation { duration: 70 } }
                    Behavior on y { NumberAnimation { duration: 70 } }

                    Rectangle {
                        x: 9; y: 7
                        width: 10; height: 10
                        radius: 5
                        color: "#ffffff"
                    }
                }
            }
        }
    }
}

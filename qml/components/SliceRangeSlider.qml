/*
  harbour-3dmviewer — SliceRangeSlider.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  A vertical two-handle range slider, for peeling print layers off a sliced
  G-code model from the top and from the bottom — the control PrusaSlicer puts
  down the right of its preview.

  WHY NOT SILICA'S Slider
  -----------------------
  Silica's Slider is horizontal and has exactly one handle. Two handles on one
  track is the whole point here: "show layers 12 to 80" is one gesture, and
  faking it with two separate sliders means the user has to know which one is
  which. It is drawn from Rectangles and a MouseArea, which is also what keeps
  it working on Qt 5.6 with no Controls at all.

  DRAGGING RULE
  -------------
  The nearer handle wins the press and keeps the drag until release. Deciding
  again on every move would swap handles mid-gesture the moment they cross,
  which feels like the slider fighting the finger.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Item {
    id: root

    // Inclusive range the model exposes. `count` of 0 means the loaded format
    // has no slices at all and the caller should not be showing this.
    property int count: 0
    property int minimum: 0
    property int maximum: 0
    // Height in millimetres of the topmost shown layer, for the readout.
    property real topZ: 0.0

    signal rangeChanged(int newMin, int newMax)

    width: Theme.itemSizeSmall
    implicitHeight: Theme.itemSizeLarge * 5

    // Position along the track, 0 at the bottom. Guarded against a
    // single-layer model, which would otherwise divide by zero.
    function _yFor(value) {
        var span = Math.max(1, count - 1)
        return track.height - (value / span) * track.height
    }
    function _valueAt(y) {
        var span = Math.max(1, count - 1)
        var t = 1.0 - Math.max(0, Math.min(1, y / track.height))
        return Math.round(t * span)
    }

    Rectangle {
        id: track
        anchors {
            top: parent.top; bottom: parent.bottom
            horizontalCenter: parent.horizontalCenter
            topMargin: Theme.paddingLarge
            bottomMargin: Theme.paddingLarge
        }
        width: Theme.paddingSmall
        radius: width / 2
        color: Qt.rgba(1, 1, 1, 0.18)

        // The part of the track that is actually being drawn.
        Rectangle {
            x: 0
            width: parent.width
            radius: parent.radius
            y: root._yFor(root.maximum)
            height: Math.max(2, root._yFor(root.minimum) - y)
            color: Theme.highlightColor
        }
    }

    Repeater {
        model: 2

        // index 0 = the bottom handle (minimum), 1 = the top handle (maximum).
        Rectangle {
            width: Theme.itemSizeExtraSmall / 2
            height: width
            radius: width / 2
            color: Theme.highlightColor
            border.color: Qt.rgba(0, 0, 0, 0.5)
            border.width: 1
            x: (root.width - width) / 2
            y: track.y + root._yFor(index === 0 ? root.minimum : root.maximum)
               - height / 2
        }
    }

    // The readout only appears while dragging: a permanent label would sit on
    // top of the model for the whole session to say something the user only
    // cares about for the second they are moving the handle.
    Rectangle {
        visible: dragArea.dragging
        anchors {
            left: parent.right
            verticalCenter: parent.verticalCenter
        }
        width: readout.width + 2 * Theme.paddingMedium
        height: readout.height + Theme.paddingSmall
        radius: Theme.paddingSmall
        color: Qt.rgba(0, 0, 0, 0.7)

        Label {
            id: readout
            anchors.centerIn: parent
            //: Slice slider readout: layer range and the height of the top one
            text: qsTr("%1 – %2 · %3 mm")
                .arg(root.minimum + 1).arg(root.maximum + 1)
                .arg(root.topZ.toFixed(2))
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.highlightColor
        }
    }

    MouseArea {
        id: dragArea
        anchors.fill: parent
        // The 3D view below wants this drag too, and it must not get it.
        preventStealing: true
        enabled: root.count > 1

        property bool dragging: false
        property bool draggingTop: false

        onPressed: {
            var y = mouse.y - track.y
            var dTop = Math.abs(y - root._yFor(root.maximum))
            var dBottom = Math.abs(y - root._yFor(root.minimum))
            draggingTop = dTop <= dBottom
            dragging = true
            _apply(y)
        }
        onPositionChanged: if (dragging) _apply(mouse.y - track.y)
        onReleased: dragging = false
        onCanceled: dragging = false

        function _apply(y) {
            var v = root._valueAt(y)
            if (draggingTop)
                root.rangeChanged(Math.min(root.minimum, v), v)
            else
                root.rangeChanged(v, Math.max(root.maximum, v))
        }
    }
}

/*
  harbour-3dmviewer — DropPage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Swipe left from the viewer and the model is shown still for a second, then
  let go of. There is no button: arriving on the page IS the trigger, and
  leaving it puts the model back, so swiping away and back replays the fall.

  Gravity here is measured RELATIVE to however the phone is held at the moment
  of release, so the part always starts by falling towards the bottom of the
  screen. Tilt from there and it slides, topples, and lands on another face.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0
import harbour.threedmviewer 1.0

Page {
    id: page

    // Rotation is LOCKED here. Tilting the phone is the control, and Sailfish's
    // auto-rotation reacts to that same tilt: the UI would flip a quarter turn
    // mid-gesture and "down" would jump to a different screen edge, so the
    // interaction would fight itself.
    allowedOrientations: Orientation.Portrait

    // CAMERA CUTOUT
    // The hint below is the only chrome on this page and it sits at the very
    // top edge, where a wide cutout (Jolla J2) cuts through it. No orientation
    // condition, unlike ViewerPage: this page is locked to portrait, so the
    // cutout is at its top whatever the phone is doing. Zero on a phone
    // without one. The model behind it is deliberately not inset.
    readonly property real topInset: Screen.topCutout.height

    onStatusChanged: {
        if (status === PageStatus.Active) {
            dropView.armAutoDrop()
            if (!_attached) {
                _attached = true
                pageStack.pushAttached(Qt.resolvedUrl("TrampolinePage.qml"))
            }
        } else if (status === PageStatus.Deactivating) {
            // Stop simulating the moment the page is left: the timer would
            // otherwise keep running, and repainting, behind another page.
            dropView.cancelDrop()
        }
    }
    property bool _attached: false

    ModelView {
        id: dropView
        anchors.fill: parent
        loader: modelLoader
        settings: physics
        mode: ModelView.Drop
    }

    // No finger control at all — the phone's tilt is the whole interaction.
    // The label says so, because a screen that ignores every touch otherwise
    // just looks broken.
    Label {
        anchors {
            top: parent.top
            horizontalCenter: parent.horizontalCenter
            topMargin: Theme.paddingLarge + page.topInset
        }
        text: dropView.tiltAvailable ? qsTr("Tilt the phone")
                                     : qsTr("No accelerometer — it can only fall straight down")
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.secondaryHighlightColor
        opacity: 0.7
    }
}

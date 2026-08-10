/*
  harbour-3dmviewer — TrampolinePage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  One more swipe left: the same model, but the room is a shaft five times
  deeper and its floor is rubber.

  WHY THE PART GETS SMALLER, AND WHY THERE IS NO SCALING CODE
  ----------------------------------------------------------
  Gravity here is taken ABSOLUTELY, not relative to how the phone is held —
  that is the one thing that separates this page from the drop page. Hold the
  phone flat and real gravity points into the screen, so the part falls away
  from you down the shaft. Because the view is a perspective projection,
  falling away IS shrinking and bouncing back IS growing. Nothing scales the
  model; it is simply further away.

  Lay the phone flat on a table to get the full effect.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0
import harbour.threedmviewer 1.0

Page {
    id: page

    // Locked for the same reason as the drop page: auto-rotation would fight
    // the tilt that is meant to steer the fall.
    allowedOrientations: Orientation.Portrait

    property bool _attached: false
    onStatusChanged: {
        if (status === PageStatus.Active) {
            bounceView.armAutoDrop()
            // One more swipe to the left leads to the workbench when it has
            // been switched on in About, and otherwise straight back round to
            // the viewer — this is the last page of the ring without it.
            if (!_attached) {
                _attached = true
                pageStack.pushAttached(Qt.resolvedUrl(
                    physics.enabled ? "PhysicsPage.qml" : "ViewerPage.qml"))
            }
        } else if (status === PageStatus.Deactivating) {
            bounceView.cancelDrop()
        }
    }

    ModelView {
        id: bounceView
        anchors.fill: parent
        loader: modelLoader
        settings: physics
        mode: ModelView.Trampoline
    }

    Label {
        anchors {
            top: parent.top
            horizontalCenter: parent.horizontalCenter
            topMargin: Theme.paddingLarge
        }
        //: Hint on the trampoline page. Flat is the interesting case: gravity
        //: then points into the screen and the model falls away down the shaft.
        text: qsTr("Lay the phone flat")
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.secondaryHighlightColor
        opacity: 0.7
    }
}

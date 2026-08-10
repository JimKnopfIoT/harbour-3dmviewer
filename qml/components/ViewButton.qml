/*
  harbour-3dmviewer — ViewButton.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  One button in the viewer's bottom toolbar. Deliberately flatter and smaller
  than a Silica Button: several of them sit over live 3D, and full-weight
  buttons there would read as a dialog rather than an overlay.

  It carries no behaviour of its own — the parent decides what `activated()`
  means — so the same control serves both the camera presets and the drop
  toggle without either knowing about the other.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

MouseArea {
    id: root

    property alias label: labelItem.text
    // Drawn as engaged. Used by the drop button so the model's state is visible
    // at a glance rather than only from the label text.
    property bool active: false

    signal activated()

    // paddingMedium, not Large: the toolbar carries six of these and at the
    // wider spacing the row ran off the edge of a portrait screen.
    width: labelItem.width + 2 * Theme.paddingMedium
    height: Theme.itemSizeExtraSmall

    onClicked: root.activated()

    Rectangle {
        anchors.fill: parent
        radius: Theme.paddingSmall
        color: root.pressed ? Theme.rgba(Theme.highlightBackgroundColor, 0.55)
             : root.active  ? Theme.rgba(Theme.highlightBackgroundColor, 0.35)
                            : Qt.rgba(0, 0, 0, 0.45)
    }

    Label {
        id: labelItem
        anchors.centerIn: parent
        font.pixelSize: Theme.fontSizeSmall
        color: (root.pressed || root.active) ? Theme.highlightColor : Theme.primaryColor
    }
}

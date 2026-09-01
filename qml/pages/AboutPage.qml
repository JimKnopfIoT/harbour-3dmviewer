/*
  harbour-3dmviewer — AboutPage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page
    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("About") }

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "image://theme/icon-launcher-default"
                sourceSize.width: Theme.iconSizeLarge
                sourceSize.height: Theme.iconSizeLarge
                visible: false   // placeholder slot; the icon is the app's own
            }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                text: qsTr("3DM Viewer %1").arg(appVersion)
                font.pixelSize: Theme.fontSizeLarge
                color: Theme.highlightColor
            }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("A viewer for CAD models and sliced G-code. Open a file, "
                         + "turn it around, switch layers on and off. There is no "
                         + "editing and no save path — the app never writes a model "
                         + "file.")
            }

            SectionHeader { text: qsTr("Developer") }

            TextSwitch {
                text: qsTr("Physics workbench")
                description: qsTr("Adds a page after the deep room with a slider "
                                + "for every value the falling and bouncing runs "
                                + "on. Off by default — it is a workbench, not a "
                                + "feature.")
                checked: physics.enabled
                onClicked: physics.enabled = !physics.enabled
            }

            SectionHeader { text: qsTr("How to use it") }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Reads .3dm, .stp/.step, .stl, .3mf and .bgcode.\n\n"
                         + "Drag with one finger to turn the model.\n"
                         + "Pinch to zoom.\n"
                         + "Double tap to fit the model to the screen.\n"
                         + "Swipe up from the bottom edge for the view buttons.\n"
                         + "Pull down for the layer list and the model info.\n\n"
                         + "A sliced G-code file gets a slider down the left edge "
                         + "that peels print layers away from the top and the "
                         + "bottom.")
            }

            SectionHeader { text: qsTr("Limitations") }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                text: qsTr("Rhino: surfaces are drawn from the render meshes saved "
                         + "inside the file. A model saved with \"Save Small\" has "
                         + "none, and its surfaces cannot be shown. Block instances, "
                         + "curves and annotation are not drawn.\n\n"
                         + "STEP: faces on a plane or a cylinder are tessellated on "
                         + "the phone. A face on any other surface type is reported "
                         + "rather than drawn.\n\n"
                         + "3MF: beam lattices, slice stacks and textures are "
                         + "skipped.\n\n"
                         + "Whatever cannot be shown is said out loud, never left "
                         + "silently missing.")
            }

            SectionHeader { text: qsTr("Credits and licence") }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                text: qsTr("Reads .3dm using the openNURBS toolkit by Robert McNeel "
                         + "& Associates (zlib licence), compiled in statically. The "
                         + "STEP, STL, 3MF and binary G-code readers are written for "
                         + "this app.\n\n"
                         + "Not affiliated with or endorsed by Robert McNeel & "
                         + "Associates. \"Rhinoceros\" and the Rhino logo are their "
                         + "trademarks and are not used here. Not affiliated with or "
                         + "endorsed by Prusa Research.\n\n"
                         + "This program is free software under the GNU General "
                         + "Public License, version 3 or later.")
            }
        }

        VerticalScrollDecorator { }
    }
}

/*
  harbour-3dmviewer — ModelInfoPage.qml
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

            PageHeader {
                //: Page title: details about the loaded file
                title: qsTr("Model info")
            }

            DetailItem {
                label: qsTr("File")
                value: modelLoader.fileName
            }
            DetailItem {
                label: qsTr("Format")
                value: modelLoader.formatName
            }
            DetailItem {
                //: The CAD application that wrote the file
                label: qsTr("Written by")
                value: modelLoader.producer.length > 0 ? modelLoader.producer
                                                       : qsTr("unknown")
            }
            DetailItem {
                label: qsTr("Objects")
                value: modelLoader.objectCount
            }
            DetailItem {
                label: qsTr("Triangles")
                value: modelLoader.triangleCount
                // A sliced file has no surfaces at all, so a triangle count of
                // zero there is the truth and not worth a row.
                visible: modelLoader.triangleCount > 0
            }
            DetailItem {
                //: Drawn toolpath moves in a sliced G-code file
                label: qsTr("Segments")
                value: modelLoader.lineCount
                visible: modelLoader.lineCount > 0
            }
            DetailItem {
                //: Print layers in a sliced G-code file
                label: qsTr("Print layers")
                value: modelLoader.sliceCount
                visible: modelLoader.sliceCount > 0
            }
            DetailItem {
                label: qsTr("Vertices")
                value: modelLoader.vertexCount
            }
            DetailItem {
                label: qsTr("Layers")
                value: modelLoader.layers.count
            }
            DetailItem {
                //: Bounding box size of the model in the file's own units
                label: qsTr("Size")
                value: modelLoader.sizeSummary
            }

            SectionHeader {
                text: qsTr("Notes")
                visible: modelLoader.warnings.length > 0
            }

            Repeater {
                model: modelLoader.warnings
                Label {
                    x: Theme.horizontalPageMargin
                    width: page.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryHighlightColor
                    text: modelData
                }
            }

            Item { width: 1; height: Theme.paddingLarge }

            Label {
                x: Theme.horizontalPageMargin
                width: page.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: modelLoader.filePath
            }
        }

        VerticalScrollDecorator { }
    }
}

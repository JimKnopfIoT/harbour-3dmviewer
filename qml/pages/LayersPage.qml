/*
  harbour-3dmviewer — LayersPage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Rhino layers, one switch each.

  The switches write straight through to the loaded scene, so the view behind
  this page updates live — swipe back and the model is already right. That is
  why there is no "apply" button and no local copy of the state.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page
    allowedOrientations: Orientation.All

    SilicaListView {
        id: listView
        anchors.fill: parent
        model: modelLoader.layers

        header: PageHeader {
            //: Page title: the Rhino layer list
            title: qsTr("Layers")
            description: qsTr("%1 of %2 visible")
                .arg(modelLoader.layers.visibleCount)
                .arg(modelLoader.layers.count)
        }

        PullDownMenu {
            MenuItem {
                //: Restore the layer visibility stored in the file itself
                text: qsTr("Reset to file")
                onClicked: modelLoader.layers.resetToFileDefaults()
            }
            MenuItem {
                text: qsTr("Hide all")
                onClicked: modelLoader.layers.hideAll()
            }
            MenuItem {
                text: qsTr("Show all")
                onClicked: modelLoader.layers.showAll()
            }
        }

        delegate: ListItem {
            id: delegateItem
            contentHeight: Theme.itemSizeSmall

            onClicked: modelLoader.layers.toggle(index)

            Rectangle {
                id: swatch
                anchors {
                    left: parent.left
                    leftMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                width: Theme.iconSizeExtraSmall
                height: width
                radius: width / 4
                color: layerColor
                // Rhino's default layer colour is black, which would vanish
                // against the dark theme without an outline.
                border.width: 1
                border.color: Theme.rgba(Theme.primaryColor, 0.4)
                opacity: layerVisible ? 1.0 : 0.3
            }

            Column {
                anchors {
                    left: swatch.right
                    leftMargin: Theme.paddingMedium
                    right: visibleSwitch.left
                    rightMargin: Theme.paddingMedium
                    verticalCenter: parent.verticalCenter
                }

                Label {
                    width: parent.width
                    text: layerName
                    truncationMode: TruncationMode.Fade
                    color: layerVisible ? (delegateItem.highlighted ? Theme.highlightColor
                                                                    : Theme.primaryColor)
                                        : Theme.secondaryColor
                }
                Label {
                    width: parent.width
                    text: qsTr("%1 triangles").arg(triangleCount)
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                }
            }

            Switch {
                id: visibleSwitch
                anchors {
                    right: parent.right
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                checked: layerVisible
                automaticCheck: false
                onClicked: modelLoader.layers.toggle(index)
            }
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No layers")
            hintText: qsTr("Open a Rhino model to see its layers.")
        }

        VerticalScrollDecorator { }
    }
}

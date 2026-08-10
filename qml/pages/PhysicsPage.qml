/*
  harbour-3dmviewer — PhysicsPage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The workbench: every number the two simulations run on, with its value on
  screen and a slider under it.

  The list is built from PhysicsSettings::parameters(), not written out here, so
  adding a knob is one row in the table on the C++ side and nothing at all in
  this file.

  Moving a slider re-arms the drop on the matching page. That is deliberate:
  several of these values are only read when the room is built, so letting a
  part carry on falling would show a mixture of the old settings and the new.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0
import harbour.threedmviewer 1.0
import "../components"

Page {
    id: page
    allowedOrientations: Orientation.All

    // 1 = the stiff box, 2 = the deep room. Matches ModelView.Drop/Trampoline.
    property int targetPage: 2

    function reload() {
        paramModel.clear()
        var list = physics.parameters(page.targetPage)
        for (var i = 0; i < list.length; ++i)
            paramModel.append(list[i])
    }

    Component.onCompleted: reload()
    onTargetPageChanged: reload()

    // Closes the ring: one more swipe left comes back round to the viewer.
    //
    // Silica's page stack is a chain, not a ring, so "round again" is a FRESH
    // viewer pushed after this one rather than the original coming back. Going
    // forward therefore always works; going backwards walks the chain the way
    // it was built, which after one lap is the same thing in both directions.
    property bool _attached: false
    onStatusChanged: {
        if (status === PageStatus.Active && !_attached) {
            _attached = true
            pageStack.pushAttached(Qt.resolvedUrl("ViewerPage.qml"))
        }
    }

    ListModel { id: paramModel }

    SilicaListView {
        id: listView
        anchors.fill: parent
        model: paramModel

        header: Column {
            width: listView.width

            PageHeader {
                title: qsTr("Physics")
                description: page.targetPage === 1 ? qsTr("Page 2 — the stiff box")
                                                   : qsTr("Page 3 — the deep room")
            }

            // Which page's numbers are being edited. Two plain buttons rather
            // than a combo: there are exactly two, and both should be one tap
            // away while a model is bouncing on the other screen.
            //
            // ViewButton rather than Silica's Button: it already draws an
            // engaged state, and Button's `highlighted` is not available on
            // this Qt. No padding properties either — Row gained those in Qt
            // 5.12, and using one here was what stopped this whole page from
            // loading.
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.paddingLarge

                ViewButton {
                    label: qsTr("Box")
                    active: page.targetPage === 1
                    onActivated: page.targetPage = 1
                }
                ViewButton {
                    label: qsTr("Deep room")
                    active: page.targetPage === 2
                    onActivated: page.targetPage = 2
                }
            }

            Item { width: 1; height: Theme.paddingLarge }
        }

        PullDownMenu {
            MenuItem {
                text: qsTr("Reset this page to defaults")
                onClicked: {
                    physics.resetDefaults(page.targetPage)
                    page.reload()
                }
            }
        }

        delegate: Slider {
            width: listView.width
            minimumValue: model.min
            maximumValue: model.max
            stepSize: model.step
            value: model.value
            label: model.label
            // The number is the whole point of the page, so it is always shown
            // rather than only while dragging.
            valueText: model.value.toFixed(model.decimals) + (model.unit.length ? " " + model.unit : "")

            onValueChanged: {
                if (Math.abs(value - model.value) < model.step * 0.25)
                    return
                model.value = value
                physics.setValue(page.targetPage, model.key, value)
            }
        }

        VerticalScrollDecorator { }
    }
}

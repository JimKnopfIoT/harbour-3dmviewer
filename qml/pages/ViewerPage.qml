/*
  harbour-3dmviewer — ViewerPage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The viewer. The 3D fills the entire page; the header and the toolbar float
  over it.

  TWO WAYS OF LOOKING
  -------------------
  X / Y / Z lock the camera to a world axis. In those the model cannot be
  turned at all — the fingers only pan and zoom — so they behave like drawings.
  Pressing the axis already in use turns the view a further quarter turn about
  it. 3D is the only mode that orbits freely.

  WHY THE PULLEY STILL NEEDS A STRIP OF ITS OWN
  ---------------------------------------------
  Silica's pull-down menu is opened by dragging down from the top, the same
  gesture that orbits the model. A MouseArea with preventStealing covering the
  full page would swallow the pulley and leave the app with no menu at all, so
  the header strip stays outside the interaction area and owns that drag.
  Everything below it belongs to the model. Same split MapPage.qml uses in
  harbour-catchercatcher for its map canvas.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import harbour.threedmviewer 1.0
import "../components"

Page {
    id: page
    allowedOrientations: Orientation.All

    // Swiping left leads to the drop page, and from there to the trampoline.
    // Attached once, on first activation: pushing it again on every visit would
    // stack a new copy of the page each time.
    property bool _attached: false
    onStatusChanged: {
        if (status === PageStatus.Active && !_attached) {
            _attached = true
            pageStack.pushAttached(Qt.resolvedUrl("DropPage.qml"))
        }
    }

    // --- the model, edge to edge ------------------------------------------
    // Deliberately outside the flickable and anchored to the whole page, so the
    // background reaches every corner and the chrome floats over it.
    ModelView {
        id: modelView
        anchors.fill: parent
        loader: modelLoader
        settings: physics
        visible: modelLoader.hasModel
    }

    SilicaFlickable {
        anchors.fill: parent
        // Never scrolls: it exists purely to host the pull-down menu.
        contentHeight: height

        PullDownMenu {
            busy: modelLoader.busy

            MenuItem {
                text: qsTr("About")
                onClicked: pageStack.push(Qt.resolvedUrl("AboutPage.qml"))
            }
            MenuItem {
                //: Pulley: information about the loaded file
                text: qsTr("Model info")
                enabled: modelLoader.hasModel
                onClicked: pageStack.push(Qt.resolvedUrl("ModelInfoPage.qml"))
            }
            MenuItem {
                //: Pulley: reset the camera so the whole model is visible
                text: qsTr("Fit to view")
                enabled: modelLoader.hasModel
                onClicked: modelView.fitToView()
            }
            MenuItem {
                //: Pulley: choose a model file
                text: qsTr("Open model…")
                onClicked: pageStack.push(filePickerComponent)
            }
        }

        // ------------------------------------------------------- header ---
        Item {
            id: headerStrip
            anchors { top: parent.top; left: parent.left; right: parent.right }
            height: Theme.itemSizeLarge

            Column {
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: Theme.horizontalPageMargin
                    rightMargin: Theme.horizontalPageMargin
                }

                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    text: modelLoader.hasModel ? modelLoader.fileName
                                               : qsTr("3DM Viewer")
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeLarge
                }
                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    visible: modelLoader.hasModel
                    //: Header line: format, how much geometry, how many of the
                    //: file's layers are switched on
                    text: qsTr("%1 · %2 · %3 layers visible")
                        .arg(modelLoader.formatName)
                        .arg(modelLoader.geometrySummary)
                        .arg(modelLoader.layers.visibleCount)
                    color: Theme.secondaryHighlightColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }
        }

        // --------------------------------------------------- slice range ---
        // Only a sliced format has layers to peel; a CAD model has none and
        // the slider does not exist for it. It sits on the left because the
        // right edge is where a thumb rests when the other hand is holding
        // the phone, and that is the hand that orbits.
        SliceRangeSlider {
            id: sliceSlider
            visible: modelView.sliceCount > 1
            anchors {
                left: parent.left
                top: headerStrip.bottom
                bottom: parent.bottom
                leftMargin: Theme.paddingSmall
                topMargin: Theme.paddingLarge
                bottomMargin: Theme.itemSizeSmall
            }
            count: modelView.sliceCount
            minimum: modelView.sliceMin
            maximum: modelView.sliceMax
            topZ: modelView.sliceTopZ
            onRangeChanged: {
                // Max first: setSliceMin pushes the top handle up when the two
                // would cross, so writing min last cannot undo a deliberate
                // move of the top one.
                modelView.sliceMax = newMax
                modelView.sliceMin = newMin
            }
        }

        // ----------------------------------------------------- gestures ---
        PinchArea {
            anchors {
                top: headerStrip.bottom
                left: sliceSlider.visible ? sliceSlider.right : parent.left
                right: parent.right; bottom: parent.bottom
            }
            enabled: modelLoader.hasModel

            property real lastScale: 1.0
            onPinchStarted: lastScale = 1.0
            onPinchUpdated: {
                // Relative step, not absolute scale: absolute would snap the
                // zoom back to the gesture's origin every time a finger is
                // lifted and replaced.
                var step = pinch.scale / lastScale
                lastScale = pinch.scale
                modelView.zoomBy(step)
                toolbar.poke()
            }

            MouseArea {
                id: dragArea
                anchors.fill: parent
                preventStealing: true
                enabled: modelLoader.hasModel

                property real lastX: 0
                property real lastY: 0

                onPressed: {
                    lastX = mouse.x
                    lastY = mouse.y
                }
                onPositionChanged: {
                    var dx = mouse.x - lastX
                    var dy = mouse.y - lastY
                    lastX = mouse.x
                    lastY = mouse.y

                    // In the locked view the camera is fixed: a drag can only
                    // slide the model about, never turn it. Turning happens in
                    // quarter steps from the buttons. Only 3D orbits freely.
                    if (modelView.axisLocked)
                        modelView.panBy(dx, dy)
                    else
                        modelView.orbit(dx, dy)
                    toolbar.poke()
                }
                onDoubleClicked: modelView.fitToView()
            }
        }

        // ------------------------------------------------- empty / busy ---
        Column {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.horizontalPageMargin
            spacing: Theme.paddingLarge
            visible: !modelLoader.hasModel && !modelLoader.busy

            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.secondaryHighlightColor
                text: modelLoader.errorString.length > 0
                      ? modelLoader.errorString
                      : qsTr("No model loaded")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Open model…")
                onClicked: pageStack.push(filePickerComponent)
            }
        }

        BusyIndicator {
            anchors.centerIn: parent
            size: BusyIndicatorSize.Large
            running: modelLoader.busy
        }

        ProgressBar {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            visible: modelLoader.busy
            minimumValue: 0.0
            maximumValue: 1.0
            value: modelLoader.progress
        }
    }

    // --- warnings ----------------------------------------------------------
    // Soft problems (missing render meshes, skipped blocks, GPU limits) are
    // shown here rather than in a dialog: the model IS on screen, it is just
    // incomplete, and a modal would hide what did load.
    Rectangle {
        id: warningBanner
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        anchors.bottomMargin: toolbar.shown ? toolbar.height : 0
        height: warningLabel.height + 2 * Theme.paddingMedium
        color: Qt.rgba(0, 0, 0, 0.65)
        visible: warningLabel.text.length > 0 && !modelLoader.busy

        Label {
            id: warningLabel
            anchors {
                left: parent.left; right: parent.right
                verticalCenter: parent.verticalCenter
                leftMargin: Theme.horizontalPageMargin
                rightMargin: Theme.horizontalPageMargin
            }
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.highlightColor
            text: {
                var parts = []
                for (var i = 0; i < modelLoader.warnings.length; ++i)
                    parts.push(modelLoader.warnings[i])
                if (modelView.renderWarning.length > 0)
                    parts.push(modelView.renderWarning)
                return parts.join("\n")
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: warningBanner.visible = false
        }
    }

    // --- toolbar -----------------------------------------------------------
    // Hidden until swiped up from the bottom edge, and it slides away again
    // after three seconds of nothing happening. The model is the content; the
    // controls are a visitor.
    Item {
        id: toolbar
        anchors { left: parent.left; right: parent.right }
        height: buttonRow.height + 2 * Theme.paddingMedium
        y: shown ? parent.height - height : parent.height
        visible: modelLoader.hasModel

        property bool shown: false

        // Called by anything that should keep the bar alive. It only restarts
        // the clock, never brings the bar back — so handling the model does not
        // make the chrome reappear.
        function poke() {
            if (shown)
                hideTimer.restart()
        }
        function reveal() {
            shown = true
            hideTimer.restart()
        }

        Behavior on y {
            NumberAnimation { duration: 220; easing.type: Easing.InOutQuad }
        }

        Timer {
            id: hideTimer
            interval: 3000
            onTriggered: toolbar.shown = false
        }

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.55)
        }

        Row {
            id: buttonRow
            anchors.centerIn: parent
            spacing: Theme.paddingSmall

            ViewButton {
                label: "X"
                onActivated: { modelView.stepAxis(0); toolbar.reveal() }
            }
            ViewButton {
                label: "Y"
                onActivated: { modelView.stepAxis(1); toolbar.reveal() }
            }
            ViewButton {
                label: "Z"
                onActivated: { modelView.stepAxis(2); toolbar.reveal() }
            }
            ViewButton {
                label: qsTr("3D")
                active: modelView.axisLocked
                onActivated: { modelView.setFreeView(); toolbar.reveal() }
            }
            ViewButton {
                //: Toolbar: cycles solid / wireframe / see-through, as in Rhino
                label: modelView.displayMode === ModelView.Wireframe ? qsTr("Wire")
                     : modelView.displayMode === ModelView.Ghosted   ? qsTr("Ghost")
                                                                    : qsTr("Solid")
                active: modelView.displayMode !== ModelView.Shaded
                onActivated: { modelView.cycleDisplayMode(); toolbar.reveal() }
            }
            ViewButton {
                //: Toolbar: opens the Rhino layer list
                label: qsTr("Layers")
                onActivated: {
                    toolbar.reveal()
                    pageStack.push(Qt.resolvedUrl("LayersPage.qml"))
                }
            }
        }
    }

    // The grab strip along the very bottom. It sits above the model's gesture
    // area, so an upward flick there always reaches the toolbar however the
    // model is being dragged elsewhere. It collapses once the bar is out, so it
    // never shadows the buttons it just revealed.
    MouseArea {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: toolbar.shown ? 0 : Theme.itemSizeSmall
        enabled: !toolbar.shown && modelLoader.hasModel
        preventStealing: true

        property real startY: 0
        onPressed: startY = mouse.y
        onPositionChanged: {
            if (startY - mouse.y > Theme.paddingLarge)
                toolbar.reveal()
        }
        onClicked: toolbar.reveal()
    }

    Component {
        id: filePickerComponent
        FilePickerPage {
            //: File picker title
            title: qsTr("Select a model")
            nameFilters: [ "*.3dm", "*.stp", "*.step", "*.stl", "*.3mf", "*.bgcode" ]
            onSelectedContentPropertiesChanged: {
                modelLoader.openPath(selectedContentProperties.filePath)
            }
        }
    }
}

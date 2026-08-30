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

  AND THREE WAYS OF TURNING
  -------------------------
  Within 3D, one button cycles Centre / Point / Free — about the screen centre,
  about a tapped spot on the model, or free with a two-finger roll. Pressing
  once more returns to Centre, which is a full reset: framed and centred at the
  opening perspective, roll dropped, turning point forgotten. That return is why
  it is a cycle and not a switch, and why free rotation is safe to offer at all;
  camera.h argues against it and is still right about the default.

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

    // CAMERA CUTOUT
    // Silica keeps its own PageHeader clear of a camera cutout by itself
    // (`_minimumTopMargin` in PageHeader.qml). The strip below is this page's
    // own header, so it has to be told: on a phone with a wide cutout (Jolla
    // J2) the notch edge cut straight through the file name. Portrait only,
    // as Silica has it — in landscape the cutout is at the side and a top
    // margin would be a gap for nothing. On a phone without one the rectangle
    // is empty and this is zero, so it costs the other devices nothing.
    //
    // The picture underneath is not inset on purpose: it is a picture, and
    // cropping it would cost more than the corner the notch hides.
    readonly property real topInset: orientation === Orientation.Portrait
                                     ? Screen.topCutout.height : 0

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
            anchors {
                top: parent.top; left: parent.left; right: parent.right
                topMargin: page.topInset
            }
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

        // ------------------------------------------------ turning point ---
        // Chrome, not geometry: it keeps its size on screen at any zoom.
        Item {
            id: pivotMark
            visible: modelView.pivotSet
                     && modelView.rotationMode !== ModelView.RotateCentred
                     && modelLoader.hasModel
            x: modelView.pivotScreenPos.x - width / 2
            y: modelView.pivotScreenPos.y - height / 2
            width: Theme.itemSizeExtraSmall
            height: width
            z: 1

            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.5; height: width
                radius: width / 2
                color: "transparent"
                border { width: 2; color: Theme.highlightColor }
                opacity: 0.9
            }
            Rectangle {
                anchors.centerIn: parent
                width: parent.width; height: 2
                color: Theme.highlightColor
                opacity: 0.6
            }
            Rectangle {
                anchors.centerIn: parent
                width: 2; height: parent.height
                color: Theme.highlightColor
                opacity: 0.6
            }
        }

        // Says what to do in the one state that needs the user to act before
        // anything happens. It disappears the moment they have.
        Label {
            visible: modelView.rotationMode === ModelView.RotateAboutPivot
                     && !modelView.pivotSet && modelLoader.hasModel
            anchors {
                horizontalCenter: parent.horizontalCenter
                bottom: parent.bottom
                bottomMargin: toolbar.height + Theme.paddingLarge
            }
            //: Shown after switching to "turn about a point": the user has to
            //: choose that point before the state does anything.
            text: qsTr("Tap the model to set the turning point")
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.highlightColor
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
            property real lastRotation: 0.0
            onPinchStarted: { lastScale = 1.0; lastRotation = 0.0 }
            onPinchUpdated: {
                // Relative step, not absolute scale: absolute would snap the
                // zoom back to the gesture's origin every time a finger is
                // lifted and replaced.
                var step = pinch.scale / lastScale
                lastScale = pinch.scale
                modelView.zoomBy(step)

                // The twist, for the same reason relative. Only free rotation
                // can roll — rollBy is a no-op in the other two states, so
                // there is nothing to guard here and nothing that silently
                // half-works either.
                var turn = pinch.rotation - lastRotation
                lastRotation = pinch.rotation
                if (turn !== 0.0)
                    modelView.rollBy(turn)

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
                // Only in the pivot state. Elsewhere a single tap has never
                // done anything, and a stray tap should not move a picture.
                onClicked: {
                    if (modelView.rotationMode === ModelView.RotateAboutPivot) {
                        modelView.pickPivot(mouse.x, mouse.y)
                        toolbar.reveal()
                    }
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

    // --- the hint that the bottom edge is worth touching --------------------
    /*
      The toolbar hides itself, and a control nobody knows about might as well
      not exist — so the edge fogs while it is away.

      A shader because every quantity is per-pixel: reach, density and hue all
      vary along the edge. Sums of sines at frequencies that do not divide into
      each other drive the three separately, so none of them beats together. All
      temporal frequencies are whole multiples of the clocks, which run 0..2*pi,
      so the loop closes without a jump.

      Animated only while visible: it keeps the scene graph repainting, which is
      a poor trade for a hint nobody needs once the bar is up.
    */
    Item {
        id: edgeGlow
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        // Reach below is a fraction of this, so it raises everything at once.
        height: Theme.itemSizeMedium
        visible: modelLoader.hasModel && !toolbar.shown

        // t is the slow drift of shape and colour; p is the gusting.
        property real t: 0.0
        property real p: 0.0
        NumberAnimation on t {
            running: edgeGlow.visible
            from: 0.0; to: 2.0 * Math.PI
            duration: 480000          // fog does not hurry
            loops: Animation.Infinite
        }
        NumberAnimation on p {
            running: edgeGlow.visible
            from: 0.0; to: 2.0 * Math.PI
            duration: 14000
            loops: Animation.Infinite
        }

        ShaderEffect {
            anchors.fill: parent
            property real t: edgeGlow.t
            property real p: edgeGlow.p
            fragmentShader: "
                varying highp vec2 qt_TexCoord0;
                uniform lowp float qt_Opacity;
                uniform highp float t;   // slow: shape and colour
                uniform highp float p;   // fast: the gusts

                highp float wob(highp float x, highp float f1, highp float f2,
                                highp float f3, highp float p)
                {
                    return 0.5 + 0.28 * sin(x * f1 + t + p)
                               + 0.14 * sin(x * f2 - t * 2.0 + p * 1.7)
                               + 0.08 * sin(x * f3 + t * 3.0 - p * 0.6);
                }

                lowp vec3 hsv2rgb(highp float h, lowp float s, lowp float v)
                {
                    highp vec3 k = fract(vec3(h) + vec3(0.0, 0.6666667, 0.3333333));
                    return v * mix(vec3(1.0), clamp(abs(k * 6.0 - 3.0) - 1.0, 0.0, 1.0), s);
                }

                void main()
                {
                    highp float x = qt_TexCoord0.x;
                    highp float up = 1.0 - qt_TexCoord0.y;   // 0 at the screen edge

                    // How high this gust reaches here; the second factor varies
                    // it from one gust to the next.
                    highp float reach = (0.12 + 0.42 * wob(x, 5.0, 11.0, 19.0, 0.0))
                                      * (0.45 + 0.95 * (0.5 + 0.5 * sin(p + x * 2.1)));
                    highp float d = clamp(1.0 - up / max(reach, 0.02), 0.0, 1.0);
                    d = d * d;

                    // Reaches zero in places, so the bottom edge has gaps in it
                    // instead of being one unbroken coloured bar.
                    // Slid along by the gust clock, so no two gusts look alike.
                    d *= clamp(1.30 * wob(x + 0.30 * sin(p), 3.0, 7.0, 13.0, 2.1)
                               - 0.32, 0.0, 1.0);

                    // Clamped hard to zero, so there are stretches of seconds
                    // with nothing; the slow t moves the threshold, so no two
                    // quiet stretches are the same length.
                    highp float breath = 0.5 + 0.5 * sin(p) + 0.22 * sin(p * 2.0 + 1.7);
                    highp float gate = clamp(breath * (1.05 + 0.35 * sin(t)) - 0.50, 0.0, 1.0);

                    // WHERE along the edge it comes out. Without this the gate is
                    // purely temporal and every gust spans the whole width, always
                    // in the same places.
                    highp float centre = 0.5 + 0.42 * sin(p + 1.9) * cos(p * 2.0 + 0.7);
                    highp float width  = 0.11 + 0.17 * (0.5 + 0.5 * sin(p * 3.0));
                    highp float q = (x - centre) / width;
                    gate *= exp(-q * q);

                    highp float hue = fract(0.35 * x + 0.16 * t
                                          + 0.30 * wob(x, 2.0, 6.0, 9.0, 4.2));

                    // Saturation is what decides hint versus sign. Tuned against
                    // the dimmer of the two test phones: the same numbers read
                    // noticeably fainter on one panel than the other, and a hint
                    // that only works on the brighter screen is none.
                    lowp float a = d * gate * 0.46 * qt_Opacity;
                    gl_FragColor = vec4(hsv2rgb(hue, 0.85, 1.0) * a, a);  // premultiplied
                }
            "
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
            // One button, three states, wrapping back to the first — the last
            // press is the way out of a tumbled model.
            ViewButton {
                //: Toolbar. Centre: the model turns about the middle of the
                //: screen. Point: about a spot the user taps on it. Free: any
                //: attitude, two fingers roll it.
                label: modelView.rotationMode === ModelView.RotateAboutPivot ? qsTr("Point")
                     : modelView.rotationMode === ModelView.RotateFree       ? qsTr("Free")
                                                                             : qsTr("Centre")
                active: modelView.rotationMode !== ModelView.RotateCentred
                enabled: !modelView.axisLocked
                opacity: enabled ? 1.0 : 0.4
                onActivated: { modelView.cycleRotationMode(); toolbar.reveal() }
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

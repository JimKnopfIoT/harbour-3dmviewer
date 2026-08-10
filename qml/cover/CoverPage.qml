/*
  harbour-3dmviewer — CoverPage.qml
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The cover shows what the app is about rather than what it is doing, because
  most of the time it is doing nothing: a Möbius band drawn as a translucent
  silhouette, with the rhino mark over it.

  WHY A MÖBIUS BAND
  A one-sided surface is the most compact way to say "this program is about
  surfaces, not pictures of them" — and unlike a cube or a teapot it is not
  something every other 3D app already has on its icon.

  WHY IT IS COMPUTED AND NOT AN IMAGE
  It is a parametric surface evaluated and depth-sorted in QML, so it is a few
  hundred bytes rather than a bitmap per screen density, and it scales to any
  cover size Sailfish asks for. It is drawn once per cover activation, not
  animated: a permanently animating cover costs battery for decoration.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    Canvas {
        id: band
        anchors.fill: parent
        renderStrategy: Canvas.Immediate
        renderTarget: Canvas.Image

        // Rotation of the band about the vertical axis, in radians. Fixed at an
        // angle that shows the twist clearly — head-on it just looks like a ring.
        property real yaw: 0.9
        property real tilt: 0.5

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()

            var w = width, h = height
            var cx = w * 0.5, cy = h * 0.44
            // The band is built in units where the centre-line radius is 1, so
            // one scale factor maps it onto whatever cover size we are given.
            var scale = Math.min(w, h) * 0.30

            var STEPS = 72      // segments around the loop
            var R = 1.0         // centre-line radius
            var HW = 0.38       // half-width of the strip

            var cosY = Math.cos(yaw),  sinY = Math.sin(yaw)
            var cosT = Math.cos(tilt), sinT = Math.sin(tilt)

            // Möbius: a segment of length 2*HW that rotates by half a turn
            // (t/2) while its centre travels one full turn (t).
            function point(t, s) {
                var half = t * 0.5
                var r = R + s * Math.cos(half)
                var x = r * Math.cos(t)
                var y = r * Math.sin(t)
                var z = s * Math.sin(half)

                // rotate about the world up axis, then tilt towards the viewer
                var x1 = x * cosY - y * sinY
                var y1 = x * sinY + y * cosY
                var z1 = z

                var y2 = y1 * cosT - z1 * sinT
                var z2 = y1 * sinT + z1 * cosT

                return { sx: cx + x1 * scale, sy: cy - y2 * scale, depth: z2 }
            }

            // Build the quads, then paint them back to front. Without the sort
            // the far half of the band paints over the near half and the twist
            // becomes unreadable.
            var quads = []
            for (var i = 0; i < STEPS; ++i) {
                var t0 = (i / STEPS) * 2 * Math.PI
                var t1 = ((i + 1) / STEPS) * 2 * Math.PI
                var a = point(t0, -HW), b = point(t0, HW)
                var c = point(t1, HW),  d = point(t1, -HW)
                quads.push({ pts: [a, b, c, d],
                             depth: (a.depth + b.depth + c.depth + d.depth) * 0.25 })
            }
            quads.sort(function (p, q) { return p.depth - q.depth })

            for (var k = 0; k < quads.length; ++k) {
                var q = quads[k]
                // Nearer quads are more opaque: that alone makes a flat white
                // silhouette read as a surface with a front and a back.
                var alpha = 0.16 + 0.30 * (k / quads.length)
                ctx.beginPath()
                ctx.moveTo(q.pts[0].sx, q.pts[0].sy)
                for (var j = 1; j < 4; ++j)
                    ctx.lineTo(q.pts[j].sx, q.pts[j].sy)
                ctx.closePath()
                ctx.fillStyle = Qt.rgba(1, 1, 1, alpha)
                ctx.fill()
            }
        }

        // Cover sizes are decided by the compositor, so paint on resize rather
        // than only on completion.
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    // The app mark, enlarged, over the band.
    Image {
        id: mark
        anchors {
            horizontalCenter: parent.horizontalCenter
            verticalCenter: parent.verticalCenter
            verticalCenterOffset: -parent.height * 0.06
        }
        source: "/usr/share/icons/hicolor/172x172/apps/harbour-3dmviewer.png"
        width: parent.width * 0.52
        height: width
        fillMode: Image.PreserveAspectFit
        smooth: true
        opacity: 0.95
    }

    Column {
        anchors {
            left: parent.left; right: parent.right
            bottom: parent.bottom
            margins: Theme.paddingMedium
        }

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            truncationMode: TruncationMode.Fade
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.highlightColor
            text: modelLoader.hasModel ? modelLoader.fileName : qsTr("3DM Viewer")
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            visible: modelLoader.hasModel
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
            text: qsTr("%1 layers").arg(modelLoader.layers.count)
        }
    }
}

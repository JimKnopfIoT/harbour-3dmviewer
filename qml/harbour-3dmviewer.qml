/*
  harbour-3dmviewer — application root.
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
import QtQuick 2.0
import Sailfish.Silica 1.0
// Without this, `ViewerPage { }` below is not a known type and the window comes
// up empty with only a warning in the journal.
import "pages"

ApplicationWindow {
    id: app

    initialPage: Component { ViewerPage { } }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")

    // A 3D model has no preferred orientation, and turning the phone is the
    // cheapest way to get a wider view of a long part.
    allowedOrientations: Orientation.All
}

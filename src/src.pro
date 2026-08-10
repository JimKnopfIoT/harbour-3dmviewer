# harbour-3dmviewer — the installed Silica application.
#
# NOTE ON sailfishapp: this is a subdirs project, and sailfishapp.prf's implicit
# install rules do not survive that. So we use PKGCONFIG += sailfishapp for the
# link and spell out every INSTALLS target by hand, the same way the CPC loaders
# in this tree do. Do NOT add `CONFIG += sailfishapp` here expecting the icon and
# QML installs to appear on their own — they will not.

TARGET = harbour-3dmviewer

# The rpm build passes these (%qmake5 VERSION=%{version}); the fallbacks are for
# a standalone/IDE build so the About page never shows an empty version.
isEmpty(VERSION): VERSION = 0.1.0
isEmpty(RELEASE): RELEASE = 1

# sensors: the drop simulation steers gravity from the accelerometer, so tilting
# the phone tips the world the fallen model lies in.
QT += quick qml gui sensors
CONFIG += c++17 link_pkgconfig
PKGCONFIG += sailfishapp

# Same ARM64 tuning the other apps in this tree use (Xperia 10 III).
QMAKE_CXXFLAGS_RELEASE += -O2 -march=armv8-a -mtune=cortex-a53

# --- openNURBS static library ----------------------------------------------
INCLUDEPATH += $$PWD/../lib/opennurbs
LIBS += -L$$OUT_PWD/../lib -lopennurbs

# --- Sources ----------------------------------------------------------------
INCLUDEPATH += $$PWD

HEADERS += \
    model/scene.h \
    model/layermodel.h \
    io/bgcodecodecs.h \
    io/bgcodereader.h \
    io/modelloader.h \
    io/rhinoreader.h \
    io/stepparser.h \
    io/stepreader.h \
    io/stlreader.h \
    io/threemfreader.h \
    physics/dropsim.h \
    physics/physicssettings.h \
    physics/tiltsource.h \
    render/camera.h \
    render/modelview.h

SOURCES += \
    app/main.cpp \
    model/scene.cpp \
    model/layermodel.cpp \
    io/bgcodecodecs.cpp \
    io/bgcodereader.cpp \
    io/modelloader.cpp \
    io/rhinoreader.cpp \
    io/stepparser.cpp \
    io/stepreader.cpp \
    io/stlreader.cpp \
    io/threemfreader.cpp \
    physics/dropsim.cpp \
    physics/physicssettings.cpp \
    physics/tiltsource.cpp \
    render/camera.cpp \
    render/modelview.cpp

DEFINES += APP_VERSION=\\\"$$VERSION\\\"
DEFINES += APP_RELEASE=\\\"$$RELEASE\\\"

# --- Translations -----------------------------------------------------------
# English source strings + German. libsailfishapp loads the .qm matching the
# device locale; anything else falls back to the English source text.
CONFIG += sailfishapp_i18n
TRANSLATIONS += ../translations/harbour-3dmviewer-de.ts
lupdate_only {
    SOURCES += ../qml/*.qml ../qml/pages/*.qml ../qml/cover/*.qml ../qml/components/*.qml
}

# --- Installs ---------------------------------------------------------------
target.path = /usr/bin
INSTALLS += target

qml.path  = /usr/share/$$TARGET/qml
qml.files = ../qml/*
INSTALLS += qml

desktop.path  = /usr/share/applications
desktop.files = ../harbour-3dmviewer.desktop
INSTALLS += desktop

# SAILFISHAPP_ICONS does not install anything in the mb2/rpm flow from a subdirs
# project, so the four sizes are listed explicitly. Regenerate them from
# icons/icon.svg with ./scripts/make-icons.sh.
icon86.path   = /usr/share/icons/hicolor/86x86/apps
icon86.files  = ../icons/86x86/harbour-3dmviewer.png
icon108.path  = /usr/share/icons/hicolor/108x108/apps
icon108.files = ../icons/108x108/harbour-3dmviewer.png
icon128.path  = /usr/share/icons/hicolor/128x128/apps
icon128.files = ../icons/128x128/harbour-3dmviewer.png
icon172.path  = /usr/share/icons/hicolor/172x172/apps
icon172.files = ../icons/172x172/harbour-3dmviewer.png
INSTALLS += icon86 icon108 icon128 icon172

OTHER_FILES += \
    ../qml/harbour-3dmviewer.qml \
    ../qml/cover/CoverPage.qml \
    ../qml/pages/ViewerPage.qml \
    ../qml/pages/DropPage.qml \
    ../qml/pages/TrampolinePage.qml \
    ../qml/pages/LayersPage.qml \
    ../qml/pages/ModelInfoPage.qml \
    ../qml/pages/AboutPage.qml \
    ../qml/pages/PhysicsPage.qml \
    ../qml/components/SliceRangeSlider.qml \
    ../qml/components/ViewButton.qml

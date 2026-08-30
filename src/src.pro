# harbour-3dmviewer — the installed Silica application.
#
# NOTE ON sailfishapp: this is a subdirs project, and sailfishapp.prf's implicit
# install rules do not survive that. So we use PKGCONFIG += sailfishapp for the
# link and spell out every INSTALLS target by hand, the same way the CPC loaders
# in this tree do. Do NOT add `CONFIG += sailfishapp` here expecting the icon and
# QML installs to appear on their own — they will not.

include(../common.pri)

TARGET = $$APP_NAME

# The rpm build passes these (%qmake5 VERSION=%{version}); the fallbacks are for
# a standalone/IDE build so the About page never shows an empty version.
isEmpty(VERSION): VERSION = 0.1.0
isEmpty(RELEASE): RELEASE = 1

# sensors: the drop simulation steers gravity from the accelerometer, so tilting
# the phone tips the world the fallen model lies in.
QT += quick qml gui sensors
CONFIG += c++17 link_pkgconfig
# zlib for the 3MF and bgcode readers. The system one; see io/bgcodereader.cpp.
PKGCONFIG += sailfishapp zlib

# -march=armv8-a is the baseline every Sailfish aarch64 device meets. No -mtune:
# it used to name the development phone's core, which the fleet outgrew.
QMAKE_CXXFLAGS_RELEASE += -O2 -march=armv8-a

# --- the Rhino reader is not linked here ------------------------------------
# openNURBS lives in src/rhino, dlopened on the first .3dm. It must not go back
# on this link line: in the image at process start it kills EGL startup on newer
# hybris devices (src/rhino/rhinoplugin.cpp).
DEFINES += RHINO_PLUGIN_FILE=\\\"$$RHINO_PLUGIN_FILE\\\"

# dlopen/dlsym for that plugin. glibc 2.34 folded libdl into libc, but the
# 5.0.0.62 target is older than that and still needs it named.
LIBS += -ldl

# sailfishapp.pc puts -rdynamic on the link line and that exports every symbol
# a static archive contributes, from an executable that sits at the front of the
# global lookup scope. When openNURBS was linked here it put 22725 symbols
# there, including an unprefixed 1.2.3-era zlib that shadowed the system one —
# that cost a release. No archive is linked here now, so this is a guard; where
# archives really are linked, src/rhino, the same flag does real work.
#
#   readelf --dyn-syms -W src/harbour-3dmviewer | awk '$7!="UND"' | wc -l
#   -> 19, not 22000.
QMAKE_LFLAGS += -Wl,--exclude-libs,ALL

# Objects do not depend on the Makefile, so a changed flag otherwise leaves the
# previous binary in the package. This forces a relink on a changed LINK flag;
# compile flags still need build-app.sh to drop the objects.
PRE_TARGETDEPS += $$OUT_PWD/Makefile

# --- Sources ----------------------------------------------------------------
INCLUDEPATH += $$PWD

HEADERS += \
    model/scene.h \
    io/rhinoplugin.h \
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
    # The plugin has no translator of its own — its tr() calls resolve against
    # the one libsailfishapp installs here — so its strings belong in this .ts.
    SOURCES += rhino/rhinoplugin.cpp
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

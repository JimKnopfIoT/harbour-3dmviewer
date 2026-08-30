# harbour-3dmviewer — a read-only 3D viewer for SailfishOS.
#
# Opens Rhino .3dm, STEP .stp/.step, STL, 3MF and Prusa binary G-code .bgcode,
# draws them with OpenGL ES 2, and lets you orbit/zoom/pan and switch layers on
# and off. It never writes a model file — there is no edit path anywhere in the
# code.
#
# lib    openNURBS, ~250 translation units, built once as a static library.
# rhino  the .3dm reader, linked against it into a shared object the app loads
#        only when a .3dm is opened — a separate binary because openNURBS in the
#        executable image kills EGL startup on newer hybris devices, see
#        src/rhino/rhinoplugin.cpp.
# src    the application. Links no static archive at all.
#
# CONFIG+=ordered plus the explicit .depends, which a parallel build needs.
#
# Build:  ./scripts/build-app.sh   (wraps mb2, prints the RPM path)
#         the target version lives in scripts/lib-common.sh and nowhere else.

TEMPLATE = subdirs

SUBDIRS = opennurbs rhino app

opennurbs.subdir = lib

rhino.subdir  = src/rhino
rhino.depends = opennurbs

app.subdir = src

CONFIG += ordered

OTHER_FILES += \
    README.md \
    common.pri \
    rpm/harbour-3dmviewer.spec \
    harbour-3dmviewer.desktop

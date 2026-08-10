# harbour-3dmviewer — a read-only 3D viewer for SailfishOS.
#
# Opens Rhino .3dm models (and, later, STEP .stp and Prusa binary G-code
# .bgcode), draws them with OpenGL ES 2, and lets you orbit/zoom/pan and switch
# Rhino layers on and off. It never writes a model file — there is no edit path
# anywhere in the code.
#
# TEMPLATE=subdirs because openNURBS is ~250 translation units that must be
# built once as a static library and then linked, exactly like the chips core in
# the CPC loaders. CONFIG+=ordered keeps qmake from starting src before the
# library it links against exists.
#
# Build:  ./scripts/build-app.sh   (wraps mb2, prints the RPM path)
#         the target version lives in scripts/lib-common.sh and nowhere else.

TEMPLATE = subdirs
SUBDIRS = lib src

CONFIG += ordered
src.depends = lib

OTHER_FILES += \
    README.md \
    rpm/harbour-3dmviewer.spec \
    harbour-3dmviewer.desktop

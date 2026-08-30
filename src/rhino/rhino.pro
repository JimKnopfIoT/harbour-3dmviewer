# src/rhino/rhino.pro — the Rhino .3dm reader as a loadable shared object.
# A separate binary because openNURBS in the application's image kills EGL
# startup on newer hybris devices; see rhinoplugin.cpp.

include(../../common.pri)

TEMPLATE = lib
TARGET   = $$RHINO_PLUGIN_BASENAME

# CONFIG+=plugin for the naming only: one unversioned libfoo.so instead of
# .so.1.0.0 plus symlinks. No plugin metadata, no QPluginLoader.
CONFIG += plugin c++17 hide_symbols

# QtGui for the QColor and QVector3D that Scene is made of. It never draws.
QT += core gui

QMAKE_CXXFLAGS_RELEASE += -O2 -march=armv8-a

INCLUDEPATH += $$PWD/../..           # so "io/..." and "model/..." resolve
INCLUDEPATH += $$PWD/..
INCLUDEPATH += $$PWD/../../lib/opennurbs

# The static library built by lib/lib.pro, one directory up from this one's
# build directory in both an in-source and a shadow build.
LIBS += -L$$OUT_PWD/../../lib -lopennurbs

# openNURBS contributes ~22700 symbols. Without this they land in this object's
# dynamic symbol table, which is hashed and relocated on every dlopen and which
# nothing ever reads — the app resolves one name and opens it RTLD_LOCAL.
QMAKE_LFLAGS += -Wl,--exclude-libs,ALL

HEADERS += ../io/rhinoplugin.h \
           ../model/scene.h

# scene.cpp is compiled into BOTH binaries on purpose. The plugin calls
# Scene::growBounds() and isEmpty(); exporting them from the executable instead
# is the arrangement that cost the 0.1.2 release. Duplication is safe here only
# because Scene has no virtual functions and no mutable static state — if it
# ever grows either, this line stops being correct.
SOURCES += rhinoplugin.cpp \
           ../model/scene.cpp

# Private to this application.
target.path = $$RHINO_PLUGIN_DIR
INSTALLS += target

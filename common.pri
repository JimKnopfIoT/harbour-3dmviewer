# common.pri — the names and paths the application and the Rhino plugin must
# agree on. If they drift apart everything still builds and packages, and the
# app reports "Rhino support is not installed" on the first .3dm — too quiet a
# failure to risk on two independently maintained strings.

APP_NAME = harbour-3dmviewer

# A plain dlopen'd shared object, not a Qt plugin: CONFIG+=plugin is only for
# the unversioned file name. It lives in a private directory under the library
# path because that is where rpmbuild's strip pass looks; under %{_datadir} it
# is packaged unstripped, 67 MB of it.
#
# LIBDIR comes from %qmake5; the fallback is for a plain qmake run.
isEmpty(LIBDIR): LIBDIR = /usr/lib64

RHINO_PLUGIN_BASENAME = $${APP_NAME}-rhino
RHINO_PLUGIN_DIR      = $${LIBDIR}/$${APP_NAME}
RHINO_PLUGIN_FILE     = $${RHINO_PLUGIN_DIR}/lib$${RHINO_PLUGIN_BASENAME}.so

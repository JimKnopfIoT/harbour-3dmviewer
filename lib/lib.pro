# openNURBS 8.x — McNeel's 3dm reader, built as a static C++ library.
#
# WHY A SEPARATE PROJECT FILE, NOT ONE INSIDE opennurbs/
#   opennurbs/ is McNeel's source, checked out from their repository and not
#   redistributed by us — in the public repository it is a git submodule. So it
#   must stay untouched, and this project file, which is ours, sits beside it
#   rather than inside it. That also means a fresh clone needs
#       git submodule update --init
#   before there is anything here to build.
#
# WHY NOT A SYSTEM PACKAGE
#   Nothing in the Sailfish repos reads .3dm. openNURBS is the only complete
#   implementation of the format and it is self-contained: it brings its own
#   zlib and its own uuid generator, so the reader adds exactly zero runtime
#   dependencies to the RPM.
#
# THIS MIRRORS upstream CMakeLists.txt. If you move to a newer openNURBS,
# re-check the exclusion list below against its OPENNURBS_PUBLIC_SOURCES.

TEMPLATE = lib
CONFIG += staticlib
CONFIG -= qt
TARGET = opennurbs

CONFIG += c++17
QMAKE_CXXFLAGS += -std=c++17

# ON_COMPILING_OPENNURBS is what upstream's CMake sets on this target; it
# switches the headers from "consumer" to "implementation" mode.
DEFINES += ON_COMPILING_OPENNURBS

# openNURBS is third-party code that compiles with a lot of noise (missing
# overrides, fallthrough switches, deprecated declarations). Upstream silences
# the same set. Keeping it quiet means OUR warnings stay visible in the log.
QMAKE_CXXFLAGS += -w
QMAKE_CFLAGS   += -w

# Cortex-A53/A73 (Xperia 10 III) — same flags the other projects in this tree
# use. -O2 rather than -O3: opennurbs is huge and -O3 on 250 translation units
# costs several minutes for no measurable gain in a read-only workload.
QMAKE_CXXFLAGS_RELEASE += -O2
QMAKE_CFLAGS_RELEASE   += -O2

ON_DIR = $$PWD/opennurbs
INCLUDEPATH += $$ON_DIR $$ON_DIR/zlib $$ON_DIR/android_uuid

# --- Sources ---------------------------------------------------------------
# Everything in the top level EXCEPT the five files upstream's CMakeLists.txt
# deliberately leaves out. An exclusion list is used rather than a 247-entry
# inclusion list so that re-vendoring does not silently drop new files.
#
#   opennurbs_gl.cpp              legacy OpenGL demo helper (fixed-function GL,
#                                 would not even compile against GLES2)
#   opennurbs_precompiledheader   PCH stub, meaningless outside MSVC
#   opennurbs_unicode_cp932/949   Shift-JIS / EUC-KR codepage tables, not built
#   opennurbs_userdata_obsolete   pre-V5 userdata, superseded upstream
ON_EXCLUDE = \
    opennurbs_gl.cpp \
    opennurbs_precompiledheader.cpp \
    opennurbs_unicode_cp932.cpp \
    opennurbs_unicode_cp949.cpp \
    opennurbs_userdata_obsolete.cpp

for (f, $$list($$files($$ON_DIR/*.cpp))) {
    base = $$basename(f)
    !contains(ON_EXCLUDE, $$base): SOURCES += $$f
}

# zlib: the subset openNURBS ships, used for compressed mesh and bitmap chunks.
# Its zconf.h self-defines Z_PREFIX and MY_ZCALLOC. zcalloc/zcfree come from
# opennurbs_zlib_memory.cpp above, which is why MY_ZCALLOC must stay set.
#
# DO NOT believe Z_PREFIX makes this collision-proof. That list is 1.2.3-era: it
# renames the ~30 names zlib had then and nothing since. inflateGetHeader,
# inflatePrime, deflateSetHeader, deflateTune, crc32_combine, adler32_combine
# and every internal (inflate_fast, inflate_table, _tr_*, _dist_code, z_errmsg)
# keep their real names and WILL shadow the system libz if they reach a dynamic
# symbol table. Same for android_uuid/ vs libuuid.so.1. src/src.pro links with
# -Wl,--exclude-libs,ALL to keep them out of one — see the comment there.
SOURCES += $$files($$ON_DIR/zlib/*.c)

# android_uuid: a trimmed util-linux libuuid. Upstream builds it on Linux too,
# and it keeps us off the system libuuid. Three files are excluded upstream:
# clear.c and compare.c are unused, gen_uuid_nt.c is the Windows generator.
UUID_EXCLUDE = clear.c compare.c gen_uuid_nt.c
for (f, $$list($$files($$ON_DIR/android_uuid/*.c))) {
    base = $$basename(f)
    !contains(UUID_EXCLUDE, $$base): SOURCES += $$f
}

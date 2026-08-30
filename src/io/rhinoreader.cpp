/*
  harbour-3dmviewer — rhinoreader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The application half of the Rhino reader: everything except the reading. That
  lives in a shared object loaded on the first .3dm, for the reason set out in
  src/rhino/rhinoplugin.cpp. The contract in rhinoreader.h is unchanged, so
  nothing outside these two files knows about the split.
*/
#include "io/rhinoreader.h"

#include "io/rhinoplugin.h"

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <dlfcn.h>

#ifndef RHINO_PLUGIN_FILE
#  error "RHINO_PLUGIN_FILE must be defined by the build (see common.pri)"
#endif

namespace {

/*
  Loaded once and never dlclose()d: openNURBS builds global state in its static
  initialisers, and unloading it while a Scene read from it is still on screen
  would run their destructors under it.

  RTLD_NOW so a bad symbol surfaces here and not halfway through a model.
  RTLD_LOCAL so openNURBS' symbols stay inside this object.
*/
struct RhinoPlugin
{
    RhinoPluginReadFn read = nullptr;
    QString           error;      // translated, user-facing; empty while unloaded
    bool              tried = false;
};

QMutex g_pluginMutex;
RhinoPlugin g_plugin;

// Returns the entry point, or null with a reason in *errorOut. The mutex is
// there because "load once" and "read a pointer" are two operations.
RhinoPluginReadFn resolveEntry(QString *errorOut)
{
    QMutexLocker lock(&g_pluginMutex);

    if (!g_plugin.tried) {
        g_plugin.tried = true;

        void *handle = dlopen(RHINO_PLUGIN_FILE, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            // dlerror() is developer text, but this app has no crash reporter:
            // the detail is what makes a user's report usable.
            g_plugin.error = QObject::tr("Rhino (.3dm) support is not installed: %1")
                                 .arg(QString::fromLocal8Bit(dlerror()));
        } else {
            void *sym = dlsym(handle, RHINO_PLUGIN_ENTRY_SYMBOL);
            if (!sym) {
                g_plugin.error = QObject::tr("Rhino (.3dm) support is installed but unusable: %1")
                                     .arg(QString::fromLocal8Bit(dlerror()));
            } else {
                g_plugin.read = reinterpret_cast<RhinoPluginReadFn>(sym);
            }
        }
    }

    if (!g_plugin.read && errorOut)
        *errorOut = g_plugin.error;

    return g_plugin.read;
}

} // namespace

ScenePtr RhinoReader::read(const QString &path,
                           QString *errorOut,
                           void (*progress)(void *, float),
                           void *progressCtx)
{
    if (errorOut)
        errorOut->clear();

    // Checked here too, so a missing file costs a message and not a dlopen.
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isReadable()) {
        if (errorOut)
            *errorOut = QObject::tr("File is missing or not readable.");
        return ScenePtr();
    }

    const RhinoPluginReadFn entry = resolveEntry(errorOut);
    if (!entry)
        return ScenePtr();

    // Owned on this side; the plugin only fills it.
    ScenePtr scene(new Scene);
    if (!entry(path, scene.data(), errorOut, progress, progressCtx))
        return ScenePtr();

    return scene;
}

/*
  harbour-3dmviewer — rhinoplugin.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The one symbol libharbour-3dmviewer-rhino.so exports, declared for both sides
  of the dlopen: io/rhinoreader.cpp looks it up, rhino/rhinoplugin.cpp defines it.

  Qt types cross this boundary. That is safe only because both sides are built
  from the same tree by the same compiler against the same Qt and ship in the
  same RPM — it is a loading boundary, not a compatibility one, and nothing else
  may load this object. extern "C" is for the unmangled name, not for the types.
*/
#ifndef RHINOPLUGIN_H
#define RHINOPLUGIN_H

#include "model/scene.h"

#include <QString>

// Kept next to the declaration: a typo here is a runtime failure, not a
// compile error.
#define RHINO_PLUGIN_ENTRY_SYMBOL "harbour3dmviewerReadRhino"

extern "C" {

/*
  Reads `path` into the caller-owned `scene`. False on hard failure, with a
  translated reason in *errorOut; a file that reads but holds nothing drawable
  returns true with Scene::warnings filled in. `progress` may be null and runs
  on the calling thread (the loader thread).
*/
Q_DECL_EXPORT bool harbour3dmviewerReadRhino(const QString &path,
                                             Scene *scene,
                                             QString *errorOut,
                                             void (*progress)(void *, float),
                                             void *progressCtx);

} // extern "C"

// What dlsym() returns, cast to something callable.
typedef bool (*RhinoPluginReadFn)(const QString &, Scene *, QString *,
                                  void (*)(void *, float), void *);

#endif // RHINOPLUGIN_H

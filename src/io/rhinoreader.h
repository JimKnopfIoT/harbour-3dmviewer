/*
  harbour-3dmviewer — rhinoreader.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Reads a Rhino .3dm file into a Scene using McNeel's openNURBS toolkit.

  THE ONE THING TO UNDERSTAND ABOUT .3dm AND THIS APP
  ---------------------------------------------------
  A Rhino file stores real CAD geometry: trimmed NURBS breps, extrusions,
  SubD. The *public* openNURBS toolkit can read all of that but contains no
  mesher — the tessellator lives in the closed part of Rhino. So we cannot turn
  a NURBS surface into triangles ourselves.

  What saves us is that Rhino caches a RENDER MESH inside each brep/extrusion
  when the document is saved normally. That cache is what the viewport draws,
  and it is what we draw. All 222 objects in the reference model carry one.

  A file saved with "Save Small" (or written by a third-party exporter) has no
  cached meshes. Those objects genuinely cannot be displayed by this app, so the
  reader counts them and reports them as a warning rather than showing an empty
  screen with no explanation.
*/
#ifndef RHINOREADER_H
#define RHINOREADER_H

#include "model/scene.h"

#include <QString>

namespace RhinoReader {

// Reads `path` into a fresh Scene. Returns a null ScenePtr on hard failure and
// puts the reason in *errorOut. A successfully read file that contains nothing
// drawable still returns a valid Scene — with warnings explaining why it is
// empty, which is far more useful than an error.
//
// `progress` is called with 0.0..1.0 and may be null. It is invoked from the
// calling thread, which is the loader thread, never the GUI thread.
ScenePtr read(const QString &path,
              QString *errorOut,
              void (*progress)(void *ctx, float f) = nullptr,
              void *progressCtx = nullptr);

} // namespace RhinoReader

#endif // RHINOREADER_H

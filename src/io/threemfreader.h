/*
  harbour-3dmviewer — threemfreader.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Reads 3MF (3D Manufacturing Format) into a Scene. This is what PrusaSlicer
  saves a project as, and what most slicers now prefer over STL.

  WHAT A 3MF ACTUALLY IS
  ----------------------
  A ZIP archive. Inside it, "3D/3dmodel.model" is an XML document listing
  objects, each with a plain indexed mesh, and a build section placing those
  objects with a 4x3 transform. Unlike STL it carries units, names, colours and
  instancing — and unlike STEP it is already triangulated, so there is nothing
  to tessellate.

  WHAT THIS READER DOES NOT DO
  ----------------------------
  Beam lattices, slice stacks, textures and the production extension are all
  skipped. Each is a whole feature of its own and none appears in a model that
  came out of a slicer's "save project". Anything skipped is counted and
  reported rather than passed over in silence.

  ZIP, WITHOUT A ZIP LIBRARY
  --------------------------
  Only two things are needed from the archive: find one entry by name, and
  inflate it. Both are a few dozen lines against the system zlib, which every
  Sailfish device has. Pulling in a ZIP library for that would add a
  dependency to save nothing.
*/
#ifndef THREEMFREADER_H
#define THREEMFREADER_H

#include <QString>

#include "model/scene.h"

class ThreeMfReader
{
public:
    static ScenePtr read(const QString &path,
                         QString *errorOut,
                         void (*progress)(void *, float) = nullptr,
                         void *progressCtx = nullptr);
};

#endif // THREEMFREADER_H

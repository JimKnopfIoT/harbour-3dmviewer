/*
  harbour-3dmviewer — bgcodereader.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Reads a PrusaSlicer binary G-code file (.bgcode) into a Scene.

  WHAT COMES OUT IS NOT A SURFACE
  -------------------------------
  Every other format this app reads describes a shape. A sliced file describes
  a JOURNEY: where the nozzle went and where it was pushing plastic while it
  went there. There is no surface to recover — only the path — so the Scene
  gets line batches rather than triangles, one batch per feature type, in the
  colours PrusaSlicer uses so the picture is recognisable to someone who has
  just come from the slicer.

  The per-layer visibility that exists for Rhino layers therefore doubles as a
  feature filter here: turning "Internal infill" off is the same mechanism as
  turning a CAD layer off, and costs the same single skipped draw call.

  THE FILE IS THREE FORMATS DEEP
  ------------------------------
      container  typed blocks with sizes and CRC32
      layer 1    heatshrink (12/4 here) per G-code block
      layer 2    MeatPack, two characters to a byte
  Only after all three is there any G-code text. See bgcodecodecs.h.
*/
#ifndef BGCODEREADER_H
#define BGCODEREADER_H

#include <QString>

#include "model/scene.h"

class BgCodeReader
{
public:
    // Same signature as the other readers: the loader treats them alike and
    // the progress callback keeps the reader free of any Qt signal machinery.
    static ScenePtr read(const QString &path,
                         QString *errorOut,
                         void (*progress)(void *, float) = nullptr,
                         void *progressCtx = nullptr);
};

#endif // BGCODEREADER_H

/*
  harbour-3dmviewer — stepreader.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Reads STEP (.stp / .step) into a Scene.

  WHY THIS IS THE HARD FORMAT
  ---------------------------
  Every other format this app opens hands over triangles, or (in Rhino's case)
  a render mesh the CAD system already computed. STEP hands over MATHEMATICS: a
  face is a surface plus the loops of curves that cut a piece out of it, and
  nothing in the file says where a triangle goes. So this reader has to build
  the triangles itself, and that is what most of the code below is.

  THE APPROACH: TRIANGULATE IN PARAMETER SPACE
  --------------------------------------------
  A curved surface is hard to triangulate in 3D and easy in its own (u,v)
  coordinates, where a cylinder is a flat rectangle. So for every face:

      1. sample each boundary curve into 3D points
      2. map those points into the surface's (u,v) using its exact inverse
      3. triangulate the resulting flat polygon-with-holes by ear clipping
      4. map the triangle corners back to 3D and take exact normals there

  Only step 3 is generic, which is the point: one triangulator serves every
  surface type, and adding a cone or a sphere later means writing an eval and
  an inverse, not another mesher.

  WHAT IS SUPPORTED
  -----------------
  Surfaces: plane and cylinder. Curves: line, circle, B-spline (any degree,
  rational or not) and trimmed curves over those. That covers this file
  completely — 89 planes and 10 cylinders, with B-splines only ever used as
  edges — and covers the great majority of mechanical parts, whose faces are
  overwhelmingly flat or round. A face on a surface type that is not handled
  is counted and reported, never silently dropped.

  WHY NOT OpenCascade
  -------------------
  It is the obvious answer and it is not available: no Sailfish build, and its
  size is out of proportion to a phone viewer. openNURBS, which this app
  already links, has no mesher at all — so either way the tessellator had to
  be written.
*/
#ifndef STEPREADER_H
#define STEPREADER_H

#include <QString>

#include "model/scene.h"

class StepReader
{
public:
    static ScenePtr read(const QString &path,
                         QString *errorOut,
                         void (*progress)(void *, float) = nullptr,
                         void *progressCtx = nullptr);
};

#endif // STEPREADER_H

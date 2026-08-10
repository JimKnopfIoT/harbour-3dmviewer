/*
  harbour-3dmviewer — scene.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The renderer-facing model. Every file format this app supports (.3dm now,
  .stp and .bgcode later) is parsed into exactly this structure, so the OpenGL
  side never learns what a Rhino brep or a STEP face is.

  WHY GEOMETRY IS GROUPED BY LAYER
  --------------------------------
  Rhino organises everything by layer, and the user wants to toggle layers. If
  each of the file's 200-odd objects were its own vertex buffer, a toggle would
  mean bookkeeping across hundreds of draw calls. Merging every object of one
  layer into one interleaved buffer turns "hide this layer" into "skip one draw
  call" and cuts the per-frame call count to the number of layers (18 in the
  reference model). Per-object colour survives the merge because colour is
  carried per vertex, not per batch.

  VERTEX LAYOUT: 9 interleaved floats
      px py pz   nx ny nz   r g b
  Positions are already in world space (any instance transform is baked in at
  read time), so the renderer only ever applies the camera matrices.
*/
#ifndef SCENE_H
#define SCENE_H

#include <limits>

#include <QColor>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector3D>

// Floats per vertex in SceneLayer::vertices. Renderer and readers must agree;
// everything derives from this constant rather than repeating "9".
static const int kFloatsPerVertex = 9;

/*
  What the indices in a batch describe. A CAD model is surfaces; a sliced
  G-code file is a path the nozzle travelled, which has no surface at all and
  would be nonsense to shade. Both still fit the same interleaved buffer, so
  the only thing the renderer needs to know is which GL primitive to hand the
  indices to — and that lighting means nothing for a line.
*/
enum ScenePrimitive {
    PrimitiveTriangles,
    PrimitiveLines
};

struct SceneLayer
{
    QString name;
    QColor  color;        // the layer's own colour, for the layer list UI
    bool    visible = true;  // initial state as stored in the file
    int     sourceIndex = 0; // the layer index used by the file format

    ScenePrimitive primitive = PrimitiveTriangles;

    QVector<float>   vertices;  // interleaved, kFloatsPerVertex per vertex
    QVector<quint32> indices;   // triangle list, or line list

    /*
      Where each print slice starts inside `indices`.

      Only sliced formats fill this. Entry k is the first index belonging to
      print layer k and the array has Scene::sliceCount + 1 entries, the last
      being indices.size(). Because a G-code file is written bottom layer
      first and we append in that order, this array is sorted — so "show only
      layers 12 to 80" is a single draw call with an offset and a count, with
      no geometry rebuilt and nothing uploaded again.
    */
    QVector<int> sliceStart;

    int triangleCount() const
    {
        return primitive == PrimitiveTriangles ? indices.size() / 3 : 0;
    }
    int lineCount() const
    {
        return primitive == PrimitiveLines ? indices.size() / 2 : 0;
    }
    bool isEmpty() const { return indices.isEmpty(); }
};

/*
  One loaded model. Immutable once the reader hands it over: the loader thread
  fills it, then ownership passes to the GUI thread as a QSharedPointer and
  nothing mutates it again except the per-layer `visible` flag (GUI thread only).
*/
class Scene
{
public:
    QVector<SceneLayer> layers;

    /*
      Axis-aligned bounds over all geometry, used to frame the camera.

      Deliberately started INVERTED, which is the "nothing added yet" marker
      growBounds() tests for. Leaving them default-constructed would start them
      both at the origin, and since no real box can then be smaller, every
      model would silently have the origin inside its bounds. A CAD model
      usually straddles the origin anyway so the mistake hides — but a printed
      part sits in the middle of the bed, and its box would stretch all the way
      back to the bed corner, doubling the apparent size and aiming the camera
      at empty air.
    */
    QVector3D bbMin = QVector3D( std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max());
    QVector3D bbMax = QVector3D(-std::numeric_limits<float>::max(),
                                -std::numeric_limits<float>::max(),
                                -std::numeric_limits<float>::max());

    QString filePath;
    QString formatName;       // "Rhino 3DM", "STEP", "Binary G-code"
    QString producer;         // application that wrote the file, if recorded
    QStringList warnings;     // shown to the user; never silently swallowed

    int objectCount = 0;

    /*
      Print slices, for formats that have them (.bgcode). Zero everywhere else,
      which is what the UI tests to decide whether the layer-range slider
      exists at all — a CAD model has no such thing and must not grow one.

      sliceZ holds the nozzle height of each slice, so the slider can label
      itself in millimetres rather than in slice numbers nobody can picture.
    */
    int sliceCount = 0;
    QVector<float> sliceZ;

    bool isEmpty() const;
    int triangleCount() const;
    // Drawn toolpath segments. Zero for a surface model, which is what tells
    // the UI to count triangles instead.
    int lineCount() const;
    int vertexCount() const;

    // Centre and bounding-sphere radius of the whole model. The radius is
    // clamped away from zero so a degenerate or single-point model cannot
    // produce a division by zero in the camera framing maths.
    QVector3D center() const;
    float radius() const;

    // Bounds over only the currently visible layers, so "fit to view" frames
    // what the user can actually see rather than the whole file. Falls back to
    // the full bounds when every layer is hidden.
    void visibleBounds(QVector3D *outMin, QVector3D *outMax) const;

    void growBounds(const QVector3D &p);
};

typedef QSharedPointer<Scene> ScenePtr;

#endif // SCENE_H

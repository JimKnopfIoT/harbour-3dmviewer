/*
  harbour-3dmviewer — scene.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "scene.h"

#include <limits>

// An "empty" bounding box is marked by min > max, which no real box can be.
// Readers start from this and call growBounds() per vertex.
static const float kBig = std::numeric_limits<float>::max();

bool Scene::isEmpty() const
{
    for (int i = 0; i < layers.size(); ++i)
        if (!layers.at(i).isEmpty())
            return false;
    return true;
}

int Scene::triangleCount() const
{
    int n = 0;
    for (int i = 0; i < layers.size(); ++i)
        n += layers.at(i).triangleCount();
    return n;
}

int Scene::lineCount() const
{
    int n = 0;
    for (int i = 0; i < layers.size(); ++i)
        n += layers.at(i).lineCount();
    return n;
}

int Scene::vertexCount() const
{
    int n = 0;
    for (int i = 0; i < layers.size(); ++i)
        n += layers.at(i).vertices.size() / kFloatsPerVertex;
    return n;
}

void Scene::growBounds(const QVector3D &p)
{
    if (bbMin.x() > bbMax.x()) {   // still the "unset" marker
        bbMin = p;
        bbMax = p;
        return;
    }
    bbMin.setX(qMin(bbMin.x(), p.x()));
    bbMin.setY(qMin(bbMin.y(), p.y()));
    bbMin.setZ(qMin(bbMin.z(), p.z()));
    bbMax.setX(qMax(bbMax.x(), p.x()));
    bbMax.setY(qMax(bbMax.y(), p.y()));
    bbMax.setZ(qMax(bbMax.z(), p.z()));
}

QVector3D Scene::center() const
{
    if (bbMin.x() > bbMax.x())
        return QVector3D();
    return (bbMin + bbMax) * 0.5f;
}

float Scene::radius() const
{
    if (bbMin.x() > bbMax.x())
        return 1.0f;
    const float r = (bbMax - bbMin).length() * 0.5f;
    // A flat or single-point model would otherwise hand the camera a zero
    // distance and every subsequent projection would collapse to NaN.
    return r > 1e-6f ? r : 1.0f;
}

void Scene::visibleBounds(QVector3D *outMin, QVector3D *outMax) const
{
    QVector3D lo(kBig, kBig, kBig);
    QVector3D hi(-kBig, -kBig, -kBig);
    bool any = false;

    for (int li = 0; li < layers.size(); ++li) {
        const SceneLayer &L = layers.at(li);
        if (!L.visible || L.isEmpty())
            continue;
        for (int v = 0; v < L.vertices.size(); v += kFloatsPerVertex) {
            const float x = L.vertices.at(v);
            const float y = L.vertices.at(v + 1);
            const float z = L.vertices.at(v + 2);
            lo.setX(qMin(lo.x(), x)); hi.setX(qMax(hi.x(), x));
            lo.setY(qMin(lo.y(), y)); hi.setY(qMax(hi.y(), y));
            lo.setZ(qMin(lo.z(), z)); hi.setZ(qMax(hi.z(), z));
            any = true;
        }
    }

    // Everything hidden: framing the empty set is meaningless, so fall back to
    // the whole model. The caller then still gets a usable camera.
    if (!any) {
        *outMin = bbMin;
        *outMax = bbMax;
        return;
    }
    *outMin = lo;
    *outMax = hi;
}

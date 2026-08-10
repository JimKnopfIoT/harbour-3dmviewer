/*
  harbour-3dmviewer — rhinoreader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/rhinoreader.h"

#include <QFileInfo>
#include <QHash>
#include <QtGlobal>

#include "opennurbs.h"

namespace {

// openNURBS speaks wchar_t. Qt speaks QString. ON_wString is not necessarily
// NUL-terminated storage we can hand to QString::fromWCharArray blindly, so go
// through its accessors.
QString toQString(const ON_wString &s)
{
    const wchar_t *w = static_cast<const wchar_t *>(s);
    if (!w)
        return QString();
    return QString::fromWCharArray(w, s.Length());
}

QColor toQColor(const ON_Color &c)
{
    // ON_Color packs its channels internally in a Windows COLORREF order; the
    // accessors are the documented way to get them out and the only thing that
    // stays correct if that packing ever changes.
    return QColor(c.Red(), c.Green(), c.Blue());
}

/*
  Appends one ON_Mesh to a layer batch.

  NORMALS
  Rhino's render meshes normally carry vertex normals, but a mesh assembled by
  an exporter may not. ON_Mesh::ComputeVertexNormals() would fix that, except
  the meshes we get out of a brep are const (they belong to the model we must
  not modify). So when normals are missing we accumulate area-weighted face
  normals ourselves. Area weighting comes for free: the un-normalised cross
  product is already proportional to the triangle's area, which is exactly the
  weighting that gives smooth results on irregular tessellations.

  FACES
  ON_MeshFace is a quad; a triangle is stored as a quad whose last two indices
  are equal (IsTriangle()). We emit one or two triangles accordingly.
*/
void appendMesh(SceneLayer *layer, const ON_Mesh *mesh, const QColor &color, Scene *scene)
{
    const int nv = mesh->VertexCount();
    const int nf = mesh->FaceCount();
    if (nv <= 0 || nf <= 0)
        return;

    // Index of our first vertex, so the mesh's own 0-based face indices can be
    // rebased into the shared per-layer buffer.
    const quint32 base = static_cast<quint32>(layer->vertices.size() / kFloatsPerVertex);

    const bool haveNormals = (mesh->m_N.Count() == nv);

    QVector<QVector3D> computed;
    if (!haveNormals)
        computed.resize(nv);   // QVector zero-initialises, which is the correct start

    // --- vertices ---------------------------------------------------------
    layer->vertices.reserve(layer->vertices.size() + nv * kFloatsPerVertex);
    QVector<QVector3D> pos(nv);
    for (int i = 0; i < nv; ++i) {
        // Vertex() handles both the float array and the optional double-precision
        // one, so a model authored far from the origin keeps its accuracy.
        const ON_3dPoint p = mesh->Vertex(i);
        pos[i] = QVector3D(float(p.x), float(p.y), float(p.z));
        scene->growBounds(pos[i]);
    }

    // --- faces ------------------------------------------------------------
    layer->indices.reserve(layer->indices.size() + nf * 6);
    for (int f = 0; f < nf; ++f) {
        const ON_MeshFace &face = mesh->m_F[f];
        const int a = face.vi[0], b = face.vi[1], c = face.vi[2], d = face.vi[3];
        if (a < 0 || b < 0 || c < 0 || d < 0 || a >= nv || b >= nv || c >= nv || d >= nv)
            continue;   // corrupt face; skipping beats indexing out of bounds

        layer->indices << base + quint32(a) << base + quint32(b) << base + quint32(c);
        if (!face.IsTriangle())
            layer->indices << base + quint32(a) << base + quint32(c) << base + quint32(d);

        if (!haveNormals) {
            const QVector3D n1 = QVector3D::crossProduct(pos[b] - pos[a], pos[c] - pos[a]);
            computed[a] += n1; computed[b] += n1; computed[c] += n1;
            if (!face.IsTriangle()) {
                const QVector3D n2 = QVector3D::crossProduct(pos[c] - pos[a], pos[d] - pos[a]);
                computed[a] += n2; computed[c] += n2; computed[d] += n2;
            }
        }
    }

    // --- interleave -------------------------------------------------------
    const float r = float(color.redF());
    const float g = float(color.greenF());
    const float bl = float(color.blueF());

    for (int i = 0; i < nv; ++i) {
        QVector3D n;
        if (haveNormals) {
            const ON_3fVector &on = mesh->m_N[i];
            n = QVector3D(on.x, on.y, on.z);
        } else {
            n = computed[i];
        }
        // A vertex belonging only to degenerate faces gets a zero normal;
        // normalizing that yields NaN and paints a black hole in the model.
        if (n.lengthSquared() < 1e-20f)
            n = QVector3D(0.0f, 0.0f, 1.0f);
        else
            n.normalize();

        layer->vertices << pos[i].x() << pos[i].y() << pos[i].z()
                        << n.x() << n.y() << n.z()
                        << r << g << bl;
    }
}

} // namespace

ScenePtr RhinoReader::read(const QString &path,
                           QString *errorOut,
                           void (*progress)(void *, float),
                           void *progressCtx)
{
    if (errorOut)
        errorOut->clear();

    QFileInfo fi(path);
    if (!fi.exists() || !fi.isReadable()) {
        if (errorOut)
            *errorOut = QObject::tr("File is missing or not readable.");
        return ScenePtr();
    }

    // ON::Begin() initialises openNURBS' class registry. It is idempotent and
    // cheap, and calling it per read keeps the reader free of global setup that
    // main() would otherwise have to remember to do.
    ON::Begin();

    ONX_Model model;
    if (!model.Read(path.toUtf8().constData())) {
        if (errorOut)
            *errorOut = QObject::tr("Not a readable Rhino 3DM file.");
        return ScenePtr();
    }
    if (progress)
        progress(progressCtx, 0.35f);

    ScenePtr scene(new Scene);
    scene->filePath = path;
    scene->formatName = QStringLiteral("Rhino 3DM");
    scene->producer = toQString(model.m_properties.m_Application.m_application_name);

    // --- layer table ------------------------------------------------------
    // Rhino layer indices are not guaranteed to be contiguous or to start at
    // zero, so map source index -> our batch position instead of assuming.
    QHash<int, int> layerSlot;
    {
        ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::Layer);
        for (const ON_ModelComponent *c = it.FirstComponent(); c; c = it.NextComponent()) {
            const ON_Layer *L = ON_Layer::Cast(c);
            if (!L)
                continue;
            SceneLayer sl;
            sl.name = toQString(L->Name());
            if (sl.name.isEmpty())
                sl.name = QObject::tr("Layer %1").arg(L->Index());
            sl.color = toQColor(L->Color());
            // Honour what the file says. The reference model ships 18 layers of
            // which only 4 are visible; showing all 18 would bury the model in
            // its own construction geometry.
            sl.visible = L->IsVisible();
            sl.sourceIndex = L->Index();
            layerSlot.insert(L->Index(), scene->layers.size());
            scene->layers.append(sl);
        }
    }

    // Geometry can reference a layer index that has no entry in the table (a
    // damaged file, or a default-layer object in a file with no layer table).
    // Rather than dropping it, everything unmatched lands in one fallback layer
    // created on demand.
    int fallbackSlot = -1;

    // --- geometry ---------------------------------------------------------
    int noMeshCount = 0;
    int instanceRefCount = 0;

    ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::ModelGeometry);
    for (const ON_ModelComponent *c = it.FirstComponent(); c; c = it.NextComponent()) {
        const ON_ModelGeometryComponent *mg = ON_ModelGeometryComponent::Cast(c);
        if (!mg)
            continue;
        const ON_Geometry *geo = mg->Geometry(nullptr);
        const ON_3dmObjectAttributes *att = mg->Attributes(nullptr);
        if (!geo || !att)
            continue;
        if (att->IsInstanceDefinitionObject())
            continue;   // drawn through its instance references, not directly
        if (!att->IsVisible())
            continue;   // the object itself is hidden in Rhino

        scene->objectCount++;

        // Where does it go?
        int slot;
        if (layerSlot.contains(att->m_layer_index)) {
            slot = layerSlot.value(att->m_layer_index);
        } else {
            if (fallbackSlot < 0) {
                SceneLayer sl;
                sl.name = QObject::tr("(no layer)");
                sl.color = QColor(200, 200, 200);
                sl.visible = true;
                sl.sourceIndex = -1;
                fallbackSlot = scene->layers.size();
                scene->layers.append(sl);
            }
            slot = fallbackSlot;
        }
        SceneLayer &batch = scene->layers[slot];

        // Object colour overrides layer colour only when Rhino says so.
        const QColor color = (att->ColorSource() == ON::color_from_object)
                                 ? toQColor(att->m_color)
                                 : batch.color;

        // --- the four things we know how to draw --------------------------
        if (const ON_Mesh *m = ON_Mesh::Cast(geo)) {
            appendMesh(&batch, m, color, scene.data());
        } else if (const ON_Brep *b = ON_Brep::Cast(geo)) {
            ON_SimpleArray<const ON_Mesh *> meshes;
            if (b->GetMesh(ON::render_mesh, meshes) > 0) {
                for (int i = 0; i < meshes.Count(); ++i)
                    if (meshes[i])
                        appendMesh(&batch, meshes[i], color, scene.data());
            } else {
                noMeshCount++;
            }
        } else if (const ON_Extrusion *e = ON_Extrusion::Cast(geo)) {
            if (const ON_Mesh *m = e->Mesh(ON::render_mesh))
                appendMesh(&batch, m, color, scene.data());
            else
                noMeshCount++;
        } else if (ON_InstanceRef::Cast(geo)) {
            // Blocks. Drawing them means resolving the instance definition and
            // baking its transform; not implemented yet, so say so rather than
            // letting the user wonder where part of the model went.
            instanceRefCount++;
        }
        // Curves, points, annotation and lights are silently ignored: this is a
        // shaded solid viewer, and drawing a dimension leader as a triangle
        // soup would be worse than not drawing it.
    }

    if (progress)
        progress(progressCtx, 0.9f);

    // --- report what could not be drawn -----------------------------------
    if (noMeshCount > 0) {
        scene->warnings << QObject::tr(
            "%1 surface object(s) carry no render mesh and cannot be shown. "
            "Re-save the file in Rhino without \"Save Small\".").arg(noMeshCount);
    }
    if (instanceRefCount > 0) {
        scene->warnings << QObject::tr(
            "%1 block instance(s) are not supported yet and were skipped.").arg(instanceRefCount);
    }
    if (scene->isEmpty() && scene->warnings.isEmpty()) {
        scene->warnings << QObject::tr(
            "The file was read successfully but contains no drawable surfaces.");
    }

    // Empty layers only clutter the layer list — but keep them if the whole
    // model is empty, otherwise the user is left with a blank list and no clue.
    if (!scene->isEmpty()) {
        for (int i = scene->layers.size() - 1; i >= 0; --i)
            if (scene->layers.at(i).isEmpty())
                scene->layers.remove(i);
    }

    if (progress)
        progress(progressCtx, 1.0f);

    return scene;
}

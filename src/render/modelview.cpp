/*
  harbour-3dmviewer — modelview.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "render/modelview.h"

#include "io/modelloader.h"

#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QPointF>
#include <QVector4D>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QHash>
#include <QVector>
#include <QtMath>

const float ModelView::kOrbitDegPerPixel = 0.30f;

// ---------------------------------------------------------------- shaders ---
//
// GLSL ES 1.00. No "#version" line: on GLES2 its absence means 100, and
// spelling it out breaks on some drivers that only accept "#version 100" as the
// very first byte including whitespace. Not worth the risk for zero benefit.

static const char *kVertexShader =
    "attribute highp vec3 aPosition;\n"
    "attribute mediump vec3 aNormal;\n"
    "attribute lowp vec3 aColor;\n"
    "uniform highp mat4 uMvp;\n"
    "uniform highp mat4 uModelView;\n"
    "uniform mediump mat3 uNormalMatrix;\n"
    "varying mediump vec3 vNormal;\n"
    "varying highp vec3 vEyePos;\n"
    "varying lowp vec3 vColor;\n"
    "void main() {\n"
    "    vNormal = uNormalMatrix * aNormal;\n"
    "    vEyePos = (uModelView * vec4(aPosition, 1.0)).xyz;\n"
    "    vColor = aColor;\n"
    "    gl_Position = uMvp * vec4(aPosition, 1.0);\n"
    "}\n";

// A headlight: the light sits at the camera, so whatever you turn towards you
// is lit. For a viewer with no light editing that is the only model that never
// leaves the user staring at an unlit face wondering if the geometry is broken.
//
// Two-sided shading via gl_FrontFacing matters more than it looks: CAD meshes
// routinely have inconsistent face winding, and with single-sided lighting
// every reversed face would render black.
static const char *kFragmentShader =
    "varying mediump vec3 vNormal;\n"
    "varying highp vec3 vEyePos;\n"
    "varying lowp vec3 vColor;\n"
    "uniform mediump float uRim;\n"
    "uniform lowp float uAlpha;\n"
    // 1 = draw the colour as-is. Lighting a line is meaningless: an edge has no
    // surface, and shading it by the vertex normal makes half the wireframe
    // vanish against the background.
    "uniform lowp float uUnlit;\n"
    "void main() {\n"
    "    mediump vec3 n = normalize(vNormal);\n"
    "    if (!gl_FrontFacing) n = -n;\n"
    "    mediump vec3 l = normalize(-vEyePos);\n"
    "    mediump float diff = max(dot(n, l), 0.0);\n"
    "    mediump float rim = pow(1.0 - diff, 3.0);\n"
    "    mediump vec3 c = vColor * (0.32 + 0.68 * diff) + vec3(0.16) * rim * uRim;\n"
    "    c = mix(c, vColor, uUnlit);\n"
    "    gl_FragColor = vec4(min(c, vec3(1.0)), uAlpha);\n"
    "}\n";

namespace {

// One GPU batch: the merged geometry of a single layer.
struct GpuBatch
{
    QOpenGLBuffer vbo;
    QOpenGLBuffer ibo;      // triangles, or toolpath lines
    QOpenGLBuffer ebo;      // wireframe edges
    int indexCount = 0;
    int edgeCount = 0;
    GLenum indexType = GL_UNSIGNED_INT;
    // GL_TRIANGLES for a surface, GL_LINES for a sliced toolpath.
    GLenum drawMode = GL_TRIANGLES;
    bool visible = true;

    // Copy of SceneLayer::sliceStart. Empty unless the format has print
    // slices; when it is filled, a slice range turns into a first index and a
    // count handed straight to glDrawElements.
    QVector<int> sliceStart;

    GpuBatch()
        : vbo(QOpenGLBuffer::VertexBuffer)
        , ibo(QOpenGLBuffer::IndexBuffer)
        , ebo(QOpenGLBuffer::IndexBuffer) {}

    int indexSizeBytes() const
    {
        return indexType == GL_UNSIGNED_INT ? int(sizeof(quint32))
                                            : int(sizeof(quint16));
    }
};

/*
  Picks the edges worth drawing as a wireframe.

  THE ANGLE BETWEEN FACES IS THE WRONG TEST
  -----------------------------------------
  The obvious rule — draw an edge where the two triangles either side turn by
  more than N degrees — cannot work. A 6 degree ramp meeting a floor is a real
  edge and must be drawn, while a cylinder tessellated into 32 facets turns
  about 11 degrees at every one of its interior edges and must not be. No
  threshold separates those two.

  THE TOPOLOGY IS THE RIGHT TEST
  ------------------------------
  Rhino meshes each brep face separately, and the reader appends each face's
  mesh with its own block of vertices. Two neighbouring faces therefore share no
  vertex indices at all, so every real face boundary is already a TOPOLOGICAL
  boundary — an edge belonging to one triangle only — whatever angle it turns
  through. The tessellation edges inside a single smooth face do share their
  vertices, and drop out on their own.

  So: draw the boundary edges. The 6 degree ramp appears because it is a
  different face, not because it is steep enough.

  THE ONE CASE THAT BREAKS
  ------------------------
  An unwelded mesh — every triangle carrying its own three vertices, as an STL
  import does — has NO shared edges, so the rule above would draw the entire
  triangulation. That is detected (nearly every edge coming out as a boundary is
  not a plausible shape) and the vertices are then welded by position first, at
  which point the same rule applies to the welded topology, with an angle test
  as the only tool left for a mesh that carries no face structure at all.
*/
// Only used on unwelded meshes, where topology tells us nothing.
const float kFallbackCreaseDegrees = 20.0f;
// Interior creases in a welded mesh: a safety net for mesh objects that really
// do have a hard edge across shared vertices. Deliberately steep so it cannot
// start drawing tessellation.
const float kWeldedCreaseDegrees = 40.0f;
// Above this share of boundary edges the mesh is not a surface, it is loose
// triangles.
const float kUnweldedShare = 0.6f;

QVector<quint32> buildEdgeIndices(const QVector<float> &verts,
                                  const QVector<quint32> &tris)
{
    const int vertexCount = verts.size() / kFloatsPerVertex;
    if (vertexCount <= 0 || tris.isEmpty())
        return QVector<quint32>();

    const auto vertexAt = [&](quint32 i) {
        const int o = int(i) * kFloatsPerVertex;
        return QVector3D(verts.at(o), verts.at(o + 1), verts.at(o + 2));
    };

    struct EdgeFaces { QVector3D n0; QVector3D n1; int count; };

    // `remap` is identity on the first pass and a weld map on the second.
    const auto collect = [&](const QVector<quint32> &remap,
                             QHash<quint64, EdgeFaces> *out) {
        out->clear();
        out->reserve(tris.size());
        for (int t = 0; t + 2 < tris.size(); t += 3) {
            const quint32 ra = remap.isEmpty() ? tris.at(t) : remap.at(tris.at(t));
            const quint32 rb = remap.isEmpty() ? tris.at(t + 1) : remap.at(tris.at(t + 1));
            const quint32 rc = remap.isEmpty() ? tris.at(t + 2) : remap.at(tris.at(t + 2));

            QVector3D n = QVector3D::crossProduct(vertexAt(tris.at(t + 1)) - vertexAt(tris.at(t)),
                                                  vertexAt(tris.at(t + 2)) - vertexAt(tris.at(t)));
            if (n.lengthSquared() < 1e-20f)
                continue;                     // degenerate triangle: no opinion
            n.normalize();

            const quint32 tri[3][2] = { { ra, rb }, { rb, rc }, { rc, ra } };
            for (int e = 0; e < 3; ++e) {
                const quint32 lo = qMin(tri[e][0], tri[e][1]);
                const quint32 hi = qMax(tri[e][0], tri[e][1]);
                if (lo == hi)
                    continue;
                const quint64 key = (quint64(lo) << 32) | quint64(hi);
                QHash<quint64, EdgeFaces>::iterator it = out->find(key);
                if (it == out->end()) {
                    EdgeFaces ef; ef.n0 = n; ef.count = 1;
                    out->insert(key, ef);
                } else {
                    if (it->count == 1)
                        it->n1 = n;
                    it->count++;
                }
            }
        }
    };

    QHash<quint64, EdgeFaces> edges;
    collect(QVector<quint32>(), &edges);

    int boundary = 0;
    for (QHash<quint64, EdgeFaces>::const_iterator it = edges.constBegin();
         it != edges.constEnd(); ++it)
        if (it->count == 1)
            ++boundary;

    float creaseDegrees = kWeldedCreaseDegrees;

    if (!edges.isEmpty() && float(boundary) / float(edges.size()) > kUnweldedShare) {
        // Loose triangles. Weld by position, then judge by angle — the only
        // signal such a mesh carries.
        QHash<quint64, quint32> byPosition;
        QVector<quint32> remap(vertexCount);
        // A grid a thousandth of the model across: fine enough not to merge
        // distinct corners, coarse enough to catch float noise between two
        // copies of the same point.
        float extent = 0.0f;
        for (int i = 0; i < vertexCount; ++i)
            extent = qMax(extent, vertexAt(quint32(i)).length());
        const float cell = qMax(extent * 1e-4f, 1e-6f);

        for (int i = 0; i < vertexCount; ++i) {
            const QVector3D p = vertexAt(quint32(i)) / cell;
            const quint64 key = (quint64(qint32(p.x()) & 0x1fffff) << 42)
                                | (quint64(qint32(p.y()) & 0x1fffff) << 21)
                                | quint64(qint32(p.z()) & 0x1fffff);
            QHash<quint64, quint32>::const_iterator f = byPosition.constFind(key);
            if (f == byPosition.constEnd()) {
                byPosition.insert(key, quint32(i));
                remap[i] = quint32(i);
            } else {
                remap[i] = f.value();
            }
        }

        collect(remap, &edges);
        creaseDegrees = kFallbackCreaseDegrees;
    }

    const float cosLimit = qCos(qDegreesToRadians(creaseDegrees));
    QVector<quint32> out;
    out.reserve(edges.size());
    for (QHash<quint64, EdgeFaces>::const_iterator it = edges.constBegin();
         it != edges.constEnd(); ++it) {
        const EdgeFaces &ef = it.value();
        // An edge with one triangle is an open boundary — a face edge in a
        // brep, or the rim of an open mesh. Always drawn. An edge with two is
        // interior, and only a genuine hard crease across shared vertices
        // survives.
        const bool keep = (ef.count == 1)
                          || QVector3D::dotProduct(ef.n0, ef.n1) < cosLimit;
        if (!keep)
            continue;
        out << quint32(it.key() >> 32) << quint32(it.key() & 0xffffffffu);
    }
    return out;
}

} // namespace

// --------------------------------------------------------------- renderer ---

class ModelViewRenderer : public QQuickFramebufferObject::Renderer,
                          protected QOpenGLFunctions
{
public:
    ModelViewRenderer()
        : m_program(nullptr)
        , m_sceneGeneration(0)
        , m_uploaded(false)
        , m_uintIndicesSupported(true)
        , m_aspect(1.0f)
    {
        ++s_liveRenderers;
        initializeOpenGLFunctions();

        // GLES2 only guarantees 16-bit indices. Every GPU we care about has the
        // extension (and a GLES3 context has it in core), but a model whose
        // layer exceeds 65535 vertices would otherwise render as garbage rather
        // than fail loudly, so the capability is checked instead of assumed.
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (ctx) {
            m_uintIndicesSupported =
                ctx->hasExtension(QByteArrayLiteral("GL_OES_element_index_uint"))
                || ctx->format().majorVersion() >= 3
                || !ctx->isOpenGLES();
        }
    }

    ~ModelViewRenderer() override
    {
        --s_liveRenderers;
        releaseBatches();
        delete m_program;
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        m_aspect = size.height() > 0 ? float(size.width()) / float(size.height()) : 1.0f;

        /*
          Multisampling at whatever level this GPU turned out to afford. It is
          worth asking for — aliasing on CAD edges is the most visible quality
          difference on a phone — but it cannot be checked here: on a Mali-G610
          the allocation is lazy, so construction succeeds, the framebuffer
          reports itself complete, and every draw into it then fails with
          GL_OUT_OF_MEMORY. The level is decided in render() by whether drawing
          worked, and remembered in s_maxSamples for the other views.
        */
        loadRememberedSampleCount();

        QOpenGLFramebufferObjectFormat fmt;
        // Depth is not optional — without it the model draws in file order and
        // the back of the part paints over the front.
        fmt.setAttachment(QOpenGLFramebufferObject::Depth);
        fmt.setSamples(s_maxSamples);
        return new QOpenGLFramebufferObject(size, fmt);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        // Runs with the GUI thread blocked: the only safe place to read the item.
        ModelView *view = static_cast<ModelView *>(item);

        // While the model is falling its pose comes from the simulation, which
        // already works in eye space — so it IS the modelview matrix and the
        // camera's own view matrix is bypassed entirely. That is what lets the
        // part keep tumbling in place while the camera stays put.
        m_view = view->m_sim.isActive() ? view->m_sim.modelViewMatrix()
                                        : view->m_camera.viewMatrix();
        m_proj = view->m_camera.projectionMatrix(m_aspect);
        m_background = view->m_background;
        m_displayMode = int(view->m_displayMode);

        ModelLoader *loader = view->loader();
        const ScenePtr scene = loader ? loader->scene() : ScenePtr();

        if (view->m_sceneGeneration != m_sceneGeneration || m_scene != scene) {
            m_scene = scene;
            m_sceneGeneration = view->m_sceneGeneration;
            m_uploaded = false;   // rebuild buffers on the next render()
        }

        // Layer visibility is cheap to mirror every sync and saves any
        // cross-thread signalling for the common case of toggling a switch.
        if (!m_scene.isNull() && m_batches.size() == m_scene->layers.size()) {
            for (int i = 0; i < m_batches.size(); ++i)
                m_batches[i]->visible = m_scene->layers.at(i).visible;
        }

        // Dragging the slice slider changes only these two numbers; no
        // geometry is rebuilt and nothing is uploaded again.
        m_sliceMin = view->m_sliceMin;
        m_sliceMax = view->m_sliceMax;

        // Drawn from the moment the page prepares it, not only once the part
        // is released — otherwise the page opens black and snaps into a lit
        // room a second later.
        m_roomVisible = view->m_roomReady;
        if (m_roomVisible) {
            m_frontZ = view->m_sim.roomFrontZ();
            m_backZ = view->m_sim.roomBackZ();
            m_frontHalf = view->m_sim.roomFrontHalf();
            m_backHalf = view->m_sim.roomBackHalf();
        }

        if (!m_pendingWarning.isEmpty()) {
            view->setRenderWarning(m_pendingWarning);
            m_pendingWarning.clear();
        }
    }

    void render() override
    {
        if (!m_program && !buildProgram())
            return;
        if (!m_uploaded)
            uploadScene();

        if (!m_diagnosed) {
            m_diagnosed = true;
            dumpRenderEnvironment();
        }

        // Whatever the previous frame ran into, including the driver finally
        // admitting it could not afford the target it happily handed us.
        if (stepDownIfOutOfMemory())
            return;

        glClearColor(float(m_background.redF()), float(m_background.greenF()),
                     float(m_background.blueF()), 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        // Culling stays OFF on purpose: see the two-sided lighting note above.
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        if (m_batches.isEmpty())
            return;

        if (m_roomVisible)
            drawRoom();

        const QMatrix4x4 mvp = m_proj * m_view;

        // 0 Shaded, 1 Wireframe, 2 Ghosted — see ModelView::DisplayMode.
        const bool wireframe = (m_displayMode == 1);
        const bool ghosted = (m_displayMode == 2);

        if (ghosted) {
            // See-through: keep testing depth so the room still occludes
            // properly, but stop WRITING it, or the first surface drawn would
            // hide everything behind it and the model would not be see-through
            // at all. Draw order is then unsorted, which for a ghost is fine.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        m_program->bind();
        m_program->setUniformValue("uMvp", mvp);
        m_program->setUniformValue("uModelView", m_view);
        // Model matrix is identity (world-space vertices), so the normal matrix
        // is just the view's rotation part.
        m_program->setUniformValue("uNormalMatrix", m_view.normalMatrix());
        m_program->setUniformValue("uRim", wireframe ? 0.0f : 1.0f);
        m_program->setUniformValue("uAlpha", ghosted ? 0.34f : 1.0f);
        m_program->setUniformValue("uUnlit", wireframe ? 1.0f : 0.0f);

        const int posLoc = m_program->attributeLocation("aPosition");
        const int nrmLoc = m_program->attributeLocation("aNormal");
        const int colLoc = m_program->attributeLocation("aColor");

        const int stride = kFloatsPerVertex * int(sizeof(float));

        for (int i = 0; i < m_batches.size(); ++i) {
            GpuBatch *b = m_batches.at(i);
            if (!b->visible || b->indexCount == 0)
                continue;

            // A toolpath is already lines; the wireframe mode has nothing to
            // strip from it, so it draws the same geometry rather than
            // vanishing (its edge buffer is deliberately never built).
            const bool asLines = (b->drawMode == GL_LINES);
            const bool useEdges = wireframe && !asLines;
            if (wireframe && !asLines && b->edgeCount == 0)
                continue;

            // Which part of the index buffer this slice range selects. For a
            // format without slices it is simply all of it.
            int first = 0;
            int count = useEdges ? b->edgeCount : b->indexCount;
            if (!useEdges && b->sliceStart.size() >= 2) {
                // The table has one entry per slice plus a closing one, so the
                // highest addressable slice is size - 2. Clamping against that
                // rather than adding one to m_sliceMax matters: the default
                // "no limit" value is INT_MAX, and incrementing it first would
                // overflow.
                const int lastSlice = b->sliceStart.size() - 2;
                const int lo = qBound(0, m_sliceMin, lastSlice);
                const int hi = qBound(lo, qMin(m_sliceMax, lastSlice), lastSlice);
                first = b->sliceStart.at(lo);
                count = b->sliceStart.at(hi + 1) - first;
                if (count <= 0)
                    continue;
            }

            b->vbo.bind();
            if (useEdges)
                b->ebo.bind();
            else
                b->ibo.bind();

            m_program->enableAttributeArray(posLoc);
            m_program->setAttributeBuffer(posLoc, GL_FLOAT, 0, 3, stride);
            m_program->enableAttributeArray(nrmLoc);
            m_program->setAttributeBuffer(nrmLoc, GL_FLOAT, 3 * int(sizeof(float)), 3, stride);
            m_program->enableAttributeArray(colLoc);
            m_program->setAttributeBuffer(colLoc, GL_FLOAT, 6 * int(sizeof(float)), 3, stride);

            // Lines carry no meaningful normal, so they are drawn unlit
            // whatever the display mode says.
            if (asLines)
                m_program->setUniformValue("uUnlit", 1.0f);

            // The offset argument is a BYTE offset into the bound buffer, not
            // an index count — multiplying by the index size is what makes the
            // slice range land on the right element.
            const void *offset = reinterpret_cast<const void *>(
                qintptr(first) * b->indexSizeBytes());
            glDrawElements(useEdges ? GL_LINES : b->drawMode,
                           count, b->indexType, offset);
            reportFirstGlError("glDrawElements", i, count);

            if (asLines)
                m_program->setUniformValue("uUnlit", wireframe ? 1.0f : 0.0f);

            m_program->disableAttributeArray(posLoc);
            m_program->disableAttributeArray(nrmLoc);
            m_program->disableAttributeArray(colLoc);
            if (useEdges)
                b->ebo.release();
            else
                b->ibo.release();
            b->vbo.release();
        }

        m_program->release();

        if (ghosted) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        // Qt Quick's own renderer shares this context and assumes nothing was
        // left enabled. Leaving depth testing on paints the Silica UI wrong.
        glDisable(GL_DEPTH_TEST);
    }

    /*
      Draws the five inner walls of the room the body falls in: back, floor,
      ceiling and both sides. There is no front wall — that is the glass we are
      looking through.

      The room's coordinates are ALREADY in eye space (that is where the
      simulation lives), so unlike the model it needs no view transform at all:
      its modelview is the identity and only the projection applies.

      The geometry is 30 vertices rebuilt per frame into a plain array rather
      than a VBO — at that size the upload machinery would cost more than the
      data, and the box changes whenever the viewport does.

      Shading comes for free from the same headlight shader: the back wall faces
      the camera and comes out bright, the side walls are edge-on and fall away
      dark. That gradient is what makes the space read as a box rather than a
      flat backdrop.
    */
    void drawRoom()
    {
        // Two rectangles, near and far. Equal for a parallel box, growing with
        // depth for a frustum room — one code path draws both.
        const float fx = m_frontHalf.x(), fy = m_frontHalf.y(), z1 = m_frontZ;
        const float bx = m_backHalf.x(),  by = m_backHalf.y(),  z0 = m_backZ;

        // Dim enough to stay behind the model rather than compete with it. The
        // back wall is lifted slightly so the closed-off depth is unmistakable.
        // The far wall faces the camera head-on and so takes the full lambert
        // term, which made it the brightest thing in the room; the side walls
        // are edge-on and fall away almost to black. Pulling the far wall down
        // to 0.11 evens that out and stops it competing with the model.
        const float back = 0.11f, side = 0.13f, floor = 0.16f;

        QVector<float> v;
        v.reserve(30 * kFloatsPerVertex);

        struct Quad { QVector3D a, b, c, d, n; float g; };
        const Quad quads[5] = {
            // far wall
            { QVector3D(-bx, -by, z0), QVector3D(bx, -by, z0),
              QVector3D(bx, by, z0), QVector3D(-bx, by, z0), QVector3D(0, 0, 1), back },
            // floor
            { QVector3D(-bx, -by, z0), QVector3D(bx, -by, z0),
              QVector3D(fx, -fy, z1), QVector3D(-fx, -fy, z1), QVector3D(0, 1, 0), floor },
            // ceiling
            { QVector3D(-bx, by, z0), QVector3D(bx, by, z0),
              QVector3D(fx, fy, z1), QVector3D(-fx, fy, z1), QVector3D(0, -1, 0), side },
            // left
            { QVector3D(-bx, -by, z0), QVector3D(-bx, by, z0),
              QVector3D(-fx, fy, z1), QVector3D(-fx, -fy, z1), QVector3D(1, 0, 0), side },
            // right
            { QVector3D(bx, -by, z0), QVector3D(bx, by, z0),
              QVector3D(fx, fy, z1), QVector3D(fx, -fy, z1), QVector3D(-1, 0, 0), side },
        };

        for (int i = 0; i < 5; ++i) {
            const Quad &q = quads[i];
            const QVector3D tri[6] = { q.a, q.b, q.c, q.a, q.c, q.d };
            for (int k = 0; k < 6; ++k) {
                v << tri[k].x() << tri[k].y() << tri[k].z()
                  << q.n.x() << q.n.y() << q.n.z()
                  << q.g << q.g << q.g;
            }
        }

        m_program->bind();
        m_program->setUniformValue("uMvp", m_proj);
        m_program->setUniformValue("uModelView", QMatrix4x4());
        m_program->setUniformValue("uNormalMatrix", QMatrix3x3());
        // No rim light on the walls. The rim term peaks at grazing angles, so
        // on a room it does the opposite of what it should: the side walls,
        // which are nearly edge-on and ought to fall away dark, came out
        // BRIGHTER than the far wall facing us. Straight lambert reads as a
        // room; with the rim it read as a glowing tunnel mouth.
        m_program->setUniformValue("uRim", 0.0f);
        m_program->setUniformValue("uAlpha", 1.0f);
        m_program->setUniformValue("uUnlit", 0.0f);

        const int posLoc = m_program->attributeLocation("aPosition");
        const int nrmLoc = m_program->attributeLocation("aNormal");
        const int colLoc = m_program->attributeLocation("aColor");
        const int stride = kFloatsPerVertex * int(sizeof(float));
        const float *base = v.constData();

        // The room is the ONE place drawing from a client-side array instead of
        // a VBO, and GLES2 changes what the pointer below means depending on
        // GL state: with a buffer bound to GL_ARRAY_BUFFER, glVertexAttribPointer
        // reads its argument as a BYTE OFFSET INTO THAT BUFFER, not as an
        // address. Qt Quick shares this context and makes no promise about what
        // it left bound when render() is entered, so `base` could be read as an
        // offset of some hundred million bytes into a scene graph buffer — a
        // driver-dependent out-of-bounds fetch that works on one GPU and
        // crashes on the next. Unbinding first is what makes the pointer a
        // pointer. (Only ARRAY_BUFFER matters here: the room is drawn with
        // glDrawArrays, which never touches the element buffer binding.)
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_program->enableAttributeArray(posLoc);
        m_program->setAttributeArray(posLoc, base, 3, stride);
        m_program->enableAttributeArray(nrmLoc);
        m_program->setAttributeArray(nrmLoc, base + 3, 3, stride);
        m_program->enableAttributeArray(colLoc);
        m_program->setAttributeArray(colLoc, base + 6, 3, stride);

        glDrawArrays(GL_TRIANGLES, 0, v.size() / kFloatsPerVertex);

        m_program->disableAttributeArray(posLoc);
        m_program->disableAttributeArray(nrmLoc);
        m_program->disableAttributeArray(colLoc);
        m_program->release();
    }

private:
    /*
      What the driver is, and what it made of our buffers. "The model does not
      appear" is the least diagnosable bug a renderer has — no error, no crash,
      no log — and the phone that misbehaves is the one that cannot be attached
      to. Unconditional, because an app started from its icon has no environment
      anybody can set: the shared part once per process, one line per view.
    */
    void dumpRenderEnvironment()
    {
        if (!s_describedGpu) {
            s_describedGpu = true;
            const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
            const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
            const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
            QOpenGLContext *ctx = QOpenGLContext::currentContext();
            qWarning("GLDIAG %s / %s / %s", vendor ? vendor : "?",
                     renderer ? renderer : "?", version ? version : "?");
            if (ctx) {
                qWarning("GLDIAG es=%d %d.%d depth=%d uintIndices=%d",
                         int(ctx->isOpenGLES()), ctx->format().majorVersion(),
                         ctx->format().minorVersion(), ctx->format().depthBufferSize(),
                         int(m_uintIndicesSupported));
            }
        }

        int totalIndices = 0;
        int visibleBatches = 0;
        for (int i = 0; i < m_batches.size(); ++i) {
            const GpuBatch *b = m_batches.at(i);
            totalIndices += b->indexCount;
            if (b->visible && b->indexCount > 0)
                ++visibleBatches;
        }

        // An incomplete framebuffer draws nothing and says nothing, so its
        // status belongs in the same line as what we tried to draw into it.
        QOpenGLFramebufferObject *fbo = framebufferObject();
        qWarning("GLDIAG view fbo=%u %dx%d samples=%d status=0x%x linked=%d"
                 " batches=%d visible=%d indices=%d uploadedKiB=%d liveViews=%d",
                 fbo ? fbo->handle() : 0u,
                 fbo ? fbo->size().width() : 0, fbo ? fbo->size().height() : 0,
                 fbo ? fbo->format().samples() : -1,
                 unsigned(glCheckFramebufferStatus(GL_FRAMEBUFFER)),
                 int(m_program && m_program->isLinked()),
                 m_batches.size(), visibleBatches, totalIndices,
                 int(m_uploadedBytes / 1024), s_liveRenderers);

        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
            qWarning("GLDIAG pre-draw glGetError=0x%x", unsigned(e));
    }

    /*
      Gives up multisampling a rung at a time on GL_OUT_OF_MEMORY. A driver may
      defer the allocation behind a framebuffer object — Mali does — so the only
      honest question is whether the last frame worked, not what the GPU claims
      up front. True means the target is being rebuilt: abandon this frame.
    */
    bool stepDownIfOutOfMemory()
    {
        bool oom = false;
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
            if (e == GL_OUT_OF_MEMORY)
                oom = true;
        }
        if (!oom)
            return false;

        /*
          Re-upload as well. A glBufferData issued while the context is out of
          memory does not fail loudly: it leaves an empty buffer that stays
          empty for the life of the view, however healthy the context becomes
          afterwards. That is what left two of the three pages blank and the
          third correct.
        */
        releaseBatches();
        m_uploaded = false;

        if (s_maxSamples <= 0) {
            // Nothing left to give up. One re-upload, then say so plainly: the
            // render target is not what is exhausting this GPU.
            if (!m_oomRetried) {
                m_oomRetried = true;
                qWarning("GLDIAG GL_OUT_OF_MEMORY with multisampling already off"
                         " — discarding this view's buffers and uploading again");
                update();
                return true;
            }
            if (!m_oomReported) {
                m_oomReported = true;
                qWarning("GLDIAG GL_OUT_OF_MEMORY persists with multisampling off"
                         " and after a re-upload — the render target is not what"
                         " is exhausting this GPU");
            }
            return false;
        }

        const int was = s_maxSamples;
        s_maxSamples = (s_maxSamples > 2) ? 2 : 0;
        qWarning("GLDIAG GL_OUT_OF_MEMORY drawing into a %d-sample target;"
                 " rebuilding every view with %d", was, s_maxSamples);
        rememberSampleCount(s_maxSamples);

        invalidateFramebufferObject();
        update();
        return true;
    }

    /*
      What this GPU turned out to afford, kept across launches — otherwise the
      discovery is made again on every start, and the views that allocate before
      the step-down come up blank each time. Same settings.conf PhysicsSettings
      uses.
    */
    static QString settingsFile()
    {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(dir);
        return dir + QStringLiteral("/settings.conf");
    }

    static void loadRememberedSampleCount()
    {
        if (s_samplesLoaded)
            return;
        s_samplesLoaded = true;
        QSettings st(settingsFile(), QSettings::IniFormat);
        const int stored = st.value(QStringLiteral("gl/maxSamples"), 4).toInt();
        // Only ever believe a value we could have written ourselves.
        if (stored == 0 || stored == 2 || stored == 4)
            s_maxSamples = stored;
    }

    static void rememberSampleCount(int samples)
    {
        QSettings st(settingsFile(), QSettings::IniFormat);
        st.setValue(QStringLiteral("gl/maxSamples"), samples);
    }

    // Reports at most one draw-call error per renderer: a broken draw usually
    // repeats every frame, and a log line per frame per batch would bury it.
    void reportFirstGlError(const char *where, int batch, int count)
    {
        if (m_drawErrorReported)
            return;
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            return;
        m_drawErrorReported = true;
        qWarning("GLDIAG %s failed: batch=%d count=%d glGetError=0x%x",
                 where, batch, count, unsigned(e));
    }

    bool buildProgram()
    {
        m_program = new QOpenGLShaderProgram;
        if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
            || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
            || !m_program->link()) {
            m_pendingWarning = QObject::tr("Shader compilation failed: %1")
                                   .arg(m_program->log().trimmed());
            delete m_program;
            m_program = nullptr;
            return false;
        }
        return true;
    }

    void releaseBatches()
    {
        m_uploadedBytes = 0;
        for (int i = 0; i < m_batches.size(); ++i) {
            m_batches.at(i)->vbo.destroy();
            m_batches.at(i)->ibo.destroy();
            m_batches.at(i)->ebo.destroy();
            delete m_batches.at(i);
        }
        m_batches.clear();
    }

    void uploadScene()
    {
        releaseBatches();
        m_uploaded = true;
        m_uploadedBytes = 0;

        if (m_scene.isNull())
            return;

        int skipped = 0;

        for (int li = 0; li < m_scene->layers.size(); ++li) {
            const SceneLayer &L = m_scene->layers.at(li);
            GpuBatch *b = new GpuBatch;
            b->visible = L.visible;
            b->drawMode = (L.primitive == PrimitiveLines) ? GL_LINES : GL_TRIANGLES;
            b->sliceStart = L.sliceStart;
            m_batches.append(b);

            if (L.isEmpty())
                continue;

            const int vertexCount = L.vertices.size() / kFloatsPerVertex;

            b->vbo.create();
            b->vbo.bind();
            b->vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
            b->vbo.allocate(L.vertices.constData(), L.vertices.size() * int(sizeof(float)));
            m_uploadedBytes += L.vertices.size() * qint64(sizeof(float));
            b->vbo.release();

            b->ibo.create();
            b->ibo.bind();
            b->ibo.setUsagePattern(QOpenGLBuffer::StaticDraw);

            if (m_uintIndicesSupported) {
                b->indexType = GL_UNSIGNED_INT;
                b->ibo.allocate(L.indices.constData(), L.indices.size() * int(sizeof(quint32)));
                b->indexCount = L.indices.size();
            } else if (vertexCount <= 65535) {
                // Narrow to 16 bits, which this batch provably fits into.
                QVector<quint16> narrow;
                narrow.reserve(L.indices.size());
                for (int k = 0; k < L.indices.size(); ++k)
                    narrow.append(quint16(L.indices.at(k)));
                b->indexType = GL_UNSIGNED_SHORT;
                b->ibo.allocate(narrow.constData(), narrow.size() * int(sizeof(quint16)));
                b->indexCount = narrow.size();
            } else {
                // Would need splitting and re-indexing. No GPU this app targets
                // lands here, so say so rather than draw a corrupted mesh.
                b->indexCount = 0;
                ++skipped;
            }
            b->ibo.release();

            // Wireframe edges. Built here rather than lazily so switching
            // display mode is instant; it is one pass over the triangles and
            // costs a few milliseconds on the reference model.
            //
            // Skipped for a toolpath: buildEdgeIndices reads the index buffer
            // three at a time as triangles, and a line list read that way is
            // not merely useless but nonsense.
            if (L.primitive == PrimitiveLines)
                continue;

            const QVector<quint32> edges = buildEdgeIndices(L.vertices, L.indices);
            if (!edges.isEmpty() && m_uintIndicesSupported) {
                b->ebo.create();
                b->ebo.bind();
                b->ebo.setUsagePattern(QOpenGLBuffer::StaticDraw);
                b->ebo.allocate(edges.constData(), edges.size() * int(sizeof(quint32)));
                b->ebo.release();
                b->edgeCount = edges.size();
            } else if (!edges.isEmpty() && vertexCount <= 65535) {
                QVector<quint16> narrow;
                narrow.reserve(edges.size());
                for (int k = 0; k < edges.size(); ++k)
                    narrow.append(quint16(edges.at(k)));
                b->ebo.create();
                b->ebo.bind();
                b->ebo.setUsagePattern(QOpenGLBuffer::StaticDraw);
                b->ebo.allocate(narrow.constData(), narrow.size() * int(sizeof(quint16)));
                b->ebo.release();
                b->edgeCount = narrow.size();
            }
        }

        if (skipped > 0) {
            m_pendingWarning = QObject::tr(
                "%1 layer(s) are too large for this GPU's 16-bit vertex indices "
                "and were not drawn.").arg(skipped);
        }
    }

    QOpenGLShaderProgram *m_program;
    QVector<GpuBatch *> m_batches;

    ScenePtr m_scene;
    quint64 m_sceneGeneration;
    bool m_uploaded;
    bool m_uintIndicesSupported;
    bool m_diagnosed = false;
    bool m_drawErrorReported = false;
    bool m_oomReported = false;
    bool m_oomRetried = false;
    qint64 m_uploadedBytes = 0;

    // Shared: what one view learns about this GPU is true for the others.
    static int s_maxSamples;
    static bool s_samplesLoaded;
    // Normally three: all three pages sit in the page stack, each with its own
    // full-screen target and its own copy of the geometry.
    static int s_liveRenderers;
    static bool s_describedGpu;

    QMatrix4x4 m_view;
    QMatrix4x4 m_proj;
    float m_aspect;
    QColor m_background;
    QString m_pendingWarning;
    int m_displayMode = 0;   // ModelView::Shaded
    // Inclusive print-slice range. Wide open until a sliced model sets it.
    int m_sliceMin = 0;
    int m_sliceMax = 0x7fffffff;

    // The room the dropped body falls in, in eye space.
    bool m_roomVisible = false;
    float m_frontZ = -1.0f;
    float m_backZ = -2.0f;
    QVector3D m_frontHalf;
    QVector3D m_backHalf;
};

// Starts optimistic; only stepDownIfOutOfMemory() ever lowers it.
int ModelViewRenderer::s_maxSamples = 4;
bool ModelViewRenderer::s_samplesLoaded = false;
int ModelViewRenderer::s_liveRenderers = 0;
bool ModelViewRenderer::s_describedGpu = false;

// ------------------------------------------------------------------- item ---

ModelView::ModelView(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
    , m_loader(nullptr)
    // A dark neutral background: CAD colours are usually saturated and light,
    // and a mid grey would fight them. Not pure black, so a black-coloured
    // layer is still distinguishable from the void.
    , m_background(QColor(24, 26, 30))
    , m_sceneGeneration(0)
    , m_sliceCount(0)
    , m_sliceMin(0)
    , m_sliceMax(0)
    , m_lastTickMs(0)
    , m_dropReturnDistance(0.0f)
    , m_debugAccum(0.0f)
    , m_mode(Viewer)
    , m_settings(nullptr)
    , m_displayMode(Shaded)
    , m_roomReady(false)
{
    setMirrorVertically(true);

    // 60 Hz drive for the simulation. The solver substeps internally, so this
    // rate only decides how often the picture is refreshed, not the accuracy.
    m_simTimer.setInterval(16);
    connect(&m_simTimer, &QTimer::timeout, this, &ModelView::onSimTick);

    m_revealTimer.setSingleShot(true);
    connect(&m_revealTimer, &QTimer::timeout, this, &ModelView::onAutoDropTimeout);
    connect(&m_tilt, &TiltSource::gravityChanged, this, &ModelView::onGravityChanged);
    connect(&m_tilt, &TiltSource::availabilityChanged,
            this, &ModelView::tiltAvailableChanged);

    // Diagnostic hook: with TILT_DEBUG set, the accelerometer runs from startup
    // so its behaviour can be inspected over ssh, where nobody can press Drop.
    if (!qEnvironmentVariableIsEmpty("TILT_DEBUG"))
        m_tilt.start();
}

bool ModelView::dropped() const
{
    return m_sim.isActive();
}

void ModelView::setUiRotation(int degrees)
{
    m_tilt.setUiRotation(degrees);
}

void ModelView::syncSimViewport()
{
    const float aspect = height() > 0 ? float(width() / height()) : 1.0f;
    m_camera.setAspect(aspect);
    m_sim.setViewport(m_camera.distance(), m_camera.fovDegrees(), aspect);
}

// How far the camera pulls back when the model is dropped.
//
// WHY THIS EXISTS AT ALL: the camera frames a model to fill the screen, and the
// drop room is sized from that same frustum — so without pulling back, the room
// is barely larger than the part and it is effectively wedged against all six
// walls. It could not fall, and tilting the phone did almost nothing, because
// there was nowhere to go.
// It is mode-dependent because the two pages need opposite things.
//
// The DROP page has to fall towards the bottom of the screen, so it needs
// vertical room — but 2.4x put the part in the far distance, so this is as
// close as it can be while still leaving somewhere to fall.
//
// The TRAMPOLINE page needs no lateral room at all: it falls straight back down
// the shaft. So it stays at the framing distance, which means the part opens
// filling the frame and touching the side walls — reading as if it were resting
// right against the glass — and then visibly recedes as it drops away.
static const float kDropZoomOutDrop = 1.7f;
static const float kDropZoomOutTrampoline = 1.0f;

// The trampoline room is this many times deeper than the normal one, all of it
// added behind the model. Widening the shaft made the same depth read as
// shallower, hence 5.5 where 4 had looked right in a narrow tube. With the phone held flat, real gravity points into
// the screen, so the part falls away down this shaft — and because the view is
// perspective, falling away IS getting smaller, and bouncing back IS getting
// bigger. No scaling trick is involved; it is just depth.
static const float kTrampolineDepth = 5.5f;

// Page 2's box, in the same room-depth units. Two rather than one: at a single
// depth the part had barely more room behind it than its own thickness.
static const float kDropDepth = 2.0f;

// Where page 2's glass sits, as a multiple of the distance at which the model
// exactly fills the frame.
//
// It cannot be 1.0. At 1.0 the model touches the edges of the picture when it
// is against the glass, and since the room is measured at that same wall, the
// room would then be exactly the size of the model with no room to fall in.
// Pushing the glass back a third gives the part somewhere to go and costs a
// quarter of its apparent size.
static const float kDropFrontFactor = 1.35f;

// Rubber. A CAD part landing on a hard floor keeps almost nothing (0.12).
//
// CAREFUL WITH THIS NUMBER: restitution is a ratio of SPEEDS, and height goes
// with the square of speed. A "95% trampoline" in the intuitive sense — comes
// back to 95% of the height — therefore needs sqrt(0.95), not 0.95. Setting it
// to 0.95 returns only 90% of the height, which is exactly the "it does not
// spring high enough" this went through.
// 0.99 rather than 0.975: with the sheet losing almost nothing, what remains is
// the small drag on the way up, and the part arrives back at the glass.
static const float kTrampolineRestitution = 0.99f;

// How far past the glass a racket stroke may throw the part before an invisible
// wall catches it, in the same "metres" the room's depth is quoted in.
static const float kFrontOverrunMetres = 2.0f;

// How much wider than the screen the trampoline shaft is. At 1.0 the part fills
// the tube and grinds down the walls; this gives it air around it, which is both
// what makes it read as falling into a room and what stops side friction eating
// the rebound.
static const float kTrampolineWidth = 2.2f;

// Gain on the phone's measured velocity when it is handed to the simulation as
// the sheet's velocity. A real swing is around 1-2 m/s, which against a part
// arriving at roughly 1000 model units/s would add a barely visible tenth. The
// point of the gesture is that it should feel like a racket, so it is amplified
// deliberately.
static const float kRacketGain = 3.5f;

// How long the model is held still before it is released.
static const int kRevealMs = 1000;

// The pose that reproduces the camera's current view exactly, so a drop can
// begin (and end) without the picture jumping. The view matrix maps world space
// to eye space; the body's pose has to reproduce both its rotation and where it
// puts the model's centre.
void ModelView::poseFromCamera(QQuaternion *orientation, QVector3D *position) const
{
    const QMatrix4x4 v = m_camera.viewMatrix();
    *position = v.map(m_sim.modelCentre());
    *orientation = QQuaternion::fromRotationMatrix(v.toGenericMatrix<3, 3>());
}

void ModelView::setSettings(PhysicsSettings *s)
{
    if (m_settings == s)
        return;
    if (m_settings)
        disconnect(m_settings, &PhysicsSettings::changed, this, &ModelView::onSettingsChanged);
    m_settings = s;
    if (m_settings) {
        connect(m_settings, &PhysicsSettings::changed, this, &ModelView::onSettingsChanged);
        connect(m_settings, &PhysicsSettings::displayModeChanged,
                this, &ModelView::onDisplayModeSettingChanged);
        // Adopt whatever is already chosen, so a page opened later matches.
        onDisplayModeSettingChanged();
    }
    emit settingsChanged();
}

// A slider moved. Re-apply everything for this page and, if a part is already
// in the air, start it over — half the values only take effect when the room is
// built, so continuing mid-flight would show a mixture of old and new.
void ModelView::onSettingsChanged(int page)
{
    if (m_mode == Viewer || int(m_mode) != page)
        return;
    armAutoDrop();
}

void ModelView::setMode(Mode m)
{
    if (m_mode == m)
        return;
    cancelDrop();
    m_mode = m;
    emit modeChanged();
}

void ModelView::armAutoDrop()
{
    if (m_mode == Viewer)
        return;
    if (!m_loader || m_loader->scene().isNull() || m_loader->scene()->isEmpty())
        return;

    // Start from a clean, framed view every time the page is entered, so the
    // fall always replays from the beginning rather than continuing wherever
    // the previous visit left the part lying.
    cancelDrop();
    fitToView();

    // Build the room NOW, not when the part is released. The walls used to
    // appear only once the simulation started, so the page opened on a black
    // background and snapped into a lit room a second later. Setting it up here
    // also means the camera is already pulled back during the pause, so
    // releasing the part changes nothing about the picture except that it
    // starts to move.
    m_sim.setScene(m_loader->scene());
    prepareRoom();
    m_roomReady = true;

    m_revealTimer.start(int(1000.0f * (m_settings ? m_settings->get(int(m_mode), "revealSeconds")
                                                  : kRevealMs / 1000.0f)));
    update();
}

// Camera framing and room geometry for the current mode. Split out because it
// is needed both when the page opens (to draw the room during the pause) and
// again at release (the viewport may have been resized in between).
void ModelView::prepareRoom()
{
    const bool trampoline = (m_mode == Trampoline);

    // Every value below has a fallback equal to the constant it replaced, so
    // the app behaves identically with no settings object attached.
    const auto tune = [&](const char *key, float fallback) {
        return m_settings ? m_settings->get(int(m_mode), key) : fallback;
    };

    if (m_dropReturnDistance <= 0.0f) {
        m_dropReturnDistance = m_camera.distance();
        m_camera.setDistance(m_camera.distance()
                             * tune("zoomOut", trampoline ? kDropZoomOutTrampoline
                                                          : kDropZoomOutDrop));
    }

    const float aspect = height() > 0 ? float(width() / height()) : 1.0f;
    m_camera.setAspect(aspect);
    m_sim.setFallRate(tune("fallRate", 14.0f));
    m_sim.setFriction(tune("friction", 0.62f));
    m_sim.setAngularDamping(tune("angularDamping", 0.5f));
    m_sim.setReleaseSpin(tune("releaseSpin", 0.85f));
    m_sim.setMaxAngularRate(tune("maxSpin", 2.0f));
    m_sim.setTorqueShare(tune("torqueShare", 0.07f));
    m_sim.setBounceSpinKeep(tune("bounceSpinKeep", 0.72f));

    m_sim.setViewport(m_camera.distance(), m_camera.fovDegrees(), aspect,
                      tune("depth", trampoline ? kTrampolineDepth : kDropDepth),
                      trampoline ? tune("width", kTrampolineWidth) : 1.0f);

    // Hard walls everywhere. On the trampoline page the far end of the shaft
    // additionally gets the sheet, which is a separate mechanism — see
    // DropSim::setTrampoline.
    m_sim.setRestitution(tune("restitution", 0.12f));
    m_sim.setTrampoline(trampoline, tune("trampoline", kTrampolineRestitution));
    // Page 3's shaft is long enough that ordinary air drag would visibly eat
    // the rebound; page 2's box is not, and there the drag helps a part settle.
    m_sim.setLinearDamping(tune("linearDamping", trampoline ? 0.005f : 0.015f));
    // Page 2: the glass sits exactly at the distance where the model fills the
    // frame, so lying against it means seen whole and at full size. Page 3
    // keeps the derived front — there the part starts at the frame-filling
    // distance already and the interest is all in the depth behind it.
    m_sim.setFrontDistance(trampoline ? 0.0f
                                      : m_dropReturnDistance * kDropFrontFactor);
    // Only the trampoline lets the part escape towards the viewer; the drop
    // page is a closed display case. The glass never bounces it back either
    // way.
    m_sim.setFrontOpen(trampoline, tune("frontOverrun", kFrontOverrunMetres));
    m_sim.setWallVelocity(QVector3D());

    // The deep room reaches far past the default far plane, which is sized for
    // the model. Without this the part would simply vanish partway down.
    m_camera.setFarOverride(-m_sim.boxMin().z() * 1.2f);
}

void ModelView::cancelDrop()
{
    m_revealTimer.stop();
    m_roomReady = false;

    const bool wasActive = m_sim.isActive();
    if (wasActive) {
        m_sim.reset();
        m_simTimer.stop();
        m_tilt.stop();
        if (m_dropReturnDistance > 0.0f)
            m_camera.setDistance(m_dropReturnDistance);
        m_camera.clearFarOverride();
        emit droppedChanged();
    }
    if (m_dropReturnDistance > 0.0f) {
        m_camera.setDistance(m_dropReturnDistance);
        m_dropReturnDistance = 0.0f;
    }
    m_camera.clearFarOverride();
    update();
}

void ModelView::onAutoDropTimeout()
{
    if (!m_loader || m_loader->scene().isNull() || m_loader->scene()->isEmpty())
        return;

    // The room and the framing were set up when the page opened; all that is
    // left is to let go. Refreshed first in case the viewport changed size
    // during the pause.
    prepareRoom();

    QQuaternion q;
    QVector3D p;
    poseFromCamera(&q, &p);
    m_sim.start(q, p);

    m_tilt.start();
    if (m_mode != Trampoline) {
        // Drop page: however the phone is being held counts as "screen
        // upright", so the part always falls towards the bottom of the screen.
        m_tilt.captureReference();
    }
    // Trampoline page: NO reference. Gravity is taken absolutely, which is the
    // whole point — hold the phone flat and "down" is into the screen, so the
    // part falls away from you down the shaft and bounces back up at you.

    m_lastTickMs = 0;
    m_simClock.start();
    m_simTimer.start();
    emit droppedChanged();
    update();
}

void ModelView::onGravityChanged()
{
    m_sim.setGravityDirection(m_tilt.gravityDirection());

    // Hand the phone's own motion to the simulation as the velocity of the
    // room's walls.
    //
    // UNITS: the sensor speaks metres per second, the model speaks whatever the
    // .3dm file was drawn in. The simulation's gravity value IS 9.81 m/s^2
    // expressed in model units, so it is exactly the conversion factor between
    // them — no guess about the file's units is needed anywhere.
    const float perMetre = m_sim.gravityAccel() / 9.81f;
    const float gain = m_settings ? m_settings->get(int(m_mode), "racketGain") : kRacketGain;
    QVector3D wall = m_tilt.deviceVelocity() * (perMetre * gain);

    // A shake can produce a large integral; cap it so a violent flick cannot
    // launch the part at a speed the solver has to deal with in one substep.
    const float maxWall = 3.0f * qSqrt(2.0f * m_sim.gravityAccel()
                                       * qMax(1.0f, m_sim.boxMax().y() - m_sim.boxMin().y()));
    if (wall.length() > maxWall)
        wall = wall.normalized() * maxWall;

    m_sim.setWallVelocity(wall);
}

void ModelView::onSimTick()
{
    const qint64 now = m_simClock.elapsed();
    // First tick after start has no previous stamp; assume one frame.
    float dt = m_lastTickMs == 0 ? 0.016f : float(now - m_lastTickMs) / 1000.0f;
    m_lastTickMs = now;
    if (dt <= 0.0f)
        return;

    const bool wasActive = m_sim.isActive();
    m_sim.step(dt);

    if (!m_sim.isActive() && wasActive) {
        // The return animation finished: stop burning CPU and hand the picture
        // back to the camera.
        m_simTimer.stop();
        emit droppedChanged();
        update();
        return;
    }

    // Twice a second, record what the body is actually doing. This is here
    // because "it does not react to tilting" and "it reacts, but into the
    // screen where perspective hides it" look identical on the screen and can
    // only be told apart from the numbers.
    m_debugAccum += dt;
    if (m_debugAccum > 0.5f) {
        m_debugAccum = 0.0f;
        qWarning("DROP: %s", qPrintable(m_sim.debugState()));
    }

    // A body that has settled needs no further repaints — but the timer stays
    // running, because tilting the phone has to be able to wake it again.
    if (!m_sim.isAsleep())
        update();
}


QQuickFramebufferObject::Renderer *ModelView::createRenderer() const
{
    return new ModelViewRenderer;
}

void ModelView::setLoader(ModelLoader *l)
{
    if (m_loader == l)
        return;

    if (m_loader) {
        disconnect(m_loader, &ModelLoader::sceneChanged, this, &ModelView::onSceneChanged);
        disconnect(m_loader, &ModelLoader::layerVisibilityChanged,
                   this, &ModelView::onLayerVisibilityChanged);
    }
    m_loader = l;
    if (m_loader) {
        connect(m_loader, &ModelLoader::sceneChanged, this, &ModelView::onSceneChanged);
        connect(m_loader, &ModelLoader::layerVisibilityChanged,
                this, &ModelView::onLayerVisibilityChanged);
    }
    emit loaderChanged();
    onSceneChanged();
}

void ModelView::setBackgroundColor(const QColor &c)
{
    if (m_background == c)
        return;
    m_background = c;
    emit backgroundColorChanged();
    update();
}

void ModelView::setRenderWarning(const QString &w)
{
    if (m_renderWarning == w)
        return;
    m_renderWarning = w;
    emit renderWarningChanged();
}

qreal ModelView::sliceTopZ() const
{
    const ScenePtr scene = m_loader ? m_loader->scene() : ScenePtr();
    if (scene.isNull() || scene->sliceZ.isEmpty())
        return 0.0;
    const int i = qBound(0, m_sliceMax, scene->sliceZ.size() - 1);
    return qreal(scene->sliceZ.at(i));
}

void ModelView::setSliceMin(int v)
{
    // The two handles cannot cross: the bottom one pushes the top one along
    // rather than producing an empty range that would blank the view.
    const int clamped = qBound(0, v, qMax(0, m_sliceCount - 1));
    if (clamped == m_sliceMin)
        return;
    m_sliceMin = clamped;
    if (m_sliceMax < m_sliceMin)
        m_sliceMax = m_sliceMin;
    emit sliceRangeChanged();
    update();
}

void ModelView::setSliceMax(int v)
{
    const int clamped = qBound(0, v, qMax(0, m_sliceCount - 1));
    if (clamped == m_sliceMax)
        return;
    m_sliceMax = clamped;
    if (m_sliceMin > m_sliceMax)
        m_sliceMin = m_sliceMax;
    emit sliceRangeChanged();
    update();
}

void ModelView::onSceneChanged()
{
    ++m_sceneGeneration;
    m_renderWarning.clear();
    emit renderWarningChanged();

    // A new model starts with every slice shown. Keeping the old range would
    // silently hide most of the next file, and the two models have no reason
    // to have the same number of layers.
    const ScenePtr scene = m_loader ? m_loader->scene() : ScenePtr();
    m_sliceCount = scene.isNull() ? 0 : scene->sliceCount;
    m_sliceMin = 0;
    m_sliceMax = qMax(0, m_sliceCount - 1);
    emit sliceRangeChanged();

    // A new model cancels any drop in progress: the body that was falling no
    // longer exists, and its contact points belong to the old geometry.
    m_revealTimer.stop();
    if (m_sim.isActive()) {
        m_sim.reset();
        m_simTimer.stop();
        m_tilt.stop();
        m_camera.clearFarOverride();
        emit droppedChanged();
    }

    fitToView();   // also calls update()
}

void ModelView::onLayerVisibilityChanged()
{
    update();
}

void ModelView::orbit(qreal dxPixels, qreal dyPixels)
{
    // While the model is falling, the phone's own tilt is the control — a
    // finger orbit here would rotate the camera out from under the simulation
    // and the floor would appear to swing away.
    if (m_sim.isActive())
        return;

    if (m_camera.rotationMode() == Camera::Free) {
        // Same degrees per pixel as the turntable, so the mode switch does not
        // also change how far a drag turns the model.
        m_camera.rotateFree(float(-dxPixels) * kOrbitDegPerPixel,
                            float(-dyPixels) * kOrbitDegPerPixel);
    } else {
        // Dragging up looks down on the model, which is what a turntable does
        // when you push the far side away — hence the sign on the pitch.
        m_camera.orbit(float(-dxPixels) * kOrbitDegPerPixel,
                       float(dyPixels) * kOrbitDegPerPixel);
    }
    emit cameraChanged();
    update();
}

ModelView::RotationMode ModelView::rotationMode() const
{
    return RotationMode(int(m_camera.rotationMode()));
}

bool ModelView::pivotSet() const
{
    return m_camera.hasPivot();
}

QPointF ModelView::pivotScreenPos() const
{
    if (!m_camera.hasPivot() || width() <= 0 || height() <= 0)
        return QPointF(-1.0, -1.0);

    const QMatrix4x4 mvp =
        m_camera.projectionMatrix(float(width() / height())) * m_camera.viewMatrix();
    const QVector4D clip = mvp * QVector4D(m_camera.pivot(), 1.0f);
    if (clip.w() <= 0.0f)
        return QPointF(-1.0, -1.0);   // behind the camera

    // Normalised device coordinates have y up; item pixels have y down.
    const float ndcX = clip.x() / clip.w();
    const float ndcY = clip.y() / clip.w();
    return QPointF((ndcX * 0.5 + 0.5) * width(),
                   (1.0 - (ndcY * 0.5 + 0.5)) * height());
}

void ModelView::cycleRotationMode()
{
    if (m_sim.isActive())
        return;

    const int next = (int(m_camera.rotationMode()) + 1) % 3;
    m_camera.setRotationMode(Camera::RotationMode(next));

    // Centre is a full reset, not just "stop using a pivot": by the time anyone
    // reaches for it the model is usually tumbled, pinned and zoomed in on.
    // Reusing setStandardView() keeps "the state a model opens in" one
    // definition rather than two.
    if (next == RotateCentred)
        setStandardView(QStringLiteral("perspective"));

    // Stepping into AboutPivot has no pivot yet — the user taps next — so until
    // they do it behaves exactly as Centred did and nothing jumps.
    emit rotationModeChanged();
    emit cameraChanged();
    update();
}

void ModelView::rollBy(qreal degrees)
{
    if (m_sim.isActive())
        return;
    m_camera.roll(float(degrees));
    emit cameraChanged();
    update();
}

namespace {

// Möller–Trumbore. Ray parameter of the hit, negative on a miss — which is also
// what a degenerate triangle gives, and the right answer for one.
float rayTriangle(const QVector3D &origin, const QVector3D &dir,
                  const QVector3D &a, const QVector3D &b, const QVector3D &c)
{
    const QVector3D ab = b - a;
    const QVector3D ac = c - a;
    const QVector3D pv = QVector3D::crossProduct(dir, ac);
    const float det = QVector3D::dotProduct(ab, pv);
    if (qAbs(det) < 1e-9f)
        return -1.0f;   // ray parallel to the triangle's plane

    const float invDet = 1.0f / det;
    const QVector3D tv = origin - a;
    const float u = QVector3D::dotProduct(tv, pv) * invDet;
    if (u < 0.0f || u > 1.0f)
        return -1.0f;

    const QVector3D qv = QVector3D::crossProduct(tv, ab);
    const float v = QVector3D::dotProduct(dir, qv) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return -1.0f;

    return QVector3D::dotProduct(ac, qv) * invDet;
}

} // namespace

bool ModelView::pickPivot(qreal x, qreal y)
{
    if (width() <= 0 || height() <= 0)
        return false;

    // Normalised screen coordinates: 0,0 at the centre, +/-1 at the edges,
    // y up — which is what the camera works in.
    const float nx = float((x / width()) * 2.0 - 1.0);
    const float ny = float(1.0 - (y / height()) * 2.0);

    m_camera.setAspect(float(width() / height()));

    QVector3D origin, dir;
    m_camera.screenRay(nx, ny, &origin, &dir);

    // Straight through every visible triangle: 16k of them on the reference
    // model, once per tap rather than per frame, so a spatial index would be a
    // structure to keep correct for nothing. Hidden layers are skipped — a
    // pivot on something invisible is indistinguishable from a miss.
    float best = -1.0f;
    if (m_loader) {
        const ScenePtr scene = m_loader->scene();
        if (!scene.isNull()) {
            for (int li = 0; li < scene->layers.size(); ++li) {
                const SceneLayer &L = scene->layers.at(li);
                if (!L.visible || L.primitive != PrimitiveTriangles)
                    continue;
                for (int i = 0; i + 2 < L.indices.size(); i += 3) {
                    const int ia = int(L.indices.at(i))     * kFloatsPerVertex;
                    const int ib = int(L.indices.at(i + 1)) * kFloatsPerVertex;
                    const int ic = int(L.indices.at(i + 2)) * kFloatsPerVertex;
                    if (ic + 2 >= L.vertices.size())
                        continue;
                    const QVector3D a(L.vertices.at(ia), L.vertices.at(ia + 1),
                                      L.vertices.at(ia + 2));
                    const QVector3D b(L.vertices.at(ib), L.vertices.at(ib + 1),
                                      L.vertices.at(ib + 2));
                    const QVector3D c(L.vertices.at(ic), L.vertices.at(ic + 1),
                                      L.vertices.at(ic + 2));
                    const float t = rayTriangle(origin, dir, a, b, c);
                    if (t > 1e-4f && (best < 0.0f || t < best))
                        best = t;
                }
            }
        }
    }

    const bool hit = (best > 0.0f);
    // A miss still sets a pivot, on the plane the model is framed in: the user
    // pressed the button, aimed and tapped, and deserves an answer.
    m_camera.setPivot(hit ? origin + dir * best
                          : m_camera.screenPointOnPlane(nx, ny, m_camera.target()));

    emit cameraChanged();
    update();
    return hit;
}

void ModelView::panBy(qreal dxPixels, qreal dyPixels)
{
    if (m_sim.isActive())
        return;
    m_camera.pan(float(dxPixels), float(dyPixels), float(qMax(1.0, height())));
    emit cameraChanged();
    update();
}

void ModelView::zoomBy(qreal factor)
{
    m_camera.zoomBy(float(factor));
    emit cameraChanged();
    update();
}

void ModelView::fitToView()
{
    // The aspect has to be current before framing, or a wide model framed in
    // portrait would be sized against the vertical field of view alone and run
    // off the sides.
    if (height() > 0)
        m_camera.setAspect(float(width() / height()));

    if (m_loader) {
        const ScenePtr s = m_loader->scene();
        if (!s.isNull() && !s->isEmpty()) {
            QVector3D lo, hi;
            s->visibleBounds(&lo, &hi);
            // The trampoline page opens with the part against the glass, so it
            // is framed edge to edge; the drop page keeps a little slack.
            m_camera.frame(lo, hi, m_mode == Trampoline ? 1.0f : 1.15f);
        }
    }
    emit cameraChanged();
    update();
}

void ModelView::setDisplayMode(DisplayMode m)
{
    // Written through to the shared settings when there are any, so choosing a
    // mode on one page changes it on the others too. The settings object then
    // signals back and every view picks it up, this one included.
    if (m_settings) {
        m_settings->setDisplayMode(int(m));
        return;
    }
    if (m_displayMode == m)
        return;
    m_displayMode = m;
    emit displayModeChanged();
    update();
}

void ModelView::onDisplayModeSettingChanged()
{
    if (!m_settings)
        return;
    const DisplayMode m = DisplayMode(m_settings->displayMode());
    if (m_displayMode == m)
        return;
    m_displayMode = m;
    emit displayModeChanged();
    update();
}

void ModelView::cycleDisplayMode()
{
    setDisplayMode(DisplayMode((int(m_displayMode) + 1) % 3));
}

bool ModelView::axisLocked() const
{
    return m_camera.axisLocked();
}

void ModelView::stepAxis(int which)
{
    if (m_sim.isActive())
        return;
    m_camera.stepAxis(which);
    fitToView();          // also repaints
    emit rotationModeChanged();   // stepAxis() leaves free rotation behind
    emit viewAxisChanged();
}

void ModelView::setFreeView()
{
    if (m_sim.isActive())
        return;
    m_camera.setFreeOrbit();
    fitToView();
    emit viewAxisChanged();
}

void ModelView::setStandardView(const QString &name)
{
    Camera::StandardView v = Camera::Perspective;
    const QString n = name.toLower();
    if (n == QLatin1String("top"))        v = Camera::Top;
    else if (n == QLatin1String("front")) v = Camera::Front;
    else if (n == QLatin1String("right")) v = Camera::Right;

    // setStandardView frames, and framing without a current aspect lets a wide
    // model run off the sides in portrait. fitToView() has always done this.
    if (height() > 0)
        m_camera.setAspect(float(width() / height()));

    QVector3D lo, hi;
    if (m_loader && !m_loader->scene().isNull()) {
        m_loader->scene()->visibleBounds(&lo, &hi);
    }
    m_camera.setStandardView(v, lo, hi);
    emit viewAxisChanged();
    update();
}

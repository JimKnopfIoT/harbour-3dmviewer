/*
  harbour-3dmviewer — stlreader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/stlreader.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QtGlobal>

#include <cstring>

namespace {

const int kBinaryHeaderSize = 80;

// The colour an STL gets, since the format carries none. A warm neutral grey
// reads as "material" rather than as a deliberate colour choice.
const QColor kDefaultColor(198, 198, 192);

float readFloatLE(const char *p)
{
    // memcpy rather than a reinterpret_cast: the offset inside the record is
    // not guaranteed to be four-byte aligned, and an unaligned float load is
    // undefined behaviour even where it happens to work.
    quint32 bits = quint32(uchar(p[0]))
                 | (quint32(uchar(p[1])) << 8)
                 | (quint32(uchar(p[2])) << 16)
                 | (quint32(uchar(p[3])) << 24);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void appendFacet(SceneLayer *layer, Scene *scene,
                 const QVector3D &a, const QVector3D &b, const QVector3D &c,
                 const QVector3D &storedNormal)
{
    QVector3D n = storedNormal;
    // Plenty of exporters write zeros, and some write normals that do not
    // match the winding at all. The triangle is the authority; the stored
    // normal is only used when it is usable and roughly agrees.
    const QVector3D geometric = QVector3D::crossProduct(b - a, c - a);
    if (n.lengthSquared() < 1e-12f || geometric.lengthSquared() < 1e-20f) {
        n = geometric;
    } else {
        n.normalize();
        QVector3D g = geometric.normalized();
        if (QVector3D::dotProduct(n, g) < 0.0f)
            n = g;   // stored normal points the wrong way; trust the winding
    }
    if (n.lengthSquared() < 1e-20f)
        n = QVector3D(0.0f, 0.0f, 1.0f);   // degenerate facet
    else
        n.normalize();

    const quint32 base = quint32(layer->vertices.size() / kFloatsPerVertex);
    const float r = float(kDefaultColor.redF());
    const float g = float(kDefaultColor.greenF());
    const float bl = float(kDefaultColor.blueF());

    const QVector3D corner[3] = { a, b, c };
    for (int i = 0; i < 3; ++i) {
        layer->vertices << corner[i].x() << corner[i].y() << corner[i].z()
                        << n.x() << n.y() << n.z()
                        << r << g << bl;
        scene->growBounds(corner[i]);
    }
    layer->indices << base << base + 1 << base + 2;
}

// --- ASCII ----------------------------------------------------------------

// Reads the three numbers after a keyword. Returns false at end of input.
bool scanVector(const QByteArray &d, int *pos, QVector3D *out)
{
    double v[3] = { 0.0, 0.0, 0.0 };
    int found = 0;
    int i = *pos;
    const int n = d.size();

    while (i < n && found < 3) {
        // Skip to the next token.
        while (i < n && (d.at(i) == ' ' || d.at(i) == '\t'))
            ++i;
        if (i >= n)
            break;
        if (d.at(i) == '\n' || d.at(i) == '\r')
            break;

        bool ok = false;
        int start = i;
        while (i < n && d.at(i) != ' ' && d.at(i) != '\t'
               && d.at(i) != '\n' && d.at(i) != '\r')
            ++i;
        const double parsed = d.mid(start, i - start).toDouble(&ok);
        if (!ok)
            return false;
        v[found++] = parsed;
    }

    if (found < 3)
        return false;
    *pos = i;
    *out = QVector3D(float(v[0]), float(v[1]), float(v[2]));
    return true;
}

void readAscii(const QByteArray &data, Scene *scene, SceneLayer *layer,
               int *facetsOut)
{
    int facets = 0;
    QVector3D normal;
    QVector3D corner[3];
    int corners = 0;

    int i = 0;
    const int n = data.size();
    while (i < n) {
        // One token at a time. The format is regular enough that looking only
        // for "normal" and "vertex" is both sufficient and immune to the
        // whitespace and capitalisation exporters vary in.
        while (i < n && (data.at(i) == ' ' || data.at(i) == '\t'
                         || data.at(i) == '\n' || data.at(i) == '\r'))
            ++i;
        const int start = i;
        while (i < n && data.at(i) != ' ' && data.at(i) != '\t'
               && data.at(i) != '\n' && data.at(i) != '\r')
            ++i;
        if (i == start)
            break;

        const QByteArray token = data.mid(start, i - start);
        if (token == "normal") {
            scanVector(data, &i, &normal);
            corners = 0;
        } else if (token == "vertex") {
            QVector3D v;
            if (scanVector(data, &i, &v) && corners < 3)
                corner[corners++] = v;
            if (corners == 3) {
                appendFacet(layer, scene, corner[0], corner[1], corner[2], normal);
                ++facets;
                corners = 0;
            }
        }
    }

    *facetsOut = facets;
}

} // namespace

ScenePtr StlReader::read(const QString &path,
                         QString *errorOut,
                         void (*progress)(void *, float),
                         void *progressCtx)
{
    if (errorOut)
        errorOut->clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QObject::tr("File is missing or not readable.");
        return ScenePtr();
    }
    const QByteArray data = f.readAll();
    f.close();

    if (data.size() < 15) {   // shorter than the smallest possible ASCII solid
        if (errorOut)
            *errorOut = QObject::tr("Not a readable STL file.");
        return ScenePtr();
    }

    ScenePtr scene(new Scene);
    scene->filePath = path;

    SceneLayer layer;
    layer.name = QFileInfo(path).completeBaseName();
    layer.color = kDefaultColor;
    layer.visible = true;
    layer.sourceIndex = 0;

    // --- binary or ASCII? -------------------------------------------------
    bool binary = false;
    if (data.size() >= kBinaryHeaderSize + 4) {
        const quint32 declared = quint32(uchar(data.at(80)))
                               | (quint32(uchar(data.at(81))) << 8)
                               | (quint32(uchar(data.at(82))) << 16)
                               | (quint32(uchar(data.at(83))) << 24);
        // The size test is exact and therefore decisive. Only if it fails do
        // we fall back to the keyword, which is the weaker signal.
        if (qint64(declared) * 50 + 84 == qint64(data.size()))
            binary = true;
    }
    if (!binary && !data.trimmed().startsWith("solid")) {
        // Neither the arithmetic nor the keyword fits. Treat it as binary
        // anyway if the header at least declares a plausible facet count,
        // since a truncated binary file is far more common than an ASCII one
        // that forgot to say "solid".
        binary = data.size() > kBinaryHeaderSize + 4;
    }

    int facets = 0;
    int truncated = 0;

    if (binary) {
        const quint32 declared = quint32(uchar(data.at(80)))
                               | (quint32(uchar(data.at(81))) << 8)
                               | (quint32(uchar(data.at(82))) << 16)
                               | (quint32(uchar(data.at(83))) << 24);
        const int available = (data.size() - 84) / 50;
        const int count = qMin(int(declared), qMax(0, available));
        if (int(declared) > count)
            truncated = int(declared) - count;

        scene->formatName = QStringLiteral("STL (binary)");
        layer.vertices.reserve(count * 3 * kFloatsPerVertex);
        layer.indices.reserve(count * 3);

        const char *p = data.constData() + 84;
        for (int i = 0; i < count; ++i, p += 50) {
            const QVector3D n(readFloatLE(p), readFloatLE(p + 4), readFloatLE(p + 8));
            const QVector3D a(readFloatLE(p + 12), readFloatLE(p + 16), readFloatLE(p + 20));
            const QVector3D b(readFloatLE(p + 24), readFloatLE(p + 28), readFloatLE(p + 32));
            const QVector3D c(readFloatLE(p + 36), readFloatLE(p + 40), readFloatLE(p + 44));
            // The two attribute bytes at p + 48 carry a colour in some
            // dialects, in two incompatible bit orders that cannot be told
            // apart from the file. Guessing wrong paints the model in
            // nonsense, so they are ignored.
            appendFacet(&layer, scene.data(), a, b, c, n);
            ++facets;

            if (progress && (i & 0x3FFF) == 0 && count > 0)
                progress(progressCtx, 0.9f * float(i) / float(count));
        }
    } else {
        scene->formatName = QStringLiteral("STL (ASCII)");
        readAscii(data, scene.data(), &layer, &facets);
        if (progress)
            progress(progressCtx, 0.9f);
    }

    scene->objectCount = 1;
    if (!layer.isEmpty())
        scene->layers.append(layer);

    if (truncated > 0) {
        scene->warnings << QObject::tr(
            "The file claims %1 more facet(s) than it contains and was read as "
            "far as it goes.").arg(truncated);
    }
    if (scene->isEmpty()) {
        if (errorOut)
            *errorOut = QObject::tr("Not a readable STL file.");
        return ScenePtr();
    }

    if (progress)
        progress(progressCtx, 1.0f);

    return scene;
}

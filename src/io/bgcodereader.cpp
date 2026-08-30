/*
  harbour-3dmviewer — bgcodereader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/bgcodereader.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QtGlobal>
#include <QtMath>

#include "io/bgcodecodecs.h"

// The system zlib. This used to be openNURBS' bundled 1.2.3-era copy, which
// was only on the include path by accident of the build — and whose unprefixed
// symbols were what shadowed the real one and cost a release.
#include <zlib.h>

namespace {

// --- container ------------------------------------------------------------

// Block types, as written in the block header.
enum BlockType {
    BlockFileMetadata = 0,
    BlockGCode = 1,
    BlockSlicerMetadata = 2,
    BlockPrinterMetadata = 3,
    BlockPrintMetadata = 4,
    BlockThumbnail = 5
};

enum Compression {
    CompNone = 0,
    CompDeflate = 1,
    CompHeatshrink11 = 2,
    CompHeatshrink12 = 3
};

// G-code payload encoding, from the block's parameter word.
enum GCodeEncoding {
    EncNone = 0,
    EncMeatPack = 1,
    EncMeatPackComments = 2
};

// Little-endian readers. QDataStream would do this too, but at ~2000 header
// fields the explicit version is both faster and easier to check against the
// format description.
quint16 readU16(const QByteArray &d, int off)
{
    return quint16(uchar(d.at(off))) | (quint16(uchar(d.at(off + 1))) << 8);
}

quint32 readU32(const QByteArray &d, int off)
{
    return quint32(uchar(d.at(off)))
         | (quint32(uchar(d.at(off + 1))) << 8)
         | (quint32(uchar(d.at(off + 2))) << 16)
         | (quint32(uchar(d.at(off + 3))) << 24);
}

QByteArray inflateBlock(const QByteArray &in, int expected)
{
    QByteArray out;
    out.resize(expected);
    uLongf destLen = uLongf(expected);
    const int rc = uncompress(reinterpret_cast<Bytef *>(out.data()), &destLen,
                              reinterpret_cast<const Bytef *>(in.constData()),
                              uLong(in.size()));
    if (rc != Z_OK)
        return QByteArray();
    out.resize(int(destLen));
    return out;
}

// --- G-code number scanning -----------------------------------------------

/*
  Reads a number starting at `i`, which is left on the first character that is
  not part of it.

  QByteArray::mid().toDouble() would allocate once per parameter, and a sliced
  file has about a million of them. This also has to accept the forms a slicer
  actually writes — ".0043" and "-.5" have no leading digit, and MeatPack's
  no-spaces mode leaves parameters butted straight against each other
  ("G1X10.5Y20.25E.004").
*/
bool scanNumber(const QByteArray &s, int *i, double *out)
{
    const int n = s.size();
    int p = *i;
    bool negative = false;

    if (p < n && (s.at(p) == '+' || s.at(p) == '-')) {
        negative = (s.at(p) == '-');
        ++p;
    }

    double value = 0.0;
    bool anyDigit = false;
    while (p < n && s.at(p) >= '0' && s.at(p) <= '9') {
        value = value * 10.0 + (s.at(p) - '0');
        anyDigit = true;
        ++p;
    }

    if (p < n && s.at(p) == '.') {
        ++p;
        double scale = 0.1;
        while (p < n && s.at(p) >= '0' && s.at(p) <= '9') {
            value += (s.at(p) - '0') * scale;
            scale *= 0.1;
            anyDigit = true;
            ++p;
        }
    }

    if (!anyDigit)
        return false;

    *i = p;
    *out = negative ? -value : value;
    return true;
}

// --- feature types --------------------------------------------------------

struct FeatureStyle
{
    const char *type;    // exactly what PrusaSlicer writes after ";TYPE:"
    int r, g, b;
    bool visible;
};

/*
  PrusaSlicer's own feature colours, so a model opened here looks like the
  preview it was sliced in. Internal infill and support start switched off:
  they bury the part in exactly the material the user cannot see on the print
  either, and the layer list turns them back on.
*/
const FeatureStyle kFeatureStyles[] = {
    { "External perimeter",         255, 128,  50, true  },
    { "Perimeter",                  255, 216,  77, true  },
    { "Overhang perimeter",          31,  31, 255, true  },
    { "Internal infill",            176,  48,  41, false },
    { "Solid infill",               150,  84, 204, true  },
    { "Top solid infill",           240,  64,  64, true  },
    { "Bridge infill",               77, 128, 186, true  },
    { "Gap fill",                   255, 255, 255, true  },
    { "Skirt/Brim",                   0, 135, 110, true  },
    { "Support material",             0, 255,   0, false },
    { "Support material interface",   0, 128,   0, false },
    { "Wipe tower",                 179, 227, 171, true  },
    // Start and end G-code: the purge line at the edge of the bed and the wipe.
    // Printed material, so the slicer calls it a feature, but it is not part of
    // the object and it sits 150 mm away from it. Off by default.
    { "Custom",                      94, 209, 148, false }
};
const int kFeatureStyleCount = int(sizeof(kFeatureStyles) / sizeof(kFeatureStyles[0]));

// Travel is not a feature the slicer names; we invent it, and it stays off
// unless asked for — it would otherwise draw a dense scribble over the part.
const char *kTravelName = "Travel";

// Chord tolerance when flattening a G2/G3 arc, in millimetres. 0.02 is well
// under a layer height and under what the nozzle itself resolves, so the arc
// looks round at any zoom this app can reach.
const double kArcTolerance = 0.02;
// However fine the tolerance, never emit more than this per arc: a huge radius
// with a tiny sweep must not turn into thousands of segments.
const int kMaxArcSegments = 180;

/*
  Builds the Scene while walking the G-code.

  WHY THE SLICE TABLE IS BUILT HERE AND NOT AFTERWARDS
  ----------------------------------------------------
  G-code is written bottom layer first, so geometry arrives already sorted by
  print layer. Recording each batch's index count at every layer change is
  therefore enough to turn "show layers 12 to 80" into an offset and a count in
  one draw call. Sorting it afterwards would mean touching every vertex again.
*/
class ToolpathBuilder
{
public:
    explicit ToolpathBuilder(Scene *scene)
        : m_scene(scene)
        , m_slice(0)
        , m_runBatch(-1)
        , m_runVertex(0)
        , m_haveRun(false)
    {
        m_sliceZ.append(0.0f);
    }

    void setFeature(const QString &type) { m_feature = type; m_haveRun = false; }

    void beginSlice(float z)
    {
        // Close the slice that just ended in every batch, including the ones
        // that drew nothing in it — the table has to stay parallel across
        // batches or a range would select different layers in each.
        for (int i = 0; i < m_scene->layers.size(); ++i)
            m_scene->layers[i].sliceStart.append(m_scene->layers.at(i).indices.size());
        ++m_slice;
        m_sliceZ.append(z);
        m_haveRun = false;
    }

    void setSliceZ(float z)
    {
        if (m_slice < m_sliceZ.size())
            m_sliceZ[m_slice] = z;
    }

    // One drawn move. `extruding` picks the feature batch, otherwise travel.
    void addSegment(const QVector3D &from, const QVector3D &to, bool extruding)
    {
        const int slot = batchFor(extruding ? m_feature : QString::fromLatin1(kTravelName));
        if (slot < 0)
            return;
        SceneLayer &L = m_scene->layers[slot];

        // What the object measures, and what the camera should frame, is the
        // printed part — not the purge line at the far edge of the bed, which
        // is extrusion too and would otherwise stretch the model to bed size.
        const bool bound = extruding && m_feature != QLatin1String("Custom");

        // A continuous path shares its interior points: the end of one segment
        // is the start of the next, which halves the vertex data for what is
        // overwhelmingly one long polyline per feature per layer.
        quint32 a;
        if (m_haveRun && m_runBatch == slot && nearlyEqual(m_runPoint, from)) {
            a = m_runVertex;
        } else {
            a = appendVertex(&L, from, bound);
        }
        const quint32 b = appendVertex(&L, to, bound);

        L.indices << a << b;

        m_runBatch = slot;
        m_runVertex = b;
        m_runPoint = to;
        m_haveRun = true;
    }

    void finish()
    {
        for (int i = 0; i < m_scene->layers.size(); ++i)
            m_scene->layers[i].sliceStart.append(m_scene->layers.at(i).indices.size());
        m_scene->sliceCount = m_slice + 1;
        m_sliceZ.resize(m_scene->sliceCount);
        m_scene->sliceZ = m_sliceZ;
    }

private:
    static bool nearlyEqual(const QVector3D &a, const QVector3D &b)
    {
        // A hundredth of the finest nozzle step: closer than this and the two
        // points are the same point written twice.
        return (a - b).lengthSquared() < 1e-8f;
    }

    quint32 appendVertex(SceneLayer *L, const QVector3D &p, bool bound)
    {
        const quint32 index = quint32(L->vertices.size() / kFloatsPerVertex);
        // Lines are drawn unlit, so the normal slot is never read. It still has
        // to be filled: the batch layout is shared with the surface formats and
        // the renderer binds the same three attributes for both.
        L->vertices << p.x() << p.y() << p.z()
                    << 0.0f << 0.0f << 1.0f
                    << m_r << m_g << m_b;
        // Only printed material counts towards the model's size. Travel moves
        // sweep the whole bed and park the head high above it, so counting them
        // would report a 240 mm object for a 60 mm part and frame the camera on
        // mostly empty air.
        if (bound)
            m_scene->growBounds(p);
        return index;
    }

    int batchFor(const QString &name)
    {
        const int existing = m_slot.value(name, -1);
        if (existing >= 0) {
            setColorFrom(m_scene->layers.at(existing).color);
            return existing;
        }

        SceneLayer L;
        L.name = name;
        L.primitive = PrimitiveLines;
        L.sourceIndex = m_scene->layers.size();
        L.visible = true;
        L.color = QColor(190, 190, 190);

        if (name == QLatin1String(kTravelName)) {
            L.color = QColor(90, 100, 110);
            L.visible = false;
        } else {
            for (int i = 0; i < kFeatureStyleCount; ++i) {
                if (name == QLatin1String(kFeatureStyles[i].type)) {
                    L.color = QColor(kFeatureStyles[i].r, kFeatureStyles[i].g,
                                     kFeatureStyles[i].b);
                    L.visible = kFeatureStyles[i].visible;
                    break;
                }
            }
        }

        // A batch born mid-print still needs an entry for every slice before
        // it, all of them empty, so its table lines up with the others.
        L.sliceStart = QVector<int>(m_slice + 1, 0);

        const int slot = m_scene->layers.size();
        m_scene->layers.append(L);
        m_slot.insert(name, slot);
        setColorFrom(L.color);
        return slot;
    }

    void setColorFrom(const QColor &c)
    {
        m_r = float(c.redF());
        m_g = float(c.greenF());
        m_b = float(c.blueF());
    }

    Scene *m_scene;
    QHash<QString, int> m_slot;
    QString m_feature;

    int m_slice;
    QVector<float> m_sliceZ;

    // The polyline currently being extended.
    int m_runBatch;
    quint32 m_runVertex;
    QVector3D m_runPoint;
    bool m_haveRun;

    float m_r = 1.0f, m_g = 1.0f, m_b = 1.0f;
};

} // namespace

ScenePtr BgCodeReader::read(const QString &path,
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

    // magic(4) + version(4) + checksum type(2)
    if (data.size() < 10 || data.left(4) != QByteArray("GCDE")) {
        if (errorOut)
            *errorOut = QObject::tr("Not a binary G-code file.");
        return ScenePtr();
    }
    const quint32 version = readU32(data, 4);
    const quint16 checksumType = readU16(data, 8);

    ScenePtr scene(new Scene);
    scene->filePath = path;
    scene->formatName = QStringLiteral("Binary G-code");

    // --- walk the blocks --------------------------------------------------
    QByteArray gcode;
    // A .bgcode of this size holds roughly 7 MB of G-code; reserving it up
    // front avoids a hundred reallocations of a growing multi-megabyte buffer.
    gcode.reserve(data.size() * 3);

    int thumbnails = 0;
    int badBlocks = 0;
    bool unknownEncoding = false;

    int off = 10;
    while (off + 8 <= data.size()) {
        const quint16 type = readU16(data, off);
        const quint16 compression = readU16(data, off + 2);
        const quint32 uncompressedSize = readU32(data, off + 4);
        int p = off + 8;

        quint32 compressedSize = 0;
        if (compression != CompNone) {
            if (p + 4 > data.size())
                break;
            compressedSize = readU32(data, p);
            p += 4;
        }

        // Parameters: a thumbnail carries format and pixel size, everything
        // else a single encoding word.
        quint16 encoding = 0;
        if (type == BlockThumbnail) {
            if (p + 6 > data.size())
                break;
            p += 6;
            ++thumbnails;
        } else {
            if (p + 2 > data.size())
                break;
            encoding = readU16(data, p);
            p += 2;
        }

        const int payloadSize = int(compression != CompNone ? compressedSize
                                                            : uncompressedSize);
        if (payloadSize < 0 || p + payloadSize > data.size())
            break;

        if (type == BlockGCode || type == BlockFileMetadata) {
            QByteArray payload = data.mid(p, payloadSize);

            switch (compression) {
            case CompNone:
                break;
            case CompDeflate:
                payload = inflateBlock(payload, int(uncompressedSize));
                break;
            case CompHeatshrink11:
                payload = BgCodeCodecs::heatshrinkDecode(payload, 11, 4,
                                                         int(uncompressedSize));
                break;
            case CompHeatshrink12:
                payload = BgCodeCodecs::heatshrinkDecode(payload, 12, 4,
                                                         int(uncompressedSize));
                break;
            default:
                payload.clear();
                break;
            }

            if (payload.size() != int(uncompressedSize) && !payload.isEmpty())
                ++badBlocks;

            if (type == BlockGCode) {
                if (encoding == EncMeatPack || encoding == EncMeatPackComments)
                    payload = BgCodeCodecs::meatPackDecode(payload);
                else if (encoding != EncNone)
                    unknownEncoding = true;
                gcode.append(payload);
            } else {
                // FileMetadata is "Key=Value" per line; Producer is the only
                // field worth showing.
                const QList<QByteArray> lines = payload.split('\n');
                for (int i = 0; i < lines.size(); ++i) {
                    const QByteArray &line = lines.at(i);
                    if (line.startsWith("Producer=")) {
                        scene->producer =
                            QString::fromUtf8(line.mid(9)).trimmed();
                        break;
                    }
                }
            }
        }

        // + CRC32, when the header said the file carries one.
        off = p + payloadSize + (checksumType != 0 ? 4 : 0);

        if (progress && data.size() > 0)
            progress(progressCtx, 0.55f * float(off) / float(data.size()));
    }

    if (gcode.isEmpty()) {
        if (errorOut) {
            *errorOut = version > 1
                ? QObject::tr("This is binary G-code version %1, which this "
                              "version of the app cannot read.").arg(version)
                : QObject::tr("The file contains no readable G-code.");
        }
        return ScenePtr();
    }

    // --- interpret the G-code ---------------------------------------------
    ToolpathBuilder builder(scene.data());

    QVector3D pos(0.0f, 0.0f, 0.0f);
    double extruded = 0.0;
    bool absolutePositions = true;
    bool absoluteExtrusion = true;
    int arcsWithoutCentre = 0;
    int moves = 0;

    const int n = gcode.size();
    int i = 0;
    int lastProgressAt = 0;

    while (i < n) {
        // One line at a time; `i` ends up on the character after the newline.
        const int lineStart = i;
        int lineEnd = gcode.indexOf('\n', i);
        if (lineEnd < 0)
            lineEnd = n;
        i = lineEnd + 1;

        if (progress && lineStart - lastProgressAt > 262144) {
            lastProgressAt = lineStart;
            progress(progressCtx, 0.55f + 0.4f * float(lineStart) / float(n));
        }

        if (lineEnd == lineStart)
            continue;

        // --- comments: the slicer's own annotations are the only structure a
        // G-code file has, and both things we need are in them.
        if (gcode.at(lineStart) == ';') {
            const QByteArray comment = gcode.mid(lineStart, lineEnd - lineStart);
            if (comment.startsWith(";TYPE:")) {
                builder.setFeature(QString::fromUtf8(comment.mid(6)).trimmed());
            } else if (comment.startsWith(";LAYER_CHANGE")) {
                builder.beginSlice(pos.z());
            } else if (comment.startsWith(";Z:")) {
                int k = 3;
                double z = 0.0;
                if (scanNumber(comment, &k, &z))
                    builder.setSliceZ(float(z));
            }
            continue;
        }

        if (gcode.at(lineStart) != 'G' && gcode.at(lineStart) != 'M')
            continue;

        int k = lineStart + 1;
        double codeValue = 0.0;
        if (!scanNumber(gcode, &k, &codeValue))
            continue;
        const bool isG = (gcode.at(lineStart) == 'G');
        const int code = int(codeValue);

        if (!isG) {
            // M82/M83 decide whether E is a running total or a per-move
            // amount. Reading E the wrong way turns every travel move into an
            // extrusion and floods the model with lines that were never
            // printed.
            if (code == 82)
                absoluteExtrusion = true;
            else if (code == 83)
                absoluteExtrusion = false;
            continue;
        }

        if (code == 90) { absolutePositions = true; continue; }
        if (code == 91) { absolutePositions = false; continue; }

        if (code == 92) {
            // Sets the current position without moving. Used constantly to
            // reset E, and if ignored the next move looks like a huge retract.
            double v = 0.0;
            while (k < lineEnd) {
                const char c = gcode.at(k);
                if (c == 'X' || c == 'Y' || c == 'Z' || c == 'E') {
                    ++k;
                    if (!scanNumber(gcode, &k, &v))
                        break;
                    if (c == 'X') pos.setX(float(v));
                    else if (c == 'Y') pos.setY(float(v));
                    else if (c == 'Z') pos.setZ(float(v));
                    else extruded = v;
                } else {
                    ++k;
                }
            }
            continue;
        }

        const bool isLinear = (code == 0 || code == 1);
        const bool isArc = (code == 2 || code == 3);
        if (!isLinear && !isArc)
            continue;

        // --- parameters ----------------------------------------------------
        double x = pos.x(), y = pos.y(), z = pos.z();
        double e = extruded;
        double iOff = 0.0, jOff = 0.0;
        bool haveE = false, haveI = false, haveJ = false;

        while (k < lineEnd) {
            const char c = gcode.at(k);
            if (c == ';')
                break;
            if (c >= 'A' && c <= 'Z') {
                ++k;
                double v = 0.0;
                if (!scanNumber(gcode, &k, &v))
                    continue;
                switch (c) {
                case 'X': x = absolutePositions ? v : pos.x() + v; break;
                case 'Y': y = absolutePositions ? v : pos.y() + v; break;
                case 'Z': z = absolutePositions ? v : pos.z() + v; break;
                case 'E':
                    e = absoluteExtrusion ? v : extruded + v;
                    haveE = true;
                    break;
                case 'I': iOff = v; haveI = true; break;
                case 'J': jOff = v; haveJ = true; break;
                default: break;   // F, S, P and friends do not move anything
                }
            } else {
                ++k;
            }
        }

        const QVector3D from = pos;
        // Braces, not parentheses: with float(x) inside, the parenthesised form
        // is a function declaration as far as the language is concerned.
        const QVector3D to{float(x), float(y), float(z)};
        // Pushing plastic, not just moving. A retraction has E going backwards
        // and must not draw anything.
        const bool extruding = haveE && (e - extruded) > 1e-9;

        if (isArc) {
            if (!haveI && !haveJ) {
                // The R form gives a radius instead of a centre. PrusaSlicer
                // does not write it; if some other slicer does, say so rather
                // than draw a straight line through what should be a curve.
                ++arcsWithoutCentre;
            } else {
                const double cx = from.x() + iOff;
                const double cy = from.y() + jOff;
                const double r = qSqrt((from.x() - cx) * (from.x() - cx)
                                       + (from.y() - cy) * (from.y() - cy));
                if (r > 1e-9) {
                    double a0 = qAtan2(from.y() - cy, from.x() - cx);
                    double a1 = qAtan2(y - cy, x - cx);
                    // G2 turns clockwise, G3 counter-clockwise. A sweep that
                    // comes out as zero is a full circle, not a no-op.
                    double sweep = (code == 2) ? (a0 - a1) : (a1 - a0);
                    while (sweep <= 0.0)
                        sweep += 2.0 * M_PI;

                    // Chord tolerance -> maximum angle per segment. acos is
                    // undefined once the tolerance exceeds the radius, which
                    // happens on arcs finer than the tolerance itself.
                    double stepAngle = M_PI;
                    if (r > kArcTolerance)
                        stepAngle = 2.0 * qAcos(1.0 - kArcTolerance / r);
                    int steps = int(qCeil(sweep / qMax(stepAngle, 1e-6)));
                    steps = qBound(1, steps, kMaxArcSegments);

                    QVector3D prev = from;
                    for (int s = 1; s <= steps; ++s) {
                        const double t = double(s) / double(steps);
                        const double a = (code == 2) ? (a0 - sweep * t)
                                                     : (a0 + sweep * t);
                        const QVector3D p(float(cx + r * qCos(a)),
                                          float(cy + r * qSin(a)),
                                          float(from.z() + (z - from.z()) * t));
                        builder.addSegment(prev, p, extruding);
                        prev = p;
                    }
                    ++moves;
                }
            }
        } else if (from != to) {
            builder.addSegment(from, to, extruding);
            ++moves;
        }

        pos = to;
        if (haveE)
            extruded = e;
    }

    builder.finish();
    scene->objectCount = moves;

    // --- report -----------------------------------------------------------
    if (arcsWithoutCentre > 0) {
        scene->warnings << QObject::tr(
            "%1 arc move(s) use a radius instead of a centre and were skipped.")
            .arg(arcsWithoutCentre);
    }
    if (unknownEncoding) {
        scene->warnings << QObject::tr(
            "Part of the G-code uses an encoding this app does not know and "
            "was skipped.");
    }
    if (badBlocks > 0) {
        scene->warnings << QObject::tr(
            "%1 block(s) did not decompress to their stated size; the toolpath "
            "may be incomplete.").arg(badBlocks);
    }
    if (scene->isEmpty()) {
        scene->warnings << QObject::tr(
            "The G-code was read but contains no extrusion moves.");
    }
    if (thumbnails > 0 && scene->producer.isEmpty())
        scene->producer = QStringLiteral("PrusaSlicer");

    if (progress)
        progress(progressCtx, 1.0f);

    return scene;
}

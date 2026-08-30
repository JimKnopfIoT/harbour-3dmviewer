/*
  harbour-3dmviewer — threemfreader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/threemfreader.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMatrix4x4>
#include <QObject>
#include <QSet>
#include <QVector>
#include <QXmlStreamReader>
#include <QtGlobal>

// The system zlib; see the note in bgcodereader.cpp.
#include <zlib.h>

namespace {

// --- the ZIP archive ------------------------------------------------------

const quint32 kEndOfCentralDir = 0x06054b50;
const quint32 kCentralFileHeader = 0x02014b50;
const quint32 kLocalFileHeader = 0x04034b50;

quint16 u16(const QByteArray &d, int off)
{
    return quint16(uchar(d.at(off))) | (quint16(uchar(d.at(off + 1))) << 8);
}

quint32 u32(const QByteArray &d, int off)
{
    return quint32(uchar(d.at(off)))
         | (quint32(uchar(d.at(off + 1))) << 8)
         | (quint32(uchar(d.at(off + 2))) << 16)
         | (quint32(uchar(d.at(off + 3))) << 24);
}

/*
  Inflates a raw deflate stream — no zlib or gzip wrapper, which is what a ZIP
  member is. That is what the negative window size means to inflateInit2.

  `expected` comes from the ZIP header; it is used as the output size rather
  than trusted blindly, so a header that lies produces a short result the
  caller notices instead of an overrun.
*/
QByteArray inflateRaw(const QByteArray &in, int expected)
{
    QByteArray out;
    if (expected <= 0)
        return out;
    out.resize(expected);

    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
        return QByteArray();

    s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    s.avail_in = uInt(in.size());
    s.next_out = reinterpret_cast<Bytef *>(out.data());
    s.avail_out = uInt(expected);

    const int rc = inflate(&s, Z_FINISH);
    const int produced = int(s.total_out);
    inflateEnd(&s);

    if (rc != Z_STREAM_END && rc != Z_OK)
        return QByteArray();
    out.resize(produced);
    return out;
}

// Pulls one member out of the archive by name. Returns an empty array if the
// entry is missing or cannot be decompressed.
QByteArray zipExtract(const QByteArray &zip, const QString &wanted, bool *zip64)
{
    *zip64 = false;
    if (zip.size() < 22)
        return QByteArray();

    // The end-of-central-directory record sits at the very end, unless there
    // is an archive comment — so scan back over the largest comment possible.
    int eocd = -1;
    const int searchFrom = qMax(0, zip.size() - (22 + 65535));
    for (int i = zip.size() - 22; i >= searchFrom; --i) {
        if (u32(zip, i) == kEndOfCentralDir) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return QByteArray();

    const int entryCount = int(u16(zip, eocd + 10));
    int p = int(u32(zip, eocd + 16));   // offset of the central directory

    for (int e = 0; e < entryCount; ++e) {
        if (p + 46 > zip.size() || u32(zip, p) != kCentralFileHeader)
            break;

        const quint16 method = u16(zip, p + 10);
        const quint32 compressedSize = u32(zip, p + 20);
        const quint32 uncompressedSize = u32(zip, p + 24);
        const int nameLen = int(u16(zip, p + 28));
        const int extraLen = int(u16(zip, p + 30));
        const int commentLen = int(u16(zip, p + 32));
        const quint32 localOffset = u32(zip, p + 42);

        const QString name = QString::fromUtf8(zip.mid(p + 46, nameLen));

        if (name.compare(wanted, Qt::CaseInsensitive) == 0) {
            // A 0xFFFFFFFF field means the real value lives in a ZIP64 extra
            // field. Slicer output is far below 4 GB, so rather than implement
            // ZIP64 we say plainly that we cannot.
            if (compressedSize == 0xFFFFFFFFu || uncompressedSize == 0xFFFFFFFFu
                || localOffset == 0xFFFFFFFFu) {
                *zip64 = true;
                return QByteArray();
            }

            const int lp = int(localOffset);
            if (lp + 30 > zip.size() || u32(zip, lp) != kLocalFileHeader)
                return QByteArray();
            // The local header repeats the name and extra lengths, and its
            // extra field is NOT required to match the central one — reading
            // the central directory's length here is a classic way to land in
            // the middle of the data.
            const int localNameLen = int(u16(zip, lp + 26));
            const int localExtraLen = int(u16(zip, lp + 28));
            const int dataAt = lp + 30 + localNameLen + localExtraLen;
            if (dataAt + int(compressedSize) > zip.size())
                return QByteArray();

            const QByteArray payload = zip.mid(dataAt, int(compressedSize));
            if (method == 0)
                return payload;                       // stored
            if (method == 8)
                return inflateRaw(payload, int(uncompressedSize));
            return QByteArray();                      // bzip2, lzma, …
        }

        p += 46 + nameLen + extraLen + commentLen;
    }

    return QByteArray();
}

// --- the model document ---------------------------------------------------

struct MfMesh
{
    QVector<QVector3D> vertices;
    QVector<quint32> indices;      // triangle list into `vertices`
    QVector<int> triangleColor;    // one entry per triangle, -1 = none
};

struct MfComponent
{
    QString objectId;
    QMatrix4x4 transform;
};

struct MfObject
{
    QString id;
    QString name;
    MfMesh mesh;
    QVector<MfComponent> components;
    int colorIndex = -1;           // from the object's own pid/pindex
};

/*
  A 3MF transform is twelve numbers: three rows of a 3x3 basis followed by the
  translation. Written row by row, which is the opposite of what QMatrix4x4's
  constructor wants, so the values are placed one at a time rather than
  handed over as a block.
*/
QMatrix4x4 parseTransform(const QString &s, bool *ok)
{
    QMatrix4x4 m;   // identity
    const QVector<QStringRef> parts = s.splitRef(QLatin1Char(' '), QString::SkipEmptyParts);
    if (parts.size() != 12) {
        *ok = false;
        return m;
    }
    double v[12];
    for (int i = 0; i < 12; ++i) {
        bool good = false;
        v[i] = parts.at(i).toDouble(&good);
        if (!good) {
            *ok = false;
            return m;
        }
    }
    m.setRow(0, QVector4D(float(v[0]), float(v[3]), float(v[6]), float(v[9])));
    m.setRow(1, QVector4D(float(v[1]), float(v[4]), float(v[7]), float(v[10])));
    m.setRow(2, QVector4D(float(v[2]), float(v[5]), float(v[8]), float(v[11])));
    m.setRow(3, QVector4D(0.0f, 0.0f, 0.0f, 1.0f));
    *ok = true;
    return m;
}

QColor parseDisplayColor(const QString &s)
{
    // "#RRGGBB" or "#RRGGBBAA". Alpha is parsed and dropped: the renderer has
    // one global alpha for the ghosted mode and no per-vertex one.
    QString hex = s;
    if (hex.startsWith(QLatin1Char('#')))
        hex.remove(0, 1);
    if (hex.size() < 6)
        return QColor();
    bool ok = false;
    const uint rgb = hex.left(6).toUInt(&ok, 16);
    if (!ok)
        return QColor();
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

class ModelDocument
{
public:
    QHash<QString, MfObject> objects;
    QVector<MfComponent> build;        // build items, objectId + transform
    QVector<QColor> colors;            // flattened palette
    QHash<QString, int> colorGroupBase; // resource id -> first index in colors
    QString unit;
    int skippedBeamLattices = 0;
    int skippedTriangles = 0;

    bool parse(const QByteArray &xml, QString *errorOut);

private:
    void readObject(QXmlStreamReader *r);
    void readMesh(QXmlStreamReader *r, MfObject *obj);
    void readBaseMaterials(QXmlStreamReader *r);
    void readColorGroup(QXmlStreamReader *r);
    void readBuild(QXmlStreamReader *r);
};

bool ModelDocument::parse(const QByteArray &xml, QString *errorOut)
{
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement())
            continue;

        const QStringRef name = r.name();
        if (name == QLatin1String("model")) {
            unit = r.attributes().value(QLatin1String("unit")).toString();
        } else if (name == QLatin1String("object")) {
            readObject(&r);
        } else if (name == QLatin1String("basematerials")) {
            readBaseMaterials(&r);
        } else if (name == QLatin1String("colorgroup")) {
            readColorGroup(&r);
        } else if (name == QLatin1String("build")) {
            readBuild(&r);
        }
    }

    if (r.hasError()) {
        if (errorOut)
            *errorOut = r.errorString();
        return false;
    }
    return true;
}

void ModelDocument::readObject(QXmlStreamReader *r)
{
    MfObject obj;
    const QXmlStreamAttributes a = r->attributes();
    obj.id = a.value(QLatin1String("id")).toString();
    obj.name = a.value(QLatin1String("name")).toString();

    const QString pid = a.value(QLatin1String("pid")).toString();
    const QString pindex = a.value(QLatin1String("pindex")).toString();
    if (!pid.isEmpty() && !pindex.isEmpty()) {
        const int base = colorGroupBase.value(pid, -1);
        if (base >= 0)
            obj.colorIndex = base + pindex.toInt();
    }

    while (!r->atEnd()) {
        r->readNext();
        if (r->isEndElement() && r->name() == QLatin1String("object"))
            break;
        if (!r->isStartElement())
            continue;

        if (r->name() == QLatin1String("mesh")) {
            readMesh(r, &obj);
        } else if (r->name() == QLatin1String("component")) {
            MfComponent c;
            c.objectId = r->attributes().value(QLatin1String("objectid")).toString();
            const QString t = r->attributes().value(QLatin1String("transform")).toString();
            if (!t.isEmpty()) {
                bool ok = false;
                c.transform = parseTransform(t, &ok);
            }
            obj.components.append(c);
        }
    }

    if (!obj.id.isEmpty())
        objects.insert(obj.id, obj);
}

void ModelDocument::readMesh(QXmlStreamReader *r, MfObject *obj)
{
    while (!r->atEnd()) {
        r->readNext();
        if (r->isEndElement() && r->name() == QLatin1String("mesh"))
            break;
        if (!r->isStartElement())
            continue;

        if (r->name() == QLatin1String("vertex")) {
            const QXmlStreamAttributes a = r->attributes();
            obj->mesh.vertices.append(
                QVector3D(a.value(QLatin1String("x")).toFloat(),
                          a.value(QLatin1String("y")).toFloat(),
                          a.value(QLatin1String("z")).toFloat()));
        } else if (r->name() == QLatin1String("triangle")) {
            const QXmlStreamAttributes a = r->attributes();
            bool ok1 = false, ok2 = false, ok3 = false;
            const int v1 = a.value(QLatin1String("v1")).toInt(&ok1);
            const int v2 = a.value(QLatin1String("v2")).toInt(&ok2);
            const int v3 = a.value(QLatin1String("v3")).toInt(&ok3);
            if (!ok1 || !ok2 || !ok3) {
                ++skippedTriangles;
                continue;
            }
            obj->mesh.indices << quint32(v1) << quint32(v2) << quint32(v3);

            int color = -1;
            const QString pid = a.value(QLatin1String("pid")).toString();
            const QString p1 = a.value(QLatin1String("p1")).toString();
            if (!pid.isEmpty() && !p1.isEmpty()) {
                const int base = colorGroupBase.value(pid, -1);
                if (base >= 0)
                    color = base + p1.toInt();
            }
            obj->mesh.triangleColor.append(color);
        } else if (r->name() == QLatin1String("beamlattice")) {
            ++skippedBeamLattices;
        }
    }
}

void ModelDocument::readBaseMaterials(QXmlStreamReader *r)
{
    const QString id = r->attributes().value(QLatin1String("id")).toString();
    if (!id.isEmpty())
        colorGroupBase.insert(id, colors.size());

    while (!r->atEnd()) {
        r->readNext();
        if (r->isEndElement() && r->name() == QLatin1String("basematerials"))
            break;
        if (r->isStartElement() && r->name() == QLatin1String("base")) {
            const QColor c = parseDisplayColor(
                r->attributes().value(QLatin1String("displaycolor")).toString());
            colors.append(c.isValid() ? c : QColor(200, 200, 200));
        }
    }
}

void ModelDocument::readColorGroup(QXmlStreamReader *r)
{
    const QString id = r->attributes().value(QLatin1String("id")).toString();
    if (!id.isEmpty())
        colorGroupBase.insert(id, colors.size());

    while (!r->atEnd()) {
        r->readNext();
        if (r->isEndElement() && r->name() == QLatin1String("colorgroup"))
            break;
        if (r->isStartElement() && r->name() == QLatin1String("color")) {
            const QColor c = parseDisplayColor(
                r->attributes().value(QLatin1String("color")).toString());
            colors.append(c.isValid() ? c : QColor(200, 200, 200));
        }
    }
}

void ModelDocument::readBuild(QXmlStreamReader *r)
{
    while (!r->atEnd()) {
        r->readNext();
        if (r->isEndElement() && r->name() == QLatin1String("build"))
            break;
        if (r->isStartElement() && r->name() == QLatin1String("item")) {
            MfComponent it;
            it.objectId = r->attributes().value(QLatin1String("objectid")).toString();
            const QString t = r->attributes().value(QLatin1String("transform")).toString();
            if (!t.isEmpty()) {
                bool ok = false;
                it.transform = parseTransform(t, &ok);
            }
            build.append(it);
        }
    }
}

// --- flattening -----------------------------------------------------------

/*
  Walks one build item, following components, and appends every mesh it finds
  into `layer` with the accumulated transform baked in.

  `visiting` is not an optimisation: the format does not forbid an object from
  referencing itself through a chain of components, and without the guard such
  a file would recurse until the stack ran out.
*/
void emitObject(const ModelDocument &doc,
                const QString &objectId,
                const QMatrix4x4 &transform,
                SceneLayer *layer,
                Scene *scene,
                QSet<QString> *visiting,
                int *cycles)
{
    if (visiting->contains(objectId)) {
        ++(*cycles);
        return;
    }
    const QHash<QString, MfObject>::const_iterator it = doc.objects.constFind(objectId);
    if (it == doc.objects.constEnd())
        return;
    const MfObject &obj = it.value();

    visiting->insert(objectId);

    const MfMesh &mesh = obj.mesh;
    if (!mesh.indices.isEmpty() && !mesh.vertices.isEmpty()) {
        const int nv = mesh.vertices.size();
        const quint32 base = quint32(layer->vertices.size() / kFloatsPerVertex);

        // Transformed positions first: the normals below are computed from the
        // transformed triangles, which is what keeps a mirrored or non-uniform
        // instance correctly lit instead of inside out.
        QVector<QVector3D> pos(nv);
        for (int i = 0; i < nv; ++i) {
            pos[i] = transform * mesh.vertices.at(i);
            scene->growBounds(pos[i]);
        }

        QVector<QVector3D> normals(nv);   // zero-initialised by QVector
        for (int t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const quint32 a = mesh.indices.at(t);
            const quint32 b = mesh.indices.at(t + 1);
            const quint32 c = mesh.indices.at(t + 2);
            if (int(a) >= nv || int(b) >= nv || int(c) >= nv)
                continue;
            const QVector3D n = QVector3D::crossProduct(pos[b] - pos[a],
                                                        pos[c] - pos[a]);
            // Un-normalised on purpose: its length is twice the triangle area,
            // which is exactly the weighting that gives a smooth result on an
            // uneven triangulation.
            normals[a] += n;
            normals[b] += n;
            normals[c] += n;
            layer->indices << base + a << base + b << base + c;
        }

        QColor color = layer->color;
        if (obj.colorIndex >= 0 && obj.colorIndex < doc.colors.size())
            color = doc.colors.at(obj.colorIndex);
        const float r = float(color.redF());
        const float g = float(color.greenF());
        const float bl = float(color.blueF());

        for (int i = 0; i < nv; ++i) {
            QVector3D n = normals.at(i);
            if (n.lengthSquared() < 1e-20f)
                n = QVector3D(0.0f, 0.0f, 1.0f);
            else
                n.normalize();
            layer->vertices << pos[i].x() << pos[i].y() << pos[i].z()
                            << n.x() << n.y() << n.z()
                            << r << g << bl;
        }
    }

    for (int i = 0; i < obj.components.size(); ++i) {
        const MfComponent &c = obj.components.at(i);
        emitObject(doc, c.objectId, transform * c.transform,
                   layer, scene, visiting, cycles);
    }

    visiting->remove(objectId);
}

} // namespace

ScenePtr ThreeMfReader::read(const QString &path,
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
    const QByteArray zip = f.readAll();
    f.close();

    if (progress)
        progress(progressCtx, 0.15f);

    bool zip64 = false;
    QByteArray xml = zipExtract(zip, QStringLiteral("3D/3dmodel.model"), &zip64);
    if (xml.isEmpty() && !zip64) {
        // Some writers name the part differently. The proper route is to read
        // _rels/.rels and follow the relationship, which is worth doing only
        // if a real file ever needs it; the two spellings below cover what is
        // actually out there.
        xml = zipExtract(zip, QStringLiteral("3d/3dmodel.model"), &zip64);
    }
    if (xml.isEmpty()) {
        if (errorOut) {
            *errorOut = zip64
                ? QObject::tr("This 3MF uses ZIP64, which this app cannot read.")
                : QObject::tr("Not a readable 3MF file.");
        }
        return ScenePtr();
    }

    if (progress)
        progress(progressCtx, 0.4f);

    ModelDocument doc;
    QString xmlError;
    if (!doc.parse(xml, &xmlError)) {
        if (errorOut)
            *errorOut = QObject::tr("The 3MF model is malformed: %1").arg(xmlError);
        return ScenePtr();
    }

    if (progress)
        progress(progressCtx, 0.7f);

    ScenePtr scene(new Scene);
    scene->filePath = path;
    scene->formatName = QStringLiteral("3MF");

    // The build section is what a 3MF actually places. An object nobody builds
    // is a resource, not part of the model, and drawing it would show parts
    // the file deliberately left out.
    QVector<MfComponent> items = doc.build;
    if (items.isEmpty()) {
        // A file with no build section is out of spec but does turn up. Fall
        // back to every top-level object, so the user sees something.
        for (QHash<QString, MfObject>::const_iterator it = doc.objects.constBegin();
             it != doc.objects.constEnd(); ++it) {
            MfComponent c;
            c.objectId = it.key();
            items.append(c);
        }
        scene->warnings << QObject::tr(
            "The file has no build section; every object in it is shown.");
    }

    int cycles = 0;
    static const QColor kPalette[] = {
        QColor(198, 198, 192), QColor(140, 175, 205), QColor(205, 170, 130),
        QColor(160, 200, 160), QColor(200, 155, 190), QColor(190, 190, 140)
    };
    const int paletteSize = int(sizeof(kPalette) / sizeof(kPalette[0]));

    for (int i = 0; i < items.size(); ++i) {
        const MfComponent &item = items.at(i);
        const QHash<QString, MfObject>::const_iterator o =
            doc.objects.constFind(item.objectId);

        SceneLayer layer;
        layer.name = (o != doc.objects.constEnd() && !o->name.isEmpty())
                         ? o->name
                         : QObject::tr("Object %1").arg(i + 1);
        layer.color = kPalette[i % paletteSize];
        layer.visible = true;
        layer.sourceIndex = i;

        QSet<QString> visiting;
        emitObject(doc, item.objectId, item.transform, &layer, scene.data(),
                   &visiting, &cycles);

        if (!layer.isEmpty()) {
            scene->layers.append(layer);
            scene->objectCount++;
        }

        if (progress && !items.isEmpty())
            progress(progressCtx, 0.7f + 0.3f * float(i + 1) / float(items.size()));
    }

    // 3MF measures in millimetres unless it says otherwise, and every other
    // unit would show up as a model a thousand times too big or too small.
    if (!doc.unit.isEmpty() && doc.unit != QLatin1String("millimeter")) {
        scene->warnings << QObject::tr(
            "The file is measured in %1; sizes are shown in that unit.")
            .arg(doc.unit);
    }
    if (doc.skippedBeamLattices > 0) {
        scene->warnings << QObject::tr(
            "%1 beam lattice(s) are not supported and were skipped.")
            .arg(doc.skippedBeamLattices);
    }
    if (doc.skippedTriangles > 0) {
        scene->warnings << QObject::tr("%1 malformed triangle(s) were skipped.")
            .arg(doc.skippedTriangles);
    }
    if (cycles > 0) {
        scene->warnings << QObject::tr(
            "%1 component reference(s) form a loop and were not followed.")
            .arg(cycles);
    }
    if (scene->isEmpty()) {
        if (errorOut)
            *errorOut = QObject::tr("The 3MF file contains no drawable mesh.");
        return ScenePtr();
    }

    if (progress)
        progress(progressCtx, 1.0f);

    return scene;
}

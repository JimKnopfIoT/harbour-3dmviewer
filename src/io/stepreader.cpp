/*
  harbour-3dmviewer — stepreader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/stepreader.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QtGlobal>
#include <QtMath>

#include <algorithm>

#include "io/stepparser.h"

namespace {

// Sampling tolerances, in model units. Scaled to the model in read() so a
// watch part and a bridge girder both come out reasonably.
struct Tolerances
{
    double chord = 0.02;     // largest allowed sag between a curve and its chord
    double weld = 1e-5;      // two points closer than this are the same point
};

const int kMinArcSegments = 6;
const int kMaxArcSegments = 96;
const int kMaxSplineDepth = 6;   // 2^6 = 64 segments at most per spline

// ---------------------------------------------------------------- geometry --

struct Placement
{
    QVector3D origin;
    QVector3D x = QVector3D(1, 0, 0);
    QVector3D y = QVector3D(0, 1, 0);
    QVector3D z = QVector3D(0, 0, 1);
};

/*
  A surface, reduced to the two things the tessellator needs: where a
  parameter pair lands in space, and where a point in space lands in
  parameters. Both are exact for the types handled here — no iteration, no
  closest-point search.
*/
struct Surface
{
    enum Type { None, Plane, Cylinder };

    Type type = None;
    Placement p;
    double radius = 0.0;

    QVector3D eval(double u, double v) const
    {
        if (type == Cylinder) {
            return p.origin
                 + float(radius * qCos(u)) * p.x
                 + float(radius * qSin(u)) * p.y
                 + float(v) * p.z;
        }
        return p.origin + float(u) * p.x + float(v) * p.y;
    }

    void invert(const QVector3D &point, double *u, double *v) const
    {
        const QVector3D d = point - p.origin;
        if (type == Cylinder) {
            *v = double(QVector3D::dotProduct(d, p.z));
            const QVector3D w = d - float(*v) * p.z;
            *u = qAtan2(double(QVector3D::dotProduct(w, p.y)),
                        double(QVector3D::dotProduct(w, p.x)));
            return;
        }
        *u = double(QVector3D::dotProduct(d, p.x));
        *v = double(QVector3D::dotProduct(d, p.y));
    }

    QVector3D normalAt(double u, double /*v*/) const
    {
        if (type == Cylinder)
            return (float(qCos(u)) * p.x + float(qSin(u)) * p.y).normalized();
        return p.z;
    }

    /*
      How much one unit of u is worth in millimetres. On a cylinder u is an
      angle, so without this the parameter rectangle would be a few radians
      wide and tens of millimetres tall — and the ear clipper's idea of a
      "thin" triangle, which is a geometric judgement, would be nonsense.
    */
    double uScale() const { return type == Cylinder ? qMax(radius, 1e-9) : 1.0; }
    bool periodicU() const { return type == Cylinder; }
};

// ------------------------------------------------------------ B-spline eval --

/*
  A NURBS curve, evaluated with de Boor's algorithm.

  openNURBS is linked into this app and can of course evaluate a NURBS. It is
  deliberately not used here: this reader would then only build where openNURBS
  builds, and the whole STEP path could not be compiled and tested on the
  development machine in two seconds. De Boor is forty lines.
*/
struct NurbsCurve
{
    int degree = 1;
    QVector<QVector3D> cv;
    QVector<double> weight;     // empty when the curve is not rational
    QVector<double> knot;       // full vector, cv.size() + degree + 1 entries

    bool isValid() const
    {
        return degree >= 1 && cv.size() > degree
            && knot.size() == cv.size() + degree + 1;
    }

    double t0() const { return knot.at(degree); }
    double t1() const { return knot.at(cv.size()); }

    QVector3D eval(double t) const
    {
        const int n = cv.size();
        t = qBound(t0(), t, t1());

        // The span containing t: the last k with knot[k] <= t < knot[k+1],
        // clamped so that t exactly at the end still lands in a real span.
        int k = degree;
        while (k < n - 1 && t >= knot.at(k + 1))
            ++k;

        const bool rational = (weight.size() == n);

        // Working copies of the degree+1 control points that influence t,
        // in homogeneous form so a rational curve needs no separate path.
        QVector<QVector3D> d(degree + 1);
        QVector<double> w(degree + 1);
        for (int j = 0; j <= degree; ++j) {
            const int idx = qBound(0, k - degree + j, n - 1);
            const double wj = rational ? weight.at(idx) : 1.0;
            d[j] = cv.at(idx) * float(wj);
            w[j] = wj;
        }

        for (int r = 1; r <= degree; ++r) {
            for (int j = degree; j >= r; --j) {
                const int i = k - degree + j;
                const double lo = knot.at(i);
                const double hi = knot.at(i + degree - r + 1);
                const double denom = hi - lo;
                const double a = denom > 1e-12 ? (t - lo) / denom : 0.0;
                d[j] = d[j - 1] * float(1.0 - a) + d[j] * float(a);
                w[j] = w[j - 1] * (1.0 - a) + w[j] * a;
            }
        }

        if (qAbs(w[degree]) < 1e-12)
            return d[degree];
        return d[degree] / float(w[degree]);
    }
};

// ------------------------------------------------------------- the reader ---

class Builder
{
public:
    Builder(const StepFile &file, Tolerances tol) : m_file(file), m_tol(tol) {}

    // Counters the caller turns into warnings.
    int unsupportedSurfaces = 0;
    int unsupportedCurves = 0;
    int failedFaces = 0;
    int degenerateLoops = 0;
    QSet<QString> unsupportedSurfaceNames;

    bool buildFace(int faceId, SceneLayer *layer, Scene *scene);

private:
    // --- record helpers ---
    const StepRecord *rec(int id) const { return m_file.record(id); }
    int refArg(const StepRecord *r, int i) const
    {
        return (r && i < r->args.size() && r->args.at(i).isRef())
                   ? r->args.at(i).ref : 0;
    }
    double numArg(const StepRecord *r, int i) const
    {
        return (r && i < r->args.size()
                && r->args.at(i).kind == StepValue::Number)
                   ? r->args.at(i).number : 0.0;
    }

    QVector3D point3(int id) const;
    QVector3D direction(int id) const;
    Placement placement(int id) const;
    bool surfaceOf(int id, Surface *out);

    bool nurbsOf(const StepRecord *r, NurbsCurve *out) const;

    // Appends the samples of one oriented edge, from its start vertex towards
    // its end vertex, EXCLUDING the final point (the next edge contributes it).
    bool sampleOrientedEdge(int orientedEdgeId, QVector<QVector3D> *out);
    bool sampleCurve(int curveId, const QVector3D &from, const QVector3D &to,
                     bool forward, QVector<QVector3D> *out);
    void sampleSpline(const NurbsCurve &c, double a, double b,
                      const QVector3D &pa, const QVector3D &pb,
                      int depth, QVector<QVector3D> *out);

    bool loopPoints(int loopId, QVector<QVector3D> *out);

    const StepFile &m_file;
    Tolerances m_tol;
};

QVector3D Builder::point3(int id) const
{
    const StepRecord *r = rec(id);
    if (!r || r->args.size() < 2 || !r->args.at(1).isList())
        return QVector3D();
    const QVector<StepValue> &c = r->args.at(1).items;
    return QVector3D(c.size() > 0 ? float(c.at(0).number) : 0.0f,
                     c.size() > 1 ? float(c.at(1).number) : 0.0f,
                     c.size() > 2 ? float(c.at(2).number) : 0.0f);
}

QVector3D Builder::direction(int id) const
{
    const QVector3D d = point3(id);   // DIRECTION has the same shape
    return d.lengthSquared() > 1e-20f ? d.normalized() : QVector3D(0, 0, 1);
}

Placement Builder::placement(int id) const
{
    Placement pl;
    const StepRecord *r = rec(id);
    if (!r)
        return pl;

    pl.origin = point3(refArg(r, 1));

    // Both directions are optional in the schema; the defaults are the world
    // axes, which is exactly what a placement with $ arguments means.
    const int axisId = refArg(r, 2);
    const int refId = refArg(r, 3);
    if (axisId)
        pl.z = direction(axisId);
    if (refId)
        pl.x = direction(refId);

    // ref_direction is only a hint: the schema says the x axis is its
    // projection into the plane normal to the axis. Files do contain
    // ref_directions that are not perpendicular, and using one unprojected
    // gives a skewed frame in which the inverse mapping is simply wrong.
    pl.x = pl.x - QVector3D::dotProduct(pl.x, pl.z) * pl.z;
    if (pl.x.lengthSquared() < 1e-12f) {
        // Degenerate hint: any perpendicular will do, since nothing else in
        // the file depends on where u = 0 sits.
        pl.x = qAbs(pl.z.x()) < 0.9f ? QVector3D::crossProduct(pl.z, QVector3D(1, 0, 0))
                                     : QVector3D::crossProduct(pl.z, QVector3D(0, 1, 0));
    }
    pl.x.normalize();
    pl.y = QVector3D::crossProduct(pl.z, pl.x).normalized();
    return pl;
}

bool Builder::surfaceOf(int id, Surface *out)
{
    const StepRecord *r = rec(id);
    if (!r)
        return false;

    if (r->type == QLatin1String("PLANE")) {
        out->type = Surface::Plane;
        out->p = placement(refArg(r, 1));
        return true;
    }
    if (r->type == QLatin1String("CYLINDRICAL_SURFACE")) {
        out->type = Surface::Cylinder;
        out->p = placement(refArg(r, 1));
        out->radius = numArg(r, 2);
        if (out->radius <= 0.0)
            return false;
        return true;
    }

    ++unsupportedSurfaces;
    unsupportedSurfaceNames.insert(r->type);
    return false;
}

bool Builder::nurbsOf(const StepRecord *r, NurbsCurve *out) const
{
    /*
      The simple form is one record carrying everything:

          B_SPLINE_CURVE_WITH_KNOTS(name, degree, control_points, form,
                                    closed, self_intersect,
                                    multiplicities, knots, spec)

      The complex form is the same information SPLIT ACROSS PARTS, because
      that is how Part 21 writes multiple inheritance — each part carries only
      the attributes its own entity declares:

          B_SPLINE_CURVE(degree, control_points, form, closed, self_intersect)
          B_SPLINE_CURVE_WITH_KNOTS(multiplicities, knots, spec)
          RATIONAL_B_SPLINE_CURVE(weights)

      Note there is no name and no repetition: reading the knots part as if it
      were the whole entity finds three arguments where nine were expected.
      Every seam edge of a cylinder in the reference file is written this way,
      as are the long edges of its chamfers.
    */
    const bool complex = r->isComplex();
    const StepRecord *shape = complex ? r->part("B_SPLINE_CURVE") : r;
    const StepRecord *knotPart = complex ? r->part("B_SPLINE_CURVE_WITH_KNOTS") : r;
    const StepRecord *rational = complex ? r->part("RATIONAL_B_SPLINE_CURVE") : nullptr;
    if (!shape || !knotPart)
        return false;

    // Where each attribute sits differs between the two forms only by the
    // leading name argument the simple form has and the complex form has not.
    const int degreeAt = complex ? 0 : 1;
    const int cvAt = complex ? 1 : 2;
    const int multAt = complex ? 0 : 6;
    const int knotAt = complex ? 1 : 7;

    if (shape->args.size() <= cvAt || knotPart->args.size() <= knotAt)
        return false;

    out->degree = int(shape->args.at(degreeAt).number);
    if (out->degree < 1)
        return false;

    const QVector<StepValue> &cps = shape->args.at(cvAt).items;
    for (int i = 0; i < cps.size(); ++i) {
        if (!cps.at(i).isRef())
            return false;
        out->cv.append(point3(cps.at(i).ref));
    }

    const QVector<StepValue> &mult = knotPart->args.at(multAt).items;
    const QVector<StepValue> &knots = knotPart->args.at(knotAt).items;
    if (mult.size() != knots.size() || mult.isEmpty())
        return false;

    // STEP stores distinct knots with multiplicities; de Boor wants them
    // written out.
    for (int i = 0; i < knots.size(); ++i) {
        const int m = int(mult.at(i).number);
        for (int k = 0; k < m; ++k)
            out->knot.append(knots.at(i).number);
    }

    if (rational && rational->args.size() >= 1) {
        const QVector<StepValue> &w = rational->args.at(0).items;
        if (w.size() == out->cv.size())
            for (int i = 0; i < w.size(); ++i)
                out->weight.append(w.at(i).number);
    }

    return out->isValid();
}

/*
  Splits a spline in half until the middle of each piece is within the chord
  tolerance of the straight line across it.

  Sampling at a fixed rate instead would either waste points on the straight
  degree-1 splines that make up most of this file — ST-Developer writes even
  plain edges that way — or under-sample a tight fillet.
*/
void Builder::sampleSpline(const NurbsCurve &c, double a, double b,
                           const QVector3D &pa, const QVector3D &pb,
                           int depth, QVector<QVector3D> *out)
{
    const double mid = 0.5 * (a + b);
    const QVector3D pm = c.eval(mid);

    bool flat = true;
    if (depth < kMaxSplineDepth) {
        const QVector3D chord = pb - pa;
        const double len2 = double(chord.lengthSquared());
        double sag;
        if (len2 < 1e-18) {
            // The ends coincide: a closed or doubled-back piece, where the
            // distance from the midpoint to the "chord" is the only measure
            // left and a zero-length chord would divide by zero.
            sag = double((pm - pa).length());
        } else {
            const QVector3D cross = QVector3D::crossProduct(pm - pa, chord);
            sag = double(cross.length()) / qSqrt(len2);
        }
        flat = sag <= m_tol.chord;
    }

    if (flat) {
        out->append(pa);
        return;
    }
    sampleSpline(c, a, mid, pa, pm, depth + 1, out);
    sampleSpline(c, mid, b, pm, pb, depth + 1, out);
}

bool Builder::sampleCurve(int curveId, const QVector3D &from, const QVector3D &to,
                          bool forward, QVector<QVector3D> *out)
{
    const StepRecord *r = rec(curveId);
    if (!r)
        return false;

    // --- a trimmed curve is a wrapper; unwrap it and keep the sense ---------
    if (r->type == QLatin1String("TRIMMED_CURVE")) {
        // TRIMMED_CURVE(name, basis, trim1, trim2, sense_agreement, master)
        const int basis = refArg(r, 1);
        bool agree = true;
        if (r->args.size() > 4 && r->args.at(4).kind == StepValue::Enum)
            agree = r->args.at(4).isEnum("T");
        // The trim parameters are ignored on purpose: the edge's own two
        // vertices already say where this piece starts and ends, and they are
        // the same two points the neighbouring edges must meet at.
        return sampleCurve(basis, from, to, forward == agree, out);
    }

    if (r->type == QLatin1String("LINE")) {
        out->append(from);
        return true;
    }

    if (r->type == QLatin1String("CIRCLE")) {
        const Placement pl = placement(refArg(r, 1));
        const double radius = numArg(r, 2);
        if (radius <= 0.0)
            return false;

        const QVector3D da = from - pl.origin;
        const QVector3D db = to - pl.origin;
        const double a0 = qAtan2(double(QVector3D::dotProduct(da, pl.y)),
                                 double(QVector3D::dotProduct(da, pl.x)));
        const double a1 = qAtan2(double(QVector3D::dotProduct(db, pl.y)),
                                 double(QVector3D::dotProduct(db, pl.x)));

        // A circle is parameterised anticlockwise about its axis. Which of the
        // two arcs between the endpoints this edge means is NOT decidable from
        // the endpoints — only the sense flags say, which is why they are
        // tracked all the way down here.
        double sweep = forward ? (a1 - a0) : (a0 - a1);
        while (sweep <= 1e-9)
            sweep += 2.0 * M_PI;
        // Endpoints that coincide mean the whole circle, not a zero-length arc.
        if ((from - to).lengthSquared() < float(m_tol.weld * m_tol.weld))
            sweep = 2.0 * M_PI;

        double step = M_PI;
        if (radius > m_tol.chord)
            step = 2.0 * qAcos(1.0 - m_tol.chord / radius);
        int segments = int(qCeil(sweep / qMax(step, 1e-6)));
        segments = qBound(kMinArcSegments, segments, kMaxArcSegments);

        for (int i = 0; i < segments; ++i) {
            const double t = double(i) / double(segments);
            const double ang = forward ? (a0 + sweep * t) : (a0 - sweep * t);
            out->append(pl.origin
                        + float(radius * qCos(ang)) * pl.x
                        + float(radius * qSin(ang)) * pl.y);
        }
        return true;
    }

    if (r->type == QLatin1String("B_SPLINE_CURVE_WITH_KNOTS")
        || (r->isComplex() && r->part("B_SPLINE_CURVE_WITH_KNOTS"))) {
        NurbsCurve c;
        if (!nurbsOf(r, &c)) {
            // Contribute the corner even so. Returning nothing here costs the
            // loop a vertex, and a boundary short of two vertices is thrown
            // away whole — one unreadable edge silently deleting an entire
            // face from the solid.
            ++unsupportedCurves;
            out->append(from);
            return true;
        }

        QVector<QVector3D> pts;
        sampleSpline(c, c.t0(), c.t1(), c.eval(c.t0()), c.eval(c.t1()), 0, &pts);
        pts.append(c.eval(c.t1()));

        if (!forward)
            std::reverse(pts.begin(), pts.end());

        // The sense flags have been followed faithfully, but files do contain
        // edges whose flags disagree with their own vertices. The vertices win:
        // a loop that does not close cannot be triangulated at all, while a
        // curve traversed the wrong way merely looks wrong.
        if (!pts.isEmpty()) {
            const float dStart = (pts.first() - from).lengthSquared();
            const float dEnd = (pts.last() - from).lengthSquared();
            if (dEnd < dStart)
                std::reverse(pts.begin(), pts.end());
        }

        pts.removeLast();          // the next edge contributes the shared point
        out->append(pts);
        return true;
    }

    ++unsupportedCurves;
    // Straight from vertex to vertex. Wrong in shape, but it keeps the loop
    // closed so the rest of the face still appears.
    out->append(from);
    return true;
}

bool Builder::sampleOrientedEdge(int orientedEdgeId, QVector<QVector3D> *out)
{
    const StepRecord *oe = rec(orientedEdgeId);
    if (!oe || oe->args.size() < 5)
        return false;

    const int edgeId = refArg(oe, 3);
    bool orientation = true;
    if (oe->args.at(4).kind == StepValue::Enum)
        orientation = oe->args.at(4).isEnum("T");

    const StepRecord *ec = rec(edgeId);
    if (!ec || ec->args.size() < 5)
        return false;

    const QVector3D v1 = point3(refArg(rec(refArg(ec, 1)), 1));
    const QVector3D v2 = point3(refArg(rec(refArg(ec, 2)), 1));
    const int curveId = refArg(ec, 3);
    bool sameSense = true;
    if (ec->args.at(4).kind == StepValue::Enum)
        sameSense = ec->args.at(4).isEnum("T");

    // The edge runs v1 -> v2; the oriented edge may traverse it backwards.
    const QVector3D from = orientation ? v1 : v2;
    const QVector3D to = orientation ? v2 : v1;
    // And the curve's own direction agrees with the traversal only when the
    // two flags agree.
    const bool forward = (sameSense == orientation);

    return sampleCurve(curveId, from, to, forward, out);
}

bool Builder::loopPoints(int loopId, QVector<QVector3D> *out)
{
    const StepRecord *loop = rec(loopId);
    if (!loop)
        return false;
    // EDGE_LOOP(name, edge_list); VERTEX_LOOP is a single point and bounds
    // nothing, so it is not a loop we can triangulate.
    if (loop->type != QLatin1String("EDGE_LOOP") || loop->args.size() < 2)
        return false;

    const QVector<StepValue> &edges = loop->args.at(1).items;
    for (int i = 0; i < edges.size(); ++i) {
        if (!edges.at(i).isRef())
            continue;
        sampleOrientedEdge(edges.at(i).ref, out);
    }

    // Drop points that repeat: a shared vertex sampled by both neighbours, or
    // a zero-length edge. An ear clipper handed two identical points in a row
    // produces a zero-area ear and can stall on it.
    QVector<QVector3D> cleaned;
    const float weld2 = float(m_tol.weld * m_tol.weld);
    for (int i = 0; i < out->size(); ++i) {
        if (!cleaned.isEmpty() && (out->at(i) - cleaned.last()).lengthSquared() < weld2)
            continue;
        cleaned.append(out->at(i));
    }
    while (cleaned.size() > 1
           && (cleaned.first() - cleaned.last()).lengthSquared() < weld2)
        cleaned.removeLast();

    *out = cleaned;
    return out->size() >= 3;
}

// ------------------------------------------------------- polygon machinery --

double signedArea(const QVector<QPointF> &poly)
{
    double a = 0.0;
    for (int i = 0, n = poly.size(); i < n; ++i) {
        const QPointF &p = poly.at(i);
        const QPointF &q = poly.at((i + 1) % n);
        a += p.x() * q.y() - q.x() * p.y();
    }
    return 0.5 * a;
}

double cross2(const QPointF &o, const QPointF &a, const QPointF &b)
{
    return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

bool samePoint(const QPointF &a, const QPointF &b)
{
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return dx * dx + dy * dy < 1e-18;
}

bool pointInTriangle(const QPointF &p, const QPointF &a, const QPointF &b,
                     const QPointF &c)
{
    const double d1 = cross2(a, b, p);
    const double d2 = cross2(b, c, p);
    const double d3 = cross2(c, a, p);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

bool segmentsCross(const QPointF &a, const QPointF &b,
                   const QPointF &c, const QPointF &d)
{
    // Shared endpoints are not crossings: bridges deliberately touch the
    // polygon at a vertex.
    const double eps = 1e-12;
    const double d1 = cross2(c, d, a);
    const double d2 = cross2(c, d, b);
    const double d3 = cross2(a, b, c);
    const double d4 = cross2(a, b, d);
    if (((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps))
        && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps)))
        return true;
    return false;
}

/*
  Ear clipping.

  O(n^2), which for the few hundred points a face boundary comes to is
  nothing, and unlike a sweep-line triangulator it is short enough to be read
  and trusted. The `guard` counter is the answer to the one real failure mode:
  a polygon that is not simple — self-intersecting because the file's geometry
  is inconsistent — has no ear at all, and without the counter the loop would
  never end.
*/
QVector<int> earClip(const QVector<QPointF> &poly)
{
    QVector<int> tris;
    const int n = poly.size();
    if (n < 3)
        return tris;

    QVector<int> idx(n);
    for (int i = 0; i < n; ++i)
        idx[i] = i;

    // Work anticlockwise, so a convex corner is a positive turn.
    if (signedArea(poly) < 0.0)
        std::reverse(idx.begin(), idx.end());

    int guard = 0;
    const int guardLimit = n * n + 16;

    // Choosing the BEST ear rather than the first is what keeps a long thin
    // face — a cylinder's seam-to-seam band is exactly that — from being
    // triangulated as a fan. Scanning from the start each time clips
    // everything around one corner, and on a curved surface those long
    // triangles cut straight through the solid instead of following it: 3 mm²
    // of surface facing the wrong way, not a rounding error.
    //
    // Quality is the ratio of area to squared edge length, which is largest
    // for an equilateral triangle and near zero for a sliver — so a
    // near-degenerate ear, the kind that only looks convex because of
    // floating-point noise along a straight edge, always loses to a real one.
    // Above this many points the search is quadratic per clip and the first
    // usable ear is taken instead; no face in a mechanical part comes close.
    const int kBestEarLimit = 256;
    const bool bestEar = (n <= kBestEarLimit);

    while (idx.size() > 3 && guard++ < guardLimit) {
        bool clipped = false;
        int bestAt = -1;
        double bestQuality = -1.0;

        for (int i = 0; i < idx.size(); ++i) {
            const int ia = idx.at((i + idx.size() - 1) % idx.size());
            const int ib = idx.at(i);
            const int ic = idx.at((i + 1) % idx.size());
            const QPointF &a = poly.at(ia);
            const QPointF &b = poly.at(ib);
            const QPointF &c = poly.at(ic);

            if (cross2(a, b, c) <= 0.0)
                continue;                      // reflex or straight: no ear

            bool contains = false;
            for (int k = 0; k < idx.size(); ++k) {
                const int p = idx.at(k);
                if (p == ia || p == ib || p == ic)
                    continue;
                // Compare POSITIONS, not just indices. Bridging a hole leaves
                // both ends of the bridge in the ring twice, and a vertex that
                // merely repeats one of this triangle's own corners is not a
                // point inside it. Testing indices alone rejects every ear
                // touching a bridge, which is exactly the ears a face with a
                // hole depends on — the face then produces no triangles at
                // all, while a face without holes sails through.
                const QPointF &q = poly.at(p);
                if (samePoint(q, a) || samePoint(q, b) || samePoint(q, c))
                    continue;
                if (pointInTriangle(q, a, b, c)) {
                    contains = true;
                    break;
                }
            }
            if (contains)
                continue;

            if (!bestEar) {
                bestAt = i;
                break;
            }

            const double ab = QPointF::dotProduct(b - a, b - a);
            const double bc = QPointF::dotProduct(c - b, c - b);
            const double ca = QPointF::dotProduct(a - c, a - c);
            const double sum = ab + bc + ca;
            const double quality = sum > 1e-18 ? cross2(a, b, c) / sum : 0.0;
            if (quality > bestQuality) {
                bestQuality = quality;
                bestAt = i;
            }
        }

        if (bestAt >= 0) {
            const int ia = idx.at((bestAt + idx.size() - 1) % idx.size());
            const int ib = idx.at(bestAt);
            const int ic = idx.at((bestAt + 1) % idx.size());
            tris << ia << ib << ic;
            idx.remove(bestAt);
            clipped = true;
        }

        if (!clipped)
            break;   // no ear anywhere: the polygon is not simple
    }

    if (idx.size() == 3)
        tris << idx.at(0) << idx.at(1) << idx.at(2);

    return tris;
}

/*
  Cuts each hole into the outer boundary with a bridge, turning a polygon with
  holes into one simple polygon the ear clipper can handle.

  The bridge is a pair of coincident edges run out to the hole and back. That
  is the standard trick and it is why the ear clipper must tolerate duplicated
  points: both bridge vertices appear twice in the merged ring.

  Holes are taken rightmost first. Merging in that order means a hole is only
  ever bridged against boundary that is already final, so one hole's bridge
  cannot cut through another hole that has not been merged yet.
*/
QVector<int> mergeHoles(QVector<QPointF> *ring, QVector<QVector<QPointF> > holes)
{
    QVector<int> dummy;
    if (holes.isEmpty())
        return dummy;

    // Outer anticlockwise, holes clockwise: opposite windings are what make
    // the bridged ring a single consistently-wound loop.
    if (signedArea(*ring) < 0.0)
        std::reverse(ring->begin(), ring->end());
    for (int h = 0; h < holes.size(); ++h)
        if (signedArea(holes.at(h)) > 0.0)
            std::reverse(holes[h].begin(), holes[h].end());

    // Rightmost hole first.
    std::sort(holes.begin(), holes.end(),
              [](const QVector<QPointF> &a, const QVector<QPointF> &b) {
                  double ax = -1e300, bx = -1e300;
                  for (int i = 0; i < a.size(); ++i) ax = qMax(ax, a.at(i).x());
                  for (int i = 0; i < b.size(); ++i) bx = qMax(bx, b.at(i).x());
                  return ax > bx;
              });

    for (int h = 0; h < holes.size(); ++h) {
        const QVector<QPointF> &hole = holes.at(h);
        if (hole.size() < 3)
            continue;

        // Candidate hole vertices: the rightmost first, then a spread of
        // others as a fallback for the awkward cases where it is not visible.
        QVector<int> candidates;
        int rightmost = 0;
        for (int i = 1; i < hole.size(); ++i)
            if (hole.at(i).x() > hole.at(rightmost).x())
                rightmost = i;
        candidates.append(rightmost);
        const int stride = qMax(1, hole.size() / 8);
        for (int i = 0; i < hole.size(); i += stride)
            if (i != rightmost)
                candidates.append(i);

        bool bridged = false;
        for (int ci = 0; ci < candidates.size() && !bridged; ++ci) {
            const int hi = candidates.at(ci);
            const QPointF &hp = hole.at(hi);

            // Outer vertices by distance: the shortest bridge that works is
            // also the one least likely to make a sliver.
            QVector<QPair<double, int> > order;
            order.reserve(ring->size());
            for (int i = 0; i < ring->size(); ++i) {
                const QPointF d = ring->at(i) - hp;
                order.append(qMakePair(d.x() * d.x() + d.y() * d.y(), i));
            }
            std::sort(order.begin(), order.end());

            for (int oi = 0; oi < order.size(); ++oi) {
                const int ri = order.at(oi).second;
                const QPointF &rp = ring->at(ri);

                bool blocked = false;
                for (int i = 0; i < ring->size() && !blocked; ++i) {
                    const QPointF &a = ring->at(i);
                    const QPointF &b = ring->at((i + 1) % ring->size());
                    if (segmentsCross(hp, rp, a, b))
                        blocked = true;
                }
                for (int k = h; k < holes.size() && !blocked; ++k) {
                    const QVector<QPointF> &other = holes.at(k);
                    for (int i = 0; i < other.size() && !blocked; ++i) {
                        const QPointF &a = other.at(i);
                        const QPointF &b = other.at((i + 1) % other.size());
                        if (segmentsCross(hp, rp, a, b))
                            blocked = true;
                    }
                }
                if (blocked)
                    continue;

                // Splice: ...ring[ri], hole[hi..], hole[..hi], ring[ri]...
                QVector<QPointF> merged;
                merged.reserve(ring->size() + hole.size() + 2);
                for (int i = 0; i <= ri; ++i)
                    merged.append(ring->at(i));
                for (int i = 0; i < hole.size(); ++i)
                    merged.append(hole.at((hi + i) % hole.size()));
                merged.append(hole.at(hi));
                for (int i = ri; i < ring->size(); ++i)
                    merged.append(ring->at(i));
                *ring = merged;
                bridged = true;
                break;
            }
        }
    }
    return dummy;
}

bool Builder::buildFace(int faceId, SceneLayer *layer, Scene *scene)
{
    const StepRecord *face = rec(faceId);
    if (!face || face->args.size() < 4)
        return false;

    Surface surface;
    if (!surfaceOf(refArg(face, 2), &surface))
        return false;

    bool sameSense = true;
    if (face->args.at(3).kind == StepValue::Enum)
        sameSense = face->args.at(3).isEnum("T");

    // --- the loops, in 3D --------------------------------------------------
    QVector<QVector<QVector3D> > loops3d;
    QVector<bool> isOuter;

    const QVector<StepValue> &bounds = face->args.at(1).items;
    for (int i = 0; i < bounds.size(); ++i) {
        if (!bounds.at(i).isRef())
            continue;
        const StepRecord *b = rec(bounds.at(i).ref);
        if (!b || b->args.size() < 3)
            continue;

        QVector<QVector3D> pts;
        if (!loopPoints(refArg(b, 1), &pts)) {
            ++degenerateLoops;
            continue;
        }

        // FACE_BOUND's orientation flag reverses the loop. It matters because
        // which side is "inside" is decided by winding.
        if (b->args.at(2).kind == StepValue::Enum && b->args.at(2).isEnum("F"))
            std::reverse(pts.begin(), pts.end());

        loops3d.append(pts);
        isOuter.append(b->type == QLatin1String("FACE_OUTER_BOUND"));
    }

    if (loops3d.isEmpty())
        return false;

    // --- into (u,v) --------------------------------------------------------
    const double us = surface.uScale();
    QVector<QVector<QPointF> > loops2d;
    loops2d.reserve(loops3d.size());

    for (int l = 0; l < loops3d.size(); ++l) {
        const QVector<QVector3D> &src = loops3d.at(l);
        QVector<QPointF> uv;
        uv.reserve(src.size());

        double prevU = 0.0;
        for (int i = 0; i < src.size(); ++i) {
            double u = 0.0, v = 0.0;
            surface.invert(src.at(i), &u, &v);

            // On a cylinder u is an angle and comes back inside one turn, so a
            // loop crossing the seam would jump by 2*pi and fold the polygon
            // over itself. Following the nearest branch instead keeps it
            // continuous, and a loop that really does go all the way round
            // then correctly spans a full 2*pi rather than closing early.
            if (surface.periodicU() && i > 0) {
                while (u - prevU > M_PI)
                    u -= 2.0 * M_PI;
                while (prevU - u > M_PI)
                    u += 2.0 * M_PI;
            }
            prevU = u;
            uv.append(QPointF(u * us, v));
        }
        loops2d.append(uv);
    }

    // Which loop is the outer one. The flag in the file is the authority, but
    // some writers mark every bound FACE_BOUND, and then the largest area is
    // the only thing left to go on.
    int outerIndex = -1;
    for (int i = 0; i < loops2d.size(); ++i) {
        if (isOuter.at(i)) {
            outerIndex = i;
            break;
        }
    }
    if (outerIndex < 0) {
        double best = -1.0;
        for (int i = 0; i < loops2d.size(); ++i) {
            const double a = qAbs(signedArea(loops2d.at(i)));
            if (a > best) {
                best = a;
                outerIndex = i;
            }
        }
    }
    if (outerIndex < 0)
        return false;

    QVector<QPointF> ring = loops2d.at(outerIndex);
    QVector<QVector<QPointF> > holes;
    for (int i = 0; i < loops2d.size(); ++i)
        if (i != outerIndex)
            holes.append(loops2d.at(i));

    if (ring.size() < 3)
        return false;

    mergeHoles(&ring, holes);

    const QVector<int> tris = earClip(ring);
    if (tris.isEmpty()) {
        ++failedFaces;
        return false;
    }

    // --- back to 3D --------------------------------------------------------
    //
    // Each face keeps its own block of vertices rather than sharing them with
    // its neighbours. That is not waste: the wireframe mode finds real edges
    // by looking for triangle edges belonging to only one triangle, and that
    // test only works if two different faces share no vertex indices.
    const quint32 base = quint32(layer->vertices.size() / kFloatsPerVertex);
    const float r = float(layer->color.redF());
    const float g = float(layer->color.greenF());
    const float bl = float(layer->color.blueF());

    for (int i = 0; i < ring.size(); ++i) {
        const double u = ring.at(i).x() / us;
        const double v = ring.at(i).y();
        const QVector3D p = surface.eval(u, v);
        QVector3D n = surface.normalAt(u, v);
        if (!sameSense)
            n = -n;
        layer->vertices << p.x() << p.y() << p.z()
                        << n.x() << n.y() << n.z()
                        << r << g << bl;
        scene->growBounds(p);
    }

    for (int t = 0; t + 2 < tris.size(); t += 3) {
        // The ear clipper works anticlockwise in parameter space, which is the
        // front face when the surface normal is the face normal. A face marked
        // same_sense = false is the same geometry seen from the other side, so
        // its triangles have to be wound the other way to match.
        if (sameSense) {
            layer->indices << base + quint32(tris.at(t))
                           << base + quint32(tris.at(t + 1))
                           << base + quint32(tris.at(t + 2));
        } else {
            layer->indices << base + quint32(tris.at(t))
                           << base + quint32(tris.at(t + 2))
                           << base + quint32(tris.at(t + 1));
        }
    }

    return true;
}

} // namespace

ScenePtr StepReader::read(const QString &path,
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

    StepFile file;
    QString parseError;
    if (!file.parse(data, &parseError)) {
        if (errorOut)
            *errorOut = parseError;
        return ScenePtr();
    }

    if (progress)
        progress(progressCtx, 0.25f);

    ScenePtr scene(new Scene);
    scene->filePath = path;
    scene->formatName = QStringLiteral("STEP");
    scene->producer = file.originatingSystem();

    // Tolerances scaled to the model. A tolerance fixed in millimetres would
    // over-tessellate a large part and visibly facet a small one; the model's
    // own size is the only scale available before anything is built.
    Tolerances tol;
    {
        QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        const QVector<int> pts = file.idsOfType(QStringLiteral("CARTESIAN_POINT"));
        for (int i = 0; i < pts.size(); ++i) {
            const StepRecord *r = file.record(pts.at(i));
            if (!r || r->args.size() < 2 || !r->args.at(1).isList())
                continue;
            const QVector<StepValue> &c = r->args.at(1).items;
            if (c.size() < 3)
                continue;
            const QVector3D p(float(c.at(0).number), float(c.at(1).number),
                              float(c.at(2).number));
            lo.setX(qMin(lo.x(), p.x())); hi.setX(qMax(hi.x(), p.x()));
            lo.setY(qMin(lo.y(), p.y())); hi.setY(qMax(hi.y(), p.y()));
            lo.setZ(qMin(lo.z(), p.z())); hi.setZ(qMax(hi.z(), p.z()));
        }
        const double diagonal = (hi.x() >= lo.x()) ? double((hi - lo).length()) : 0.0;
        if (diagonal > 1e-9) {
            tol.chord = qBound(1e-5, diagonal * 0.0004, 1.0);
            tol.weld = qBound(1e-9, diagonal * 1e-6, 1e-2);
        }
    }

    // --- which faces belong to which solid --------------------------------
    //
    // Grouping by solid is what gives the layer list something meaningful to
    // switch: an assembly exported as one file becomes one entry per part
    // rather than one entry for everything.
    QVector<QPair<QString, QVector<int> > > groups;
    QSet<int> grouped;

    const QVector<int> solids = file.idsOfType(QStringLiteral("MANIFOLD_SOLID_BREP"));
    for (int i = 0; i < solids.size(); ++i) {
        const StepRecord *solid = file.record(solids.at(i));
        if (!solid || solid->args.size() < 2)
            continue;
        QString name = solid->args.at(0).kind == StepValue::Text
                           ? solid->args.at(0).text : QString();
        const StepRecord *shell = solid->args.at(1).isRef()
                                      ? file.record(solid->args.at(1).ref) : nullptr;
        if (!shell || shell->args.size() < 2 || !shell->args.at(1).isList())
            continue;

        QVector<int> faces;
        const QVector<StepValue> &fl = shell->args.at(1).items;
        for (int k = 0; k < fl.size(); ++k) {
            if (!fl.at(k).isRef())
                continue;
            faces.append(fl.at(k).ref);
            grouped.insert(fl.at(k).ref);
        }
        if (name.isEmpty())
            name = QObject::tr("Solid %1").arg(i + 1);
        groups.append(qMakePair(name, faces));
    }

    // Faces that belong to no solid — an open shell, or a surface model.
    QVector<int> loose;
    const QVector<int> allFaces = file.idsOfType(QStringLiteral("ADVANCED_FACE"));
    for (int i = 0; i < allFaces.size(); ++i)
        if (!grouped.contains(allFaces.at(i)))
            loose.append(allFaces.at(i));
    if (!loose.isEmpty())
        groups.append(qMakePair(QObject::tr("Surfaces"), loose));

    if (groups.isEmpty()) {
        if (errorOut)
            *errorOut = QObject::tr("The STEP file contains no solid or surface.");
        return ScenePtr();
    }

    // --- build -------------------------------------------------------------
    Builder builder(file, tol);

    static const QColor kPalette[] = {
        QColor(190, 192, 196), QColor(150, 178, 205), QColor(205, 176, 140),
        QColor(160, 200, 170), QColor(198, 160, 190), QColor(190, 190, 150)
    };
    const int paletteSize = int(sizeof(kPalette) / sizeof(kPalette[0]));

    int totalFaces = 0;
    for (int i = 0; i < groups.size(); ++i)
        totalFaces += groups.at(i).second.size();
    int done = 0;

    for (int gi = 0; gi < groups.size(); ++gi) {
        SceneLayer layer;
        layer.name = groups.at(gi).first;
        layer.color = kPalette[gi % paletteSize];
        layer.visible = true;
        layer.sourceIndex = gi;

        const QVector<int> &faces = groups.at(gi).second;
        for (int i = 0; i < faces.size(); ++i) {
            builder.buildFace(faces.at(i), &layer, scene.data());
            ++done;
            if (progress && totalFaces > 0 && (done & 7) == 0)
                progress(progressCtx, 0.25f + 0.7f * float(done) / float(totalFaces));
        }

        if (!layer.isEmpty()) {
            scene->layers.append(layer);
            scene->objectCount++;
        }
    }

    // --- report ------------------------------------------------------------
    if (builder.unsupportedSurfaces > 0) {
        QStringList names = builder.unsupportedSurfaceNames.toList();
        std::sort(names.begin(), names.end());
        scene->warnings << QObject::tr(
            "%1 face(s) use a surface type this app cannot tessellate (%2) and "
            "were skipped.")
            .arg(builder.unsupportedSurfaces)
            .arg(names.join(QStringLiteral(", ")));
    }
    if (builder.unsupportedCurves > 0) {
        scene->warnings << QObject::tr(
            "%1 edge(s) use an unsupported curve type and were straightened.")
            .arg(builder.unsupportedCurves);
    }
    if (builder.failedFaces > 0) {
        scene->warnings << QObject::tr(
            "%1 face(s) could not be triangulated and are missing.")
            .arg(builder.failedFaces);
    }
    if (builder.degenerateLoops > 0) {
        scene->warnings << QObject::tr("%1 boundary loop(s) were degenerate.")
            .arg(builder.degenerateLoops);
    }
    if (scene->isEmpty()) {
        if (errorOut)
            *errorOut = QObject::tr("Nothing in the STEP file could be drawn.");
        return ScenePtr();
    }

    if (progress)
        progress(progressCtx, 1.0f);

    return scene;
}

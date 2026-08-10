/*
  harbour-3dmviewer — dropsim.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "physics/dropsim.h"

#include <QtMath>
#include <QStringList>

namespace {

// Fixed physics timestep. Contact solvers are only stable at a steady dt, and
// 240 Hz is small enough that a fast-falling part cannot pass through the floor
// between two steps.
const float kFixedDt = 1.0f / 240.0f;
// Cap on how much simulated time one frame may consume. Without it, a long
// stall (page transition, model load) is followed by a burst of hundreds of
// substeps and the app appears to freeze.
const float kMaxFrameTime = 0.1f;

// Gravity is expressed as "how fast does it fall relative to the box", not as
// an absolute acceleration: the model's units are unknown, so 9.81 would be
// right only if the file happened to be modelled in metres. This value makes
// the body fall the box's half-height in roughly 0.38 s, which reads as normal
// gravity at any model scale.  (t = sqrt(2/kFallRate))
const float kFallRate = 14.0f;
const float kRestitution = 0.12f;  // low: CAD parts are not bouncy balls
const float kFriction = 0.62f;     // high enough that it topples instead of sliding
const int   kSolverIterations = 8;
// Iterations of the separate positional pass. Fewer are needed than for
// velocity because the pass is purely geometric and converges quickly.
const int   kPositionIterations = 3;

// How much of the off-centre torque the trampoline sheet passes on. A rigid
// floor would deliver all of it (and eat the rebound doing so); fabric gives
// way and delivers a fraction, which is enough to keep the part tumbling
// without costing it height.
//
// KEEP THIS SMALL. Every bounce ADDS torque, and with the sheet returning 99%
// there is almost no loss to work against it — at 0.22 the spin compounded
// from bounce to bounce until the part was whirling. It is an increment, not a
// target.
const float kTrampolineTorqueShare = 0.07f;

// Hard ceiling on the tumble, in radians per second. Roughly a third of a turn
// per second, which reads as a slow roll rather than a spin. A cap is the only
// thing that actually guarantees the accumulation above can never run away,
// whatever the restitution is set to.
const float kMaxAngularRate = 2.0f;

// Tumble given to the part at the moment it is released. Strictly speaking an
// object let go from rest does not rotate — but then it falls dead straight and
// lands on the same face every time, which looks like a bug rather than like
// physics. All three components are non-zero so it turns about every axis.
const QVector3D kReleaseSpin(0.62f, 1.00f, 0.35f);
const float kReleaseSpinRate = 0.85f;   // radians per second — a drift, not a spin

// Penetration is corrected gradually rather than at once, otherwise resting
// contacts inject energy and the body jitters forever. Slop leaves a sliver of
// overlap unresolved so contacts stay persistent instead of flickering.
//
// BOTH ARE RELATIVE TO THE MODEL, and that matters more than it looks. An
// absolute slop of a thousandth of a unit is meaningless for a part 140 units
// across: every contact then counts as deeply penetrating, and the bias term
// (beta/dt = 48 at 240 Hz) turns a ten-unit overlap into a 480 unit/s kick.
// That was the shivering.
const float kBaumgarte = 0.2f;
const float kSlopRelative = 0.004f;      // times the model radius

// Room depth, as a fraction of the on-screen half height and as a floor in
// model radii. Tuned by eye: 0.45/1.15 read as too shallow, then doubled, then
// a further 15% on top.
const float kRoomDepthOfHeight = 1.035f;
const float kRoomMinDepthRadii = 2.645f;

// Sleep thresholds, relative to model size — an absolute value would send a
// 500 mm part to sleep while still visibly moving, or never sleep a 5 mm one.
const float kSleepLinear = 0.012f;
const float kSleepAngular = 0.06f;
const float kSleepTime = 0.45f;

// Directions used to pick extreme vertices as contact candidates. A subdivided
// octahedron: cheap to write out, evenly spread, and dense enough that a flat
// face contributes several well-separated points — which is what a body needs
// to rest stably rather than pivot on a single corner.
QVector<QVector3D> sampleDirections()
{
    QVector<QVector3D> dirs;
    const int N = 5;   // steps per axis of the cube face grid
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            const float u = -1.0f + 2.0f * float(i) / float(N - 1);
            const float v = -1.0f + 2.0f * float(j) / float(N - 1);
            dirs << QVector3D( 1,  u,  v).normalized();
            dirs << QVector3D(-1,  u,  v).normalized();
            dirs << QVector3D( u,  1,  v).normalized();
            dirs << QVector3D( u, -1,  v).normalized();
            dirs << QVector3D( u,  v,  1).normalized();
            dirs << QVector3D( u,  v, -1).normalized();
        }
    }
    return dirs;
}

QVector3D rotate(const QQuaternion &q, const QVector3D &v)
{
    return q.rotatedVector(v);
}

} // namespace

DropSim::DropSim()
    : m_state(Idle)
    , m_mass(1.0f)
    , m_invMass(1.0f)
    , m_returnT(0.0f)
    , m_radius(1.0f)
    , m_gravityDir(0.0f, -1.0f, 0.0f)
    , m_gravityAccel(1.0f)
    , m_boxMin(-1.0f, -1.0f, -2.0f)
    , m_boxMax(1.0f, 1.0f, 0.0f)
    , m_restitution(kRestitution)
    , m_linearDamping(0.015f)
    , m_fallRate(kFallRate)
    , m_friction(kFriction)
    , m_angularDamping(0.5f)
    , m_releaseSpin(kReleaseSpinRate)
    , m_maxAngularRate(kMaxAngularRate)
    , m_torqueShare(kTrampolineTorqueShare)
    , m_bounceSpinKeep(0.72f)
    , m_frontOpen(false)
    , m_frontOverrunMetres(2.0f)
    , m_metre(1.0f)
    , m_frontDistance(0.0f)
    , m_frontZ(-1.0f)
    , m_backZ(-2.0f)
    , m_trampoline(false)
    , m_trampolineRestitution(0.95f)
    , m_onTrampoline(false)
    , m_asleep(false)
    , m_restTimer(0.0f)
{
    m_inertia = QVector3D(1, 1, 1);
    m_invInertia = QVector3D(1, 1, 1);
}

void DropSim::setScene(const ScenePtr &scene)
{
    m_contactPoints.clear();
    if (scene.isNull() || scene->isEmpty()) {
        m_centre = QVector3D();
        m_radius = 1.0f;
        return;
    }

    QVector3D lo, hi;
    scene->visibleBounds(&lo, &hi);
    m_centre = (lo + hi) * 0.5f;

    const QVector3D size = hi - lo;
    m_radius = qMax(size.length() * 0.5f, 1e-4f);

    // Mass is arbitrary — only ratios matter in a gravity-only simulation — but
    // scaling it with volume keeps the inertia numbers in a sane float range.
    m_mass = qMax(size.x() * size.y() * size.z(), 1e-6f);
    m_invMass = 1.0f / m_mass;

    // Solid-box inertia about the centre of mass.
    const float w = qMax(size.x(), 1e-4f);
    const float h = qMax(size.y(), 1e-4f);
    const float d = qMax(size.z(), 1e-4f);
    m_inertia = QVector3D(m_mass * (h * h + d * d) / 12.0f,
                          m_mass * (w * w + d * d) / 12.0f,
                          m_mass * (w * w + h * h) / 12.0f);
    m_invInertia = QVector3D(1.0f / m_inertia.x(),
                             1.0f / m_inertia.y(),
                             1.0f / m_inertia.z());

    buildContactPoints(scene);
}

void DropSim::buildContactPoints(const ScenePtr &scene)
{
    const QVector<QVector3D> dirs = sampleDirections();
    QVector<float> best(dirs.size(), -std::numeric_limits<float>::max());
    QVector<QVector3D> found(dirs.size());
    bool any = false;

    // One pass over the visible vertices, keeping the furthest vertex along
    // each sample direction. That is the support point of the convex hull in
    // that direction, so we get the hull's corners without building a hull.
    for (int li = 0; li < scene->layers.size(); ++li) {
        const SceneLayer &L = scene->layers.at(li);
        if (!L.visible || L.isEmpty())
            continue;
        for (int v = 0; v + kFloatsPerVertex <= L.vertices.size(); v += kFloatsPerVertex) {
            const QVector3D p(L.vertices.at(v), L.vertices.at(v + 1), L.vertices.at(v + 2));
            const QVector3D rel = p - m_centre;
            for (int d = 0; d < dirs.size(); ++d) {
                const float proj = QVector3D::dotProduct(rel, dirs.at(d));
                if (proj > best[d]) {
                    best[d] = proj;
                    found[d] = rel;
                }
            }
            any = true;
        }
    }
    if (!any)
        return;

    // The same vertex is the extreme point for many directions, so deduplicate;
    // duplicated contacts would each apply their own impulse and make the body
    // behave as if that corner were far heavier than the rest.
    const float mergeDist = m_radius * 0.02f;
    for (int d = 0; d < found.size(); ++d) {
        bool dup = false;
        for (int k = 0; k < m_contactPoints.size(); ++k) {
            if ((m_contactPoints.at(k) - found.at(d)).lengthSquared() < mergeDist * mergeDist) {
                dup = true;
                break;
            }
        }
        if (!dup)
            m_contactPoints.append(found.at(d));
    }
}

void DropSim::setViewport(float distance, float fovDeg, float aspect,
                          float depthScale, float widthScale)
{
    if (distance <= 0.0f || aspect <= 0.0f)
        return;

    const float tanV = qTan(qDegreesToRadians(fovDeg * 0.5f));

    /*
      The room's cross-section is measured at the FRONT wall, not at the camera,
      whenever the caller has pinned a front.

      That is what keeps everything inside the room on screen. The visible area
      narrows towards the camera, so a box sized at the camera's own distance is
      wider than the picture anywhere nearer than that — its floor lies below
      the bottom edge, and a part resting on it drops out of sight. Sized at the
      front wall instead, the box is at its widest exactly where the picture is
      narrowest, and every deeper point is comfortably inside.
    */
    const float sizingDistance = m_frontDistance > 0.0f ? m_frontDistance : distance;
    const float screenHalfH = sizingDistance * tanV;
    const float widen = qMax(1.0f, widthScale);

    // Depth is measured from the SCREEN height, not the widened one, so making
    // the shaft roomier does not also make it longer.
    const float halfD = qMax(kRoomDepthOfHeight * screenHalfH,
                             m_radius * kRoomMinDepthRadii);

    // The glass. Either pinned by the caller — at the depth where the model
    // exactly fills the frame — or half a room-depth in front of the model.
    m_frontZ = m_frontDistance > 0.0f ? -m_frontDistance : -distance + halfD;
    m_backZ = m_frontZ - 2.0f * halfD * qMax(1.0f, depthScale);

    /*
      Parallel sides, sized at the camera distance.

      A room whose walls FOLLOW THE FRUSTUM was tried, so that its edges would
      be the screen's edges at every depth and nothing inside it could ever be
      off screen. It is wrong for two reasons, both of them geometric rather
      than a matter of tuning:

        - its floor is not level. That plane runs from the eye out to the bottom
          edge of the picture, so it slopes away downhill, and gravity — which
          points straight down — has a component along it. The part slides down
          the slope into the distance and shrinks away to nothing.
        - its side walls are invisible. Their planes contain the eye, so they
          project to lines and have no area on screen at all.

      A level floor is worth the price: near the camera the box's floor is below
      the bottom of the picture, so a part that comes right forward can be
      clipped a little.
    */
    const float hw = screenHalfH * aspect * widen;
    const float hh = screenHalfH * widen;
    m_frontHalf = QVector3D(hw, hh, 0.0f);
    m_backHalf = m_frontHalf;

    // An axis-aligned hull of the room, for the far clip plane and for framing.
    const float hx = qMax(m_frontHalf.x(), m_backHalf.x());
    const float hy = qMax(m_frontHalf.y(), m_backHalf.y());
    m_boxMin = QVector3D(-hx, -hy, m_backZ);
    m_boxMax = QVector3D(hx, hy, m_frontZ);

    // One "metre" is one base room depth, which is what makes a room built
    // with depthScale 4 exactly "4 m" deep.
    m_metre = 2.0f * halfD;

    m_gravityAccel = m_fallRate * screenHalfH;

    buildWalls();
}

/*
  The six walls, as inward-facing planes. A point is inside when
  dot(n, p) >= offset, so the penetration depth of an outside point is simply
  offset - dot(n, p) — one expression that works for a parallel box and a
  frustum alike, which is why the room is described as two rectangles rather
  than as a min/max pair.
*/
void DropSim::buildWalls()
{
    const float zf = m_frontZ, zb = m_backZ;
    const float dz = zb - zf;                       // negative: further away

    // --- left and right, from the corresponding edges of the two rectangles
    {
        const float x1 = -m_frontHalf.x(), x2 = -m_backHalf.x();
        QVector3D n(-dz, 0.0f, x2 - x1);            // inward = +x
        n.normalize();
        m_walls[0].n = n;
        m_walls[0].offset = QVector3D::dotProduct(n, QVector3D(x1, 0.0f, zf));
    }
    {
        const float x1 = m_frontHalf.x(), x2 = m_backHalf.x();
        QVector3D n(dz, 0.0f, -(x2 - x1));          // inward = -x
        n.normalize();
        m_walls[1].n = n;
        m_walls[1].offset = QVector3D::dotProduct(n, QVector3D(x1, 0.0f, zf));
    }
    // --- floor and ceiling
    {
        const float y1 = -m_frontHalf.y(), y2 = -m_backHalf.y();
        QVector3D n(0.0f, -dz, y2 - y1);            // inward = +y
        n.normalize();
        m_walls[2].n = n;
        m_walls[2].offset = QVector3D::dotProduct(n, QVector3D(0.0f, y1, zf));
    }
    {
        const float y1 = m_frontHalf.y(), y2 = m_backHalf.y();
        QVector3D n(0.0f, dz, -(y2 - y1));          // inward = -y
        n.normalize();
        m_walls[3].n = n;
        m_walls[3].offset = QVector3D::dotProduct(n, QVector3D(0.0f, y1, zf));
    }
    // --- far end (the trampoline) and the glass
    m_walls[4].n = QVector3D(0.0f, 0.0f, 1.0f);
    m_walls[4].offset = zb;
    m_walls[5].n = QVector3D(0.0f, 0.0f, -1.0f);
    m_walls[5].offset = -zf;
}

void DropSim::setFrontDistance(float distance)
{
    m_frontDistance = qMax(0.0f, distance);
}

void DropSim::setWallVelocity(const QVector3D &v)
{
    m_wallVelocity = v;
}

void DropSim::setFrontOpen(bool open, float overrunMetres)
{
    m_frontOpen = open;
    m_frontOverrunMetres = qMax(0.0f, overrunMetres);
}

void DropSim::setFallRate(float rate)          { m_fallRate = qMax(0.1f, rate); }
void DropSim::setFriction(float mu)            { m_friction = qBound(0.0f, mu, 2.0f); }
void DropSim::setAngularDamping(float v)       { m_angularDamping = qMax(0.0f, v); }
void DropSim::setReleaseSpin(float v)          { m_releaseSpin = qMax(0.0f, v); }
void DropSim::setMaxAngularRate(float v)       { m_maxAngularRate = qMax(0.05f, v); }
void DropSim::setTorqueShare(float v)          { m_torqueShare = qBound(0.0f, v, 1.0f); }
void DropSim::setBounceSpinKeep(float v)       { m_bounceSpinKeep = qBound(0.0f, v, 1.0f); }

void DropSim::setLinearDamping(float perSecond)
{
    m_linearDamping = qMax(0.0f, perSecond);
}

void DropSim::setRestitution(float e)
{
    m_restitution = qBound(0.0f, e, 0.98f);
}

void DropSim::setTrampoline(bool enabled, float restitution)
{
    m_trampoline = enabled;
    m_trampolineRestitution = qBound(0.0f, restitution, 0.99f);
    m_onTrampoline = false;
}

/*
  The trampoline sheet. Fires once, on the step the body first touches the far
  end of the shaft, and acts on the centre of mass rather than on whichever
  corner happened to arrive first — see the note in the header for why.

  The rebound is measured against the WALL's velocity, so pulling the phone
  upwards as the part lands means the sheet is rising to meet it and the part
  leaves faster than it arrived. That is the racket stroke.
*/
void DropSim::applyTrampoline()
{
    if (!m_trampoline)
        return;

    // The far end of the shaft, whose inward normal points back towards the
    // viewer.
    const QVector3D n = m_walls[4].n;
    const float offset = m_walls[4].offset;

    // Where the body is touching the sheet. The centroid of the touching
    // corners, not the first one found: a part landing on an edge presses the
    // fabric down along that whole edge, and the push comes back from the
    // middle of the dent.
    QVector3D centroid;
    int touchCount = 0;
    const QQuaternion q = m_orientation;
    for (int ci = 0; ci < m_contactPoints.size(); ++ci) {
        const QVector3D r = rotate(q, m_contactPoints.at(ci));
        if (QVector3D::dotProduct(n, m_position + r) <= offset) {
            centroid += r;
            ++touchCount;
        }
    }

    if (touchCount == 0) {
        m_onTrampoline = false;
        return;
    }
    if (m_onTrampoline)
        return;          // already handled this landing
    m_onTrampoline = true;

    centroid /= float(touchCount);

    // Approach speed of the body as a whole, relative to the sheet.
    const float vn = QVector3D::dotProduct(m_velocity - m_wallVelocity, n);
    if (vn >= 0.0f)
        return;          // already moving away

    // --- the rebound --------------------------------------------------------
    // The linear part is applied to the centre of mass so the whole body comes
    // back, regardless of which corner arrived first. That is what a sheet
    // does, and it is what makes the part return to full height instead of
    // spending its energy spinning on the floor.
    const float dv = -(1.0f + m_trampolineRestitution) * vn;
    m_velocity += n * dv;

    // --- the tumble ---------------------------------------------------------
    // The contact is still off centre, so there IS a torque; a sheet just does
    // not deliver all of it the way a rigid corner would. Taking a fraction of
    // it keeps the part turning as it flies — and because the torque axis is
    // the cross product of the contact offset with the sheet normal, a landing
    // on a different corner spins it about a different axis, so successive
    // bounces tumble it around all three.
    //
    // Applying NONE of it, which is what this did at first, produced a part
    // that hopped straight up and down without ever rotating. Physically tidy,
    // and completely lifeless.
    const float impulse = dv / m_invMass;      // the linear impulse just applied
    const QVector3D torque = QVector3D::crossProduct(centroid, n * impulse);
    const QQuaternion qc = q.conjugated();
    const QVector3D tb = rotate(qc, torque * m_torqueShare);
    m_angularVelocity += rotate(q, QVector3D(tb.x() * m_invInertia.x(),
                                             tb.y() * m_invInertia.y(),
                                             tb.z() * m_invInertia.z()));

    // Take a real bite out of the spin it arrived with. The sheet absorbs
    // rotation far more readily than a hard floor does — that is what stops
    // successive bounces compounding into a whirl.
    m_angularVelocity *= m_bounceSpinKeep;
}

void DropSim::start(const QQuaternion &orientation, const QVector3D &position)
{
    // The caller hands us the pose that reproduces the camera's current view,
    // so the first simulated frame is identical to the last camera-rendered
    // one. An identity orientation would be wrong: the camera's view matrix
    // carries the orbit's yaw and pitch, and ignoring it snapped the model
    // upright the instant the drop began.
    m_position = position;
    m_orientation = orientation;
    m_velocity = QVector3D();
    m_angularVelocity = kReleaseSpin.normalized() * m_releaseSpin;

    m_startPosition = m_position;
    m_startOrientation = m_orientation;

    m_asleep = false;
    m_onTrampoline = false;
    m_restTimer = 0.0f;
    m_state = Falling;
}

void DropSim::returnToStart(const QQuaternion &orientation, const QVector3D &position)
{
    if (m_state != Falling)
        return;
    // Target the pose the camera will resume at, which is not necessarily the
    // pose the drop began from — the caller restores its zoom first.
    m_startPosition = position;
    m_startOrientation = orientation;
    m_returnFromPosition = m_position;
    m_returnFromOrientation = m_orientation;
    m_returnT = 0.0f;
    m_state = Returning;
}

void DropSim::reset()
{
    m_state = Idle;
    m_asleep = false;
    m_onTrampoline = false;
    m_restTimer = 0.0f;
    m_velocity = QVector3D();
    m_angularVelocity = QVector3D();
}

void DropSim::setGravityDirection(const QVector3D &dir)
{
    if (dir.lengthSquared() < 1e-6f)
        return;
    const QVector3D n = dir.normalized();
    // Tilting the phone is the user interacting: if the body had gone to sleep
    // on a face, a meaningful change of direction has to wake it, otherwise it
    // would stay glued in place however far the phone is turned.
    if (m_asleep && QVector3D::dotProduct(n, m_gravityDir) < 0.995f) {
        m_asleep = false;
        m_restTimer = 0.0f;
    }
    m_gravityDir = n;
}

void DropSim::step(float dt)
{
    if (m_state == Idle)
        return;

    if (m_state == Returning) {
        // Ease back to the starting pose, then hand control to the camera again.
        // Interpolating from the pose held when the button was pressed (not from
        // the live pose) keeps the motion a clean arc instead of a curve that
        // shortens itself every frame.
        const float duration = 0.55f;
        m_returnT = qMin(1.0f, m_returnT + dt / duration);
        const float t = m_returnT * m_returnT * (3.0f - 2.0f * m_returnT);  // smoothstep

        m_position = m_returnFromPosition * (1.0f - t) + m_startPosition * t;
        m_orientation = QQuaternion::slerp(m_returnFromOrientation, m_startOrientation, t);

        if (m_returnT >= 1.0f) {
            m_state = Idle;
            m_velocity = QVector3D();
            m_angularVelocity = QVector3D();
        }
        return;
    }

    if (m_asleep)
        return;

    float remaining = qMin(dt, kMaxFrameTime);
    while (remaining > 0.0f) {
        const float h = qMin(remaining, kFixedDt);
        integrate(h);
        applyTrampoline();
        solveContacts(h);
        remaining -= h;
    }

    // Sleep test. Both thresholds scale with the model so they mean the same
    // thing for a 5 mm part and a 500 mm one.
    // Relative to the speed reached falling the box's half height, so a part
    // is judged "still" by the same standard whatever the model's units are.
    const float refSpeed = qSqrt(2.0f * m_gravityAccel * (0.5f * (m_boxMax.y() - m_boxMin.y())));
    const float linThresh = kSleepLinear * refSpeed;
    if (m_velocity.length() < linThresh && m_angularVelocity.length() < kSleepAngular) {
        m_restTimer += dt;
        if (m_restTimer > kSleepTime) {
            m_asleep = true;
            m_velocity = QVector3D();
            m_angularVelocity = QVector3D();
        }
    } else {
        m_restTimer = 0.0f;
    }
}

void DropSim::integrate(float dt)
{
    m_velocity += m_gravityDir * m_gravityAccel * dt;

    // Damping stands in for air resistance and keeps a body that should be
    // settling from drifting for ever.
    //
    // The LINEAR term is deliberately tiny. A round trip down the trampoline
    // shaft and back takes about two seconds, so even a modest 0.06/s would eat
    // 11% of the speed — a fifth of the rebound height — and quietly turn a
    // 95% trampoline into a 71% one. Whatever restitution is set to should be
    // what you actually see. Angular damping can stay firm: it settles tumbling
    // without touching how high the part comes back.
    m_velocity *= (1.0f - m_linearDamping * dt);
    // Angular drag is firm on purpose: it settles the tumbling without touching
    // how high the part comes back, since height depends only on the linear
    // speed.
    m_angularVelocity *= (1.0f - m_angularDamping * dt);

    // ...and a hard ceiling, so no amount of accumulated contact torque can
    // turn a roll into a whirl.
    const float w = m_angularVelocity.length();
    if (w > m_maxAngularRate)
        m_angularVelocity *= m_maxAngularRate / w;

    m_position += m_velocity * dt;

    // Quaternion integration: dq/dt = 0.5 * omega_quat * q.
    const QQuaternion omega(0.0f, m_angularVelocity.x(), m_angularVelocity.y(),
                            m_angularVelocity.z());
    QQuaternion dq = omega * m_orientation;
    m_orientation = QQuaternion(m_orientation.scalar() + 0.5f * dt * dq.scalar(),
                                m_orientation.vector() + 0.5f * dt * dq.vector());
    m_orientation.normalize();
}

void DropSim::solveContacts(float dt)
{
    if (m_contactPoints.isEmpty())
        return;

    const QQuaternion q = m_orientation;
    const QQuaternion qc = q.conjugated();

    const float slop = kSlopRelative * m_radius;
    const float impactThreshold =
        0.06f * qSqrt(2.0f * m_gravityAccel * (0.5f * (m_boxMax.y() - m_boxMin.y())));

    const auto applyInvInertia = [&](const QVector3D &v) {
        const QVector3D b = rotate(qc, v);
        return rotate(q, QVector3D(b.x() * m_invInertia.x(),
                                   b.y() * m_invInertia.y(),
                                   b.z() * m_invInertia.z()));
    };
    const auto effectiveInvMass = [&](const QVector3D &r, const QVector3D &d) {
        const QVector3D ang = applyInvInertia(QVector3D::crossProduct(r, d));
        return m_invMass + QVector3D::dotProduct(d, QVector3D::crossProduct(ang, r));
    };

    /*
      One contact: a model corner against one wall.

      `targetVn` is the separating speed this contact must end the step with —
      the restitution goal. It is captured ONCE, from the approach speed before
      any impulse is applied, and then held fixed. Recomputing restitution from
      the current velocity inside the iteration loop (which is what this solver
      did at first) makes each iteration bounce off its own previous result, and
      with several corners landing together the impulses fight instead of
      converging. That is why the trampoline did not throw the part back.

      `pn`/`pt` accumulate the TOTAL impulse applied so far. Clamping the
      accumulated value rather than each increment is what lets a later
      iteration take back too large an earlier push. Without it the solver
      cannot correct an overshoot, and a body landing on a face has most of its
      impulse turned into spin — measured here, an impact at 4750 units/s came
      away still moving into the wall at 2910.
    */
    struct Contact {
        QVector3D r;
        QVector3D n;
        float depth;
        float invMassN;
        float targetVn;
        float pn;
        float pt;
    };

    QVector<Contact> contacts;
    contacts.reserve(8);

    for (int ci = 0; ci < m_contactPoints.size(); ++ci) {
        const QVector3D r = rotate(q, m_contactPoints.at(ci));
        const QVector3D p = m_position + r;

        for (int pi = 0; pi < 6; ++pi) {
            // Wall 5 is the glass we look through. When the room is open it is
            // not removed but MOVED OUT: the part can fly through it towards
            // the viewer, growing past the edges of the screen, and is caught a
            // couple of metres beyond by a wall nobody can see. Without that
            // catcher a good stroke would send it out of sight and the fall
            // back would take an age.
            float offset = m_walls[pi].offset;
            if (pi == 5 && m_frontOpen)
                offset -= m_frontOverrunMetres * m_metre;

            const float depth = offset - QVector3D::dotProduct(m_walls[pi].n, p);
            if (depth <= 0.0f)
                continue;

            Contact k;
            k.r = r;
            k.n = m_walls[pi].n;
            k.depth = depth;
            k.invMassN = effectiveInvMass(r, k.n);
            if (k.invMassN <= 0.0f)
                continue;

            // Relative to the WALL, which may be moving with the phone.
            const QVector3D vp = m_velocity - m_wallVelocity
                                 + QVector3D::crossProduct(m_angularVelocity, r);
            const float vn0 = QVector3D::dotProduct(vp, k.n);

            // Every wall here is hard, including the sides and the far end.
            // The trampoline is not a restitution value on a plane; it is the
            // central impulse in applyTrampoline(), already applied by now.
            k.targetVn = (vn0 < -impactThreshold) ? (-m_restitution * vn0) : 0.0f;
            k.pn = 0.0f;
            k.pt = 0.0f;
            contacts.append(k);
        }
    }

    if (contacts.isEmpty())
        return;

    // ---------------------------------------------------------------- pass 1
    // Velocity, with accumulated impulses. No positional bias here: mixing
    // penetration correction into the real velocity made a resting part twitch
    // between 3 and 150 units/s forever.
    for (int iter = 0; iter < kSolverIterations; ++iter) {
        for (int i = 0; i < contacts.size(); ++i) {
            Contact &k = contacts[i];

            const QVector3D vp = m_velocity - m_wallVelocity
                                 + QVector3D::crossProduct(m_angularVelocity, k.r);
            const float vn = QVector3D::dotProduct(vp, k.n);

            float dpn = (k.targetVn - vn) / k.invMassN;
            const float oldPn = k.pn;
            k.pn = qMax(0.0f, k.pn + dpn);   // clamp the TOTAL, not the increment
            dpn = k.pn - oldPn;
            if (!qFuzzyIsNull(dpn)) {
                m_velocity += k.n * (dpn * m_invMass);
                m_angularVelocity += applyInvInertia(QVector3D::crossProduct(k.r, k.n * dpn));
            }

            // --- friction, also accumulated -----------------------------
            // Without it the part slides down a tilted wall instead of tipping
            // over its edge, which is the behaviour asked for when the phone is
            // tilted too far.
            const QVector3D vp2 = m_velocity - m_wallVelocity
                                  + QVector3D::crossProduct(m_angularVelocity, k.r);
            const QVector3D vt = vp2 - k.n * QVector3D::dotProduct(vp2, k.n);
            if (vt.lengthSquared() < 1e-12f)
                continue;

            const QVector3D t = vt.normalized();
            const float invMassT = effectiveInvMass(k.r, t);
            if (invMassT <= 0.0f)
                continue;

            float dpt = -QVector3D::dotProduct(vp2, t) / invMassT;
            const float maxFriction = m_friction * k.pn;
            const float oldPt = k.pt;
            k.pt = qBound(-maxFriction, k.pt + dpt, maxFriction);
            dpt = k.pt - oldPt;
            if (!qFuzzyIsNull(dpt)) {
                m_velocity += t * (dpt * m_invMass);
                m_angularVelocity += applyInvInertia(QVector3D::crossProduct(k.r, t * dpt));
            }
        }
    }

    // ---------------------------------------------------------------- pass 2
    // Position, via "split impulse": the overlap is pushed out using SEPARATE
    // pseudo-velocities that move the body and are then thrown away. Nothing
    // here reaches the real velocity, so resting contacts cost no energy.
    QVector3D pseudoVel;
    QVector3D pseudoAng;
    QVector<float> pp(contacts.size(), 0.0f);
    bool corrected = false;

    for (int iter = 0; iter < kPositionIterations; ++iter) {
        for (int i = 0; i < contacts.size(); ++i) {
            const Contact &k = contacts.at(i);
            if (k.depth <= slop)
                continue;

            const QVector3D vp = pseudoVel + QVector3D::crossProduct(pseudoAng, k.r);
            const float vn = QVector3D::dotProduct(vp, k.n);
            const float bias = (kBaumgarte / dt) * (k.depth - slop);

            float dp = (bias - vn) / k.invMassN;
            const float oldP = pp[i];
            pp[i] = qMax(0.0f, pp[i] + dp);
            dp = pp[i] - oldP;
            if (qFuzzyIsNull(dp))
                continue;

            pseudoVel += k.n * (dp * m_invMass);
            pseudoAng += applyInvInertia(QVector3D::crossProduct(k.r, k.n * dp));
            corrected = true;
        }
    }

    if (corrected) {
        m_position += pseudoVel * dt;

        const QVector3D w = pseudoAng * dt;
        if (w.lengthSquared() > 1e-14f) {
            const QQuaternion spin(0.0f, w.x(), w.y(), w.z());
            const QQuaternion dq = spin * m_orientation;
            m_orientation = QQuaternion(m_orientation.scalar() + 0.5f * dq.scalar(),
                                        m_orientation.vector() + 0.5f * dq.vector());
            m_orientation.normalize();
        }
    }
}

QMatrix4x4 DropSim::modelViewMatrix() const
{
    QMatrix4x4 m;
    m.translate(m_position);
    m.rotate(m_orientation);
    // The vertices are stored around the model's own origin; rotation has to
    // happen about the centre of mass, so shift the geometry into place last.
    m.translate(-m_centre);
    return m;
}

QString DropSim::debugState() const
{
    const QVector3D centre = (m_boxMin + m_boxMax) * 0.5f;
    const QVector3D half = (m_boxMax - m_boxMin) * 0.5f;
    const QVector3D rel = m_position - centre;
    const QVector3D frac(half.x() > 0.0f ? rel.x() / half.x() : 0.0f,
                         half.y() > 0.0f ? rel.y() / half.y() : 0.0f,
                         half.z() > 0.0f ? rel.z() / half.z() : 0.0f);

    // Which walls the body is actually touching. This is the question that
    // matters: a part pinned to the back wall looks almost motionless from the
    // camera however hard the phone is tilted, and that is indistinguishable
    // from "the tilt is not working" unless you can see the contacts.
    QStringList walls;
    const QQuaternion q = m_orientation;
    const float touch = m_radius * 0.02f;
    bool hit[6] = { false, false, false, false, false, false };
    for (int ci = 0; ci < m_contactPoints.size(); ++ci) {
        const QVector3D p = m_position + q.rotatedVector(m_contactPoints.at(ci));
        for (int w = 0; w < 6; ++w)
            if (QVector3D::dotProduct(m_walls[w].n, p) - m_walls[w].offset <= touch)
                hit[w] = true;
    }
    const char *names[6] = { "left", "right", "floor", "ceil", "back", "front" };
    for (int i = 0; i < 6; ++i)
        if (hit[i]) walls << QLatin1String(names[i]);

    return QStringLiteral(
        "g=(%1 %2 %3) pos%=(%4 %5 %6) v=%7 w=%8 asleep=%9 pts=%10 box=(%11 %12 %13) walls=[%14]")
        .arg(double(m_gravityDir.x()), 0, 'f', 2)
        .arg(double(m_gravityDir.y()), 0, 'f', 2)
        .arg(double(m_gravityDir.z()), 0, 'f', 2)
        .arg(double(frac.x()), 0, 'f', 2)
        .arg(double(frac.y()), 0, 'f', 2)
        .arg(double(frac.z()), 0, 'f', 2)
        .arg(double(m_velocity.length()), 0, 'f', 1)
        .arg(double(m_angularVelocity.length()), 0, 'f', 2)
        .arg(m_asleep ? 1 : 0)
        .arg(m_contactPoints.size())
        .arg(double(half.x()), 0, 'f', 0)
        .arg(double(half.y()), 0, 'f', 0)
        .arg(double(half.z()), 0, 'f', 0)
        .arg(walls.join(QLatin1Char(',')));
}

/*
  harbour-3dmviewer — camera.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "render/camera.h"

#include <QMatrix3x3>
#include <QtMath>

namespace {
// Just short of the pole. At exactly 90 degrees the view direction and the up
// vector are parallel and QMatrix4x4::lookAt() produces a singular matrix.
const float kMaxPitch = 89.5f;
const QVector3D kUp(0.0f, 0.0f, 1.0f);   // Rhino is Z-up
}

Camera::Camera()
    : m_target(0.0f, 0.0f, 0.0f)
    , m_distance(10.0f)
    , m_yawDeg(-45.0f)
    , m_pitchDeg(25.0f)     // a mild three-quarter view, like Rhino's default
    , m_fovDeg(35.0f)
    , m_aspect(1.0f)
    , m_sceneRadius(1.0f)
    , m_farOverride(0.0f)
    , m_axisLocked(false)
    , m_rotationMode(Centred)
    , m_hasPivot(false)
    , m_pivot(0.0f, 0.0f, 0.0f)
{
}

QQuaternion Camera::orientation() const
{
    if (m_rotationMode == Free)
        return m_orientation;

    // The rotation lookAt() would build, written out rather than dug back out
    // of a 4x4.
    const QVector3D forward = (m_target - eye()).normalized();
    QVector3D right = QVector3D::crossProduct(forward, kUp);
    if (right.lengthSquared() < 1e-12f)
        right = QVector3D(1.0f, 0.0f, 0.0f);   // looking straight down the up axis
    right.normalize();
    const QVector3D up = QVector3D::crossProduct(right, forward).normalized();

    // Rows of a world -> camera rotation are the camera's axes, and the camera
    // looks down its own -Z.
    float v[9] = { right.x(), right.y(), right.z(),
                   up.x(),    up.y(),    up.z(),
                   -forward.x(), -forward.y(), -forward.z() };
    return QQuaternion::fromRotationMatrix(QMatrix3x3(v));
}

void Camera::setRotationMode(RotationMode mode)
{
    if (mode == m_rotationMode)
        return;

    if (mode == Free) {
        // Enter free rotation exactly where the turntable left off, or the
        // model would jump the moment the button is pressed.
        m_orientation = orientation();
    } else if (m_rotationMode == Free) {
        // Folding the attitude back into yaw and pitch drops the roll, which is
        // the point: it is what puts a tumbled model straight again.
        const QVector3D dir =
            m_orientation.conjugated().rotatedVector(QVector3D(0.0f, 0.0f, 1.0f));
        const float sinPitch = qBound(-1.0f, dir.z(), 1.0f);
        m_pitchDeg = qBound(-kMaxPitch,
                            float(qRadiansToDegrees(qAsin(sinPitch))),
                            kMaxPitch);
        m_yawDeg = float(qRadiansToDegrees(qAtan2(dir.y(), dir.x())));
    }

    m_rotationMode = mode;

    if (mode == Centred)
        clearPivot();
}

void Camera::setPivot(const QVector3D &worldPoint)
{
    m_pivot = worldPoint;
    m_hasPivot = true;
}

void Camera::clearPivot()
{
    m_hasPivot = false;
}

void Camera::keepPivotFixed(const QQuaternion &before)
{
    if (!m_hasPivot)
        return;

    /*
      V = translate(0,0,-distance) * R * translate(-target), so a point keeps
      its place on screen exactly when R * (P - target) is unchanged. Solving
      for the new target is exact: no iteration, no drift.
    */
    const QQuaternion after = orientation();
    const QVector3D inCamera = before.rotatedVector(m_pivot - m_target);
    m_target = m_pivot - after.conjugated().rotatedVector(inCamera);
}

void Camera::rotateFree(float dRightDeg, float dUpDeg)
{
    if (m_rotationMode != Free)
        return;

    const QQuaternion before = m_orientation;

    // Composed on the LEFT, in camera space: the model follows the finger
    // whatever attitude it is in. On the right it would turn about its own
    // axes and stop matching the finger after a few drags.
    const QQuaternion turn =
        QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, dRightDeg)
        * QQuaternion::fromAxisAndAngle(1.0f, 0.0f, 0.0f, dUpDeg);
    m_orientation = (turn * m_orientation).normalized();

    keepPivotFixed(before);
}

void Camera::roll(float dDeg)
{
    if (m_rotationMode != Free)
        return;

    const QQuaternion before = m_orientation;
    m_orientation =
        (QQuaternion::fromAxisAndAngle(0.0f, 0.0f, 1.0f, dDeg) * m_orientation)
            .normalized();
    keepPivotFixed(before);
}

void Camera::screenRay(float nx, float ny, QVector3D *origin, QVector3D *direction) const
{
    const QQuaternion r = orientation();
    const QQuaternion inv = r.conjugated();

    const float tanV = qTan(qDegreesToRadians(m_fovDeg * 0.5f));
    // Camera space: -Z ahead, x right, y up.
    const QVector3D dirCam(nx * tanV * m_aspect, ny * tanV, -1.0f);

    if (origin)
        *origin = eye();
    if (direction)
        *direction = inv.rotatedVector(dirCam).normalized();
}

QVector3D Camera::screenPointOnPlane(float nx, float ny, const QVector3D &plane) const
{
    QVector3D o, d;
    screenRay(nx, ny, &o, &d);

    const QQuaternion inv = orientation().conjugated();
    const QVector3D normal = inv.rotatedVector(QVector3D(0.0f, 0.0f, 1.0f));

    const float denom = QVector3D::dotProduct(normal, d);
    if (qAbs(denom) < 1e-6f)
        return plane;   // ray parallel to the plane; nothing sensible to say
    const float t = QVector3D::dotProduct(normal, plane - o) / denom;
    if (t <= 0.0f)
        return plane;   // behind the camera
    return o + d * t;
}

void Camera::stepAxis(int which)
{
    // A locked view is square-on, so it is also the way out of a tumbled one.
    setRotationMode(Centred);

    if (!m_axisLocked) {
        // First press only squares the view up. Without this the plain front
        // view could never be reached, since every press would turn away from it.
        m_axisLocked = true;
        m_axisRotation = QQuaternion();
        return;
    }

    QVector3D axis;
    switch (which) {
    case StepX: axis = QVector3D(0.0f, 0.0f, 1.0f); break;   // world Z
    case StepY: axis = QVector3D(0.0f, 1.0f, 0.0f); break;   // world Y
    case StepZ: axis = QVector3D(1.0f, 0.0f, 0.0f); break;   // world X
    default: return;
    }

    // Left multiplication: the turn is about the WORLD axis, not the model's
    // own already-turned one.
    m_axisRotation = QQuaternion::fromAxisAndAngle(axis, 90.0f) * m_axisRotation;
    m_axisRotation.normalize();
}

void Camera::setFreeOrbit()
{
    m_axisLocked = false;
    m_axisRotation = QQuaternion();
}

void Camera::setAspect(float aspect)
{
    if (aspect > 0.0f)
        m_aspect = aspect;
}

void Camera::setDistance(float d)
{
    if (d > 0.0f)
        m_distance = d;
}

void Camera::orbit(float dYawDeg, float dPitchDeg)
{
    // Free rotation is steered by rotateFree().
    if (m_rotationMode == Free)
        return;

    const QQuaternion before = orientation();

    m_yawDeg += dYawDeg;
    // Keep yaw bounded so it cannot drift into float ranges where the sine
    // loses precision after a few thousand drags.
    if (m_yawDeg > 180.0f)  m_yawDeg -= 360.0f;
    if (m_yawDeg < -180.0f) m_yawDeg += 360.0f;

    m_pitchDeg = qBound(-kMaxPitch, m_pitchDeg + dPitchDeg, kMaxPitch);

    keepPivotFixed(before);
}

void Camera::zoomBy(float factor)
{
    if (factor <= 0.0f)
        return;
    // Clamped to a window around the model: closer than a thousandth of the
    // model radius is inside the geometry, further than 100x is a dot.
    m_distance = qBound(m_sceneRadius * 0.01f,
                        m_distance / factor,
                        m_sceneRadius * 100.0f);
}

QVector3D Camera::eye() const
{
    // Square-on from the front. The model is turned instead of the camera, so
    // the eye never moves in a locked view.
    if (m_axisLocked)
        return m_target + QVector3D(0.0f, -1.0f, 0.0f) * m_distance;

    // Free rotation has no yaw and pitch; the attitude is primary.
    if (m_rotationMode == Free) {
        return m_target + m_orientation.conjugated().rotatedVector(
                              QVector3D(0.0f, 0.0f, m_distance));
    }

    const float yaw = qDegreesToRadians(m_yawDeg);
    const float pitch = qDegreesToRadians(m_pitchDeg);
    const float cp = qCos(pitch);
    const QVector3D dir(cp * qCos(yaw), cp * qSin(yaw), qSin(pitch));
    return m_target + dir * m_distance;
}

void Camera::pan(float dxPixels, float dyPixels, float viewportHeightPixels)
{
    if (viewportHeightPixels <= 0.0f)
        return;

    // World units covered by the full viewport height at the target's depth.
    const float worldPerScreen =
        2.0f * m_distance * qTan(qDegreesToRadians(m_fovDeg * 0.5f)) / viewportHeightPixels;

    const QVector3D forward = (m_target - eye()).normalized();
    QVector3D right;
    QVector3D up;

    if (m_axisLocked) {
        // The camera is fixed and the WORLD is turned, so the world direction
        // that appears as "right on screen" is the screen axis carried back
        // through the inverse of that turn. Without this the model would slide
        // sideways when dragged after a quarter turn.
        const QQuaternion inv = m_axisRotation.conjugated();
        right = inv.rotatedVector(QVector3D(1.0f, 0.0f, 0.0f));
        up = inv.rotatedVector(QVector3D(0.0f, 0.0f, 1.0f));
    } else if (m_rotationMode == Free) {
        // The camera's own axes, which after a roll are not the world's.
        const QQuaternion inv = m_orientation.conjugated();
        right = inv.rotatedVector(QVector3D(1.0f, 0.0f, 0.0f));
        up = inv.rotatedVector(QVector3D(0.0f, 1.0f, 0.0f));
    } else {
        right = QVector3D::crossProduct(forward, kUp);
        if (right.lengthSquared() < 1e-12f)
            right = QVector3D(1.0f, 0.0f, 0.0f);  // looking down the up axis
        right.normalize();
        up = QVector3D::crossProduct(right, forward).normalized();
    }

    // Dragging moves the MODEL with the finger, so the target moves opposite
    // to the drag on x, and with it on y (screen y grows downwards).
    m_target += right * (-dxPixels * worldPerScreen);
    m_target += up * (dyPixels * worldPerScreen);
}

void Camera::frame(const QVector3D &bbMin, const QVector3D &bbMax, float margin)
{
    if (bbMin.x() > bbMax.x())
        return;   // unset bounds: leave the camera alone

    m_target = (bbMin + bbMax) * 0.5f;

    float radius = (bbMax - bbMin).length() * 0.5f;
    if (radius < 1e-6f)
        radius = 1.0f;
    m_sceneRadius = radius;

    // Both half-angles. The horizontal one is derived from the vertical, and in
    // portrait it is much the tighter of the two — framing on the vertical
    // alone let a wide model run off the sides of the screen.
    const float halfFovV = qDegreesToRadians(m_fovDeg * 0.5f);
    const float tanV = qTan(halfFovV);
    const float tanH = m_aspect * tanV;

    // The camera's own axes at the current yaw/pitch.
    const QVector3D dir = (eye() - m_target).normalized();   // target -> eye
    QVector3D right = QVector3D::crossProduct(-dir, kUp);
    if (right.lengthSquared() < 1e-12f)
        right = QVector3D(1.0f, 0.0f, 0.0f);
    right.normalize();
    const QVector3D up = QVector3D::crossProduct(right, -dir).normalized();

    // In a locked view the world is turned before it is looked at, so the box
    // has to be measured after that turn — otherwise the framing would be right
    // only in the un-turned view and the model would run off the screen in the
    // other three.
    const bool turned = m_axisLocked && !m_axisRotation.isIdentity();

    // For a corner p, its depth in front of the eye is d - dot(p, dir), and it
    // is inside the frustum when |dot(p, right)| <= depth * tanH and likewise
    // vertically. Solving each for d and taking the largest gives the closest
    // the camera can be with every corner still on screen.
    float needed = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const QVector3D corner((i & 1) ? bbMax.x() : bbMin.x(),
                               (i & 2) ? bbMax.y() : bbMin.y(),
                               (i & 4) ? bbMax.z() : bbMin.z());
        QVector3D p = corner - m_target;
        if (turned)
            p = m_axisRotation.rotatedVector(p);
        const float along = QVector3D::dotProduct(p, dir);
        const float h = qAbs(QVector3D::dotProduct(p, right));
        const float v = qAbs(QVector3D::dotProduct(p, up));

        needed = qMax(needed, h / tanH + along);
        needed = qMax(needed, v / tanV + along);
    }

    if (needed <= 0.0f)
        needed = radius / qSin(qMin(halfFovV, float(qAtan(qreal(tanH)))));

    m_distance = needed * qMax(1.0f, margin);
}

void Camera::setStandardView(StandardView v, const QVector3D &bbMin, const QVector3D &bbMax)
{
    m_axisLocked = false;
    m_axisRotation = QQuaternion();
    switch (v) {
    case Top:         m_yawDeg = -90.0f; m_pitchDeg =  kMaxPitch; break;
    case Front:       m_yawDeg = -90.0f; m_pitchDeg =   0.0f;     break;
    case Right:       m_yawDeg =   0.0f; m_pitchDeg =   0.0f;     break;
    case Perspective: m_yawDeg = -45.0f; m_pitchDeg =  25.0f;     break;
    }
    frame(bbMin, bbMax);
}

float Camera::nearPlane() const
{
    // Scaled to the model, and never so small that depth precision collapses.
    return qMax(m_sceneRadius * 0.001f, m_distance * 0.005f);
}

void Camera::setFarOverride(float f)
{
    m_farOverride = f > 0.0f ? f : 0.0f;
}

void Camera::clearFarOverride()
{
    m_farOverride = 0.0f;
}

float Camera::farPlane() const
{
    const float normal = m_distance + m_sceneRadius * 10.0f;
    return m_farOverride > normal ? m_farOverride : normal;
}

QMatrix4x4 Camera::viewMatrix() const
{
    QMatrix4x4 m;

    if (m_rotationMode == Free && !m_axisLocked) {
        // Built directly: lookAt() cannot express roll, since it always takes
        // its up vector from the world.
        m.translate(0.0f, 0.0f, -m_distance);
        m.rotate(m_orientation);
        m.translate(-m_target);
        return m;
    }

    m.lookAt(eye(), m_target, kUp);
    if (m_axisLocked) {
        // Turn the world about the model's own centre, so a quarter turn never
        // walks the part off the screen.
        m.translate(m_target);
        m.rotate(m_axisRotation);
        m.translate(-m_target);
    }
    return m;
}

QMatrix4x4 Camera::projectionMatrix(float aspect) const
{
    QMatrix4x4 m;
    if (aspect <= 0.0f)
        aspect = 1.0f;
    m.perspective(m_fovDeg, aspect, nearPlane(), farPlane());
    return m;
}

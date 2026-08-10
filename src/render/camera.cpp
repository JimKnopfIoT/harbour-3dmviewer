/*
  harbour-3dmviewer — camera.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "render/camera.h"

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
{
}

void Camera::stepAxis(int which)
{
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
    m_yawDeg += dYawDeg;
    // Keep yaw bounded so it cannot drift into float ranges where the sine
    // loses precision after a few thousand drags.
    if (m_yawDeg > 180.0f)  m_yawDeg -= 360.0f;
    if (m_yawDeg < -180.0f) m_yawDeg += 360.0f;

    m_pitchDeg = qBound(-kMaxPitch, m_pitchDeg + dPitchDeg, kMaxPitch);
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

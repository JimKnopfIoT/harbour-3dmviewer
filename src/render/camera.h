/*
  harbour-3dmviewer — camera.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  A turntable camera, deliberately not an arcball.

  WHY TURNTABLE
  -------------
  An arcball is the "correct" free rotation, but on a phone it lets the model
  roll about the view axis, and once a CAD model is tilted sideways with no
  handle to straighten it, users are lost. A turntable keeps the model's up
  axis vertical on screen at all times, so "spin it round and look from a bit
  higher" — which is the whole interaction this app exists for — always works
  and is always recoverable.

  UP AXIS IS +Z
  -------------
  Rhino is a Z-up world. Using Y-up here would lay every imported model on its
  side. Pitch is clamped just short of the poles because at exactly ±90° the
  view direction becomes parallel to the up vector and the look-at matrix is
  degenerate.
*/
#ifndef CAMERA_H
#define CAMERA_H

#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>

class Camera
{
public:
    Camera();

    // Standard viewpoints, named as Rhino names them.
    enum StandardView { Top, Front, Right, Perspective };

    /*
      Locked, stepped views.

      In this mode the camera does not orbit at all: it sits square in front of
      the model and each press turns the MODEL a quarter turn about one fixed
      world axis. That is what makes the views behave like drawings — the ground
      stays the ground, and every state is one of the six square-on views or a
      quarter turn from it.

      The buttons are lettered by the screen direction the turn happens in, not
      by the world axis it uses:

        stepX  turns about world Z — the underside stays down, the four side
               views cycle past
        stepY  turns about world Y — the same face keeps facing you and spins
               in the plane of the screen
        stepZ  turns about world X — front, top, back, bottom

      The first press from free orbit only snaps square-on without turning;
      otherwise the plain front view would be unreachable.
    */
    enum { StepX = 0, StepY = 1, StepZ = 2 };

    void stepAxis(int which);
    void setFreeOrbit();
    bool axisLocked() const { return m_axisLocked; }

    void orbit(float dYawDeg, float dPitchDeg);
    // Pan is in screen pixels; the camera converts them to world units at the
    // target's depth so dragging feels the same whether zoomed in or out.
    void pan(float dxPixels, float dyPixels, float viewportHeightPixels);
    void zoomBy(float factor);

    // Frames the given box: sets the target to its centre and backs off far
    // enough that the box fits the vertical field of view with a small margin.
    // Frames the box so it just fits, with `margin` as slack (1.0 = the model
    // touches the screen edges).
    //
    // It fits the box's eight CORNERS, not its bounding sphere. A sphere is
    // easy but always too big: a part 85 x 99 x 138 has a bounding sphere 190
    // across, so framing on the sphere leaves the model filling barely half the
    // width no matter what margin is asked for.
    void frame(const QVector3D &bbMin, const QVector3D &bbMax, float margin = 1.15f);
    void setStandardView(StandardView v, const QVector3D &bbMin, const QVector3D &bbMax);

    // The viewport's width/height. frame() needs it: with only a vertical field
    // of view, a wide model framed in portrait runs off the sides of the screen.
    void setAspect(float aspect);
    float aspect() const { return m_aspect; }
    float fovDegrees() const { return m_fovDeg; }

    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix(float aspect) const;

    QVector3D eye() const;
    QVector3D target() const { return m_target; }
    float distance() const { return m_distance; }
    // Used by the drop simulation to pull back and give the model room to fall
    // in. Not clamped against the model like zoomBy(): the caller is setting a
    // deliberate framing, not responding to a pinch.
    void setDistance(float d);

    // Near/far are derived from the model size rather than fixed, because a
    // fixed 0.1..1000 range z-fights badly on a 200 mm part and clips a 50 m one.
    float nearPlane() const;
    float farPlane() const;

    // The trampoline room is a shaft several times deeper than the model, and
    // the default far plane is sized for the model — the falling part would
    // simply vanish partway down. This pushes the far plane out to cover it.
    void setFarOverride(float f);
    void clearFarOverride();

private:
    QVector3D m_target;
    float m_distance;
    float m_yawDeg;     // rotation about +Z
    float m_pitchDeg;   // elevation above the XY plane
    float m_fovDeg;
    float m_aspect;
    float m_sceneRadius; // remembered from the last frame() for near/far
    float m_farOverride; // 0 = derive from the model as usual
    bool m_axisLocked;
    // Turns applied to the world in the locked view. Composed on the LEFT, so
    // every press turns about a fixed world axis rather than about whichever
    // way the model happens to be facing — that is what keeps "the underside
    // stays down" true however many presses have gone before.
    QQuaternion m_axisRotation;
};

#endif // CAMERA_H

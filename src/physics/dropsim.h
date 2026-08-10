/*
  harbour-3dmviewer — dropsim.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  A single-rigid-body drop simulation: the loaded model becomes a free body,
  falls to the bottom of the screen, tumbles onto its real corners and settles.

  WHY A PURPOSE-BUILT SOLVER AND NOT AN ENGINE
  --------------------------------------------
  PhysX is NVIDIA's, proprietary, and has no ARM-Linux build; Bullet is not in
  the Jolla repositories. Both would also be wildly oversized for one body in a
  box. What we need is: integrate one body, collide it with six planes, and make
  it come to rest convincingly. That is a few hundred lines, and it lets the
  contact points be the model's OWN vertices — so the part tips over its actual
  corners instead of over a proxy hull.

  THE SIMULATION RUNS IN EYE SPACE, NOT WORLD SPACE
  -------------------------------------------------
  "Falls to the bottom edge of the screen" is a statement about the screen, not
  about the model's world axes. In eye space +X is right, +Y is up and -Z goes
  into the screen, so "down" is simply (0,-1,0) — no conversion, and it stays
  correct no matter how the camera was pointing when the drop started. It also
  matches the accelerometer, which reports gravity in the phone's own frame,
  i.e. the same frame as the screen.

  The box the body falls in is therefore an eye-space box sized to the viewport
  at the model's depth. Its floor is the bottom edge of the screen.
*/
#ifndef DROPSIM_H
#define DROPSIM_H

#include <QMatrix4x4>
#include <QString>
#include <QQuaternion>
#include <QVector>
#include <QVector3D>

#include "model/scene.h"

class DropSim
{
public:
    DropSim();

    // Builds the body from the scene: mass properties from the bounding box,
    // contact points sampled from the visible geometry. Cheap enough to call
    // whenever the model or its layer visibility changes.
    void setScene(const ScenePtr &scene);

    // The room the body lives in, in eye space. `distance` is how far the
    // camera is from the model and `fovDeg`/`aspect` describe the frustum;
    // together they give the on-screen extents at that depth.
    //
    // `depthScale` deepens the room BACKWARDS ONLY. That asymmetry is the whole
    // point: the room is centred on the camera's focus, so scaling it
    // symmetrically would push the front wall behind the viewer. Extending only
    // away from the camera gives a deep shaft to fall into — and because it is
    // a perspective view, falling into it makes the part visibly shrink.
    // `widthScale` widens the shaft's cross-section beyond the screen. It matters
    // more than it sounds: the model is framed to fill the screen, and the room
    // is measured from that same frame, so at 1.0 the part fills the tube like a
    // piston — it scrapes the side walls all the way down and the friction eats
    // exactly the energy that should have carried it back up.
    void setViewport(float distance, float fovDeg, float aspect,
                     float depthScale = 1.0f, float widthScale = 1.0f);

    // Pins the front wall to an explicit depth instead of deriving it from the
    // room's own thickness. Pass the distance at which the model exactly fills
    // the frame and "as far forward as it can go" becomes "as large as it can
    // be drawn, still whole" — which is not otherwise true: with the camera
    // pulled back 1.7x and the wall half a room-depth in front of the model,
    // the closest the part could ever get was 1.15x the fitting distance, so it
    // never reached full size. 0 restores the derived position.
    void setFrontDistance(float distance);

    // Velocity of the room itself, in model units per second, eye space. The
    // room is bolted to the phone, so swinging the phone moves the walls — and
    // a rebound is measured against the wall's motion, not against a fixed
    // world. Yanking the phone up as the part lands is therefore a racket
    // stroke: the floor rises into it and the part leaves faster than it came.
    void setWallVelocity(const QVector3D &v);

    // Leaves the front of the room open, so a hard enough stroke can send the
    // part out through the glass towards the viewer instead of bouncing it off
    // an invisible pane. `overrunMetres` is how far past the glass it may get
    // before an invisible wall stops it — without a limit a good stroke would
    // throw it out of sight and the fall back would take an age.
    //
    // "Metres" here are the same units the room's depth is quoted in: a room
    // built with depthScale 4 is "4 m" deep, so one metre is one base room
    // depth. The file's real units never enter into it.
    void setFrontOpen(bool open, float overrunMetres = 2.0f);

    // The knobs the tuning page drives. Each replaces what used to be a
    // compile-time constant; the defaults in PhysicsSettings are exactly those
    // former values, so nothing changes until a slider is moved.
    void setFallRate(float rate);          // gravity, as multiples of screen height
    void setFriction(float mu);
    void setAngularDamping(float perSecond);
    void setReleaseSpin(float radPerSec);
    void setMaxAngularRate(float radPerSec);
    void setTorqueShare(float share);      // trampoline twist
    void setBounceSpinKeep(float keep);    // spin surviving a bounce

    // Air drag, per second. Small values matter more than they look on a deep
    // shaft: a round trip there takes about two seconds, so 0.015/s quietly
    // costs a twentieth of the rebound height.
    void setLinearDamping(float perSecond);

    // How bouncy the ORDINARY walls are — the sides, the ceiling and the far
    // end. A CAD part on a hard surface keeps very little, around 0.1.
    void setRestitution(float e);

    // Turns the FAR END of the shaft — and only that one face — into a
    // trampoline of the given bounciness. Every other wall stays hard.
    //
    // WHY THE TRAMPOLINE IS NOT JUST A HIGH RESTITUTION ON THAT PLANE
    // A rigid body landing on a corner converts most of the impulse into spin,
    // not rebound: at the contact point the inertia term dominates the
    // effective mass roughly fourfold. That is correct for a brick hitting
    // concrete, and it is why simply raising the restitution left the part
    // spinning on the floor instead of coming back up. A real trampoline sheet
    // gives way, spreads the load and pushes the WHOLE body back, so this is
    // modelled as a central impulse on the centre of mass — which is both what
    // the fabric actually does and what makes it return to full height.
    void setTrampoline(bool enabled, float restitution);

    // The room as two rectangles, near and far, in eye space. A parallel box
    // and a frustum are both just "two rectangles at two depths", so the
    // renderer and the collision planes share one description.
    float roomFrontZ() const { return m_frontZ; }
    float roomBackZ() const { return m_backZ; }
    QVector3D roomFrontHalf() const { return m_frontHalf; }
    QVector3D roomBackHalf() const { return m_backHalf; }

    // Kept for the far clip plane and for framing maths.
    QVector3D boxMin() const { return m_boxMin; }
    QVector3D boxMax() const { return m_boxMax; }

    // Starts a drop from an explicit pose. The caller passes the pose that
    // reproduces the camera's current view exactly, so the first simulated
    // frame is identical to the last camera-rendered one and the model simply
    // begins to move. Passing an identity orientation here would snap the model
    // upright the instant the button is pressed.
    void start(const QQuaternion &orientation, const QVector3D &position);

    // Begins the animated flight back to `orientation`/`position`, which is the
    // pose the camera will resume rendering at. Passing it explicitly (rather
    // than reusing the pose the drop started from) is what lets the caller
    // restore a zoomed-out camera before the flight, so the handover at the end
    // is seamless instead of a jump in apparent size.
    void returnToStart(const QQuaternion &orientation, const QVector3D &position);
    void reset();

    // Gravity direction in eye space, unit length. (0,-1,0) is straight down
    // the screen. Fed from the accelerometer.
    void setGravityDirection(const QVector3D &dir);

    // Advances by dt seconds. Internally substepped, so a dropped frame cannot
    // let the body tunnel through the floor.
    void step(float dt);

    bool isActive() const { return m_state != Idle; }
    bool isReturning() const { return m_state == Returning; }
    // True once the body has stopped moving. The view can then stop asking for
    // repaints and let the GPU idle.
    bool isAsleep() const { return m_asleep; }

    // The simulation's gravity in model units per second squared. Callers need
    // it to convert real-world metres into model units: this value stands for
    // 9.81 m/s^2, so it is the bridge between the two worlds.
    float gravityAccel() const { return m_gravityAccel; }

    // Centre of mass in the model's own coordinates. The view needs it to work
    // out where the camera is currently putting the body.
    QVector3D modelCentre() const { return m_centre; }

    // One line describing what the body is doing right now, for the journal.
    // Position is reported as a fraction of the box half-extents, so -1..+1
    // says directly which wall it is resting against.
    QString debugState() const;

    // The body's pose as a modelview matrix: takes the model's own world-space
    // vertices straight to eye space. Replaces the camera's view matrix while
    // the simulation is running.
    QMatrix4x4 modelViewMatrix() const;

private:
    enum State { Idle, Falling, Returning };

    void integrate(float dt);
    void solveContacts(float dt);
    void applyTrampoline();
    void buildContactPoints(const ScenePtr &scene);
    void updateInertia();

    State m_state;

    // --- body ---------------------------------------------------------------
    // Position of the body's centre of mass in eye space, and its orientation
    // relative to the pose it started in.
    QVector3D m_position;
    QQuaternion m_orientation;
    QVector3D m_velocity;
    QVector3D m_angularVelocity;

    float m_mass;
    float m_invMass;
    // Diagonal inertia tensor in body space. A box approximation: the true mesh
    // tensor would need a solid-volume integral over triangles, and for how a
    // part tumbles in a box the difference is not visible.
    QVector3D m_inertia;
    QVector3D m_invInertia;

    // The pose the body started from and returns to.
    QVector3D m_startPosition;
    QQuaternion m_startOrientation;
    // The pose held at the moment the return was triggered — the fixed origin
    // of the return interpolation.
    QVector3D m_returnFromPosition;
    QQuaternion m_returnFromOrientation;
    float m_returnT;

    // Model-space offset from the geometry's origin to its centre of mass.
    // Rotation must happen about the centre of mass, but the vertices are
    // stored around the model's own origin, which is somewhere else entirely.
    QVector3D m_centre;

    // Contact candidates in body space, relative to the centre of mass.
    QVector<QVector3D> m_contactPoints;
    float m_radius;

    // --- environment --------------------------------------------------------
    QVector3D m_gravityDir;
    // Magnitude, in model units per second squared. Derived from the box rather
    // than fixed: the file's units are unknown (a part may be modelled in mm,
    // cm or inches) so any constant would be right for one model and wrong for
    // the rest. Scaling to the box makes the fall take the same TIME regardless.
    float m_gravityAccel;
    // The room as explicit corners rather than centre+extents, so its depth can
    // grow backwards without moving the front wall.
    QVector3D m_boxMin;
    QVector3D m_boxMax;
    float m_frontZ;
    float m_backZ;
    QVector3D m_frontHalf;
    QVector3D m_backHalf;

    // The six walls as inward-facing planes: a point is inside when
    // dot(normal, p) >= offset. Rebuilt whenever the room changes.
    struct Wall { QVector3D n; float offset; };
    Wall m_walls[6];
    void buildWalls();
    // Bounciness of the ordinary hard walls.
    float m_restitution;
    float m_linearDamping;
    float m_fallRate;
    float m_friction;
    float m_angularDamping;
    float m_releaseSpin;
    float m_maxAngularRate;
    float m_torqueShare;
    float m_bounceSpinKeep;
    bool m_trampoline;
    float m_trampolineRestitution;
    // True while the body is in contact with the trampoline, so the central
    // impulse fires once per landing instead of every substep.
    bool m_onTrampoline;
    QVector3D m_wallVelocity;
    bool m_frontOpen;
    float m_frontOverrunMetres;
    // One "metre" in model units: the depth of the base room.
    float m_metre;
    float m_frontDistance;

    bool m_asleep;
    float m_restTimer;
};

#endif // DROPSIM_H

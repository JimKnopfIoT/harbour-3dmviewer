/*
  harbour-3dmviewer — tiltsource.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Turns the accelerometer into a gravity direction expressed in the same frame
  the renderer uses for the screen, so tilting the phone tilts the world the
  dropped model lives in.

  WHY THE ACCELEROMETER AND NOT THE GYROSCOPE
  -------------------------------------------
  We do not want the RATE of turning, we want which way is down — an absolute
  quantity. A gyroscope only gives change and drifts; an accelerometer at rest
  measures exactly the direction of gravity and never drifts. Integrating a
  gyroscope here would mean the floor slowly rotating away under a part that is
  simply lying still.

  SIGN CONVENTION
  ---------------
  An accelerometer at rest reports proper acceleration, which points UP — hold
  the phone upright and it reads about +9.81 on the axis pointing at the sky.
  Gravity, the direction things fall, is therefore the NEGATED reading.

  Qt's device frame for a phone held upright in portrait is: +x right across the
  screen, +y up the screen, +z out of the screen towards the user. That is the
  same set of axes as eye space, so the negated reading can be used as an
  eye-space gravity direction with no conversion at all. Landscape is handled by
  rotating the vector by the UI orientation, because the eye-space axes follow
  the rotated UI while the sensor axes stay bolted to the hardware.
*/
#ifndef TILTSOURCE_H
#define TILTSOURCE_H

#include <QElapsedTimer>
#include <QObject>
#include <QVector3D>
#include <QtSensors/QAccelerometer>

class TiltSource : public QObject
{
    Q_OBJECT
    // Not CONSTANT: the real answer is only known after the first start(), see
    // the note in the constructor.
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    explicit TiltSource(QObject *parent = nullptr);

    bool available() const { return m_available; }
    bool active() const { return m_active; }

    // Unit vector, eye space. (0,-1,0) means "down the screen".
    QVector3D gravityDirection() const { return m_gravity; }

    // Takes the phone's CURRENT attitude to mean "screen upright", so that
    // gravity starts as exactly (0,-1,0) and every later tilt is measured from
    // there.
    //
    // WHY RELATIVE AND NOT ABSOLUTE GRAVITY
    // Nobody holds a phone vertical to look at it. Measured absolutely, gravity
    // then points mostly INTO the screen, so the model's floor is the back wall
    // and it lies flat against the glass — it cannot topple, because a rotation
    // on that wall turns about an axis aimed at the camera and merely looks like
    // sliding. The device log made this unmistakable: the body's depth sat at
    // -0.85 of the box for an entire session with "back" in the contact list on
    // nearly every line. Referencing the pose at the moment of the drop makes
    // however you happen to be holding the phone count as upright.
    void captureReference();

    // The phone's own linear velocity, in metres per second, in eye space.
    //
    // The accelerometer reports gravity and hand movement added together in one
    // vector. Subtracting the smoothed value — which IS the gravity estimate —
    // leaves the movement, and a leaky integral of that is a short-memory
    // velocity: accurate over the fraction of a second that a swing lasts, and
    // deliberately forgetful beyond it, because integrating accelerometer noise
    // for any longer drifts without bound.
    //
    // The simulation treats this as the velocity of the room's walls, so
    // yanking the phone upwards as the part lands means the floor is rising to
    // meet it and the rebound is measured against a moving surface. That is
    // exactly what a tennis racket does.
    QVector3D deviceVelocity() const { return m_deviceVelocity; }

    // Degrees the UI is rotated relative to the hardware's portrait frame
    // (0, 90, 180, 270). Silica reports this per page.
    void setUiRotation(int degrees);

    // Only run the sensor while a drop is in progress — an accelerometer left
    // polling in the background is a pointless battery drain in a file viewer.
    void start();
    void stop();

signals:
    void activeChanged();
    void availabilityChanged();
    void gravityChanged();

private slots:
    void onReadingChanged();

private:
    QAccelerometer *m_sensor;
    // Gravity as delivered to the simulation: relative to the reference pose.
    QVector3D m_gravity;
    // The same reading before referencing, in eye space.
    QVector3D m_rawEye;
    // The attitude that counts as "screen upright" for this drop.
    QVector3D m_reference;
    bool m_hasReference;
    // A reference can only be taken from a real reading, and the first one may
    // not have arrived when the drop starts — so the request is deferred.
    bool m_captureRequested;
    // Smoothed raw reading (m/s^2, eye space) = the gravity estimate. What is
    // left after subtracting it is hand movement.
    QVector3D m_lowPassRaw;
    bool m_haveLowPass;
    QVector3D m_deviceVelocity;
    QElapsedTimer m_clock;
    qint64 m_lastMs;
    bool m_available;
    bool m_active;
    int m_uiRotation;
};

#endif // TILTSOURCE_H

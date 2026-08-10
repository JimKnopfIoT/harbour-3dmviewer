/*
  harbour-3dmviewer — tiltsource.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "physics/tiltsource.h"

#include <QDebug>
#include <QQuaternion>
#include <QtMath>

// Set TILT_DEBUG=1 in the environment to start the accelerometer as soon as the
// view exists and log what it delivers. The sensor is normally only started by
// a drop, which needs a finger on the screen — this makes the sensor path
// diagnosable over ssh, where there is nobody to press the button.
static const bool kTiltDebug = !qEnvironmentVariableIsEmpty("TILT_DEBUG");

namespace {
// How strongly each new reading pulls the smoothed value. Raw accelerometer
// data on this hardware is noisy enough that an unfiltered gravity vector makes
// a resting part shiver; too much smoothing and the model lags behind the hand.
const float kSmoothing = 0.38f;
}

TiltSource::TiltSource(QObject *parent)
    : QObject(parent)
    , m_sensor(new QAccelerometer(this))
    , m_gravity(0.0f, -1.0f, 0.0f)
    , m_rawEye(0.0f, -1.0f, 0.0f)
    , m_reference(0.0f, -1.0f, 0.0f)
    , m_hasReference(false)
    , m_captureRequested(false)
    , m_haveLowPass(false)
    , m_lastMs(0)
    , m_available(false)
    , m_active(false)
    , m_uiRotation(0)
{
    // 20 ms is plenty: the simulation runs at a fixed 240 Hz internally and only
    // needs to know which way is down, which no hand changes faster than 50 Hz.
    m_sensor->setDataRate(50);
    // Keep delivering while the screen is on but the app is backgrounded — the
    // alternative is a sensor that silently stops and a model frozen mid-fall.
    m_sensor->setProperty("alwaysOn", false);

    connect(m_sensor, &QAccelerometer::readingChanged,
            this, &TiltSource::onReadingChanged);

    // Availability is NOT probed here. Sailfish brings sensorfw up lazily, and
    // an app that asks during construction gets
    //     "There is no sensor manager yet, do not initialize accelerometersensor"
    // and would latch "no accelerometer" for the rest of the session even
    // though the sensor works perfectly a second later. So assume it is there
    // and let the first start() settle the question.
    m_available = true;
}

void TiltSource::setUiRotation(int degrees)
{
    m_uiRotation = ((degrees % 360) + 360) % 360;
}

void TiltSource::start()
{
    if (m_active)
        return;

    m_sensor->start();
    m_active = m_sensor->isActive();

    if (kTiltDebug) {
        qWarning() << "TILT: identifier=" << m_sensor->identifier()
                   << "active=" << m_active
                   << "dataRate=" << m_sensor->dataRate()
                   << "connected=" << m_sensor->isConnectedToBackend();
        const qrangelist rates = m_sensor->availableDataRates();
        for (int i = 0; i < rates.size(); ++i)
            qWarning() << "TILT: available rate" << rates.at(i).first
                       << "-" << rates.at(i).second;
    }

    // Only now is the answer trustworthy. A device without a working
    // accelerometer leaves the simulation on its default gravity — straight
    // down the screen — which still falls, it just cannot be steered.
    const bool wasAvailable = m_available;
    m_available = m_active;

    if (m_available != wasAvailable)
        emit availabilityChanged();
    emit activeChanged();
}

void TiltSource::captureReference()
{
    m_deviceVelocity = QVector3D();
    m_lastMs = 0;
    m_clock.start();
    // Deferred: taken from the next reading, which may not have arrived yet.
    m_captureRequested = true;
    m_hasReference = false;
    m_gravity = QVector3D(0.0f, -1.0f, 0.0f);
}

void TiltSource::stop()
{
    if (!m_active)
        return;
    m_hasReference = false;
    m_captureRequested = false;
    m_sensor->stop();
    m_active = false;
    emit activeChanged();
}

void TiltSource::onReadingChanged()
{
    QAccelerometerReading *r = m_sensor->reading();
    if (!r)
        return;

    if (kTiltDebug) {
        static int n = 0;
        if ((n++ % 25) == 0)
            qWarning("TILT: raw x=%.2f y=%.2f z=%.2f  -> g=(%.2f %.2f %.2f) rot=%d",
                     r->x(), r->y(), r->z(),
                     double(m_gravity.x()), double(m_gravity.y()), double(m_gravity.z()),
                     m_uiRotation);
    }

    // The reading as measured, before any interpretation.
    QVector3D raw(float(r->x()), float(r->y()), float(r->z()));

    // At rest the reading points UP, so gravity is its negation.
    QVector3D g(-raw.x(), -raw.y(), -raw.z());
    if (g.lengthSquared() < 1e-4f)
        return;
    g.normalize();

    // The sensor axes are fixed to the hardware, but eye space follows the
    // rotated UI. Rotating the vector about the screen normal by the UI's own
    // rotation puts both back in the same frame, so "down the screen" stays
    // down the screen in landscape too.
    if (m_uiRotation != 0) {
        const float a = qDegreesToRadians(float(m_uiRotation));
        const float c = qCos(a), s = qSin(a);
        g = QVector3D(c * g.x() + s * g.y(), -s * g.x() + c * g.y(), g.z());
        raw = QVector3D(c * raw.x() + s * raw.y(), -s * raw.x() + c * raw.y(), raw.z());
    }

    // --- the phone's own movement ------------------------------------------
    // Low-pass the reading to isolate gravity, then the remainder is what the
    // hand is doing. Same smoothing constant as the direction, so the two
    // estimates stay consistent with one another.
    if (!m_haveLowPass) {
        m_lowPassRaw = raw;
        m_haveLowPass = true;
    } else {
        m_lowPassRaw = m_lowPassRaw * (1.0f - kSmoothing) + raw * kSmoothing;
    }

    if (m_clock.isValid()) {
        const qint64 now = m_clock.elapsed();
        const float dtSec = m_lastMs == 0 ? 0.0f : float(now - m_lastMs) / 1000.0f;
        m_lastMs = now;

        if (dtSec > 0.0f && dtSec < 0.2f) {
            const QVector3D linear = raw - m_lowPassRaw;   // m/s^2
            m_deviceVelocity += linear * dtSec;
            // Leak towards zero with a ~0.25 s time constant. Without this the
            // integral of sensor noise wanders off and the walls would appear
            // to drift for ever.
            m_deviceVelocity *= qMax(0.0f, 1.0f - dtSec / 0.25f);
        }
    }

    // Exponential smoothing, then renormalise: lerping two unit vectors gives a
    // shorter one, and the simulation multiplies this by a gravity magnitude.
    m_rawEye = (m_rawEye * (1.0f - kSmoothing) + g * kSmoothing);
    if (m_rawEye.lengthSquared() < 1e-6f)
        m_rawEye = g;
    else
        m_rawEye.normalize();

    if (m_captureRequested) {
        m_reference = m_rawEye;
        m_hasReference = true;
        m_captureRequested = false;
    }

    if (m_hasReference) {
        // Rotate "down the screen" by however far the phone has turned since the
        // reference was taken. Holding still therefore gives exactly (0,-1,0);
        // tilting sideways swings gravity sideways; tilting the display forward
        // or back swings it into or out of the screen, which is what lets the
        // part topple towards the back or the glass.
        const QQuaternion turn = QQuaternion::rotationTo(m_reference, m_rawEye);
        m_gravity = turn.rotatedVector(QVector3D(0.0f, -1.0f, 0.0f)).normalized();
    } else {
        m_gravity = m_rawEye;
    }

    emit gravityChanged();
}

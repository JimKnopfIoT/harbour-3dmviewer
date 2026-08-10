/*
  harbour-3dmviewer — physicssettings.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "physics/physicssettings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QtGlobal>
#include <QVariantMap>

namespace {

struct Param
{
    const char *key;
    const char *label;
    const char *unit;
    double min;
    double max;
    double step;
    int decimals;
    double defDrop;         // default on page 2
    double defTrampoline;   // default on page 3
    bool onDrop;
    bool onTrampoline;
};

/*
  Every default here is the value the corresponding constant had in the code
  when this page was built, so switching the page on changes nothing until a
  slider is moved.

  A note on `restitution` versus `trampoline`: restitution is a ratio of
  SPEEDS and rebound height goes with its square. 0.95 returns 90% of the
  height, not 95% — the single most misleading number on this page, hence the
  separate height reading in the label.
*/
const Param kParameters[] = {
    // key               label                    unit    min    max   step  dec   drop   tramp   p2     p3
    { "fallRate",        "Gravity",               "",     4.0,  40.0,  0.5,  1,   14.0,  14.0,  true,  true  },
    { "friction",        "Friction",              "",     0.0,   1.5,  0.02, 2,    0.62,  0.62, true,  true  },
    { "restitution",     "Wall bounce",           "",     0.0,   0.9,  0.02, 2,    0.12,  0.12, true,  true  },
    { "linearDamping",   "Air drag",              "/s",   0.0,   0.2,  0.005,3,    0.015, 0.005,true,  true  },
    { "angularDamping",  "Spin drag",             "/s",   0.0,   2.0,  0.05, 2,    0.5,   0.5,  true,  true  },
    { "releaseSpin",     "Spin on release",       "rad/s",0.0,   4.0,  0.05, 2,    0.85,  0.85, true,  true  },
    { "maxSpin",         "Spin limit",            "rad/s",0.5,   8.0,  0.1,  1,    2.0,   2.0,  true,  true  },
    { "revealSeconds",   "Pause before release",  "s",    0.0,   3.0,  0.1,  1,    1.0,   1.0,  true,  true  },
    { "zoomOut",         "Camera pull-back",      "x",    1.0,   3.0,  0.05, 2,    1.7,   1.0,  true,  true  },

    // Deeper room on page 3 only, the rest of this block with it.
    { "depth",           "Room depth",            "m",    1.0,  12.0,  0.5,  1,    2.0,   5.5,  true,  true  },
    { "width",           "Shaft width",           "x",    1.0,   4.0,  0.1,  1,    1.0,   2.2,  false, true  },
    { "trampoline",      "Trampoline rebound",    "",     0.0,  0.99,  0.005,3,    0.0,   0.99, false, true  },
    { "torqueShare",     "Trampoline twist",      "",     0.0,   0.5,  0.01, 2,    0.0,   0.07, false, true  },
    { "bounceSpinKeep",  "Spin kept on bounce",   "",     0.0,   1.0,  0.02, 2,    0.0,   0.72, false, true  },
    { "racketGain",      "Shake response",        "x",    0.0,  12.0,  0.25, 2,    2.5,   3.5,  true,  true  },
    { "frontOverrun",    "Fly-out limit",         "m",    0.0,   6.0,  0.25, 2,    0.0,   2.0,  false, true  },
};
const int kParameterCount = int(sizeof(kParameters) / sizeof(kParameters[0]));

bool appliesTo(const Param &p, int page)
{
    return page == PhysicsSettings::Trampoline ? p.onTrampoline : p.onDrop;
}

double defaultFor(const Param &p, int page)
{
    return page == PhysicsSettings::Trampoline ? p.defTrampoline : p.defDrop;
}

const Param *find(const QString &key)
{
    for (int i = 0; i < kParameterCount; ++i)
        if (key == QLatin1String(kParameters[i].key))
            return &kParameters[i];
    return nullptr;
}

} // namespace

QString PhysicsSettings::configFile() const
{
    // AppConfigLocation is ~/.config/<organization>/<application>, and main()
    // sets those two to the same pair the .desktop file gives Sailjail — so
    // this is the directory the sandbox actually created for us.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/settings.conf");
}

PhysicsSettings::PhysicsSettings(QObject *parent)
    : QObject(parent)
    , m_enabled(false)
    , m_displayMode(0)
{
    load();
}

void PhysicsSettings::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    QSettings s(configFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("physics/enabled"), on);
    emit enabledChanged();
}

void PhysicsSettings::setDisplayMode(int m)
{
    const int clamped = qBound(0, m, 2);
    if (m_displayMode == clamped)
        return;
    m_displayMode = clamped;
    QSettings s(configFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("view/displayMode"), clamped);
    emit displayModeChanged();
}

QString PhysicsSettings::storageKey(int page, const QString &key) const
{
    return QStringLiteral("physics/page%1/%2").arg(page).arg(key);
}

void PhysicsSettings::load()
{
    QSettings s(configFile(), QSettings::IniFormat);
    m_enabled = s.value(QStringLiteral("physics/enabled"), false).toBool();
    m_displayMode = qBound(0, s.value(QStringLiteral("view/displayMode"), 0).toInt(), 2);

    const int pages[2] = { Drop, Trampoline };
    for (int pi = 0; pi < 2; ++pi) {
        const int page = pages[pi];
        for (int i = 0; i < kParameterCount; ++i) {
            const Param &p = kParameters[i];
            if (!appliesTo(p, page))
                continue;
            const QString key = QLatin1String(p.key);
            m_values[page][key] =
                s.value(storageKey(page, key), defaultFor(p, page)).toDouble();
        }
    }

    // Write the whole set back on the first run, so the file exists from the
    // start. It costs nothing, and it means "are my settings being saved?" can
    // be answered by looking at the phone rather than by changing something and
    // restarting.
    if (!s.contains(QStringLiteral("physics/enabled"))) {
        s.setValue(QStringLiteral("physics/enabled"), m_enabled);
        s.setValue(QStringLiteral("view/displayMode"), m_displayMode);
        for (int pi = 0; pi < 2; ++pi)
            for (int i = 0; i < kParameterCount; ++i) {
                const Param &p = kParameters[i];
                if (!appliesTo(p, pages[pi]))
                    continue;
                const QString key = QLatin1String(p.key);
                s.setValue(storageKey(pages[pi], key), m_values[pages[pi]][key]);
            }
    }
}

void PhysicsSettings::save(int page, const QString &key, double v)
{
    QSettings s(configFile(), QSettings::IniFormat);
    s.setValue(storageKey(page, key), v);
}

QVariantList PhysicsSettings::parameters(int page) const
{
    QVariantList out;
    for (int i = 0; i < kParameterCount; ++i) {
        const Param &p = kParameters[i];
        if (!appliesTo(p, page))
            continue;

        QVariantMap m;
        m.insert(QStringLiteral("key"), QLatin1String(p.key));
        m.insert(QStringLiteral("label"), QLatin1String(p.label));
        m.insert(QStringLiteral("unit"), QLatin1String(p.unit));
        m.insert(QStringLiteral("min"), p.min);
        m.insert(QStringLiteral("max"), p.max);
        m.insert(QStringLiteral("step"), p.step);
        m.insert(QStringLiteral("decimals"), p.decimals);
        m.insert(QStringLiteral("value"), value(page, QLatin1String(p.key)));
        m.insert(QStringLiteral("defaultValue"), defaultFor(p, page));
        out.append(m);
    }
    return out;
}

double PhysicsSettings::value(int page, const QString &key) const
{
    const QHash<QString, double> &page_values = m_values[page];
    if (page_values.contains(key))
        return page_values.value(key);
    const Param *p = find(key);
    return p ? defaultFor(*p, page) : 0.0;
}

void PhysicsSettings::setValue(int page, const QString &key, double v)
{
    if (qFuzzyCompare(value(page, key) + 1.0, v + 1.0))
        return;
    m_values[page][key] = v;
    save(page, key, v);
    emit changed(page);
}

void PhysicsSettings::resetDefaults(int page)
{
    for (int i = 0; i < kParameterCount; ++i) {
        const Param &p = kParameters[i];
        if (!appliesTo(p, page))
            continue;
        const QString key = QLatin1String(p.key);
        m_values[page][key] = defaultFor(p, page);
        save(page, key, m_values[page][key]);
    }
    emit changed(page);
}

float PhysicsSettings::get(int page, const char *key) const
{
    return float(value(page, QLatin1String(key)));
}

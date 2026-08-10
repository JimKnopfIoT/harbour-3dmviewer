/*
  harbour-3dmviewer — physicssettings.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Live, editable values for the drop and trampoline simulations.

  WHY THIS EXISTS
  ---------------
  Every one of these numbers was arrived at by changing a constant, rebuilding,
  reinstalling and asking someone to tilt a phone — minutes per guess, and the
  guesses interact. Restitution against damping against shaft depth cannot be
  judged one at a time. Exposing them on a page with the numbers on screen turns
  that loop into a slider.

  The parameter table is defined here rather than in QML so the page stays a
  generic list of sliders: adding a knob is one row in kParameters, and the UI
  picks it up with no further work.

  Values are per page, because the two simulations want opposite things — the
  drop box wants damping so a part settles, the shaft wants almost none so the
  rebound survives the trip.

  It also carries the one setting that is NOT physics: how the model is drawn.
  Solid, wireframe and ghosted are chosen on the viewer page but have to hold
  for the falling pages too, and each page owns a separate ModelView — so the
  choice has to live somewhere all of them can see. This object is already that
  place, and it brings persistence with it.
*/
#ifndef PHYSICSSETTINGS_H
#define PHYSICSSETTINGS_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

class PhysicsSettings : public QObject
{
    Q_OBJECT
    // Whether the tuning page is reachable at all. Off by default: it is a
    // workbench, not a feature, and it should not be in the way of somebody who
    // just wants to look at a model.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // ModelView::DisplayMode as an int: 0 solid, 1 wireframe, 2 ghosted. Shared
    // by every page and remembered between runs.
    Q_PROPERTY(int displayMode READ displayMode WRITE setDisplayMode NOTIFY displayModeChanged)

public:
    // Matches ModelView::Mode. Only Drop (1) and Trampoline (2) are tunable.
    enum Page { Drop = 1, Trampoline = 2 };
    Q_ENUM(Page)

    explicit PhysicsSettings(QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    int displayMode() const { return m_displayMode; }
    void setDisplayMode(int m);

    // One entry per knob that applies to `page`: key, label, unit, min, max,
    // step, decimals and the current value. Consumed directly by the QML
    // Repeater that builds the sliders.
    Q_INVOKABLE QVariantList parameters(int page) const;

    Q_INVOKABLE double value(int page, const QString &key) const;
    Q_INVOKABLE void setValue(int page, const QString &key, double v);
    Q_INVOKABLE void resetDefaults(int page);

    // C++ accessor used by ModelView when it sets a drop up.
    float get(int page, const char *key) const;

signals:
    void enabledChanged();
    void displayModeChanged();
    // A knob moved. ModelView re-applies the whole set rather than tracking
    // which one, since a drop is configured in one place anyway.
    void changed(int page);

private:
    // All persistence goes through this, and never through a default-built
    // QSettings.
    //
    // WHY: Sailjail creates and passes through exactly ONE config directory —
    // the one named by OrganizationName/ApplicationName in the .desktop file.
    // SailfishApp sets QCoreApplication's names to the binary name instead, so
    // a default QSettings wrote to ~/.config/harbour-3dmviewer, which the
    // sandbox does not pass through: the file lived in the private overlay and
    // vanished with the process. Every slider was saved, and every one of them
    // was gone at the next start.
    QString configFile() const;
    QString storageKey(int page, const QString &key) const;
    void load();
    void save(int page, const QString &key, double v);

    bool m_enabled;
    int m_displayMode;
    // page -> (key -> value)
    QHash<int, QHash<QString, double> > m_values;
};

#endif // PHYSICSSETTINGS_H

/*
  harbour-3dmviewer — modelview.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The QML item that draws the model.

  WHY QQuickFramebufferObject AND NOT Qt3D / Qt Quick 3D
  ------------------------------------------------------
  The build target is Qt 5.6.3. Qt Quick 3D did not exist until 5.15, and Qt3D
  in 5.6 is both immature and not packaged for Sailfish. QQuickFramebufferObject
  has been stable since 5.2, renders into its own FBO on the Qt Quick render
  thread, and composites into the Silica scene like any other item — so the
  pull-down menu, page transitions and the cover all keep working over the top
  of live 3D. Hand-written GLES2 is a small price for that.

  THREADING
  ---------
  Renderer lives on the render thread. It may only touch the item inside
  synchronize(), which Qt calls with the GUI thread blocked. Everything the
  renderer needs is copied across there — never dereferenced later.
*/
#ifndef MODELVIEW_H
#define MODELVIEW_H

#include <QColor>
#include <QElapsedTimer>
#include <QQuaternion>
#include <QQuickFramebufferObject>
#include <QTimer>

#include "model/scene.h"
#include "physics/dropsim.h"
#include "physics/physicssettings.h"
#include "physics/tiltsource.h"
#include "render/camera.h"

class ModelLoader;

class ModelView : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(ModelLoader *loader READ loader WRITE setLoader NOTIFY loaderChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY backgroundColorChanged)
    // Reported back so the UI can show what the renderer could not do (for
    // example an index-size limitation on an old GPU) instead of silently
    // drawing a partial model.
    Q_PROPERTY(QString renderWarning READ renderWarning NOTIFY renderWarningChanged)

    // True while the model is a falling body rather than a framed exhibit.
    // The UI uses it to switch the button's label and to stop treating finger
    // drags as camera orbits.
    Q_PROPERTY(bool dropped READ dropped NOTIFY droppedChanged)
    // False when there is no working accelerometer, so the UI can say why
    // tilting does nothing instead of leaving the user shaking a phone at a
    // still picture. Only meaningful after the first drop — see TiltSource.
    Q_PROPERTY(bool tiltAvailable READ tiltAvailable NOTIFY tiltAvailableChanged)
    // Whether the camera is in the locked, stepped view. The UI reads it to
    // decide whether a drag orbits or pans, and to light up the 3D button.
    Q_PROPERTY(bool axisLocked READ axisLocked NOTIFY viewAxisChanged)
    Q_PROPERTY(DisplayMode displayMode READ displayMode WRITE setDisplayMode NOTIFY displayModeChanged)

    // How a drag turns the model; see Camera::RotationMode. pivotSet and
    // pivotScreenPos let the UI mark where the turn happens — without a mark,
    // tapping a point gives no feedback until the next drag.
    Q_PROPERTY(RotationMode rotationMode READ rotationMode NOTIFY rotationModeChanged)
    Q_PROPERTY(bool pivotSet READ pivotSet NOTIFY cameraChanged)
    Q_PROPERTY(QPointF pivotScreenPos READ pivotScreenPos NOTIFY cameraChanged)

    /*
      Print slices, for sliced formats only (.bgcode). sliceCount is 0 for a
      CAD model, which is what the UI tests before it shows the slice slider at
      all — there is nothing to slice in a Rhino file.

      sliceMin/sliceMax are the inclusive range that is drawn. Changing them
      costs one repaint: the geometry stays on the GPU and only the offset and
      count handed to the draw call move.
    */
    Q_PROPERTY(int sliceCount READ sliceCount NOTIFY sliceRangeChanged)
    Q_PROPERTY(int sliceMin READ sliceMin WRITE setSliceMin NOTIFY sliceRangeChanged)
    Q_PROPERTY(int sliceMax READ sliceMax WRITE setSliceMax NOTIFY sliceRangeChanged)
    // Nozzle height of the topmost drawn slice, in millimetres, for the label
    // next to the slider.
    Q_PROPERTY(qreal sliceTopZ READ sliceTopZ NOTIFY sliceRangeChanged)
    Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
    // The live tuning values. Set from QML so the view does not need a global.
    Q_PROPERTY(PhysicsSettings *settings READ settings WRITE setSettings NOTIFY settingsChanged)

public:
    /*
      What this view is for. Each of the three pages uses the same item in a
      different mode; the mode decides how gravity is interpreted and how deep
      the room is, which is all that actually differs between them.
    */
    // How the model is drawn. Named after Rhino's own display modes, since that
    // is where the expectation comes from.
    enum DisplayMode {
        Shaded,      // solid, lit
        Wireframe,   // edges only
        Ghosted      // solid but see-through
    };
    Q_ENUM(DisplayMode)

    enum Mode {
        Viewer,      // camera control, no simulation
        Drop,        // relative gravity, normal room: falls to the bottom of the screen
        Trampoline   // absolute gravity, deep bouncy room: falls INTO the screen
    };
    Q_ENUM(Mode)

    explicit ModelView(QQuickItem *parent = nullptr);

    Renderer *createRenderer() const override;

    ModelLoader *loader() const { return m_loader; }
    void setLoader(ModelLoader *l);

    QColor backgroundColor() const { return m_background; }
    void setBackgroundColor(const QColor &c);

    QString renderWarning() const { return m_renderWarning; }
    void setRenderWarning(const QString &w);   // called from the render thread's sync

    // --- interaction, all in device-independent screen pixels ---------------
    // Mirrors Camera::RotationMode; declared here because QML needs the enum on
    // the type that carries the property.
    enum RotationMode {
        RotateCentred = 0,   // turntable about the centre of the screen
        RotateAboutPivot,    // turntable about a tapped point on the model
        RotateFree           // arcball: any attitude, two-finger twist rolls
    };
    Q_ENUM(RotationMode)

    RotationMode rotationMode() const;
    bool pivotSet() const;
    // In this item's pixels; meaningless while pivotSet is false.
    QPointF pivotScreenPos() const;

    // Wraps back to RotateCentred, which is also the reset.
    Q_INVOKABLE void cycleRotationMode();

    // Makes the point of the model under (x, y) the centre of rotation. A tap
    // that misses falls back to the plane through the current centre. Returns
    // whether it actually hit.
    Q_INVOKABLE bool pickPivot(qreal x, qreal y);

    // A two-finger twist, in degrees. Only free rotation can roll.
    Q_INVOKABLE void rollBy(qreal degrees);

    Q_INVOKABLE void orbit(qreal dxPixels, qreal dyPixels);
    Q_INVOKABLE void panBy(qreal dxPixels, qreal dyPixels);
    Q_INVOKABLE void zoomBy(qreal factor);
    Q_INVOKABLE void fitToView();
    // "top", "front", "right", "perspective" — matching Rhino's names.
    Q_INVOKABLE void setStandardView(const QString &name);

    // A quarter turn in the locked view: 0 = the X button, 1 = Y, 2 = Z. See
    // Camera::stepAxis for which world axis each one actually turns about.
    Q_INVOKABLE void stepAxis(int which);
    Q_INVOKABLE void setFreeView();

    DisplayMode displayMode() const { return m_displayMode; }
    void setDisplayMode(DisplayMode m);
    // Shaded -> Wireframe -> Ghosted -> Shaded.
    Q_INVOKABLE void cycleDisplayMode();
    // True while the camera is square-on and stepped rather than free to orbit.
    bool axisLocked() const;

    int sliceCount() const { return m_sliceCount; }
    int sliceMin() const { return m_sliceMin; }
    int sliceMax() const { return m_sliceMax; }
    void setSliceMin(int v);
    void setSliceMax(int v);
    qreal sliceTopZ() const;

    bool dropped() const;
    bool tiltAvailable() const { return m_tilt.available(); }

    Mode mode() const { return m_mode; }
    void setMode(Mode m);

    // Shows the model still for a moment, then lets go of it. This is what the
    // drop and trampoline pages call when they become the visible page — there
    // is no button, the page itself is the trigger.
    PhysicsSettings *settings() const { return m_settings; }
    void setSettings(PhysicsSettings *s);

    Q_INVOKABLE void armAutoDrop();
    // Puts the model back and stops the simulation. Called when the page is
    // left, so coming back to it always replays the fall from the beginning.
    Q_INVOKABLE void cancelDrop();
    // Silica reports the page's rotation; the tilt source needs it to keep
    // "down the screen" pointing down the screen in landscape.
    Q_INVOKABLE void setUiRotation(int degrees);

signals:
    void loaderChanged();
    void backgroundColorChanged();
    void renderWarningChanged();
    void droppedChanged();
    void tiltAvailableChanged();
    void modeChanged();
    void viewAxisChanged();
    void displayModeChanged();
    void settingsChanged();
    void sliceRangeChanged();
    void rotationModeChanged();
    // Whenever the camera moved, so a QML pivot marker keeps up.
    void cameraChanged();

private slots:
    void onSceneChanged();
    void onLayerVisibilityChanged();
    void onSimTick();
    void onGravityChanged();
    void onAutoDropTimeout();
    void onSettingsChanged(int page);
    void onDisplayModeSettingChanged();

private:
    friend class ModelViewRenderer;

    void syncSimViewport();
    // The body pose that reproduces the camera's current view exactly.
    void poseFromCamera(QQuaternion *orientation, QVector3D *position) const;
    // Camera framing and room geometry for the current mode.
    void prepareRoom();

    ModelLoader *m_loader;
    Camera m_camera;
    QColor m_background;
    QString m_renderWarning;

    // --- drop simulation ----------------------------------------------------
    DropSim m_sim;
    TiltSource m_tilt;
    // Drives the simulation from the GUI thread. Qt Quick has no "animate this
    // item every frame" hook in 5.6 that is safe to do physics in, and a plain
    // timer keeps the simulation on the same thread as the pose the renderer
    // reads during synchronize().
    QTimer m_simTimer;
    // Holds the model still for a beat before releasing it, so you see what is
    // about to fall rather than catching only the blur.
    QTimer m_revealTimer;
    Mode m_mode;
    PhysicsSettings *m_settings;
    DisplayMode m_displayMode;
    // True once the room exists and should be drawn, which happens when the
    // page opens rather than when the part is released.
    bool m_roomReady;
    // Camera distance to restore when the model is caught: the drop pulls the
    // camera back to give the body room, and catching must undo exactly that.
    float m_dropReturnDistance;
    // Real elapsed time between ticks. Using the timer's nominal interval would
    // make the model fall in slow motion whenever a frame is late.
    QElapsedTimer m_simClock;
    qint64 m_lastTickMs;
    // Seconds since the last diagnostic line was written.
    float m_debugAccum;

    // Bumped every time the scene is replaced. The renderer compares it with
    // its own copy to decide whether to rebuild GPU buffers — cheaper and far
    // more robust than trying to signal across the thread boundary.
    quint64 m_sceneGeneration;

    // Print slices of the loaded model, and the inclusive range being drawn.
    int m_sliceCount;
    int m_sliceMin;
    int m_sliceMax;

    // Degrees of orbit per screen pixel dragged. A full screen width is about
    // one and a half turns, which matches how these gestures feel elsewhere.
    static const float kOrbitDegPerPixel;
};

#endif // MODELVIEW_H

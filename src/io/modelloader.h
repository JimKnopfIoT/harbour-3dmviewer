/*
  harbour-3dmviewer — modelloader.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The QML-facing document: it opens a model file on a worker thread, holds the
  resulting Scene, and publishes everything the UI needs to show about it.

  WHY A THREAD FOR A FILE THAT LOADS IN UNDER A SECOND
  ----------------------------------------------------
  The reference model parses in well under a second, but .3dm has no size limit
  and openNURBS decompresses every mesh chunk while reading. A 200 MB assembly
  would freeze the UI for long enough that Sailfish's compositor shows the "not
  responding" treatment. Parsing off the GUI thread also gives us a progress
  value for free, and it is far easier to build in now than to retrofit.

  THREAD SAFETY CONTRACT
  ----------------------
  The worker builds a Scene that nothing else can see, then hands it over by
  value (a QSharedPointer in a queued signal). After that the Scene is treated
  as immutable, with exactly one exception: SceneLayer::visible is toggled from
  the GUI thread. The render thread reads it inside synchronize(), which Qt runs
  with the GUI thread blocked — so that flag never needs a mutex.
*/
#ifndef MODELLOADER_H
#define MODELLOADER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include "model/scene.h"

class LayerModel;

// Lives on the worker thread. Owns nothing the GUI thread touches.
class ModelLoadWorker : public QObject
{
    Q_OBJECT
public:
    explicit ModelLoadWorker(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void doLoad(const QString &path);

signals:
    void progress(qreal fraction);
    void finished(ScenePtr scene, const QString &errorString);
};

class ModelLoader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool hasModel READ hasModel NOTIFY sceneChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

    Q_PROPERTY(QString fileName READ fileName NOTIFY sceneChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY sceneChanged)
    Q_PROPERTY(QString formatName READ formatName NOTIFY sceneChanged)
    Q_PROPERTY(QString producer READ producer NOTIFY sceneChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY sceneChanged)
    Q_PROPERTY(int objectCount READ objectCount NOTIFY sceneChanged)
    Q_PROPERTY(int triangleCount READ triangleCount NOTIFY sceneChanged)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY sceneChanged)
    Q_PROPERTY(int sliceCount READ sliceCount NOTIFY sceneChanged)
    Q_PROPERTY(int vertexCount READ vertexCount NOTIFY sceneChanged)
    Q_PROPERTY(QString sizeSummary READ sizeSummary NOTIFY sceneChanged)
    // "16 058 triangles" or "197 306 segments", whichever the loaded format
    // actually produced. The header line reads it so it never has to know
    // which kind of file is open.
    Q_PROPERTY(QString geometrySummary READ geometrySummary NOTIFY sceneChanged)

    Q_PROPERTY(LayerModel *layers READ layers CONSTANT)

public:
    explicit ModelLoader(QObject *parent = nullptr);
    ~ModelLoader() override;

    // Accepts both a file:// URL from the Silica file picker and a plain path
    // from the command line, because both reach this app.
    Q_INVOKABLE void open(const QUrl &url);
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void close();

    // True if this app has any chance of reading the file, judged by suffix.
    // Used to filter the picker and to fail early with a clear message.
    Q_INVOKABLE static bool isSupported(const QString &path);
    Q_INVOKABLE static QStringList supportedSuffixes();

    bool busy() const { return m_busy; }
    qreal progress() const { return m_progress; }
    bool hasModel() const { return !m_scene.isNull() && !m_scene->isEmpty(); }
    QString errorString() const { return m_error; }

    QString fileName() const;
    QString filePath() const;
    QString formatName() const;
    QString producer() const;
    QStringList warnings() const;
    int objectCount() const;
    int triangleCount() const;
    int lineCount() const;
    int sliceCount() const;
    int vertexCount() const;
    QString sizeSummary() const;
    QString geometrySummary() const;

    LayerModel *layers() const { return m_layers; }

    // For the renderer. Never null-checked away: callers must handle a null.
    ScenePtr scene() const { return m_scene; }

signals:
    void busyChanged();
    void progressChanged();
    void errorStringChanged();

    // The scene itself was replaced: the renderer must rebuild its buffers.
    void sceneChanged();
    // Only a layer's visible flag changed: a repaint is enough.
    void layerVisibilityChanged();

    void loadFailed(const QString &message);

private slots:
    void onWorkerProgress(qreal f);
    void onWorkerFinished(ScenePtr scene, const QString &errorString);

private:
    void setBusy(bool b);
    void setProgress(qreal p);

    QThread m_thread;
    ModelLoadWorker *m_worker;
    LayerModel *m_layers;

    ScenePtr m_scene;
    QString m_error;
    bool m_busy;
    qreal m_progress;
};

#endif // MODELLOADER_H

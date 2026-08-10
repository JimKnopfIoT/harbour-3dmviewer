/*
  harbour-3dmviewer — modelloader.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/modelloader.h"

#include "io/bgcodereader.h"
#include "io/rhinoreader.h"
#include "io/stepreader.h"
#include "io/stlreader.h"
#include "io/threemfreader.h"
#include "model/layermodel.h"

#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>

namespace {

// Trampoline so the C-style progress callback in the reader API can reach a
// worker instance without the reader having to know about Qt signals.
void progressTrampoline(void *ctx, float f)
{
    ModelLoadWorker *w = static_cast<ModelLoadWorker *>(ctx);
    if (w)
        emit w->progress(qreal(f));
}

} // namespace

// ---------------------------------------------------------------- worker ---

void ModelLoadWorker::doLoad(const QString &path)
{
    emit progress(0.0);

    const QString suffix = QFileInfo(path).suffix().toLower();
    QString error;
    ScenePtr scene;

    if (suffix == QLatin1String("3dm")) {
        scene = RhinoReader::read(path, &error, progressTrampoline, this);
    } else if (suffix == QLatin1String("bgcode")) {
        scene = BgCodeReader::read(path, &error, progressTrampoline, this);
    } else if (suffix == QLatin1String("stl")) {
        scene = StlReader::read(path, &error, progressTrampoline, this);
    } else if (suffix == QLatin1String("3mf")) {
        scene = ThreeMfReader::read(path, &error, progressTrampoline, this);
    } else if (suffix == QLatin1String("stp") || suffix == QLatin1String("step")) {
        scene = StepReader::read(path, &error, progressTrampoline, this);
    } else {
        error = tr("Unknown file type \"%1\".").arg(suffix);
    }

    emit progress(1.0);
    emit finished(scene, error);
}

// ---------------------------------------------------------------- loader ---

ModelLoader::ModelLoader(QObject *parent)
    : QObject(parent)
    , m_worker(new ModelLoadWorker)
    , m_layers(new LayerModel(this))
    , m_busy(false)
    , m_progress(0.0)
{
    // ScenePtr crosses a thread boundary in a queued connection, so Qt needs to
    // know how to copy it. Without this the connection silently does nothing.
    qRegisterMetaType<ScenePtr>("ScenePtr");

    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &ModelLoadWorker::progress, this, &ModelLoader::onWorkerProgress);
    connect(m_worker, &ModelLoadWorker::finished, this, &ModelLoader::onWorkerFinished);
    m_thread.start();

    // Toggling a layer in the list must repaint the view. The list model owns
    // the toggle; the loader owns the signal the renderer listens to.
    connect(m_layers, &LayerModel::visibilityChanged,
            this, &ModelLoader::layerVisibilityChanged);
}

ModelLoader::~ModelLoader()
{
    // Stop the thread before the members it may still be touching go away.
    m_thread.quit();
    m_thread.wait();
}

bool ModelLoader::isSupported(const QString &path)
{
    return supportedSuffixes().contains(QFileInfo(path).suffix().toLower());
}

QStringList ModelLoader::supportedSuffixes()
{
    // Everything the picker offers and the loader can actually open. Keep this
    // in step with ModelLoadWorker::doLoad and with the picker's nameFilters
    // in ViewerPage.qml — a suffix listed here but not handled there gives the
    // user a file that opens into an error.
    return QStringList() << QStringLiteral("3dm")
                         << QStringLiteral("stp")
                         << QStringLiteral("step")
                         << QStringLiteral("stl")
                         << QStringLiteral("3mf")
                         << QStringLiteral("bgcode");
}

void ModelLoader::open(const QUrl &url)
{
    openPath(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

void ModelLoader::openPath(const QString &path)
{
    if (m_busy)
        return;   // one load at a time; the UI disables the menu item anyway

    if (path.isEmpty())
        return;

    m_error.clear();
    emit errorStringChanged();

    setBusy(true);
    setProgress(0.0);

    // Queued: this runs doLoad() on the worker thread, not here.
    QMetaObject::invokeMethod(m_worker, "doLoad", Qt::QueuedConnection,
                              Q_ARG(QString, path));
}

void ModelLoader::close()
{
    m_scene.clear();
    m_layers->setScene(ScenePtr());
    m_error.clear();
    emit errorStringChanged();
    emit sceneChanged();
}

void ModelLoader::onWorkerProgress(qreal f)
{
    setProgress(f);
}

void ModelLoader::onWorkerFinished(ScenePtr scene, const QString &errorString)
{
    setBusy(false);
    setProgress(1.0);

    if (scene.isNull()) {
        // Keep whatever was on screen: losing the previous model because the
        // next one failed to open would be gratuitous.
        m_error = errorString.isEmpty() ? tr("The file could not be opened.") : errorString;
        emit errorStringChanged();
        emit loadFailed(m_error);
        return;
    }

    m_scene = scene;
    m_error = errorString;   // usually empty; a soft error stays visible
    emit errorStringChanged();

    m_layers->setScene(m_scene);
    emit sceneChanged();
}

void ModelLoader::setBusy(bool b)
{
    if (m_busy == b)
        return;
    m_busy = b;
    emit busyChanged();
}

void ModelLoader::setProgress(qreal p)
{
    if (qFuzzyCompare(m_progress + 1.0, p + 1.0))
        return;
    m_progress = p;
    emit progressChanged();
}

// ------------------------------------------------------------ properties ---

QString ModelLoader::fileName() const
{
    return m_scene.isNull() ? QString() : QFileInfo(m_scene->filePath).fileName();
}

QString ModelLoader::filePath() const
{
    return m_scene.isNull() ? QString() : m_scene->filePath;
}

QString ModelLoader::formatName() const
{
    return m_scene.isNull() ? QString() : m_scene->formatName;
}

QString ModelLoader::producer() const
{
    return m_scene.isNull() ? QString() : m_scene->producer;
}

QStringList ModelLoader::warnings() const
{
    return m_scene.isNull() ? QStringList() : m_scene->warnings;
}

int ModelLoader::objectCount() const
{
    return m_scene.isNull() ? 0 : m_scene->objectCount;
}

int ModelLoader::triangleCount() const
{
    return m_scene.isNull() ? 0 : m_scene->triangleCount();
}

int ModelLoader::lineCount() const
{
    return m_scene.isNull() ? 0 : m_scene->lineCount();
}

int ModelLoader::sliceCount() const
{
    return m_scene.isNull() ? 0 : m_scene->sliceCount;
}

int ModelLoader::vertexCount() const
{
    return m_scene.isNull() ? 0 : m_scene->vertexCount();
}

QString ModelLoader::geometrySummary() const
{
    if (m_scene.isNull())
        return QString();
    QLocale loc;
    // A sliced file has no triangles at all, and a surface model has no
    // segments; whichever is non-zero is the one worth naming.
    if (m_scene->lineCount() > 0 && m_scene->triangleCount() == 0)
        return tr("%1 segments").arg(loc.toString(m_scene->lineCount()));
    return tr("%1 triangles").arg(loc.toString(m_scene->triangleCount()));
}

QString ModelLoader::sizeSummary() const
{
    if (m_scene.isNull())
        return QString();
    // Grouped digits: "12 882" reads instantly, "12882" does not.
    QLocale loc;
    const QVector3D d = m_scene->bbMax - m_scene->bbMin;
    return tr("%1 x %2 x %3 units")
        .arg(loc.toString(double(d.x()), 'f', 1),
             loc.toString(double(d.y()), 'f', 1),
             loc.toString(double(d.z()), 'f', 1));
}

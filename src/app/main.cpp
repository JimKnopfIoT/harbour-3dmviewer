/*
  harbour-3dmviewer — main.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  A read-only 3D viewer. It opens Rhino .3dm models, draws them, and lets you
  turn them around. There is no write path anywhere in this program.
*/
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QScopedPointer>
#include <QStringList>
#include <QUrl>

#include <sailfishapp.h>

#include "io/modelloader.h"
#include "model/layermodel.h"
#include "physics/physicssettings.h"
#include "render/modelview.h"

int main(int argc, char *argv[])
{
    // SailfishApp::application() sets the organisation/app name and the QML
    // import paths the Silica components need; creating a bare QGuiApplication
    // instead leaves Silica unable to find its own theme.
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    app->setApplicationVersion(QStringLiteral(APP_VERSION));

    // These MUST match OrganizationName/ApplicationName in the .desktop file.
    //
    // Sailjail creates and passes through exactly one config directory, the one
    // named by those two. SailfishApp sets QCoreApplication's names to the
    // binary name instead, which sends QSettings to a path the sandbox does not
    // pass through — the file is written into the private overlay and is gone
    // when the app exits. Settings appeared to save and never came back.
    app->setOrganizationName(QStringLiteral("io.github.jimknopfiot"));
    app->setApplicationName(QStringLiteral("harbour-3dmviewer"));

    // ModelView is instantiated from QML; LayerModel is only ever handed out by
    // ModelLoader, so it is registered uncreatable — that way QML can still
    // name its role enums without being able to construct a dangling one.
    qmlRegisterType<ModelView>("harbour.threedmviewer", 1, 0, "ModelView");
    qmlRegisterUncreatableType<LayerModel>(
        "harbour.threedmviewer", 1, 0, "LayerModel",
        QStringLiteral("LayerModel is provided by ModelLoader.layers"));
    // ModelLoader reaches QML as the `modelLoader` context property, so it is
    // never constructed from QML — but it still has to be registered, because
    // ModelView.loader is typed ModelLoader* and QML refuses to assign to a
    // property whose type it does not know ("unknown property type").
    qmlRegisterUncreatableType<PhysicsSettings>(
        "harbour.threedmviewer", 1, 0, "PhysicsSettings",
        QStringLiteral("PhysicsSettings is exposed as the global 'physics'"));
    qmlRegisterUncreatableType<ModelLoader>(
        "harbour.threedmviewer", 1, 0, "ModelLoader",
        QStringLiteral("ModelLoader is exposed as the global 'modelLoader'"));

    // Persisted under the app's own organisation/name, which SailfishApp has
    // already set on the QCoreApplication by this point.
    PhysicsSettings physics;

    ModelLoader loader;

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->rootContext()->setContextProperty(QStringLiteral("modelLoader"), &loader);
    view->rootContext()->setContextProperty(QStringLiteral("physics"), &physics);
    view->rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                            QStringLiteral(APP_VERSION));

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    // Opening a file straight from the command line: handy when testing over
    // ssh, and it is what a future MIME association would use. Done after
    // show() so the QML side already exists to display the progress.
    const QStringList args = app->arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a.startsWith(QLatin1Char('-')))
            continue;
        loader.openPath(a);
        break;
    }

    return app->exec();
}

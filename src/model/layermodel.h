/*
  harbour-3dmviewer — layermodel.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The Rhino layer table as a QAbstractListModel, so LayersPage.qml can be a
  plain SilicaListView with a TextSwitch per row.

  The model does not own the layers — it is a view onto the Scene's layer
  array, and toggling a row writes straight through to SceneLayer::visible.
  That is deliberate: the renderer reads the same flag, so there is exactly one
  copy of "is this layer shown" in the whole program and it cannot go stale.
*/
#ifndef LAYERMODEL_H
#define LAYERMODEL_H

#include <QAbstractListModel>

#include "model/scene.h"

class LayerModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY visibilityChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        ColorRole,
        VisibleRole,
        TriangleCountRole
    };

    explicit LayerModel(QObject *parent = nullptr);

    void setScene(const ScenePtr &scene);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int visibleCount() const;

    Q_INVOKABLE void setVisible(int row, bool visible);
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void showAll();
    Q_INVOKABLE void hideAll();
    // Restores what the file itself said was visible. Rhino files routinely
    // carry construction layers that are off by design, and after showing
    // everything there is otherwise no way back short of reopening the file.
    Q_INVOKABLE void resetToFileDefaults();

signals:
    void countChanged();
    void visibilityChanged();

private:
    ScenePtr m_scene;
    QVector<bool> m_fileDefaults;   // visibility as stored in the file
};

#endif // LAYERMODEL_H

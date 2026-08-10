/*
  harbour-3dmviewer — layermodel.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "model/layermodel.h"

LayerModel::LayerModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void LayerModel::setScene(const ScenePtr &scene)
{
    beginResetModel();
    m_scene = scene;
    m_fileDefaults.clear();
    if (!m_scene.isNull()) {
        m_fileDefaults.reserve(m_scene->layers.size());
        for (int i = 0; i < m_scene->layers.size(); ++i)
            m_fileDefaults.append(m_scene->layers.at(i).visible);
    }
    endResetModel();

    emit countChanged();
    emit visibilityChanged();
}

int LayerModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || m_scene.isNull())
        return 0;
    return m_scene->layers.size();
}

QVariant LayerModel::data(const QModelIndex &index, int role) const
{
    if (m_scene.isNull() || !index.isValid())
        return QVariant();
    const int row = index.row();
    if (row < 0 || row >= m_scene->layers.size())
        return QVariant();

    const SceneLayer &L = m_scene->layers.at(row);
    switch (role) {
    case NameRole:          return L.name;
    case ColorRole:         return L.color;
    case VisibleRole:       return L.visible;
    case TriangleCountRole: return L.triangleCount();
    default:                return QVariant();
    }
}

QHash<int, QByteArray> LayerModel::roleNames() const
{
    QHash<int, QByteArray> r;
    r.insert(NameRole, "layerName");
    r.insert(ColorRole, "layerColor");
    r.insert(VisibleRole, "layerVisible");
    r.insert(TriangleCountRole, "triangleCount");
    return r;
}

int LayerModel::visibleCount() const
{
    if (m_scene.isNull())
        return 0;
    int n = 0;
    for (int i = 0; i < m_scene->layers.size(); ++i)
        if (m_scene->layers.at(i).visible)
            ++n;
    return n;
}

void LayerModel::setVisible(int row, bool visible)
{
    if (m_scene.isNull() || row < 0 || row >= m_scene->layers.size())
        return;
    if (m_scene->layers.at(row).visible == visible)
        return;

    m_scene->layers[row].visible = visible;

    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, QVector<int>() << VisibleRole);
    emit visibilityChanged();
}

void LayerModel::toggle(int row)
{
    if (m_scene.isNull() || row < 0 || row >= m_scene->layers.size())
        return;
    setVisible(row, !m_scene->layers.at(row).visible);
}

void LayerModel::showAll()
{
    if (m_scene.isNull() || m_scene->layers.isEmpty())
        return;
    for (int i = 0; i < m_scene->layers.size(); ++i)
        m_scene->layers[i].visible = true;
    emit dataChanged(index(0, 0), index(m_scene->layers.size() - 1, 0),
                     QVector<int>() << VisibleRole);
    emit visibilityChanged();
}

void LayerModel::hideAll()
{
    if (m_scene.isNull() || m_scene->layers.isEmpty())
        return;
    for (int i = 0; i < m_scene->layers.size(); ++i)
        m_scene->layers[i].visible = false;
    emit dataChanged(index(0, 0), index(m_scene->layers.size() - 1, 0),
                     QVector<int>() << VisibleRole);
    emit visibilityChanged();
}

void LayerModel::resetToFileDefaults()
{
    if (m_scene.isNull() || m_scene->layers.isEmpty())
        return;
    // m_fileDefaults was captured in setScene() and has the same length as the
    // layer array; guard anyway so a future reader that mutates layers after
    // handing over cannot walk off the end.
    const int n = qMin(m_fileDefaults.size(), m_scene->layers.size());
    for (int i = 0; i < n; ++i)
        m_scene->layers[i].visible = m_fileDefaults.at(i);
    emit dataChanged(index(0, 0), index(m_scene->layers.size() - 1, 0),
                     QVector<int>() << VisibleRole);
    emit visibilityChanged();
}

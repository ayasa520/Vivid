#pragma once

#include <QObject>
#include <QString>
#include <qqml.h>

#ifndef VIVID_KDE_VERSION
#define VIVID_KDE_VERSION "1.0.0"
#endif

class PluginInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString version READ version CONSTANT)

public:
    explicit PluginInfo(QObject* parent = nullptr): QObject(parent) {}

    QString version() const { return QStringLiteral(VIVID_KDE_VERSION); }
};

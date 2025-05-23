// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <QList>
#include <QMetaType>

#include "propertyvaluecontainer.h"

namespace QmlDesigner {

class ChangeValuesCommand
{
    friend QDataStream &operator>>(QDataStream &in, ChangeValuesCommand &command);
    friend QDebug operator <<(QDebug debug, const ChangeValuesCommand &command);

public:
    ChangeValuesCommand();
    explicit ChangeValuesCommand(const QList<PropertyValueContainer> &valueChangeVector);

    const QList<PropertyValueContainer> valueChanges() const;

private:
    QList<PropertyValueContainer> m_valueChangeVector;
};

QDataStream &operator<<(QDataStream &out, const ChangeValuesCommand &command);
QDataStream &operator>>(QDataStream &in, ChangeValuesCommand &command);

QDebug operator <<(QDebug debug, const ChangeValuesCommand &command);

} // namespace QmlDesigner

Q_DECLARE_METATYPE(QmlDesigner::ChangeValuesCommand)

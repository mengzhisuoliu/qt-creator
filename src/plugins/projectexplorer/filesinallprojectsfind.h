// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "allprojectsfind.h"

namespace ProjectExplorer::Internal {

class FilesInAllProjectsFind : public AllProjectsFind
{
    Q_OBJECT

public:
    QString id() const override;
    QString displayName() const override;

    Utils::Store save() const override;
    void restore(const Utils::Store &s) override;

    // deprecated
    QByteArray settingsKey() const override;

protected:
    TextEditor::FileContainerProvider fileContainerProvider() const override;
    QString label() const override;
};

} // namespace ProjectExplorer::Internal


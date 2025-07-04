// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <projectexplorer/ioutputparser.h>
#include <projectexplorer/task.h>

#include <QRegularExpression>

namespace CMakeProjectManager::Internal {

class CMakeAutogenParser : public ProjectExplorer::OutputTaskParser
{
public:
    explicit CMakeAutogenParser();

private:
    Result handleLine(const QString &line, Utils::OutputFormat type) override;
    void flush() override;

    enum ExpectedState { NONE, LINE_SEPARATOR, LINE_DESCRIPTION };

    ExpectedState m_expectedState = NONE;

    ProjectExplorer::Task m_lastTask;
    QRegularExpression m_commonError;
    QRegularExpression m_commonWarning;
    QRegularExpression m_separatorLine;
    int m_lines = 0;
};

#ifdef WITH_TESTS
QObject *createCMakeAutogenParserTest();
#endif

} // namespace CMakeProjectManager::Internal

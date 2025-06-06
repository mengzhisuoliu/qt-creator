// Copyright (C) 2016 Axonian LLC.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "cmake_global.h"

#include <projectexplorer/ioutputparser.h>
#include <projectexplorer/task.h>

#include <utils/filepath.h>

#include <QRegularExpression>

#include <optional>

namespace CMakeProjectManager {

class CMAKE_EXPORT CMakeOutputParser : public ProjectExplorer::OutputTaskParser
{
public:
    explicit CMakeOutputParser();
    void setSourceDirectory(const Utils::FilePath &sourceDir);

private:
    Result handleLine(const QString &line, Utils::OutputFormat type) override;
    void flush() override;
    Utils::FilePath resolvePath(const QString &path) const;

    enum TripleLineError { NONE, LINE_LOCATION, LINE_DESCRIPTION, LINE_DESCRIPTION2 };

    TripleLineError m_expectTripleLineErrorData = NONE;

    std::optional<Utils::FilePath> m_sourceDirectory;
    ProjectExplorer::Task m_lastTask;
    QRegularExpression m_commonError;
    QRegularExpression m_nextSubError;
    QRegularExpression m_commonWarning;
    QRegularExpression m_locationLine;
    QRegularExpression m_sourceLineAndFunction;
    bool m_skippedFirstEmptyLine = false;
    int m_lines = 0;

    struct CallStackLine
    {
        Utils::FilePath file;
        int line = -1;
        QString function;
    };
    QList<CallStackLine> m_callStack;
    bool m_callStackDetected = false;
    CallStackLine m_errorOrWarningLine;
};

#ifdef WITH_TESTS
namespace Internal { QObject *createCMakeOutputParserTest(); }
#endif

} // CMakeProjectManager

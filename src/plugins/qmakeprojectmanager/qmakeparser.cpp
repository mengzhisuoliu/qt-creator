// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmakeparser.h"

#include <projectexplorer/task.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/buildmanager.h>

using namespace ProjectExplorer;
using namespace Utils;

namespace QmakeProjectManager {
using namespace Internal;

QMakeParser::QMakeParser() : m_error(QLatin1String("^(.+?):(\\d+?):\\s(.+?)$"))
{
    setObjectName(QLatin1String("QMakeParser"));
}

OutputLineParser::Result QMakeParser::handleLine(const QString &line, OutputFormat type)
{
    if (type != StdErrFormat)
        return Status::NotHandled;
    QString lne = rightTrimmed(line);
    QRegularExpressionMatch match = m_error.match(lne);
    if (match.hasMatch()) {
        QString fileName = match.captured(1);
        Task::TaskType type = Task::Error;
        const QString description = match.captured(3);
        int fileNameOffset = match.capturedStart(1);
        if (fileName.startsWith(QLatin1String("WARNING: "))) {
            type = Task::Warning;
            fileName = fileName.mid(9);
            fileNameOffset += 9;
        } else if (fileName.startsWith(QLatin1String("ERROR: "))) {
            fileName = fileName.mid(7);
            fileNameOffset += 7;
        }
        if (description.startsWith(QLatin1String("note:"), Qt::CaseInsensitive))
            type = Task::Unknown;
        else if (description.startsWith(QLatin1String("warning:"), Qt::CaseInsensitive))
            type = Task::Warning;
        else if (description.startsWith(QLatin1String("error:"), Qt::CaseInsensitive))
            type = Task::Error;

        QmakeTask t(type, description, absoluteFilePath(FilePath::fromUserInput(fileName)),
                          match.captured(2).toInt() /* line */);
        LinkSpecs linkSpecs;
        addLinkSpecForAbsoluteFilePath(linkSpecs, t.file(), t.line(), t.column(), fileNameOffset,
                                       fileName.length());
        scheduleTask(t, 1);
        return {Status::Done, linkSpecs};
    }
    if (lne.startsWith(QLatin1String("Project ERROR: "))
            || lne.startsWith(QLatin1String("ERROR: "))) {
        const QString description = lne.mid(lne.indexOf(QLatin1Char(':')) + 2);
        scheduleTask(QmakeTask(Task::Error, description), 1);
        return Status::Done;
    }
    if (lne.startsWith(QLatin1String("Project WARNING: "))
            || lne.startsWith(QLatin1String("WARNING: "))) {
        const QString description = lne.mid(lne.indexOf(QLatin1Char(':')) + 2);
        scheduleTask(QmakeTask(Task::Warning, description), 1);
        return Status::Done;
    }
    return Status::NotHandled;
}

} // QmakeProjectManager

// Unit tests:

#ifdef WITH_TESTS

#include <projectexplorer/outputparser_test.h>

#include <QTest>

namespace QmakeProjectManager::Internal {

class QmakeOutputParserTest final : public QObject
{
    Q_OBJECT

private slots:
    void testQmakeOutputParsers_data();
    void testQmakeOutputParsers();
};

void QmakeOutputParserTest::testQmakeOutputParsers_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<OutputParserTester::Channel>("inputChannel");
    QTest::addColumn<QStringList>("childStdOutLines");
    QTest::addColumn<QStringList>("childStdErrLines");
    QTest::addColumn<Tasks >("tasks");

    QTest::newRow("pass-through stdout")
            << QString::fromLatin1("Sometext") << OutputParserTester::STDOUT
        << QStringList("Sometext") << QStringList()
            << Tasks();
    QTest::newRow("pass-through stderr")
            << QString::fromLatin1("Sometext") << OutputParserTester::STDERR
            << QStringList() << QStringList("Sometext")
            << Tasks();

    QTest::newRow("qMake error")
            << QString::fromLatin1("Project ERROR: undefined file")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << QmakeTask(Task::Error,
                                   "undefined file"));

    QTest::newRow("qMake Parse Error")
            << QString::fromLatin1("e:\\project.pro:14: Parse Error ('sth odd')")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << QmakeTask(Task::Error,
                                   "Parse Error ('sth odd')",
                                   FilePath::fromUserInput("e:\\project.pro"),
                                   14));

    QTest::newRow("qMake warning")
            << QString::fromLatin1("Project WARNING: bearer module might require ReadUserData capability")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << QmakeTask(Task::Warning,
                                   "bearer module might require ReadUserData capability"));

    QTest::newRow("qMake warning 2")
            << QString::fromLatin1("WARNING: Failure to find: blackberrycreatepackagestepconfigwidget.cpp")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << QmakeTask(Task::Warning,
                                   "Failure to find: blackberrycreatepackagestepconfigwidget.cpp"));

    QTest::newRow("qMake warning with location")
            << QString::fromLatin1("WARNING: e:\\QtSDK\\Simulator\\Qt\\msvc2008\\lib\\qtmaind.prl:1: Unescaped backslashes are deprecated.")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << QmakeTask(Task::Warning,
                                   "Unescaped backslashes are deprecated.",
                                   FilePath::fromUserInput("e:\\QtSDK\\Simulator\\Qt\\msvc2008\\lib\\qtmaind.prl"), 1));

    QTest::newRow("moc note")
            << QString::fromLatin1("/home/qtwebkithelpviewer.h:0: Note: No relevant classes found. No output generated.")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << QmakeTask(Task::Unknown,
                        "Note: No relevant classes found. No output generated.",
                        FilePath::fromUserInput("/home/qtwebkithelpviewer.h"), -1));
}

void QmakeOutputParserTest::testQmakeOutputParsers()
{
    OutputParserTester testbench;
    testbench.addLineParser(new QMakeParser);
    QFETCH(QString, input);
    QFETCH(OutputParserTester::Channel, inputChannel);
    QFETCH(Tasks, tasks);
    QFETCH(QStringList, childStdOutLines);
    QFETCH(QStringList, childStdErrLines);

    testbench.testParsing(input, inputChannel, tasks, childStdOutLines, childStdErrLines);
}

QObject *createQmakeOutputParserTest()
{
    return new QmakeOutputParserTest;
}

} // QmakeProjectManager::Internal

#endif

#include "qmakeparser.moc"

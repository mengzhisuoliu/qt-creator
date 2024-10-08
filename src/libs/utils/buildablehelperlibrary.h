// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "utils_global.h"

#include "filepath.h"

#include <QCoreApplication>

namespace Utils {

class Environment;

class QTCREATOR_UTILS_EXPORT BuildableHelperLibrary
{
public:
    static FilePaths findQtsInEnvironment(const Environment &env);
    static bool isQtChooser(const FilePath &filePath);
    static FilePath qtChooserToQmakePath(const FilePath &path);
    // return true if the qmake at qmakePath is a Qt (used by QtVersion)
    static QString qtVersionForQMake(const FilePath &qmakePath);
    // returns something like qmake4, qmake, qmake-qt4 or whatever distributions have chosen (used by QtVersion)
    static QStringList possibleQMakeCommands();
    static QString filterForQmakeFileDialog();
};

} // Utils

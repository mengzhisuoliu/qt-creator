// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "utils_global.h"

#include "result.h"

#include <QTemporaryFile>

namespace Utils {

class FilePath;
class TemporaryFilePathPrivate;

// Note: This can work remotely.
class QTCREATOR_UTILS_EXPORT TemporaryFilePath
{
public:
    TemporaryFilePath() = delete;
    TemporaryFilePath(const TemporaryFilePath &other) = delete;

    ~TemporaryFilePath();

    static Result<std::unique_ptr<TemporaryFilePath>> create(const FilePath &templatePath);

    void setAutoRemove(bool autoDelete);
    bool autoRemove() const;

    FilePath templatePath() const;
    FilePath filePath() const;

private:
    TemporaryFilePath(const FilePath &templatePath, const FilePath &filePath);

private:
    std::unique_ptr<TemporaryFilePathPrivate> d;
};

// Note: This is local-only
class QTCREATOR_UTILS_EXPORT TemporaryFile : public QTemporaryFile
{
public:
    explicit TemporaryFile(const QString &pattern);
    ~TemporaryFile();

    FilePath filePath() const;

    [[deprecated("Use filePath")]] QString fileName() const override;
};

} // namespace Utils

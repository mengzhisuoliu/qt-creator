// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "generatedfile.h"

#include "coreplugintr.h"
#include "editormanager/editormanager.h"

#include <utils/fileutils.h>
#include <utils/textfileformat.h>

#include <QCoreApplication>
#include <QDebug>

#include <optional>

using namespace Utils;

namespace Core {

/*!
    \class Core::GeneratedFile
    \inheaderfile coreplugin/generatedfile.h
    \inmodule QtCreator

    \brief The GeneratedFile class represents a file generated by a wizard.

    The BaseFileWizard class checks whether each file already exists and
    reports any errors that may occur during creation of the files.

    \sa Core::WizardDialogParameters, Core::BaseFileWizard,
 */

class GeneratedFilePrivate : public QSharedData
{
public:
    GeneratedFilePrivate() = default;
    explicit GeneratedFilePrivate(const FilePath &path);

    Result<> writeContents() const;
    Result<> writePermissions() const;

    FilePath path;
    std::optional<QFile::Permissions> permissions;
    QByteArray contents;
    Id editorId;
    bool binary = false;
    GeneratedFile::Attributes attributes;
};

inline QDebug &operator<<(QDebug &debug, const Core::GeneratedFilePrivate &file)
{
    debug << "path: " << file.path
          << "; editorId: " << file.editorId.toString()
          << "; binary: " << file.binary
          << "; contents: " << file.contents.size();
    return debug;
}

QDebug &operator<<(QDebug &debug, const Core::GeneratedFile &file)
{
    debug << "GeneratedFile{_: " << *file.m_d << "}";
    return debug;
}

GeneratedFilePrivate::GeneratedFilePrivate(const FilePath &path) :
    path(path.cleanPath()),
    attributes({})
{
}

GeneratedFile::GeneratedFile() :
    m_d(new GeneratedFilePrivate)
{
}

GeneratedFile::GeneratedFile(const FilePath &path) :
    m_d(new GeneratedFilePrivate(path))
{
}

GeneratedFile::GeneratedFile(const GeneratedFile &rhs) = default;

GeneratedFile &GeneratedFile::operator=(const GeneratedFile &rhs)
{
    if (this != &rhs)
        m_d.operator=(rhs.m_d);
    return *this;
}

GeneratedFile::~GeneratedFile() = default;

FilePath GeneratedFile::filePath() const
{
    return m_d->path;
}

void GeneratedFile::setFilePath(const FilePath &p)
{
    m_d->path = p;
}

void GeneratedFile::setPermissions(QFileDevice::Permissions permissions)
{
    m_d->permissions = permissions;
}

std::optional<QFileDevice::Permissions> GeneratedFile::permissions() const
{
    return m_d->permissions;
}

QString GeneratedFile::contents() const
{
    return QString::fromUtf8(m_d->contents);
}

void GeneratedFile::setContents(const QString &c)
{
    m_d->contents = c.toUtf8();
}

QByteArray GeneratedFile::binaryContents() const
{
    return m_d->contents;
}

void GeneratedFile::setBinaryContents(const QByteArray &c)
{
    m_d->contents = c;
}

bool GeneratedFile::isBinary() const
{
    return m_d->binary;
}

void GeneratedFile::setBinary(bool b)
{
    m_d->binary = b;
}

Id GeneratedFile::editorId() const
{
    return m_d->editorId;
}

void GeneratedFile::setEditorId(Id id)
{
    m_d->editorId = id;
}

Result<> GeneratedFilePrivate::writeContents() const
{
    if (binary) {
        QIODevice::OpenMode flags = QIODevice::WriteOnly | QIODevice::Truncate;
        FileSaver saver(path, flags);
        saver.write(contents);
        return saver.finalize();
    }

    TextFileFormat format;
    format.setEncoding(EditorManager::defaultTextEncoding());
    format.lineTerminationMode = EditorManager::defaultLineEnding();
    return format.writeFile(path, QString::fromUtf8(contents));
}

Result<> GeneratedFilePrivate::writePermissions() const
{
    if (!permissions)
        return ResultOk;
    if (!path.setPermissions(*permissions))
        return ResultError(Tr::tr("Failed to set permissions."));
    return ResultOk;
}

Result<> GeneratedFile::write() const
{
    // Ensure the directory
    const FilePath parentDir = m_d->path.parentDir();
    if (!parentDir.isDir()) {
        if (!parentDir.createDir()) {
            return ResultError(Tr::tr("Unable to create the directory %1.")
                                .arg(parentDir.toUserOutput()));
        }
    }

    // Write out
    if (const Result<> res = m_d->writeContents(); !res)
        return res;
    return m_d->writePermissions();
}

GeneratedFile::Attributes GeneratedFile::attributes() const
{
    return m_d->attributes;
}

void GeneratedFile::setAttributes(Attributes a)
{
    m_d->attributes = a;
}

} // namespace Core

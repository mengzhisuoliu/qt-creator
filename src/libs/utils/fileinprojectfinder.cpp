// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "fileinprojectfinder.h"

#include "algorithm.h"
#include "fileutils.h"
#include "qrcparser.h"
#include "qtcassert.h"

#include <QCursor>
#include <QDir>
#include <QLoggingCategory>
#include <QMenu>
#include <QUrl>

namespace {
static Q_LOGGING_CATEGORY(finderLog, "qtc.utils.fileinprojectfinder", QtWarningMsg);
}

namespace Utils {

static bool checkPath(const FilePath &candidate, int matchLength,
                      FileInProjectFinder::FileHandler fileHandler,
                      FileInProjectFinder::DirectoryHandler directoryHandler)
{
    if (fileHandler && candidate.isFile()) {
        fileHandler(candidate, matchLength);
        return true;
    } else if (directoryHandler && candidate.isDir()) {
        directoryHandler(QDir(candidate.toFSPathString()).entryList(), matchLength);
        return true;
    }
    return false;
}

/*!
  \class Utils::FileInProjectFinder
  \inmodule QtCreator

  \brief The FileInProjectFinder class is a helper class to find the \e original
  file in the project directory for a given file URL.

  Often files are copied in the build and deploy process. findFile() searches for an existing file
  in the project directory for a given file path.

  For example, the following file paths should all be mapped to
  $PROJECTDIR/qml/app/main.qml:
  \list
  \li C:/app-build-desktop/qml/app/main.qml (shadow build directory)
  \li /Users/x/app-build-desktop/App.app/Contents/Resources/qml/App/main.qml (folder on macOS)
  \endlist
*/

FileInProjectFinder::FileInProjectFinder() = default;
FileInProjectFinder::~FileInProjectFinder() = default;

void FileInProjectFinder::setProjectDirectory(const FilePath &absoluteProjectPath)
{
    if (absoluteProjectPath == m_projectDir)
        return;

    QTC_CHECK(absoluteProjectPath.isEmpty()
              || (absoluteProjectPath.exists() && absoluteProjectPath.isAbsolutePath()));

    m_projectDir = absoluteProjectPath;
    m_cache.clear();
}

FilePath  FileInProjectFinder::projectDirectory() const
{
    return m_projectDir;
}

void FileInProjectFinder::setProjectFiles(const FilePaths &projectFiles)
{
    if (m_projectFiles == projectFiles)
        return;

    m_projectFiles = projectFiles;
    m_cache.clear();
    m_qrcUrlFinder.setProjectFiles(projectFiles);
}

void FileInProjectFinder::setSysroot(const FilePath &sysroot)
{
    if (m_sysroot == sysroot)
        return;

    m_sysroot = sysroot;
    m_cache.clear();
}

void FileInProjectFinder::addMappedPath(const FilePath &localFilePath, const QString &remoteFilePath)
{
    const QStringList segments = remoteFilePath.split('/', Qt::SkipEmptyParts);

    PathMappingNode *node = &m_pathMapRoot;
    for (const QString &segment : segments) {
        auto it = node->children.find(segment);
        if (it == node->children.end())
            it = node->children.insert(segment, PathMappingNode());
        node = &*it;
    }
    node->localPath = localFilePath;
}

/*!
  Returns the best match for the file URL \a fileUrl in the project directory.

  The function first checks whether the file inside the project directory exists.
  If not, the leading directory in the path is stripped, and the - now shorter - path is
  checked for existence, and so on. Second, it tries to locate the file in the sysroot
  folder specified. Third, it walks the list of project files and searches for a file name match
  there.

  If all fails, the function returns the original path from the file URL. To
  indicate that no match was found in the project, \a success is set to false.
  */
FilePaths FileInProjectFinder::findFile(const QUrl &fileUrl, bool *success) const
{
    qCDebug(finderLog) << "FileInProjectFinder: trying to find file" << fileUrl.toString() << "...";

    if (fileUrl.scheme() == "qrc" || fileUrl.toString().startsWith(':')) {
        const FilePaths result = m_qrcUrlFinder.find(fileUrl);
        if (!result.isEmpty()) {
            if (success)
                *success = true;
            return result;
        }
    }

    FilePath originalPath = FilePath::fromString(fileUrl.toLocalFile());
    if (originalPath.isEmpty()) // e.g. qrc://
        originalPath = FilePath::fromString(fileUrl.path());

    FilePaths result;
    bool found = findFileOrDirectory(originalPath, [&](const FilePath &fileName, int) {
        result << fileName;
    });
    if (!found)
        result << originalPath;

    if (success)
        *success = found;

    return result;
}

bool FileInProjectFinder::handleSuccess(const FilePath &originalPath, const FilePaths &found,
                                        int matchLength, const char *where) const
{
    qCDebug(finderLog) << "FileInProjectFinder: found" << found << where;
    CacheEntry entry;
    entry.paths = found;
    entry.matchLength = matchLength;
    m_cache.insert(originalPath, entry);
    return true;
}

bool FileInProjectFinder::checkRootDirectory(const FilePath &originalPath,
                                             DirectoryHandler directoryHandler) const
{
    if (!directoryHandler)
        return false;

    if (originalPath != m_projectDir || !originalPath.isDir())
        return false;

    const QString originalFSPath = originalPath.toFSPathString();
    const QStringList realEntries = QDir(originalFSPath)
                                        .entryList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);

    directoryHandler(realEntries, originalFSPath.size());
    return true;
}

bool FileInProjectFinder::checkMappedPath(const FilePath &originalPath,
                                          FileHandler fileHandler,
                                          DirectoryHandler directoryHandler) const
{
    const QString origFsPath = originalPath.toFSPathString();
    const auto segments = origFsPath.split('/', Qt::SkipEmptyParts);

    const PathMappingNode *node = &m_pathMapRoot;
    for (const QString &segment : segments) {
        auto it = node->children.find(segment);
        if (it == node->children.end()) {
            node = nullptr;
            break;
        }
        node = &*it;
    }
    if (!node)
        return false;

    const int origLength = origFsPath.size();

    if (!node->localPath.isEmpty()) {
        if (checkPath(node->localPath, origLength, fileHandler, directoryHandler)) {
            return handleSuccess(originalPath, {node->localPath}, origLength, "in mapped paths");
        }
    } else if (directoryHandler) {
        directoryHandler(node->children.keys(), origLength);
        qCDebug(finderLog) << "FileInProjectFinder: found virtual directory" << originalPath
                           << "in mapped paths";
        return true;
    }
    return false;
}

bool FileInProjectFinder::checkCache(const FilePath &originalPath,
                                     FileHandler fileHandler,
                                     DirectoryHandler directoryHandler) const
{
    auto it = m_cache.find(originalPath);
    if (it == m_cache.end())
        return false;

    qCDebug(finderLog) << "FileInProjectFinder: checking cache ...";
    CacheEntry &candidate = it.value();
    for (auto pathIt = candidate.paths.begin(); pathIt != candidate.paths.end();) {
        if (checkPath(*pathIt, candidate.matchLength, fileHandler, directoryHandler)) {
            qCDebug(finderLog) << "FileInProjectFinder: found" << *pathIt << "in the cache";
            ++pathIt;
        } else {
            pathIt = candidate.paths.erase(pathIt);
        }
    }

    if (!candidate.paths.empty())
        return true;

    m_cache.erase(it);
    return false;
}

bool FileInProjectFinder::checkProjectDirectory(const FilePath &originalPath,
                                                FileHandler fileHandler,
                                                DirectoryHandler directoryHandler) const
{
    if (m_projectDir.isEmpty())
        return false;

    qCDebug(finderLog) << "FileInProjectFinder: checking project directory ...";

    const QString origFsPath = originalPath.toFSPathString();
    const int origLength = origFsPath.size();
    const QChar separator = QLatin1Char('/');

    int prefixToIgnore = -1;
    if (originalPath.startsWith(m_projectDir.toFSPathString() + separator)) {
        if (originalPath.osType() == OsTypeMac) {
            // starting with the project path is not sufficient if the file was
            // copied in an insource build, e.g. into MyApp.app/Contents/Resources
            static const QString appResourcePath = QString::fromLatin1(".app/Contents/Resources");
            if (originalPath.contains(appResourcePath)) {
                // the path is inside the project, but most probably as a resource of an insource build
                // so ignore that path
                prefixToIgnore = origFsPath.indexOf(appResourcePath) + appResourcePath.length();
            }
        }
        if (prefixToIgnore == -1
            && checkPath(originalPath, origLength, fileHandler, directoryHandler)) {
            return handleSuccess(originalPath, {originalPath}, origLength, "in project directory");
        }
    }

    qCDebug(finderLog) << "FileInProjectFinder: checking stripped paths in project directory ...";

    // Strip directories one by one from the beginning of the path,
    // and see if the new relative path exists in the build directory.
    if (prefixToIgnore < 0) {
        if (!originalPath.isAbsolutePath() && !originalPath.startsWith(separator)) {
            prefixToIgnore = 0;
        } else {
            prefixToIgnore = origFsPath.indexOf(separator);
        }
    }

    // Repeatedly strip directories from the front
    while (prefixToIgnore != -1) {
        QString candidateString = origFsPath;
        candidateString.remove(0, prefixToIgnore);
        candidateString.prepend(m_projectDir.toUrlishString());
        const FilePath candidate = FilePath::fromString(candidateString);
        const int matchLength = origLength - prefixToIgnore;
        // FIXME: This might be a worse match than what we find later.
        if (checkPath(candidate, matchLength, fileHandler, directoryHandler)) {
            return handleSuccess(originalPath, {candidate}, matchLength, "in project directory");
        }
        prefixToIgnore = originalPath.toUrlishString().indexOf(separator, prefixToIgnore + 1);
    }
    return false;
}

bool FileInProjectFinder::checkProjectFiles(const FilePath &originalPath,
                                            FileHandler fileHandler,
                                            DirectoryHandler directoryHandler) const
{
    qCDebug(finderLog) << "FileInProjectFinder: checking project files ...";

    const QString lastSegment = originalPath.fileName();
    FilePaths matches;
    if (fileHandler)
        matches.append(filesWithSameFileName(lastSegment));
    if (directoryHandler)
        matches.append(pathSegmentsWithSameName(lastSegment));

    const FilePaths matchedFilePaths = bestMatches(matches, originalPath);
    if (matchedFilePaths.isEmpty())
        return false;

    const int matchLength = commonPostFixLength(matchedFilePaths.first(), originalPath);
    FilePaths hits;
    for (const FilePath &matchedFilePath : matchedFilePaths) {
        if (checkPath(matchedFilePath, matchLength, fileHandler, directoryHandler))
            hits.append(matchedFilePath);
    }
    if (hits.isEmpty())
        return false;

    return handleSuccess(originalPath, hits, matchLength, "when matching project files");
}

bool FileInProjectFinder::checkSearchPaths(const FilePath &originalPath,
                                           FileHandler fileHandler,
                                           DirectoryHandler directoryHandler) const
{
    for (const FilePath &dirPath : m_searchDirectories) {
        const CacheEntry found = findInSearchPath(dirPath, originalPath,
                                                  fileHandler, directoryHandler);

        if (!found.paths.isEmpty()) {
            return handleSuccess(originalPath, found.paths, found.matchLength,
                                 "in search path");
        }
    }
    return false;
}

bool FileInProjectFinder::checkSysroot(const FilePath &originalPath,
                                       FileHandler fileHandler,
                                       DirectoryHandler directoryHandler) const
{
    qCDebug(finderLog) << "FileInProjectFinder: checking absolute path in sysroot ...";

    if (m_sysroot.isEmpty())
        return false;

    const int origLength = originalPath.toFSPathString().length();
    const FilePath sysrootPath = m_sysroot.pathAppended(originalPath.toUrlishString());

    if (!checkPath(sysrootPath, origLength, fileHandler, directoryHandler))
        return false;

    return handleSuccess(originalPath, {sysrootPath}, origLength, "in sysroot");
}

bool FileInProjectFinder::findFileOrDirectory(const FilePath &originalPath,
                                              FileHandler fileHandler,
                                              DirectoryHandler directoryHandler) const
{
    if (originalPath.isEmpty()) {
        qCDebug(finderLog) << "FileInProjectFinder: malformed original path, returning";
        return false;
    }

    if (checkRootDirectory(originalPath, directoryHandler))
        return true;

    if (checkMappedPath(originalPath, fileHandler, directoryHandler))
        return true;

    if (checkCache(originalPath, fileHandler, directoryHandler))
        return true;

    if (checkProjectDirectory(originalPath, fileHandler, directoryHandler))
        return true;

    if (checkProjectFiles(originalPath, fileHandler, directoryHandler))
        return true;

    if (checkSearchPaths(originalPath, fileHandler, directoryHandler))
        return true;

    if (checkSysroot(originalPath, fileHandler, directoryHandler))
        return true;

    qCDebug(finderLog) << "FileInProjectFinder: couldn't find file!";
    return false;
}

static QString chopFirstDir(const QString &dirPath)
{
    int i = dirPath.indexOf(QLatin1Char('/'));
    if (i == -1)
        return QString();
    else
        return dirPath.mid(i + 1);
}

FileInProjectFinder::CacheEntry FileInProjectFinder::findInSearchPath(
        const FilePath &searchPath, const FilePath &filePath,
        FileHandler fileHandler, DirectoryHandler directoryHandler)
{
    qCDebug(finderLog) << "FileInProjectFinder: checking search path" << searchPath;

    QString s = filePath.toFSPathString();
    while (!s.isEmpty()) {
        CacheEntry result;
        result.paths << searchPath / s;
        result.matchLength = s.length() + 1;
        qCDebug(finderLog) << "FileInProjectFinder: trying" << result.paths.first();

        if (checkPath(result.paths.first(), result.matchLength, fileHandler, directoryHandler))
            return result;

        QString next = chopFirstDir(s);
        if (next.isEmpty()) {
            if (directoryHandler && searchPath.fileName() == s) {
                result.paths = {searchPath};
                directoryHandler(QDir(searchPath.toFSPathString()).entryList(),
                                 result.matchLength);
                return result;
            }
            break;
        }
        s = next;
    }

    return CacheEntry();
}

FilePaths FileInProjectFinder::filesWithSameFileName(const QString &fileName) const
{
    FilePaths result;
    for (const FilePath &f : m_projectFiles) {
        if (f.fileName() == fileName)
            result << f;
    }
    return result;
}

FilePaths FileInProjectFinder::pathSegmentsWithSameName(const QString &pathSegment) const
{
    FilePaths result;
    for (const FilePath &f : m_projectFiles) {
        FilePath currentPath = f.parentDir();
        do {
            if (currentPath.fileName() == pathSegment) {
                if (result.isEmpty() || result.last() != currentPath)
                    result.append(currentPath);
            }
            currentPath = currentPath.parentDir();
        } while (!currentPath.isEmpty());
    }
    FilePath::removeDuplicates(result);
    return result;
}

int FileInProjectFinder::commonPostFixLength(const FilePath &candidatePath,
                                             const FilePath &filePathToFind)
{
    int rank = 0;
    const QStringView candidate = candidatePath.pathView();
    const QStringView needle = filePathToFind.pathView();
    for (int a = candidate.size(), b = needle.size();
             --a >= 0 && --b >= 0 && candidate.at(a) == needle.at(b);)
        rank++;
    return rank;
}

FilePaths FileInProjectFinder::bestMatches(const FilePaths &filePaths,
                                           const FilePath &filePathToFind)
{
    if (filePaths.isEmpty())
        return {};
    if (filePaths.length() == 1) {
        qCDebug(finderLog) << "FileInProjectFinder: found" << filePaths.first()
                           << "in project files";
        return filePaths;
    }
    int bestRank = -1;
    FilePaths bestFilePaths;
    for (const FilePath &fp : filePaths) {
        const int currentRank = commonPostFixLength(fp, filePathToFind);
        if (currentRank < bestRank)
            continue;
        if (currentRank > bestRank) {
            bestRank = currentRank;
            bestFilePaths.clear();
        }
        bestFilePaths << fp;
    }
    QTC_CHECK(!bestFilePaths.empty());
    return bestFilePaths;
}

FilePaths FileInProjectFinder::searchDirectories() const
{
    return m_searchDirectories;
}

void FileInProjectFinder::setAdditionalSearchDirectories(const FilePaths &searchDirectories)
{
    m_searchDirectories = searchDirectories;
}

FilePaths FileInProjectFinder::QrcUrlFinder::find(const QUrl &fileUrl) const
{
    const auto fileIt = m_fileCache.constFind(fileUrl);
    if (fileIt != m_fileCache.cend())
        return fileIt.value();
    FilePaths result;
    for (const FilePath &f : m_allQrcFiles) {
        QrcParser::Ptr &qrcParser = m_parserCache[f];
        if (!qrcParser)
            qrcParser = QrcParser::parseQrcFile(f, QString());
        if (!qrcParser->isValid())
            continue;
        qrcParser->collectFilesAtPath(QrcParser::normalizedQrcFilePath(fileUrl.toString()), &result);
    }
    FilePath::removeDuplicates(result);
    m_fileCache.insert(fileUrl, result);
    return result;
}

void FileInProjectFinder::QrcUrlFinder::setProjectFiles(const FilePaths &projectFiles)
{
    m_allQrcFiles = filtered(projectFiles, [](const FilePath &f) { return f.endsWith(".qrc"); });
    m_fileCache.clear();
    m_parserCache.clear();
}

FilePath chooseFileFromList(const FilePaths &candidates)
{
    if (candidates.length() == 1)
        return candidates.first();
    QMenu filesMenu;
    for (const FilePath &candidate : candidates)
        filesMenu.addAction(candidate.toUserOutput());
    if (const QAction * const action = filesMenu.exec(QCursor::pos()))
        return FilePath::fromUserInput(action->text());
    return {};
}

} // namespace Utils

// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qrcparser.h"

#include "filepath.h"
#include "qtcassert.h"

#include <qmljs/qmljstr.h> // Yes, the translations are still there

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QLoggingCategory>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QWriteLocker>

static Q_LOGGING_CATEGORY(qrcParserLog, "qtc.qrcParser", QtWarningMsg)

namespace Utils {

namespace Internal {

class QrcParserPrivate
{
public:
    QrcParserPrivate(QrcParser *q);

    bool parseFile(const FilePath &path, const QString &contents);
    FilePath firstFileAtPath(const QString &path, const QLocale &locale) const;
    void collectFilesAtPath(
        const QString &path, FilePaths *res, const QLocale *locale = nullptr) const;
    bool hasDirAtPath(const QString &path, const QLocale *locale = nullptr) const;
    void collectFilesInPath(
        const QString &path,
        QMap<QString, FilePaths> *res,
        bool addDirs = false,
        const QLocale *locale = nullptr) const;
    void collectResourceFilesForSourceFile(
        const FilePath &sourceFile, QStringList *res, const QLocale *locale = nullptr) const;
    QrcParser::MatchResult longestReverseMatches(const QString &) const;

    QStringList errorMessages() const;
    QStringList languages() const;

private:
    static QString fixPrefix(const QString &prefix);
    const QStringList allUiLanguages(const QLocale *locale) const;

    QMap<QString, FilePaths> m_resources;
    QMap<QString, FilePaths> m_reverseResources;
    QMap<FilePath, QStringList> m_files;
    QStringList m_languages;
    QStringList m_errorMessages;
};

class QrcCachePrivate
{
public:
    QrcCachePrivate(QrcCache *q);

    QrcParser::Ptr addPath(const FilePath &path, const QString &contents);
    void removePath(const FilePath &path);
    QrcParser::Ptr updatePath(const FilePath &path, const QString &contents);
    QrcParser::Ptr parsedPath(const FilePath &path);
    void clear();

private:
    QHash<FilePath, QPair<QrcParser::Ptr,int> > m_cache;
    QReadWriteLock m_mutex;
};
} // namespace Internal

/*!
    \class Utils::QrcParser
    \inmodule QtCreator
    \brief The QrcParser class parses one or more QRC files and keeps their
    content cached.

    A \l{The Qt Resource System}{Qt resource collection (QRC)} contains files
    read from the file system but organized in a possibly different way.
    To easily describe that with a simple structure, we use a map from QRC paths
    to the paths in the filesystem.
    By using a map, we can easily find all QRC paths that start with a given
    prefix, and thus loop on a QRC directory.

    QRC files also support languages, which are mapped to a prefix of the QRC
    path. For example, the French /image/bla.png (lang=fr) will have the path
    \c {fr/image/bla.png}. The empty language represents the default resource.
    Languages are looked up using the locale uiLanguages() property

    For a single QRC, a given path maps to a single file, but when one has
    multiple (platform-specific and mutually exclusive) QRC files, multiple
    files match, so QStringList are used.

    Especially, the \c collect* functions are thought of as low level interface.
 */

/*!
    \typedef Utils::QrcParser::Ptr
    Represents pointers.
 */

/*!
    \typedef Utils::QrcParser::ConstPtr
    Represents constant pointers.
*/

/*!
    Normalizes the \a path to a file in a QRC resource by dropping the \c qrc:/
    or \c : and any extra slashes in the beginning.
 */
QString QrcParser::normalizedQrcFilePath(const QString &path) {
    QString normPath = path;
    int endPrefix = 0;
    if (path.startsWith(QLatin1String("qrc:/")))
        endPrefix = 4;
    else if (path.startsWith(QLatin1String(":/")))
        endPrefix = 1;
    if (endPrefix < path.size() && path.at(endPrefix) == QLatin1Char('/'))
        while (endPrefix + 1 < path.size() && path.at(endPrefix+1) == QLatin1Char('/'))
            ++endPrefix;
    normPath = path.right(path.size()-endPrefix);
    if (!normPath.startsWith(QLatin1Char('/')))
        normPath.insert(0, QLatin1Char('/'));
    return normPath;
}

/*!
    Returns the path to a directory normalized to \a path in a QRC resource by
    dropping the \c qrc:/ or \c : and any extra slashes at the beginning, and
    by ensuring that the path ends with a slash
 */
QString QrcParser::normalizedQrcDirectoryPath(const QString &path) {
    QString normPath = normalizedQrcFilePath(path);
    if (!normPath.endsWith(QLatin1Char('/')))
        normPath.append(QLatin1Char('/'));
    return normPath;
}

/*!
    Returns the QRC directory path for \a file.
*/
QString QrcParser::qrcDirectoryPathForQrcFilePath(const QString &file)
{
    return file.left(file.lastIndexOf(QLatin1Char('/')));
}

QrcParser::QrcParser()
{
    d = new Internal::QrcParserPrivate(this);
}

/*!
    \internal
*/
QrcParser::~QrcParser()
{
    delete d;
}

/*!
    Parses the QRC file at \a path. If \a contents is not empty, it is used as
    the file contents instead of reading it from the file system.

    Returns whether the parsing succeeded.

    \sa errorMessages(), parseQrcFile()
*/
bool QrcParser::parseFile(const FilePath &path, const QString &contents)
{
    return d->parseFile(path, contents);
}

/*!
    Returns the file system path of the first (active) file at the given QRC
    \a path and \a locale.
 */
FilePath QrcParser::firstFileAtPath(const QString &path, const QLocale &locale) const
{
    return d->firstFileAtPath(path, locale);
}

/*!
    Adds the file system paths for the given QRC \a path to \a res.

    If \a locale is null, all possible files are added. Otherwise, just
    the first one that matches the locale is added.
 */
void QrcParser::collectFilesAtPath(const QString &path, FilePaths *res, const QLocale *locale) const
{
    d->collectFilesAtPath(path, res, locale);
}

QrcParser::MatchResult QrcParser::longestReverseMatches(const QString &p) const
{
    return d->longestReverseMatches(p);
}

/*!
    Returns \c true if \a path is a non-empty directory and matches \a locale.

 */
bool QrcParser::hasDirAtPath(const QString &path, const QLocale *locale) const
{
    return d->hasDirAtPath(path, locale);
}

/*!
    Adds the directory contents of the given QRC \a path to \a res if \a addDirs
    is set to \c true.

    Adds the QRC filename to file system path associations contained in the
    given \a path to \a res. If addDirs() is \c true, directories are also
    added.

    If \a locale is null, all possible files are added. Otherwise, just the
    first file with a matching the locale is added.
 */
void QrcParser::collectFilesInPath(const QString &path, QMap<QString, FilePaths> *res, bool addDirs,
                                   const QLocale *locale) const
{
    d->collectFilesInPath(path, res, addDirs, locale);
}

/*!
    Adds the resource files from the QRC file \a sourceFile to \a res.

    If \a locale is null, all possible files are added. Otherwise, just
    the first file with a matching the locale is added.
 */
void QrcParser::collectResourceFilesForSourceFile(
    const FilePath &sourceFile, QStringList *res, const QLocale *locale) const
{
    d->collectResourceFilesForSourceFile(sourceFile, res, locale);
}

/*!
    Returns the errors found while parsing.
 */
QStringList QrcParser::errorMessages() const
{
    return d->errorMessages();
}

/*!
    Returns all languages used in this QRC.
 */
QStringList QrcParser::languages() const
{
    return d->languages();
}

/*!
    Indicates whether the QRC contents are valid.

    Returns an error if the QRC is empty.
 */
bool QrcParser::isValid() const
{
    return errorMessages().isEmpty();
}

/*!
    Returns the \a contents of the QRC file at \a path.
*/
QrcParser::Ptr QrcParser::parseQrcFile(const FilePath &path, const QString &contents)
{
    Ptr res(new QrcParser);
    if (!path.isEmpty())
        res->parseFile(path, contents);
    return res;
}

// ----------------

/*!
    \class Utils::QrcCache
    \inmodule QtCreator
    \brief The QrcCache class caches the contents of parsed QRC files.

    \sa Utils::QrcParser
*/

QrcCache::QrcCache()
{
    d = new Internal::QrcCachePrivate(this);
}

/*!
    \internal
*/
QrcCache::~QrcCache()
{
    delete d;
}

/*!
    Parses the QRC file at \a path and caches the parser. If \a contents is not
    empty, it is used as the file contents instead of reading it from the file
    system.

    Returns whether the parsing succeeded.

    \sa QrcParser::errorMessages(), QrcParser::parseQrcFile()
*/
QrcParser::ConstPtr QrcCache::addPath(const FilePath &path, const QString &contents)
{
    return d->addPath(path, contents);
}

/*!
    Removes \a path from the cache.
*/
void QrcCache::removePath(const FilePath &path)
{
    d->removePath(path);
}

/*!
    Reparses the QRC file at \a path and returns the \a contents of the file.
*/
QrcParser::ConstPtr QrcCache::updatePath(const FilePath &path, const QString &contents)
{
    return d->updatePath(path, contents);
}

/*!
    Returns the cached QRC parser for the QRC file at \a path.
*/
QrcParser::ConstPtr QrcCache::parsedPath(const FilePath &path)
{
    return d->parsedPath(path);
}

/*!
    Clears the contents of the cache.
*/
void QrcCache::clear()
{
    d->clear();
}

// --------------------

namespace Internal {

QrcParserPrivate::QrcParserPrivate(QrcParser *)
{ }

bool QrcParserPrivate::parseFile(const FilePath &path, const QString &contents)
{
    QDomDocument doc;
    FilePath baseDir = path.parentDir();

    if (contents.isEmpty()) {
        // Regular file
        QFile file(path.toFSPathString());
        if (!file.open(QIODevice::ReadOnly)) {
            m_errorMessages.append(file.errorString());
            return false;
        }

        QString error_msg;
        int error_line, error_col;
        if (!doc.setContent(&file, &error_msg, &error_line, &error_col)) {
            m_errorMessages.append(QmlJS::Tr::tr("XML error on line %1, col %2: %3")
                                   .arg(error_line).arg(error_col).arg(error_msg));
            return false;
        }
    } else {
        // Virtual file from qmake evaluator
        QString error_msg;
        int error_line, error_col;
        if (!doc.setContent(contents, &error_msg, &error_line, &error_col)) {
            m_errorMessages.append(QmlJS::Tr::tr("XML error on line %1, col %2: %3")
                                   .arg(error_line).arg(error_col).arg(error_msg));
            return false;
        }
    }

    QDomElement root = doc.firstChildElement(QLatin1String("RCC"));
    if (root.isNull()) {
        m_errorMessages.append(QmlJS::Tr::tr("The <RCC> root element is missing."));
        return false;
    }

    QDomElement relt = root.firstChildElement(QLatin1String("qresource"));
    for (; !relt.isNull(); relt = relt.nextSiblingElement(QLatin1String("qresource"))) {

        QString prefix = fixPrefix(relt.attribute(QLatin1String("prefix")));
        const QString language = relt.attribute(QLatin1String("lang"));
        if (!m_languages.contains(language))
            m_languages.append(language);

        QDomElement felt = relt.firstChildElement(QLatin1String("file"));
        for (; !felt.isNull(); felt = felt.nextSiblingElement(QLatin1String("file"))) {
            const QString fileName = felt.text();
            const QString alias = felt.attribute(QLatin1String("alias"));
            FilePath filePath = baseDir.resolvePath(fileName);
            QString accessPath;
            if (!alias.isEmpty())
                accessPath = language + prefix + alias;
            else
                accessPath = language + prefix + fileName;
            FilePaths &resources = m_resources[accessPath];
            if (!resources.contains(filePath)) {
                resources.append(filePath);
                QString reversePath(accessPath);
                std::reverse(reversePath.begin(), reversePath.end());
                if (!reversePath.endsWith('/'))
                    reversePath.append('/');
                m_reverseResources[reversePath].append(filePath);
            }
            QStringList &files = m_files[filePath];
            if (!files.contains(accessPath))
                files.append(accessPath);
        }
    }
    return true;
}

// path is assumed to be a normalized absolute path
FilePath QrcParserPrivate::firstFileAtPath(const QString &path, const QLocale &locale) const
{
    QTC_CHECK(path.startsWith(QLatin1Char('/')));
    for (const QString &language : allUiLanguages(&locale)) {
        if (m_languages.contains(language)) {
            auto res = m_resources.find(language + path);
            if (res != m_resources.end())
                return res.value().at(0);
        }
    }
    return {};
}

void QrcParserPrivate::collectFilesAtPath(const QString &path, FilePaths *files,
                                          const QLocale *locale) const
{
    QTC_CHECK(path.startsWith(QLatin1Char('/')));
    for (const QString &language : allUiLanguages(locale)) {
        if (m_languages.contains(language)) {
            auto res = m_resources.find(language + path);
            if (res != m_resources.end())
                (*files) << res.value();
        }
    }
}

// path is expected to be normalized and start and end with a slash
bool QrcParserPrivate::hasDirAtPath(const QString &path, const QLocale *locale) const
{
    QTC_CHECK(path.startsWith(QLatin1Char('/')));
    QTC_CHECK(path.endsWith(QLatin1Char('/')));
    for (const QString &language : allUiLanguages(locale)) {
        if (m_languages.contains(language)) {
            QString key = language + path;
            auto res = m_resources.lowerBound(key);
            if (res != m_resources.end() && res.key().startsWith(key))
                return true;
        }
    }
    return false;
}

void QrcParserPrivate::collectFilesInPath(const QString &path, QMap<QString, FilePaths> *contents,
                                          bool addDirs, const QLocale *locale) const
{
    QTC_CHECK(path.startsWith(QLatin1Char('/')));
    QTC_CHECK(path.endsWith(QLatin1Char('/')));
    auto end = m_resources.end();
    for (const QString &language : allUiLanguages(locale)) {
        QString key = language + path;
        auto res = m_resources.lowerBound(key);
        while (res != end && res.key().startsWith(key)) {
            const QString &actualKey = res.key();
            int endDir = actualKey.indexOf(QLatin1Char('/'), key.size());
            if (endDir == -1) {
                QString fileName = res.key().right(res.key().size()-key.size());
                FilePaths &els = (*contents)[fileName];
                for (const FilePath &val : res.value())
                    if (!els.contains(val))
                        els << val;
                ++res;
            } else {
                QString dirName = res.key().mid(key.size(), endDir - key.size() + 1);
                if (addDirs)
                    contents->insert(dirName, {});
                QString key2 = key + dirName;
                do {
                    ++res;
                } while (res != end && res.key().startsWith(key2));
            }
        }
    }
}

void QrcParserPrivate::collectResourceFilesForSourceFile(const FilePath &sourceFile,
                                                         QStringList *results,
                                                         const QLocale *locale) const
{
    // TODO: use FileName from fileutils for file paths

    const QStringList langs = allUiLanguages(locale);
    auto file = m_files.find(sourceFile);
    if (file == m_files.end())
        return;
    for (const QString &resource : file.value()) {
        for (const QString &language : langs) {
            if (resource.startsWith(language) && !results->contains(resource))
                results->append(resource);
        }
    }
}

QrcParser::MatchResult QrcParserPrivate::longestReverseMatches(const QString &reversePath) const
{
    QrcParser::MatchResult res;
    if (reversePath.length() == 1)
        return res;
    auto lastMatch = m_reverseResources.end();
    qsizetype matchedUntil = 0;
    for (qsizetype i = 1, j = 0; i < reversePath.size(); i = j + 1) {
        j = reversePath.indexOf(u'/', i);
        if (j == -1)
            j = reversePath.size() - 1;
        auto match = m_reverseResources.lowerBound(reversePath.mid(0, j + 1));
        QString pNow = reversePath.left(j + 1);
        if (match == m_reverseResources.end() || match.key().left(j + 1) != pNow)
            break;
        ++res.matchDepth;
        matchedUntil = j + 1;
        lastMatch = match;
    }
    res.reversedPaths.clear();
    res.sourceFiles.clear();
    for (auto it = lastMatch; it != m_reverseResources.end()
                              && it.key().left(matchedUntil) == reversePath.left(matchedUntil);
         ++it) {
        res.reversedPaths.append(it.key());
        for (const FilePath &filePath : it.value())
            res.sourceFiles.append(filePath);
    }
    return res;
}

QStringList QrcParserPrivate::errorMessages() const
{
    return m_errorMessages;
}

QStringList QrcParserPrivate::languages() const
{
    return m_languages;
}

QString QrcParserPrivate::fixPrefix(const QString &prefix)
{
    const QChar slash = QLatin1Char('/');
    QString result = QString(slash);
    for (int i = 0; i < prefix.size(); ++i) {
        const QChar c = prefix.at(i);
        if (c == slash && result.at(result.size() - 1) == slash)
            continue;
        result.append(c);
    }

    if (!result.endsWith(slash))
        result.append(slash);

    return result;
}

const QStringList QrcParserPrivate::allUiLanguages(const QLocale *locale) const
{
    if (!locale)
        return languages();
    bool hasEmptyString = false;
    const QStringList langs = locale->uiLanguages();
    QStringList allLangs = langs;
    for (const QString &language : langs) {
        if (language.isEmpty())
            hasEmptyString = true;
        else if (language.contains('_') || language.contains('-')) {
            const QStringList splits = QString(language).replace('_', '-').split('-');
            if (splits.size() > 1 && !allLangs.contains(splits.at(0)))
                allLangs.append(splits.at(0));
        }
    }
    if (!hasEmptyString)
        allLangs.append(QString());
    return allLangs;
}

// ----------------

QrcCachePrivate::QrcCachePrivate(QrcCache *)
{ }

QrcParser::Ptr QrcCachePrivate::addPath(const FilePath &path, const QString &contents)
{
    QPair<QrcParser::Ptr,int> currentValue;
    {
        QWriteLocker l(&m_mutex);
        currentValue = m_cache.value(path, {QrcParser::Ptr(nullptr), 0});
        currentValue.second += 1;
        if (currentValue.second > 1) {
            m_cache.insert(path, currentValue);
            return currentValue.first;
        }
    }
    QrcParser::Ptr newParser = QrcParser::parseQrcFile(path, contents);
    if (!newParser->isValid())
        qCWarning(qrcParserLog) << "adding invalid qrc " << path << " to the cache:" << newParser->errorMessages();
    {
        QWriteLocker l(&m_mutex);
        QPair<QrcParser::Ptr, int> currentValue = m_cache.value(path, {{}, 0});
        if (!currentValue.first)
            currentValue.first = newParser;
        currentValue.second += 1;
        m_cache.insert(path, currentValue);
        return currentValue.first;
    }
}

void QrcCachePrivate::removePath(const FilePath &path)
{
    QPair<QrcParser::Ptr,int> currentValue;
    {
        QWriteLocker l(&m_mutex);
        currentValue = m_cache.value(path, {QrcParser::Ptr(nullptr), 0});
        if (currentValue.second == 1) {
            m_cache.remove(path);
        } else if (currentValue.second > 1) {
            currentValue.second -= 1;
            m_cache.insert(path, currentValue);
        } else {
            QTC_CHECK(!m_cache.contains(path));
        }
    }
}

QrcParser::Ptr QrcCachePrivate::updatePath(const FilePath &path, const QString &contents)
{
    QrcParser::Ptr newParser = QrcParser::parseQrcFile(path, contents);
    {
        QWriteLocker l(&m_mutex);
        QPair<QrcParser::Ptr,int> currentValue = m_cache.value(path, {QrcParser::Ptr(nullptr), 0});
        currentValue.first = newParser;
        if (currentValue.second == 0)
            currentValue.second = 1; // add qrc files that are not in the resources of a project
        m_cache.insert(path, currentValue);
        return currentValue.first;
    }
}

QrcParser::Ptr QrcCachePrivate::parsedPath(const FilePath &path)
{
    QReadLocker l(&m_mutex);
    QPair<QrcParser::Ptr,int> currentValue = m_cache.value(path, {QrcParser::Ptr(nullptr), 0});
    return currentValue.first;
}

void QrcCachePrivate::clear()
{
    QWriteLocker l(&m_mutex);
    m_cache.clear();
}

} // namespace Internal
} // namespace Utils

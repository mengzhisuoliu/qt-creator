// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "commandline.h"

#include "environment.h"
#include "macroexpander.h"
#include "qtcassert.h"
#include "stringutils.h"

#include <QDebug>
#include <QDir>
#include <QRegularExpression>
#include <QStack>

// The main state of the Unix shell parser
enum MxQuoting { MxBasic, MxSingleQuote, MxDoubleQuote, MxParen, MxSubst, MxGroup, MxMath };

struct MxState
{
    MxQuoting current;
    // Bizarrely enough, double quoting has an impact on the behavior of some
    // complex expressions within the quoted string.
    bool dquote;
};
QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(MxState, Q_PRIMITIVE_TYPE);
QT_END_NAMESPACE

// Pushed state for the case where a $(()) expansion turns out bogus
struct MxSave
{
    QString str;
    int pos, varPos;
};
QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(MxSave, Q_MOVABLE_TYPE);
QT_END_NAMESPACE

namespace Utils {

/*!
    \class Utils::ProcessArgs
    \inmodule QtCreator

    \brief Handles shell-quoted process arguments.
*/

inline static bool isMetaCharWin(ushort c)
{
    static const uchar iqm[] = {
        0x00, 0x00, 0x00, 0x00, 0x40, 0x03, 0x00, 0x50,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    }; // &()<>|

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

static void envExpandWin(QString &args, const Environment *env, const QString &pwd)
{
    static const QString cdName = QLatin1String("CD");
    int off = 0;
  next:
    for (int prev = -1, that;
         (that = args.indexOf(QLatin1Char('%'), off)) >= 0;
         prev = that, off = that + 1) {
        if (prev >= 0) {
            const QString var = args.mid(prev + 1, that - prev - 1).toUpper();
            const QString val = (var == cdName && !pwd.isEmpty())
                                    ? QDir::toNativeSeparators(pwd) : env->expandedValueForKey(var);
            if (!val.isEmpty()) { // Empty values are impossible, so this is an existence check
                args.replace(prev, that - prev + 1, val);
                off = prev + val.length();
                goto next;
            }
        }
    }
}

static QString prepareArgsWin(const QString &_args, ProcessArgs::SplitError *err,
                              const Environment *env, const QString &pwd)
{
    QString args = _args;

    if (env) {
        envExpandWin(args, env, pwd);
    } else {
        if (args.indexOf(QLatin1Char('%')) >= 0) {
            if (err)
                *err = ProcessArgs::FoundMeta;
            return {};
        }
    }

    if (!args.isEmpty() && args.unicode()[0].unicode() == '@')
        args.remove(0, 1);

    for (int p = 0; p < args.length(); p++) {
        ushort c = args.unicode()[p].unicode();
        if (c == '^') {
            args.remove(p, 1);
        } else if (c == '"') {
            do {
                if (++p == args.length())
                    break; // For cmd, this is no error.
            } while (args.unicode()[p].unicode() != '"');
        } else if (isMetaCharWin(c)) {
            if (err)
                *err = ProcessArgs::FoundMeta;
            return {};
        }
    }

    if (err)
        *err = ProcessArgs::SplitOk;
    return args;
}

inline static bool isWhiteSpaceWin(ushort c)
{
    return c == ' ' || c == '\t';
}

static QStringList doSplitArgsWin(const QString &args, ProcessArgs::SplitError *err)
{
    QStringList ret;

    if (err)
        *err = ProcessArgs::SplitOk;

    int p = 0;
    const int length = args.length();
    forever {
        forever {
            if (p == length)
                return ret;
            if (!isWhiteSpaceWin(args.unicode()[p].unicode()))
                break;
            ++p;
        }

        QString arg;
        bool inquote = false;
        forever {
            bool copy = true; // copy this char
            int bslashes = 0; // number of preceding backslashes to insert
            while (p < length && args.unicode()[p] == QLatin1Char('\\')) {
                ++p;
                ++bslashes;
            }
            if (p < length && args.unicode()[p] == QLatin1Char('"')) {
                if (!(bslashes & 1)) {
                    // Even number of backslashes, so the quote is not escaped.
                    if (inquote) {
                        if (p + 1 < length && args.unicode()[p + 1] == QLatin1Char('"')) {
                            // This is not documented on MSDN.
                            // Two consecutive quotes make a literal quote. Brain damage:
                            // this still closes the quoting, so a 3rd quote is required,
                            // which makes the runtime's quoting run out of sync with the
                            // shell's one unless the 2nd quote is escaped.
                            ++p;
                        } else {
                            // Closing quote
                            copy = false;
                        }
                        inquote = false;
                    } else {
                        // Opening quote
                        copy = false;
                        inquote = true;
                    }
                }
                bslashes >>= 1;
            }

            while (--bslashes >= 0)
                arg.append(QLatin1Char('\\'));

            if (p == length || (!inquote && isWhiteSpaceWin(args.unicode()[p].unicode()))) {
                ret.append(arg);
                if (inquote) {
                    if (err)
                        *err = ProcessArgs::BadQuoting;
                    return {};
                }
                break;
            }

            if (copy)
                arg.append(args.unicode()[p]);
            ++p;
        }
    }
    //not reached
}

/*!
    \internal
    Splits \a _args according to system shell word splitting and quoting rules.

    \section1 Unix

    The behavior is based on the POSIX shell and bash:
    \list
    \li Whitespace splits tokens.
    \li The backslash quotes the following character.
    \li A string enclosed in single quotes is not split. No shell meta
        characters are interpreted.
    \li A string enclosed in double quotes is not split. Within the string,
        the backslash quotes shell meta characters - if it is followed
        by a "meaningless" character, the backslash is output verbatim.
    \endlist
    If \a abortOnMeta is \c false, only the splitting and quoting rules apply,
    while other meta characters (substitutions, redirections, etc.) are ignored.
    If \a abortOnMeta is \c true, encounters of unhandled meta characters are
    treated as errors.

    If \a err is not NULL, stores a status code at the pointer target. For more
    information, see \l SplitError.

    If \a env is not NULL, performs variable substitution with the
    given environment.

    Returns a list of unquoted words or an empty list if an error occurred.

    \section1 Windows

    The behavior is defined by the Microsoft C runtime:
    \list
    \li Whitespace splits tokens.
    \li A string enclosed in double quotes is not split.
    \list
        \li 3N double quotes within a quoted string yield N literal quotes.
           This is not documented on MSDN.
    \endlist
    \li Backslashes have special semantics if they are followed by a double quote:
    \list
        \li 2N backslashes + double quote => N backslashes and begin/end quoting
        \li 2N+1 backslashes + double quote => N backslashes + literal quote
    \endlist
    \endlist
    Qt and many other implementations comply with this standard, but many do not.

    If \a abortOnMeta is \c true, cmd shell semantics are applied before
    proceeding with word splitting:
    \list
    \li Cmd ignores \e all special chars between double quotes.
        Note that the quotes are \e not removed at this stage - the
        tokenization rules described above still apply.
    \li The \c circumflex is the escape char for everything including itself.
    \endlist
    As the quoting levels are independent from each other and have different
    semantics, you need a command line like \c{"foo "\^"" bar"} to get
    \c{foo " bar}.
 */

static QStringList splitArgsWin(const QString &_args, bool abortOnMeta,
                                ProcessArgs::SplitError *err,
                                const Environment *env, const QString &pwd)
{
    if (abortOnMeta) {
        ProcessArgs::SplitError perr;
        if (!err)
            err = &perr;
        QString args = prepareArgsWin(_args, &perr, env, pwd);
        if (*err != ProcessArgs::SplitOk)
            return {};
        return doSplitArgsWin(args, err);
    } else {
        QString args = _args;
        if (env)
            envExpandWin(args, env, pwd);
        return doSplitArgsWin(args, err);
    }
}


static bool isMetaUnix(QChar cUnicode)
{
    static const uchar iqm[] = {
        0x00, 0x00, 0x00, 0x00, 0xdc, 0x07, 0x00, 0xd8,
        0x00, 0x00, 0x00, 0x38, 0x01, 0x00, 0x00, 0x38
    }; // \'"$`<>|;&(){}*?#[]

    uint c = cUnicode.unicode();

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

static QStringList splitArgsUnix(const QString &args, bool abortOnMeta,
                                 ProcessArgs::SplitError *err,
                                 const Environment *env, const QString &pwd)
{
    static const QString pwdName = QLatin1String("PWD");
    QStringList ret;

    for (int pos = 0; ; ) {
        QChar c;
        do {
            if (pos >= args.length())
                goto okret;
            c = args.unicode()[pos++];
        } while (c.isSpace());
        QString cret;
        bool hadWord = false;
        if (c == QLatin1Char('~')) {
            if (pos >= args.length()
                || args.unicode()[pos].isSpace() || args.unicode()[pos] == QLatin1Char('/')) {
                cret = QDir::homePath();
                hadWord = true;
                goto getc;
            } else if (abortOnMeta) {
                goto metaerr;
            }
        }
        do {
            if (c == QLatin1Char('\'')) {
                int spos = pos;
                do {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                } while (c != QLatin1Char('\''));
                cret += args.mid(spos, pos - spos - 1);
                hadWord = true;
            } else if (c == QLatin1Char('"')) {
                for (;;) {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                  nextq:
                    if (c == QLatin1Char('"'))
                        break;
                    if (c == QLatin1Char('\\')) {
                        if (pos >= args.length())
                            goto quoteerr;
                        c = args.unicode()[pos++];
                        if (c != QLatin1Char('"') &&
                            c != QLatin1Char('\\') &&
                            !(abortOnMeta &&
                              (c == QLatin1Char('$') ||
                               c == QLatin1Char('`'))))
                            cret += QLatin1Char('\\');
                    } else if (c == QLatin1Char('$') && env) {
                        if (pos >= args.length())
                            goto quoteerr;
                        c = args.unicode()[pos++];
                        bool braced = false;
                        if (c == QLatin1Char('{')) {
                            if (pos >= args.length())
                                goto quoteerr;
                            c = args.unicode()[pos++];
                            braced = true;
                        }
                        QString var;
                        while (c.isLetterOrNumber() || c == QLatin1Char('_')) {
                            var += c;
                            if (pos >= args.length())
                                goto quoteerr;
                            c = args.unicode()[pos++];
                        }
                        if (var == pwdName && !pwd.isEmpty()) {
                            cret += pwd;
                        } else {
                            const Environment::FindResult res = env->find(var);
                            if (!res) {
                                if (abortOnMeta)
                                    goto metaerr; // Assume this is a shell builtin
                            } else {
                                cret += env->expandedValueForKey(res->key);
                            }
                        }
                        if (!braced)
                            goto nextq;
                        if (c != QLatin1Char('}')) {
                            if (abortOnMeta)
                                goto metaerr; // Assume this is a complex expansion
                            goto quoteerr; // Otherwise it's just garbage
                        }
                        continue;
                    } else if (abortOnMeta &&
                               (c == QLatin1Char('$') ||
                                c == QLatin1Char('`'))) {
                        goto metaerr;
                    }
                    cret += c;
                }
                hadWord = true;
            } else if (c == QLatin1Char('$') && env) {
                if (pos >= args.length())
                    goto quoteerr; // Bash just takes it verbatim, but whatever
                c = args.unicode()[pos++];
                bool braced = false;
                if (c == QLatin1Char('{')) {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                    braced = true;
                }
                QString var;
                while (c.isLetterOrNumber() || c == QLatin1Char('_')) {
                    var += c;
                    if (pos >= args.length()) {
                        if (braced)
                            goto quoteerr;
                        c = QLatin1Char(' ');
                        break;
                    }
                    c = args.unicode()[pos++];
                }
                QString val;
                if (var == pwdName && !pwd.isEmpty()) {
                    val = pwd;
                } else {
                    const Environment::FindResult res = env->find(var);
                    if (!res) {
                        if (abortOnMeta)
                            goto metaerr; // Assume this is a shell builtin
                    } else {
                        val = env->expandedValueForKey(res->key);
                    }
                }
                for (int i = 0; i < val.length(); i++) {
                    const QChar cc = val.unicode()[i];
                    if (cc.unicode() == 9 || cc.unicode() == 10 || cc.unicode() == 32) {
                        if (hadWord) {
                            ret += cret;
                            cret.clear();
                            hadWord = false;
                        }
                    } else {
                        cret += cc;
                        hadWord = true;
                    }
                }
                if (!braced)
                    goto nextc;
                if (c != QLatin1Char('}')) {
                    if (abortOnMeta)
                        goto metaerr; // Assume this is a complex expansion
                    goto quoteerr; // Otherwise it's just garbage
                }
            } else {
                if (c == QLatin1Char('\\')) {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                } else if (abortOnMeta && isMetaUnix(c)) {
                    goto metaerr;
                }
                cret += c;
                hadWord = true;
            }
          getc:
            if (pos >= args.length())
                break;
            c = args.unicode()[pos++];
          nextc: ;
        } while (!c.isSpace());
        if (hadWord)
            ret += cret;
    }

  okret:
    if (err)
        *err = ProcessArgs::SplitOk;
    return ret;

  quoteerr:
    if (err)
        *err = ProcessArgs::BadQuoting;
    return {};

  metaerr:
    if (err)
        *err = ProcessArgs::FoundMeta;
    return {};
}

inline static bool isSpecialCharUnix(ushort c)
{
    // Chars that should be quoted (TM). This includes:
    static const uchar iqm[] = {
        0xff, 0xff, 0xff, 0xff, 0xdf, 0x07, 0x00, 0xd8,
        0x00, 0x00, 0x00, 0x38, 0x01, 0x00, 0x00, 0x78
    }; // 0-32 \'"$`<>|;&(){}*?#!~[]

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

inline static bool hasSpecialCharsUnix(const QString &arg)
{
    for (int x = arg.length() - 1; x >= 0; --x)
        if (isSpecialCharUnix(arg.unicode()[x].unicode()))
            return true;
    return false;
}

QStringList ProcessArgs::splitArgs(const QString &args, OsType osType,
                                   bool abortOnMeta, ProcessArgs::SplitError *err,
                                   const Environment *env, const QString &pwd)
{
    if (osType == OsTypeWindows)
        return splitArgsWin(args, abortOnMeta, err, env, pwd);
    else
        return splitArgsUnix(args, abortOnMeta, err, env, pwd);
}

QString ProcessArgs::quoteArgUnix(const QString &arg)
{
    if (arg.isEmpty())
        return QString::fromLatin1("''");

    QString ret(arg);
    if (hasSpecialCharsUnix(ret)) {
        if (arg == "&&" || arg == "||" || arg == "&" || arg == ';')
            return ret;

        ret.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        ret.prepend(QLatin1Char('\''));
        ret.append(QLatin1Char('\''));
    }
    return ret;
}

static bool isSpecialCharWin(ushort c)
{
    // Chars that should be quoted (TM). This includes:
    // - control chars & space
    // - the shell meta chars "&()<>^|
    // - the potential separators ,;=
    static const uchar iqm[] = {
        0xff, 0xff, 0xff, 0xff, 0x45, 0x13, 0x00, 0x78,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x10
    };

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

static bool hasSpecialCharsWin(const QString &arg)
{
    for (int x = arg.length() - 1; x >= 0; --x)
        if (isSpecialCharWin(arg.unicode()[x].unicode()))
            return true;
    return false;
}

static QString quoteArgWin(const QString &arg)
{
    if (arg.isEmpty())
        return QString::fromLatin1("\"\"");

    QString ret(arg);
    if (hasSpecialCharsWin(ret)) {
        if (arg == "&&" || arg == "||" || arg == "&" || arg == ';')
            return ret;

        // Quotes are escaped and their preceding backslashes are doubled.
        // It's impossible to escape anything inside a quoted string on cmd
        // level, so the outer quoting must be "suspended".
        static const QRegularExpression regexp("(\\\\*)\"");
        ret.replace(regexp, QLatin1String("\"\\1\\1\\^\"\""));
        // The argument must not end with a \ since this would be interpreted
        // as escaping the quote -- rather put the \ behind the quote: e.g.
        // rather use "foo"\ than "foo\"
        int i = ret.length();
        while (i > 0 && ret.at(i - 1) == QLatin1Char('\\'))
            --i;
        ret.insert(i, QLatin1Char('"'));
        ret.prepend(QLatin1Char('"'));
    }
    // FIXME: Without this, quoting is not foolproof. But it needs support in the process setup, etc.
    //ret.replace('%', QLatin1String("%PERCENT_SIGN%"));
    return ret;
}

QString ProcessArgs::prepareShellArgs(const QString &args, SplitError *err, OsType osType,
                                      const Environment *env, const FilePath &pwd)
{
    const QString wd = pwd.path();
    QString res;
    if (osType == OsTypeWindows)
        res = prepareArgsWin(args, err, env, wd);
    else
        res = joinArgs(splitArgsUnix(args, true, err, env, wd), OsTypeLinux);
    return res;
}

QString ProcessArgs::quoteArg(const QString &arg, OsType osType)
{
    if (osType == OsTypeWindows)
        return quoteArgWin(arg);
    else
        return quoteArgUnix(arg);
}

void ProcessArgs::addArg(QString *args, const QString &arg, OsType osType)
{
    if (!args->isEmpty())
        *args += QLatin1Char(' ');
    *args += quoteArg(arg, osType);
}

QString ProcessArgs::joinArgs(const QStringList &args, OsType osType)
{
    QString ret;
    for (const QString &arg : args)
        addArg(&ret, arg, osType);
    return ret;
}

void ProcessArgs::addArgs(QString *args, const QString &inArgs)
{
    if (!inArgs.isEmpty()) {
        if (!args->isEmpty())
            *args += QLatin1Char(' ');
        *args += inArgs;
    }
}

void ProcessArgs::addArgs(QString *args, const QStringList &inArgs)
{
    for (const QString &arg : inArgs)
        addArg(args, arg);
}

void ProcessArgs::addArgs(QString *args, const QStringList &inArgs, OsType osType)
{
    for (const QString &arg : inArgs)
        addArg(args, arg, osType);
}

CommandLine &CommandLine::operator<<(const QString &arg)
{
    addArg(arg);
    return *this;
}

CommandLine &CommandLine::operator<<(const QStringList &args)
{
    addArgs(args);
    return *this;
}

bool ProcessArgs::prepareCommand(const CommandLine &cmdLine, QString *outCmd, QString *outArgs,
                                 const Environment *env, const FilePath &pwd)
{
    const FilePath executable = cmdLine.executable();
    if (executable.isEmpty())
        return false;

    const QString arguments = cmdLine.arguments();
    const OsType osType = executable.osType();

    ProcessArgs::SplitError err;
    *outArgs = ProcessArgs::prepareShellArgs(arguments, &err, osType, env, pwd);

    if (err == ProcessArgs::SplitOk) {
        *outCmd = executable.path();
    } else {
        if (osType == OsTypeWindows) {
            *outCmd = qtcEnvironmentVariable("COMSPEC");
            *outArgs = "/v:off /s /c \""
                        + quoteArgWin(executable.nativePath()) + ' ' + arguments + '"';
        } else {
            if (err != ProcessArgs::FoundMeta)
                return false;
            *outCmd = qtcEnvironmentVariable("SHELL", "/bin/sh");
            ProcessArgs::addArg(outArgs, "-c", osType);
            ProcessArgs::addArg(outArgs, quoteArg(executable.path()) + ' ' + arguments, osType);
        }
    }
    return true;
}

// This function assumes that the resulting string will be quoted.
// That's irrelevant if it does not contain quotes itself.
static int quoteArgInternalWin(QString &ret, int bslashes)
{
    // Quotes are escaped and their preceding backslashes are doubled.
    // It's impossible to escape anything inside a quoted string on cmd
    // level, so the outer quoting must be "suspended".
    const QChar bs(QLatin1Char('\\')), dq(QLatin1Char('"'));
    for (int p = 0; p < ret.length(); p++) {
        if (ret.at(p) == bs) {
            bslashes++;
        } else {
            if (ret.at(p) == dq) {
                if (bslashes) {
                    ret.insert(p, QString(bslashes, bs));
                    p += bslashes;
                }
                ret.insert(p, QLatin1String("\"\\^\""));
                p += 4;
            }
            bslashes = 0;
        }
    }
    return bslashes;
}


// TODO: This documentation is relevant for end-users. Where to put it?
/*!
 * Searches macros with the function \a findMacro and performs in-place macro expansion
 * (substitution) on the string \a cmd, which is expected to contain a shell
 * command. \a osType specifies the syntax, which is Bourne Shell compatible
 * for Unix and \c cmd compatible for Windows.
 *
 * Returns \c false if substitution cannot be performed safely, because the
 * command cannot be parsed -- for example due to quoting errors.
 *
 * \note This function is designed to be safe to use with expando objects
 * that contain shell meta-characters. However, placing expandos in the wrong
 * place of the command may defeat the expander's efforts to quote their
 * contents, which will likely result in incorrect command execution.
 * In particular, expandos that contain untrusted data might expose the
 * end-user of the application to critical shell code injection
 * vulnerabilities. To avoid these issues, follow the guidelines in
 * \l {Unix security considerations} and \l {Windows security considerations}.
 * Generally, it is a better idea to invoke shell scripts rather than to
 * assemble complex one-line commands.
 *
 * \section1 Unix notes
 *
 * Explicitly supported shell constructs:
 *   \\ '' "" {} () $(()) ${} $() ``
 *
 * Implicitly supported shell constructs:
 *   (())
 *
 * Unsupported shell constructs that will cause problems:
 * \list
 * \li Shortened \c{case $v in pat)} syntax. Use \c{case $v in (pat)} instead.
 * \li Bash-style \c{$""} and \c{$''} string quoting syntax.
 * \endlist
 *
 * The rest of the shell syntax (including bash syntax) should not cause
 * problems and is ignored.
 *
 * \section2 Unix security considerations
 * \list
 * \li Backslash-escaping an expando is treated as a quoting error.
 * \li Do not put expandos into double quoted substitutions as this may
 *     trigger parser bugs in some shells:
 *     \badcode
 *       "${VAR:-%{macro}}"
 *     \endcode
 * \li Do not put expandos into command line arguments that are nested shell
 *     commands. For example, the following is unsafe:
 *     \badcode
 *       su %{user} -c 'foo %{file}'
 *     \endcode
 *     Instead you can assign the macro to an environment variable and pass
 *     that into the call:
 *     \badcode
 *       file=%{file} su %{user} -c 'foo "$file"'
 *     \endcode
 * \endlist
 *
 * \section1 Windows notes
 *
 * All quoting syntax supported by \c splitArgs() is supported here as well.
 * Additionally, command grouping via parentheses is recognized -- but
 * note that the parser is much stricter about unquoted parentheses
 * than \c cmd itself.
 * The rest of the \c cmd syntax should not cause problems and is ignored.
 *
 *
 * \section2 Windows security considerations
 * \list
 * \li Circumflex-escaping an expando is treated as a quoting error.
 * \li Closing double quotes right before expandos and opening double quotes
 *     right after expandos are treated as quoting errors.
 * \li Do not put expandos into nested commands.
 *     For example, the following is unsafe:
 *     \badcode
 *       for /f "usebackq" \%v in (`foo \%{file}`) do \@echo \%v
 *     \endcode
 * \li A macro's value must not contain anything which may be interpreted
 *     as an environment variable expansion. A solution is replacing any
 *     percent signs with a fixed string like \c{\%PERCENT_SIGN\%} and
 *     injecting \c{PERCENT_SIGN=\%} into the shell's environment.
 * \li Enabling delayed environment variable expansion (\c{cmd /v:on}) should
 *     have no security implications. But it might still cause issues because
 *     the parser is not prepared for the fact that literal exclamation marks
 *     in the command need to be \e circumflex-escaped, and pre-existing
 *     circumflexes need to be doubled.
 * \endlist
 *
 */
bool ProcessArgs::expandMacros(QString *cmd, const FindMacro &findMacro, OsType osType)
{
    QString str = *cmd;
    if (str.isEmpty())
        return true;

    QString rsts;
    int varLen;
    int varPos = 0;
    if (!(varLen = findMacro(str, &varPos, &rsts)))
        return true;

    int pos = 0;

    if (osType == OsTypeWindows) {
        enum { // cmd.exe parsing state
            ShellBasic, // initial state
            ShellQuoted, // double-quoted state => *no* other meta chars are interpreted
            ShellEscaped // circumflex-escaped state => next char is not interpreted
        } shellState = ShellBasic;
        enum { // CommandLineToArgv() parsing state and some more
            CrtBasic, // initial state
            CrtNeedWord, // after empty expando; insert empty argument if whitespace follows
            CrtInWord, // in non-whitespace
            CrtClosed, // previous char closed the double-quoting
            CrtHadQuote, // closed double-quoting after an expando
            // The remaining two need to be numerically higher
            CrtQuoted, // double-quoted state => spaces don't split tokens
            CrtNeedQuote // expando opened quote; close if no expando follows
        } crtState = CrtBasic;
        int bslashes = 0; // previous chars were manual backslashes
        int rbslashes = 0; // trailing backslashes in replacement

        forever {
            if (pos == varPos) {
                if (shellState == ShellEscaped)
                    return false; // Circumflex'd quoted expando would be Bad (TM).
                if ((shellState == ShellQuoted) != (crtState == CrtQuoted))
                    return false; // CRT quoting out of sync with shell quoting. Ahoy to Redmond.
                rbslashes += bslashes;
                bslashes = 0;
                if (crtState < CrtQuoted) {
                    if (rsts.isEmpty()) {
                        if (crtState == CrtBasic) {
                            // Outside any quoting and the string is empty, so put
                            // a pair of quotes. Delaying that is just pedantry.
                            crtState = CrtNeedWord;
                        }
                    } else {
                        if (hasSpecialCharsWin(rsts)) {
                            if (crtState == CrtClosed) {
                                // Quoted expando right after closing quote. Can't do that.
                                return false;
                            }
                            int tbslashes = quoteArgInternalWin(rsts, 0);
                            rsts.prepend(QLatin1Char('"'));
                            if (rbslashes)
                                rsts.prepend(QString(rbslashes, QLatin1Char('\\')));
                            crtState = CrtNeedQuote;
                            rbslashes = tbslashes;
                        } else {
                            crtState = CrtInWord; // We know that this string contains no spaces.
                            // We know that this string contains no quotes,
                            // so the function won't make a mess.
                            rbslashes = quoteArgInternalWin(rsts, rbslashes);
                        }
                    }
                } else {
                    rbslashes = quoteArgInternalWin(rsts, rbslashes);
                }
                str.replace(pos, varLen, rsts);
                pos += rsts.length();
                varPos = pos;
                if (!(varLen = findMacro(str, &varPos, &rsts))) {
                    // Don't leave immediately, as we may be in CrtNeedWord state which could
                    // be still resolved, or we may have inserted trailing backslashes.
                    varPos = INT_MAX;
                }
                continue;
            }
            if (crtState == CrtNeedQuote) {
                if (rbslashes) {
                    str.insert(pos, QString(rbslashes, QLatin1Char('\\')));
                    pos += rbslashes;
                    varPos += rbslashes;
                    rbslashes = 0;
                }
                str.insert(pos, QLatin1Char('"'));
                pos++;
                varPos++;
                crtState = CrtHadQuote;
            }
            ushort cc = str.unicode()[pos].unicode();
            if (shellState == ShellBasic && cc == '^') {
                shellState = ShellEscaped;
            } else {
                if (!cc || cc == ' ' || cc == '\t') {
                    if (crtState < CrtQuoted) {
                        if (crtState == CrtNeedWord) {
                            str.insert(pos, QLatin1String("\"\""));
                            pos += 2;
                            varPos += 2;
                        }
                        crtState = CrtBasic;
                    }
                    if (!cc)
                        break;
                    bslashes = 0;
                    rbslashes = 0;
                } else {
                    if (cc == '\\') {
                        bslashes++;
                        if (crtState < CrtQuoted)
                            crtState = CrtInWord;
                    } else {
                        if (cc == '"') {
                            if (shellState != ShellEscaped)
                                shellState = (shellState == ShellQuoted) ? ShellBasic : ShellQuoted;
                            if (rbslashes) {
                                // Offset -1: skip possible circumflex. We have at least
                                // one backslash, so a fixed offset is ok.
                                str.insert(pos - 1, QString(rbslashes, QLatin1Char('\\')));
                                pos += rbslashes;
                                varPos += rbslashes;
                            }
                            if (!(bslashes & 1)) {
                                // Even number of backslashes, so the quote is not escaped.
                                switch (crtState) {
                                case CrtQuoted:
                                    // Closing quote
                                    crtState = CrtClosed;
                                    break;
                                case CrtClosed:
                                    // Two consecutive quotes make a literal quote - and
                                    // still close quoting. See Process::quoteArg().
                                    crtState = CrtInWord;
                                    break;
                                case CrtHadQuote:
                                    // Opening quote right after quoted expando. Can't do that.
                                    return false;
                                default:
                                    // Opening quote
                                    crtState = CrtQuoted;
                                    break;
                                }
                            } else if (crtState < CrtQuoted) {
                                crtState = CrtInWord;
                            }
                        } else if (crtState < CrtQuoted) {
                            crtState = CrtInWord;
                        }
                        bslashes = 0;
                        rbslashes = 0;
                    }
                }
                if (varPos == INT_MAX && !rbslashes)
                    break;
                if (shellState == ShellEscaped)
                    shellState = ShellBasic;
            }
            pos++;
        }
    } else {
        // !Windows
        MxState state = {MxBasic, false};
        QStack<MxState> sstack;
        QStack<MxSave> ostack;

        while (pos < str.length()) {
            if (pos == varPos) {
                // Our expansion rules trigger in any context
                if (state.dquote) {
                    // We are within a double-quoted string. Escape relevant meta characters.
                    static const QRegularExpression regexp("([$`\"\\\\])");
                    rsts.replace(regexp, QLatin1String("\\\\1"));
                } else if (state.current == MxSingleQuote) {
                    // We are within a single-quoted string. "Suspend" single-quoting and put a
                    // single escaped quote for each single quote inside the string.
                    rsts.replace(QLatin1Char('\''), QLatin1String("'\\''"));
                } else if (rsts.isEmpty() || hasSpecialCharsUnix(rsts)) {
                    // String contains "quote-worthy" characters. Use single quoting - but
                    // that choice is arbitrary.
                    rsts.replace(QLatin1Char('\''), QLatin1String("'\\''"));
                    rsts.prepend(QLatin1Char('\''));
                    rsts.append(QLatin1Char('\''));
                } // Else just use the string verbatim.
                str.replace(pos, varLen, rsts);
                pos += rsts.length();
                varPos = pos;
                if (!(varLen = findMacro(str, &varPos, &rsts)))
                    break;
                continue;
            }
            ushort cc = str.unicode()[pos].unicode();
            if (state.current == MxSingleQuote) {
                // Single quoted context - only the single quote has any special meaning.
                if (cc == '\'')
                    state = sstack.pop();
            } else if (cc == '\\') {
                // In any other context, the backslash starts an escape.
                pos += 2;
                if (varPos < pos)
                    return false; // Backslash'd quoted expando would be Bad (TM).
                continue;
            } else if (cc == '$') {
                cc = str.unicode()[++pos].unicode();
                if (cc == '(') {
                    sstack.push(state);
                    if (str.unicode()[pos + 1].unicode() == '(') {
                        // $(( starts a math expression. This may also be a $( ( in fact,
                        // so we push the current string and offset on a stack so we can retry.
                        MxSave sav = {str, pos + 2, varPos};
                        ostack.push(sav);
                        state.current = MxMath;
                        pos += 2;
                        continue;
                    } else {
                        // $( starts a command substitution. This actually "opens a new context"
                        // which overrides surrounding double quoting.
                        state.current = MxParen;
                        state.dquote = false;
                    }
                } else if (cc == '{') {
                    // ${ starts a "braced" variable substitution.
                    sstack.push(state);
                    state.current = MxSubst;
                } // Else assume that a "bare" variable substitution has started
            } else if (cc == '`') {
                // Backticks are evil, as every shell interprets escapes within them differently,
                // which is a danger for the quoting of our own expansions.
                // So we just apply *our* rules (which match bash) and transform it into a POSIX
                // command substitution which has clear semantics.
                str.replace(pos, 1, QLatin1String("$( " )); // add space -> avoid creating $((
                varPos += 2;
                int pos2 = pos += 3;
                forever {
                    if (pos2 >= str.length())
                        return false; // Syntax error - unterminated backtick expression.
                    cc = str.unicode()[pos2].unicode();
                    if (cc == '`')
                        break;
                    if (cc == '\\') {
                        cc = str.unicode()[++pos2].unicode();
                        if (cc == '$' || cc == '`' || cc == '\\' ||
                            (cc == '"' && state.dquote))
                        {
                            str.remove(pos2 - 1, 1);
                            if (varPos >= pos2)
                                varPos--;
                            continue;
                        }
                    }
                    pos2++;
                }
                str[pos2] = QLatin1Char(')');
                sstack.push(state);
                state.current = MxParen;
                state.dquote = false;
                continue;
            } else if (state.current == MxDoubleQuote) {
                // (Truly) double quoted context - only remaining special char is the closing quote.
                if (cc == '"')
                    state = sstack.pop();
            } else if (cc == '\'') {
                // Start single quote if we are not in "inherited" double quoted context.
                if (!state.dquote) {
                    sstack.push(state);
                    state.current = MxSingleQuote;
                }
            } else if (cc == '"') {
                // Same for double quoting.
                if (!state.dquote) {
                    sstack.push(state);
                    state.current = MxDoubleQuote;
                    state.dquote = true;
                }
            } else if (state.current == MxSubst) {
                // "Braced" substitution context - only remaining special char is the closing brace.
                if (cc == '}')
                    state = sstack.pop();
            } else if (cc == ')') {
                if (state.current == MxMath) {
                    if (str.unicode()[pos + 1].unicode() == ')') {
                        state = sstack.pop();
                        pos += 2;
                    } else {
                        // False hit: the $(( was a $( ( in fact.
                        // ash does not care (and will complain), but bash actually parses it.
                        varPos = ostack.top().varPos;
                        pos = ostack.top().pos;
                        str = ostack.top().str;
                        ostack.pop();
                        state.current = MxParen;
                        state.dquote = false;
                        sstack.push(state);
                    }
                    continue;
                } else if (state.current == MxParen) {
                    state = sstack.pop();
                } else {
                    break; // Syntax error - excess closing parenthesis.
                }
            } else if (cc == '}') {
                if (state.current == MxGroup)
                    state = sstack.pop();
                else
                    break; // Syntax error - excess closing brace.
            } else if (cc == '(') {
                // Context-saving command grouping.
                sstack.push(state);
                state.current = MxParen;
            } else if (cc == '{') {
                // Plain command grouping.
                sstack.push(state);
                state.current = MxGroup;
            }
            pos++;
        }
        // FIXME? May complain if (!sstack.empty()), but we don't really care anyway.
    }

    *cmd = str;
    return true;
}

bool ProcessArgs::ArgIterator::next()
{
    // We delay the setting of m_prev so we can still delete the last argument
    // after we find that there are no more arguments. It's a bit of a hack ...
    int prev = m_pos;

    m_simple = true;
    m_value.clear();

    if (m_osType == OsTypeWindows) {
        enum { // cmd.exe parsing state
            ShellBasic, // initial state
            ShellQuoted, // double-quoted state => *no* other meta chars are interpreted
            ShellEscaped // circumflex-escaped state => next char is not interpreted
        } shellState = ShellBasic;
        enum { // CommandLineToArgv() parsing state and some more
            CrtBasic, // initial state
            CrtInWord, // in non-whitespace
            CrtClosed, // previous char closed the double-quoting
            CrtQuoted // double-quoted state => spaces don't split tokens
        } crtState = CrtBasic;
        enum { NoVar, NewVar, FullVar } varState = NoVar; // inside a potential env variable expansion
        int bslashes = 0; // number of preceding backslashes

        for (;; m_pos++) {
            ushort cc = m_pos < m_str->length() ? m_str->unicode()[m_pos].unicode() : 0;
            if (shellState == ShellBasic && cc == '^') {
                varState = NoVar;
                shellState = ShellEscaped;
            } else if ((shellState == ShellBasic && isMetaCharWin(cc)) || !cc) { // A "bit" simplistic ...
                // We ignore crtQuote state here. Whatever ...
              doReturn:
                if (m_simple)
                    while (--bslashes >= 0)
                        m_value += QLatin1Char('\\');
                else
                    m_value.clear();
                if (crtState != CrtBasic) {
                    m_prev = prev;
                    return true;
                }
                return false;
            } else {
                if (crtState != CrtQuoted && (cc == ' ' || cc == '\t')) {
                    if (crtState != CrtBasic) {
                        // We'll lose shellQuote state here. Whatever ...
                        goto doReturn;
                    }
                } else {
                    if (cc == '\\') {
                        bslashes++;
                        if (crtState != CrtQuoted)
                            crtState = CrtInWord;
                        varState = NoVar;
                    } else {
                        if (cc == '"') {
                            varState = NoVar;
                            if (shellState != ShellEscaped)
                                shellState = (shellState == ShellQuoted) ? ShellBasic : ShellQuoted;
                            int obslashes = bslashes;
                            bslashes >>= 1;
                            if (!(obslashes & 1)) {
                                // Even number of backslashes, so the quote is not escaped.
                                switch (crtState) {
                                case CrtQuoted:
                                    // Closing quote
                                    crtState = CrtClosed;
                                    continue;
                                case CrtClosed:
                                    // Two consecutive quotes make a literal quote - and
                                    // still close quoting. See quoteArg().
                                    crtState = CrtInWord;
                                    break;
                                default:
                                    // Opening quote
                                    crtState = CrtQuoted;
                                    continue;
                                }
                            } else if (crtState != CrtQuoted) {
                                crtState = CrtInWord;
                            }
                        } else {
                            if (cc == '%') {
                                if (varState == FullVar) {
                                    m_simple = false;
                                    varState = NoVar;
                                } else {
                                    varState = NewVar;
                                }
                            } else if (varState != NoVar) {
                                // This check doesn't really reflect cmd reality, but it is an
                                // approximation of what would be sane.
                                varState = (cc == '_' || cc == '-' || cc == '.'
                                         || QChar(cc).isLetterOrNumber()) ? FullVar : NoVar;

                            }
                            if (crtState != CrtQuoted)
                                crtState = CrtInWord;
                        }
                        for (; bslashes > 0; bslashes--)
                            m_value += QLatin1Char('\\');
                        m_value += QChar(cc);
                    }
                }
                if (shellState == ShellEscaped)
                    shellState = ShellBasic;
            }
        }
    } else {
        MxState state = {MxBasic, false};
        QStack<MxState> sstack;
        QStack<int> ostack;
        bool hadWord = false;

        for (; m_pos < m_str->length(); m_pos++) {
            ushort cc = m_str->unicode()[m_pos].unicode();
            if (state.current == MxSingleQuote) {
                if (cc == '\'') {
                    state = sstack.pop();
                    continue;
                }
            } else if (cc == '\\') {
                if (++m_pos >= m_str->length())
                    break;
                cc = m_str->unicode()[m_pos].unicode();
                if (state.dquote && cc != '"' && cc != '\\' && cc != '$' && cc != '`')
                    m_value += QLatin1Char('\\');
            } else if (cc == '$') {
                if (++m_pos >= m_str->length())
                    break;
                cc = m_str->unicode()[m_pos].unicode();
                if (cc == '(') {
                    sstack.push(state);
                    if (++m_pos >= m_str->length())
                        break;
                    if (m_str->unicode()[m_pos].unicode() == '(') {
                        ostack.push(m_pos);
                        state.current = MxMath;
                    } else {
                        state.dquote = false;
                        state.current = MxParen;
                        // m_pos too far by one now - whatever.
                    }
                } else if (cc == '{') {
                    sstack.push(state);
                    state.current = MxSubst;
                } else {
                    // m_pos too far by one now - whatever.
                }
                m_simple = false;
                hadWord = true;
                continue;
            } else if (cc == '`') {
                forever {
                    if (++m_pos >= m_str->length()) {
                        m_simple = false;
                        m_prev = prev;
                        return true;
                    }
                    cc = m_str->unicode()[m_pos].unicode();
                    if (cc == '`')
                        break;
                    if (cc == '\\')
                        m_pos++; // m_pos may be too far by one now - whatever.
                }
                m_simple = false;
                hadWord = true;
                continue;
            } else if (state.current == MxDoubleQuote) {
                if (cc == '"') {
                    state = sstack.pop();
                    continue;
                }
            } else if (cc == '\'') {
                if (!state.dquote) {
                    sstack.push(state);
                    state.current = MxSingleQuote;
                    hadWord = true;
                    continue;
                }
            } else if (cc == '"') {
                if (!state.dquote) {
                    sstack.push(state);
                    state.dquote = true;
                    state.current = MxDoubleQuote;
                    hadWord = true;
                    continue;
                }
            } else if (state.current == MxSubst) {
                if (cc == '}')
                    state = sstack.pop();
                continue; // Not simple anyway
            } else if (cc == ')') {
                if (state.current == MxMath) {
                    if (++m_pos >= m_str->length())
                        break;
                    if (m_str->unicode()[m_pos].unicode() == ')') {
                        ostack.pop();
                        state = sstack.pop();
                    } else {
                        // false hit: the $(( was a $( ( in fact.
                        // ash does not care, but bash does.
                        m_pos = ostack.pop();
                        state.current = MxParen;
                        state.dquote = false;
                        sstack.push(state);
                    }
                    continue;
                } else if (state.current == MxParen) {
                    state = sstack.pop();
                    continue;
                } else {
                    break;
                }
#if 0 // MxGroup is impossible, see below.
            } else if (cc == '}') {
                if (state.current == MxGroup) {
                    state = sstack.pop();
                    continue;
                }
#endif
            } else if (cc == '(') {
                sstack.push(state);
                state.current = MxParen;
                m_simple = false;
                hadWord = true;
                continue;
#if 0 // Should match only at the beginning of a command, which we never have currently.
            } else if (cc == '{') {
                sstack.push(state);
                state.current = MxGroup;
                m_simple = false;
                hadWord = true;
                continue;
#endif
            } else if (cc == '<' || cc == '>' || cc == '&' || cc == '|' || cc == ';') {
                if (sstack.isEmpty())
                    break;
            } else if (cc == ' ' || cc == '\t') {
                if (!hadWord)
                    continue;
                if (sstack.isEmpty())
                    break;
            }
            m_value += QChar(cc);
            hadWord = true;
        }
        // TODO: Possibly complain here if (!sstack.empty())
        if (!m_simple)
            m_value.clear();
        if (hadWord) {
            m_prev = prev;
            return true;
        }
        return false;
    }
}

void ProcessArgs::ArgIterator::deleteArg()
{
    if (!m_prev)
        while (m_pos < m_str->length() && m_str->at(m_pos).isSpace())
            m_pos++;
    m_str->remove(m_prev, m_pos - m_prev);
    m_pos = m_prev;
}

void ProcessArgs::ArgIterator::appendArg(const QString &str)
{
    const QString qstr = quoteArg(str);
    if (!m_pos)
        m_str->insert(0, qstr + QLatin1Char(' '));
    else
        m_str->insert(m_pos, QLatin1Char(' ') + qstr);
    m_pos += qstr.length() + 1;
}

/*!
    \class Utils::CommandLine
    \inmodule QtCreator

    \brief The CommandLine class represents a command line of a QProcess or
    similar utility.
 */

CommandLine::CommandLine() = default;

CommandLine::CommandLine(const FilePath &executable)
    : m_executable(executable)
{}

CommandLine::~CommandLine() = default;

CommandLine::CommandLine(const FilePath &exe, const QStringList &args)
    : m_executable(exe)
{
    addArgs(args);
}

CommandLine::CommandLine(const FilePath &exe, std::initializer_list<ArgRef> args)
    : m_executable(exe)
{
    for (const ArgRef &arg : args) {
        if (const auto ptr = std::get_if<const char *>(&arg.m_arg))
            addArg(QString::fromUtf8(*ptr));
        else if (const auto ptr = std::get_if<std::reference_wrapper<const QString>>(&arg.m_arg)) {
            if (arg.m_raw)
                addArgs(*ptr, Raw);
            else
                addArg(*ptr);
        } else if (const auto ptr = std::get_if<std::reference_wrapper<const QStringList>>(&arg.m_arg))
            addArgs(*ptr);
        else if (const auto ptr = std::get_if<QStringList>(&arg.m_arg))
            addArgs(*ptr);
    }
}

CommandLine::CommandLine(const FilePath &exe, const QStringList &args, OsType osType)
    : m_executable(exe)
{
    addArgs(args, osType);
}

CommandLine::CommandLine(const FilePath &exe, const QString &args, RawType)
    : m_executable(exe)
{
    addArgs(args, Raw);
}

CommandLine CommandLine::fromUserInput(const QString &cmdline, MacroExpander *expander)
{
    if (cmdline.isEmpty())
        return {};

    QString input = cmdline.trimmed();

    if (expander)
        input = expander->expand(input);

    const QStringList result = ProcessArgs::splitArgs(input, HostOsInfo::hostOs());

    if (result.isEmpty())
        return {};

    return {FilePath::fromUserInput(result.value(0)), result.mid(1)};
}

void CommandLine::addArg(const QString &arg)
{
    addArg(arg, m_executable.osType());
}

void CommandLine::addArg(const QString &arg, OsType osType)
{
    ProcessArgs::addArg(&m_arguments, arg, osType);
}

void CommandLine::addMaskedArg(const QString &arg)
{
    addMaskedArg(arg, m_executable.osType());
}

void CommandLine::addMaskedArg(const QString &arg, OsType osType)
{
    int start = m_arguments.size();
    if (start > 0)
        ++start;
    addArg(arg, osType);
    m_masked.push_back({start, m_arguments.size() - start});
}

void CommandLine::addArgs(const QStringList &inArgs)
{
    for (const QString &arg : inArgs)
        addArg(arg);
}

void CommandLine::addArgs(const QStringList &inArgs, OsType osType)
{
    for (const QString &arg : inArgs)
        addArg(arg, osType);
}

// Adds cmd's executable and arguments one by one to this commandline.
// Useful for 'sudo', 'nice', etc
void CommandLine::addCommandLineAsArgs(const CommandLine &cmd)
{
    addArg(cmd.executable().path());
    addArgs(cmd.splitArguments());
}

void CommandLine::addCommandLineAsArgs(const CommandLine &cmd, RawType)
{
    addArg(cmd.executable().path());
    addArgs(cmd.arguments(), Raw);
}

void CommandLine::addCommandLineAsSingleArg(const CommandLine &cmd)
{
    QString combined;
    ProcessArgs::addArg(&combined, cmd.executable().path());
    ProcessArgs::addArgs(&combined, cmd.arguments());

    addArg(combined);
}

void CommandLine::addCommandLineAsSingleArg(const CommandLine &cmd, OsType osType)
{
    QString combined;
    ProcessArgs::addArg(&combined, cmd.executable().path(), osType);
    ProcessArgs::addArgs(&combined, cmd.arguments());

    addArg(combined, osType);
}

void CommandLine::addCommandLineWithAnd(const CommandLine &cmd)
{
    if (m_executable.isEmpty()) {
        *this = cmd;
        return;
    }

    addArgs("&&", Raw);
    addCommandLineAsArgs(cmd, Raw);
}

void CommandLine::addCommandLineWithOr(const CommandLine &cmd)
{
    if (m_executable.isEmpty()) {
        *this = cmd;
        return;
    }

    addArgs("||", Raw);
    addCommandLineAsArgs(cmd, Raw);
}

void CommandLine::addArgs(const QString &inArgs, RawType)
{
    ProcessArgs::addArgs(&m_arguments, inArgs);
}

void CommandLine::prependArgs(const QStringList &inArgs)
{
    QString oldArgs = m_arguments;
    m_arguments.clear();
    addArgs(inArgs);
    addArgs(oldArgs, Raw);
}

void CommandLine::prependArgs(const QString &inArgs, RawType)
{
    QString oldArgs = m_arguments;
    m_arguments = inArgs;
    addArgs(oldArgs, Raw);
}

QString CommandLine::toUserOutput() const
{
    QString res = m_executable.toUserOutput();
    if (!m_arguments.isEmpty()) {
        QString args = m_arguments;
        for (auto it = m_masked.crbegin(), end = m_masked.crend(); it != end; ++it)
            args.replace(it->first, it->second, "*******");
        res += ' ' + args;
    }
    return res;
}

QString CommandLine::displayName() const
{
    return m_executable.displayName(m_arguments);
}

QStringList CommandLine::splitArguments() const
{
    return ProcessArgs::splitArgs(m_arguments, m_executable.osType());
}

CommandLine CommandLine::toLocal() const
{
    if (m_executable.isLocal())
        return *this;

    QTC_CHECK(false); // TODO: Does it make sense?
    CommandLine cmd = *this;
    cmd.setExecutable(FilePath::fromString(m_executable.path()));
    return cmd;
}

QStringList ProcessArgs::filterSimpleArgs(const QString &args, OsType osType)
{
    QStringList result;
    QString args_ = args;
    for (ArgIterator ait(&args_, osType); ait.next(); ) {
        // This filters out items containing e.g. shell variables like '$FOO'
        if (ait.isSimple())
            result << ait.value();
    }
    return result;
}

QTCREATOR_UTILS_EXPORT bool operator==(const CommandLine &first, const CommandLine &second)
{
    return first.m_executable == second.m_executable && first.m_arguments == second.m_arguments;
}

QTCREATOR_UTILS_EXPORT QDebug operator<<(QDebug dbg, const CommandLine &cmd)
{
    return dbg << cmd.toUserOutput();
}

} // Utils

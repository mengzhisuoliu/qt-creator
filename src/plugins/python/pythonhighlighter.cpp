// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

/**
 * @brief The Highlighter class pre-highlights Python source using simple scanner.
 *
 * Highlighter doesn't highlight user types (classes and enumerations), syntax
 * and semantic errors, unnecessary code, etc. It's implements only
 * basic highlight mechanism.
 *
 * Main highlight procedure is highlightBlock().
 */

#include "pythonhighlighter.h"
#include "pythonscanner.h"

#include <texteditor/textdocument.h>
#include <texteditor/textdocumentlayout.h>
#include <texteditor/texteditorconstants.h>

#include <utils/qtcassert.h>

namespace Python::Internal {

/**
 * @class PythonEditor::Internal::PythonHighlighter
 * @brief Handles incremental lexical highlighting, but not semantic
 *
 * Incremental lexical highlighting works every time when any character typed
 * or some text inserted (i.e. copied & pasted).
 * Each line keeps associated scanner state - integer number. This state is the
 * scanner context for next line. For example, 3 quotes begin a multiline
 * string, and each line up to next 3 quotes has state 'MultiLineString'.
 *
 * @code
 *  def __init__:               # Normal
 *      self.__doc__ = """      # MultiLineString (next line is inside)
 *                     banana   # MultiLineString
 *                     """      # Normal
 * @endcode
 */

static TextEditor::TextStyle styleForFormat(int format)
{
    using namespace TextEditor;
    const auto f = Format(format);
    switch (f) {
    case Format_Number: return C_NUMBER;
    case Format_String: return C_STRING;
    case Format_Keyword: return C_KEYWORD;
    case Format_Type: return C_TYPE;
    case Format_ClassField: return C_FIELD;
    case Format_MagicAttr: return C_JS_SCOPE_VAR;
    case Format_Operator: return C_OPERATOR;
    case Format_Comment: return C_COMMENT;
    case Format_Doxygen: return C_DOXYGEN_COMMENT;
    case Format_Identifier: return C_TEXT;
    case Format_Whitespace: return C_VISUAL_WHITESPACE;
    case Format_ImportedModule: return C_STRING;
    case Format_LParen: return C_OPERATOR;
    case Format_RParen: return C_OPERATOR;
    case Format_FormatsAmount:
        QTC_CHECK(false); // should never get here
        return C_TEXT;
    }
    QTC_CHECK(false); // should never get here
    return C_TEXT;
}

class PythonHighlighter : public TextEditor::SyntaxHighlighter
{
public:
    PythonHighlighter()
    {
        setTextFormatCategories(Format_FormatsAmount, styleForFormat);
    }

private:
    void highlightBlock(const QString &text) override;
    int highlightLine(const QString &text, int initialState);
    void highlightImport(Internal::Scanner &scanner);

    int m_lastIndent = 0;
    bool withinLicenseHeader = false;
};

/**
 * @brief PythonHighlighter::highlightBlock highlights single line of Python code
 * @param text is single line without EOLN symbol. Access to all block data
 * can be obtained through inherited currentBlock() function.
 *
 * This function receives state (int number) from previously highlighted block,
 * scans block using received state and sets initial highlighting for current
 * block. At the end, it saves internal state in current block.
 */
void PythonHighlighter::highlightBlock(const QString &text)
{
    int initialState = previousBlockState();
    if (initialState == -1)
        initialState = 0;
    setCurrentBlockState(highlightLine(text, initialState));
}

/**
 * @return True if this keyword is acceptable at start of import line
 */
static bool isImportKeyword(const QString &keyword)
{
    return keyword == "import" || keyword == "from";
}

static int indent(const QString &line)
{
    for (int i = 0, size = line.size(); i < size; ++i) {
        if (!line.at(i).isSpace())
            return i;
    }
    return -1;
}

static void setFoldingIndent(const QTextBlock &block, int indent)
{
    TextEditor::TextBlockUserData::setFoldingIndent(block, indent);
    TextEditor::TextBlockUserData::setFoldingStartIncluded(block, false);
    TextEditor::TextBlockUserData::setFoldingEndIncluded(block, false);
}

/**
 * @brief Highlight line of code, returns new block state
 * @param text Source code to highlight
 * @param initialState Initial state of scanner, retrieved from previous block
 * @return Final state of scanner, should be saved with current block
 */
int PythonHighlighter::highlightLine(const QString &text, int initialState)
{
    Scanner scanner(text.constData(), text.size());
    scanner.setState(initialState);

    const int pos = indent(text);
    if (pos < 0) {
        // Empty lines do not change folding indent
        setFoldingIndent(currentBlock(), m_lastIndent);
    } else {
        m_lastIndent = pos;
        if (pos == 0 && text.startsWith('#') && !text.startsWith("#!")) {
            // A comment block at indentation 0. Fold on first line.
            setFoldingIndent(currentBlock(), withinLicenseHeader ? 1 : 0);
            withinLicenseHeader = true;
        } else {
            // Normal Python code. Line indentation can be used as folding indent.
            setFoldingIndent(currentBlock(), m_lastIndent);
            withinLicenseHeader = false;
        }
    }

    FormatToken tk;
    TextEditor::Parentheses parentheses;
    bool hasOnlyWhitespace = true;
    while (!(tk = scanner.read()).isEndOfBlock()) {
        Format format = tk.format();
        if (format == Format_Keyword && isImportKeyword(scanner.value(tk)) && hasOnlyWhitespace) {
            setFormat(tk.begin(), tk.length(), formatForCategory(format));
            highlightImport(scanner);
        } else if (format == Format_Comment
                   || format == Format_String
                   || format == Format_Doxygen) {
            setFormatWithSpaces(text, tk.begin(), tk.length(), formatForCategory(format));
        } else {
            if (format == Format_LParen) {
                parentheses.append(TextEditor::Parenthesis(TextEditor::Parenthesis::Opened,
                                                           text.at(tk.begin()), tk.begin()));
            } else if (format == Format_RParen) {
                parentheses.append(TextEditor::Parenthesis(TextEditor::Parenthesis::Closed,
                                                           text.at(tk.begin()), tk.begin()));
            }
            setFormat(tk.begin(), tk.length(), formatForCategory(format));
        }

        if (format != Format_Whitespace)
            hasOnlyWhitespace = false;
    }
    TextEditor::TextBlockUserData::setParentheses(currentBlock(), parentheses);
    return scanner.state();
}

/**
 * @brief Highlights rest of line as import directive
 */
void PythonHighlighter::highlightImport(Scanner &scanner)
{
    FormatToken tk;
    while (!(tk = scanner.read()).isEndOfBlock()) {
        Format format = tk.format();
        if (tk.format() == Format_Identifier)
            format = Format_ImportedModule;
        setFormat(tk.begin(), tk.length(), formatForCategory(format));
    }
}

TextEditor::SyntaxHighlighter *createPythonHighlighter()
{
    return new PythonHighlighter;
}

} // namespace Python::Internal

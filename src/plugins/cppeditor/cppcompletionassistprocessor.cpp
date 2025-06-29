// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cppcompletionassistprocessor.h"

#include <cppeditor/cppeditorconstants.h>

#include <cplusplus/BackwardsScanner.h>
#include <cplusplus/ExpressionUnderCursor.h>
#include <cplusplus/SimpleLexer.h>
#include <cplusplus/Token.h>

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

using namespace CPlusPlus;

namespace CppEditor {

CppCompletionAssistProcessor::CppCompletionAssistProcessor(int snippetItemOrder)
    : m_snippetCollector(QLatin1String(CppEditor::Constants::CPP_SNIPPETS_GROUP_ID),
                         QIcon(QLatin1String(":/texteditor/images/snippet.png")),
                         snippetItemOrder)
{
}

const QStringList CppCompletionAssistProcessor::preprocessorCompletions()
{
    static QStringList list{"define", "error", "include", "line", "pragma", "pragma once",
                            "pragma omp atomic", "pragma omp parallel", "pragma omp for",
                            "pragma omp ordered", "pragma omp parallel for", "pragma omp section",
                            "pragma omp sections", "pragma omp parallel sections", "pragma omp single",
                            "pragma omp master", "pragma omp critical", "pragma omp barrier",
                            "pragma omp flush", "pragma omp threadprivate", "undef", "if", "ifdef",
                            "ifndef", "elif", "else", "endif"};
    return list;
}

void CppCompletionAssistProcessor::addSnippets()
{
    m_completions.append(m_snippetCollector.collect());
}

static bool isDoxygenTagCompletionCharacter(const QChar &character)
{
    return character == QLatin1Char('\\')
        || character == QLatin1Char('@') ;
}

static bool twoIndentifiersBeforeLBrace(const Tokens &tokens, int tokenIdx)
{
    const Token &previousToken = tokens.at(tokenIdx - 1);
    if (previousToken.kind() != T_IDENTIFIER)
        return false;
    for (int index = tokenIdx - 2; index >= 0; index -= 2) {
        const Token &token = tokens.at(index);
        if (token.kind() == T_IDENTIFIER)
            return true;

        if (token.kind() != T_COLON_COLON)
            return false;
    }
    return false;
}

void CppCompletionAssistProcessor::startOfOperator(QTextDocument *textDocument,
        int positionInDocument,
        unsigned *kind,
        int &start,
        const CPlusPlus::LanguageFeatures &languageFeatures,
        bool adjustForQt5SignalSlotCompletion,
        DotAtIncludeCompletionHandler dotAtIncludeCompletionHandler)
{
    if (start != positionInDocument) {
        QTextCursor tc(textDocument);
        tc.setPosition(positionInDocument);

        // Include completion: make sure the quote character is the first one on the line
        if (*kind == T_STRING_LITERAL) {
            QTextCursor s = tc;
            s.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
            QString sel = s.selectedText();
            if (sel.indexOf(QLatin1Char('"')) < sel.length() - 1) {
                *kind = T_EOF_SYMBOL;
                start = positionInDocument;
            }
        }

        if (*kind == T_COMMA) {
            ExpressionUnderCursor expressionUnderCursor(languageFeatures);
            if (expressionUnderCursor.startOfFunctionCall(tc) == -1) {
                *kind = T_EOF_SYMBOL;
                start = positionInDocument;
            }
        }

        SimpleLexer tokenize;
        tokenize.setLanguageFeatures(languageFeatures);
        tokenize.setSkipComments(false);
        const Tokens &tokens = tokenize(tc.block().text(), BackwardsScanner::previousBlockState(tc.block()));
        const int tokenIdx = SimpleLexer::tokenBefore(tokens, qMax(0, tc.positionInBlock() - 1)); // get the token at the left of the cursor
        const Token tk = (tokenIdx == -1) ? Token() : tokens.at(tokenIdx);
        const QChar characterBeforePositionInDocument
                = textDocument->characterAt(positionInDocument - 1);

        if (adjustForQt5SignalSlotCompletion && *kind == T_AMPER && tokenIdx > 0) {
            const Token &previousToken = tokens.at(tokenIdx - 1);
            if (previousToken.kind() == T_COMMA)
                start = positionInDocument - (tk.utf16charOffset - previousToken.utf16charOffset) - 1;
        } else if (*kind == T_DOXY_COMMENT && !(tk.is(T_DOXY_COMMENT) || tk.is(T_CPP_DOXY_COMMENT))) {
            *kind = T_EOF_SYMBOL;
            start = positionInDocument;
        // Do not complete in comments, except in doxygen comments for doxygen commands.
        // Do not complete in strings, except it is for include completion.
        } else if (tk.is(T_COMMENT) || tk.is(T_CPP_COMMENT)
                 || ((tk.is(T_CPP_DOXY_COMMENT) || tk.is(T_DOXY_COMMENT))
                        && !isDoxygenTagCompletionCharacter(characterBeforePositionInDocument))
                 || (tk.isLiteral() && (*kind != T_STRING_LITERAL
                                     && *kind != T_ANGLE_STRING_LITERAL
                                     && *kind != T_SLASH
                                     && *kind != T_DOT))) {
            *kind = T_EOF_SYMBOL;
            start = positionInDocument;
        // Include completion: can be triggered by slash, but only in a string
        } else if (*kind == T_SLASH && (tk.isNot(T_STRING_LITERAL) && tk.isNot(T_ANGLE_STRING_LITERAL))) {
            *kind = T_EOF_SYMBOL;
            start = positionInDocument;
        } else if (*kind == T_LPAREN) {
            if (tokenIdx > 0) {
                const Token &previousToken = tokens.at(tokenIdx - 1); // look at the token at the left of T_LPAREN
                switch (previousToken.kind()) {
                case T_IDENTIFIER:
                case T_GREATER:
                case T_SIGNAL:
                case T_SLOT:
                    break; // good

                // For lambdas (both calls and definitions), we don't want a function hint,
                // and we want to abort an existing one if the lambda is a function parameter,
                // as it introduces a new context.
                case T_RBRACKET:
                case T_RBRACE:
                    *kind = T_EOF_SYMBOL;
                    start = INT_MIN;
                    break;

                default:
                    // that's a bad token :)
                    *kind = T_EOF_SYMBOL;
                    start = positionInDocument;
                }
            } else {
                *kind = T_EOF_SYMBOL;
                start = positionInDocument;
            }
        } else if (*kind == T_LBRACE) {
            if (tokenIdx <= 0 || !twoIndentifiersBeforeLBrace(tokens, tokenIdx)) {
                *kind = T_EOF_SYMBOL;
                start = positionInDocument;
            }
        }
        // Check for include preprocessor directive
        else if (*kind == T_STRING_LITERAL || *kind == T_ANGLE_STRING_LITERAL || *kind == T_SLASH
                 || (*kind == T_DOT
                        && (tk.is(T_STRING_LITERAL) || tk.is(T_ANGLE_STRING_LITERAL)))) {
            bool include = false;
            if (tokens.size() >= 3) {
                if (tokens.at(0).is(T_POUND) && tokens.at(1).is(T_IDENTIFIER) && (tokens.at(2).is(T_STRING_LITERAL) ||
                                                                                  tokens.at(2).is(T_ANGLE_STRING_LITERAL))) {
                    const Token &directiveToken = tokens.at(1);
                    QString directive = tc.block().text().mid(directiveToken.utf16charsBegin(),
                                                              directiveToken.utf16chars());
                    if (directive == QLatin1String("include") ||
                            directive == QLatin1String("include_next") ||
                            directive == QLatin1String("import")) {
                        include = true;
                    }
                }
            }

            if (!include) {
                *kind = T_EOF_SYMBOL;
                start = positionInDocument;
            } else if (*kind == T_DOT && dotAtIncludeCompletionHandler){
                dotAtIncludeCompletionHandler(start, kind);
            }
        }
    }
}

} // namespace CppEditor

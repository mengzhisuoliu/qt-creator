// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmljscompletionassist.h"
#include "qmljseditor.h"
#include "qmljseditorconstants.h"
#include "qmljsreuse.h"
#include "qmlexpressionundercursor.h"

#include <coreplugin/idocument.h>

#include <texteditor/codeassist/assistinterface.h>
#include <texteditor/codeassist/genericproposal.h>
#include <texteditor/codeassist/functionhintproposal.h>
#include <texteditor/codeassist/ifunctionhintproposalmodel.h>
#include <texteditor/texteditorsettings.h>
#include <texteditor/completionsettings.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projecttree.h>

#include <qmljs/qmljsmodelmanagerinterface.h>
#include <qmljs/parser/qmljsast_p.h>
#include <qmljs/qmljsinterpreter.h>
#include <qmljs/qmljscontext.h>
#include <qmljs/qmljsscopechain.h>
#include <qmljs/qmljsscanner.h>
#include <qmljs/qmljsbind.h>
#include <qmljs/qmljscompletioncontextfinder.h>
#include <qmljs/qmljsbundle.h>
#include <qmljs/qmljsscopebuilder.h>

#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QStringList>
#include <QIcon>
#include <QTextDocumentFragment>

using namespace QmlJS;
using namespace QmlJSTools;
using namespace TextEditor;
using namespace Utils;

namespace QmlJSEditor {

using namespace Internal;

namespace {

enum CompletionOrder {
    EnumValueOrder = -5,
    SnippetOrder = -15,
    PropertyOrder = -10,
    SymbolOrder = -20,
    KeywordOrder = -25,
    TypeOrder = -30
};

static void addCompletion(QList<AssistProposalItemInterface *> *completions,
                          const QString &text,
                          const QIcon &icon,
                          int order,
                          const QVariant &data = QVariant())
{
    if (text.isEmpty())
        return;

    AssistProposalItem *item = new QmlJSAssistProposalItem;
    item->setText(text);
    item->setIcon(icon);
    item->setOrder(order);
    item->setData(data);
    completions->append(item);
}

static void addCompletions(QList<AssistProposalItemInterface *> *completions,
                           const QStringList &newCompletions,
                           const QIcon &icon,
                           int order)
{
    for (const QString &text : newCompletions)
        addCompletion(completions, text, icon, order);
}

class PropertyProcessor
{
public:
    virtual void operator()(const Value *base, const QString &name, const Value *value) = 0;
};

class CompleteFunctionCall
{
public:
    CompleteFunctionCall(bool hasArguments = true) : hasArguments(hasArguments) {}
    bool hasArguments;
};

class CompletionAdder : public PropertyProcessor
{
protected:
    QList<AssistProposalItemInterface *> *completions;

public:
    CompletionAdder(QList<AssistProposalItemInterface *> *completions,
                    const QIcon &icon, int order)
        : completions(completions)
        , icon(icon)
        , order(order)
    {}

    void operator()(const Value *base, const QString &name, const Value *value) override
    {
        Q_UNUSED(base)
        QVariant data;
        if (const FunctionValue *func = value->asFunctionValue()) {
            // constructors usually also have other interesting members,
            // don't consider them pure functions and complete the '()'
            if (!func->lookupMember(QLatin1String("prototype"), nullptr, nullptr, false))
                data = QVariant::fromValue(CompleteFunctionCall(func->namedArgumentCount() || func->isVariadic()));
        }
        addCompletion(completions, name, icon, order, data);
    }

    QIcon icon;
    int order;
};

class LhsCompletionAdder : public CompletionAdder
{
public:
    LhsCompletionAdder(QList<AssistProposalItemInterface *> *completions,
                       const QIcon &icon,
                       int order,
                       bool afterOn)
        : CompletionAdder(completions, icon, order)
        , afterOn(afterOn)
    {}

    void operator ()(const Value *base, const QString &name, const Value *) override
    {
        const CppComponentValue *qmlBase = value_cast<CppComponentValue>(base);

        QString itemText = name;
        QString postfix;
        if (!itemText.isEmpty() && itemText.at(0).isLower())
            postfix = QLatin1String(": ");
        if (afterOn)
            postfix = QLatin1String(" {");

        // readonly pointer properties (anchors, ...) always get a .
        if (qmlBase && !qmlBase->isWritable(name) && qmlBase->isPointer(name))
            postfix = QLatin1Char('.');

        itemText.append(postfix);

        addCompletion(completions, itemText, icon, order);
    }

    bool afterOn;
};

class ProcessProperties: private MemberProcessor
{
    QSet<const ObjectValue *> _processed;
    bool _globalCompletion = false;
    bool _enumerateGeneratedSlots = false;
    bool _enumerateMethods = true;
    const ScopeChain *_scopeChain;
    const ObjectValue *_currentObject = nullptr;
    PropertyProcessor *_propertyProcessor = nullptr;

public:
    ProcessProperties(const ScopeChain *scopeChain)
        : _scopeChain(scopeChain)
    {
    }

    void setGlobalCompletion(bool globalCompletion)
    {
        _globalCompletion = globalCompletion;
    }

    void setEnumerateGeneratedSlots(bool enumerate)
    {
        _enumerateGeneratedSlots = enumerate;
    }

    void setEnumerateMethods(bool enumerate)
    {
        _enumerateMethods = enumerate;
    }

    void operator ()(const Value *value, PropertyProcessor *processor)
    {
        _processed.clear();
        _propertyProcessor = processor;

        processProperties(value);
    }

    void operator ()(PropertyProcessor *processor)
    {
        _processed.clear();
        _propertyProcessor = processor;

        const QList<const ObjectValue *> scopes = _scopeChain->all();
        for (const ObjectValue *scope : scopes)
            processProperties(scope);
    }

private:
    void process(const QString &name, const Value *value)
    {
        (*_propertyProcessor)(_currentObject, name, value);
    }

    bool processProperty(const QString &name, const Value *value, const PropertyInfo &) override
    {
        process(name, value);
        return true;
    }

    bool processEnumerator(const QString &name, const Value *value) override
    {
        if (! _globalCompletion)
            process(name, value);
        return true;
    }

    bool processSignal(const QString &name, const Value *value) override
    {
        if (_globalCompletion || _enumerateMethods)
            process(name, value);
        return true;
    }

    bool processSlot(const QString &name, const Value *value) override
    {
        if (_enumerateMethods)
            process(name, value);
        return true;
    }

    bool processGeneratedSlot(const QString &name, const Value *value) override
    {
        if (_enumerateGeneratedSlots || (_currentObject && _currentObject->className().endsWith(QLatin1String("Keys")))) {
            // ### FIXME: add support for attached properties.
            process(name, value);
        }
        return true;
    }

    void processProperties(const Value *value)
    {
        if (! value)
            return;
        if (const ObjectValue *object = value->asObjectValue())
            processProperties(object);
    }

    void processProperties(const ObjectValue *object)
    {
        if (! object || !Utils::insert(_processed, object))
            return;

        processProperties(object->prototype(_scopeChain->context()));

        _currentObject = object;
        object->processMembers(this);
        _currentObject = nullptr;
    }
};

const Value *getPropertyValue(const ObjectValue *object,
                                           const QStringList &propertyNames,
                                           const ContextPtr &context)
{
    if (propertyNames.isEmpty() || !object)
        return nullptr;

    const Value *value = object;
    for (const QString &name : propertyNames) {
        if (const ObjectValue *objectValue = value->asObjectValue()) {
            value = objectValue->lookupMember(name, context);
            if (!value)
                return nullptr;
        } else {
            return nullptr;
        }
    }
    return value;
}

bool isLiteral(AST::Node *ast)
{
    if (AST::cast<AST::StringLiteral *>(ast))
        return true;
    else if (AST::cast<AST::NumericLiteral *>(ast))
        return true;
    else
        return false;
}

} // Anonymous

QStringList qmlJSAutoComplete(QTextDocument *textDocument,
                              int position,
                              const Utils::FilePath &fileName,
                              TextEditor::AssistReason reason,
                              const SemanticInfo &info)
{
    QStringList list;
    QmlJSCompletionAssistProcessor processor;
    QTextCursor cursor(textDocument);
    cursor.setPosition(position);
    std::unique_ptr<QmlJSCompletionAssistInterface>
        interface = std::make_unique<QmlJSCompletionAssistInterface>(cursor, fileName, reason, info);
    if (QScopedPointer<IAssistProposal> proposal{processor.start(std::move(interface))}) {
        GenericProposalModelPtr model = proposal->model().staticCast<GenericProposalModel>();

        int basePosition = proposal->basePosition();
        const QString prefix = textDocument->toPlainText().mid(basePosition,
                                                               position - basePosition);

        if (reason == TextEditor::ExplicitlyInvoked) {
            model->filter(prefix);
            model->sort(prefix);
        }

        for (int i = 0; i < model->size(); ++i)
            list.append(proposal->model()->text(i).trimmed());
        list.append(prefix);
    }

    return list;
}

} // namesapce QmlJSEditor

Q_DECLARE_METATYPE(QmlJSEditor::CompleteFunctionCall)

namespace QmlJSEditor {

// -----------------------
// QmlJSAssistProposalItem
// -----------------------
bool QmlJSAssistProposalItem::prematurelyApplies(const QChar &c) const
{
    if (data().canConvert<QString>()) // snippet
        return false;

    return (text().endsWith(QLatin1String(": ")) && c == QLatin1Char(':'))
            || (text().endsWith(QLatin1Char('.')) && c == QLatin1Char('.'));
}

void QmlJSAssistProposalItem::applyContextualContent(TextEditorWidget *editorWidget,
                                                     int basePosition) const
{
    QTC_ASSERT(editorWidget, return);
    const int currentPosition = editorWidget->position();
    editorWidget->replace(basePosition, currentPosition - basePosition, QString());

    QString content = text();
    int cursorOffset = 0;

    const bool autoInsertBrackets =
        TextEditorSettings::completionSettings().m_autoInsertBrackets;

    if (autoInsertBrackets && data().canConvert<CompleteFunctionCall>()) {
        CompleteFunctionCall function = data().value<CompleteFunctionCall>();
        content += QLatin1String("()");
        if (function.hasArguments)
            cursorOffset = -1;
    }

    QString replaceable = content;
    int replacedLength = 0;
    for (int i = 0; i < replaceable.length(); ++i) {
        const QChar a = replaceable.at(i);
        const QChar b = editorWidget->characterAt(editorWidget->position() + i);
        if (a == b)
            ++replacedLength;
        else
            break;
    }
    const int length = editorWidget->position() - basePosition + replacedLength;
    editorWidget->replace(basePosition, length, content);
    if (cursorOffset) {
        editorWidget->setCursorPosition(editorWidget->position() + cursorOffset);
        editorWidget->setAutoCompleteSkipPosition(editorWidget->textCursor());
    }
}

// -------------------------
// FunctionHintProposalModel
// -------------------------
class FunctionHintProposalModel : public IFunctionHintProposalModel
{
public:
    FunctionHintProposalModel(const QString &functionName, const QStringList &namedArguments,
                              int optionalNamedArguments, bool isVariadic)
        : m_functionName(functionName)
        , m_namedArguments(namedArguments)
        , m_optionalNamedArguments(optionalNamedArguments)
        , m_isVariadic(isVariadic)
    {}

    void reset() override {}
    int size() const override { return 1; }
    QString text(int index) const override;
    int activeArgument(const QString &prefix) const override;

private:
    QString m_functionName;
    QStringList m_namedArguments;
    int m_optionalNamedArguments;
    bool m_isVariadic;
};

QString FunctionHintProposalModel::text(int index) const
{
    Q_UNUSED(index)

    QString prettyMethod;
    prettyMethod += QString::fromLatin1("function ");
    prettyMethod += m_functionName;
    prettyMethod += QLatin1Char('(');
    for (int i = 0; i < m_namedArguments.size(); ++i) {
        if (i == m_namedArguments.size() - m_optionalNamedArguments)
            prettyMethod += QLatin1Char('[');
        if (i != 0)
            prettyMethod += QLatin1String(", ");

        QString arg = m_namedArguments.at(i);
        if (arg.isEmpty()) {
            arg = QLatin1String("arg");
            arg += QString::number(i + 1);
        }

        prettyMethod += arg;
    }
    if (m_isVariadic) {
        if (!m_namedArguments.isEmpty())
            prettyMethod += QLatin1String(", ");
        prettyMethod += QLatin1String("...");
    }
    if (m_optionalNamedArguments)
        prettyMethod += QLatin1Char(']');
    prettyMethod += QLatin1Char(')');
    return prettyMethod;
}

int FunctionHintProposalModel::activeArgument(const QString &prefix) const
{
    int argnr = 0;
    int parcount = 0;
    Scanner tokenize;
    const QList<Token> tokens = tokenize(prefix);
    for (auto &tk : tokens) {
        if (tk.is(Token::LeftParenthesis))
            ++parcount;
        else if (tk.is(Token::RightParenthesis))
            --parcount;
        else if (! parcount && tk.is(Token::Colon))
            ++argnr;
    }

    if (parcount < 0)
        return -1;

    return argnr;
}

// -----------------------------
// QmlJSCompletionAssistProvider
// -----------------------------
int QmlJSCompletionAssistProvider::activationCharSequenceLength() const
{
    return 1;
}

bool QmlJSCompletionAssistProvider::isActivationCharSequence(const QString &sequence) const
{
    return isActivationChar(sequence.at(0));
}

bool QmlJSCompletionAssistProvider::isContinuationChar(const QChar &c) const
{
    return isIdentifierChar(c, false);
}

IAssistProcessor *QmlJSCompletionAssistProvider::createProcessor(const AssistInterface *) const
{
    return new QmlJSCompletionAssistProcessor;
}

// ------------------------------
// QmlJSCompletionAssistProcessor
// ------------------------------
QmlJSCompletionAssistProcessor::QmlJSCompletionAssistProcessor()
    : m_startPosition(0)
    , m_snippetCollector(QLatin1String(Constants::QML_SNIPPETS_GROUP_ID), iconForColor(Qt::red), SnippetOrder)
{}

QmlJSCompletionAssistProcessor::~QmlJSCompletionAssistProcessor() = default;

IAssistProposal *QmlJSCompletionAssistProcessor::createContentProposal() const
{
    GenericProposalModelPtr model(new QmlJSAssistProposalModel(m_completions));
    return new GenericProposal(m_startPosition, model);
}

IAssistProposal *QmlJSCompletionAssistProcessor::createHintProposal(
        const QString &functionName, const QStringList &namedArguments,
        int optionalNamedArguments, bool isVariadic) const
{
    FunctionHintProposalModelPtr model(new FunctionHintProposalModel(
                functionName, namedArguments, optionalNamedArguments, isVariadic));
    return new FunctionHintProposal(m_startPosition, model);
}

IAssistProposal *QmlJSCompletionAssistProcessor::performAsync()
{
    if (interface()->reason() == IdleEditor && !acceptsIdleEditor())
        return nullptr;

    m_startPosition = interface()->position();
    while (isIdentifierChar(interface()->textDocument()->characterAt(m_startPosition - 1), false, false))
        --m_startPosition;
    const bool onIdentifier = m_startPosition != interface()->position();

    m_completions.clear();

    auto qmlInterface = static_cast<const QmlJSCompletionAssistInterface *>(interface());
    const SemanticInfo &semanticInfo = qmlInterface->semanticInfo();
    if (!semanticInfo.isValid())
        return nullptr;

    const Document::Ptr document = semanticInfo.document;

    bool isQmlFile = false;
    if (interface()->filePath().endsWith(".qml"))
        isQmlFile = true;

    const QList<AST::Node *> path = semanticInfo.rangePath(interface()->position());
    const ContextPtr &context = semanticInfo.context;
    const ScopeChain &scopeChain = semanticInfo.scopeChain(path);

    // The completionOperator is the character under the cursor or directly before the
    // identifier under cursor. Use in conjunction with onIdentifier. Examples:
    // a + b<complete> -> ' '
    // a +<complete> -> '+'
    // a +b<complete> -> '+'
    QChar completionOperator;
    if (m_startPosition > 0)
        completionOperator = interface()->textDocument()->characterAt(m_startPosition - 1);

    QTextCursor startPositionCursor(qmlInterface->textDocument());
    startPositionCursor.setPosition(m_startPosition);
    CompletionContextFinder contextFinder(startPositionCursor);

    const ObjectValue *qmlScopeType = nullptr;
    if (contextFinder.isInQmlContext()) {
        // find the enclosing qml object
        // ### this should use semanticInfo.declaringMember instead, but that may also return functions
        int i;
        for (i = path.size() - 1; i >= 0; --i) {
            AST::Node *node = path[i];
            if (AST::cast<AST::UiObjectDefinition *>(node) || AST::cast<AST::UiObjectBinding *>(node)) {
                qmlScopeType = document->bind()->findQmlObject(node);
                if (qmlScopeType)
                    break;
            }
        }
        // grouped property bindings change the scope type
        for (i++; i < path.size(); ++i) {
            auto objDef = AST::cast<AST::UiObjectDefinition *>(path[i]);
            if (!objDef || !document->bind()->isGroupedPropertyBinding(objDef))
                break;
            const ObjectValue *newScopeType = qmlScopeType;
            for (AST::UiQualifiedId *it = objDef->qualifiedTypeNameId; it; it = it->next) {
                if (!newScopeType || it->name.isEmpty()) {
                    newScopeType = nullptr;
                    break;
                }
                const Value *v = newScopeType->lookupMember(it->name.toString(), context);
                v = context->lookupReference(v);
                newScopeType = value_cast<ObjectValue>(v);
            }
            if (!newScopeType)
                break;
            qmlScopeType = newScopeType;
        }
        // fallback to getting the base type object
        if (!qmlScopeType)
            qmlScopeType = context->lookupType(document.data(), contextFinder.qmlObjectTypeName());
    }

    if (contextFinder.isInStringLiteral()) {
        // get the text of the literal up to the cursor position
        //QTextCursor tc = textWidget->textCursor();
        QTextCursor tc(qmlInterface->textDocument());
        tc.setPosition(qmlInterface->position());
        QmlExpressionUnderCursor expressionUnderCursor;
        expressionUnderCursor(tc);
        QString literalText = expressionUnderCursor.text();

        // expression under cursor only looks at one line, so multi-line strings
        // are handled incorrectly and are recognizable by don't starting with ' or "
        if (!literalText.isEmpty()
                && literalText.at(0) != QLatin1Char('"')
                && literalText.at(0) != QLatin1Char('\'')) {
            return nullptr;
        }

        literalText = literalText.mid(1);

        if (contextFinder.isInImport()) {
            QStringList patterns;
            patterns << QLatin1String("*.qml") << QLatin1String("*.js");
            if (completeFileName(document->path(), literalText, patterns))
                return createContentProposal();
            return nullptr;
        }

        const Value *value =
                getPropertyValue(qmlScopeType, contextFinder.bindingPropertyName(), context);
        if (!value) {
            // do nothing
        } else if (value->asUrlValue()) {
            if (completeUrl(document->path(), literalText))
                return createContentProposal();
        }

        // ### enum completion?

        return nullptr;
    }

    // currently path-in-stringliteral is the only completion available in imports
    if (contextFinder.isInImport()) {
        ModelManagerInterface::ProjectInfo pInfo = ModelManagerInterface::instance()
                ->projectInfo(ProjectExplorer::ProjectTree::currentProject());
        QmlBundle platform = pInfo.extendedBundle.bundleForLanguage(document->language());
        if (!platform.supportedImports().isEmpty()) {
            QTextCursor tc(qmlInterface->textDocument());
            tc.setPosition(qmlInterface->position());
            QmlExpressionUnderCursor expressionUnderCursor;
            expressionUnderCursor(tc);
            QString libVersion = contextFinder.libVersionImport();
            if (!libVersion.isNull()) {
                QStringList completions = platform.supportedImports().complete(libVersion, QString(),
                    PersistentTrie::LookupFlags(PersistentTrie::CaseInsensitive|PersistentTrie::SkipChars|PersistentTrie::SkipSpaces));
                completions = PersistentTrie::matchStrengthSort(libVersion, completions);

                int toSkip = qMax(libVersion.lastIndexOf(QLatin1Char(' '))
                                  , libVersion.lastIndexOf(QLatin1Char('.')));
                if (++toSkip > 0) {
                    QStringList nCompletions;
                    QString prefix(libVersion.left(toSkip));
                    nCompletions.reserve(completions.size());
                    for (const QString &completion : std::as_const(completions))
                        if (completion.startsWith(prefix))
                            nCompletions.append(completion.right(completion.size()-toSkip));
                    completions = nCompletions;
                }
                addCompletions(&m_completions, completions, QmlJSCompletionAssistInterface::fileNameIcon(), KeywordOrder);
                return createContentProposal();
            }
        }
        return nullptr;
    }

    // member "a.bc<complete>" or function "foo(<complete>" completion
    if (completionOperator == QLatin1Char('.')
            || (completionOperator == QLatin1Char('(') && !onIdentifier)) {
        // Look at the expression under cursor.
        //QTextCursor tc = textWidget->textCursor();
        QTextCursor tc(qmlInterface->textDocument());
        tc.setPosition(m_startPosition - 1);

        QmlExpressionUnderCursor expressionUnderCursor;
        AST::ExpressionNode *expression = expressionUnderCursor(tc);

        if (expression && ! isLiteral(expression)) {
            // Evaluate the expression under cursor.
            ValueOwner *interp = context->valueOwner();
            const Value *value =
                    interp->convertToObject(scopeChain.evaluate(expression));
            //qCDebug(qmljsLog) << "type:" << interp->typeId(value);

            if (value && completionOperator == QLatin1Char('.')) { // member completion
                ProcessProperties processProperties(&scopeChain);
                if (contextFinder.isInLhsOfBinding() && qmlScopeType) {
                    LhsCompletionAdder completionAdder(&m_completions, QmlJSCompletionAssistInterface::symbolIcon(),
                                                       PropertyOrder, contextFinder.isAfterOnInLhsOfBinding());
                    processProperties.setEnumerateGeneratedSlots(true);
                    processProperties(value, &completionAdder);
                } else {
                    CompletionAdder completionAdder(&m_completions, QmlJSCompletionAssistInterface::symbolIcon(), SymbolOrder);
                    processProperties(value, &completionAdder);
                }
            } else if (value
                       && completionOperator == QLatin1Char('(')
                       && m_startPosition == interface()->position()) {
                // function completion
                if (const FunctionValue *f = value->asFunctionValue()) {
                    QString functionName = expressionUnderCursor.text();
                    int indexOfDot = functionName.lastIndexOf(QLatin1Char('.'));
                    if (indexOfDot != -1)
                        functionName = functionName.mid(indexOfDot + 1);

                    QStringList namedArguments;
                    for (int i = 0; i < f->namedArgumentCount(); ++i)
                        namedArguments.append(f->argumentName(i));

                    return createHintProposal(functionName.trimmed(), namedArguments,
                                              f->optionalNamedArgumentCount(), f->isVariadic());
                }
            }
        }

        if (! m_completions.isEmpty())
            return createContentProposal();
        return nullptr;
    }

    // global completion
    if (onIdentifier || interface()->reason() == ExplicitlyInvoked) {

        bool doGlobalCompletion = true;
        bool doQmlKeywordCompletion = true;
        bool doJsKeywordCompletion = true;
        bool doQmlTypeCompletion = false;

        if (contextFinder.isInLhsOfBinding() && qmlScopeType) {
            doGlobalCompletion = false;
            doJsKeywordCompletion = false;
            doQmlTypeCompletion = true;

            ProcessProperties processProperties(&scopeChain);
            processProperties.setGlobalCompletion(true);
            processProperties.setEnumerateGeneratedSlots(true);
            processProperties.setEnumerateMethods(false);

            // id: is special
            AssistProposalItem *idProposalItem = new QmlJSAssistProposalItem;
            idProposalItem->setText(QLatin1String("id: "));
            idProposalItem->setIcon(QmlJSCompletionAssistInterface::symbolIcon());
            idProposalItem->setOrder(PropertyOrder);
            m_completions.append(idProposalItem);

            {
                LhsCompletionAdder completionAdder(&m_completions, QmlJSCompletionAssistInterface::symbolIcon(),
                                                   PropertyOrder, contextFinder.isAfterOnInLhsOfBinding());
                processProperties(qmlScopeType, &completionAdder);
            }

            if (ScopeBuilder::isPropertyChangesObject(context, qmlScopeType)
                    && scopeChain.qmlScopeObjects().size() == 2) {
                CompletionAdder completionAdder(&m_completions, QmlJSCompletionAssistInterface::symbolIcon(), SymbolOrder);
                processProperties(scopeChain.qmlScopeObjects().constFirst(), &completionAdder);
            }
        }

        if (contextFinder.isInRhsOfBinding() && qmlScopeType) {
            doQmlKeywordCompletion = false;

            // complete enum values for enum properties
            const Value *value =
                    getPropertyValue(qmlScopeType, contextFinder.bindingPropertyName(), context);
            if (const QmlEnumValue *enumValue =
                    value_cast<QmlEnumValue>(value)) {
                const QString &name = context->imports(document.data())->nameForImportedObject(enumValue->owner(), context.data());
                const QStringList keys = enumValue->keys();
                for (const QString &key : keys) {
                    QString completion;
                    if (name.isEmpty())
                        completion = QString::fromLatin1("\"%1\"").arg(key);
                    else
                        completion = QString::fromLatin1("%1.%2").arg(name, key);
                    addCompletion(&m_completions, key, QmlJSCompletionAssistInterface::symbolIcon(),
                                  EnumValueOrder, completion);
                }
            }
        }

        if (!contextFinder.isInImport() && !contextFinder.isInQmlContext())
            doQmlTypeCompletion = true;

        if (doQmlTypeCompletion) {
            if (const ObjectValue *qmlTypes = scopeChain.qmlTypes()) {
                ProcessProperties processProperties(&scopeChain);
                CompletionAdder completionAdder(&m_completions, QmlJSCompletionAssistInterface::symbolIcon(), TypeOrder);
                processProperties(qmlTypes, &completionAdder);
            }
        }

        if (doGlobalCompletion) {
            // It's a global completion.
            ProcessProperties processProperties(&scopeChain);
            processProperties.setGlobalCompletion(true);
            CompletionAdder completionAdder(&m_completions, QmlJSCompletionAssistInterface::symbolIcon(), SymbolOrder);
            processProperties(&completionAdder);
        }

        if (doJsKeywordCompletion) {
            // add js keywords
            addCompletions(&m_completions, Scanner::keywords(), QmlJSCompletionAssistInterface::keywordIcon(), KeywordOrder);
        }

        // add qml extra words
        if (doQmlKeywordCompletion && isQmlFile) {
            static QStringList qmlWords{
                QLatin1String("property"),
                //QLatin1String("readonly")
                QLatin1String("signal"),
                QLatin1String("import")
            };
            static QStringList qmlWordsAlsoInJs{
                QLatin1String("default"), QLatin1String("function")
            };

            addCompletions(&m_completions, qmlWords, QmlJSCompletionAssistInterface::keywordIcon(), KeywordOrder);
            if (!doJsKeywordCompletion)
                addCompletions(&m_completions, qmlWordsAlsoInJs, QmlJSCompletionAssistInterface::keywordIcon(), KeywordOrder);
        }

        m_completions.append(m_snippetCollector.collect());

        if (! m_completions.isEmpty())
            return createContentProposal();
        return nullptr;
    }

    return nullptr;
}

bool QmlJSCompletionAssistProcessor::acceptsIdleEditor() const
{
    const int cursorPos = interface()->position();

    bool maybeAccept = false;
    const QChar &charBeforeCursor = interface()->textDocument()->characterAt(cursorPos - 1);
    if (isActivationChar(charBeforeCursor)) {
        maybeAccept = true;
    } else {
        const QChar &charUnderCursor = interface()->textDocument()->characterAt(cursorPos);
        if (isValidIdentifierChar(charUnderCursor))
            return false;
        if (isIdentifierChar(charBeforeCursor)
                && ((charUnderCursor.isSpace()
                    || charUnderCursor.isNull()
                    || isDelimiterChar(charUnderCursor))
                || isIdentifierChar(charUnderCursor))) {

            int startPos = cursorPos - 1;
            for (; startPos != -1; --startPos) {
                if (!isIdentifierChar(interface()->textDocument()->characterAt(startPos)))
                    break;
            }
            ++startPos;

            const QString &word = interface()->textAt(startPos, cursorPos - startPos);
            if (word.length() >= TextEditorSettings::completionSettings().m_characterThreshold
                    && isIdentifierChar(word.at(0), true)) {
                for (int i = 1; i < word.length(); ++i) {
                    if (!isIdentifierChar(word.at(i)))
                        return false;
                }
                maybeAccept = true;
            }
        }
    }

    if (maybeAccept) {
        QTextCursor tc(interface()->textDocument());
        tc.setPosition(interface()->position());
        const QTextBlock &block = tc.block();
        const QString &blockText = block.text();
        const int blockState = qMax(0, block.previous().userState());

        Scanner scanner;
        const QList<Token> tokens = scanner(blockText, blockState);
        const int column = block.position() - interface()->position();
        for (const Token &tk : tokens) {
            if (column >= tk.begin() && column <= tk.end()) {
                if (charBeforeCursor == QLatin1Char('/') && tk.is(Token::String))
                    return true; // path completion inside string literals
                if (tk.is(Token::Comment) || tk.is(Token::String) || tk.is(Token::RegExp))
                    return false;
                break;
            }
        }
        if (charBeforeCursor != QLatin1Char('/'))
            return true;
    }

    return false;
}

bool QmlJSCompletionAssistProcessor::completeFileName(const FilePath &relativeBasePath,
                                                      const QString &fileName,
                                                      const QStringList &patterns)
{
    FilePath directoryPrefix = relativeBasePath.resolvePath(fileName);
    if (!fileName.endsWith('/'))
        directoryPrefix = directoryPrefix.parentDir();

    if (!directoryPrefix.exists())
        return false;

    directoryPrefix.iterateDirectory([this](const FilePath &filePath) {
        AssistProposalItem *item = new QmlJSAssistProposalItem;
        item->setText(filePath.fileName());
        item->setIcon(QmlJSCompletionAssistInterface::fileNameIcon());
        m_completions.append(item);
        return IterationPolicy::Continue;
    }, {patterns, QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot});

    return !m_completions.isEmpty();
}

bool QmlJSCompletionAssistProcessor::completeUrl(const FilePath &relativeBasePath, const QString &urlString)
{
    const QUrl url(urlString);
    QString fileName;
    if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) == 0) {
        fileName = url.toLocalFile();
        // should not trigger completion on 'file://'
        if (fileName.isEmpty())
            return false;
    } else if (url.scheme().isEmpty()) {
        // don't trigger completion while typing a scheme
        if (urlString.endsWith(QLatin1String(":/")))
            return false;
        fileName = urlString;
    } else {
        return false;
    }

    return completeFileName(relativeBasePath, fileName);
}

// ------------------------------
// QmlJSCompletionAssistInterface
// ------------------------------
QmlJSCompletionAssistInterface::QmlJSCompletionAssistInterface(const QTextCursor &cursor,
                                                               const Utils::FilePath &fileName,
                                                               AssistReason reason,
                                                               const SemanticInfo &info)
    : AssistInterface(cursor, fileName, reason)
    , m_semanticInfo(info)
{}

const SemanticInfo &QmlJSCompletionAssistInterface::semanticInfo() const
{
    return m_semanticInfo;
}

const QIcon &QmlJSCompletionAssistInterface::fileNameIcon()
{
    static QIcon darkBlueIcon(iconForColor(Qt::darkBlue));
    return darkBlueIcon;
}

const QIcon &QmlJSCompletionAssistInterface::keywordIcon()
{
    static QIcon darkYellowIcon(iconForColor(Qt::darkYellow));
    return darkYellowIcon;
}

const QIcon &QmlJSCompletionAssistInterface::symbolIcon()
{
    static QIcon darkCyanIcon(iconForColor(Qt::darkCyan));
    return darkCyanIcon;
}

namespace {

class QmlJSLessThan
{
public:
    QmlJSLessThan(const QString &searchString) : m_searchString(searchString)
    { }
    bool operator() (const AssistProposalItemInterface *a, const AssistProposalItemInterface *b)
    {
        if (a->order() != b->order())
            return a->order() > b->order();
        else if (a->text().isEmpty() && ! b->text().isEmpty())
            return true;
        else if (b->text().isEmpty())
            return false;
        else if (a->text().at(0).isUpper() && b->text().at(0).isLower())
            return false;
        else if (a->text().at(0).isLower() && b->text().at(0).isUpper())
            return true;
        int m1 = PersistentTrie::matchStrength(m_searchString, a->text());
        int m2 = PersistentTrie::matchStrength(m_searchString, b->text());
        if (m1 != m2)
            return m1 > m2;
        return a->text() < b->text();
    }
private:
    QString m_searchString;
};

} // Anonymous

// -------------------------
// QmlJSAssistProposalModel
// -------------------------
void QmlJSAssistProposalModel::filter(const QString &prefix)
{
    GenericProposalModel::filter(prefix);
    if (prefix.startsWith(QLatin1String("__")))
        return;
    QList<AssistProposalItemInterface *> newCurrentItems;
    newCurrentItems.reserve(m_currentItems.size());
    for (AssistProposalItemInterface *item : std::as_const(m_currentItems))
        if (!item->text().startsWith(QLatin1String("__")))
            newCurrentItems << item;
    m_currentItems = newCurrentItems;
}

void QmlJSAssistProposalModel::sort(const QString &prefix)
{
    std::sort(m_currentItems.begin(), m_currentItems.end(), QmlJSLessThan(prefix));
}

bool QmlJSAssistProposalModel::keepPerfectMatch(AssistReason reason) const
{
    return reason == ExplicitlyInvoked;
}

} // namespace QmlJSEditor

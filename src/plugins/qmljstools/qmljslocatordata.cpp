// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmljslocatordata.h"

#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>

#include <qmljs/qmljsmodelmanagerinterface.h>
#include <qmljs/qmljsutils.h>
#include <qmljs/parser/qmljsast_p.h>

#include <QDebug>
#include <QMutexLocker>

using namespace QmlJS;
using namespace QmlJS::AST;

namespace QmlJSTools::Internal {

LocatorData::LocatorData()
{
    ModelManagerInterface *manager = ModelManagerInterface::instance();
    Q_ASSERT(thread() == manager->thread()); // we do not protect accesses below

    // Force the updating of source file when updating a project (they could be cached, in such
    // case LocatorData::onDocumentUpdated will not be called.
    connect(manager,
            &ModelManagerInterface::projectInfoUpdated,
            [manager](const ModelManagerInterface::ProjectInfo &info) {
                Utils::FilePaths files;
                for (const Utils::FilePath &f :
                     info.project->files(ProjectExplorer::Project::SourceFiles))
                    files << f;
                manager->updateSourceFiles(files, true);
            });

    connect(manager, &ModelManagerInterface::documentUpdated,
            this, &LocatorData::onDocumentUpdated);
    connect(manager, &ModelManagerInterface::aboutToRemoveFiles,
            this, &LocatorData::onAboutToRemoveFiles);

    ProjectExplorer::ProjectManager *session = ProjectExplorer::ProjectManager::instance();
    if (session)
        connect(session,
                &ProjectExplorer::ProjectManager::projectRemoved,
                this,
                [this](ProjectExplorer::Project *) { m_entries.clear(); });
}

LocatorData::~LocatorData() = default;

namespace {

class FunctionFinder : protected AST::Visitor
{
    QList<LocatorData::Entry> m_entries;
    Document::Ptr m_doc;
    QString m_context;
    QString m_documentContext;

public:
    QList<LocatorData::Entry> run(const Document::Ptr &doc)
    {
        m_doc = doc;
        if (!doc->componentName().isEmpty())
            m_documentContext = doc->componentName();
        else
            m_documentContext = doc->fileName().fileName();
        accept(doc->ast(), m_documentContext);
        return m_entries;
    }

protected:
    QString contextString(const QString &extra)
    {
        return QString::fromLatin1("%1, %2").arg(extra, m_documentContext);
    }

    LocatorData::Entry basicEntry(SourceLocation loc)
    {
        LocatorData::Entry entry;
        entry.type = LocatorData::Function;
        entry.extraInfo = m_context;
        entry.fileName = m_doc->fileName();
        entry.line = loc.startLine;
        entry.column = loc.startColumn - 1;
        return entry;
    }

    void accept(Node *ast, const QString &context)
    {
        const QString old = m_context;
        m_context = context;
        Node::accept(ast, this);
        m_context = old;
    }

    bool visit(FunctionDeclaration *ast) override
    {
        return visit(static_cast<FunctionExpression *>(ast));
    }

    bool visit(FunctionExpression *ast) override
    {
        if (ast->name.isEmpty())
            return true;

        LocatorData::Entry entry = basicEntry(ast->identifierToken);

        entry.type = LocatorData::Function;
        entry.displayName = ast->name.toString();
        entry.displayName += QLatin1Char('(');
        for (FormalParameterList *it = ast->formals; it; it = it->next) {
            if (it != ast->formals)
                entry.displayName += QLatin1String(", ");
            if (!it->element->bindingIdentifier.isEmpty())
                entry.displayName += it->element->bindingIdentifier.toString();
        }
        entry.displayName += QLatin1Char(')');
        entry.symbolName = entry.displayName;

        m_entries += entry;

        accept(ast->body, contextString(QString::fromLatin1("function %1").arg(entry.displayName)));
        return false;
    }

    bool visit(UiScriptBinding *ast) override
    {
        if (!ast->qualifiedId)
            return true;
        const QString qualifiedIdString = toString(ast->qualifiedId);

        if (cast<Block *>(ast->statement)) {
            LocatorData::Entry entry = basicEntry(ast->qualifiedId->identifierToken);
            entry.displayName = qualifiedIdString;
            entry.symbolName = qualifiedIdString;
            m_entries += entry;
        }

        accept(ast->statement, contextString(toString(ast->qualifiedId)));
        return false;
    }

    bool visit(UiObjectBinding *ast) override
    {
        if (!ast->qualifiedTypeNameId)
            return true;

        QString context = toString(ast->qualifiedTypeNameId);
        const QString id = idOfObject(ast);
        if (!id.isEmpty())
            context = QString::fromLatin1("%1 (%2)").arg(id, context);
        accept(ast->initializer, contextString(context));
        return false;
    }

    bool visit(UiObjectDefinition *ast) override
    {
        if (!ast->qualifiedTypeNameId)
            return true;

        QString context = toString(ast->qualifiedTypeNameId);
        const QString id = idOfObject(ast);
        if (!id.isEmpty())
            context = QString::fromLatin1("%1 (%2)").arg(id, context);
        accept(ast->initializer, contextString(context));
        return false;
    }

    bool visit(AST::BinaryExpression *ast) override
    {
        auto fieldExpr = AST::cast<AST::FieldMemberExpression *>(ast->left);
        auto funcExpr = AST::cast<AST::FunctionExpression *>(ast->right);

        if (fieldExpr && funcExpr && funcExpr->body && (ast->op == QSOperator::Assign)) {
            LocatorData::Entry entry = basicEntry(ast->operatorToken);

            entry.type = LocatorData::Function;
            entry.displayName = fieldExpr->name.toString();
            while (fieldExpr) {
                if (auto field = AST::cast<AST::FieldMemberExpression *>(fieldExpr->base)) {
                    entry.displayName.prepend(field->name.toString() + QLatin1Char('.'));
                    fieldExpr = field;
                } else {
                    if (auto ident = AST::cast<AST::IdentifierExpression *>(fieldExpr->base))
                        entry.displayName.prepend(ident->name.toString() + QLatin1Char('.'));
                    break;
                }
            }

            entry.displayName += QLatin1Char('(');
            for (FormalParameterList *it = funcExpr->formals; it; it = it->next) {
                if (it != funcExpr->formals)
                    entry.displayName += QLatin1String(", ");
                if (!it->element->bindingIdentifier.isEmpty())
                    entry.displayName += it->element->bindingIdentifier.toString();
            }
            entry.displayName += QLatin1Char(')');
            entry.symbolName = entry.displayName;

            m_entries += entry;

            accept(funcExpr->body, contextString(QString::fromLatin1("function %1").arg(entry.displayName)));
            return false;
        }

        return true;
    }

    void throwRecursionDepthError() override
    {
        qWarning("Warning: Hit maximum recursion limit visiting AST in FunctionFinder.");
    }
};
} // anonymous namespace

QHash<Utils::FilePath, QList<LocatorData::Entry>> LocatorData::entries() const
{
    QMutexLocker l(&m_mutex);
    return m_entries;
}

void LocatorData::onDocumentUpdated(const Document::Ptr &doc)
{
    QList<Entry> entries = FunctionFinder().run(doc);
    QMutexLocker l(&m_mutex);
    m_entries.insert(doc->fileName(), entries);
}

void LocatorData::onAboutToRemoveFiles(const Utils::FilePaths &files)
{
    QMutexLocker l(&m_mutex);
    for (const Utils::FilePath &file : files) {
        m_entries.remove(file);
    }
}

} // namespace QmlJSTools::Internal

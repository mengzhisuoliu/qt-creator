// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "LookupContext.h"

#include "Overview.h"
#include "DeprecatedGenTemplateInstance.h"
#include "CppRewriter.h"

#include <cplusplus/CoreTypes.h>
#include <cplusplus/Symbols.h>
#include <cplusplus/Literals.h>
#include <cplusplus/Names.h>
#include <cplusplus/Scope.h>
#include <cplusplus/Control.h>

#include <utils/algorithm.h>

#include <QStack>
#include <QHash>
#include <QVarLengthArray>
#include <QDebug>

using namespace Utils;

namespace CPlusPlus {

static const bool debug = qEnvironmentVariableIsSet("QTC_LOOKUPCONTEXT_DEBUG");

static void addNames(const Name *name, QList<const Name *> *names, bool addAllNames = false)
{
    if (! name)
        return;
    if (const QualifiedNameId *q = name->asQualifiedNameId()) {
        addNames(q->base(), names);
        addNames(q->name(), names, addAllNames);
    } else if (addAllNames || name->asNameId() || name->asTemplateNameId() || name->asAnonymousNameId()) {
        names->append(name);
    }
}

static void path_helper(Symbol *symbol,
                        QList<const Name *> *names,
                        LookupContext::InlineNamespacePolicy policy)
{
    if (! symbol)
        return;

    path_helper(symbol->enclosingScope(), names, policy);

    if (symbol->name()) {
        if (symbol->asClass() || symbol->asNamespace()) {
            if (policy == LookupContext::HideInlineNamespaces) {
                auto ns = symbol->asNamespace();
                if (ns && ns->isInline())
                    return;
            }
            addNames(symbol->name(), names);

        } else if (symbol->asObjCClass() || symbol->asObjCBaseClass() || symbol->asObjCProtocol()
                || symbol->asObjCForwardClassDeclaration() || symbol->asObjCForwardProtocolDeclaration()
                || symbol->asForwardClassDeclaration()) {
            addNames(symbol->name(), names);

        } else if (symbol->asFunction()) {
            if (const QualifiedNameId *q = symbol->name()->asQualifiedNameId())
                addNames(q->base(), names);
        } else if (Enum *e = symbol->asEnum()) {
            if (e->isScoped())
                addNames(symbol->name(), names);
        }
    }
}

static bool isNestedInstantiationEnclosingTemplate(
        ClassOrNamespace *nestedClassOrNamespaceInstantiation,
        ClassOrNamespace *enclosingTemplateClassInstantiation)
{
    QSet<ClassOrNamespace *> processed;
    while (enclosingTemplateClassInstantiation
           && Utils::insert(processed, enclosingTemplateClassInstantiation)) {
        if (enclosingTemplateClassInstantiation == nestedClassOrNamespaceInstantiation)
            return false;
        enclosingTemplateClassInstantiation = enclosingTemplateClassInstantiation->parent();
    }

    return true;
}

static inline bool compareName(const Name *name, const Name *other)
{
    if (name == other)
        return true;

    if (name && other) {
        const Identifier *id = name->identifier();
        const Identifier *otherId = other->identifier();

        if (id == otherId || (id && id->match(otherId)))
            return true;
    }

    return false;
}

bool compareFullyQualifiedName(const QList<const Name *> &path, const QList<const Name *> &other)
{
    if (path.length() != other.length())
        return false;

    for (int i = 0; i < path.length(); ++i) {
        if (! compareName(path.at(i), other.at(i)))
            return false;
    }

    return true;
}

namespace Internal {

bool operator==(const FullyQualifiedName &left, const FullyQualifiedName &right)
{
    return compareFullyQualifiedName(left.fqn, right.fqn);
}

size_t qHash(const FullyQualifiedName &fullyQualifiedName)
{
    size_t h = 0;
    for (int i = 0; i < fullyQualifiedName.fqn.size(); ++i) {
        if (const Name *n = fullyQualifiedName.fqn.at(i)) {
            if (const Identifier *id = n->identifier()) {
                h <<= 1;
                h += id->hashCode();
            }
        }
    }
    return h;
}

}

/////////////////////////////////////////////////////////////////////
// LookupContext
/////////////////////////////////////////////////////////////////////
LookupContext::LookupContext()
    : m_expandTemplates(false)
{ }

LookupContext::LookupContext(Document::Ptr thisDocument, const Snapshot &snapshot)
    : _expressionDocument(Document::create(FilePath::fromPathPart(u"<LookupContext>")))
    , _thisDocument(thisDocument)
    , _snapshot(snapshot)
    , _bindings(new CreateBindings(thisDocument, snapshot))
    , m_expandTemplates(false)
{}

LookupContext::LookupContext(Document::Ptr expressionDocument,
                             Document::Ptr thisDocument,
                             const Snapshot &snapshot,
                             std::shared_ptr<CreateBindings> bindings)
    : _expressionDocument(expressionDocument)
    , _thisDocument(thisDocument)
    , _snapshot(snapshot)
    , _bindings(bindings)
    , m_expandTemplates(false)
{}

LookupContext::LookupContext(const LookupContext &other)
    : _expressionDocument(other._expressionDocument)
    , _thisDocument(other._thisDocument)
    , _snapshot(other._snapshot)
    , _bindings(other._bindings)
    , m_expandTemplates(other.m_expandTemplates)
{}

LookupContext &LookupContext::operator=(const LookupContext &other)
{
    _expressionDocument = other._expressionDocument;
    _thisDocument = other._thisDocument;
    _snapshot = other._snapshot;
    _bindings = other._bindings;
    m_expandTemplates = other.m_expandTemplates;
    return *this;
}

QList<const Name *> LookupContext::fullyQualifiedName(Symbol *symbol, InlineNamespacePolicy policy)
{
    if (symbol->asTypenameArgument() || symbol->asTemplateTypeArgument())
        return {symbol->name()};
    QList<const Name *> qualifiedName = path(symbol->enclosingScope(), policy);
    addNames(symbol->name(), &qualifiedName, /*add all names*/ true);
    return qualifiedName;
}

QList<const Name *> LookupContext::path(Symbol *symbol, InlineNamespacePolicy policy)
{
    QList<const Name *> names;
    path_helper(symbol, &names, policy);
    return names;
}

static bool symbolIdentical(Symbol *s1, Symbol *s2)
{
    if (!s1 || !s2)
        return false;
    if (s1->line() != s2->line())
        return false;
    if (s1->column() != s2->column())
        return false;

    return QByteArray(s1->fileName()) == QByteArray(s2->fileName());
}

static bool isInlineNamespace(ClassOrNamespace *con, const Name *name)
{
    const QList<LookupItem> items = con->find(name);
    if (!items.isEmpty()) {
        if (const Symbol *declaration = items.first().declaration() ) {
            if (const Namespace *ns = declaration->asNamespace())
                return ns->isInline();
        }
    }

    return false;
}

const Name *LookupContext::minimalName(Symbol *symbol, ClassOrNamespace *target, Control *control)
{
    const Name *n = nullptr;
    std::optional<QList<const Name *>> minimal;
    QList<const Name *> names = LookupContext::fullyQualifiedName(symbol);

    const auto getNameFromItems = [symbol, target, control](const QList<LookupItem> &items,
            const QList<const Name *> &names) -> std::optional<QList<const Name *>> {
        for (const LookupItem &item : items) {
            if (!symbol->asUsingDeclaration() && !symbolIdentical(item.declaration(), symbol))
                continue;

            // eliminate inline namespaces
            QList<const Name *> minimal = names;
            for (int i = minimal.size() - 2; i >= 0; --i) {
                const Name *candidate = control->toName(minimal.mid(0, i + 1));
                if (isInlineNamespace(target, candidate))
                    minimal.removeAt(i);
            }

            return minimal;
        }
        return {};
    };

    do {
        ClassOrNamespace *current = target;
        n = nullptr;

        for (int i = names.size() - 1; i >= 0; --i) {
            if (! n)
                n = names.at(i);
            else
                n = control->qualifiedNameId(names.at(i), n);

            if (target) {
                const auto candidate = getNameFromItems(target->lookup(n), names.mid(i));
                if (candidate && (!minimal || minimal->size() > candidate->size())) {
                    minimal = candidate;
                    if (minimal && minimal->size() == 1)
                        return control->toName(*minimal);
                }
            }

            if (current) {
                const ClassOrNamespace * const nested = current->getNested(names.last());
                if (nested) {
                    const QList<const Name *> nameList
                            = names.mid(0, names.size() - i - 1) << names.last();
                    const QList<ClassOrNamespace *> usings = nested->usings();
                    for (ClassOrNamespace * const u : usings) {
                        const auto candidate = getNameFromItems(u->lookup(symbol->name()), nameList);
                        if (candidate && (!minimal || minimal->size() > candidate->size())) {
                            minimal = candidate;
                            if (minimal && minimal->size() == 1)
                                return control->toName(*minimal);
                        }
                    }
                }
                current = current->getNested(names.at(names.size() - i - 1));
            }
        }
        if (target)
            target = target->parent();
    } while (target);

    if (minimal)
        return control->toName(*minimal);
    return n;
}

QList<LookupItem> LookupContext::lookupByUsing(const Name *name,
                                               ClassOrNamespace *bindingScope) const
{
    QList<LookupItem> candidates;
    // if it is a nameId there can be a using declaration for it
    if (name->asNameId() || name->asTemplateNameId()) {
        const QList<Symbol *> symbols = bindingScope->symbols();
        for (Symbol *s : symbols) {
            if (Scope *scope = s->asScope()) {
                for (int i = 0, count = scope->memberCount(); i < count; ++i) {
                    if (UsingDeclaration *u = scope->memberAt(i)->asUsingDeclaration()) {
                        if (const Name *usingDeclarationName = u->name()) {
                            if (const QualifiedNameId *q
                                    = usingDeclarationName->asQualifiedNameId()) {
                                if (q->name() && q->identifier() && name->identifier()
                                        && q->name()->identifier()->match(name->identifier())) {
                                    candidates = bindings()->globalNamespace()->find(q);

                                    // if it is not a global scope(scope of scope is not equal 0)
                                    // then add current using declaration as a candidate
                                    if (scope->enclosingScope()) {
                                        LookupItem item;
                                        item.setDeclaration(u);
                                        item.setScope(scope);
                                        candidates.append(item);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (const QualifiedNameId *q = name->asQualifiedNameId()) {
        const QList<Symbol *> symbols = bindingScope->symbols();
        for (Symbol *s : symbols) {
            if (Scope *scope = s->asScope()) {
                ClassOrNamespace *base = lookupType(q->base(), scope);
                if (base)
                    candidates = lookupByUsing(q->name(), base);
                if (!candidates.isEmpty())
                    return candidates;
            }
        }
    }
    return candidates;
}


Document::Ptr LookupContext::expressionDocument() const
{ return _expressionDocument; }

Document::Ptr LookupContext::thisDocument() const
{ return _thisDocument; }

Document::Ptr LookupContext::document(const FilePath &filePath) const
{ return _snapshot.document(filePath); }

Snapshot LookupContext::snapshot() const
{ return _snapshot; }

ClassOrNamespace *LookupContext::globalNamespace() const
{
    return bindings()->globalNamespace();
}

ClassOrNamespace *LookupContext::lookupType(const Name *name, Scope *scope,
                                            ClassOrNamespace *enclosingBinding,
                                            QSet<const Declaration *> typedefsBeingResolved) const
{
    if (! scope || ! name) {
        return nullptr;
    } else if (Block *block = scope->asBlock()) {
        for (int i = 0; i < block->memberCount(); ++i) {
            Symbol *m = block->memberAt(i);
            if (UsingNamespaceDirective *u = m->asUsingNamespaceDirective()) {
                if (ClassOrNamespace *uu = lookupType(u->name(), scope->enclosingNamespace())) {
                    if (ClassOrNamespace *r = uu->lookupType(name))
                        return r;
                }
            } else if (Declaration *d = m->asDeclaration()) {
                if (d->name() && d->name()->match(name->asNameId())) {
                    if (d->isTypedef() && d->type()) {
                        if (Q_UNLIKELY(debug)) {
                            Overview oo;
                            qDebug() << "Looks like" << oo.prettyName(name)
                                     << "is a typedef for" << oo.prettyType(d->type());
                        }
                        if (const NamedType *namedTy = d->type()->asNamedType()) {
                            // Stop on recursive typedef declarations
                            if (!Utils::insert(typedefsBeingResolved, d))
                                return nullptr;
                            return lookupType(namedTy->name(), scope, nullptr,
                                              typedefsBeingResolved);
                        }
                    }
                }
            } else if (UsingDeclaration *ud = m->asUsingDeclaration()) {
                if (name->asNameId()) {
                    if (const Name *usingDeclarationName = ud->name()) {
                        if (const QualifiedNameId *q = usingDeclarationName->asQualifiedNameId()) {
                            if (q->name() && q->name()->match(name))
                                return bindings()->globalNamespace()->lookupType(q);
                        }
                    }
                }
            }
        }
        // try to find it in block (rare case but has priority before enclosing scope)
        // e.g.: void foo() { struct S {};  S s; }
        if (ClassOrNamespace *b = bindings()->lookupType(scope, enclosingBinding)) {
            if (ClassOrNamespace *classOrNamespaceNestedInNestedBlock = b->lookupType(name, block))
                return classOrNamespaceNestedInNestedBlock;
        }

        // try to find type in enclosing scope(typical case)
        if (ClassOrNamespace *found = lookupType(name, scope->enclosingScope()))
            return found;

    } else if (ClassOrNamespace *b = bindings()->lookupType(scope, enclosingBinding)) {
        return b->lookupType(name);
    } else if (Class *scopeAsClass = scope->asClass()) {
        if (scopeAsClass->enclosingScope()->asBlock()) {
            if (ClassOrNamespace *b = lookupType(scopeAsClass->name(),
                                                 scopeAsClass->enclosingScope(),
                                                 enclosingBinding,
                                                 typedefsBeingResolved)) {
                return b->lookupType(name);
            }
        }
    }

    return nullptr;
}

ClassOrNamespace *LookupContext::lookupType(Symbol *symbol,
                                            ClassOrNamespace *enclosingBinding) const
{
    return bindings()->lookupType(symbol, enclosingBinding);
}

QList<LookupItem> LookupContext::lookup(const Name *name, Scope *scope) const
{
    QList<LookupItem> candidates;

    if (!name)
        return candidates;

    for (; scope; scope = scope->enclosingScope()) {
        if (name->identifier() != nullptr && scope->asBlock()) {
            bindings()->lookupInScope(name, scope, &candidates, /*templateId = */ nullptr, /*binding=*/ nullptr);

            if (! candidates.isEmpty()) {
                // it's a local.
                //for qualified it can be outside of the local scope
                if (name->asQualifiedNameId())
                    continue;
                else
                    break;
            }

            for (int i = 0; i < scope->memberCount(); ++i) {
                if (UsingNamespaceDirective *u = scope->memberAt(i)->asUsingNamespaceDirective()) {
                    if (ClassOrNamespace *uu = lookupType(u->name(), scope->enclosingNamespace())) {
                        candidates = uu->find(name);

                        if (! candidates.isEmpty())
                            return candidates;
                    }
                }
            }

            if (ClassOrNamespace *bindingScope = bindings()->lookupType(scope)) {
                if (ClassOrNamespace *bindingBlock = bindingScope->findBlock(scope->asBlock())) {
                    candidates = lookupByUsing(name, bindingBlock);
                    if (! candidates.isEmpty())
                        return candidates;

                    candidates = bindingBlock->find(name);

                    if (! candidates.isEmpty())
                        return candidates;
                }
            }

        } else if (Function *fun = scope->asFunction()) {
            bindings()->lookupInScope(name, fun, &candidates, /*templateId = */ nullptr, /*binding=*/ nullptr);

            if (! candidates.isEmpty()) {
                // it's an argument or a template parameter.
                //for qualified it can be outside of the local scope
                if (name->asQualifiedNameId())
                    continue;
                else
                    break;
            }

            if (fun->name() && fun->name()->asQualifiedNameId()) {
                if (ClassOrNamespace *binding = bindings()->lookupType(fun)) {
                    candidates = binding->find(name);

                    // try find this name in parent class
                    QSet<ClassOrNamespace *> processed;
                    while (candidates.isEmpty() && (binding = binding->parent())) {
                        if (!Utils::insert(processed, binding))
                            break;
                        candidates = binding->find(name);
                    }

                    if (! candidates.isEmpty())
                        return candidates;
                }
            }

            // continue, and look at the enclosing scope.

        } else if (ObjCMethod *method = scope->asObjCMethod()) {
            bindings()->lookupInScope(name, method, &candidates, /*templateId = */ nullptr, /*binding=*/ nullptr);

            if (! candidates.isEmpty())
                break; // it's a formal argument.

        } else if (Template *templ = scope->asTemplate()) {
            bindings()->lookupInScope(name, templ, &candidates, /*templateId = */ nullptr, /*binding=*/ nullptr);

            if (! candidates.isEmpty()) {
                // it's a template parameter.
                //for qualified it can be outside of the local scope
                if (name->asQualifiedNameId())
                    continue;
                else
                    break;
            }

        } else if (scope->asNamespace()
                   || scope->asClass()
                   || (scope->asEnum() && scope->asEnum()->isScoped())) {

            if (ClassOrNamespace *bindingScope = bindings()->lookupType(scope)) {
                candidates = bindingScope->find(name);

                if (! candidates.isEmpty())
                    return candidates;

                candidates = lookupByUsing(name, bindingScope);
                if (!candidates.isEmpty())
                    return candidates;
            }

            // the scope can be defined inside a block, try to find it
            if (Block *block = scope->enclosingBlock()) {
                if (ClassOrNamespace *b = bindings()->lookupType(block)) {
                    if (ClassOrNamespace *classOrNamespaceNestedInNestedBlock = b->lookupType(scope->name(), block))
                        candidates = classOrNamespaceNestedInNestedBlock->find(name);
                }
            }

            if (! candidates.isEmpty())
                return candidates;

        } else if (scope->asObjCClass() || scope->asObjCProtocol()) {
            if (ClassOrNamespace *binding = bindings()->lookupType(scope))
                candidates = binding->find(name);

            if (! candidates.isEmpty())
                return candidates;
        }
    }

    return candidates;
}

ClassOrNamespace *LookupContext::lookupParent(Symbol *symbol) const
{
    const QList<const Name *> fqName = path(symbol);
    ClassOrNamespace *binding = globalNamespace();
    for (const Name *name : fqName) {
        binding = binding->findType(name);
        if (!binding)
            return nullptr;
    }

    return binding;
}

ClassOrNamespace::ClassOrNamespace(CreateBindings *factory, ClassOrNamespace *parent)
    : _factory(factory)
    , _parent(parent)
    , _scopeLookupCache(nullptr)
    , _templateId(nullptr)
    , _instantiationOrigin(nullptr)
    , _rootClass(nullptr)
    , _name(nullptr)
{
    Q_ASSERT(factory);
}

ClassOrNamespace::~ClassOrNamespace()
{
    delete _scopeLookupCache;
}

const TemplateNameId *ClassOrNamespace::templateId() const
{
    return _templateId;
}

ClassOrNamespace *ClassOrNamespace::instantiationOrigin() const
{
    return _instantiationOrigin;
}

ClassOrNamespace *ClassOrNamespace::parent() const
{
    return _parent;
}

const QList<ClassOrNamespace *> ClassOrNamespace::usings() const
{
    const_cast<ClassOrNamespace *>(this)->flush();
    return _usings;
}

QList<Enum *> ClassOrNamespace::unscopedEnums() const
{
    const_cast<ClassOrNamespace *>(this)->flush();
    return _enums;
}

const QList<Symbol *> ClassOrNamespace::symbols() const
{
    const_cast<ClassOrNamespace *>(this)->flush();
    return _symbols;
}

ClassOrNamespace *ClassOrNamespace::globalNamespace() const
{
    ClassOrNamespace *e = const_cast<ClassOrNamespace *>(this);

    do {
        if (! e->_parent)
            break;

        e = e->_parent;
    } while (e);

    return e;
}

QList<LookupItem> ClassOrNamespace::find(const Name *name)
{
    return lookup_helper(name, false);
}

QList<LookupItem> ClassOrNamespace::lookup(const Name *name)
{
    return lookup_helper(name, true);
}

QList<LookupItem> ClassOrNamespace::lookup_helper(const Name *name, bool searchInEnclosingScope)
{
    QList<LookupItem> result;

    if (name) {

        if (const QualifiedNameId *q = name->asQualifiedNameId()) {
            if (! q->base()) { // e.g. ::std::string
                result = globalNamespace()->find(q->name());
            } else if (ClassOrNamespace *binding = lookupType(q->base())) {
                result = binding->find(q->name());

                QList<const Name *> fullName;
                addNames(name, &fullName);

                // It's also possible that there are matches in the parent binding through
                // a qualified name. For instance, a nested class which is forward declared
                // in the class but defined outside it - we should capture both.
                Symbol *match = nullptr;
                QSet<ClassOrNamespace *> processed;
                for (ClassOrNamespace *parentBinding = binding->parent();
                        parentBinding && !match;
                        parentBinding = parentBinding->parent()) {
                    if (!Utils::insert(processed, parentBinding))
                        break;
                    match = parentBinding->lookupInScope(fullName);
                }

                if (match) {
                    LookupItem item;
                    item.setDeclaration(match);
                    item.setBinding(binding);
                    result.append(item);
                }
            }

            return result;
        }

        QSet<ClassOrNamespace *> processed;
        QSet<ClassOrNamespace *> processedOwnParents;
        ClassOrNamespace *binding = this;
        do {
            if (!Utils::insert(processedOwnParents, binding))
                break;
            lookup_helper(name, binding, &result, &processed, /*templateId = */ nullptr);
            binding = binding->_parent;
        } while (searchInEnclosingScope && binding);
    }

    return result;
}

void ClassOrNamespace::lookup_helper(const Name *name, ClassOrNamespace *binding,
                                          QList<LookupItem> *result,
                                          QSet<ClassOrNamespace *> *processed,
                                          const TemplateNameId *templateId)
{
    if (binding && Utils::insert(*processed, binding)) {
        const Identifier *nameId = name->identifier();

        const QList<Symbol *> symbols = binding->symbols();
        for (Symbol *s : symbols) {
            if (s->isFriend())
                continue;
            else if (s->asUsingNamespaceDirective())
                continue;


            if (Scope *scope = s->asScope()) {
                if (Class *klass = scope->asClass()) {
                    if (const Identifier *id = klass->identifier()) {
                        if (nameId && nameId->match(id)) {
                            LookupItem item;
                            item.setDeclaration(klass);
                            item.setBinding(binding);
                            result->append(item);
                        }
                    }
                }
                _factory->lookupInScope(name, scope, result, templateId, binding);
            }
        }

        const QList<Enum *> enums = binding->unscopedEnums();
        for (Enum *e : enums)
            _factory->lookupInScope(name, e, result, templateId, binding);

        const QList<ClassOrNamespace *> usings = binding->usings();
        for (ClassOrNamespace *u : usings)
            lookup_helper(name, u, result, processed, binding->_templateId);

        Anonymouses::const_iterator cit = binding->_anonymouses.constBegin();
        Anonymouses::const_iterator citEnd = binding->_anonymouses.constEnd();
        for (; cit != citEnd; ++cit) {
            const AnonymousNameId *anonymousNameId = cit.key();
            ClassOrNamespace *a = cit.value();
            if (!binding->_declaredOrTypedefedAnonymouses.contains(anonymousNameId))
                lookup_helper(name, a, result, processed, binding->_templateId);
        }
    }
}

void CreateBindings::lookupInScope(const Name *name, Scope *scope,
                                   QList<LookupItem> *result,
                                   const TemplateNameId *templateId,
                                   ClassOrNamespace *binding)
{
    if (!name)
        return;

    if (const OperatorNameId *op = name->asOperatorNameId()) {
        for (Symbol *s = scope->find(op->kind()); s; s = s->next()) {
            if (! s->name())
                continue;
            else if (s->isFriend())
                continue;
            else if (! s->name()->match(op))
                continue;

            LookupItem item;
            item.setDeclaration(s);
            item.setBinding(binding);

            if (Symbol *inst = instantiateTemplateFunction(name, s->asTemplate()))
                item.setType(inst->type());

            result->append(item);
        }
        return;
    }

    if (const ConversionNameId * const conv = name->asConversionNameId()) {
        if (Symbol * const s = scope->find(conv)) {
            LookupItem item;
            item.setDeclaration(s);
            item.setBinding(binding);

            if (Symbol *inst = instantiateTemplateFunction(name, s->asTemplate()))
                item.setType(inst->type());

            result->append(item);
        }
        return;
    }

    if (const Identifier *id = name->identifier()) {
        for (Symbol *s = scope->find(id); s; s = s->next()) {
            if (s->isFriend())
                continue; // skip friends
            else if (s->asUsingNamespaceDirective())
                continue; // skip using namespace directives
            else if (! id->match(s->identifier()))
                continue;
            else if (s->name() && s->name()->asQualifiedNameId())
                continue; // skip qualified ids.

            if (Q_UNLIKELY(debug)) {
                Overview oo;
                qDebug() << "Found" << id->chars() << "in"
                         << (binding ? oo.prettyName(binding->_name) : QString::fromLatin1("<null>"));
            }

            LookupItem item;
            item.setDeclaration(s);
            item.setBinding(binding);

            if (s->asNamespaceAlias() && binding) {
                ClassOrNamespace *targetNamespaceBinding = binding->lookupType(name);
                //there can be many namespace definitions
                if (targetNamespaceBinding && targetNamespaceBinding->symbols().size() > 0) {
                    Symbol *resolvedSymbol = targetNamespaceBinding->symbols().constFirst();
                    item.setType(resolvedSymbol->type()); // override the type
                }
            }

            if (templateId && (s->asDeclaration() || s->asFunction())) {
                FullySpecifiedType ty = DeprecatedGenTemplateInstance::instantiate(templateId, s, control());
                item.setType(ty); // override the type.
            }

            if (Symbol *inst = instantiateTemplateFunction(name, s->asTemplate()))
                item.setType(inst->type());

            if (Template *templ = s->asTemplate())
                if (templ->declaration() && templ->declaration()->asClass())
                    item.setType(control()->namedType(name));

            result->append(item);
        }
    }
}

ClassOrNamespace *ClassOrNamespace::lookupType(const Name *name)
{
    if (! name)
        return nullptr;

    QSet<ClassOrNamespace *> processed;
    return lookupType_helper(name, &processed, /*searchInEnclosingScope =*/ true, this);
}

ClassOrNamespace *ClassOrNamespace::lookupType(const Name *name, Block *block)
{
    flush();

    QHash<Block *, ClassOrNamespace *>::const_iterator citBlock = _blocks.constFind(block);
    if (citBlock != _blocks.constEnd()) {
        ClassOrNamespace *nestedBlock = citBlock.value();
        QSet<ClassOrNamespace *> processed;
        if (ClassOrNamespace *foundInNestedBlock
                = nestedBlock->lookupType_helper(name,
                                                 &processed,
                                                 /*searchInEnclosingScope = */ true,
                                                 this)) {
            return foundInNestedBlock;
        }
    }

    for (citBlock = _blocks.constBegin(); citBlock != _blocks.constEnd(); ++citBlock) {
        if (ClassOrNamespace *foundNestedBlock = citBlock.value()->lookupType(name, block))
            return foundNestedBlock;
    }

    return nullptr;
}

ClassOrNamespace *ClassOrNamespace::findType(const Name *name)
{
    QSet<ClassOrNamespace *> processed;
    return lookupType_helper(name, &processed, /*searchInEnclosingScope =*/ false, this);
}

ClassOrNamespace *ClassOrNamespace::findBlock_helper(Block *block,
                                                     QSet<ClassOrNamespace *> *processed,
                                                     bool searchInEnclosingScope)
{
    for (ClassOrNamespace *binding = this; binding; binding = binding->_parent) {
        if (!Utils::insert(*processed, binding))
            break;
        binding->flush();
        auto end = binding->_blocks.end();
        auto citBlock = binding->_blocks.find(block);
        if (citBlock != end)
            return citBlock.value();

        for (citBlock = binding->_blocks.begin(); citBlock != end; ++citBlock) {
            if (ClassOrNamespace *foundNestedBlock =
                    citBlock.value()->findBlock_helper(block, processed, false)) {
                return foundNestedBlock;
            }
        }
        if (!searchInEnclosingScope)
            break;
    }
    return nullptr;
}

ClassOrNamespace *ClassOrNamespace::findBlock(Block *block)
{
    QSet<ClassOrNamespace *> processed;
    return findBlock_helper(block, &processed, true);
}

ClassOrNamespace *ClassOrNamespace::getNested(const Name *name)
{
    flush();
    const auto it = _classOrNamespaces.find(name);
    if (it != _classOrNamespaces.cend())
        return it->second;
    return nullptr;
}

Symbol *ClassOrNamespace::lookupInScope(const QList<const Name *> &fullName)
{
    if (!_scopeLookupCache) {
        _scopeLookupCache = new QHash<Internal::FullyQualifiedName, Symbol *>;

        for (int j = 0; j < symbols().size(); ++j) {
            if (Scope *scope = symbols().at(j)->asScope()) {
                for (int i = 0; i < scope->memberCount(); ++i) {
                    Symbol *s = scope->memberAt(i);
                    _scopeLookupCache->insert(LookupContext::fullyQualifiedName(s), s);
                }
            }
        }
    }

    return _scopeLookupCache->value(fullName, 0);
}

ClassOrNamespace *ClassOrNamespace::lookupType_helper(const Name *name,
                                                      QSet<ClassOrNamespace *> *processed,
                                                      bool searchInEnclosingScope,
                                                      ClassOrNamespace *origin)
{
    if (Q_UNLIKELY(debug)) {
        Overview oo;
        qDebug() << "Looking up" << oo.prettyName(name) << "in" << oo.prettyName(_name);
    }

    if (const QualifiedNameId *q = name->asQualifiedNameId()) {

        QSet<ClassOrNamespace *> innerProcessed;
        if (! q->base())
            return globalNamespace()->lookupType_helper(q->name(), &innerProcessed, true, origin);

        if (ClassOrNamespace *binding = lookupType_helper(q->base(), processed, true, origin))
            return binding->lookupType_helper(q->name(), &innerProcessed, false, origin);

        return nullptr;

    } else if (Utils::insert(*processed, this)) {
        if (name->asNameId() || name->asTemplateNameId() || name->asAnonymousNameId()) {
            flush();

            const QList<Symbol *> symbolList = symbols();
            for (Symbol *s : symbolList) {
                if (Class *klass = s->asClass()) {
                    if (klass->identifier() && klass->identifier()->match(name->identifier()))
                        return this;
                }
            }
            const QList<Enum *> enums = unscopedEnums();
            for (Enum *e : enums) {
                if (e->identifier() && e->identifier()->match(name->identifier()))
                    return this;
            }

            if (ClassOrNamespace *e = nestedType(name, processed, origin))
                return e;

            if (_templateId) {
                if (_usings.size() == 1) {
                    ClassOrNamespace *delegate = _usings.first();

                    if (ClassOrNamespace *r = delegate->lookupType_helper(name,
                                                                          processed,
                                                                          /*searchInEnclosingScope = */ true,
                                                                          origin))
                        return r;
                } else if (Q_UNLIKELY(debug)) {
                    qWarning() << "expected one using declaration. Number of using declarations is:"
                               << _usings.size();
                }
            }

            const QList<ClassOrNamespace *> usingList = usings();
            for (ClassOrNamespace *u : usingList) {
                if (ClassOrNamespace *r = u->lookupType_helper(name,
                                                               processed,
                                                               /*searchInEnclosingScope =*/ false,
                                                               origin))
                    return r;
            }
        }

        if (_parent && searchInEnclosingScope)
            return _parent->lookupType_helper(name, processed, searchInEnclosingScope, origin);
    }

    return nullptr;
}

static ClassOrNamespace *findSpecializationWithMatchingTemplateArgument(const Name *argumentName,
                                                                        ClassOrNamespace *reference)
{
    const QList<Symbol *> symbols = reference->symbols();
    for (Symbol *s : symbols) {
        if (Class *clazz = s->asClass()) {
            if (Template *templateSpecialization = clazz->enclosingTemplate()) {
                const int argumentCountOfSpecialization
                                    = templateSpecialization->templateParameterCount();
                for (int i = 0; i < argumentCountOfSpecialization; ++i) {
                    if (Symbol *param = templateSpecialization->templateParameterAt(i);
                        param->asTypenameArgument() ||
                        param->asTemplateTypeArgument()) {
                        if (const Name *name = param->name()) {
                            if (compareName(name, argumentName))
                                return reference;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

ClassOrNamespace *ClassOrNamespace::findSpecialization(const TemplateNameId *templId,
                                                       const TemplateNameIdTable &specializations)
{
    // we go through all specialization and try to find that one with template argument as pointer
    for (TemplateNameIdTable::const_iterator cit = specializations.begin();
         cit != specializations.end(); ++cit) {
        const TemplateNameId *specializationNameId = cit->first;
        const int specializationTemplateArgumentCount
                = specializationNameId->templateArgumentCount();
        const int initializationTemplateArgumentCount
                = templId->templateArgumentCount();
        // for now it works only when we have the same number of arguments in specialization
        // and initialization(in future it should be more clever)
        if (specializationTemplateArgumentCount == initializationTemplateArgumentCount) {
            for (int i = 0; i < initializationTemplateArgumentCount; ++i) {
                const TemplateArgument &specializationTemplateArgument
                        = specializationNameId->templateArgumentAt(i);
                const TemplateArgument &initializationTemplateArgument
                        = templId->templateArgumentAt(i);
                PointerType *specPointer
                        = specializationTemplateArgument.type().type()->asPointerType();
                // specialization and initialization argument have to be a pointer
                // additionally type of pointer argument of specialization has to be namedType
                if (specPointer && initializationTemplateArgument.type().type()->asPointerType()
                        && specPointer->elementType().type()->asNamedType()) {
                    return cit->second;
                }

                ArrayType *specArray
                        = specializationTemplateArgument.type().type()->asArrayType();
                if (specArray && initializationTemplateArgument.type().type()->asArrayType()) {
                    if (const NamedType *argumentNamedType
                            = specArray->elementType().type()->asNamedType()) {
                        if (const Name *argumentName = argumentNamedType->name()) {
                            if (ClassOrNamespace *reference
                                    = findSpecializationWithMatchingTemplateArgument(
                                            argumentName, cit->second)) {
                                return reference;
                            }
                        }
                    }
                }
            }
        }
    }

    return nullptr;
}

ClassOrNamespace *ClassOrNamespace::findOrCreateNestedAnonymousType(
        const AnonymousNameId *anonymousNameId)
{
    QHash<const AnonymousNameId *, ClassOrNamespace *>::const_iterator cit
            = _anonymouses.constFind(anonymousNameId);
    if (cit != _anonymouses.constEnd()) {
        return cit.value();
    } else {
        ClassOrNamespace *newAnonymous = _factory->allocClassOrNamespace(this);
        if (Q_UNLIKELY(debug))
            newAnonymous->_name = anonymousNameId;
        _anonymouses[anonymousNameId] = newAnonymous;
        return newAnonymous;
    }
}

ClassOrNamespace *ClassOrNamespace::nestedType(const Name *name,
                                               QSet<ClassOrNamespace *> *processed,
                                               ClassOrNamespace *origin)
{
    Q_ASSERT(name != nullptr);
    Q_ASSERT(name->asNameId() || name->asTemplateNameId() || name->asAnonymousNameId());

    const_cast<ClassOrNamespace *>(this)->flush();

    if (const AnonymousNameId *anonymousNameId = name->asAnonymousNameId())
        return findOrCreateNestedAnonymousType(anonymousNameId);

    Table::const_iterator it = _classOrNamespaces.find(name);
    if (it == _classOrNamespaces.end())
        return nullptr;

    ClassOrNamespace *reference = it->second;
    ClassOrNamespace *baseTemplateClassReference = reference;

    const TemplateNameId *templId = name->asTemplateNameId();
    if (templId) {
        // for "using" we should use the real one ClassOrNamespace(it should be the first
        // one item from usings list)
        // we indicate that it is a 'using' by checking number of symbols(it should be 0)
        if (reference->symbols().isEmpty() && !reference->usings().isEmpty())
            reference = reference->_usings[0];

        // if it is a TemplateNameId it could be a specialization(full or partial) or
        // instantiation of one of the specialization(reference->_specialization) or
        // base class(reference)
        if (templId->isSpecialization()) {
            // if it is a specialization we try to find or create new one and
            // add to base class(reference)
            TemplateNameIdTable::const_iterator cit
                    = reference->_specializations.find(templId);
            if (cit != reference->_specializations.end()) {
                return cit->second;
            } else {
                ClassOrNamespace *newSpecialization = _factory->allocClassOrNamespace(reference);
                if (Q_UNLIKELY(debug))
                    newSpecialization->_name = templId;
                reference->_specializations[templId] = newSpecialization;
                return newSpecialization;
            }
        } else {
            const auto citInstantiation = reference->_instantiations.constFind(templId);
            if (citInstantiation != reference->_instantiations.constEnd())
                return citInstantiation.value();

            const TemplateNameIdTable &specializations = reference->_specializations;
            const TemplateNameId templIdAsSpecialization(templId->identifier(),
                                                         /*isSpecializaton=*/ true,
                                                         templId->firstTemplateArgument(),
                                                         templId->lastTemplateArgument());
            TemplateNameIdTable::const_iterator cit
                    = specializations.find(&templIdAsSpecialization);

            if (cit != specializations.end()) {
                // we found full specialization
                reference = cit->second;
            } else {
                ClassOrNamespace *specializationWithPointer
                        = findSpecialization(templId, specializations);
                if (specializationWithPointer)
                    reference = specializationWithPointer;

                int maximumArgumentsMatched = 0;

                for (const auto &p : specializations) {
                    const TemplateNameId *templateSpecialization = p.first;
                    ClassOrNamespace *specializationClassOrNamespace = p.second;

                    const int argumentCountOfInitialization = templId->templateArgumentCount();
                    const int argumentCountOfSpecialization =
                            templateSpecialization->templateArgumentCount();

                    int argumentsMatched = 0;
                    for (int i = 0;
                         i < argumentCountOfInitialization && i < argumentCountOfSpecialization;
                         ++i) {
                        if (templId->templateArgumentAt(i) ==
                                templateSpecialization->templateArgumentAt(i)) {
                            argumentsMatched++;
                        }
                    }

                    if (argumentsMatched > maximumArgumentsMatched) {
                        reference = specializationClassOrNamespace;
                        maximumArgumentsMatched = argumentsMatched;
                    }
                }
            }
        }
    }

    // The reference binding might still be missing some of its base classes in the case they
    // are templates. We need to collect them now. First, we track the bases which are already
    // part of the binding so we can identify the missings ones later.

    Class *referenceClass = nullptr;
    QList<const Name *> allBases;
    const QList<Symbol *> symbols = reference->symbols();
    for (Symbol *s : symbols) {
        if (Class *clazz = s->asClass()) {
            for (int i = 0; i < clazz->baseClassCount(); ++i) {
                BaseClass *baseClass = clazz->baseClassAt(i);
                if (baseClass->name())
                    allBases.append(baseClass->name());
            }
            referenceClass = clazz;
            break;
        }
    }

    if (!referenceClass)
        return reference;

    if ((! templId && _alreadyConsideredClasses.contains(referenceClass)) ||
            (templId &&
            _alreadyConsideredTemplates.contains(templId))) {
            return reference;
    }

    if (!name->asTemplateNameId())
        _alreadyConsideredClasses.insert(referenceClass);

    QSet<ClassOrNamespace *> knownUsings = Utils::toSet(reference->usings());

    // If we are dealling with a template type, more work is required, since we need to
    // construct all instantiation data.
    if (templId) {
        _alreadyConsideredTemplates.insert(templId);
        ClassOrNamespace *instantiation = _factory->allocClassOrNamespace(baseTemplateClassReference);
        if (Q_UNLIKELY(debug))
            instantiation->_name = templId;
        instantiation->_templateId = templId;

        QSet<ClassOrNamespace *> otherProcessed;
        while (!origin->_symbols.isEmpty() && origin->_symbols[0]->asBlock()) {
            if (!Utils::insert(otherProcessed, origin))
                break;
            origin = origin->parent();
        }

        instantiation->_instantiationOrigin = origin;

        // The instantiation should have all symbols, enums, and usings from the reference.
        instantiation->_enums.append(reference->unscopedEnums());
        instantiation->_usings.append(reference->usings());

        instantiation->_rootClass = reference->rootClass();

        // It gets a bit complicated if the reference is actually a class template because we
        // now must worry about dependent names in base classes.
        if (Template *templateSpecialization = referenceClass->enclosingTemplate()) {
            const int argumentCountOfInitialization = templId->templateArgumentCount();
            const int argumentCountOfSpecialization
                    = templateSpecialization->templateParameterCount();

            Subst subst(_control.get());
            if (_factory->expandTemplates()) {
                const TemplateNameId *templSpecId
                        = templateSpecialization->name()->asTemplateNameId();
                const int templSpecArgumentCount = templSpecId ?
                            templSpecId->templateArgumentCount() : 0;
                Clone cloner(_control.get());
                for (int i = 0; i < argumentCountOfSpecialization; ++i) {

                    Symbol *tParam = templateSpecialization->templateParameterAt(i);
                    if (!tParam->asTypenameArgument() &&
                        !tParam->asTemplateTypeArgument())
                        continue;
                    const Name *name = tParam->name();
                    if (!name)
                        continue;

                    int argumentPositionInReferenceClass=i;

                    if (referenceClass->name() && referenceClass->name()->asTemplateNameId()) {
                        argumentPositionInReferenceClass=-1;
                        const TemplateNameId* refTemp = referenceClass->name()->asTemplateNameId();
                        for (int argPos=0; argPos < refTemp->templateArgumentCount(); argPos++) {
                            const Type* argType = refTemp->templateArgumentAt(argPos).type().type();
                            if (argType->asNamedType()
                                    && argType->asNamedType()->name() == name) {
                                argumentPositionInReferenceClass = argPos;
                                break;
                            }
                            if (argType->asPointerType()
                                    && argType->asPointerType()->elementType().type()->asNamedType()
                                    && argType->asPointerType()->elementType().type()
                                    ->asNamedType()->name() == name) {
                                argumentPositionInReferenceClass = argPos;
                                break;
                            }
                        }

                        if (argumentPositionInReferenceClass < 0) {
                            continue;
                        }
                    }


                    FullySpecifiedType ty = (argumentPositionInReferenceClass < argumentCountOfInitialization) ?
                                templId->templateArgumentAt(argumentPositionInReferenceClass).type():
                                cloner.type(tParam->type(), &subst);

                    if (i < templSpecArgumentCount
                            && templSpecId->templateArgumentAt(i).type()->asPointerType()) {
                        if (PointerType *pointerType = ty->asPointerType())
                            ty = pointerType->elementType();
                    }

                    subst.bind(cloner.name(name, &subst), ty);
                }

                const QList<Symbol *> symbols = reference->symbols();
                for (Symbol *s : symbols) {
                    Symbol *clone = cloner.symbol(s, &subst);
                    clone->setEnclosingScope(s->enclosingScope());
                    instantiation->_symbols.append(clone);
                    if (Q_UNLIKELY(debug)) {
                        Overview oo;
                        oo.showFunctionSignatures = true;
                        oo.showReturnTypes = true;
                        oo.showTemplateParameters = true;
                        qDebug() << "cloned" << oo.prettyType(clone->type());
                        if (Class *klass = clone->asClass()) {
                            const int klassMemberCount = klass->memberCount();
                            for (int i = 0; i < klassMemberCount; ++i){
                                Symbol *klassMemberAsSymbol = klass->memberAt(i);
                                if (klassMemberAsSymbol->isTypedef()) {
                                    if (Declaration *declaration = klassMemberAsSymbol->asDeclaration())
                                        qDebug() << "Member: " << oo.prettyType(declaration->type(), declaration->name());
                                }
                            }
                        }
                    }
                }
                instantiateNestedClasses(reference, cloner, subst, instantiation);
            } else {
                instantiation->_symbols.append(reference->symbols());
            }

            QHash<const Name*, int> templParams;
            for (int i = 0; i < argumentCountOfSpecialization; ++i)
                templParams.insert(templateSpecialization->templateParameterAt(i)->name(), i);

            for (const Name *baseName : std::as_const(allBases)) {
                ClassOrNamespace *baseBinding = nullptr;

                if (const Identifier *nameId = baseName->asNameId()) {
                    // This is the simple case in which a template parameter is itself a base.
                    // Ex.: template <class T> class A : public T {};
                    if (templParams.contains(nameId)) {
                        const int parameterIndex = templParams.value(nameId);
                        if (parameterIndex < argumentCountOfInitialization) {
                            const FullySpecifiedType &fullType =
                                    templId->templateArgumentAt(parameterIndex).type();
                            if (fullType.isValid()) {
                                if (NamedType *namedType = fullType.type()->asNamedType())
                                    baseBinding = lookupType(namedType->name());
                            }
                        }
                    }
                    if (!baseBinding && subst.contains(baseName)) {
                        const FullySpecifiedType &fullType = subst[baseName];
                        if (fullType.isValid()) {
                            if (NamedType *namedType = fullType.type()->asNamedType())
                                baseBinding = lookupType(namedType->name());
                        }
                    }
                } else {
                    SubstitutionMap map;
                    for (int i = 0; i < argumentCountOfSpecialization; ++i) {
                        const Name *name = templateSpecialization->templateParameterAt(i)->name();
                        FullySpecifiedType ty = (i < argumentCountOfInitialization) ?
                                    templId->templateArgumentAt(i).type():
                                    templateSpecialization->templateParameterAt(i)->type();

                        map.bind(name, ty);
                    }
                    SubstitutionEnvironment env;
                    env.enter(&map);

                    baseName = rewriteName(baseName, &env, _control.get());

                    if (const TemplateNameId *baseTemplId = baseName->asTemplateNameId()) {
                        // Another template that uses the dependent name.
                        // Ex.: template <class T> class A : public B<T> {};
                        if (baseTemplId->identifier() != templId->identifier())
                            baseBinding = nestedType(baseName, processed, origin);
                    } else if (const QualifiedNameId *qBaseName = baseName->asQualifiedNameId()) {
                        // Qualified names in general.
                        // Ex.: template <class T> class A : public B<T>::Type {};
                        ClassOrNamespace *binding = this;
                        if (const Name *qualification = qBaseName->base()) {
                            const TemplateNameId *baseTemplName = qualification->asTemplateNameId();
                            if (!baseTemplName || !compareName(baseTemplName, templateSpecialization->name()))
                                binding = lookupType(qualification);
                        }
                        baseName = qBaseName->name();

                        if (binding)
                            baseBinding = binding->lookupType(baseName);
                    }
                }

                if (baseBinding && !knownUsings.contains(baseBinding))
                    instantiation->addUsing(baseBinding);
            }
        } else {
            instantiation->_classOrNamespaces = reference->_classOrNamespaces;
            instantiation->_symbols.append(reference->symbols());
        }

        _alreadyConsideredTemplates.clear(templId);
        baseTemplateClassReference->_instantiations[templId] = instantiation;
        return instantiation;
    }

    if (allBases.isEmpty() || allBases.size() == knownUsings.size())
        return reference;

    // Find the missing bases for regular (non-template) types.
    // Ex.: class A : public B<Some>::Type {};
    for (const Name *baseName : std::as_const(allBases)) {
        ClassOrNamespace *binding = this;
        if (const QualifiedNameId *qBaseName = baseName->asQualifiedNameId()) {
            if (const Name *qualification = qBaseName->base())
                binding = lookupType(qualification);
            else if (binding->parent() != nullptr)
                //if this is global identifier we take global namespace
                //Ex: class A{}; namespace NS { class A: public ::A{}; }
                binding = binding->globalNamespace();
            else
                //if we are in the global scope
                continue;
            baseName = qBaseName->name();
        }

        if (binding) {
            ClassOrNamespace * baseBinding
                    = binding->lookupType_helper(baseName, processed, true, this);
            if (baseBinding && !knownUsings.contains(baseBinding))
                reference->addUsing(baseBinding);
        }
    }

    _alreadyConsideredClasses.clear(referenceClass);
    return reference;
}


void ClassOrNamespace::instantiateNestedClasses(ClassOrNamespace *enclosingTemplateClass,
                                                Clone &cloner,
                                                Subst &subst,
                                                ClassOrNamespace *enclosingTemplateClassInstantiation)
{
    NestedClassInstantiator nestedClassInstantiator(_factory, cloner, subst);
    nestedClassInstantiator.instantiate(enclosingTemplateClass, enclosingTemplateClassInstantiation);
}

void ClassOrNamespace::NestedClassInstantiator::instantiate(ClassOrNamespace *enclosingTemplateClass,
                                                ClassOrNamespace *enclosingTemplateClassInstantiation)
{
    if (_alreadyConsideredNestedClassInstantiations.size() >= 3)
        return;
    if (!Utils::insert(_alreadyConsideredNestedClassInstantiations, enclosingTemplateClass))
        return;
    ClassOrNamespace::Table::const_iterator cit = enclosingTemplateClass->_classOrNamespaces.begin();
    for (; cit != enclosingTemplateClass->_classOrNamespaces.end(); ++cit) {
        const Name *nestedName = cit->first;
        ClassOrNamespace *nestedClassOrNamespace = cit->second;
        ClassOrNamespace *nestedClassOrNamespaceInstantiation = nestedClassOrNamespace;

        if (isInstantiateNestedClassNeeded(nestedClassOrNamespace->_symbols)) {
            nestedClassOrNamespaceInstantiation = _factory->allocClassOrNamespace(nestedClassOrNamespace);
            nestedClassOrNamespaceInstantiation->_enums.append(nestedClassOrNamespace->unscopedEnums());
            nestedClassOrNamespaceInstantiation->_usings.append(nestedClassOrNamespace->usings());
            nestedClassOrNamespaceInstantiation->_instantiationOrigin = nestedClassOrNamespace;

            const QList<Symbol *> symbols = nestedClassOrNamespace->_symbols;
            for (Symbol *s : symbols) {
                Symbol *clone = _cloner.symbol(s, &_subst);
                if (!clone->enclosingScope()) // Not from the cache but just cloned.
                    clone->setEnclosingScope(s->enclosingScope());
                nestedClassOrNamespaceInstantiation->_symbols.append(clone);
            }
        }

        if (isNestedInstantiationEnclosingTemplate(nestedClassOrNamespaceInstantiation,
                                                   enclosingTemplateClass)) {
            nestedClassOrNamespaceInstantiation->_parent = enclosingTemplateClassInstantiation;
        }
        instantiate(nestedClassOrNamespace, nestedClassOrNamespaceInstantiation);

        enclosingTemplateClassInstantiation->_classOrNamespaces[nestedName] =
                nestedClassOrNamespaceInstantiation;
    }
    _alreadyConsideredNestedClassInstantiations.remove(enclosingTemplateClass);
}

bool ClassOrNamespace::NestedClassInstantiator::isInstantiateNestedClassNeeded(const QList<Symbol *> &symbols) const
{
    for (Symbol *s : symbols) {
        if (Class *klass = s->asClass()) {
            int memberCount = klass->memberCount();
            for (int i = 0; i < memberCount; ++i) {
                Symbol *memberAsSymbol = klass->memberAt(i);
                if (Declaration *declaration = memberAsSymbol->asDeclaration()) {
                    if (containsTemplateType(declaration))
                        return true;
                } else if (Function *function = memberAsSymbol->asFunction()) {
                    if (containsTemplateType(function))
                        return true;
                }
            }
        }
    }

    return false;
}

bool ClassOrNamespace::NestedClassInstantiator::containsTemplateType(Declaration *declaration) const
{
    Type *memberType = declaration->type().type();
    NamedType *namedType = findNamedType(memberType);
    return namedType && _subst.contains(namedType->name());
}

bool ClassOrNamespace::NestedClassInstantiator::containsTemplateType(Function *function) const
{
    Type *returnType = function->returnType().type();
    NamedType *namedType = findNamedType(returnType);
    return namedType && _subst.contains(namedType->name());
    //TODO: in future we will need also check function arguments, for now returned value is enough
}

NamedType *ClassOrNamespace::NestedClassInstantiator::findNamedType(Type *memberType) const
{
    if (NamedType *namedType = memberType->asNamedType())
        return namedType;
    else if (PointerType *pointerType = memberType->asPointerType())
        return findNamedType(pointerType->elementType().type());
    else if (ReferenceType *referenceType = memberType->asReferenceType())
        return findNamedType(referenceType->elementType().type());

    return nullptr;
}

void ClassOrNamespace::flush()
{
    if (! _todo.isEmpty()) {
        const QList<Symbol *> todo = _todo;
        _todo.clear();

        for (Symbol *member : todo)
            _factory->process(member, this);
    }
}

void ClassOrNamespace::addSymbol(Symbol *symbol)
{
    _symbols.append(symbol);
}

void ClassOrNamespace::addTodo(Symbol *symbol)
{
    _todo.append(symbol);
}

void ClassOrNamespace::addUnscopedEnum(Enum *e)
{
    _enums.append(e);
}

void ClassOrNamespace::addUsing(ClassOrNamespace *u)
{
    _usings.append(u);
}

void ClassOrNamespace::addNestedType(const Name *alias, ClassOrNamespace *e)
{
    _classOrNamespaces[alias] = e;
}

ClassOrNamespace *ClassOrNamespace::findOrCreateType(const Name *name, ClassOrNamespace *origin,
                                                     Class *clazz)
{
    if (! name)
        return this;
    if (! origin)
        origin = this;

    if (const QualifiedNameId *q = name->asQualifiedNameId()) {
        if (! q->base())
            return globalNamespace()->findOrCreateType(q->name(), origin, clazz);

        return findOrCreateType(q->base(), origin)->findOrCreateType(q->name(), origin, clazz);

    } else if (name->asNameId() || name->asTemplateNameId() || name->asAnonymousNameId()) {
        QSet<ClassOrNamespace *> processed;
        ClassOrNamespace *e = nestedType(name, &processed, origin);

        if (! e) {
            e = _factory->allocClassOrNamespace(this);
            e->_rootClass = clazz;
            if (Q_UNLIKELY(debug))
                e->_name = name;
            _classOrNamespaces[name] = e;
        }

        return e;
    }

    return nullptr;
}

CreateBindings::CreateBindings(Document::Ptr thisDocument, const Snapshot &snapshot)
    : _snapshot(snapshot)
    , _control(std::shared_ptr<Control>(new Control))
    , _expandTemplates(false)
{
    _globalNamespace = allocClassOrNamespace(/*parent = */ nullptr);
    _currentClassOrNamespace = _globalNamespace;

    process(thisDocument);
}

CreateBindings::~CreateBindings()
{
    qDeleteAll(_entities);
}

ClassOrNamespace *CreateBindings::switchCurrentClassOrNamespace(ClassOrNamespace *classOrNamespace)
{
    ClassOrNamespace *previous = _currentClassOrNamespace;
    _currentClassOrNamespace = classOrNamespace;
    return previous;
}

ClassOrNamespace *CreateBindings::globalNamespace() const
{
    return _globalNamespace;
}

ClassOrNamespace *CreateBindings::lookupType(Symbol *symbol, ClassOrNamespace *enclosingBinding)
{
    const QList<const Name *> path = LookupContext::path(symbol);
    return lookupType(path, enclosingBinding);
}

ClassOrNamespace *CreateBindings::lookupType(const QList<const Name *> &path,
                                             ClassOrNamespace *enclosingBinding)
{
    if (path.isEmpty())
        return _globalNamespace;

    if (enclosingBinding) {
        if (ClassOrNamespace *b = enclosingBinding->lookupType(path.last()))
            return b;
    }

    ClassOrNamespace *b = _globalNamespace->lookupType(path.at(0));

    for (int i = 1; b && i < path.size(); ++i)
        b = b->findType(path.at(i));

    return b;
}

void CreateBindings::process(Symbol *s, ClassOrNamespace *classOrNamespace)
{
    ClassOrNamespace *previous = switchCurrentClassOrNamespace(classOrNamespace);
    accept(s);
    (void) switchCurrentClassOrNamespace(previous);
}

void CreateBindings::process(Symbol *symbol)
{
    _currentClassOrNamespace->addTodo(symbol);
}

ClassOrNamespace *CreateBindings::allocClassOrNamespace(ClassOrNamespace *parent)
{
    ClassOrNamespace *e = new ClassOrNamespace(this, parent);
    e->_control = control();
    _entities.append(e);
    return e;
}

void CreateBindings::process(Document::Ptr doc)
{
    if (! doc)
        return;

    if (Namespace *globalNamespace = doc->globalNamespace()) {
        if (Utils::insert(_processed, globalNamespace)) {
            const QList<Document::Include> includes = doc->resolvedIncludes();
            for (const Document::Include &i : includes) {
                if (Document::Ptr incl = _snapshot.document(i.resolvedFileName()))
                    process(incl);
            }

            accept(globalNamespace);
        }
    }
}

ClassOrNamespace *CreateBindings::enterClassOrNamespaceBinding(Symbol *symbol)
{
    ClassOrNamespace *entity = _currentClassOrNamespace->findOrCreateType(symbol->name(), nullptr,
                                                                          symbol->asClass());
    entity->addSymbol(symbol);

    return switchCurrentClassOrNamespace(entity);
}

ClassOrNamespace *CreateBindings::enterGlobalClassOrNamespace(Symbol *symbol)
{
    ClassOrNamespace *entity = _globalNamespace->findOrCreateType(symbol->name(), nullptr,
                                                                  symbol->asClass());
    entity->addSymbol(symbol);

    return switchCurrentClassOrNamespace(entity);
}

bool CreateBindings::visit(Template *templ)
{
    if (Symbol *d = templ->declaration())
        accept(d);

    return false;
}

bool CreateBindings::visit(Namespace *ns)
{
    ClassOrNamespace *previous = enterClassOrNamespaceBinding(ns);

    for (int i = 0; i < ns->memberCount(); ++i)
        process(ns->memberAt(i));

    if (ns->isInline() && previous)
        previous->addUsing(_currentClassOrNamespace);

    _currentClassOrNamespace = previous;

    return false;
}

bool CreateBindings::visit(Class *klass)
{
    ClassOrNamespace *previous = _currentClassOrNamespace;
    ClassOrNamespace *binding = nullptr;

    if (klass->name() && klass->name()->asQualifiedNameId())
        binding = _currentClassOrNamespace->lookupType(klass->name());

    if (! binding)
        binding = _currentClassOrNamespace->findOrCreateType(klass->name(), nullptr, klass);

    _currentClassOrNamespace = binding;
    _currentClassOrNamespace->addSymbol(klass);

    for (int i = 0; i < klass->baseClassCount(); ++i)
        process(klass->baseClassAt(i));

    for (int i = 0; i < klass->memberCount(); ++i)
        process(klass->memberAt(i));

    _currentClassOrNamespace = previous;
    return false;
}

bool CreateBindings::visit(ForwardClassDeclaration *klass)
{
    if (! klass->isFriend()) {
        ClassOrNamespace *previous = enterClassOrNamespaceBinding(klass);
        _currentClassOrNamespace = previous;
    }

    return false;
}

bool CreateBindings::visit(Enum *e)
{
    if (e->isScoped()) {
        ClassOrNamespace *previous = enterClassOrNamespaceBinding(e);
        _currentClassOrNamespace = previous;
    } else {
        _currentClassOrNamespace->addUnscopedEnum(e);
    }
    return false;
}

bool CreateBindings::visit(Declaration *decl)
{
    if (decl->isTypedef()) {
        FullySpecifiedType ty = decl->type();
        const Identifier *typedefId = decl->identifier();

        if (typedefId && ! (ty.isConst() || ty.isVolatile())) {
            if (const NamedType *namedTy = ty->asNamedType()) {
                if (ClassOrNamespace *e = _currentClassOrNamespace->lookupType(namedTy->name())) {
                    _currentClassOrNamespace->addNestedType(decl->name(), e);
                } else if (false) {
                    Overview oo;
                    qDebug() << "found entity not found for" << oo.prettyName(namedTy->name());
                }
            } else if (Class *klass = ty->asClassType()) {
                if (const Identifier *nameId = decl->name()->asNameId()) {
                    ClassOrNamespace *binding
                        = _currentClassOrNamespace->findOrCreateType(nameId, nullptr, klass);
                    binding->addSymbol(klass);
                }
            }
        }
    }
    if (Class *clazz = decl->type()->asClassType()) {
        if (const Name *name = clazz->name()) {
            if (const AnonymousNameId *anonymousNameId = name->asAnonymousNameId())
                _currentClassOrNamespace->_declaredOrTypedefedAnonymouses.insert(anonymousNameId);
        }
    }
    return false;
}

bool CreateBindings::visit(Function *function)
{
    ClassOrNamespace *previous = _currentClassOrNamespace;
    ClassOrNamespace *binding = lookupType(function, previous);
    if (!binding)
        return false;
    _currentClassOrNamespace = binding;
    for (int i = 0, count = function->memberCount(); i < count; ++i) {
        Symbol *s = function->memberAt(i);
        if (Block *b = s->asBlock())
            visit(b);
    }
    _currentClassOrNamespace = previous;
    return false;
}

bool CreateBindings::visit(Block *block)
{
    ClassOrNamespace *previous = _currentClassOrNamespace;

    ClassOrNamespace *binding = new ClassOrNamespace(this, previous);
    binding->_control = control();

    _currentClassOrNamespace = binding;
    _currentClassOrNamespace->addSymbol(block);

    for (int i = 0; i < block->memberCount(); ++i)
        // we cannot use lazy processing here, because we have to know
        // does this block contain any other blocks or classOrNamespaces
        process(block->memberAt(i), _currentClassOrNamespace);

    // we add this block to parent ClassOrNamespace only if it contains
    // any nested ClassOrNamespaces or other blocks(which have to contain
    // nested ClassOrNamespaces)
    if (! _currentClassOrNamespace->_blocks.empty()
            || ! _currentClassOrNamespace->_classOrNamespaces.empty()
            || ! _currentClassOrNamespace->_enums.empty()
            || ! _currentClassOrNamespace->_anonymouses.empty()) {
        previous->_blocks[block] = binding;
        _entities.append(binding);
    } else {
        delete binding;
        binding = nullptr;
    }

    _currentClassOrNamespace = previous;

    return false;
}

bool CreateBindings::visit(BaseClass *b)
{
    if (ClassOrNamespace *base = _currentClassOrNamespace->lookupType(b->name())) {
        _currentClassOrNamespace->addUsing(base);
    } else if (false) {
        Overview oo;
        qDebug() << "no entity for:" << oo.prettyName(b->name());
    }
    return false;
}

bool CreateBindings::visit(UsingDeclaration *u)
{
    if (u->name()) {
        if (const QualifiedNameId *q = u->name()->asQualifiedNameId()) {
            if (const Identifier *unqualifiedId = q->name()->asNameId()) {
                if (ClassOrNamespace *delegate = _currentClassOrNamespace->lookupType(q)) {
                    ClassOrNamespace *b = _currentClassOrNamespace->findOrCreateType(unqualifiedId);
                    b->addUsing(delegate);
                }
            }
        }
    }
    return false;
}

bool CreateBindings::visit(UsingNamespaceDirective *u)
{
    if (ClassOrNamespace *e = _currentClassOrNamespace->lookupType(u->name())) {
        _currentClassOrNamespace->addUsing(e);
    } else if (false) {
        Overview oo;
        qDebug() << "no entity for namespace:" << oo.prettyName(u->name());
    }
    return false;
}

bool CreateBindings::visit(NamespaceAlias *a)
{
    if (! a->identifier()) {
        return false;

    } else if (ClassOrNamespace *e = _currentClassOrNamespace->lookupType(a->namespaceName())) {
        if (a->name()->asNameId() || a->name()->asTemplateNameId() || a->name()->asAnonymousNameId())
            _currentClassOrNamespace->addNestedType(a->name(), e);

    } else if (false) {
        Overview oo;
        qDebug() << "no entity for namespace:" << oo.prettyName(a->namespaceName());
    }

    return false;
}

bool CreateBindings::visit(ObjCClass *klass)
{
    ClassOrNamespace *previous = enterGlobalClassOrNamespace(klass);

    process(klass->baseClass());

    for (int i = 0; i < klass->protocolCount(); ++i)
        process(klass->protocolAt(i));

    for (int i = 0; i < klass->memberCount(); ++i)
        process(klass->memberAt(i));

    _currentClassOrNamespace = previous;
    return false;
}

bool CreateBindings::visit(ObjCBaseClass *b)
{
    if (ClassOrNamespace *base = _globalNamespace->lookupType(b->name())) {
        _currentClassOrNamespace->addUsing(base);
    } else if (false) {
        Overview oo;
        qDebug() << "no entity for:" << oo.prettyName(b->name());
    }
    return false;
}

bool CreateBindings::visit(ObjCForwardClassDeclaration *klass)
{
    ClassOrNamespace *previous = enterGlobalClassOrNamespace(klass);
    _currentClassOrNamespace = previous;
    return false;
}

bool CreateBindings::visit(ObjCProtocol *proto)
{
    ClassOrNamespace *previous = enterGlobalClassOrNamespace(proto);

    for (int i = 0; i < proto->protocolCount(); ++i)
        process(proto->protocolAt(i));

    for (int i = 0; i < proto->memberCount(); ++i)
        process(proto->memberAt(i));

    _currentClassOrNamespace = previous;
    return false;
}

bool CreateBindings::visit(ObjCBaseProtocol *b)
{
    if (ClassOrNamespace *base = _globalNamespace->lookupType(b->name())) {
        _currentClassOrNamespace->addUsing(base);
    } else if (false) {
        Overview oo;
        qDebug() << "no entity for:" << oo.prettyName(b->name());
    }
    return false;
}

bool CreateBindings::visit(ObjCForwardProtocolDeclaration *proto)
{
    ClassOrNamespace *previous = enterGlobalClassOrNamespace(proto);
    _currentClassOrNamespace = previous;
    return false;
}

bool CreateBindings::visit(ObjCMethod *)
{
    return false;
}

Symbol *CreateBindings::instantiateTemplateFunction(const Name *instantiationName,
                                                    Template *specialization) const
{
    if (!specialization || !specialization->declaration()
        || !specialization->declaration()->asFunction())
        return nullptr;

    int argumentCountOfInstantiation = 0;
    const TemplateNameId *instantiation = nullptr;
    if (instantiationName->asTemplateNameId()) {
        instantiation = instantiationName->asTemplateNameId();
        argumentCountOfInstantiation = instantiation->templateArgumentCount();
    } else {
        // no template arguments passed in function call
        // check if all template parameters have default arguments (only check first parameter)
        if (specialization->templateParameterCount() == 0)
            return nullptr;

        if (Symbol *tParam = specialization->templateParameterAt(0);
            (!tParam->asTypenameArgument() &&
             !tParam->asTemplateTypeArgument()) ||
            !tParam->type().isValid())
            return nullptr;
    }

    const int argumentCountOfSpecialization = specialization->templateParameterCount();

    Clone cloner(_control.get());
    Subst subst(_control.get());
    for (int i = 0; i < argumentCountOfSpecialization; ++i) {


        Symbol *tParam = specialization->templateParameterAt(i);
        if (!tParam->asTypenameArgument() &&
            !tParam->asTemplateTypeArgument())
            continue;

        const Name *name = tParam->name();

        if (!name)
            continue;

        FullySpecifiedType ty = (i < argumentCountOfInstantiation) ?
                    instantiation->templateArgumentAt(i).type():
                    cloner.type(tParam->type(), &subst);

        subst.bind(cloner.name(name, &subst), ty);
    }
    return cloner.symbol(specialization, &subst);
}

} // CPlusPlus

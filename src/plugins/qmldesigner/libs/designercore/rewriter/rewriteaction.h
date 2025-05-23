// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "abstractproperty.h"
#include "modelnodepositionstorage.h"

#include <filemanager/qmlrefactoring.h>

namespace QmlDesigner {
namespace Internal {

class AddImportRewriteAction;
class AddPropertyRewriteAction;
class ChangeIdRewriteAction;
class ChangePropertyRewriteAction;
class ChangeTypeRewriteAction;
class RemoveImportRewriteAction;
class RemoveNodeRewriteAction;
class RemovePropertyRewriteAction;
class ReparentNodeRewriteAction;
class MoveNodeRewriteAction;

class RewriteAction
{
public:
    virtual bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) = 0;
    virtual QString info() const = 0;

    virtual AddImportRewriteAction *asAddImportRewriteAction() { return nullptr; }
    virtual AddPropertyRewriteAction *asAddPropertyRewriteAction() { return nullptr; }
    virtual ChangeIdRewriteAction *asChangeIdRewriteAction() { return nullptr; }
    virtual ChangePropertyRewriteAction *asChangePropertyRewriteAction() { return nullptr; }
    virtual ChangeTypeRewriteAction *asChangeTypeRewriteAction() { return nullptr; }
    virtual RemoveImportRewriteAction *asRemoveImportRewriteAction() { return nullptr; }
    virtual RemoveNodeRewriteAction *asRemoveNodeRewriteAction() { return nullptr; }
    virtual RemovePropertyRewriteAction *asRemovePropertyRewriteAction() { return nullptr; }
    virtual ReparentNodeRewriteAction *asReparentNodeRewriteAction() { return nullptr; }
    virtual MoveNodeRewriteAction *asMoveNodeRewriteAction() { return nullptr; }
    virtual ~RewriteAction() = default;

protected:
    RewriteAction() = default;

public:
    RewriteAction(const RewriteAction&) = delete;
    RewriteAction &operator=(const RewriteAction&) = delete;
};

class PropertyRewriteAction : public RewriteAction
{
public:
    PropertyRewriteAction(const AbstractProperty &property,
                          const QString &valueText,
                          QmlDesigner::QmlRefactoring::PropertyType propertyType,
                          const ModelNode &containedModelNode /* = ModelNode()*/)
        : m_property(property)
        , m_valueText(valueText)
        , m_propertyType(propertyType)
        , m_containedModelNode(containedModelNode)
        , m_scheduledInHierarchy(property.isValid() && property.parentModelNode().isInHierarchy())
    {}

    AbstractProperty property() const { return m_property; }

    QString valueText() const { return m_valueText; }

    QmlDesigner::QmlRefactoring::PropertyType propertyType() const { return m_propertyType; }

    ModelNode containedModelNode() const { return m_containedModelNode; }

    void setMovedAfterCreation(bool moved) { m_movedAfterCreation = moved; }

protected:
    bool scheduledInHierarchy() const { return m_scheduledInHierarchy; }
    bool movedAfterCreation() const { return m_movedAfterCreation; }
    std::optional<int> nodeLocation() const;

private:
    AbstractProperty m_property;
    QString m_valueText;
    QmlDesigner::QmlRefactoring::PropertyType m_propertyType;
    ModelNode m_containedModelNode;
    bool m_scheduledInHierarchy;
    bool m_movedAfterCreation = false;
};

class AddPropertyRewriteAction : public PropertyRewriteAction
{
public:
    using PropertyRewriteAction::PropertyRewriteAction;
    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;
    AddPropertyRewriteAction *asAddPropertyRewriteAction() override { return this; }
};

class ChangeIdRewriteAction: public RewriteAction
{
public:
    ChangeIdRewriteAction(const ModelNode &node, const QString &oldId, const QString &newId):
            m_node(node), m_oldId(oldId), m_newId(newId)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    ChangeIdRewriteAction *asChangeIdRewriteAction() override { return this; }

    ModelNode node() const
    { return m_node; }

private:
    ModelNode m_node;
    QString m_oldId;
    QString m_newId;
};

class ChangePropertyRewriteAction : public PropertyRewriteAction
{
public:
    using PropertyRewriteAction::PropertyRewriteAction;
    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;
    ChangePropertyRewriteAction *asChangePropertyRewriteAction() override { return this; }
};

class ChangeTypeRewriteAction:public RewriteAction
{
public:
    ChangeTypeRewriteAction(const ModelNode &node):
            m_node(node)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    ChangeTypeRewriteAction *asChangeTypeRewriteAction() override { return this; }

    ModelNode node() const
    { return m_node; }

private:
    ModelNode m_node;
};

class RemoveNodeRewriteAction: public RewriteAction
{
public:
    RemoveNodeRewriteAction(const ModelNode &node):
            m_node(node)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    RemoveNodeRewriteAction *asRemoveNodeRewriteAction() override { return this; }

    ModelNode node() const
    { return m_node; }

private:
    ModelNode m_node;
};

class RemovePropertyRewriteAction: public RewriteAction
{
public:
    RemovePropertyRewriteAction(const AbstractProperty &property):
            m_property(property)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    RemovePropertyRewriteAction *asRemovePropertyRewriteAction() override { return this; }

    AbstractProperty property() const
    { return m_property; }

private:
    AbstractProperty m_property;
};

class ReparentNodeRewriteAction: public RewriteAction
{
public:
    ReparentNodeRewriteAction(const ModelNode &node, const AbstractProperty &oldParentProperty, const AbstractProperty &targetProperty, QmlDesigner::QmlRefactoring::PropertyType propertyType):
            m_node(node), m_oldParentProperty(oldParentProperty), m_targetProperty(targetProperty), m_propertyType(propertyType)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    ReparentNodeRewriteAction *asReparentNodeRewriteAction() override { return this; }

    ModelNode reparentedNode() const
    { return m_node; }

    void setOldParentProperty(const AbstractProperty &oldParentProperty)
    { m_oldParentProperty = oldParentProperty; }

    AbstractProperty oldParentProperty() const
    { return m_oldParentProperty; }

    AbstractProperty targetProperty() const
    { return m_targetProperty; }

    QmlDesigner::QmlRefactoring::PropertyType propertyType() const
    { return m_propertyType; }

private:
    ModelNode m_node;
    AbstractProperty m_oldParentProperty;
    AbstractProperty m_targetProperty;
    QmlDesigner::QmlRefactoring::PropertyType m_propertyType;
};

class MoveNodeRewriteAction: public RewriteAction
{
public:
    MoveNodeRewriteAction(const ModelNode &movingNode, const ModelNode &newTrailingNode):
            m_movingNode(movingNode), m_newTrailingNode(newTrailingNode)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    MoveNodeRewriteAction *asMoveNodeRewriteAction() override { return this; }

    ModelNode movingNode() const { return m_movingNode; }

private:
    ModelNode m_movingNode;
    ModelNode m_newTrailingNode;
};

class AddImportRewriteAction: public RewriteAction
{
public:
    AddImportRewriteAction(const Import &import):
            m_import(import)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    AddImportRewriteAction *asAddImportRewriteAction() override { return this; }

    Import import() const { return m_import; }

private:
    Import m_import;
};

class RemoveImportRewriteAction: public RewriteAction
{
public:
    RemoveImportRewriteAction(const Import &import):
            m_import(import)
    {}

    bool execute(QmlDesigner::QmlRefactoring &refactoring, ModelNodePositionStorage &positionStore) override;
    QString info() const override;

    RemoveImportRewriteAction *asRemoveImportRewriteAction() override { return this; }

    Import import() const { return m_import; }

private:
    Import m_import;
};

} // namespace Internal
} // namespace QmlDesigner

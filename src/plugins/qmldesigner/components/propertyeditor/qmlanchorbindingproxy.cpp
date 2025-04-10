// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmlanchorbindingproxy.h"

#include <abstractview.h>
#include <exception.h>
#include <modelnodeoperations.h>
#include <modelnodeutils.h>
#include <nodeabstractproperty.h>
#include <qmlanchors.h>
#include <utils/qtcassert.h>
#include <utils/smallstring.h>
#include <variantproperty.h>

#include <QDebug>
#include <QtQml>

namespace QmlDesigner {

class ModelNode;
class NodeState;

namespace {
const Utils::SmallString auxDataString("anchors_");

Utils::SmallString auxPropertyString(Utils::SmallStringView name)
{
    return auxDataString + name;
}
} // namespace

inline static void backupPropertyAndRemove(const ModelNode &node, const PropertyName &propertyName)
{
    ModelNodeUtils::backupPropertyAndRemove(node, propertyName, auxPropertyString(propertyName));
}

inline static void restoreProperty(const ModelNode &node, const PropertyName &propertyName)
{
    ModelNodeUtils::restoreProperty(node, propertyName, auxPropertyString(propertyName));
}

QmlAnchorBindingProxy::QmlAnchorBindingProxy(QObject *parent) :
    QObject(parent),
    m_relativeTopTarget(SameEdge), m_relativeBottomTarget(SameEdge),
    m_relativeLeftTarget(SameEdge), m_relativeRightTarget(SameEdge),
    m_relativeVerticalTarget(Center), m_relativeHorizontalTarget(Center),
    m_locked(false), m_ignoreQml(false)
{
}

QmlAnchorBindingProxy::~QmlAnchorBindingProxy() = default;

void QmlAnchorBindingProxy::setup(const QmlItemNode &fxItemNode)
{
    m_qmlItemNode = fxItemNode;

    m_ignoreQml = true;

    setupAnchorTargets();

    emit itemNodeChanged();
    emit parentChanged();

    emitAnchorSignals();

    if (m_qmlItemNode.hasNodeParent()) {
        emit topTargetChanged();
        emit bottomTargetChanged();
        emit leftTargetChanged();
        emit rightTargetChanged();
        emit verticalTargetChanged();
        emit horizontalTargetChanged();
    }

    emit invalidated();

    m_ignoreQml = false;
}

void QmlAnchorBindingProxy::invalidate(const QmlItemNode &fxItemNode)
{
    if (m_locked)
        return;

    m_qmlItemNode = fxItemNode;

    m_ignoreQml = true;

    auto parentModelNode = [](const QmlItemNode &node) {
        QTC_ASSERT(node.modelNode().hasParentProperty(), return ModelNode());
        return node.modelNode().parentProperty().parentModelNode();
    };

    m_verticalTarget =
            m_horizontalTarget =
            m_topTarget =
            m_bottomTarget =
            m_leftTarget =
            m_rightTarget =
            parentModelNode(m_qmlItemNode);

    setupAnchorTargets();

    emitAnchorSignals();

    if (m_qmlItemNode.hasNodeParent()) {
        emit itemNodeChanged();
        emit topTargetChanged();
        emit bottomTargetChanged();
        emit leftTargetChanged();
        emit rightTargetChanged();
        emit verticalTargetChanged();
        emit horizontalTargetChanged();
    }

    emit invalidated();

    m_ignoreQml = false;
}

void QmlAnchorBindingProxy::setupAnchorTargets()
{
    if (m_qmlItemNode.modelNode().hasParentProperty())
        setDefaultAnchorTarget(m_qmlItemNode.modelNode().parentProperty().parentModelNode());
    else
        setDefaultAnchorTarget(ModelNode());

    if (topAnchored()) {
        AnchorLine topAnchor = m_qmlItemNode.anchors().instanceAnchor(AnchorLineTop);
        ModelNode targetNode = topAnchor.qmlItemNode();
        if (targetNode.isValid()) {
            m_topTarget = targetNode;
            if (topAnchor.type() == AnchorLineTop) {
                m_relativeTopTarget = SameEdge;
            } else if (topAnchor.type() == AnchorLineBottom) {
                m_relativeTopTarget = OppositeEdge;
            } else if (topAnchor.type() == AnchorLineVerticalCenter) {
                m_relativeTopTarget = Center;
            } else {
                qWarning() << __FUNCTION__ << "invalid anchor line";
            }
        } else {
            m_relativeTopTarget = SameEdge;
        }
    }

    if (bottomAnchored()) {
        AnchorLine bottomAnchor = m_qmlItemNode.anchors().instanceAnchor(AnchorLineBottom);
        ModelNode targetNode = bottomAnchor.qmlItemNode();
        if (targetNode.isValid()) {
            m_bottomTarget = targetNode;
            if (bottomAnchor.type() == AnchorLineBottom) {
                m_relativeBottomTarget = SameEdge;
            } else if (bottomAnchor.type() == AnchorLineTop) {
                m_relativeBottomTarget = OppositeEdge;
            } else if (bottomAnchor.type() == AnchorLineVerticalCenter) {
                m_relativeBottomTarget = Center;
            } else {
                qWarning() << __FUNCTION__ << "invalid anchor line";
            }
        } else {
            m_relativeBottomTarget = SameEdge;
        }
    }

    if (leftAnchored()) {
        AnchorLine leftAnchor = m_qmlItemNode.anchors().instanceAnchor(AnchorLineLeft);
        ModelNode targetNode = leftAnchor.qmlItemNode();
        if (targetNode.isValid()) {
            m_leftTarget = targetNode;
            if (leftAnchor.type() == AnchorLineLeft) {
                m_relativeLeftTarget = SameEdge;
            } else if (leftAnchor.type() == AnchorLineRight) {
                m_relativeLeftTarget = OppositeEdge;
            } else if (leftAnchor.type() == AnchorLineHorizontalCenter) {
                m_relativeLeftTarget = Center;
            } else {
                qWarning() << __FUNCTION__ << "invalid anchor line";
            }
        } else {
            m_relativeLeftTarget = SameEdge;
        }
    }

    if (rightAnchored()) {
        AnchorLine rightAnchor = m_qmlItemNode.anchors().instanceAnchor(AnchorLineRight);
        ModelNode targetNode = rightAnchor.qmlItemNode();
        if (targetNode.isValid()) {
            m_rightTarget = targetNode;
            if (rightAnchor.type() == AnchorLineRight) {
                m_relativeRightTarget = SameEdge;
            } else if (rightAnchor.type() == AnchorLineLeft) {
                m_relativeRightTarget = OppositeEdge;
            } else if (rightAnchor.type() == AnchorLineHorizontalCenter) {
                m_relativeRightTarget = Center;
            } else {
                qWarning() << __FUNCTION__ << "invalid anchor line";
            }
        } else {
            m_relativeRightTarget = SameEdge;
        }
    }

    if (verticalCentered()) {
        ModelNode targetNode = m_qmlItemNode.anchors().instanceAnchor(AnchorLineVerticalCenter).qmlItemNode();
        if (targetNode.isValid())
            m_verticalTarget = targetNode;
    }

    if (horizontalCentered()) {
        ModelNode targetNode = m_qmlItemNode.anchors().instanceAnchor(AnchorLineHorizontalCenter).qmlItemNode();
        if (targetNode.isValid())
            m_horizontalTarget = targetNode;
    }
}

void QmlAnchorBindingProxy::emitAnchorSignals()
{
    emit topAnchorChanged();
    emit bottomAnchorChanged();
    emit leftAnchorChanged();
    emit rightAnchorChanged();
    emit centeredHChanged();
    emit centeredVChanged();
    emit anchorsChanged();

    emit relativeAnchorTargetTopChanged();
    emit relativeAnchorTargetBottomChanged();
    emit relativeAnchorTargetLeftChanged();
    emit relativeAnchorTargetRightChanged();
}

void QmlAnchorBindingProxy::setDefaultRelativeTopTarget()
{
    if (m_topTarget.modelNode() == m_qmlItemNode.modelNode().parentProperty().parentModelNode()) {
        m_relativeTopTarget = SameEdge;
    } else {
        m_relativeTopTarget = OppositeEdge;
    }
}

void QmlAnchorBindingProxy::setDefaultRelativeBottomTarget()
{
    if (m_bottomTarget.modelNode() == m_qmlItemNode.modelNode().parentProperty().parentModelNode()) {
        m_relativeBottomTarget = SameEdge;
    } else {
        m_relativeBottomTarget = OppositeEdge;
    }
}

void QmlAnchorBindingProxy::setDefaultRelativeLeftTarget()
{
    if (m_leftTarget.modelNode() == m_qmlItemNode.modelNode().parentProperty().parentModelNode()) {
        m_relativeLeftTarget = SameEdge;
    } else {
        m_relativeLeftTarget = OppositeEdge;
    }
}

void QmlAnchorBindingProxy::setDefaultRelativeRightTarget()
{
    if (m_rightTarget.modelNode() == m_qmlItemNode.modelNode().parentProperty().parentModelNode()) {
        m_relativeRightTarget = SameEdge;
    } else {
        m_relativeRightTarget = OppositeEdge;
    }
}

bool QmlAnchorBindingProxy::executeInTransaction(const QByteArray &identifier, const AbstractView::OperationBlock &lambda)
{
    return m_qmlItemNode.modelNode().view()->executeInTransaction(identifier, lambda);
}

bool QmlAnchorBindingProxy::hasParent() const
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.hasNodeParent();
}

bool QmlAnchorBindingProxy::isInLayout() const
{
    return m_qmlItemNode.isInLayout();
}

bool QmlAnchorBindingProxy::isFilled() const
{
    return m_qmlItemNode.isValid()
            && hasAnchors()
            && topAnchored()
            && bottomAnchored()
            && leftAnchored()
            && rightAnchored()
            && (m_qmlItemNode.instanceValue("anchors.topMargin").toInt() == 0)
            && (m_qmlItemNode.instanceValue("anchors.bottomMargin").toInt() == 0)
            && (m_qmlItemNode.instanceValue("anchors.leftMargin").toInt() == 0)
            && (m_qmlItemNode.instanceValue("anchors.rightMargin").toInt() == 0);
}

bool QmlAnchorBindingProxy::topAnchored() const
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchor(AnchorLineTop);
}

bool QmlAnchorBindingProxy::bottomAnchored() const
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchor(AnchorLineBottom);
}

bool QmlAnchorBindingProxy::leftAnchored() const
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchor(AnchorLineLeft);
}

bool QmlAnchorBindingProxy::rightAnchored() const
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchor(AnchorLineRight);
}

bool QmlAnchorBindingProxy::hasAnchors() const
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchors();
}


void QmlAnchorBindingProxy::setTopTarget(const QString &target)
{

    if (m_ignoreQml)
        return;

    QmlItemNode newTarget(targetIdToNode(target));

    if (newTarget == m_topTarget)
        return;

    if (!newTarget.isValid())
        return;

    executeInTransaction("QmlAnchorBindingProxy::setTopTarget", [this, newTarget](){
        m_topTarget = newTarget;
        setDefaultRelativeTopTarget();
        anchorTop();
    });

    emit topTargetChanged();
}


void QmlAnchorBindingProxy::setBottomTarget(const QString &target)
{
    if (m_ignoreQml)
        return;

    QmlItemNode newTarget(targetIdToNode(target));

    if (newTarget == m_bottomTarget)
        return;

    if (!newTarget.isValid())
        return;

    executeInTransaction("QmlAnchorBindingProxy::setBottomTarget", [this, newTarget](){
        m_bottomTarget = newTarget;
        setDefaultRelativeBottomTarget();
        anchorBottom();

    });

    emit bottomTargetChanged();
}

void QmlAnchorBindingProxy::setLeftTarget(const QString &target)
{
    if (m_ignoreQml)
        return;

    QmlItemNode newTarget(targetIdToNode(target));

    if (newTarget == m_leftTarget)
        return;

    if (!newTarget.isValid())
        return;

    executeInTransaction("QmlAnchorBindingProxy::setLeftTarget", [this, newTarget](){
        m_leftTarget = newTarget;
        setDefaultRelativeLeftTarget();
        anchorLeft();
    });

    emit leftTargetChanged();
}

void QmlAnchorBindingProxy::setRightTarget(const QString &target)
{
    if (m_ignoreQml)
        return;

    QmlItemNode newTarget(targetIdToNode(target));

    if (newTarget == m_rightTarget)
        return;

    if (!newTarget.isValid())
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRightTarget", [this, newTarget](){
        m_rightTarget = newTarget;
        setDefaultRelativeRightTarget();
        anchorRight();
    });

    emit rightTargetChanged();
}

void QmlAnchorBindingProxy::setVerticalTarget(const QString &target)
{
    if (m_ignoreQml)
        return;

    QmlItemNode newTarget(targetIdToNode(target));

    if (newTarget == m_verticalTarget)
        return;

    if (!newTarget.isValid())
        return;

    executeInTransaction("QmlAnchorBindingProxy::setVerticalTarget", [this, newTarget](){
        m_verticalTarget = newTarget;
        anchorVertical();
    });

    emit verticalTargetChanged();
}

void QmlAnchorBindingProxy::setHorizontalTarget(const QString &target)
{
    if (m_ignoreQml)
        return;

    QmlItemNode newTarget(targetIdToNode(target));

    if (newTarget == m_horizontalTarget)
        return;

    if (!newTarget.isValid())
        return;

    executeInTransaction("QmlAnchorBindingProxy::setHorizontalTarget", [this, newTarget](){
        m_horizontalTarget = newTarget;
        anchorHorizontal();
    });

    emit horizontalTargetChanged();
}

void QmlAnchorBindingProxy::setRelativeAnchorTargetTop(QmlAnchorBindingProxy::RelativeAnchorTarget target)
{
    if (m_ignoreQml)
        return;

    if (target == m_relativeTopTarget)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRelativeAnchorTargetTop", [this, target](){
        m_relativeTopTarget = target;
        anchorTop();
    });

    emit relativeAnchorTargetTopChanged();
}

void QmlAnchorBindingProxy::setRelativeAnchorTargetBottom(QmlAnchorBindingProxy::RelativeAnchorTarget target)
{
    if (m_ignoreQml)
        return;

    if (target == m_relativeBottomTarget)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRelativeAnchorTargetBottom", [this, target](){
        m_relativeBottomTarget = target;
        anchorBottom();
    });

    emit relativeAnchorTargetBottomChanged();
}

void QmlAnchorBindingProxy::setRelativeAnchorTargetLeft(QmlAnchorBindingProxy::RelativeAnchorTarget target)
{
    if (m_ignoreQml)
        return;

    if (target == m_relativeLeftTarget)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRelativeAnchorTargetLeft", [this, target](){
        m_relativeLeftTarget = target;
        anchorLeft();

    });

    emit relativeAnchorTargetLeftChanged();
}

void QmlAnchorBindingProxy::setRelativeAnchorTargetRight(QmlAnchorBindingProxy::RelativeAnchorTarget target)
{
    if (m_ignoreQml)
        return;

    if (target == m_relativeRightTarget)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRelativeAnchorTargetRight", [this, target](){
        m_relativeRightTarget = target;
        anchorRight();
    });

    emit relativeAnchorTargetRightChanged();

}

void QmlAnchorBindingProxy::setRelativeAnchorTargetVertical(QmlAnchorBindingProxy::RelativeAnchorTarget target)
{
    if (m_ignoreQml)
        return;

    if (target == m_relativeVerticalTarget)
        return;


    executeInTransaction("QmlAnchorBindingProxy::setRelativeAnchorTargetVertical", [this, target](){
        m_relativeVerticalTarget = target;
        anchorVertical();
    });

    emit relativeAnchorTargetVerticalChanged();
}

void QmlAnchorBindingProxy::setRelativeAnchorTargetHorizontal(QmlAnchorBindingProxy::RelativeAnchorTarget target)
{
    if (m_ignoreQml)
        return;

    if (target == m_relativeHorizontalTarget)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRelativeAnchorTargetHorizontal", [this, target](){
        m_relativeHorizontalTarget = target;
        anchorHorizontal();
    });

    emit relativeAnchorTargetHorizontalChanged();
}

QStringList QmlAnchorBindingProxy::possibleTargetItems() const
{
    QStringList stringList;
    if (!m_qmlItemNode.isValid())
        return stringList;

    QList<QmlItemNode> itemList;

    if (m_qmlItemNode.instanceParent().modelNode().isValid())
        itemList = toQmlItemNodeList(m_qmlItemNode.instanceParent().modelNode().directSubModelNodes());
    itemList.removeOne(m_qmlItemNode);
    //We currently have no instanceChildren().
    //So we double check here if the instanceParents are equal.
    for (const QmlItemNode &node : std::as_const(itemList))
        if (node.isValid() && (node.instanceParent().modelNode() != m_qmlItemNode.instanceParent().modelNode()))
            itemList.removeAll(node);

    for (const QmlItemNode &itemNode : std::as_const(itemList)) {
        if (itemNode.isValid() && !itemNode.id().isEmpty())
            stringList.append(itemNode.id());
    }

    QmlItemNode parent(m_qmlItemNode.instanceParent().toQmlItemNode());

    if (parent.isValid())
        stringList.append(QStringLiteral("parent"));

    return stringList;
}

void QmlAnchorBindingProxy::registerDeclarativeType()
{
    qmlRegisterType<QmlAnchorBindingProxy>("HelperWidgets",2,0,"AnchorBindingProxy");
}

int QmlAnchorBindingProxy::indexOfPossibleTargetItem(const QString &targetName) const
{
    return possibleTargetItems().indexOf(targetName);
}

void QmlAnchorBindingProxy::resetLayout()
{

    executeInTransaction("QmlAnchorBindingProxy::resetLayout", [this](){
        m_qmlItemNode.anchors().removeAnchors();
        m_qmlItemNode.anchors().removeMargins();

        restoreProperty(modelNode(), "x");
        restoreProperty(modelNode(), "y");
        restoreProperty(modelNode(), "width");
        restoreProperty(modelNode(), "height");
    });

        emit topAnchorChanged();
        emit bottomAnchorChanged();
        emit leftAnchorChanged();
        emit rightAnchorChanged();
        emit anchorsChanged();
    }

void QmlAnchorBindingProxy::setBottomAnchor(bool anchor)
{
    if (!m_qmlItemNode.hasNodeParent())
        return;

    if (bottomAnchored() == anchor)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setBottomAnchor", [this, anchor](){
        if (!anchor) {
            removeBottomAnchor();
        } else {
            setDefaultRelativeBottomTarget();
            anchorBottom();
            backupPropertyAndRemove(modelNode(), "y");
            if (topAnchored())
                backupPropertyAndRemove(modelNode(), "height");
        }

    });

    emit relativeAnchorTargetBottomChanged();
    emit bottomAnchorChanged();

    if (hasAnchors() != anchor)
        emit anchorsChanged();
}

void QmlAnchorBindingProxy::setLeftAnchor(bool anchor)
{
    if (!m_qmlItemNode.hasNodeParent())
        return;

    if (leftAnchored() == anchor)
        return;


    executeInTransaction("QmlAnchorBindingProxy::setLeftAnchor", [this, anchor](){
        if (!anchor) {
            removeLeftAnchor();
        } else {
            setDefaultRelativeLeftTarget();

            anchorLeft();
            backupPropertyAndRemove(modelNode(), "x");
            if (rightAnchored())
                backupPropertyAndRemove(modelNode(), "width");
        }

    });

    emit relativeAnchorTargetLeftChanged();
    emit leftAnchorChanged();
    if (hasAnchors() != anchor)
        emit anchorsChanged();
}

void QmlAnchorBindingProxy::setRightAnchor(bool anchor)
{
    if (!m_qmlItemNode.hasNodeParent())
        return;

    if (rightAnchored() == anchor)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setRightAnchor", [this, anchor](){
        if (!anchor) {
            removeRightAnchor();
        } else {
            setDefaultRelativeRightTarget();

            anchorRight();
            backupPropertyAndRemove(modelNode(), "x");
            if (leftAnchored())
                backupPropertyAndRemove(modelNode(), "width");
        }

    });

    emit relativeAnchorTargetRightChanged();
    emit rightAnchorChanged();

    if (hasAnchors() != anchor)
        emit anchorsChanged();
}
 QRectF QmlAnchorBindingProxy::parentBoundingBox()
{
    if (m_qmlItemNode.hasInstanceParent()) {
        if (m_qmlItemNode.instanceParentItem().instanceContentItemBoundingRect().isValid())
            return m_qmlItemNode.instanceParentItem().instanceContentItemBoundingRect();
        return m_qmlItemNode.instanceParentItem().instanceBoundingRect();
    }

    return QRect();
}

QRectF QmlAnchorBindingProxy::boundingBox(const QmlItemNode &node)
{
    if (node.isValid())
        return node.instanceTransformWithContentTransform().mapRect(node.instanceBoundingRect());

    return QRect();
}

QRectF QmlAnchorBindingProxy::transformedBoundingBox()
{
    return m_qmlItemNode.instanceTransformWithContentTransform().mapRect(m_qmlItemNode.instanceBoundingRect());
}

void QmlAnchorBindingProxy::anchorTop()
{
    m_locked = true;

    bool topTargetIsParent = m_topTarget == m_qmlItemNode.instanceParentItem();

    if (m_relativeTopTarget == SameEdge) {
        qreal topPos = topTargetIsParent ? parentBoundingBox().top() : boundingBox(m_topTarget).top();
        qreal topMargin = transformedBoundingBox().top() - topPos;
        m_qmlItemNode.anchors().setMargin( AnchorLineTop, topMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineTop, m_topTarget, AnchorLineTop);
    } else if (m_relativeTopTarget == OppositeEdge) {
        qreal bottomPos = topTargetIsParent ? parentBoundingBox().bottom() : boundingBox(m_topTarget).bottom();
        qreal topMargin = transformedBoundingBox().top() - bottomPos;
        m_qmlItemNode.anchors().setMargin( AnchorLineTop, topMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineTop, m_topTarget, AnchorLineBottom);
    } else if (m_relativeTopTarget == Center) {
        qreal centerPos = topTargetIsParent ? parentBoundingBox().center().y() : boundingBox(m_topTarget).center().y();
        qreal topMargin = transformedBoundingBox().top() - centerPos;
        m_qmlItemNode.anchors().setMargin(AnchorLineTop, topMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineTop, m_topTarget, AnchorLineVerticalCenter);
    }

    m_locked = false;
}

void QmlAnchorBindingProxy::anchorBottom()
{
    m_locked = true;

    bool bottomTargetIsParent = m_bottomTarget == m_qmlItemNode.instanceParentItem();

    if (m_relativeBottomTarget == SameEdge) {
        qreal bottomPos = bottomTargetIsParent ? parentBoundingBox().bottom() : boundingBox(m_bottomTarget).bottom();
        qreal bottomMargin = bottomPos - transformedBoundingBox().bottom();
        m_qmlItemNode.anchors().setMargin( AnchorLineBottom, bottomMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineBottom, m_bottomTarget, AnchorLineBottom);
    } else  if (m_relativeBottomTarget == OppositeEdge) {
        qreal topPos = bottomTargetIsParent ? parentBoundingBox().top() : boundingBox(m_bottomTarget).top();
        qreal bottomMargin = topPos - transformedBoundingBox().bottom();
        m_qmlItemNode.anchors().setMargin( AnchorLineBottom, bottomMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineBottom, m_bottomTarget, AnchorLineTop);
    } else if (m_relativeBottomTarget == Center) {
        qreal centerPos = bottomTargetIsParent ? parentBoundingBox().center().y() : boundingBox(m_bottomTarget).center().y();
        qreal bottomMargin = centerPos - transformedBoundingBox().bottom();
        m_qmlItemNode.anchors().setMargin(AnchorLineBottom, bottomMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineBottom, m_bottomTarget, AnchorLineVerticalCenter);
    }

    m_locked = false;
}

void QmlAnchorBindingProxy::anchorLeft()
{
    m_locked = true;

    bool leftTargetIsParent = m_leftTarget == m_qmlItemNode.instanceParentItem();

    if (m_relativeLeftTarget == SameEdge) {
        qreal leftPos = leftTargetIsParent ? parentBoundingBox().left() : boundingBox(m_leftTarget).left();
        qreal leftMargin = transformedBoundingBox().left() - leftPos;
        m_qmlItemNode.anchors().setMargin(AnchorLineLeft, leftMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineLeft, m_leftTarget, AnchorLineLeft);
    } else if (m_relativeLeftTarget == OppositeEdge) {
        qreal rightPos = leftTargetIsParent ? parentBoundingBox().right() : boundingBox(m_leftTarget).right();
        qreal leftMargin = transformedBoundingBox().left() - rightPos;
        m_qmlItemNode.anchors().setMargin( AnchorLineLeft, leftMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineLeft, m_leftTarget, AnchorLineRight);
    } else if (m_relativeLeftTarget == Center) {
        qreal centerPos = leftTargetIsParent ? parentBoundingBox().center().x() : boundingBox(m_leftTarget).center().x();
        qreal leftMargin = transformedBoundingBox().left() - centerPos;
        m_qmlItemNode.anchors().setMargin(AnchorLineLeft, leftMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineLeft, m_leftTarget, AnchorLineHorizontalCenter);
    }

    m_locked = false;
}

void QmlAnchorBindingProxy::anchorRight()
{
    m_locked = true;

    bool rightTargetIsParent = m_rightTarget == m_qmlItemNode.instanceParentItem();

    if (m_relativeRightTarget == SameEdge) {
        qreal rightPos = rightTargetIsParent ? parentBoundingBox().right() : boundingBox(m_rightTarget).right();
        qreal rightMargin = rightPos - transformedBoundingBox().right();
        m_qmlItemNode.anchors().setMargin( AnchorLineRight, rightMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineRight, m_rightTarget, AnchorLineRight);
    } else if (m_relativeRightTarget == OppositeEdge) {
        qreal leftPos = rightTargetIsParent ? parentBoundingBox().left() : boundingBox(m_rightTarget).left();
        qreal rightMargin = leftPos - transformedBoundingBox().right();
        m_qmlItemNode.anchors().setMargin( AnchorLineRight, rightMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineRight, m_rightTarget, AnchorLineLeft);
    } else if (m_relativeRightTarget == Center) {
        qreal centerPos = rightTargetIsParent ? parentBoundingBox().center().x() : boundingBox(m_rightTarget).center().x();
        qreal rightMargin = centerPos - transformedBoundingBox().right();
        m_qmlItemNode.anchors().setMargin(AnchorLineRight, rightMargin);
        m_qmlItemNode.anchors().setAnchor(AnchorLineRight, m_rightTarget, AnchorLineHorizontalCenter);
    }

    m_locked = false;
}

void QmlAnchorBindingProxy::anchorVertical()
{
    m_locked = true;
    if (m_relativeVerticalTarget == SameEdge) {
        m_qmlItemNode.anchors().setAnchor(AnchorLineVerticalCenter, m_verticalTarget, AnchorLineRight);
    } else if (m_relativeVerticalTarget == OppositeEdge) {
        m_qmlItemNode.anchors().setAnchor(AnchorLineVerticalCenter, m_verticalTarget, AnchorLineLeft);
    } else if (m_relativeVerticalTarget == Center) {
        m_qmlItemNode.anchors().setAnchor(AnchorLineVerticalCenter, m_verticalTarget, AnchorLineVerticalCenter);

    }
    backupPropertyAndRemove(modelNode(), "y");
    m_locked = false;
}

void QmlAnchorBindingProxy::anchorHorizontal()
{
    m_locked = true;
    if (m_relativeHorizontalTarget == SameEdge) {
        m_qmlItemNode.anchors().setAnchor(AnchorLineHorizontalCenter, m_horizontalTarget, AnchorLineRight);
    } else if (m_relativeVerticalTarget == OppositeEdge) {
        m_qmlItemNode.anchors().setAnchor(AnchorLineHorizontalCenter, m_horizontalTarget, AnchorLineLeft);
    } else if (m_relativeVerticalTarget == Center) {
        m_qmlItemNode.anchors().setAnchor(AnchorLineHorizontalCenter, m_horizontalTarget, AnchorLineHorizontalCenter);
    }
     backupPropertyAndRemove(modelNode(), "x");
    m_locked = false;
}

QmlItemNode QmlAnchorBindingProxy::targetIdToNode(const QString &id) const
{
    QmlItemNode itemNode;

    if (m_qmlItemNode.isValid() && m_qmlItemNode.view()) {

        itemNode = m_qmlItemNode.view()->modelNodeForId(id);

        if (id == QStringLiteral("parent"))
            itemNode = m_qmlItemNode.instanceParent().modelNode();
    }

    return itemNode;
}

QString QmlAnchorBindingProxy::idForNode(const QmlItemNode &qmlItemNode) const
{
    if (!qmlItemNode.modelNode().isValid())
        return {};

    if (!qmlItemNode.isValid())
        return qmlItemNode.id();

    if (m_qmlItemNode.instanceParent().modelNode() == qmlItemNode)
        return QStringLiteral("parent");

    return qmlItemNode.id();
}

ModelNode QmlAnchorBindingProxy::modelNode() const
{
    return m_qmlItemNode.modelNode();
}

void QmlAnchorBindingProxy::setTopAnchor(bool anchor)
{
    if (!m_qmlItemNode.hasNodeParent())
        return ;

    if (topAnchored() == anchor)
        return;

    executeInTransaction("QmlAnchorBindingProxy::setTopAnchor", [this, anchor](){
        if (!anchor) {
            removeTopAnchor();
        } else {
            setDefaultRelativeTopTarget();

            anchorTop();
            backupPropertyAndRemove(modelNode(), "y");
            if (bottomAnchored())
                backupPropertyAndRemove(modelNode(), "height");
        }
    });

    emit relativeAnchorTargetTopChanged();
    emit topAnchorChanged();
    if (hasAnchors() != anchor)
        emit anchorsChanged();
}

void QmlAnchorBindingProxy::removeTopAnchor() {
    executeInTransaction("QmlAnchorBindingProxy::removeTopAnchor", [this](){
        m_qmlItemNode.anchors().removeAnchor(AnchorLineTop);
        m_qmlItemNode.anchors().removeMargin(AnchorLineTop);

        restoreProperty(modelNode(), "height");
        if (!(bottomAnchored() || verticalCentered()))
            restoreProperty(modelNode(), "y");
    });
}

void QmlAnchorBindingProxy::removeBottomAnchor()
{
    executeInTransaction("QmlAnchorBindingProxy::removeBottomAnchor", [this](){
        m_qmlItemNode.anchors().removeAnchor(AnchorLineBottom);
        m_qmlItemNode.anchors().removeMargin(AnchorLineBottom);

        restoreProperty(modelNode(), "height");
        if (!(topAnchored() || verticalCentered()))
            restoreProperty(modelNode(), "y");
    });
}

void QmlAnchorBindingProxy::removeLeftAnchor()
{
    executeInTransaction("QmlAnchorBindingProxy::removeLeftAnchor", [this](){
        m_qmlItemNode.anchors().removeAnchor(AnchorLineLeft);
        m_qmlItemNode.anchors().removeMargin(AnchorLineLeft);

        restoreProperty(modelNode(), "width");
        if (!(rightAnchored() || horizontalCentered()))
            restoreProperty(modelNode(), "x");
    });
}

void QmlAnchorBindingProxy::removeRightAnchor()
{
    executeInTransaction("QmlAnchorBindingProxy::removeRightAnchor", [this](){
        m_qmlItemNode.anchors().removeAnchor(AnchorLineRight);
        m_qmlItemNode.anchors().removeMargin(AnchorLineRight);

        restoreProperty(modelNode(), "width");
        if (!(leftAnchored() || horizontalCentered()))
            restoreProperty(modelNode(), "x");
    });
}

void QmlAnchorBindingProxy::setVerticalCentered(bool centered)
{
    if (!m_qmlItemNode.hasNodeParent())
        return ;

    if (verticalCentered() == centered)
        return;

    if (centered && horizontalCentered()) {
        centerIn();
        return;
    }

    m_locked = true;

    executeInTransaction("QmlAnchorBindingProxy::setVerticalCentered", [this, centered](){
        if (!centered) {
            m_qmlItemNode.anchors().removeAnchor(AnchorLineVerticalCenter);
            m_qmlItemNode.anchors().removeMargin(AnchorLineVerticalCenter);
            if (!(topAnchored() || bottomAnchored()))
                restoreProperty(m_qmlItemNode, "y");
        } else {
            m_relativeVerticalTarget = Center;

            anchorVertical();
        }

    });
    m_locked = false;

    emit relativeAnchorTargetVerticalChanged();
    emit centeredVChanged();
}

void QmlAnchorBindingProxy::setHorizontalCentered(bool centered)
{
    if (!m_qmlItemNode.hasNodeParent())
        return ;

    if (horizontalCentered() == centered)
        return;

    if (centered && verticalCentered()) {
        centerIn();
        return;
    }

    m_locked = true;

    executeInTransaction("QmlAnchorBindingProxy::setHorizontalCentered", [this, centered](){
        if (!centered) {
            m_qmlItemNode.anchors().removeAnchor(AnchorLineHorizontalCenter);
            m_qmlItemNode.anchors().removeMargin(AnchorLineHorizontalCenter);
            if (!(leftAnchored() || rightAnchored()))
                restoreProperty(m_qmlItemNode, "x");
        } else {
            m_relativeHorizontalTarget = Center;

            anchorHorizontal();
        }
    });
    m_locked = false;

    emit relativeAnchorTargetHorizontalChanged();
    emit centeredHChanged();
}

bool QmlAnchorBindingProxy::verticalCentered()
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchor(AnchorLineVerticalCenter);
}

QString QmlAnchorBindingProxy::topTarget() const
{
    return idForNode(m_topTarget);
}

QString QmlAnchorBindingProxy::bottomTarget() const
{
    return idForNode(m_bottomTarget);
}

QString QmlAnchorBindingProxy::leftTarget() const
{
    return idForNode(m_leftTarget);
}

QString QmlAnchorBindingProxy::rightTarget() const
{
    return idForNode(m_rightTarget);
}

QmlAnchorBindingProxy::RelativeAnchorTarget QmlAnchorBindingProxy::relativeAnchorTargetTop() const
{
    return m_relativeTopTarget;
}

QmlAnchorBindingProxy::RelativeAnchorTarget QmlAnchorBindingProxy::relativeAnchorTargetBottom() const
{
    return m_relativeBottomTarget;
}

QmlAnchorBindingProxy::RelativeAnchorTarget QmlAnchorBindingProxy::relativeAnchorTargetLeft() const
{
    return m_relativeLeftTarget;
}

QmlAnchorBindingProxy::RelativeAnchorTarget QmlAnchorBindingProxy::relativeAnchorTargetRight() const
{
    return m_relativeRightTarget;
}

QmlAnchorBindingProxy::RelativeAnchorTarget QmlAnchorBindingProxy::relativeAnchorTargetVertical() const
{
    return m_relativeVerticalTarget;
}

QmlAnchorBindingProxy::RelativeAnchorTarget QmlAnchorBindingProxy::relativeAnchorTargetHorizontal() const
{
    return m_relativeHorizontalTarget;
}

QString QmlAnchorBindingProxy::verticalTarget() const
{
    return idForNode(m_verticalTarget);
}

QString QmlAnchorBindingProxy::horizontalTarget() const
{
    return idForNode(m_horizontalTarget);
}

bool QmlAnchorBindingProxy::horizontalCentered()
{
    return m_qmlItemNode.isValid() && m_qmlItemNode.anchors().instanceHasAnchor(AnchorLineHorizontalCenter);
}

void QmlAnchorBindingProxy::fill()
{
    executeInTransaction("QmlAnchorBindingProxy::fill", [this](){
        backupPropertyAndRemove(modelNode(), "x");
        backupPropertyAndRemove(modelNode(), "y");
        backupPropertyAndRemove(modelNode(), "width");
        backupPropertyAndRemove(modelNode(), "height");

        m_qmlItemNode.anchors().fill();

        m_qmlItemNode.anchors().removeMargin(AnchorLineRight);
        m_qmlItemNode.anchors().removeMargin(AnchorLineLeft);
        m_qmlItemNode.anchors().removeMargin(AnchorLineTop);
        m_qmlItemNode.anchors().removeMargin(AnchorLineBottom);

    });

    emit topAnchorChanged();
    emit bottomAnchorChanged();
    emit leftAnchorChanged();
    emit rightAnchorChanged();
    emit anchorsChanged();
}

void QmlAnchorBindingProxy::centerIn()
{
    executeInTransaction("QmlAnchorBindingProxy::centerIn", [this]() {
        backupPropertyAndRemove(modelNode(), "x");
        backupPropertyAndRemove(modelNode(), "y");

        m_qmlItemNode.anchors().centerIn();

        m_qmlItemNode.anchors().removeMargin(AnchorLineRight);
        m_qmlItemNode.anchors().removeMargin(AnchorLineLeft);
        m_qmlItemNode.anchors().removeMargin(AnchorLineTop);
        m_qmlItemNode.anchors().removeMargin(AnchorLineBottom);
    });

    emitAnchorSignals();
}

void QmlAnchorBindingProxy::setDefaultAnchorTarget(const ModelNode &modelNode)
{
    m_verticalTarget = modelNode;
    m_horizontalTarget = modelNode;
    m_topTarget = modelNode;
    m_bottomTarget = modelNode;
    m_leftTarget = modelNode;
    m_rightTarget = modelNode;
}

} // namespace QmlDesigner

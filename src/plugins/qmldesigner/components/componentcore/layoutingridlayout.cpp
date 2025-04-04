// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "layoutingridlayout.h"

#include <nodeabstractproperty.h>
#include <nodemetainfo.h>
#include <variantproperty.h>
#include <bindingproperty.h>
#include <rewritertransaction.h>

#include <utils/qtcassert.h>

#include <utils/algorithm.h>

namespace QmlDesigner {

inline static void reparentTo(const ModelNode &node, const QmlItemNode &parent)
{

    if (parent.isValid() && node.isValid()) {
        NodeAbstractProperty parentProperty;

        if (parent.hasDefaultPropertyName())
            parentProperty = parent.defaultNodeAbstractProperty();
        else
            parentProperty = parent.nodeAbstractProperty("data");

        parentProperty.reparentHere(node);
    }
}

static int findFirstBigger(const QVector<int> &v, int tolerance)
{
    if (v.isEmpty())
        return 0;

    int last = v.constFirst();
    for (int i = 0; i < v.length(); ++i) {
        if (v.value(i) > last + tolerance)
            return i;
        last = v.value(i);
    }
    return v.length();
}

static void  removeSimilarValues(QVector<int> &v, int tolerance)
{
    QVector<int> newVector;

    if (v.size() < 2)
        return;

    while (!v.isEmpty()) {
        int clusterLength = findFirstBigger(v, tolerance);
        int average = v.at(0);
        newVector.append(average);
        v.remove(0, clusterLength);
    }

    v = newVector;
}

static int getCell(const QVector<int> &offsets, int offset)
{
        for (int i = 0; i < offsets.length(); ++i) {
            if (offset < offsets.at(i))
                    return i;
        }
    return offsets.length();
}

static int lowerBound(int i)
{
    if (i < 15)
        return 16;

    return i;
}

inline static QPointF getUpperLeftPosition(const QList<ModelNode> &modelNodeList)
{
    QPointF postion(std::numeric_limits<qreal>::max(), std::numeric_limits<qreal>::max());
    for (const ModelNode &modelNode : modelNodeList) {
        if (QmlItemNode::isValidQmlItemNode(modelNode)) {
            QmlItemNode qmlIitemNode = QmlItemNode(modelNode);
            if (qmlIitemNode.instancePosition().x() < postion.x())
                postion.setX(qmlIitemNode.instancePosition().x());
            if (qmlIitemNode.instancePosition().y() < postion.y())
                postion.setY(qmlIitemNode.instancePosition().y());
        }
    }
    return postion;
}

static void setUpperLeftPostionToNode(const ModelNode &layoutNode, const QList<ModelNode> &modelNodeList)
{
    QPointF upperLeftPosition = getUpperLeftPosition(modelNodeList);
    layoutNode.variantProperty("x").setValue(qRound(upperLeftPosition.x()));
    layoutNode.variantProperty("y").setValue(qRound(upperLeftPosition.y()));
}

void LayoutInGridLayout::reparentToNodeAndRemovePositionForModelNodes(
    const ModelNode &parentModelNode, const QList<ModelNode> &modelNodeList)
{
    for (ModelNode modelNode : modelNodeList) {
        reparentTo(modelNode, parentModelNode);
        modelNode.removeProperty("x");
        modelNode.removeProperty("y");
        const QList<VariantProperty> variantProperties = modelNode.variantProperties();
        for (const VariantProperty &variantProperty : variantProperties) {
            if (variantProperty.name().contains("anchors."))
                modelNode.removeProperty(variantProperty.name());
        }
        const QList<BindingProperty> bindingProperties = modelNode.bindingProperties();
        for (const BindingProperty &bindingProperty : bindingProperties) {
            if (bindingProperty.name().contains("anchors."))
                modelNode.removeProperty(bindingProperty.name());
        }
    }
}

void LayoutInGridLayout::setSizeAsPreferredSize(const QList<ModelNode> &modelNodeList)
{
     for (ModelNode modelNode : modelNodeList) {
         if (modelNode.hasVariantProperty("width")) {
             modelNode.variantProperty("Layout.preferredWidth").setValue(modelNode.variantProperty("width").value());
             modelNode.removeProperty("width");
         }
         if (modelNode.hasVariantProperty("height")) {
             modelNode.variantProperty("Layout.preferredHeight").setValue(modelNode.variantProperty("height").value());
             modelNode.removeProperty("height");
         }
     }
}

LayoutInGridLayout::LayoutInGridLayout(const QmlDesigner::SelectionContext &selectionContext) :
    m_selectionContext(selectionContext)
  ,m_startX(0)
  ,m_startY(0)
{

}

void LayoutInGridLayout::doIt()
{
    const TypeName layoutType = "QtQuick.Layouts.GridLayout";

    if (!m_selectionContext.view()
            || !m_selectionContext.view()->model()->hasNodeMetaInfo(layoutType))
        return;

    collectItemNodes();
    collectOffsets();
    sortOffsets();
    calculateGridOffsets();
    removeEmtpyRowsAndColumns();
    initializeCells();
    markUsedCells();

    QTC_ASSERT(m_parentNode.isValid(), return);

    if (QmlItemNode::isValidQmlItemNode(m_selectionContext.firstSelectedModelNode())) {
        const QmlItemNode qmlItemNode = QmlItemNode(m_selectionContext.firstSelectedModelNode());

        if (qmlItemNode.hasInstanceParentItem()) {

            ModelNode layoutNode;

            m_selectionContext.view()->executeInTransaction("LayoutInGridLayout1",[this, &layoutNode, layoutType](){
                QTC_ASSERT(m_selectionContext.view()->model()->hasNodeMetaInfo(layoutType), return);
#ifdef QDS_USE_PROJECTSTORAGE
                layoutNode = m_selectionContext.view()->createModelNode(layoutType);
#else
                NodeMetaInfo metaInfo = m_selectionContext.view()->model()->metaInfo(layoutType);
                layoutNode = m_selectionContext.view()->createModelNode(layoutType, metaInfo.majorVersion(), metaInfo.minorVersion());
#endif
                reparentTo(layoutNode, m_parentNode);
                layoutNode.ensureIdExists();
            });

            m_selectionContext.view()->executeInTransaction("LayoutInGridLayout2", [this, layoutNode](){

                fillEmptyCells();

                QList<ModelNode> sortedSelectedNodes = m_layoutedNodes;
                Utils::sort(sortedSelectedNodes, lessThan());

                removeSpacersBySpanning(sortedSelectedNodes);

                setUpperLeftPostionToNode(layoutNode, m_selectionContext.selectedModelNodes());
                reparentToNodeAndRemovePositionForModelNodes(layoutNode, sortedSelectedNodes);
                setSizeAsPreferredSize(sortedSelectedNodes);
                setSpanning(layoutNode);
            });
        }
    }
}

static bool hasQtQuickLayoutImport(const SelectionContext &context)
{
    if (context.view() && context.view()->model()) {
        Import import = Import::createLibraryImport(QStringLiteral("QtQuick.Layouts"), {});
        return context.view()->model()->hasImport(import, true, true);
    }

    return false;
}

void LayoutInGridLayout::ensureLayoutImport(const SelectionContext &context)
{
    if (!hasQtQuickLayoutImport(context)) {
        Import layoutImport = Import::createLibraryImport("QtQuick.Layouts", {});
        context.view()-> model()->changeImports({layoutImport}, {});
    }
}

void LayoutInGridLayout::layout(const SelectionContext &context)
{
    LayoutInGridLayout operation(context);
    operation.doIt();
}

int LayoutInGridLayout::columnCount() const
{
    return m_xTopOffsets.size();
}

int LayoutInGridLayout::rowCount() const
{
    return m_yTopOffsets.size();
}

void LayoutInGridLayout::collectItemNodes()
{
    const QList<ModelNode> modelNodes = m_selectionContext.selectedModelNodes();
    for (const ModelNode &modelNode : modelNodes) {
        if (QmlItemNode::isValidQmlItemNode(modelNode)) {
            QmlItemNode itemNode = modelNode;
            if (itemNode.instanceSize().width() > 0
                    && itemNode.instanceSize().height() > 0)
                m_qmlItemNodes.append(itemNode);
        }
    }

    if (m_qmlItemNodes.isEmpty())
        return;

    m_parentNode = m_qmlItemNodes.constFirst().instanceParentItem();
}

void LayoutInGridLayout::collectOffsets()
{
    //We collect all different x and y offsets that define the cells
    for (const QmlItemNode &qmlItemNode : std::as_const(m_qmlItemNodes)) {
        int x  = qRound((qmlItemNode.instancePosition().x()));
        m_xTopOffsets.append(x);
        x  = qRound((qmlItemNode.instancePosition().x() + lowerBound(qmlItemNode.instanceBoundingRect().width())));
        m_xBottomOffsets.append(x);

        int y  = qRound((qmlItemNode.instancePosition().y()) );
        m_yTopOffsets.append(y);
        y  = qRound((qmlItemNode.instancePosition().y() + lowerBound(qmlItemNode.instanceBoundingRect().height())));
        m_yBottomOffsets.append(y);
    }
}

void LayoutInGridLayout::sortOffsets()
{
    std::sort(m_xTopOffsets.begin(), m_xTopOffsets.end());
    std::sort(m_yTopOffsets.begin(), m_yTopOffsets.end());
    std::sort(m_xBottomOffsets.begin(), m_xBottomOffsets.end());
    std::sort(m_yBottomOffsets.begin(), m_yBottomOffsets.end());
}

void LayoutInGridLayout::calculateGridOffsets()
{
    if (!m_xTopOffsets.isEmpty())
        m_startX = m_xTopOffsets.constFirst();

    if (!m_yTopOffsets.isEmpty())
        m_startY = m_yTopOffsets.constFirst();

    const int defaultWidthTolerance = 64;
    const int defaultHeightTolerance = 64;

    int widthTolerance = defaultWidthTolerance;
    int heightTolerance = defaultHeightTolerance;

    //The tolerance cannot be bigger then the size of an item
    for (const auto &qmlItemNode : std::as_const(m_qmlItemNodes)) {
        widthTolerance = qMin(qmlItemNode.instanceSize().toSize().width() - 1, widthTolerance);
        heightTolerance = qMin(qmlItemNode.instanceSize().toSize().height() - 1, heightTolerance);
    }

    //Now we create clusters of similar offsets and keep only the biggest offset for each cluster

    removeSimilarValues(m_xTopOffsets, widthTolerance);
    removeSimilarValues(m_yTopOffsets, heightTolerance);
    removeSimilarValues(m_xBottomOffsets, widthTolerance);
    removeSimilarValues(m_yBottomOffsets, heightTolerance);

    m_xTopOffsets += m_xBottomOffsets;
    m_yTopOffsets += m_yBottomOffsets;

    std::sort(m_xTopOffsets.begin(), m_xTopOffsets.end());
    std::sort(m_yTopOffsets.begin(), m_yTopOffsets.end());

    removeSimilarValues(m_xTopOffsets, widthTolerance);
    removeSimilarValues(m_yTopOffsets, heightTolerance);

    //The first offset is not important, because it just defines the beginning of the layout
    if (!m_xTopOffsets.isEmpty())
        m_xTopOffsets.removeFirst();
    if (!m_yTopOffsets.isEmpty())
        m_yTopOffsets.removeFirst();
}

void LayoutInGridLayout::removeEmtpyRowsAndColumns()
{
    //Allocate arrays for used rows and columns and rows
    m_rows = QVector<bool>(rowCount());
    m_rows.fill(false);

    m_columns = QVector<bool>(columnCount());
    m_columns.fill(false);

    for (const auto &qmlItemNode : std::as_const(m_qmlItemNodes)) {
        int xCell = getCell(m_xTopOffsets, qmlItemNode.instancePosition().x());
        int yCell = getCell(m_yTopOffsets, qmlItemNode.instancePosition().y());

        int xCellRight = getCell(m_xTopOffsets, qmlItemNode.instancePosition().x() +  lowerBound(qmlItemNode.instanceSize().width()));
        int yCellbottom = getCell(m_yTopOffsets, qmlItemNode.instancePosition().y() +  lowerBound(qmlItemNode.instanceSize().height()));
        for (int x = xCell; x < xCellRight; ++x)
            for (int y = yCell; y < yCellbottom; ++y) {
                m_columns[x] = true;
                m_rows[y] = true;
            }
    }

    for (int i = m_columns.length() - 1; i >= 0; --i)
        if (!m_columns.at(i))
            m_xTopOffsets.remove(i);

    for (int i = m_rows.length() - 1; i >= 0; --i)
        if (!m_rows.at(i))
            m_yTopOffsets.remove(i);
}

void LayoutInGridLayout::initializeCells()
{
    //Allocate array and mark cells as false.
    m_cells = QVector<bool>(columnCount() * rowCount());
    m_cells.fill(false);
}

void LayoutInGridLayout::markUsedCells()
{
    //We mark cells which are covered by items with true
    for (const auto &qmlItemNode : std::as_const(m_qmlItemNodes)) {
        int xCell = getCell(m_xTopOffsets, qmlItemNode.instancePosition().x());
        int yCell = getCell(m_yTopOffsets, qmlItemNode.instancePosition().y());

        int xCellRight = getCell(m_xTopOffsets, qmlItemNode.instancePosition().x() +  lowerBound(qmlItemNode.instanceSize().width()));
        int yCellbottom = getCell(m_yTopOffsets, qmlItemNode.instancePosition().y() +  lowerBound(qmlItemNode.instanceSize().height()));

        for (int x = xCell; x < xCellRight; ++x)
            for (int y = yCell; y < yCellbottom; ++y) {
                m_cells[y * columnCount()  + x] = true;
            }
    }
}

void LayoutInGridLayout::fillEmptyCells()
{
    //Cells which are not covered by items and are not marked as true have to be filled with a "spacer" item
    m_layoutedNodes = m_selectionContext.selectedModelNodes();

    for (const auto &itemNode : std::as_const(m_qmlItemNodes)) {
        m_layoutedNodes.append(itemNode);
    }

    for (int x = 0; x < columnCount(); ++x)
        for (int y = 0; y < rowCount(); ++y)
            if (!m_cells.at(y * columnCount() + x)) { //This cell does not contain an item.
                int xPos = m_startX;
                if (x > 0)
                    xPos = m_xTopOffsets.at(x-1);

                int yPos = m_startY;
                if (y > 0)
                    yPos = m_yTopOffsets.at(y-1);

#ifdef QDS_USE_PROJECTSTORAGE
                ModelNode newNode = m_selectionContext.view()->createModelNode("Item");
#else
                NodeMetaInfo metaInfo = m_selectionContext.view()->model()->metaInfo("QtQuick.Item");

                ModelNode newNode = m_selectionContext.view()->createModelNode("QtQuick.Item", metaInfo.majorVersion(), metaInfo.minorVersion());
#endif
                reparentTo(newNode, m_parentNode);

                m_spacerNodes.append(newNode);

                QmlItemNode newItemNode(newNode);
                newItemNode.setVariantProperty("x", xPos);
                newItemNode.setVariantProperty("y", yPos);
                newItemNode.setVariantProperty("width", 14);
                newItemNode.setVariantProperty("height", 14);
                newItemNode.setId(m_selectionContext.view()->model()->generateNewId("spacer"));
            }
    m_layoutedNodes.append(m_spacerNodes);
}
namespace {
constexpr AuxiliaryDataKeyView extraSpanningProperty{AuxiliaryDataType::Document, "extraSpanning"};
}
void LayoutInGridLayout::setSpanning(const ModelNode &layoutNode)
{
    //Define a post layout operation to set columns/rows and the spanning
    if (layoutNode.isValid()) {
        layoutNode.variantProperty("columns").setValue(columnCount());
        layoutNode.variantProperty("rows").setValue(rowCount());

        for (const ModelNode &modelNode : std::as_const(m_layoutedNodes)) {
            QmlItemNode qmlItemNode(modelNode);
            int xCell = getCell(m_xTopOffsets, qmlItemNode.instancePosition().x());
            int yCell = getCell(m_yTopOffsets, qmlItemNode.instancePosition().y());

            int xCellRight = getCell(m_xTopOffsets, qmlItemNode.instancePosition().x() +  qmlItemNode.instanceSize().width());
            int yCellbottom = getCell(m_yTopOffsets, qmlItemNode.instancePosition().y() +  qmlItemNode.instanceSize().height());

            int columnSpan = xCellRight - xCell;
            int rowSpan = yCellbottom - yCell;

            if (m_spacerNodes.contains(modelNode)) {
                columnSpan = 1;
                rowSpan = 1;
            }

            if (auto data = modelNode.auxiliaryData(extraSpanningProperty))
                columnSpan += data->toInt();

            if (columnSpan > 1)
                qmlItemNode.setVariantProperty("Layout.columnSpan", columnSpan);

            if (rowSpan > 1)
                qmlItemNode.setVariantProperty("Layout.rowSpan", rowSpan);
        }
    }
}

void LayoutInGridLayout::removeSpacersBySpanning(QList<ModelNode> &nodes)
{
    for (const ModelNode &node : std::as_const(m_spacerNodes)) {
        if (int index = nodes.indexOf(node)) {
            ModelNode before;
            if (index > 0)
                before = nodes.at(index - 1);
            if (m_spacerNodes.contains(before)) {
                m_spacerNodes.removeAll(node);
                m_layoutedNodes.removeAll(node);
                nodes.removeAll(node);
                ModelNode(node).destroy();
                auto extraSpanningData = before.auxiliaryData(extraSpanningProperty);
                if (extraSpanningData)
                    before.setAuxiliaryData(extraSpanningProperty, extraSpanningData->toInt() + 1);
                else
                    before.setAuxiliaryData(extraSpanningProperty, 1);
            }
        }

    }
}

LayoutInGridLayout::LessThan LayoutInGridLayout::lessThan()
{
    return [this](const ModelNode &node1, const ModelNode &node2) {
        QmlItemNode itemNode1 = QmlItemNode(node1);
        QmlItemNode itemNode2 = QmlItemNode(node2);
        if (itemNode1.isValid() && itemNode2.isValid()) {

            int xPos1 = itemNode1.instancePosition().x();
            int yPos1 = itemNode1.instancePosition().y();

            int xPos2 = itemNode2.instancePosition().x();
            int yPos2 = itemNode2.instancePosition().y();

            /* The spacer items do not have proper instances, yet.
                 * Because of this get the position from the model instead
                 * from instances.
                 */
            if (m_spacerNodes.contains(itemNode1)) {
                xPos1 = itemNode1.modelValue("x").toInt();
                yPos1 = itemNode1.modelValue("y").toInt();

            }

            if (m_spacerNodes.contains(itemNode2)) {
                xPos2 = itemNode2.modelValue("x").toInt();
                yPos2 = itemNode2.modelValue("y").toInt();
            }

            int xCell1 = getCell(m_xTopOffsets, xPos1);
            int yCell1 = getCell(m_yTopOffsets, yPos1);

            int xCell2 = getCell(m_xTopOffsets, xPos2);
            int yCell2 = getCell(m_yTopOffsets, yPos2);

            //We have to compare the cells. First the rows then the columns.

            if (yCell1 < yCell2)
                return true;

            if ((yCell1 == yCell2) && xCell1 < xCell2)
                return true;
        }
        return false;
    };
}

} //QmlDesigner

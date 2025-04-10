// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "memoryusagemodel.h"
#include "qmlprofilereventtypes.h"
#include "qmlprofilermodelmanager.h"
#include "qmlprofilertr.h"

#include <utils/qtcassert.h>

namespace QmlProfiler::Internal {

MemoryUsageModel::MemoryUsageModel(QmlProfilerModelManager *manager,
                                   Timeline::TimelineModelAggregator *parent) :
    QmlProfilerTimelineModel(manager, MemoryAllocation, UndefinedRangeType, ProfileMemory, parent)
{
    // Register additional features. The base class already registers the main feature.
    // Don't register initializer, finalizer, or clearer as the base class has done so already.
    modelManager()->registerFeatures(Constants::QML_JS_RANGE_FEATURES ^ (1 << ProfileCompiling),
                                     std::bind(&QmlProfilerTimelineModel::loadEvent, this,
                                               std::placeholders::_1, std::placeholders::_2));
}

qint64 MemoryUsageModel::rowMaxValue(int rowNumber) const
{
    Q_UNUSED(rowNumber)
    return m_maxSize;
}

int MemoryUsageModel::expandedRow(int index) const
{
    int type = selectionId(index);
    return (type == HeapPage || type == LargeItem) ? 1 : 2;
}

int MemoryUsageModel::collapsedRow(int index) const
{
    return expandedRow(index);
}

int MemoryUsageModel::typeId(int index) const
{
    return m_data[index].typeId;
}

QRgb MemoryUsageModel::color(int index) const
{
    return colorBySelectionId(index);
}

float MemoryUsageModel::relativeHeight(int index) const
{
    return qMin(1.0f, (float)m_data[index].size / (float)m_maxSize);
}

QVariantMap MemoryUsageModel::location(int index) const
{
    return locationFromTypeId(index);
}

QVariantList MemoryUsageModel::labels() const
{
    QVariantList result;

    QVariantMap element;
    element.insert(QLatin1String("description"), Tr::tr("Memory Allocation"));
    element.insert(QLatin1String("id"), HeapPage);
    result << element;

    element.clear();
    element.insert(QLatin1String("description"), Tr::tr("Memory Usage"));
    element.insert(QLatin1String("id"), SmallItem);
    result << element;

    return result;
}

static int toSameSignedInt(qint64 number)
{
    if (number > std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    if (number < std::numeric_limits<int>::min())
        return std::numeric_limits<int>::min();
    return static_cast<int>(number);
}

QVariantMap MemoryUsageModel::details(int index) const
{
    QVariantMap result;
    const Item *ev = &m_data[index];

    if (ev->allocated >= -ev->deallocated)
        result.insert(QLatin1String("displayName"), Tr::tr("Memory Allocated"));
    else
        result.insert(QLatin1String("displayName"), Tr::tr("Memory Freed"));

    result.insert(Tr::tr("Total"), Tr::tr("%n byte(s)", nullptr, toSameSignedInt(ev->size)));
    if (ev->allocations > 0) {
        result.insert(Tr::tr("Allocated"), Tr::tr("%n byte(s)", nullptr, toSameSignedInt(ev->allocated)));
        result.insert(Tr::tr("Allocations"), ev->allocations);
    }
    if (ev->deallocations > 0) {
        result.insert(Tr::tr("Deallocated"),
                      Tr::tr("%n byte(s)", nullptr, toSameSignedInt(-ev->deallocated)));
        result.insert(Tr::tr("Deallocations"), ev->deallocations);
    }
    QString memoryTypeName;
    switch (selectionId(index)) {
    case HeapPage:  memoryTypeName = Tr::tr("Heap Allocation"); break;
    case LargeItem: memoryTypeName = Tr::tr("Large Item Allocation"); break;
    case SmallItem: memoryTypeName = Tr::tr("Heap Usage"); break;
    default: Q_UNREACHABLE();
    }
    result.insert(Tr::tr("Type"), memoryTypeName);

    result.insert(Tr::tr("Location"), modelManager()->eventType(ev->typeId).displayName());
    return result;
}

void MemoryUsageModel::loadEvent(const QmlEvent &event, const QmlEventType &type)
{
    if (type.message() != MemoryAllocation) {
        if (type.rangeType() != UndefinedRangeType) {
            m_continuation = ContinueNothing;
            if (event.rangeStage() == RangeStart)
                m_rangeStack.push(RangeStackFrame(event.typeIndex(), event.timestamp()));
            else if (event.rangeStage() == RangeEnd) {
                QTC_ASSERT(!m_rangeStack.isEmpty(), return);
                QTC_ASSERT(m_rangeStack.top().originTypeIndex == event.typeIndex(), return);
                m_rangeStack.pop();
            }
        }
        return;
    }

    auto canContinue = [&](EventContinuation continuation) {
        QTC_ASSERT(continuation != ContinueNothing, return false);
        if ((m_continuation & continuation) == 0)
            return false;

        int currentIndex = (continuation == ContinueAllocation ? m_currentJSHeapIndex :
                                                                 m_currentUsageIndex);

        if (m_rangeStack.isEmpty()) {
            qint64 amount = event.number<qint64>(0);
            // outside of ranges show monotonous allocation or deallocation
            return (amount >= 0 && m_data[currentIndex].allocated >= 0)
                    || (amount < 0 && m_data[currentIndex].deallocated > 0);
        } else {
            return m_data[currentIndex].typeId == m_rangeStack.top().originTypeIndex
                    && m_rangeStack.top().startTime < startTime(currentIndex);
        }
    };

    if (type.detailType() == SmallItem || type.detailType() == LargeItem) {
        if (canContinue(ContinueUsage)) {
            m_data[m_currentUsageIndex].update(event.number<qint64>(0));
            m_currentUsage = m_data[m_currentUsageIndex].size;
        } else {
            Item allocation(
                        m_rangeStack.empty() ? event.typeIndex() :
                                               m_rangeStack.top().originTypeIndex,
                        m_currentUsage);
            allocation.update(event.number<qint64>(0));
            m_currentUsage = allocation.size;

            if (m_currentUsageIndex != -1) {
                qint64 duration = event.timestamp() - startTime(m_currentUsageIndex);
                if (duration > 0) {
                    insertEnd(m_currentUsageIndex, duration - 1);
                    m_currentUsageIndex = insertStart(event.timestamp(), SmallItem);
                    m_data.insert(m_currentUsageIndex, allocation);
                } else {
                    // Ignore ranges of 0 duration. We only need to keep track of the sizes.
                    m_data[m_currentUsageIndex] = allocation;
                }
            } else {
                m_currentUsageIndex = insertStart(event.timestamp(), SmallItem);
                m_data.insert(m_currentUsageIndex, allocation);
            }
            m_continuation = m_continuation | ContinueUsage;
        }
    }

    if (type.detailType() == HeapPage || type.detailType() == LargeItem) {
        if (canContinue(ContinueAllocation)
                && type.detailType() == selectionId(m_currentJSHeapIndex)) {
            m_data[m_currentJSHeapIndex].update(event.number<qint64>(0));
            m_currentSize = m_data[m_currentJSHeapIndex].size;
        } else {
            Item allocation(
                        m_rangeStack.empty() ? event.typeIndex() :
                                               m_rangeStack.top().originTypeIndex,
                        m_currentSize);
            allocation.update(event.number<qint64>(0));
            m_currentSize = allocation.size;

            if (m_currentSize > m_maxSize)
                m_maxSize = m_currentSize;

            if (m_currentJSHeapIndex != -1) {
                qint64 duration = event.timestamp() - startTime(m_currentJSHeapIndex);
                if (duration > 0){
                    insertEnd(m_currentJSHeapIndex, duration - 1);
                    m_currentJSHeapIndex = insertStart(event.timestamp(), type.detailType());
                    m_data.insert(m_currentJSHeapIndex, allocation);
                } else {
                    // Ignore ranges of 0 duration. We only need to keep track of the sizes.
                    m_data[m_currentJSHeapIndex] = allocation;
                }
            } else {
                m_currentJSHeapIndex = insertStart(event.timestamp(), type.detailType());
                m_data.insert(m_currentJSHeapIndex, allocation);
            }

            m_continuation = m_continuation | ContinueAllocation;
        }
    }
}

void MemoryUsageModel::finalize()
{
    if (m_currentJSHeapIndex != -1) {
        insertEnd(m_currentJSHeapIndex,
                  modelManager()->traceEnd() - startTime(m_currentJSHeapIndex));
    }
    if (m_currentUsageIndex != -1)
        insertEnd(m_currentUsageIndex, modelManager()->traceEnd() - startTime(m_currentUsageIndex));


    computeNesting();
    setExpandedRowCount(3);
    setCollapsedRowCount(3);
    QmlProfilerTimelineModel::finalize();
}

void MemoryUsageModel::clear()
{
    m_data.clear();
    m_maxSize = 1;
    m_currentSize = 0;
    m_currentUsage = 0;
    m_currentUsageIndex = -1;
    m_currentJSHeapIndex = -1;
    m_continuation = ContinueNothing;
    m_rangeStack.clear();
    QmlProfilerTimelineModel::clear();
}

bool MemoryUsageModel::handlesTypeId(int typeId) const
{
    Q_UNUSED(typeId)
    // We don't want the memory ranges allocated by some QML/JS function to be highlighted when
    // propagating a typeId selection to the timeline. The actual range should be highlighted.
    return false;
}

MemoryUsageModel::Item::Item(int typeId, qint64 baseAmount) :
    size(baseAmount), allocated(0), deallocated(0), allocations(0), deallocations(0),
    typeId(typeId)
{
}

void MemoryUsageModel::Item::update(qint64 amount)
{
    size += amount;
    if (amount < 0) {
        deallocated += amount;
        ++deallocations;
    } else {
        allocated += amount;
        ++allocations;
    }
}

} // namespace QmlProfiler::Internal

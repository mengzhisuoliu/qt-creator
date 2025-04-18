// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
#pragma once

#include "qmlprofilereventtypes.h"

#include <tracing/traceevent.h>

#include <QString>
#include <QByteArray>
#include <QVarLengthArray>
#include <QMetaType>

#include <initializer_list>
#include <limits>
#include <type_traits>

namespace QmlProfiler {

class QmlEvent : public Timeline::TraceEvent
{
public:
    static const qint32 staticClassId = 0x716d6c65; // 'qmle';

    QmlEvent() : TraceEvent(staticClassId) {}

    template<typename Number>
    QmlEvent(qint64 timestamp, int typeIndex, std::initializer_list<Number> list)
        : TraceEvent(staticClassId, timestamp, typeIndex)
    {
        assignNumbers<std::initializer_list<Number>, Number>(list);
    }

    QmlEvent(qint64 timestamp, int typeIndex, const QString &data)
        : TraceEvent(staticClassId, timestamp, typeIndex)
    {
        assignNumbers<QByteArray, qint8>(data.toUtf8());
    }

    template<typename Number>
    QmlEvent(qint64 timestamp, int typeIndex, const QList<Number> &data)
        : TraceEvent(staticClassId, timestamp, typeIndex)
    {
        assignNumbers<QList<Number>, Number>(data);
    }

    QmlEvent(const QmlEvent &other)
        : TraceEvent(other), m_dataType(other.m_dataType), m_dataLength(other.m_dataLength)
    {
        assignData(other);
    }

    QmlEvent(QmlEvent &&other)
        : TraceEvent(other), m_dataType(other.m_dataType), m_dataLength(other.m_dataLength)
    {
        memcpy(&m_data, &other.m_data, sizeof(m_data));
        other.m_dataType = Inline8Bit; // prevent dtor from deleting the pointer
    }

    QmlEvent &operator=(const QmlEvent &other)
    {
        if (this != &other) {
            clearPointer();
            TraceEvent::operator=(other);
            m_dataType = other.m_dataType;
            m_dataLength = other.m_dataLength;
            assignData(other);
        }
        return *this;
    }

    QmlEvent &operator=(QmlEvent &&other)
    {
        if (this != &other) {
            TraceEvent::operator=(other);
            m_dataType = other.m_dataType;
            m_dataLength = other.m_dataLength;
            memcpy(&m_data, &other.m_data, sizeof(m_data));
            other.m_dataType = Inline8Bit;
        }
        return *this;
    }

    ~QmlEvent()
    {
        clearPointer();
    }

    template<typename Number>
    Number number(int i) const
    {
        // Trailing zeroes can be omitted, for example for SceneGraph events
        if (i >= m_dataLength)
            return 0;
        switch (m_dataType) {
QT_WARNING_PUSH
QT_WARNING_DISABLE_GCC("-Warray-bounds")    // we're protected by the check above
        case Inline8Bit:
            return m_data.internal8bit[i];
        case Inline16Bit:
            return m_data.internal16bit[i];
        case Inline32Bit:
            return m_data.internal32bit[i];
        case Inline64Bit:
            return m_data.internal64bit[i];
QT_WARNING_POP
        case External8Bit:
            return static_cast<const qint8 *>(m_data.external)[i];
        case External16Bit:
            return static_cast<const qint16 *>(m_data.external)[i];
        case External32Bit:
            return static_cast<const qint32 *>(m_data.external)[i];
        case External64Bit:
            return static_cast<const qint64 *>(m_data.external)[i];
        default:
            return 0;
        }
    }

    template<typename Number>
    void setNumber(int i, Number number)
    {
        QVarLengthArray<Number> nums = numbers<QVarLengthArray<Number>, Number>();
        int prevSize = nums.size();
        if (i >= prevSize) {
            nums.resize(i + 1);
            // Fill with zeroes. We don't want to accidentally prevent squeezing.
            while (prevSize < i)
                nums[prevSize++] = 0;
        }
        nums[i] = number;
        setNumbers<QVarLengthArray<Number>, Number>(nums);
    }

    template<typename Container, typename Number>
    void setNumbers(const Container &numbers)
    {
        clearPointer();
        assignNumbers<Container, Number>(numbers);
    }

    template<typename Number>
    void setNumbers(std::initializer_list<Number> numbers)
    {
        setNumbers<std::initializer_list<Number>, Number>(numbers);
    }

    template<typename Container, typename Number = qint64>
    Container numbers() const
    {
        Container container;
        for (int i = 0; i < m_dataLength; ++i)
            container.append(number<Number>(i));
        return container;
    }

    QString string() const
    {
        switch (m_dataType) {
        case External8Bit:
            return QString::fromUtf8(static_cast<const char *>(m_data.external), m_dataLength);
        case Inline8Bit:
            return QString::fromUtf8(m_data.internalChar, m_dataLength);
        default:
            Q_UNREACHABLE();
            return QString();
        }
    }

    void setString(const QString &data)
    {
        clearPointer();
        assignNumbers<QByteArray, char>(data.toUtf8());
    }

    Message rangeStage() const
    {
        Q_ASSERT(m_dataType == Inline8Bit);
        return static_cast<Message>(m_data.internal8bit[0]);
    }

    void setRangeStage(Message stage)
    {
        clearPointer();
        m_dataType = Inline8Bit;
        m_dataLength = 1;
        m_data.internal8bit[0] = stage;
    }

private:
    enum Type: quint16 {
        External = 1,
        Inline8Bit = 8,
        External8Bit = Inline8Bit | External,
        Inline16Bit = 16,
        External16Bit = Inline16Bit | External,
        Inline32Bit = 32,
        External32Bit = Inline32Bit | External,
        Inline64Bit = 64,
        External64Bit = Inline64Bit | External
    };

    Type m_dataType = Inline8Bit;
    quint16 m_dataLength = 0;

    static const int s_internalDataLength = 8;
    union {
        void  *external;
        char   internalChar [s_internalDataLength];
        qint8  internal8bit [s_internalDataLength];
        qint16 internal16bit[s_internalDataLength / 2];
        qint32 internal32bit[s_internalDataLength / 4];
        qint64 internal64bit[s_internalDataLength / 8];
    } m_data;

    void assignData(const QmlEvent &other)
    {
        if (m_dataType & External) {
            size_t length = m_dataLength * (other.m_dataType / 8);
            m_data.external = malloc(length);
            memcpy(m_data.external, other.m_data.external, length);
        } else {
            memcpy(&m_data, &other.m_data, sizeof(m_data));
        }
    }

    template<typename Big, typename Small>
    bool squeezable(Big source)
    {
        return static_cast<Small>(source) == source;
    }

    template<typename Container, typename Number>
    typename std::enable_if<(sizeof(Number) > 1), bool>::type
    squeeze(const Container &numbers)
    {
        using Small = typename QIntegerForSize<sizeof(Number) / 2>::Signed;
        for (Number item : numbers) {
            if (!squeezable<Number, Small>(item))
                return false;
        }
        assignNumbers<Container, Small>(numbers);
        return true;
    }

    template<typename Container, typename Number>
    typename std::enable_if<(sizeof(Number) <= 1), bool>::type
    squeeze(const Container &)
    {
        return false;
    }

    template<typename Container, typename Number>
    void assignNumbers(const Container &numbers)
    {
        Number *data;
        const auto size = numbers.size();
        m_dataLength = squeezable<decltype(size), quint16>(size) ?
                    static_cast<quint16>(size) : std::numeric_limits<quint16>::max();
        if (m_dataLength > sizeof(m_data) / sizeof(Number)) {
            if (squeeze<Container, Number>(numbers))
                return;
            m_dataType = static_cast<Type>((sizeof(Number) * 8) | External);
            m_data.external = malloc(m_dataLength * sizeof(Number));
            data = static_cast<Number *>(m_data.external);
        } else {
            m_dataType = static_cast<Type>(sizeof(Number) * 8);
            data = static_cast<Number *>(m_dataType & External ? m_data.external : &m_data);
        }
        quint16 i = 0;
        for (Number item : numbers) {
            if (i >= m_dataLength)
                break;
            data[i++] = item;
        }
    }

    void clearPointer()
    {
        if (m_dataType & External)
            free(m_data.external);
    }

    friend QDataStream &operator>>(QDataStream &stream, QmlEvent &event);
    friend QDataStream &operator<<(QDataStream &stream, const QmlEvent &event);
};

QDataStream &operator>>(QDataStream &stream, QmlEvent &event);
QDataStream &operator<<(QDataStream &stream, const QmlEvent &event);

} // namespace QmlProfiler

Q_DECLARE_METATYPE(QmlProfiler::QmlEvent)

QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(QmlProfiler::QmlEvent, Q_MOVABLE_TYPE);
QT_END_NAMESPACE

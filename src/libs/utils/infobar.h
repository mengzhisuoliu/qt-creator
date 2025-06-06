// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "utils_global.h"

#include "id.h"
#include "infolabel.h"
#include "qtcsettings.h"

#include <QObject>
#include <QSet>
#include <QVariant>

#include <functional>

QT_BEGIN_NAMESPACE
class QBoxLayout;
class QSettings;
QT_END_NAMESPACE

namespace Utils {

class Icon;
class InfoBar;
class InfoBarDisplay;
class Theme;

class QTCREATOR_UTILS_EXPORT InfoBarEntry
{
public:
    enum class GlobalSuppression
    {
        Disabled,
        Enabled
    };
    enum class ButtonAction
    {
        None,
        Hide,
        Suppress,
        SuppressPersistently
    };

    InfoBarEntry() = default;
    InfoBarEntry(Id _id, const QString &_infoText, GlobalSuppression _globalSuppression = GlobalSuppression::Disabled);

    Id id() const;
    QString text() const;
    QString title() const;
    GlobalSuppression globalSuppression() const;

    void setTitle(const QString &title);

    using CallBack = std::function<void()>;
    struct Button
    {
        QString text;
        CallBack callback;
        QString tooltip;
        ButtonAction action = ButtonAction::None;
        bool enabled = true;
    };
    void addCustomButton(const QString &_buttonText, CallBack callBack, const QString &tooltip = {},
                         ButtonAction action = ButtonAction::None, bool enabled = true);
    void setCancelButtonInfo(CallBack callBack);
    void setCancelButtonInfo(const QString &_cancelButtonText, CallBack callBack);
    void removeCancelButton();
    QList<Button> buttons() const;
    bool hasCancelButton() const;
    CallBack cancelButtonCallback() const;
    QString cancelButtonText() const;

    struct ComboInfo
    {
        QString displayText;
        QVariant data;
    };
    using ComboCallBack = std::function<void(const ComboInfo &)>;
    struct Combo
    {
        ComboCallBack callback;
        QList<ComboInfo> entries;
        QString tooltip;
        int currentIndex = -1;
    };
    void setComboInfo(
        const QStringList &list,
        ComboCallBack callBack,
        const QString &tooltip = {},
        int currentIndex = -1);
    void setComboInfo(const QList<ComboInfo> &infos, ComboCallBack callBack, const QString &tooltip = {}, int currentIndex = -1);
    Combo combo() const;

    using DetailsWidgetCreator = std::function<QWidget*()>;
    void setDetailsWidgetCreator(const DetailsWidgetCreator &creator);
    DetailsWidgetCreator detailsWidgetCreator() const;

    void setInfoType(InfoLabel::InfoType infoType);
    InfoLabel::InfoType infoType() const;

    static const Icon &icon(InfoLabel::InfoType infoType);

private:
    Id m_id;
    QString m_infoText;
    QString m_title;
    InfoLabel::InfoType m_infoType = InfoLabel::None;
    QList<Button> m_buttons;
    QString m_cancelButtonText;
    CallBack m_cancelButtonCallBack;
    GlobalSuppression m_globalSuppression = GlobalSuppression::Disabled;
    DetailsWidgetCreator m_detailsWidgetCreator;
    bool m_useCancelButton = true;
    Combo m_combo;
};

class QTCREATOR_UTILS_EXPORT InfoBar : public QObject
{
    Q_OBJECT

public:
    void addInfo(const InfoBarEntry &info);
    void removeInfo(Id id);
    bool containsInfo(Id id) const;
    void suppressInfo(Id id);
    bool canInfoBeAdded(Id id) const;
    void unsuppressInfo(Id id);
    void clear();
    static void globallySuppressInfo(Id id);
    static void globallyUnsuppressInfo(Id id);
    static void clearGloballySuppressed();
    static bool anyGloballySuppressed();

    static void initialize(QtcSettings *settings);
    static QtcSettings *settings();

    QList<InfoBarEntry> entries() const;

    // for InfoBarDisplay implementations
    void triggerButton(const Id &entryId, const InfoBarEntry::Button &button);

signals:
    void changed();

private:
    static void writeGloballySuppressedToSettings();

private:
    QList<InfoBarEntry> m_infoBarEntries;
    QSet<Id> m_suppressed;

    static QSet<Id> globallySuppressed;
    static QtcSettings *m_settings;
};

class QTCREATOR_UTILS_EXPORT InfoBarDisplay : public QObject
{
public:
    InfoBarDisplay(QObject *parent = nullptr);
    void setTarget(QBoxLayout *layout, int index);
    void setInfoBar(InfoBar *infoBar);
    void setEdge(Qt::Edge edge);

    InfoBar *infoBar() const;

private:
    void update();
    void infoBarDestroyed();

    QList<QWidget *> m_infoWidgets;
    InfoBar *m_infoBar = nullptr;
    QBoxLayout *m_boxLayout = nullptr;
    Qt::Edge m_edge = Qt::TopEdge;
    int m_boxIndex = 0;
    bool m_isShowingDetailsWidget = false;
};

} // namespace Utils

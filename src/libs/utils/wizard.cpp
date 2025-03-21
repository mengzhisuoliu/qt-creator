// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "wizard.h"

#include "algorithm.h"
#include "guiutils.h"
#include "hostosinfo.h"
#include "icon.h"
#include "qtcassert.h"
#include "theme/theme.h"
#include "utilstr.h"
#include "wizardpage.h"

#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QScrollArea>
#include <QVariant>
#include <QVBoxLayout>


/*!
  \class Utils::Wizard
  \inmodule QtCreator

  \brief The Wizard class implements a wizard with a progress bar on the left.

  Informs the user about the progress.
*/

namespace Utils {

class ProgressItemWidget : public QWidget
{
    Q_OBJECT
public:
    ProgressItemWidget(const QPixmap &indicatorPixmap, const QString &title, QWidget *parent = nullptr)
        : QWidget(parent),
        m_indicatorVisible(false),
        m_indicatorPixmap(indicatorPixmap)
    {
        m_indicatorLabel = new QLabel(this);
        const QSizeF indicatorSize = m_indicatorPixmap.deviceIndependentSize();
        m_indicatorLabel->setFixedSize(
            {qCeil(indicatorSize.width()), qCeil(indicatorSize.height())});
        m_titleLabel = new QLabel(title, this);
        auto l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(m_indicatorLabel);
        l->addWidget(m_titleLabel);
    }
    void setIndicatorVisible(bool visible) {
        if (m_indicatorVisible == visible)
            return;
        m_indicatorVisible = visible;
        if (m_indicatorVisible)
            m_indicatorLabel->setPixmap(m_indicatorPixmap);
        else
            m_indicatorLabel->setPixmap(QPixmap());
    }
    void setTitle(const QString &title) {
        m_titleLabel->setText(title);
    }
    void setWordWrap(bool wrap) {
        m_titleLabel->setWordWrap(wrap);
    }

private:
    bool m_indicatorVisible;
    QPixmap m_indicatorPixmap;
    QLabel *m_indicatorLabel;
    QLabel *m_titleLabel;
};

class LinearProgressWidget : public QWidget
{
    Q_OBJECT
public:
    LinearProgressWidget(WizardProgress *progress, QWidget *parent = nullptr);

private:
    void slotItemAdded(WizardProgressItem *item);
    void slotItemRemoved(WizardProgressItem *item);
    void slotItemChanged(WizardProgressItem *item);
    void slotNextItemsChanged(WizardProgressItem *item, const QList<WizardProgressItem *> &nextItems);
    void slotNextShownItemChanged(WizardProgressItem *item, WizardProgressItem *nextItem);
    void slotStartItemChanged(WizardProgressItem *item);
    void slotCurrentItemChanged(WizardProgressItem *item);
    void recreateLayout();
    void updateProgress();
    void disableUpdates();
    void enableUpdates();

    QVBoxLayout *m_mainLayout;
    QVBoxLayout *m_itemWidgetLayout;
    WizardProgress *m_wizardProgress;
    QHash<WizardProgressItem *, ProgressItemWidget *> m_itemToItemWidget;
    QList<WizardProgressItem *> m_visibleItems;
    ProgressItemWidget *m_dotsItemWidget;
    int m_disableUpdatesCount;
    QPixmap m_indicatorPixmap;
};

LinearProgressWidget::LinearProgressWidget(WizardProgress *progress, QWidget *parent)
    :
    QWidget(parent),
    m_dotsItemWidget(nullptr),
    m_disableUpdatesCount(0)
{
    m_indicatorPixmap = Icon::fromTheme("go-next").pixmap(16);
    m_wizardProgress = progress;
    m_mainLayout = new QVBoxLayout(this);
    m_itemWidgetLayout = new QVBoxLayout();
    auto spacer = new QSpacerItem(0, 0, QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_mainLayout->addLayout(m_itemWidgetLayout);
    m_mainLayout->addSpacerItem(spacer);

    m_dotsItemWidget = new ProgressItemWidget(m_indicatorPixmap, Tr::tr("..."), this);
    m_dotsItemWidget->setVisible(false);
    m_dotsItemWidget->setEnabled(false);

    connect(m_wizardProgress, &WizardProgress::itemAdded,
            this, &LinearProgressWidget::slotItemAdded);
    connect(m_wizardProgress, &WizardProgress::itemRemoved,
            this, &LinearProgressWidget::slotItemRemoved);
    connect(m_wizardProgress, &WizardProgress::itemChanged,
            this, &LinearProgressWidget::slotItemChanged);
    connect(m_wizardProgress, &WizardProgress::nextItemsChanged,
            this, &LinearProgressWidget::slotNextItemsChanged);
    connect(m_wizardProgress, &WizardProgress::nextShownItemChanged,
            this, &LinearProgressWidget::slotNextShownItemChanged);
    connect(m_wizardProgress, &WizardProgress::startItemChanged,
            this, &LinearProgressWidget::slotStartItemChanged);
    connect(m_wizardProgress, &WizardProgress::currentItemChanged,
            this, &LinearProgressWidget::slotCurrentItemChanged);

    const QSet<WizardProgressItem *> items = m_wizardProgress->items();
    for (WizardProgressItem *item : items)
        slotItemAdded(item);
    recreateLayout();

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void LinearProgressWidget::slotItemAdded(WizardProgressItem *item)
{
    ProgressItemWidget *itemWidget = new ProgressItemWidget(m_indicatorPixmap, item->title(), this);
    itemWidget->setVisible(false);
    itemWidget->setWordWrap(item->titleWordWrap());
    m_itemToItemWidget.insert(item, itemWidget);
}

void LinearProgressWidget::slotItemRemoved(WizardProgressItem *item)
{
    const auto it = m_itemToItemWidget.constFind(item);
    if (it == m_itemToItemWidget.constEnd())
        return;

    delete *it;
    m_itemToItemWidget.erase(it);
    recreateLayout();
}

void LinearProgressWidget::slotItemChanged(WizardProgressItem *item)
{
    ProgressItemWidget *itemWidget = m_itemToItemWidget.value(item);
    if (!itemWidget)
        return;

    itemWidget->setTitle(item->title());
    itemWidget->setWordWrap(item->titleWordWrap());
}

void LinearProgressWidget::slotNextItemsChanged(WizardProgressItem *item, const QList<WizardProgressItem *> &nextItems)
{
    Q_UNUSED(nextItems)
    if (m_visibleItems.contains(item))
        recreateLayout();
}

void LinearProgressWidget::slotNextShownItemChanged(WizardProgressItem *item, WizardProgressItem *nextItem)
{
    Q_UNUSED(nextItem)
    if (m_visibleItems.contains(item))
        recreateLayout();
}

void LinearProgressWidget::slotStartItemChanged(WizardProgressItem *item)
{
    Q_UNUSED(item)
    recreateLayout();
}

void LinearProgressWidget::slotCurrentItemChanged(WizardProgressItem *item)
{
    Q_UNUSED(item)
    if (m_wizardProgress->directlyReachableItems() == m_visibleItems)
        updateProgress();
    else
        recreateLayout();
}

void LinearProgressWidget::recreateLayout()
{
    disableUpdates();

    for (ProgressItemWidget *itemWidget : std::as_const(m_itemToItemWidget))
        itemWidget->setVisible(false);

    m_dotsItemWidget->setVisible(false);

    for (int i = m_itemWidgetLayout->count() - 1; i >= 0; --i) {
        QLayoutItem *item = m_itemWidgetLayout->takeAt(i);
        delete item;
    }

    m_visibleItems = m_wizardProgress->directlyReachableItems();
    for (WizardProgressItem *progressItem : std::as_const(m_visibleItems)) {
        ProgressItemWidget *itemWidget = m_itemToItemWidget.value(progressItem);
        m_itemWidgetLayout->addWidget(itemWidget);
        itemWidget->setVisible(true);
    }

    if (!m_wizardProgress->isFinalItemDirectlyReachable()) {
        m_itemWidgetLayout->addWidget(m_dotsItemWidget);
        m_dotsItemWidget->setVisible(true);
    }

    enableUpdates();
    updateProgress();
}

void LinearProgressWidget::updateProgress()
{
    disableUpdates();

    QList<WizardProgressItem *> visitedItems = m_wizardProgress->visitedItems();

    for (auto it = m_itemToItemWidget.cbegin(); it != m_itemToItemWidget.cend(); ++it) {
        WizardProgressItem *item = it.key();
        ProgressItemWidget *itemWidget = it.value();
        itemWidget->setEnabled(visitedItems.contains(item));
        itemWidget->setIndicatorVisible(false);
    }

    WizardProgressItem *currentItem = m_wizardProgress->currentItem();
    ProgressItemWidget *currentItemWidget = m_itemToItemWidget.value(currentItem);
    if (currentItemWidget)
        currentItemWidget->setIndicatorVisible(true);

    enableUpdates();
}

void LinearProgressWidget::disableUpdates()
{
    if (m_disableUpdatesCount++ == 0) {
        setUpdatesEnabled(false);
        hide();
    }
}

void LinearProgressWidget::enableUpdates()
{
    if (--m_disableUpdatesCount == 0) {
        show();
        setUpdatesEnabled(true);
    }
}

class WizardPrivate
{
public:
    bool m_automaticProgressCreation = true;
    WizardProgress *m_wizardProgress = nullptr;
    QSet<QString> m_fieldNames;
    bool m_skipForSubproject = false;
};

Wizard::Wizard(Qt::WindowFlags flags)
    : QWizard(dialogParent(), flags), d_ptr(new WizardPrivate)
{
    d_ptr->m_wizardProgress = new WizardProgress(this);
    connect(this, &QWizard::currentIdChanged, this, &Wizard::_q_currentPageChanged);
    connect(this, &QWizard::pageAdded, this, &Wizard::_q_pageAdded);
    connect(this, &QWizard::pageRemoved, this, &Wizard::_q_pageRemoved);
    setSideWidget(new LinearProgressWidget(d_ptr->m_wizardProgress, this));
    setOption(QWizard::NoCancelButton, false);
    setOption(QWizard::NoDefaultButton, false);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    if (!creatorTheme()->preferredStyles().isEmpty())
        setWizardStyle(QWizard::ModernStyle);

    if (HostOsInfo::isMacHost()) {
        setButtonLayout(QList<QWizard::WizardButton>()
                        << QWizard::CancelButton
                        << QWizard::Stretch
                        << QWizard::BackButton
                        << QWizard::NextButton
                        << QWizard::CommitButton
                        << QWizard::FinishButton);
    }
}

Wizard::~Wizard()
{
    delete d_ptr;
}

bool Wizard::isAutomaticProgressCreationEnabled() const
{
    Q_D(const Wizard);

    return d->m_automaticProgressCreation;
}

void Wizard::setAutomaticProgressCreationEnabled(bool enabled)
{
    Q_D(Wizard);

    d->m_automaticProgressCreation = enabled;
}

void Wizard::setStartId(int pageId)
{
    Q_D(Wizard);

    QWizard::setStartId(pageId);
    d->m_wizardProgress->setStartPage(startId());
}

WizardProgress *Wizard::wizardProgress() const
{
    Q_D(const Wizard);

    return d->m_wizardProgress;
}

bool Wizard::hasField(const QString &name) const
{
    return d_ptr->m_fieldNames.contains(name);
}

void Wizard::registerFieldName(const QString &name)
{
    QTC_ASSERT(!hasField(name), return);
    d_ptr->m_fieldNames.insert(name);
}

QSet<QString> Wizard::fieldNames() const
{
    return d_ptr->m_fieldNames;
}

QHash<QString, QVariant> Wizard::variables() const
{
    QHash<QString, QVariant> result;
    const QSet<QString> fields = fieldNames();
    for (const QString &f : fields)
        result.insert(f, field(f));
    return result;
}

QString typeOf(const QVariant &v)
{
    QString result;
    switch (v.typeId()) {
    case QMetaType::QVariantMap:
        result = QLatin1String("Object");
        break;
    default:
        result = QLatin1String(v.typeName());
    }
    return result;
}

void Wizard::showVariables()
{
    QString result = QLatin1String("<table>\n  <tr><td>Key</td><td>Type</td><td>Value</td><td>Eval</td></tr>\n");
    QHash<QString, QVariant> vars = variables();
    const QList<QString> keys = sorted(vars.keys());
    for (const QString &key : keys) {
        const QVariant &v = vars.value(key);
        result += QLatin1String("  <tr><td>")
                + key + QLatin1String("</td><td>")
                + typeOf(v) + QLatin1String("</td><td>")
                + stringify(v) + QLatin1String("</td><td>")
                + evaluate(v) + QLatin1String("</td></tr>\n");
    }

    result += QLatin1String("</table>");

    auto dialog = new QDialog(this);
    dialog->setMinimumSize(800, 600);
    auto layout = new QVBoxLayout(dialog);
    auto scrollArea = new QScrollArea;
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal);

    auto label = new QLabel(result);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    scrollArea->setWidget(label);
    scrollArea->setWidgetResizable(true);

    layout->addWidget(scrollArea);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(dialog, &QDialog::finished, dialog, &QObject::deleteLater);

    dialog->show();
}

QString Wizard::stringify(const QVariant &v) const
{
    return v.toString();
}

QString Wizard::evaluate(const QVariant &v) const
{
    return stringify(v);
}

bool Wizard::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape && !ke->modifiers()) {
            ke->accept();
            return true;
        }
    }
    return QWizard::event(event);
}

void Wizard::_q_currentPageChanged(int pageId)
{
    Q_D(Wizard);

    d->m_wizardProgress->setCurrentPage(pageId);
}

void Wizard::_q_pageAdded(int pageId)
{
    Q_D(Wizard);

    QWizardPage *p = page(pageId);
    auto wp = qobject_cast<WizardPage *>(p);
    if (wp)
        wp->pageWasAdded();

    if (!d->m_automaticProgressCreation)
        return;

    QVariant property = p->property(SHORT_TITLE_PROPERTY);
    const QString title = property.isNull() ? p->title() : property.toString();
    WizardProgressItem *item = d->m_wizardProgress->addItem(title);
    item->addPage(pageId);
    d->m_wizardProgress->setStartPage(startId());
    if (!d->m_wizardProgress->startItem())
        return;

    QList<int> pages = pageIds();
    int index = pages.indexOf(pageId);
    int prevId = -1;
    int nextId = -1;
    if (index > 0)
        prevId = pages.at(index - 1);
    if (index < pages.count() - 1)
        nextId = pages.at(index + 1);

    WizardProgressItem *prevItem = nullptr;
    WizardProgressItem *nextItem = nullptr;

    if (prevId >= 0)
        prevItem = d->m_wizardProgress->item(prevId);
    if (nextId >= 0)
        nextItem = d->m_wizardProgress->item(nextId);

    if (prevItem)
        prevItem->setNextItems({item});
    if (nextItem)
        item->setNextItems({nextItem});
}

void Wizard::_q_pageRemoved(int pageId)
{
    Q_D(Wizard);

    if (!d->m_automaticProgressCreation)
        return;

    WizardProgressItem *item = d->m_wizardProgress->item(pageId);
    d->m_wizardProgress->removePage(pageId);
    d->m_wizardProgress->setStartPage(startId());

    if (!item->pages().isEmpty())
        return;

    QList<int> pages = pageIds();
    int index = pages.indexOf(pageId);
    int prevId = -1;
    int nextId = -1;
    if (index > 0)
        prevId = pages.at(index - 1);
    if (index < pages.count() - 1)
        nextId = pages.at(index + 1);

    WizardProgressItem *prevItem = nullptr;
    WizardProgressItem *nextItem = nullptr;

    if (prevId >= 0)
        prevItem = d->m_wizardProgress->item(prevId);
    if (nextId >= 0)
        nextItem = d->m_wizardProgress->item(nextId);

    if (prevItem && nextItem) {
        QList<WizardProgressItem *> nextItems = prevItem->nextItems();
        nextItems.removeOne(item);
        if (!nextItems.contains(nextItem))
            nextItems.append(nextItem);
        prevItem->setNextItems(nextItems);
    }
    d->m_wizardProgress->removeItem(item);
}

void Wizard::setSkipForSubprojects(bool skip)
{
    Q_D(Wizard);
    d->m_skipForSubproject = skip;
}

int Wizard::nextId() const
{
    Q_D(const Wizard);
    if (!d->m_skipForSubproject)
        return QWizard::nextId();

    const QList<int> allIds = pageIds();
    int index = allIds.indexOf(currentId());
    QTC_ASSERT(index > -1, return QWizard::nextId());

    while (++index < allIds.size()) {
        if (auto wp = qobject_cast<WizardPage *>(page(index))) {
            if (!wp->skipForSubprojects())
                return index;
        }
    }
    return QWizard::nextId();
}

class WizardProgressPrivate
{
    WizardProgress *q_ptr;
    Q_DECLARE_PUBLIC(WizardProgress)

public:
    WizardProgressPrivate() = default;

    static bool isNextItem(WizardProgressItem *item, WizardProgressItem *nextItem);
    // if multiple paths are possible the empty list is returned
    QList<WizardProgressItem *> singlePathBetween(WizardProgressItem *fromItem, WizardProgressItem *toItem) const;
    void updateReachableItems();

    QMap<int, WizardProgressItem *> m_pageToItem;
    QSet<WizardProgressItem *> m_items;

    QList<WizardProgressItem *> m_visitedItems;
    QList<WizardProgressItem *> m_reachableItems;

    WizardProgressItem *m_currentItem = nullptr;
    WizardProgressItem *m_startItem = nullptr;
};

inline QDebug &operator<<(QDebug &debug, const WizardProgressPrivate &progress)
{
    debug << "items:" << progress.m_items.size()
          << "; visited:" << progress.m_visitedItems.size()
          << "; reachable:" << progress.m_reachableItems.size();

    return debug;
}

QDebug &operator<<(QDebug &debug, const WizardProgress &progress)
{
    debug << "WizardProgress{_: " << *progress.d_ptr << "}";
    return debug;
}

class WizardProgressItemPrivate
{
    WizardProgressItem *q_ptr;
    Q_DECLARE_PUBLIC(WizardProgressItem)
public:
    QString m_title;
    bool m_titleWordWrap;
    WizardProgress *m_wizardProgress;
    QList<int> m_pages;
    QList<WizardProgressItem *> m_nextItems;
    QList<WizardProgressItem *> m_prevItems;
    WizardProgressItem *m_nextShownItem;
};

inline QDebug &operator<<(QDebug &debug, const WizardProgressItemPrivate &item)
{
    debug << "title:" << item.m_title
          << "; word wrap:" << item.m_titleWordWrap
          << "; progress:" << *item.m_wizardProgress
          << "; pages:" << item.m_pages;

    return debug;
}

QDebug &operator<<(QDebug &debug, const WizardProgressItem &item)
{
    debug << "WizardProgressItem{_: " << *item.d_ptr << "}";
    return debug;
}

bool WizardProgressPrivate::isNextItem(WizardProgressItem *item, WizardProgressItem *nextItem)
{
    QHash<WizardProgressItem *, bool> visitedItems;
    QList<WizardProgressItem *> workingItems = item->nextItems();
    while (!workingItems.isEmpty()) {
        WizardProgressItem *workingItem = workingItems.takeFirst();
        if (workingItem == nextItem)
            return true;
        if (visitedItems.contains(workingItem))
            continue;
        visitedItems.insert(workingItem, true);
        workingItems += workingItem->nextItems();
    }
    return false;
}

QList<WizardProgressItem *> WizardProgressPrivate::singlePathBetween(WizardProgressItem *fromItem, WizardProgressItem *toItem) const
{
    WizardProgressItem *item = fromItem;
    if (!item)
        item = m_startItem;
    if (!item)
        return {};

    // Optimization. It is workaround for case A->B, B->C, A->C where "from" is A and "to" is C.
    // When we had X->A in addition and "from" was X and "to" was C, this would not work
    // (it should return the shortest path which would be X->A->C).
    if (item->nextItems().contains(toItem))
        return {toItem};

    QHash<WizardProgressItem *, QHash<WizardProgressItem *, bool>> visitedItemsToParents;
    QList<QPair<WizardProgressItem *, WizardProgressItem *>> workingItems; // next to prev item

    const QList<WizardProgressItem *> items = item->nextItems();
    for (WizardProgressItem *i : items)
        workingItems.push_back({i, item});

    while (!workingItems.isEmpty()) {
        QPair<WizardProgressItem *, WizardProgressItem *> workingItem = workingItems.takeFirst();

        QHash<WizardProgressItem *, bool> &parents = visitedItemsToParents[workingItem.first];
        parents.insert(workingItem.second, true);
        if (parents.count() > 1)
            continue;

        const QList<WizardProgressItem *> items = workingItem.first->nextItems();
        for (WizardProgressItem *i : items)
            workingItems.push_back({i, workingItem.first});
    }

    QList<WizardProgressItem *> path;

    WizardProgressItem *it = toItem;
    auto itItem = visitedItemsToParents.constFind(it);
    const auto itEnd = visitedItemsToParents.constEnd();
    while (itItem != itEnd) {
        path.prepend(itItem.key());
        if (itItem.value().count() != 1)
            return {};
        it = itItem.value().constBegin().key();
        if (it == item)
            return path;
        itItem = visitedItemsToParents.constFind(it);
    }
    return {};
}

void WizardProgressPrivate::updateReachableItems()
{
    m_reachableItems = m_visitedItems;
    WizardProgressItem *item = nullptr;
    if (m_visitedItems.count() > 0)
        item = m_visitedItems.last();
    if (!item) {
        item = m_startItem;
        m_reachableItems.append(item);
    }
    if (!item)
        return;
    while (item->nextShownItem()) {
        item = item->nextShownItem();
        m_reachableItems.append(item);
    }
}


WizardProgress::WizardProgress(QObject *parent)
    : QObject(parent), d_ptr(new WizardProgressPrivate)
{
    d_ptr->q_ptr = this;
}

WizardProgress::~WizardProgress()
{
    Q_D(WizardProgress);

    for (WizardProgressItem *item : std::as_const(d->m_items))
        delete item;
    delete d_ptr;
}

WizardProgressItem *WizardProgress::addItem(const QString &title)
{
    Q_D(WizardProgress);

    auto item = new WizardProgressItem(this, title);
    d->m_items.insert(item);
    emit itemAdded(item);
    return item;
}

void WizardProgress::removeItem(WizardProgressItem *item)
{
    Q_D(WizardProgress);

    const auto it = d->m_items.constFind(item);
    if (it == d->m_items.constEnd()) {
        qWarning("WizardProgress::removePage: Item is not a part of the wizard");
        return;
    }

    // remove item from prev items
    QList<WizardProgressItem *> prevItems = item->d_ptr->m_prevItems;
    for (int i = 0; i < prevItems.count(); i++) {
        WizardProgressItem *prevItem = prevItems.at(i);
        prevItem->d_ptr->m_nextItems.removeOne(item);
    }

    // remove item from next items
    QList<WizardProgressItem *> nextItems = item->d_ptr->m_nextItems;
    for (int i = 0; i < nextItems.count(); i++) {
        WizardProgressItem *nextItem = nextItems.at(i);
        nextItem->d_ptr->m_prevItems.removeOne(item);
    }

    // update history
    int idx = d->m_visitedItems.indexOf(item);
    if (idx >= 0)
        d->m_visitedItems.removeAt(idx);

    // update reachable items
    d->updateReachableItems();

    emit itemRemoved(item);

    QList<int> pages = item->pages();
    for (int i = 0; i < pages.count(); i++)
        d->m_pageToItem.remove(pages.at(i));
    d->m_items.erase(it);
    delete item;
}

void WizardProgress::removePage(int pageId)
{
    Q_D(WizardProgress);

    const auto it = d->m_pageToItem.constFind(pageId);
    if (it == d->m_pageToItem.constEnd()) {
        qWarning("WizardProgress::removePage: page is not a part of the wizard");
        return;
    }
    WizardProgressItem *item = it.value();
    d->m_pageToItem.erase(it);
    item->d_ptr->m_pages.removeOne(pageId);
}

QList<int> WizardProgress::pages(WizardProgressItem *item)
{
    return item->pages();
}

WizardProgressItem *WizardProgress::item(int pageId) const
{
    Q_D(const WizardProgress);

    return d->m_pageToItem.value(pageId);
}

WizardProgressItem *WizardProgress::currentItem() const
{
    Q_D(const WizardProgress);

    return d->m_currentItem;
}

QSet<WizardProgressItem *> WizardProgress::items() const
{
    Q_D(const WizardProgress);

    return d->m_items;
}

WizardProgressItem *WizardProgress::startItem() const
{
    Q_D(const WizardProgress);

    return d->m_startItem;
}

QList<WizardProgressItem *> WizardProgress::visitedItems() const
{
    Q_D(const WizardProgress);

    return d->m_visitedItems;
}

QList<WizardProgressItem *> WizardProgress::directlyReachableItems() const
{
    Q_D(const WizardProgress);

    return d->m_reachableItems;
}

bool WizardProgress::isFinalItemDirectlyReachable() const
{
    Q_D(const WizardProgress);

    if (d->m_reachableItems.isEmpty())
        return false;

    return d->m_reachableItems.last()->isFinalItem();
}

void WizardProgress::setCurrentPage(int pageId)
{
    Q_D(WizardProgress);

    if (pageId < 0) { // reset history
        d->m_currentItem = nullptr;
        d->m_visitedItems.clear();
        d->m_reachableItems.clear();
        d->updateReachableItems();
        return;
    }

    WizardProgressItem *item = d->m_pageToItem.value(pageId);
    if (!item) {
        qWarning("WizardProgress::setCurrentPage: page is not mapped to any wizard progress item");
        return;
    }

    if (d->m_currentItem == item) // nothing changes
        return;

    const bool currentStartItem = !d->m_currentItem && d->m_startItem && d->m_startItem == item;

    // Check if item is reachable with the provided history or with the next items.
    const QList<WizardProgressItem *> singleItemPath = d->singlePathBetween(d->m_currentItem, item);
    const int prevItemIndex = d->m_visitedItems.indexOf(item);

    if (singleItemPath.isEmpty() && prevItemIndex < 0 && !currentStartItem) {
        qWarning("WizardProgress::setCurrentPage: new current item is not directly reachable from the old current item");
        return;
    }

    // Update the history accordingly.
    if (prevItemIndex >= 0) {
        while (prevItemIndex + 1 < d->m_visitedItems.count())
            d->m_visitedItems.removeLast();
    } else {
        if ((!d->m_currentItem && d->m_startItem && !singleItemPath.isEmpty()) || currentStartItem)
            d->m_visitedItems += d->m_startItem;
        d->m_visitedItems += singleItemPath;
    }

    d->m_currentItem = item;

    // Update reachable items accordingly.
    d->updateReachableItems();

    emit currentItemChanged(item);
}

void WizardProgress::setStartPage(int pageId)
{
    Q_D(WizardProgress);

    WizardProgressItem *item = d->m_pageToItem.value(pageId);
    if (!item) {
        qWarning("WizardProgress::setStartPage: page is not mapped to any wizard progress item");
        return;
    }

    d->m_startItem = item;
    d->updateReachableItems();

    emit startItemChanged(item);
}

WizardProgressItem::WizardProgressItem(WizardProgress *progress, const QString &title)
    : d_ptr(new WizardProgressItemPrivate)
{
    d_ptr->q_ptr = this;
    d_ptr->m_title = title;
    d_ptr->m_titleWordWrap = false;
    d_ptr->m_wizardProgress = progress;
    d_ptr->m_nextShownItem = nullptr;
}

WizardProgressItem::~WizardProgressItem()
{
    delete d_ptr;
}

void WizardProgressItem::addPage(int pageId)
{
    Q_D(WizardProgressItem);

    if (d->m_wizardProgress->d_ptr->m_pageToItem.contains(pageId)) {
        qWarning("WizardProgress::addPage: Page is already added to the item");
        return;
    }
    d->m_pages.append(pageId);
    d->m_wizardProgress->d_ptr->m_pageToItem.insert(pageId, this);
}

QList<int> WizardProgressItem::pages() const
{
    Q_D(const WizardProgressItem);

    return d->m_pages;
}

void WizardProgressItem::setNextItems(const QList<WizardProgressItem *> &items)
{
    Q_D(WizardProgressItem);

    // check if we create cycle
    for (int i = 0; i < items.count(); i++) {
        WizardProgressItem *nextItem = items.at(i);
        if (nextItem == this || WizardProgressPrivate::isNextItem(nextItem, this)) {
            qWarning("WizardProgress::setNextItems: Setting one of the next items would create a cycle");
            return;
        }
    }

    if (d->m_nextItems == items) // nothing changes
        return;

    if (!items.contains(d->m_nextShownItem))
        setNextShownItem(nullptr);

    // update prev items (remove this item from the old next items)
    for (int i = 0; i < d->m_nextItems.count(); i++) {
        WizardProgressItem *nextItem = d->m_nextItems.at(i);
        nextItem->d_ptr->m_prevItems.removeOne(this);
    }

    d->m_nextItems = items;

    // update prev items (add this item to the new next items)
    for (int i = 0; i < d->m_nextItems.count(); i++) {
        WizardProgressItem *nextItem = d->m_nextItems.at(i);
        nextItem->d_ptr->m_prevItems.append(this);
    }

    d->m_wizardProgress->d_ptr->updateReachableItems();

    emit d->m_wizardProgress->nextItemsChanged(this, items);

    if (items.count() == 1)
        setNextShownItem(items.first());
}

QList<WizardProgressItem *> WizardProgressItem::nextItems() const
{
    Q_D(const WizardProgressItem);

    return d->m_nextItems;
}

void WizardProgressItem::setNextShownItem(WizardProgressItem *item)
{
    Q_D(WizardProgressItem);

    if (d->m_nextShownItem == item) // nothing changes
        return;

    if (item && !d->m_nextItems.contains(item)) // the "item" is not a one of next items
        return;

    d->m_nextShownItem = item;

    d->m_wizardProgress->d_ptr->updateReachableItems();

    emit d->m_wizardProgress->nextShownItemChanged(this, item);
}

WizardProgressItem *WizardProgressItem::nextShownItem() const
{
    Q_D(const WizardProgressItem);

    return d->m_nextShownItem;
}

bool WizardProgressItem::isFinalItem() const
{
    return nextItems().isEmpty();
}

void WizardProgressItem::setTitle(const QString &title)
{
    Q_D(WizardProgressItem);

    d->m_title = title;
    emit d->m_wizardProgress->itemChanged(this);
}

QString WizardProgressItem::title() const
{
    Q_D(const WizardProgressItem);

    return d->m_title;
}

void WizardProgressItem::setTitleWordWrap(bool wrap)
{
    Q_D(WizardProgressItem);

    d->m_titleWordWrap = wrap;
    emit d->m_wizardProgress->itemChanged(this);
}

bool WizardProgressItem::titleWordWrap() const
{
    Q_D(const WizardProgressItem);

    return d->m_titleWordWrap;
}

} // namespace Utils

#include "wizard.moc"

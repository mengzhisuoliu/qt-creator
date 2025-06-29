// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "buildstepspage.h"

#include "buildsteplist.h"
#include "projectexplorericons.h"
#include "projectexplorertr.h"

#include <coreplugin/icore.h>

#include <utils/detailswidget.h>
#include <utils/hostosinfo.h>
#include <utils/qtcassert.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Utils;

namespace ProjectExplorer {
namespace Internal {

ToolWidget::ToolWidget(QWidget *parent) : FadingPanel(parent)
{
    auto layout = new QHBoxLayout;
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    setLayout(layout);
    m_firstWidget = new FadingWidget(this);
    m_firstWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto hbox = new QHBoxLayout();
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);
    m_firstWidget->setLayout(hbox);
    QSize buttonSize(20, HostOsInfo::isMacHost() ? 20 : 26);

    m_disableButton = new QToolButton(m_firstWidget);
    m_disableButton->setAutoRaise(true);
    m_disableButton->setFixedSize(buttonSize);
    m_disableButton->setIcon(Icons::BUILDSTEP_DISABLE.icon());
    m_disableButton->setCheckable(true);
    hbox->addWidget(m_disableButton);
    layout->addWidget(m_firstWidget);

    m_secondWidget = new FadingWidget(this);
    m_secondWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    hbox = new QHBoxLayout();
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(4);
    m_secondWidget->setLayout(hbox);

    m_upButton = new QToolButton(m_secondWidget);
    m_upButton->setAutoRaise(true);
    m_upButton->setToolTip(Tr::tr("Move Up"));
    m_upButton->setFixedSize(buttonSize);
    m_upButton->setIcon(Icons::BUILDSTEP_MOVEUP.icon());
    hbox->addWidget(m_upButton);

    m_downButton = new QToolButton(m_secondWidget);
    m_downButton->setAutoRaise(true);
    m_downButton->setToolTip(Tr::tr("Move Down"));
    m_downButton->setFixedSize(buttonSize);
    m_downButton->setIcon(Icons::BUILDSTEP_MOVEDOWN.icon());
    hbox->addWidget(m_downButton);

    m_removeButton  = new QToolButton(m_secondWidget);
    m_removeButton->setAutoRaise(true);
    m_removeButton->setToolTip(Tr::tr("Remove Item"));
    m_removeButton->setFixedSize(buttonSize);
    m_removeButton->setIcon(Icons::BUILDSTEP_REMOVE.icon());
    hbox->addWidget(m_removeButton);

    layout->addWidget(m_secondWidget);

    connect(m_disableButton, &QAbstractButton::clicked, this, &ToolWidget::disabledClicked);
    connect(m_upButton, &QAbstractButton::clicked, this, &ToolWidget::upClicked);
    connect(m_downButton, &QAbstractButton::clicked, this, &ToolWidget::downClicked);
    connect(m_removeButton, &QAbstractButton::clicked, this, &ToolWidget::removeClicked);
}

void ToolWidget::setOpacity(qreal value)
{
    m_targetOpacity = value;
    if (m_buildStepEnabled)
        m_firstWidget->setOpacity(value);
    m_secondWidget->setOpacity(value);
}

void ToolWidget::fadeTo(qreal value)
{
    m_targetOpacity = value;
    if (m_buildStepEnabled)
        m_firstWidget->fadeTo(value);
    m_secondWidget->fadeTo(value);
}

void ToolWidget::setBuildStepEnabled(bool b)
{
    m_buildStepEnabled = b;
    if (m_buildStepEnabled) {
        if (HostOsInfo::isMacHost())
            m_firstWidget->setOpacity(m_targetOpacity);
        else
            m_firstWidget->fadeTo(m_targetOpacity);
    } else {
        if (HostOsInfo::isMacHost())
            m_firstWidget->setOpacity(.999);
        else
            m_firstWidget->fadeTo(.999);
    }
    m_disableButton->setChecked(!b);
    m_disableButton->setToolTip(b ? Tr::tr("Disable") : Tr::tr("Enable"));
}

void ToolWidget::setUpEnabled(bool b)
{
    m_upButton->setEnabled(b);
}

void ToolWidget::setDownEnabled(bool b)
{
    m_downButton->setEnabled(b);
}

void ToolWidget::setRemoveEnabled(bool b)
{
    m_removeButton->setEnabled(b);
}

void ToolWidget::setUpVisible(bool b)
{
    m_upButton->setVisible(b);
}

void ToolWidget::setDownVisible(bool b)
{
    m_downButton->setVisible(b);
}

BuildStepsWidgetData::BuildStepsWidgetData(BuildStep *s) :
    step(s), widget(nullptr), detailsWidget(nullptr)
{
    widget = s->doCreateConfigWidget();
    Q_ASSERT(widget);

    detailsWidget = new DetailsWidget;
    detailsWidget->setWidget(widget);

    toolWidget = new ToolWidget(detailsWidget);
    toolWidget->setBuildStepEnabled(step->stepEnabled());

    detailsWidget->setToolWidget(toolWidget);
    detailsWidget->setContentsMargins(0, 0, 0, 0);
    detailsWidget->setSummaryText(s->summaryText());
}

BuildStepsWidgetData::~BuildStepsWidgetData()
{
    delete detailsWidget; // other widgets are children of that!
    // We do not own the step
}

BuildStepListWidget::BuildStepListWidget(BuildStepList *bsl)
    : m_buildStepList(bsl)
{
    setupUi();

    //: %1 is the name returned by BuildStepList::displayName
    setWindowTitle(Tr::tr("%1 Steps").arg(bsl->displayName()));

    connect(bsl, &BuildStepList::stepInserted, this, &BuildStepListWidget::addBuildStep);
    connect(bsl, &BuildStepList::stepRemoved, this, &BuildStepListWidget::removeBuildStep);
    connect(bsl, &BuildStepList::stepMoved, this, &BuildStepListWidget::stepMoved);

    for (int i = 0; i < bsl->count(); ++i) {
        addBuildStep(i);
        // addBuilStep expands the config widget by default, which we don't want here
        if (m_buildStepsData.at(i)->step->widgetExpandedByDefault()) {
            m_buildStepsData.at(i)->detailsWidget->setState(
                        m_buildStepsData.at(i)->step->wasUserExpanded()
                        ? DetailsWidget::Expanded : DetailsWidget::Collapsed);
        }
    }

    m_noStepsLabel->setVisible(bsl->isEmpty());
    m_noStepsLabel->setText(Tr::tr("No %1 Steps").arg(m_buildStepList->displayName()));

    m_addButton->setText(Tr::tr("Add %1 Step").arg(m_buildStepList->displayName()));

    updateBuildStepButtonsState();
}

BuildStepListWidget::~BuildStepListWidget()
{
    qDeleteAll(m_buildStepsData);
    m_buildStepsData.clear();
}

void BuildStepListWidget::updateAddBuildStepMenu()
{
    QMenu *menu = m_addButton->menu();
    menu->clear();

    for (BuildStepFactory *factory : BuildStepFactory::allBuildStepFactories()) {
        if (!factory->canHandle(m_buildStepList))
            continue;

        const BuildStep::Flags flags = factory->stepFlags();
        if (flags & BuildStep::Uncreatable)
            continue;

        if ((flags & BuildStep::UniqueStep) && m_buildStepList->contains(factory->stepId()))
            continue;

        QAction *action = menu->addAction(factory->displayName());
        connect(action, &QAction::triggered, this, [factory, this] {
            BuildStep *newStep = factory->create(m_buildStepList);
            QTC_ASSERT(newStep, return);
            m_buildStepList->appendStep(newStep);
        });
    }
}

void BuildStepListWidget::addBuildStep(int pos)
{
    BuildStep *newStep = m_buildStepList->at(pos);

    // create everything
    auto s = new BuildStepsWidgetData(newStep);
    m_buildStepsData.insert(pos, s);

    m_vbox->insertWidget(pos, s->detailsWidget);

    connect(s->step, &BuildStep::updateSummary, this, [s] {
        s->detailsWidget->setSummaryText(s->step->summaryText());
    });

    connect(s->step, &BuildStep::stepEnabledChanged, this, [s] {
        s->toolWidget->setBuildStepEnabled(s->step->stepEnabled());
    });


    // Expand new build steps by default
    const bool expand = newStep->hasUserExpansionState()
            ? newStep->wasUserExpanded() : newStep->widgetExpandedByDefault();
    s->detailsWidget->setState(expand ? DetailsWidget::Expanded : DetailsWidget::OnlySummary);
    connect(s->detailsWidget, &DetailsWidget::expanded, newStep, &BuildStep::setUserExpanded);

    m_noStepsLabel->setVisible(false);
    updateBuildStepButtonsState();
}

void BuildStepListWidget::stepMoved(int from, int to)
{
    m_vbox->insertWidget(to, m_buildStepsData.at(from)->detailsWidget);

    Internal::BuildStepsWidgetData *data = m_buildStepsData.at(from);
    m_buildStepsData.removeAt(from);
    m_buildStepsData.insert(to, data);

    updateBuildStepButtonsState();
}

void BuildStepListWidget::removeBuildStep(int pos)
{
    delete m_buildStepsData.takeAt(pos);

    updateBuildStepButtonsState();

    bool hasSteps = m_buildStepList->isEmpty();
    m_noStepsLabel->setVisible(hasSteps);
}

void BuildStepListWidget::setupUi()
{
    if (m_addButton)
        return;

    m_vbox = new QVBoxLayout(this);
    m_vbox->setContentsMargins(0, 0, 0, 0);

    m_noStepsLabel = new QLabel(Tr::tr("No Build Steps"), this);
    m_noStepsLabel->setContentsMargins(0, 0, 0, 0);
    m_vbox->addWidget(m_noStepsLabel);

    auto hboxLayout = new QHBoxLayout();
    hboxLayout->setContentsMargins(0, 4, 0, 0);
    m_addButton = new QPushButton(this);
    m_addButton->setMenu(new QMenu(this));
    hboxLayout->addWidget(m_addButton);

    hboxLayout->addStretch(10);

    if (HostOsInfo::isMacHost())
        m_addButton->setAttribute(Qt::WA_MacSmallSize);

    m_vbox->addLayout(hboxLayout);

    connect(m_addButton->menu(), &QMenu::aboutToShow,
            this, &BuildStepListWidget::updateAddBuildStepMenu);
}

void BuildStepListWidget::updateBuildStepButtonsState()
{
    if (m_buildStepsData.count() != m_buildStepList->count())
        return;
    for (int i = 0; i < m_buildStepsData.count(); ++i) {
        BuildStepsWidgetData *s = m_buildStepsData.at(i);
        disconnect(s->toolWidget, nullptr, this, nullptr);
        connect(s->toolWidget, &ToolWidget::disabledClicked,
                this, [s] {
            BuildStep *bs = s->step;
            bs->setStepEnabled(!bs->stepEnabled());
            s->toolWidget->setBuildStepEnabled(bs->stepEnabled());
        });
        s->toolWidget->setRemoveEnabled(!m_buildStepList->at(i)->isImmutable());
        connect(s->toolWidget, &ToolWidget::removeClicked,
                this, [this, i] {
            if (!m_buildStepList->removeStep(i)) {
                QMessageBox::warning(Core::ICore::dialogParent(),
                                     Tr::tr("Removing Step failed"),
                                     Tr::tr("Cannot remove build step while building"),
                                     QMessageBox::Ok,
                                     QMessageBox::Ok);
            }
        });

        s->toolWidget->setUpEnabled((i > 0)
                                    && !(m_buildStepList->at(i)->isImmutable()
                                         && m_buildStepList->at(i - 1)->isImmutable()));
        connect(s->toolWidget, &ToolWidget::upClicked,
                this, [this, i] { m_buildStepList->moveStepUp(i); });
        s->toolWidget->setDownEnabled((i + 1 < m_buildStepList->count())
                                      && !(m_buildStepList->at(i)->isImmutable()
                                           && m_buildStepList->at(i + 1)->isImmutable()));
        connect(s->toolWidget, &ToolWidget::downClicked,
                this, [this, i] { m_buildStepList->moveStepUp(i + 1); });

        // Only show buttons when needed
        s->toolWidget->setDownVisible(m_buildStepList->count() != 1);
        s->toolWidget->setUpVisible(m_buildStepList->count() != 1);
    }
}

} // Internal
} // ProjectExplorer

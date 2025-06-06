// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "introductionwidget.h"
#include "welcometr.h"

#include <coreplugin/icore.h>
#include <coreplugin/modemanager.h>

#include <utils/algorithm.h>
#include <utils/checkablemessagebox.h>
#include <utils/infobar.h>
#include <utils/qtcassert.h>
#include <utils/stylehelper.h>

#include <QDesktopServices>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QVBoxLayout>

using namespace Core;
using namespace Utils;

namespace Welcome::Internal {

const char kTakeTourSetting[] = "TakeUITour";

struct Item
{
    QString pointerAnchorObjectName;
    QString title;
    QString brief;
    QString description;
};

class IntroductionWidget : public QWidget
{
public:
    IntroductionWidget(Core::ModeManager::Style previousModeStyle);

protected:
    bool event(QEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void paintEvent(QPaintEvent *ev) override;
    void keyPressEvent(QKeyEvent *ke) override;
    void mouseReleaseEvent(QMouseEvent *me) override;

private:
    void finish();
    void step();
    void setStep(uint index);
    void resizeToParent();

    QWidget *m_textWidget;
    QLabel *m_stepText;
    QLabel *m_continueLabel;
    QImage m_borderImage;
    QString m_bodyCss;
    std::vector<Item> m_items;
    QPointer<QWidget> m_stepPointerAnchor;
    uint m_step = 0;
    Core::ModeManager::Style m_previousModeStyle;
};

IntroductionWidget::IntroductionWidget(Core::ModeManager::Style previousModeStyle)
    : QWidget(ICore::dialogParent()),
      m_borderImage(":/welcome/images/border.png"),
      m_previousModeStyle(previousModeStyle)
{
    Core::ModeManager::setModeStyle(Core::ModeManager::Style::IconsAndText);

    setFocusPolicy(Qt::StrongFocus);
    setFocus();
    parentWidget()->installEventFilter(this);

    QPalette p = palette();
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    setPalette(p);

    m_textWidget = new QWidget(this);
    auto layout = new QVBoxLayout;
    m_textWidget->setLayout(layout);

    m_stepText = new QLabel(this);
    m_stepText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_stepText->setWordWrap(true);
    m_stepText->setTextFormat(Qt::RichText);
    // why is palette not inherited???
    m_stepText->setPalette(palette());
    connect(m_stepText, &QLabel::linkActivated, this, [this](const QString &link) {
        // clicking the User Interface link should both open the documentation page
        // and step forward (=end the tour)
        step();
        QDesktopServices::openUrl(link);
    });
    layout->addWidget(m_stepText);

    m_continueLabel = new QLabel(this);
    m_continueLabel->setAlignment(Qt::AlignCenter);
    m_continueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_continueLabel->setWordWrap(true);
    m_continueLabel->setFont(StyleHelper::uiFont(StyleHelper::UiElementH4));
    m_continueLabel->setPalette(palette());
    layout->addWidget(m_continueLabel);
    m_bodyCss = "font-size: 16px;";
    m_items = {
        {QLatin1String("ModeSelector"),
         Tr::tr("Mode Selector"),
         Tr::tr("Select different modes depending on the task at hand."),
         Tr::tr("<p style=\"margin-top: 30px\"><table>"
                "<tr><td style=\"padding-right: 20px\">Welcome:</td><td>Open examples, tutorials, and "
                "recent sessions and projects.</td></tr>"
                "<tr><td>Edit:</td><td>Work with code and navigate your project.</td></tr>"
                "<tr><td>Design:</td><td>Visually edit Widget-based user interfaces, state charts and UML models.</td></tr>"
                "<tr><td>Debug:</td><td>Analyze your application with a debugger or other "
                "analyzers.</td></tr>"
                "<tr><td>Projects:</td><td>Manage project settings.</td></tr>"
                "<tr><td>Help:</td><td>Browse the help database.</td></tr>"
                "</table></p>")},
        {QLatin1String("KitSelector.Button"),
         Tr::tr("Kit Selector"),
         Tr::tr("Select the active project or project configuration."),
         {}},
        {QLatin1String("Run.Button"),
         Tr::tr("Run Button"),
         Tr::tr("Run the active project. By default this builds the project first."),
         {}},
        {QLatin1String("Debug.Button"),
         Tr::tr("Debug Button"),
         Tr::tr("Run the active project in a debugger."),
         {}},
        {QLatin1String("Build.Button"), Tr::tr("Build Button"), Tr::tr("Build the active project."), {}},
        {QLatin1String("LocatorInput"),
         Tr::tr("Locator"),
         Tr::tr("Type here to open a file from any open project."),
         Tr::tr("Or:"
                "<ul>"
                "<li>type <code>c&lt;space&gt;&lt;pattern&gt;</code> to jump to a class definition</li>"
                "<li>type <code>f&lt;space&gt;&lt;pattern&gt;</code> to open a file from the file "
                "system</li>"
                "<li>click on the magnifier icon for a complete list of possible options</li>"
                "</ul>")},
        {QLatin1String("OutputPaneButtons"),
         Tr::tr("Output"),
         Tr::tr("Find compile and application output here, "
                "as well as a list of configuration and build issues, "
                "and the panel for global searches."),
         {}},
        {QLatin1String("ProgressInfo"),
         Tr::tr("Progress Indicator"),
         Tr::tr("Progress information about running tasks is shown here."),
         {}},
        {{},
         Tr::tr("Escape to Editor"),
         Tr::tr("Pressing the Escape key brings you back to the editor. Press it "
                "multiple times to also hide context help and output, giving the editor more "
                "space."),
         {}},
        {{},
         Tr::tr("The End"),
         Tr::tr("You have now completed the UI tour. To learn more about the highlighted "
                "controls, see <a style=\"color: #41CD52\" "
                "href=\"qthelp://org.qt-project.qtcreator/doc/creator-quick-tour.html\">User "
                "Interface</a>."),
         {}}};
    setStep(0);
    resizeToParent();
}

bool IntroductionWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

bool IntroductionWidget::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == parent() && ev->type() == QEvent::Resize)
        resizeToParent();
    return QWidget::eventFilter(obj, ev);
}

const int SPOTLIGHTMARGIN = 18;
const int POINTER_SIZE = 30;
const int POINTER_WIDTH = 3;

static int margin(const QRect &small, const QRect &big, Qt::Alignment side)
{
    switch (side) {
    case Qt::AlignRight:
        return qMax(0, big.right() - small.right());
    case Qt::AlignTop:
        return qMax(0, small.top() - big.top());
    case Qt::AlignBottom:
        return qMax(0, big.bottom() - small.bottom());
    case Qt::AlignLeft:
        return qMax(0, small.x() - big.x());
    default:
        QTC_ASSERT(false, return 0);
    }
}

static int oppositeMargin(const QRect &small, const QRect &big, Qt::Alignment side)
{
    switch (side) {
    case Qt::AlignRight:
        return margin(small, big, Qt::AlignLeft);
    case Qt::AlignTop:
        return margin(small, big, Qt::AlignBottom);
    case Qt::AlignBottom:
        return margin(small, big, Qt::AlignTop);
    case Qt::AlignLeft:
        return margin(small, big, Qt::AlignRight);
    default:
        QTC_ASSERT(false, return 100000);
    }
}

static const QVector<QPolygonF> pointerPolygon(const QRect &anchorRect, const QRect &fullRect)
{
    // Put the arrow opposite to the smallest margin,
    // with priority right, top, bottom, left.
    // Not very sophisticated but sufficient for current use cases.
    QVector<Qt::Alignment> alignments{Qt::AlignRight, Qt::AlignTop, Qt::AlignBottom, Qt::AlignLeft};
    Utils::sort(alignments, [anchorRect, fullRect](Qt::Alignment a, Qt::Alignment b) {
        return oppositeMargin(anchorRect, fullRect, a) < oppositeMargin(anchorRect, fullRect, b);
    });
    const auto alignIt = std::find_if(alignments.cbegin(),
                                      alignments.cend(),
                                      [anchorRect, fullRect](Qt::Alignment align) {
                                          return margin(anchorRect, fullRect, align) > POINTER_SIZE;
                                      });
    if (alignIt == alignments.cend())
        return {{}}; // no side with sufficient space found
    const qreal arrowHeadWidth = POINTER_SIZE/3.;
    if (*alignIt == Qt::AlignRight) {
        const qreal middleY = anchorRect.center().y();
        const qreal startX = anchorRect.right() + POINTER_SIZE;
        const qreal endX = anchorRect.right() + POINTER_WIDTH;
        return {{QVector<QPointF>{{startX, middleY}, {endX, middleY}}},
                QVector<QPointF>{{endX + arrowHeadWidth, middleY - arrowHeadWidth},
                                 {endX, middleY},
                                 {endX + arrowHeadWidth, middleY + arrowHeadWidth}}};
    }
    if (*alignIt == Qt::AlignTop) {
        const qreal middleX = anchorRect.center().x();
        const qreal startY = anchorRect.y() - POINTER_SIZE;
        const qreal endY = anchorRect.y() - POINTER_WIDTH;
        return {{QVector<QPointF>{{middleX, startY}, {middleX, endY}}},
                QVector<QPointF>{{middleX - arrowHeadWidth, endY - arrowHeadWidth},
                                 {middleX, endY},
                                 {middleX + arrowHeadWidth, endY - arrowHeadWidth}}};
    }
    if (*alignIt == Qt::AlignBottom) {
        const qreal middleX = anchorRect.center().x();
        const qreal startY = anchorRect.y() + POINTER_WIDTH;
        const qreal endY = anchorRect.y() + POINTER_SIZE;
        return {{QVector<QPointF>{{middleX, startY}, {middleX, endY}}},
                QVector<QPointF>{{middleX - arrowHeadWidth, endY + arrowHeadWidth},
                                 {middleX, endY},
                                 {middleX + arrowHeadWidth, endY + arrowHeadWidth}}};
    }

    // Qt::AlignLeft
    const qreal middleY = anchorRect.center().y();
    const qreal startX = anchorRect.x() - POINTER_WIDTH;
    const qreal endX = anchorRect.x() - POINTER_SIZE;
    return {{QVector<QPointF>{{startX, middleY}, {endX, middleY}}},
            QVector<QPointF>{{endX - arrowHeadWidth, middleY - arrowHeadWidth},
                             {endX, middleY},
                             {endX - arrowHeadWidth, middleY + arrowHeadWidth}}};
}

void IntroductionWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setOpacity(.87);
    const QColor backgroundColor = Qt::black;
    if (m_stepPointerAnchor) {
        const QPoint anchorPos = m_stepPointerAnchor->mapTo(parentWidget(), QPoint{0, 0});
        const QRect anchorRect(anchorPos, m_stepPointerAnchor->size());
        const QRect spotlightRect = anchorRect.adjusted(-SPOTLIGHTMARGIN,
                                                        -SPOTLIGHTMARGIN,
                                                        SPOTLIGHTMARGIN,
                                                        SPOTLIGHTMARGIN);

        // darken the background to create a spotlighted area
        if (spotlightRect.left() > 0) {
            p.fillRect(0, 0, spotlightRect.left(), height(), backgroundColor);
        }
        if (spotlightRect.top() > 0) {
            p.fillRect(spotlightRect.left(),
                       0,
                       width() - spotlightRect.left(),
                       spotlightRect.top(),
                       backgroundColor);
        }
        if (spotlightRect.right() < width() - 1) {
            p.fillRect(spotlightRect.right() + 1,
                       spotlightRect.top(),
                       width() - spotlightRect.right() - 1,
                       height() - spotlightRect.top(),
                       backgroundColor);
        }
        if (spotlightRect.bottom() < height() - 1) {
            p.fillRect(spotlightRect.left(),
                       spotlightRect.bottom() + 1,
                       spotlightRect.width(),
                       height() - spotlightRect.bottom() - 1,
                       backgroundColor);
        }

        // smooth borders of the spotlighted area by gradients
        StyleHelper::drawCornerImage(m_borderImage,
                                     &p,
                                     spotlightRect,
                                     SPOTLIGHTMARGIN,
                                     SPOTLIGHTMARGIN,
                                     SPOTLIGHTMARGIN,
                                     SPOTLIGHTMARGIN);

        // draw pointer
        const QColor qtGreen(65, 205, 82);
        p.setOpacity(1.);
        p.setPen(QPen(QBrush(qtGreen),
                      POINTER_WIDTH,
                      Qt::SolidLine,
                      Qt::RoundCap,
                      Qt::MiterJoin));
        p.setRenderHint(QPainter::Antialiasing);
        for (const QPolygonF &poly : pointerPolygon(spotlightRect, rect()))
            p.drawPolyline(poly);
    } else {
        p.fillRect(rect(), backgroundColor);
    }
}

void IntroductionWidget::keyPressEvent(QKeyEvent *ke)
{
    if (ke->key() == Qt::Key_Escape)
        finish();
    else if ((ke->modifiers()
              & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))
             == Qt::NoModifier) {
        const Qt::Key backKey = QGuiApplication::isLeftToRight() ? Qt::Key_Left : Qt::Key_Right;
        if (ke->key() == backKey) {
            if (m_step > 0)
                setStep(m_step - 1);
        } else {
            step();
        }
    }
}

void IntroductionWidget::mouseReleaseEvent(QMouseEvent *me)
{
    me->accept();
    step();
}

void IntroductionWidget::finish()
{
    Core::ModeManager::setModeStyle(m_previousModeStyle);
    hide();
    deleteLater();
}

void IntroductionWidget::step()
{
    if (m_step >= m_items.size() - 1)
        finish();
    else
        setStep(m_step + 1);
}

void IntroductionWidget::setStep(uint index)
{
    QTC_ASSERT(index < m_items.size(), return);
    m_step = index;
    m_continueLabel->setText(Tr::tr("UI Introduction %1/%2 >").arg(m_step + 1).arg(m_items.size()));
    const Item &item = m_items.at(m_step);
    m_stepText->setText("<html><body style=\"" + m_bodyCss + "\">" + "<h1>" + item.title
                        + "</h1><p>" + item.brief + "</p>" + item.description + "</body></html>");
    const QString anchorObjectName = m_items.at(m_step).pointerAnchorObjectName;
    if (!anchorObjectName.isEmpty()) {
        m_stepPointerAnchor = parentWidget()->findChild<QWidget *>(anchorObjectName);
        QTC_CHECK(m_stepPointerAnchor);
    } else {
        m_stepPointerAnchor.clear();
    }
    update();
}

void IntroductionWidget::resizeToParent()
{
    QTC_ASSERT(parentWidget(), return);
    setGeometry(QRect(QPoint(0, 0), parentWidget()->size()));
    m_textWidget->setGeometry(QRect(width()/4, height()/4, width()/2, height()/2));
}


// Public access.

void runUiTour()
{
    auto intro = new IntroductionWidget(Core::ModeManager::modeStyle());
    intro->show();
}

void askUserAboutIntroduction()
{
    InfoBar *infoBar = ICore::infoBar();

    // CheckableMessageBox for compatibility with Qt Creator < 4.11
    if (!CheckableDecider(Key(kTakeTourSetting)).shouldAskAgain()
        || !infoBar->canInfoBeAdded(kTakeTourSetting))
        return;

    InfoBarEntry
        info(kTakeTourSetting,
             Tr::tr("See where the important UI elements are and how they are used. "
                    "To take the tour later, select Help > UI Tour."),
             InfoBarEntry::GlobalSuppression::Enabled);
    info.setTitle(Tr::tr("Take a UI Tour?"));
    info.setInfoType(InfoLabel::Information);
    info.addCustomButton(
        Tr::tr("Take UI Tour"),
        [] { runUiTour(); },
        {},
        InfoBarEntry::ButtonAction::SuppressPersistently);
    infoBar->addInfo(info);
}

} //  Welcome::Internal

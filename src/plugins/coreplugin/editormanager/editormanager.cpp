// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "editormanager.h"
#include "editormanager_p.h"

#include "../actionmanager/actioncontainer.h"
#include "../actionmanager/actionmanager.h"
#include "../actionmanager/command.h"
#include "../coreconstants.h"
#include "../coreplugintr.h"
#include "../dialogs/openwithdialog.h"
#include "../dialogs/readonlyfilesdialog.h"
#include "../diffservice.h"
#include "../documentmanager.h"
#include "../fileutils.h"
#include "../findplaceholder.h"
#include "../generalsettings.h"
#include "../icore.h"
#include "../iversioncontrol.h"
#include "../locator/ilocatorfilter.h"
#include "../modemanager.h"
#include "../outputpane.h"
#include "../outputpanemanager.h"
#include "../rightpane.h"
#include "../settingsdatabase.h"
#include "../systemsettings.h"
#include "../vcsmanager.h"
#include "documentmodel.h"
#include "documentmodel_p.h"
#include "editorview.h"
#include "editorwindow.h"
#include "ieditor.h"
#include "ieditorfactory.h"
#include "ieditorfactory_p.h"
#include "openeditorsview.h"
#include "openeditorswindow.h"

#include <extensionsystem/pluginmanager.h>

#include <utils/algorithm.h>
#include <utils/checkablemessagebox.h>
#include <utils/environment.h>
#include <utils/filepath.h>
#include <utils/hostosinfo.h>
#include <utils/infobar.h>
#include <utils/link.h>
#include <utils/macroexpander.h>
#include <utils/mimeutils.h>
#include <utils/overridecursor.h>
#include <utils/qtcassert.h>
#include <utils/searchresultitem.h>
#include <utils/stringutils.h>
#include <utils/utilsicons.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#if defined(WITH_TESTS)
#include <QTest>
#endif

#include <memory>

enum { debugEditorManager=0 };

static const char kCurrentDocumentPrefix[] = "CurrentDocument";
static const char kCurrentDocumentXPos[] = "CurrentDocument:XPos";
static const char kCurrentDocumentYPos[] = "CurrentDocument:YPos";
static const char kMakeWritableWarning[] = "Core.EditorManager.MakeWritable";

static const char documentStatesKey[] = "EditorManager/DocumentStates";
static const char fileSystemCaseSensitivityKey[] = "Core/FileSystemCaseSensitivity";
static const char preferredEditorFactoriesKey[] = "EditorManager/PreferredEditorFactories";

static const char scratchBufferKey[] = "_q_emScratchBuffer";

static const int kMaxViews = 20;

using namespace Core::Internal;
using namespace Utils;

namespace Core {

static void checkEditorFlags(EditorManager::OpenEditorFlags flags)
{
    if (flags & EditorManager::OpenInOtherSplit) {
        QTC_CHECK(!(flags & EditorManager::SwitchSplitIfAlreadyVisible));
        QTC_CHECK(!(flags & EditorManager::AllowExternalEditor));
    }
}

//===================EditorManager=====================

/*!
    \class Core::EditorManagerPlaceHolder
    \inheaderfile coreplugin/editormanager/editormanager.h
    \inmodule QtCreator
    \ingroup mainclasses

    \brief The EditorManagerPlaceHolder class is used to integrate an editor
    area into a \l{Core::IMode}{mode}.

    Create an instance of EditorManagerPlaceHolder and integrate it into your
    mode widget's layout, to add the main editor area into your mode. The best
    place for the editor area is the central widget of a QMainWindow.
    Examples are the Edit and Debug modes.
*/

/*!
    Creates an EditorManagerPlaceHolder with the specified \a parent.
*/
EditorManagerPlaceHolder::EditorManagerPlaceHolder(QWidget *parent)
    : QWidget(parent)
{
    setLayout(new QVBoxLayout);
    layout()->setContentsMargins(0, 0, 0, 0);
    setFocusProxy(EditorManagerPrivate::mainEditorArea());
}

/*!
    \internal
*/
EditorManagerPlaceHolder::~EditorManagerPlaceHolder()
{
    // EditorManager will be deleted in ~MainWindow()
    QWidget *em = EditorManagerPrivate::mainEditorArea();
    if (em && em->parent() == this) {
        em->hide();
        em->setParent(nullptr);
    }
}

/*!
    \internal
*/
void EditorManagerPlaceHolder::showEvent(QShowEvent *)
{
    QWidget *previousFocus = nullptr;
    QWidget *em = EditorManagerPrivate::mainEditorArea();
    if (em->focusWidget() && em->focusWidget()->hasFocus())
        previousFocus = em->focusWidget();
    layout()->addWidget(em);
    em->show();
    if (previousFocus)
        previousFocus->setFocus();
}

// ---------------- EditorManager

/*!
    \class Core::EditorManager
    \inheaderfile coreplugin/editormanager/editormanager.h
    \inmodule QtCreator

    \brief The EditorManager class manages the editors created for files
    according to their MIME type.

    Whenever a user wants to edit or create a file, the EditorManager scans all
    IEditorFactory interfaces for suitable editors. The selected IEditorFactory
    is then asked to create an editor, as determined by the MIME type of the
    file.

    Users can split the editor view or open the editor in a new window when
    to work on and view multiple files on the same screen or on multiple
    screens. For more information, see
    \l{https://doc.qt.io/qtcreator/creator-coding-navigating.html#splitting-the-editor-view}
    {Splitting the Editor View}.

    Plugins use the EditorManager to open documents in editors or close them,
    and to get notified when documents are opened, closed or saved.
*/

/*!
    \enum Core::Internal::MakeWritableResult
    \internal

    This enum specifies whether the document has successfully been made writable.

    \value OpenedWithVersionControl
           The document was opened under version control.
    \value MadeWritable
           The document was made writable.
    \value SavedAs
           The document was saved under another name.
    \value Failed
           The document cannot be made writable.
*/

/*!
    \enum Core::EditorManager::OpenEditorFlag

    This enum specifies settings for opening a file in an editor.

    \value NoFlags
           Does not use any settings.
    \value DoNotChangeCurrentEditor
           Does not switch focus to the newly opened editor.
    \value IgnoreNavigationHistory
           Does not add an entry to the navigation history for the
           opened editor.
    \value DoNotMakeVisible
           Does not force the editor to become visible.
    \value OpenInOtherSplit
           Opens the document in another split of the window.
    \value DoNotSwitchToDesignMode
           Opens the document in the current mode.
    \value DoNotSwitchToEditMode
           Opens the document in the current mode.
    \value SwitchSplitIfAlreadyVisible
           Switches to another split if the document is already
           visible there.
    \value DoNotRaise
           Prevents raising the \QC window to the foreground.
    \value AllowExternalEditor
           Allows opening the file in an external editor.
*/

/*!
    \fn void Core::EditorManager::currentEditorChanged(Core::IEditor *editor)

    This signal is emitted after the current editor changed to \a editor.
*/

/*!
    \fn void Core::EditorManager::currentDocumentStateChanged()

    This signal is emitted when the meta data of the current document, for
    example file name or modified state, changed.

    \sa IDocument::changed()
*/

/*!
    \fn void Core::EditorManager::documentStateChanged(Core::IDocument *document)

    This signal is emitted when the meta data of the \a document, for
    example file name or modified state, changed.

    \sa IDocument::changed()
*/

/*!
    \fn void Core::EditorManager::editorCreated(Core::IEditor *editor, const Utils::FilePath &filePath)

    This signal is emitted after an \a editor was created for the file at
    \a filePath, but before it was opened in an editor view.
*/
/*!
    \fn void Core::EditorManager::editorOpened(Core::IEditor *editor)

    This signal is emitted after a new \a editor was opened in an editor view.

    Usually the more appropriate signal to listen to is documentOpened().
*/

/*!
    \fn void Core::EditorManager::documentOpened(Core::IDocument *document)

    This signal is emitted after the first editor for \a document opened in an
    editor view.
*/

/*!
    \fn void Core::EditorManager::editorAboutToClose(Core::IEditor *editor)

    This signal is emitted before \a editor is closed. This can be used to free
    resources that were allocated for the editor separately from the editor
    itself. It cannot be used to prevent the editor from closing. See
    addCloseEditorListener() for that.

    Usually the more appropriate signal to listen to is documentClosed().

    \sa addCloseEditorListener()
*/

/*!
    \fn void Core::EditorManager::editorsClosed(QList<Core::IEditor *> editors)

    This signal is emitted after the \a editors closed, but before they are
    deleted.

    Usually the more appropriate signal to listen to is documentClosed().
*/

/*!
    \fn void Core::EditorManager::documentClosed(Core::IDocument *document)

    This signal is emitted after the \a document closed, but before it is deleted.
*/
/*!
    \fn void EditorManager::findOnFileSystemRequest(const Utils::FilePath &path)

    \internal
*/
/*!
    \fn void Core::EditorManager::aboutToSave(Core::IDocument *document)

    This signal is emitted before the \a document is saved.
*/
/*!
    \fn void Core::EditorManager::saved(Core::IDocument *document)

    This signal is emitted after the \a document was saved.
*/
/*!
    \fn void Core::EditorManager::autoSaved()

    This signal is emitted after auto-save was triggered.
*/
/*!
    \fn void Core::EditorManager::currentEditorAboutToChange(Core::IEditor *editor)

    This signal is emitted before the current editor changes to \a editor.
*/

static EditorManager *m_instance = nullptr;
static EditorManagerPrivate *d;

static FilePath autoSaveName(const FilePath &filePath)
{
    return filePath.stringAppended(".autosave");
}

static void setFocusToEditorViewAndUnmaximizePanes(EditorView *view)
{
    IEditor *editor = view->currentEditor();
    QWidget *target = editor ? editor->widget() : view;
    QWidget *focus = target->focusWidget();
    QWidget *w = focus ? focus : target;

    w->setFocus();
    ICore::raiseWindow(w);

    OutputPanePlaceHolder *holder = OutputPanePlaceHolder::getCurrent();
    if (holder && holder->window() == view->window()) {
        // unmaximize output pane if necessary
        if (holder->isVisible() && holder->isMaximized())
            holder->setMaximized(false);
    }
}

namespace Internal {

static FilePath filePathFor(DocumentModel::Entry *entry, IEditor *editor = nullptr)
{
    if (entry)
        return entry->filePath();
    if (editor)
        return editor->document()->filePath();
    return {};
}

EditorManagerPrivate::EditorManagerPrivate(QObject *parent)
    : QObject(parent)
{
    d = this;
}

EditorManagerPrivate::~EditorManagerPrivate()
{
    // close all extra windows
    for (int i = 0; i < m_editorAreas.size(); ++i) {
        EditorArea *area = m_editorAreas.at(i);
        disconnect(area, &QObject::destroyed, this, &EditorManagerPrivate::editorAreaDestroyed);
        delete area;
    }
    m_editorAreas.clear();

    DocumentModel::destroy();
    d = nullptr;
}

static void openDocumentByIdx(int idx)
{
    DocumentModel::Entry *entry = DocumentModel::entryAtRow(idx + 1);
    if (!entry)
        return;
    EditorManager::activateEditorForEntry(entry);
};

static void openDocumentByDelta(int delta)
{
    const int count = DocumentModel::entryCount();
    const std::optional<int> curIdx = DocumentModel::indexOfDocument(
        EditorManager::currentDocument());
    if (!curIdx)
        return;
    const int newIdx = (*curIdx + delta + count) % count;
    openDocumentByIdx(newIdx);
};

static QAction *addMenuAction(
    QMenu *contextMenu,
    const QString &title,
    bool enabled,
    QObject *guard,
    const std::function<void()> &slot)
{
    QAction *action = contextMenu->addAction(title);
    action->setEnabled(enabled);
    QObject::connect(action, &QAction::triggered, guard, slot);
    return action;
}

void EditorManagerPrivate::init()
{
    DocumentModel::init();
    connect(ICore::instance(), &ICore::contextAboutToChange,
            this, &EditorManagerPrivate::handleContextChange);
    connect(qApp, &QApplication::applicationStateChanged,
            this, [](Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive)
                    EditorManager::updateWindowTitles();
            });

    const Context editManagerContext(Constants::C_EDITORMANAGER);
    // combined context for edit & design modes
    const Context editDesignContext(Constants::C_EDITORMANAGER, Constants::C_DESIGN_MODE);

    // Revert to saved
    ActionBuilder revertToSaved(this, Constants::REVERTTOSAVED);
    revertToSaved.setText(::Core::Tr::tr("Revert to Saved"));
    revertToSaved.setIcon(Icon::fromTheme("document-revert"));
    revertToSaved.setContext(editManagerContext);
    revertToSaved.bindContextAction(&m_revertToSavedAction);
    revertToSaved.setCommandAttribute(Command::CA_UpdateText);
    revertToSaved.setCommandDescription(::Core::Tr::tr("Revert File to Saved"));
    revertToSaved.addToContainer(Constants::M_FILE, Constants::G_FILE_SAVE);
    revertToSaved.addOnTriggered(this, &EditorManager::revertToSaved);

    // Save Action
    ActionBuilder save(this, Constants::SAVE);
    save.setContext(editManagerContext);
    save.bindContextAction(&m_saveAction);
    save.addOnTriggered(this, [] { EditorManager::saveDocument(); });

    // Save As Action
    ActionBuilder saveAs(this, Constants::SAVEAS);
    saveAs.setContext(editManagerContext);
    saveAs.bindContextAction(&m_saveAsAction);
    saveAs.addOnTriggered(this, &EditorManager::saveDocumentAs);

    // Window Menu
    ActionContainer *mwindow = ActionManager::actionContainer(Constants::M_WINDOW);

    // Window menu separators
    mwindow->addSeparator(editManagerContext, Constants::G_WINDOW_SPLIT);
    mwindow->addSeparator(editManagerContext, Constants::G_WINDOW_NAVIGATE);

    // Close Action
    ActionBuilder closeCurrentEditor(this, Constants::CLOSE);
    closeCurrentEditor.bindContextAction(&m_closeCurrentEditorAction);
    closeCurrentEditor.setContext(editManagerContext);
    closeCurrentEditor.setScriptable(true);
    closeCurrentEditor.setDefaultKeySequence(::Core::Tr::tr("Ctrl+W"));
    closeCurrentEditor.setCommandAttribute(Command::CA_UpdateText);
    closeCurrentEditor.setCommandDescription(m_closeCurrentEditorAction->text());
    closeCurrentEditor.addToContainer(Constants::M_FILE, Constants::G_FILE_CLOSE);
    closeCurrentEditor.addOnTriggered(this,  &EditorManager::slotCloseCurrentEditorOrDocument);

    if (HostOsInfo::isWindowsHost()) {
        // workaround for QTCREATORBUG-72
        ActionBuilder workaround(this, Constants::CLOSE_ALTERNATIVE);
        workaround.setContext(editManagerContext);
        workaround.setDefaultKeySequence(::Core::Tr::tr("Ctrl+F4"));
        workaround.setCommandDescription(::Core::Tr::tr("Close"));
        workaround.addOnTriggered(this, &EditorManager::slotCloseCurrentEditorOrDocument);
    }

    // Close All Action
    ActionBuilder closeAll(this, Constants::CLOSEALL);
    closeAll.setText(::Core::Tr::tr("Close All"));
    closeAll.setContext(editManagerContext);
    closeAll.setScriptable(true);
    closeAll.bindContextAction(&m_closeAllEditorsAction);
    closeAll.setDefaultKeySequence(::Core::Tr::tr("Ctrl+Shift+W"));
    closeAll.addToContainer(Constants::M_FILE, Constants::G_FILE_CLOSE);
    closeAll.addOnTriggered(this, &EditorManager::closeAllDocuments);

    // Close All Others Action
    ActionBuilder closeOthers(this, Constants::CLOSEOTHERS);
    closeOthers.setText(::Core::Tr::tr("Close Others"));
    closeOthers.bindContextAction(&m_closeOtherDocumentsAction);
    closeOthers.setContext(editManagerContext);
    closeOthers.setScriptable(true);
    closeOthers.addToContainer(Constants::M_FILE, Constants::G_FILE_CLOSE);
    closeOthers.setCommandAttribute(Command::CA_UpdateText);
    closeOthers.addOnTriggered(this, [] { EditorManager::closeOtherDocuments(); });

    // Close All Others Except Visible Action
    ActionBuilder closeAllExceptVisible(this, Constants::CLOSEALLEXCEPTVISIBLE);
    closeAllExceptVisible.setText(::Core::Tr::tr("Close All Except Visible"));
    closeAllExceptVisible.bindContextAction(&m_closeAllEditorsExceptVisibleAction);
    closeAllExceptVisible.setContext(editManagerContext);
    closeAllExceptVisible.setScriptable(true);
    closeAllExceptVisible.addToContainer(Constants::M_FILE, Constants::G_FILE_CLOSE);
    closeAllExceptVisible.addOnTriggered(this, &EditorManagerPrivate::closeAllEditorsExceptVisible);

    ActionBuilder openGraphicalShell(this, Constants::SHOWINGRAPHICALSHELL);
    openGraphicalShell.setText(FileUtils::msgGraphicalShellAction());
    openGraphicalShell.bindContextAction(&m_openGraphicalShellAction);
    openGraphicalShell.setContext(editManagerContext);
    openGraphicalShell.addOnTriggered(this, [] {
        if (!EditorManager::currentDocument())
            return;
        const FilePath fp = EditorManager::currentDocument()->filePath();
        if (!fp.isEmpty())
            FileUtils::showInGraphicalShell(fp);
    });

    ActionBuilder showInFileSystem(this, Constants::SHOWINFILESYSTEMVIEW);
    showInFileSystem.setText(FileUtils::msgFileSystemAction());
    showInFileSystem.setContext(editManagerContext);
    showInFileSystem.bindContextAction(&m_showInFileSystemViewAction);
    showInFileSystem.addOnTriggered(this, [] {
        if (!EditorManager::currentDocument())
            return;
        const FilePath fp = EditorManager::currentDocument()->filePath();
        if (!fp.isEmpty())
            FileUtils::showInFileSystemView(fp);
    });

    // Goto Previous In History Action
    ActionBuilder gotoPrevInHistory(this, Constants::GOTOPREVINHISTORY);
    gotoPrevInHistory.setText(::Core::Tr::tr("Previous Open Document in History"));
    gotoPrevInHistory.bindContextAction(&m_gotoPreviousDocHistoryAction);
    gotoPrevInHistory.setContext(editDesignContext);
    gotoPrevInHistory.setDefaultKeySequence(::Core::Tr::tr("Alt+Tab"), ::Core::Tr::tr("Ctrl+Tab"));
    gotoPrevInHistory.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    gotoPrevInHistory.addOnTriggered(this, &EditorManagerPrivate::gotoPreviousDocHistory);

    // Goto Next In History Action
    ActionBuilder gotoNextInHistory(this, Constants::GOTONEXTINHISTORY);
    gotoNextInHistory.setText(::Core::Tr::tr("Next Open Document in History"));
    gotoNextInHistory.bindContextAction(&m_gotoNextDocHistoryAction);
    gotoNextInHistory.setContext(editDesignContext);
    gotoNextInHistory.setDefaultKeySequence(::Core::Tr::tr("Alt+Shift+Tab"), ::Core::Tr::tr("Ctrl+Shift+Tab"));
    gotoNextInHistory.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    gotoNextInHistory.addOnTriggered(this, &EditorManagerPrivate::gotoNextDocHistory);

    // Goto Previous Tab Action
    ActionBuilder gotoPrevTab(this, Constants::GOTOPREVTAB);
    gotoPrevTab.setText(::Core::Tr::tr("Previous Tab"));
    gotoPrevTab.bindContextAction(&m_gotoPreviousTabAction);
    gotoPrevTab.bindCommand(&m_gotoPreviousTabCommand);
    gotoPrevTab.setContext(editDesignContext);
    gotoPrevTab
        .setDefaultKeySequence(::Core::Tr::tr("Ctrl+Meta+Left"), ::Core::Tr::tr("Ctrl+Alt+Left"));
    gotoPrevTab.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    gotoPrevTab.addOnTriggered(this, &EditorManagerPrivate::gotoPreviousTab);

    // Goto Next Tab Action
    ActionBuilder gotoNextTab(this, Constants::GOTONEXTTAB);
    gotoNextTab.setText(::Core::Tr::tr("Next Tab"));
    gotoNextTab.bindContextAction(&m_gotoNextTabAction);
    gotoNextTab.bindCommand(&m_gotoNextTabCommand);
    gotoNextTab.setContext(editDesignContext);
    gotoNextTab
        .setDefaultKeySequence(::Core::Tr::tr("Ctrl+Meta+Right"), ::Core::Tr::tr("Ctrl+Alt+Right"));
    gotoNextTab.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    gotoNextTab.addOnTriggered(this, &EditorManagerPrivate::gotoNextTab);

    // Go back in navigation history
    ActionBuilder goBack(this, Constants::GO_BACK);
    goBack.setIcon(Utils::Icons::PREV.icon());
    goBack.setText(::Core::Tr::tr("Go Back"));
    goBack.bindContextAction(&m_goBackAction);
    goBack.setContext(editDesignContext);
    goBack.setDefaultKeySequence(::Core::Tr::tr("Ctrl+Alt+Left"), ::Core::Tr::tr("Alt+Left"));
    goBack.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    goBack.addOnTriggered(this, &EditorManager::goBackInNavigationHistory);

    // Go forward in navigation history
    ActionBuilder goForward(this, Constants::GO_FORWARD);
    goForward.setIcon(Utils::Icons::NEXT.icon());
    goForward.setText(::Core::Tr::tr("Go Forward"));
    goForward.bindContextAction(&m_goForwardAction);
    goForward.setContext(editDesignContext);
    goForward.setDefaultKeySequence(::Core::Tr::tr("Ctrl+Alt+Right"), ::Core::Tr::tr("Alt+Right"));
    goForward.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    goForward.addOnTriggered(this, &EditorManager::goForwardInNavigationHistory);

    ActionBuilder openPreviousDocument(this, Constants::OPEN_PREVIOUS_DOCUMENT);
    openPreviousDocument.setIcon(Utils::Icons::PREV.icon());
    openPreviousDocument.setText(::Core::Tr::tr("Open Previous Document"));
    openPreviousDocument.bindContextAction(&m_prevDocAction);
    openPreviousDocument.setContext(editDesignContext);
    openPreviousDocument.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    openPreviousDocument.addOnTriggered(this, [] { openDocumentByDelta(-1); });

    ActionBuilder openNextDocument(this, Constants::OPEN_NEXT_DOCUMENT);
    openNextDocument.setIcon(Utils::Icons::NEXT.icon());
    openNextDocument.setText(::Core::Tr::tr("Open Next Document"));
    openPreviousDocument.bindContextAction(&m_nextDocAction);
    openNextDocument.setContext(editDesignContext);
    openNextDocument.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    openNextDocument.addOnTriggered(this, [] { openDocumentByDelta(1); });

    // Reopen last closed document
    ActionBuilder reopenLastClosedDocument(this, Constants::REOPEN_CLOSED_EDITOR);
    reopenLastClosedDocument.setText(::Core::Tr::tr("Reopen Last Closed Document"));
    reopenLastClosedDocument.bindContextAction(&m_reopenLastClosedDocumenAction);
    reopenLastClosedDocument.setContext(editDesignContext);
    reopenLastClosedDocument.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    reopenLastClosedDocument.addOnTriggered(this, &EditorManagerPrivate::reopenLastClosedDocument);

    // Go to last edit
    ActionBuilder gotoLastEdit(this, Constants::GOTOLASTEDIT);
    gotoLastEdit.setText(::Core::Tr::tr("Go to Last Edit"));
    gotoLastEdit.bindContextAction(&m_gotoLastEditAction);
    gotoLastEdit.setContext(editDesignContext);
    gotoLastEdit.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_NAVIGATE);
    gotoLastEdit.addOnTriggered(this, &EditorManagerPrivate::gotoLastEditLocation);

    ActionBuilder split(this, Constants::SPLIT);
    split.setText(::Core::Tr::tr("Split"));
    split.setIcon(Utils::Icons::SPLIT_HORIZONTAL.icon());
    split.bindContextAction(&m_splitAction);
    split.setContext(editManagerContext);
    split.setDefaultKeySequence(::Core::Tr::tr("Meta+E,2"), ::Core::Tr::tr("Ctrl+E,2"));
    split.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    split.addOnTriggered(this, [this] { this->split(Qt::Vertical); });

    ActionBuilder splitSideBySide(this, Constants::SPLIT_SIDE_BY_SIDE);
    splitSideBySide.setIcon(Utils::Icons::SPLIT_VERTICAL.icon());
    splitSideBySide.setText(::Core::Tr::tr("Split Side by Side"));
    splitSideBySide.bindContextAction(&m_splitSideBySideAction);
    splitSideBySide.setContext(editManagerContext);
    splitSideBySide.setDefaultKeySequence(::Core::Tr::tr("Meta+E,3"), ::Core::Tr::tr("Ctrl+E,3"));
    splitSideBySide.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    splitSideBySide.addOnTriggered(this, &EditorManager::splitSideBySide);

    ActionBuilder splitNewWindow(this, Constants::SPLIT_NEW_WINDOW);
    splitNewWindow.setText(::Core::Tr::tr("Open in New Window"));
    splitNewWindow.bindContextAction(&m_splitNewWindowAction);
    splitNewWindow.setContext(editManagerContext);
    splitNewWindow.setDefaultKeySequence(::Core::Tr::tr("Meta+E,4"), ::Core::Tr::tr("Ctrl+E,4"));
    splitNewWindow.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    splitNewWindow.addOnTriggered(this, [this] { this->splitNewWindow(currentEditorView()); });

    ActionBuilder removeCurrentSplit(this, Constants::REMOVE_CURRENT_SPLIT);
    removeCurrentSplit.setText(::Core::Tr::tr("Remove Current Split"));
    removeCurrentSplit.bindContextAction(&m_removeCurrentSplitAction);
    removeCurrentSplit.setContext(editManagerContext);
    removeCurrentSplit.setDefaultKeySequence(::Core::Tr::tr("Meta+E,0"), ::Core::Tr::tr("Ctrl+E,0"));
    removeCurrentSplit.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    removeCurrentSplit.addOnTriggered(this, &EditorManagerPrivate::removeCurrentSplit);

    ActionBuilder removeAllSplits(this, Constants::REMOVE_ALL_SPLITS);
    removeAllSplits.setText(::Core::Tr::tr("Remove All Splits"));
    removeAllSplits.bindContextAction(&m_removeAllSplitsAction);
    removeAllSplits.setContext(editManagerContext);
    removeAllSplits.setDefaultKeySequence(::Core::Tr::tr("Meta+E,1"), ::Core::Tr::tr("Ctrl+E,1"));
    removeAllSplits.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    removeAllSplits.addOnTriggered(this, &EditorManagerPrivate::removeAllSplits);

    ActionBuilder gotoPreviousSplit(this, Constants::GOTO_PREV_SPLIT);
    gotoPreviousSplit.setText(::Core::Tr::tr("Go to Previous Split or Window"));
    gotoPreviousSplit.bindContextAction(&m_gotoPreviousSplitAction);
    gotoPreviousSplit.setContext(editManagerContext);
    gotoPreviousSplit.setDefaultKeySequence(::Core::Tr::tr("Meta+E,i"), ::Core::Tr::tr("Ctrl+E,i"));
    gotoPreviousSplit.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    gotoPreviousSplit.addOnTriggered(this, &EditorManagerPrivate::gotoPreviousSplit);

    ActionBuilder gotoNextSplit(this, Constants::GOTO_NEXT_SPLIT);
    gotoNextSplit.setText(::Core::Tr::tr("Go to Next Split or Window"));
    gotoNextSplit.bindContextAction(&m_gotoNextSplitAction);
    gotoNextSplit.setContext(editManagerContext);
    gotoNextSplit.setDefaultKeySequence(QKeySequence(useMacShortcuts ? ::Core::Tr::tr("Meta+E,o") : ::Core::Tr::tr("Ctrl+E,o")));
    gotoNextSplit.addToContainer(Constants::M_WINDOW, Constants::G_WINDOW_SPLIT);
    gotoNextSplit.addOnTriggered(this, &EditorManagerPrivate::gotoNextSplit);

    ActionContainer *medit = ActionManager::actionContainer(Constants::M_EDIT);
    ActionContainer *advancedMenu = ActionManager::createMenu(Constants::M_EDIT_ADVANCED);
    medit->addMenu(advancedMenu, Constants::G_EDIT_ADVANCED);
    advancedMenu->menu()->setTitle(::Core::Tr::tr("Ad&vanced"));
    advancedMenu->appendGroup(Constants::G_EDIT_FORMAT);
    advancedMenu->appendGroup(Constants::G_EDIT_TEXT);
    advancedMenu->appendGroup(Constants::G_EDIT_COLLAPSING);
    advancedMenu->appendGroup(Constants::G_EDIT_BLOCKS);
    advancedMenu->appendGroup(Constants::G_EDIT_FONT);
    advancedMenu->appendGroup(Constants::G_EDIT_EDITOR);

    // Advanced menu separators
    advancedMenu->addSeparator(editManagerContext, Constants::G_EDIT_TEXT);
    advancedMenu->addSeparator(editManagerContext, Constants::G_EDIT_COLLAPSING);
    advancedMenu->addSeparator(editManagerContext, Constants::G_EDIT_BLOCKS);
    advancedMenu->addSeparator(editManagerContext, Constants::G_EDIT_FONT);
    advancedMenu->addSeparator(editManagerContext, Constants::G_EDIT_EDITOR);

    // other setup
    auto mainEditorArea = new EditorArea();
    // assign parent to avoid failing updates (e.g. windowTitle) before it is displayed first time
    mainEditorArea->setParent(ICore::mainWindow());
    mainEditorArea->hide();
    connect(mainEditorArea, &EditorArea::windowTitleNeedsUpdate,
            this, &EditorManagerPrivate::updateWindowTitle);
    addEditorArea(mainEditorArea);
    setCurrentView(mainEditorArea->currentView());

    updateActions();

    // The popup needs a parent to get keyboard focus.
    m_windowPopup = new OpenEditorsWindow(mainEditorArea);
    m_windowPopup->hide();

    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setObjectName("EditorManager::m_autoSaveTimer");
    connect(m_autoSaveTimer, &QTimer::timeout, this, &EditorManagerPrivate::autoSave);
    updateAutoSave();

    createOpenEditorsViewFactory();

    globalMacroExpander()->registerFileVariables(kCurrentDocumentPrefix, ::Core::Tr::tr("Current document"),
        [] {
            IDocument *document = EditorManager::currentDocument();
            return document ? document->filePath() : FilePath();
        });

    globalMacroExpander()->registerIntVariable(kCurrentDocumentXPos,
        ::Core::Tr::tr("X-coordinate of the current editor's upper left corner, relative to screen."),
        []() -> int {
            IEditor *editor = EditorManager::currentEditor();
            return editor ? editor->widget()->mapToGlobal(QPoint(0, 0)).x() : 0;
        });

    globalMacroExpander()->registerIntVariable(kCurrentDocumentYPos,
        ::Core::Tr::tr("Y-coordinate of the current editor's upper left corner, relative to screen."),
        []() -> int {
            IEditor *editor = EditorManager::currentEditor();
            return editor ? editor->widget()->mapToGlobal(QPoint(0, 0)).y() : 0;
                                               });

    connect(
        DocumentManager::instance(),
        &DocumentManager::allDocumentsRenamed,
        d,
        [](const FilePath &from, const FilePath &to) {
            EditorManagerPrivate::handleFileRenamed(from, to);
        });
    setShowingTabs(generalSettings().useTabsInEditorViews.value());
}

void EditorManagerPrivate::extensionsInitialized()
{
    // Do not ask for files to save.
    // MainWindow::closeEvent has already done that.
    ICore::addPreCloseListener([] { return EditorManager::closeAllEditors(false); });
}

EditorManagerPrivate *EditorManagerPrivate::instance()
{
    return d;
}

EditorArea *EditorManagerPrivate::mainEditorArea()
{
    return d->m_editorAreas.at(0);
}

bool EditorManagerPrivate::skipOpeningBigTextFile(const FilePath &filePath)
{
    if (!systemSettings().warnBeforeOpeningBigFiles())
        return false;

    if (!filePath.exists())
        return false;

    const MimeType mimeType = Utils::mimeTypeForFile(filePath,
                                                     MimeMatchMode::MatchDefaultAndRemote);
    if (!mimeType.inherits("text/plain"))
        return false;

    const qint64 fileSize = filePath.fileSize();
    const double fileSizeInMB = fileSize / 1000.0 / 1000.0;
    if (fileSizeInMB > systemSettings().bigFileSizeLimitInMB()
        && fileSize < EditorManager::maxTextFileSize()) {
        const QString title = ::Core::Tr::tr("Continue Opening Huge Text File?");
        const QString text = ::Core::Tr::tr(
            "The text file \"%1\" has the size %2MB and might take more memory to open"
            " and process than available.\n"
            "\n"
            "Continue?")
                .arg(filePath.fileName())
                .arg(fileSizeInMB, 0, 'f', 2);

        auto shouldAskAgain = [] { return true; };
        auto doNotAskAgain = [] { systemSettings().warnBeforeOpeningBigFiles.setValue(false); };

        QMessageBox::StandardButton clickedButton = CheckableMessageBox::question(
            title, text, {shouldAskAgain, doNotAskAgain});
        return clickedButton != QMessageBox::Yes;
    }

    return false;
}

IEditor *EditorManagerPrivate::openEditor(EditorView *view, const FilePath &filePath, Id editorId,
                                          EditorManager::OpenEditorFlags flags, bool *newEditor)
{
    if (debugEditorManager)
        qDebug() << Q_FUNC_INFO << filePath << editorId.name();

    if (filePath.isEmpty())
        return nullptr;

    if (newEditor)
        *newEditor = false;

    const QList<IEditor *> editors = DocumentModel::editorsForFilePath(filePath);
    if (!editors.isEmpty()) {
        IEditor *editor = editors.first();
        if (flags & EditorManager::SwitchSplitIfAlreadyVisible) {
            for (IEditor *ed : editors) {
                EditorView *v = viewForEditor(ed);
                // Don't switch to a view where editor is not its current editor
                if (v && v->currentEditor() == ed) {
                    editor = ed;
                    view = v;
                    break;
                }
            }
        }
        return activateEditor(view, editor, flags);
    }

    if (skipOpeningBigTextFile(filePath))
        return nullptr;

    FilePath realFp = autoSaveName(filePath);
    if (!filePath.exists() || !realFp.exists() || filePath.lastModified() >= realFp.lastModified()) {
        realFp.removeFile();
        realFp = filePath;
    }

    EditorFactories factories = IEditorFactory::preferredEditorTypes(filePath);
    if (!(flags & EditorManager::AllowExternalEditor))
        factories = Utils::filtered(factories, &IEditorFactory::isInternalEditor);

    if (factories.isEmpty()) {
        Utils::MimeType mimeType = Utils::mimeTypeForFile(filePath);
        QMessageBox msgbox(QMessageBox::Critical, ::Core::Tr::tr("File Error"),
                           ::Core::Tr::tr("Could not open \"%1\": Cannot open files of type \"%2\".")
                           .arg(realFp.toUserOutput(), mimeType.name()),
                           QMessageBox::Ok, ICore::dialogParent());
        msgbox.exec();
        return nullptr;
    }
    if (editorId.isValid()) {
        IEditorFactory *factory = IEditorFactory::editorFactoryForId(editorId);
        if (factory) {
            QTC_CHECK(factory->isInternalEditor() || (flags & EditorManager::AllowExternalEditor));
            factories.removeOne(factory);
            factories.push_front(factory);
        }
    }

    IEditor *editor = nullptr;
    auto overrideCursor = Utils::OverrideCursor(QCursor(Qt::WaitCursor));

    IEditorFactory *factory = factories.takeFirst();
    while (factory) {
        QString errorString;

        if (factory->isInternalEditor()) {
            editor = createEditor(factory, filePath);
            if (!editor) {
                factory = factories.isEmpty() ? nullptr : factories.takeFirst();
                continue;
            }
            Result<> openResult = editor->document()->open(filePath, realFp);
            if (openResult)
                break;
            errorString = openResult.error();
            overrideCursor.reset();
            delete editor;
            editor = nullptr;
        } else {
            QTC_ASSERT(factory->isExternalEditor(),
                       factory = factories.isEmpty() ? nullptr : factories.takeFirst();
                       continue);
            const Result<> res = factory->startEditor(filePath);
            if (res)
                break;
            errorString = res.error();
        }

        if (errorString.isEmpty())
            errorString = ::Core::Tr::tr("Could not open \"%1\": Unknown error.").arg(realFp.toUserOutput());

        QMessageBox msgbox(QMessageBox::Critical,
                           ::Core::Tr::tr("File Error"),
                           errorString,
                           QMessageBox::Open | QMessageBox::Cancel,
                           ICore::dialogParent());

        IEditorFactory *selectedFactory = nullptr;
        if (!factories.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
            msgbox.setOptions(QMessageBox::Option::DontUseNativeDialog);
#endif
            auto button = qobject_cast<QPushButton *>(msgbox.button(QMessageBox::Open));
            QTC_ASSERT(button, return nullptr);
            auto menu = new QMenu(button);
            for (IEditorFactory *factory : std::as_const(factories)) {
                QAction *action = menu->addAction(factory->displayName());
                connect(action, &QAction::triggered, &msgbox, [&selectedFactory, factory, &msgbox] {
                    selectedFactory = factory;
                    msgbox.done(QMessageBox::Open);
                });
            }

            button->setMenu(menu);
        } else {
            msgbox.setStandardButtons(QMessageBox::Ok);
        }

        int ret = msgbox.exec();
        if (ret == QMessageBox::Cancel || ret == QMessageBox::Ok)
            return nullptr;

        overrideCursor.set();

        factories.removeOne(selectedFactory);
        factory = selectedFactory;
    }

    if (!editor)
        return nullptr;

    if (realFp != filePath)
        editor->document()->setRestoredFrom(realFp);
    addEditor(editor);

    if (newEditor)
        *newEditor = true;

    IEditor *result = activateEditor(view, editor, flags);
    if (editor == result)
        restoreEditorState(editor);

    return result;
}

IEditor *EditorManagerPrivate::openEditorAt(EditorView *view,
                                            const Link &link,
                                            Id editorId,
                                            EditorManager::OpenEditorFlags flags,
                                            bool *newEditor)
{
    EditorManager::cutForwardNavigationHistory();
    EditorManager::addCurrentPositionToNavigationHistory();
    EditorManager::OpenEditorFlags tempFlags = flags | EditorManager::IgnoreNavigationHistory;
    IEditor *editor = openEditor(view, link.targetFilePath, editorId, tempFlags, newEditor);
    if (editor && link.target.isValid())
        editor->gotoLine(link.target.line, link.target.column);
    return editor;
}

IEditor *EditorManagerPrivate::openEditorWith(const FilePath &filePath, Id editorId)
{
    // close any open editors that have this file open
    // remember the views to open new editors in there
    QList<EditorView *> views;
    const QList<IEditor *> editorsOpenForFile = DocumentModel::editorsForFilePath(filePath);
    for (IEditor *openEditor : editorsOpenForFile) {
        EditorView *view = EditorManagerPrivate::viewForEditor(openEditor);
        if (view && view->currentEditor() == openEditor) // visible
            views.append(view);
    }
    if (!EditorManager::closeEditors(editorsOpenForFile)) // don't open if cancel was pressed
        return nullptr;

    IEditor *openedEditor = nullptr;
    if (views.isEmpty()) {
        openedEditor = EditorManager::openEditor(filePath, editorId);
    } else {
        if (EditorView *currentView = EditorManagerPrivate::currentEditorView()) {
            if (views.removeOne(currentView))
                views.prepend(currentView); // open editor in current view first
        }
        EditorManager::OpenEditorFlags flags;
        for (EditorView *view : std::as_const(views)) {
            IEditor *editor = EditorManagerPrivate::openEditor(view, filePath, editorId, flags);
            if (!openedEditor && editor)
                openedEditor = editor;
            // Do not change the current editor after opening the first one. That
            // * prevents multiple updates of focus etc which are not necessary
            // * lets us control which editor is made current by putting the current editor view
            //   to the front (if that was in the list in the first place)
            flags |= EditorManager::DoNotChangeCurrentEditor;
            // do not try to open more editors if this one failed, or editor type does not
            // support duplication anyhow
            if (!editor || !editor->duplicateSupported())
                break;
        }
    }
    return openedEditor;
}

IEditor *EditorManagerPrivate::activateEditorForDocument(EditorView *view, IDocument *document,
                                                         EditorManager::OpenEditorFlags flags)
{
    QTC_ASSERT(view, return nullptr);
    IEditor *editor = view->editorForDocument(document);
    if (!editor) {
        const QList<IEditor*> editors = DocumentModel::editorsForDocument(document);
        if (editors.isEmpty())
            return nullptr;
        editor = editors.first();
    }
    return activateEditor(view, editor, flags);
}

EditorView *EditorManagerPrivate::viewForEditor(IEditor *editor)
{
    QWidget *w = editor->widget();
    while (w) {
        w = w->parentWidget();
        if (auto view = qobject_cast<EditorView *>(w))
            return view;
    }
    return nullptr;
}

MakeWritableResult EditorManagerPrivate::makeFileWritable(IDocument *document)
{
    if (!document)
        return Failed;
    ReadOnlyFilesDialog roDialog(document, ICore::dialogParent(), document->isSaveAsAllowed());
    switch (roDialog.exec()) {
    case ReadOnlyFilesDialog::RO_MakeWritable:
    case ReadOnlyFilesDialog::RO_OpenVCS:
        return MadeWritable;
    case ReadOnlyFilesDialog::RO_SaveAs:
        return SavedAs;
    default:
        return Failed;
    }
}

/*!
    Implements the logic of the escape key shortcut (ReturnToEditor).
    Should only be called by the shortcut handler.
    \internal
*/
void EditorManagerPrivate::doEscapeKeyFocusMoveMagic()
{
    // use cases to cover:
    // 1. if app focus is in mode or external window without editor view (e.g. Design, Projects, ext. Help)
    //      if there are extra views (e.g. output)
    //        hide them
    //      otherwise
    //        activate & raise the current editor view (can be external)
    //        if that is in edit mode
    //          activate edit mode and unmaximize output pane
    // 2. if app focus is in external window with editor view
    //      hide find if necessary
    // 2. if app focus is in mode with editor view
    //      if current editor view is in external window
    //        raise and activate current editor view
    //      otherwise if the current editor view is not app focus
    //        move focus to editor view in mode and unmaximize output pane
    //      otherwise if the current view is app focus
    //        if mode is not edit mode
    //          if there are extra views (find, help, output)
    //            hide them
    //          otherwise
    //            activate edit mode and unmaximize output pane
    //        otherwise (i.e. mode is edit mode)
    //          hide extra views (find, help, output)

    QWidget *activeWindow = QApplication::activeWindow();
    if (!activeWindow)
        return;
    QWidget *focus = QApplication::focusWidget();
    EditorView *editorView = currentEditorView();
    QTC_CHECK(editorView);
    bool editorViewActive = (editorView && focus && focus == editorView->focusWidget());
    bool editorViewVisible = editorView && editorView->isVisible();

    bool stuffHidden = false;
    FindToolBarPlaceHolder *findPane = FindToolBarPlaceHolder::getCurrent();
    if (findPane && findPane->isVisible() && findPane->isUsedByWidget(focus)) {
        findPane->hide();
        stuffHidden = true;
    } else if (!( editorViewVisible && !editorViewActive && editorView->window() == activeWindow )) {
        QWidget *outputPane = OutputPanePlaceHolder::getCurrent();
        if (outputPane && outputPane->isVisible() && outputPane->window() == activeWindow) {
            OutputPaneManager::instance()->slotHide();
            stuffHidden = true;
        }
        QWidget *rightPane = RightPanePlaceHolder::current();
        if (rightPane && rightPane->isVisible() && rightPane->window() == activeWindow) {
            RightPaneWidget::instance()->setShown(false);
            stuffHidden = true;
        }
        if (findPane && findPane->isVisible() && findPane->window() == activeWindow) {
            findPane->hide();
            stuffHidden = true;
        }
    }
    if (stuffHidden)
        return;

    if (!editorViewActive && editorViewVisible) {
        setFocusToEditorViewAndUnmaximizePanes(editorView);
        return;
    }

    if (!editorViewActive && !editorViewVisible) {
        // assumption is that editorView is in main window then
        ModeManager::activateMode(Id(Constants::MODE_EDIT));
        QTC_CHECK(editorView->isVisible());
        setFocusToEditorViewAndUnmaximizePanes(editorView);
        return;
    }

    if (editorView->window() == ICore::mainWindow()) {
        // we are in a editor view and there's nothing to hide, switch to edit
        ModeManager::activateMode(Id(Constants::MODE_EDIT));
        QTC_CHECK(editorView->isVisible());
        // next call works only because editor views in main window are shared between modes
        setFocusToEditorViewAndUnmaximizePanes(editorView);
    }
}

OpenEditorsWindow *EditorManagerPrivate::windowPopup()
{
    return d->m_windowPopup;
}

void EditorManagerPrivate::showPopupOrSelectDocument()
{
    if (QApplication::keyboardModifiers() == Qt::NoModifier) {
        windowPopup()->selectAndHide();
    } else {
        QWidget *activeWindow = QApplication::activeWindow();
        // decide where to show the popup
        // if the active window has editors, we want that editor area as a reference
        // TODO: this does not work correctly with multiple editor areas in the same window
        EditorArea *activeEditorArea = nullptr;
        for (EditorArea *area : std::as_const(d->m_editorAreas)) {
            if (area->window() == activeWindow) {
                activeEditorArea = area;
                break;
            }
        }
        // otherwise we take the "current" editor area
        if (!activeEditorArea)
            activeEditorArea = findEditorArea(EditorManagerPrivate::currentEditorView());
        QTC_ASSERT(activeEditorArea, activeEditorArea = d->m_editorAreas.first());

        // editor area in main window is invisible when invoked from Design Mode.
        QWidget *referenceWidget = activeEditorArea->isVisible() ? activeEditorArea : activeEditorArea->window();
        QTC_CHECK(referenceWidget->isVisible());
        const QPoint p = referenceWidget->mapToGlobal(QPoint(0, 0));
        OpenEditorsWindow *popup = windowPopup();
        popup->setMaximumSize(qMax(popup->minimumWidth(), referenceWidget->width() / 2),
                              qMax(popup->minimumHeight(), referenceWidget->height() / 2));
        popup->adjustSize();
        popup->move((referenceWidget->width() - popup->width()) / 2 + p.x(),
                    (referenceWidget->height() - popup->height()) / 2 + p.y());
        popup->setVisible(true);
    }
}

// Run the OpenWithDialog and return the editor id
// selected by the user.
Id EditorManagerPrivate::getOpenWithEditorId(const Utils::FilePath &fileName, bool *isExternalEditor)
{
    // Collect editors that can open the file
    QList<Id> allEditorIds;
    QStringList allEditorDisplayNames;
    // Built-in
    const EditorFactories editors = IEditorFactory::preferredEditorTypes(fileName);
    const int size = editors.size();
    allEditorDisplayNames.reserve(size);
    for (int i = 0; i < size; i++) {
        allEditorIds.push_back(editors.at(i)->id());
        allEditorDisplayNames.push_back(editors.at(i)->displayName());
    }
    if (allEditorIds.empty())
        return Id();
    QTC_ASSERT(allEditorIds.size() == allEditorDisplayNames.size(), return Id());
    // Run dialog.
    OpenWithDialog dialog(fileName, ICore::dialogParent());
    dialog.setEditors(allEditorDisplayNames);
    dialog.setCurrentEditor(0);
    if (dialog.exec() != QDialog::Accepted)
        return Id();
    const Id selectedId = allEditorIds.at(dialog.editor());
    if (isExternalEditor) {
        IEditorFactory *type = IEditorFactory::editorFactoryForId(selectedId);
        *isExternalEditor = type && type->isExternalEditor();
    }
    return selectedId;
}

static QMap<QString, QVariant> toMap(const QHash<QString, IEditorFactory *> &hash)
{
    QMap<QString, QVariant> map;
    auto it = hash.begin();
    const auto end = hash.end();
    while (it != end) {
        map.insert(it.key(), it.value()->id().toSetting());
        ++it;
    }
    return map;
}

static QHash<QString, IEditorFactory *> fromMap(const QMap<QString, QVariant> &map)
{
    const EditorFactories factories = IEditorFactory::allEditorFactories();
    QHash<QString, IEditorFactory *> hash;
    auto it = map.begin();
    const auto end = map.end();
    while (it != end) {
        const Id factoryId = Id::fromSetting(it.value());
        IEditorFactory *factory = Utils::findOrDefault(factories,
                                                       Utils::equal(&IEditorFactory::id, factoryId));
        if (factory)
            hash.insert(it.key(), factory);
        ++it;
    }
    return hash;
}

void EditorManagerPrivate::saveSettings()
{
    SettingsDatabase::setValue(documentStatesKey, d->m_editorStates);

    QtcSettings *qsettings = ICore::settings();
    qsettings->setValueWithDefault(preferredEditorFactoriesKey,
                                   toMap(userPreferredEditorTypes()));
}

void EditorManagerPrivate::readSettings()
{
    QtcSettings *qs = ICore::settings();

    const Qt::CaseSensitivity defaultSensitivity = OsSpecificAspects::fileNameCaseSensitivity(
        HostOsInfo::hostOs());
    const Qt::CaseSensitivity sensitivity = readFileSystemSensitivity(qs);
    if (sensitivity == defaultSensitivity)
        HostOsInfo::unsetOverrideFileNameCaseSensitivity();
    else
        HostOsInfo::setOverrideFileNameCaseSensitivity(sensitivity);

    const QHash<QString, IEditorFactory *> preferredEditorFactories = fromMap(
        qs->value(preferredEditorFactoriesKey).toMap());
    setUserPreferredEditorTypes(preferredEditorFactories);

    if (SettingsDatabase::contains(documentStatesKey)) {
        d->m_editorStates = SettingsDatabase::value(documentStatesKey)
            .value<QMap<QString, QVariant>>();
    }

    updateAutoSave();
}

Qt::CaseSensitivity EditorManagerPrivate::readFileSystemSensitivity(QtcSettings *settings)
{
    const Qt::CaseSensitivity defaultSensitivity = OsSpecificAspects::fileNameCaseSensitivity(
        HostOsInfo::hostOs());
    if (!settings->contains(fileSystemCaseSensitivityKey))
        return defaultSensitivity;
    bool ok = false;
    const int sensitivitySetting = settings->value(fileSystemCaseSensitivityKey).toInt(&ok);
    if (ok) {
        switch (Qt::CaseSensitivity(sensitivitySetting)) {
        case Qt::CaseSensitive:
            return Qt::CaseSensitive;
        case Qt::CaseInsensitive:
            return Qt::CaseInsensitive;
        }
    }
    return defaultSensitivity;
}

void EditorManagerPrivate::writeFileSystemSensitivity(Utils::QtcSettings *settings,
                                                      Qt::CaseSensitivity sensitivity)
{
    settings->setValueWithDefault(fileSystemCaseSensitivityKey,
                                  int(sensitivity),
                                  int(OsSpecificAspects::fileNameCaseSensitivity(
                                      HostOsInfo::hostOs())));
}

EditorFactories EditorManagerPrivate::findFactories(Id editorId, const FilePath &filePath)
{
    if (debugEditorManager)
        qDebug() << Q_FUNC_INFO << editorId.name() << filePath;

    EditorFactories factories;
    if (!editorId.isValid()) {
        factories = IEditorFactory::preferredEditorFactories(filePath);
    } else {
        // Find by editor id
        IEditorFactory *factory = Utils::findOrDefault(IEditorFactory::allEditorFactories(),
                                                       Utils::equal(&IEditorFactory::id, editorId));
        if (factory)
            factories.push_back(factory);
    }
    if (factories.empty()) {
        qWarning("%s: unable to find an editor factory for the file '%s', editor Id '%s'.",
                 Q_FUNC_INFO, filePath.toUrlishString().toUtf8().constData(), editorId.name().constData());
    }

    return factories;
}

IEditor *EditorManagerPrivate::createEditor(IEditorFactory *factory, const FilePath &filePath)
{
    if (!factory)
        return nullptr;

    IEditor *editor = factory->createEditor();
    if (editor) {
        QTC_CHECK(editor->document()->id().isValid()); // sanity check that the editor has an id set
        IDocument *document = editor->document();
        connect(document, &IDocument::changed, d, [document] {
            d->handleDocumentStateChange(document);
        });
        emit m_instance->editorCreated(editor, filePath);
    }

    return editor;
}

void EditorManagerPrivate::addEditor(IEditor *editor)
{
    if (!editor)
        return;
    ICore::addContextObject(editor);

    bool isNewDocument = false;
    DocumentModelPrivate::addEditor(editor, &isNewDocument);
    if (isNewDocument) {
        IDocument *document = editor->document();
        const bool isTemporary = (document->isTemporary() || document->filePath().isEmpty());
        const bool addWatcher = !isTemporary;
        DocumentManager::addDocument(document, addWatcher);
        if (!isTemporary)
            DocumentManager::addToRecentFiles(document->filePath(), document->id());
        emit m_instance->documentOpened(document);
    }
    emit m_instance->editorOpened(editor);
    QMetaObject::invokeMethod(d, &EditorManagerPrivate::autoSuspendDocuments, Qt::QueuedConnection);
}

void EditorManagerPrivate::removeEditor(IEditor *editor, bool removeSuspendedEntry)
{
    DocumentModel::Entry *entry = DocumentModelPrivate::removeEditor(editor);
    QTC_ASSERT(entry, return);
    if (entry->isSuspended) {
        IDocument *document = editor->document();
        DocumentManager::removeDocument(document);
        if (removeSuspendedEntry)
            DocumentModelPrivate::removeEntry(entry);
        emit m_instance->documentClosed(document);
    }
    ICore::removeContextObject(editor);
}

IEditor *EditorManagerPrivate::placeEditor(EditorView *view, IEditor *editor)
{
    Q_ASSERT(view && editor);

    if (view->hasEditor(editor))
        return editor;
    if (IEditor *e = view->editorForDocument(editor->document()))
        return e;

    QTC_CHECK(DocumentModel::editorsForDocument(editor->document()).contains(editor));

    const QByteArray state = editor->saveState();
    if (EditorView *sourceView = viewForEditor(editor)) {
        // Pulling the editor over is preferred in case the editor is currently not visible,
        // to decrease resource consumption. Otherwise we duplicate if the editor supports it.
        const bool duplicateSupported = editor->duplicateSupported();
        const bool isEditorVisible = editor == sourceView->currentEditor();
        if (!isEditorVisible || !duplicateSupported) {
            // pull the IEditor over to the new view
            removeEditorsFromViews(
                {{sourceView, editor}},
                !duplicateSupported ? EditorView::RemoveTab : EditorView::KeepTab,
                RemoveEditorFlag::EnsureNewEditor);
            view->addEditor(editor);
            // possibly adapts old state to new layout
            editor->restoreState(state);
            return editor;
        } else if (QTC_GUARD(duplicateSupported)) {
            editor = duplicateEditor(editor);
            Q_ASSERT(editor);
        }
    }
    view->addEditor(editor);
    // possibly adapts old state to new layout
    editor->restoreState(state);
    return editor;
}

IEditor *EditorManagerPrivate::duplicateEditor(IEditor *editor)
{
    if (!editor->duplicateSupported())
        return nullptr;

    IEditor *duplicate = editor->duplicate();
    emit m_instance->editorCreated(duplicate, duplicate->document()->filePath());
    addEditor(duplicate);
    return duplicate;
}

IEditor *EditorManagerPrivate::activateEditor(EditorView *view, IEditor *editor,
                                              EditorManager::OpenEditorFlags flags)
{
    QTC_ASSERT(view, return nullptr);

    if (!editor)
        return nullptr;

    editor = placeEditor(view, editor);

    if (!(flags & EditorManager::DoNotChangeCurrentEditor)) {
        setCurrentEditor(editor, (flags & EditorManager::IgnoreNavigationHistory));
        if (!(flags & EditorManager::DoNotMakeVisible)) {
            // switch to design mode?
            if (!(flags & EditorManager::DoNotSwitchToDesignMode) && editor->isDesignModePreferred()) {
                ModeManager::activateMode(Constants::MODE_DESIGN);
                ModeManager::setFocusToCurrentMode();
            } else {
                if (!(flags & EditorManager::DoNotSwitchToEditMode)) {
                    int index;
                    findEditorArea(view, &index);
                    if (index == 0) // main window --> we might need to switch mode
                        if (!editor->widget()->isVisible())
                            ModeManager::activateMode(Constants::MODE_EDIT);
                }
                editor->widget()->setFocus();
                if (!(flags & EditorManager::DoNotRaise))
                    ICore::raiseWindow(editor->widget());
            }
        }
    } else if (!(flags & EditorManager::DoNotMakeVisible)) {
        view->setCurrentEditor(editor);
    }
    return editor;
}

bool EditorManagerPrivate::activateEditorForEntry(EditorView *view, DocumentModel::Entry *entry,
                                                  EditorManager::OpenEditorFlags flags)
{
    QTC_ASSERT(view, return false);
    if (!entry) { // no document
        view->setCurrentEditor(nullptr);
        setCurrentView(view);
        return false;
    }
    IDocument *document = entry->document;
    if (!entry->isSuspended)  {
        IEditor *editor = activateEditorForDocument(view, document, flags);
        return editor != nullptr;
    }

    if (!openEditor(view, entry->filePath(), entry->id(), flags)) {
        DocumentModelPrivate::removeEntry(entry);
        return false;
    }
    return true;
}

// Removes the editors given in \a editorsPerView from the respective view.
// If \a option specifies to remove tabs, and \a flag is \c EnsureNewEditor,
// for any editor that is the current editor of its view, a new editor or tab
// is switched to, as appropriate.
void EditorManagerPrivate::removeEditorsFromViews(
    const QList<std::pair<EditorView *, IEditor *>> &editorsPerView,
    EditorView::RemovalOption option,
    RemoveEditorFlag flag)
{
    EditorView *globallyCurrentView = currentEditorView();
    QList<std::pair<EditorView *, IEditor *>> secondPass;
    // First close all editors that are not the current editor of a view,
    // then all "current" editors.
    // Since we might switch to a different editor/tab when we close the "current" editor
    // of a view, and doing that might move another (non-current) editor from a different
    // view, we prevent that from messing with our list of editors to close. Otherwise,
    // a (non-current) editor that we have in our list might move to be the "current" editor
    // of a different view before we have processed (and closed) it.
    for (const std::pair<EditorView *, IEditor *> &item : editorsPerView) {
        EditorView *view = item.first;
        IEditor *editor = item.second;
        QTC_ASSERT(view->hasEditor(editor), continue);
        if (flag == RemoveEditorFlag::None || editor != view->currentEditor()
            || option == EditorView::KeepTab) {
            view->removeEditor(editor, option);
            continue;
        }
        secondPass.append(item);
    }

    // Second pass. Close "current" editors, and choose a different one to show in the view,
    // as appropriate.
    for (const std::pair<EditorView *, IEditor *> &item : secondPass) {
        EditorView *view = item.first;
        IEditor *editor = item.second;
        // Prefer setting an IEditor that already is in the view
        const QList<IEditor *> editors = view->editors(); // last seen are at the back, current last
        if (editors.size() > 1) {
            // has more than only the closed editor,
            // so set the last seen one as the new current
            view->setCurrentEditor(editors.at(editors.size() - 2));
        } else {
            const EditorManager::OpenEditorFlags openEditorflags
                = view != globallyCurrentView ? EditorManager::DoNotChangeCurrentEditor
                                              : EditorManager::NoFlags;
            // Find a next (suspended) tab to open instead.
            // If the view doesn't show tabs, this still represents previously opened documents there.
            const QList<EditorView::TabData> tabs = view->tabs();
            const int tabToBeRemoved = view->tabForEditor(editor);
            if (tabs.size() > 1 && QTC_GUARD(tabToBeRemoved >= 0)) {
                const int tabToSwitchTo = tabToBeRemoved > 0 ? tabToBeRemoved - 1
                                                             : tabToBeRemoved + 1;
                if (QTC_GUARD(tabToSwitchTo < tabs.size()))
                    activateEditorForEntry(view, tabs.at(tabToSwitchTo).entry, openEditorflags);
            } else if (!view->isShowingTabs()) {
                // If the view *is* showing tabs, we don't want to open an arbitrary editor there,
                // but in the non-tabbed case we want to avoid an "empty" view, so pick something.
                // TODO do not choose an editor for the same document that we were closing
                IEditor *newCurrent = pickUnusedEditor();
                if (newCurrent) {
                    activateEditor(view, newCurrent, openEditorflags);
                } else {
                    DocumentModel::Entry *entry = DocumentModelPrivate::firstSuspendedEntry();
                    if (entry) {
                        activateEditorForEntry(view, entry, openEditorflags);
                    } else { // no "suspended" ones, so any entry left should have a document
                        const QList<DocumentModel::Entry *> documents = DocumentModel::entries();
                        if (!documents.isEmpty()) {
                            if (IDocument *document = documents.last()->document) {
                                // Do not auto-switch to design mode if the new editor will be for
                                // the same document as the one that was closed.
                                // TODO we should not open the same document that we closed in the
                                // first place
                                const EditorManager::OpenEditorFlags addFlags
                                    = (view == globallyCurrentView && document == editor->document())
                                          ? EditorManager::DoNotSwitchToDesignMode
                                          : EditorManager::NoFlags;
                                activateEditorForDocument(view, document, openEditorflags | addFlags);
                            }
                        }
                    }
                }
            }
        }
        view->removeEditor(editor, option);
    }
}

void EditorManagerPrivate::closeEditorOrDocument(IEditor *editor)
{
    QTC_ASSERT(editor, return);

    static const auto isEditorDocumentVisibleInDifferentView = [](IEditor *editor) {
        const QList<EditorView *> allViews = EditorManagerPrivate::allEditorViews();
        return Utils::contains(allViews, [editor](EditorView *view) {
            if (view->hasEditor(editor)) // this is the view where the editor comes from, ignore
                return false;
            return Utils::contains(view->visibleTabs(), [editor](const EditorView::TabData &tabData) {
                return tabData.entry->document == editor->document();
            });
        });
    };

    EditorManager::addCurrentPositionToNavigationHistory();
    if (isEditorDocumentVisibleInDifferentView(editor)) {
        // Another view either shows a tab for the document, or it is the visible editor there if
        // tabs are not shown
        EditorManagerPrivate::addClosedDocumentToCloseHistory(editor);
        if (DocumentModel::editorsForDocument(editor->document()).size() == 1) {
            // It's the only editor for that file, but tabs are still open somewhere
            // so we need to keep it around (--> in the editor model).
            // If the view doesn't show tabs, we do not end up in this code.
            removeEditorsFromViews(
                {{viewForEditor(editor), editor}},
                EditorView::RemoveTab,
                RemoveEditorFlag::EnsureNewEditor);
        } else {
            EditorManagerPrivate::closeEditors(
                {editor}, EditorManagerPrivate::CloseFlag::SuspendRemoveTab);
        }
    } else {
        EditorManager::closeDocuments({editor->document()});
    }
}

bool EditorManagerPrivate::closeEditors(const QList<IEditor*> &editors, CloseFlag flag)
{
    if (editors.isEmpty())
        return true;
    bool closingFailed = false;
    // close Editor History list
    windowPopup()->setVisible(false);


    EditorView *currentView = currentEditorView();

    // go through all editors to close and
    // 1. ask all core listeners to check whether the editor can be closed
    // 2. keep track of the document and all the editors that might remain open for it
    QSet<IEditor*> acceptedEditors;
    QHash<IDocument *, QList<IEditor *> > editorsForDocuments;
    for (IEditor *editor : std::as_const(editors)) {
        bool editorAccepted = true;
        const QList<std::function<bool(IEditor *)>> listeners = d->m_closeEditorListeners;
        for (const std::function<bool(IEditor *)> &listener : listeners) {
            if (!listener(editor)) {
                editorAccepted = false;
                closingFailed = true;
                break;
            }
        }
        if (editorAccepted) {
            acceptedEditors.insert(editor);
            IDocument *document = editor->document();
            if (!editorsForDocuments.contains(document)) // insert the document to track
                editorsForDocuments.insert(document, DocumentModel::editorsForDocument(document));
            // keep track that we'll close this editor for the document
            editorsForDocuments[document].removeAll(editor);
        }
    }
    if (acceptedEditors.isEmpty())
        return false;

    //ask whether to save modified documents that we are about to close
    if (flag == CloseFlag::CloseWithAsking) {
        // Check for which documents we will close all editors, and therefore might have to ask the user
        QList<IDocument *> documentsToClose;
        for (auto i = editorsForDocuments.constBegin(); i != editorsForDocuments.constEnd(); ++i) {
            if (i.value().isEmpty())
                documentsToClose.append(i.key());
        }

        bool cancelled = false;
        QList<IDocument *> rejectedList;
        DocumentManager::saveModifiedDocuments(documentsToClose, QString(), &cancelled,
                                               QString(), nullptr, &rejectedList);
        if (cancelled)
            return false;
        if (!rejectedList.isEmpty()) {
            closingFailed = true;
            QSet<IEditor*> skipSet = Utils::toSet(DocumentModel::editorsForDocuments(rejectedList));
            acceptedEditors = acceptedEditors.subtract(skipSet);
        }
    }
    if (acceptedEditors.isEmpty())
        return false;

    // save editor states
    for (IEditor *editor : std::as_const(acceptedEditors)) {
        if (!editor->document()->filePath().isEmpty() && !editor->document()->isTemporary()) {
            QByteArray state = editor->saveState();
            if (!state.isEmpty())
                d->m_editorStates.insert(editor->document()->filePath().toUrlishString(), QVariant(state));
        }
    }

    EditorView *focusView = nullptr;

    // Remove accepted editors from document model/manager and context list,
    // and sort them per view, so we can remove them from views in an orderly
    // manner.
    QList<std::pair<EditorView *, IEditor *>> editorsPerView;
    for (IEditor *editor : std::as_const(acceptedEditors)) {
        emit m_instance->editorAboutToClose(editor);
        const DocumentModel::Entry *entry = DocumentModel::entryForDocument(editor->document());
        // If the file is pinned, closing it should remove the editor but keep it in Open Documents.
        const bool isPinned = QTC_GUARD(entry) && entry->pinned;
        const bool removeSuspendedEntry = !isPinned && flag != CloseFlag::Suspend
                                          && flag != CloseFlag::SuspendRemoveTab;
        removeEditor(editor, removeSuspendedEntry);
        if (EditorView *view = viewForEditor(editor)) {
            editorsPerView.append({view, editor});
            if (QApplication::focusWidget()
                && QApplication::focusWidget() == editor->widget()->focusWidget()) {
                focusView = view;
            }
        }
    }
    QTC_CHECK(!focusView || focusView == currentView);

    removeEditorsFromViews(
        editorsPerView,
        flag == CloseFlag::Suspend ? EditorView::KeepTab : EditorView::RemoveTab,
        RemoveEditorFlag::EnsureNewEditor);

    emit m_instance->editorsClosed(Utils::toList(acceptedEditors));

    if (focusView)
        activateView(focusView);
    else
        setCurrentView(currentView);

    qDeleteAll(acceptedEditors);

    return !closingFailed;
}

void EditorManagerPrivate::tabClosed(DocumentModel::Entry *entry)
{
    // a tab was closed that wasn't backed by an IEditor (e.g. suspended)
    // close the entry if it was the only one
    const QList<EditorView *> allViews = EditorManagerPrivate::allEditorViews();
    if (Utils::contains(allViews, [entry](EditorView *view) {
            return Utils::contains(
                view->visibleTabs(), Utils::equal(&EditorView::TabData::entry, entry));
        })) {
        return;
    }
    DocumentModelPrivate::removeEntry(entry);
}

// Collects all tabs from the given viewsToClose for which no other tab is shown anywhere.
QSet<DocumentModel::Entry *> EditorManagerPrivate::entriesToCloseForTabbedViews(
    const QSet<EditorView *> &viewsToClose)
{
    QSet<DocumentModel::Entry *> entriesToClose;
    QSet<DocumentModel::Entry *> visibleTabs;
    for (EditorView *view : viewsToClose) {
        if (view->isShowingTabs()) {
            const QList<EditorView::TabData> tabs = view->visibleTabs();
            for (const EditorView::TabData &tab : tabs)
                visibleTabs.insert(tab.entry);
        }
    }
    if (!visibleTabs.isEmpty()) {
        const QSet<EditorView *> allOtherViews
            = toSet(EditorManagerPrivate::allEditorViews()).subtract(viewsToClose);
        for (DocumentModel::Entry *tabEntry : visibleTabs) {
            if (!Utils::contains(allOtherViews, [tabEntry](EditorView *otherView) {
                    return Utils::contains(
                        otherView->visibleTabs(),
                        Utils::equal(&EditorView::TabData::entry, tabEntry));
                })) {
                entriesToClose.insert(tabEntry);
            }
        }
    }
    return entriesToClose;
}

void EditorManagerPrivate::activateView(EditorView *view)
{
    QTC_ASSERT(view, return);
    setCurrentView(view);
    QWidget *focusWidget = view->currentEditor() ? view->currentEditor()->widget() : view;
    focusWidget->setFocus();
    ICore::raiseWindow(focusWidget);
}

void EditorManagerPrivate::restoreEditorState(IEditor *editor)
{
    QTC_ASSERT(editor, return);
    QString fileName = editor->document()->filePath().toUrlishString();
    editor->restoreState(d->m_editorStates.value(fileName).toByteArray());
}

int EditorManagerPrivate::visibleDocumentsCount()
{
    const QList<IEditor *> editors = EditorManager::visibleEditors();
    const int editorsCount = editors.count();
    if (editorsCount < 2)
        return editorsCount;

    QSet<const IDocument *> visibleDocuments;
    for (const IEditor *editor : editors) {
        if (const IDocument *document = editor->document())
            visibleDocuments << document;
    }
    return visibleDocuments.count();
}

static void setView(QList<QPointer<EditorView>> &list, EditorView *view)
{
    // remove view from the list, as well as any deleted views
    Utils::erase(list, [view](QPointer<EditorView> v) { return !v || v == view; });
    list.prepend(view);
}

void EditorManagerPrivate::setCurrentEditor(IEditor *editor, bool ignoreNavigationHistory)
{
    IEditor *previousEditor = d->m_currentEditor;
    EditorView *previousView = d->m_currentView.isEmpty() ? nullptr : d->m_currentView.constFirst();
    EditorView *view = editor ? viewForEditor(editor) : previousView;

    if (editor != previousEditor) {
        emit m_instance->currentEditorAboutToChange(d->m_currentEditor);

        if (d->m_currentEditor && !ignoreNavigationHistory)
            EditorManager::addCurrentPositionToNavigationHistory();

        d->m_currentEditor = editor;
        // update global history
        if (editor)
            EditorView::updateEditorHistory(editor, d->m_globalHistory);
    }

    if (QTC_GUARD(view)) { // we should always have a view
        setView(d->m_currentView, view);
        view->setCurrentEditor(editor);
    }

    updateActions();

    if (QTC_GUARD(!d->m_currentView.isEmpty()) && d->m_currentView.constFirst() != previousView)
        emit d->currentViewChanged();
    if (d->m_currentEditor != previousEditor)
        emit m_instance->currentEditorChanged(d->m_currentEditor);
}

void EditorManagerPrivate::setCurrentView(EditorView *view)
{
    QTC_ASSERT(view, return); // we should always have a view
    // currentView can empty if and only if we are currently initializing and setting
    // the main editor area's view as the current one
    if (d->m_currentView.isEmpty() || view != d->m_currentView.constFirst()) {
        EditorView *previousView = d->m_currentView.isEmpty() ? nullptr
                                                              : d->m_currentView.constFirst();
        setView(d->m_currentView, view);

        if (previousView)
            previousView->update();
        if (d->m_currentView.constFirst())
            view->update();

        emit d->currentViewChanged();
    }

    setCurrentEditor(view->currentEditor());
}

EditorArea *EditorManagerPrivate::findEditorArea(const EditorView *view, int *areaIndex)
{
    if (!view)
        return nullptr;
    EditorArea *area = view->editorArea();
    QTC_ASSERT(area, return nullptr);
    int index = d->m_editorAreas.indexOf(area);
    QTC_ASSERT(index >= 0, return nullptr);
    if (areaIndex)
        *areaIndex = index;
    return area;
}

void EditorManagerPrivate::closeView(EditorView *view)
{
    if (!view)
        return;

    // Check if there are documents that should be closed because there are no other views
    // with tabs open for it.
    const QSet<DocumentModel::Entry *> entriesToClose
        = EditorManagerPrivate::entriesToCloseForTabbedViews({view});
    if (!entriesToClose.isEmpty() && !EditorManager::closeDocuments(toList(entriesToClose)))
        return;

    const QList<IEditor *> editorsToDelete = emptyView(view);
    EditorView *newCurrent = view->editorArea()->unsplit(view);
    if (newCurrent)
        EditorManagerPrivate::activateView(newCurrent);
    deleteEditors(editorsToDelete);
}

/*!
    Removes all editors from the view and from the document model, taking care of
    the handling of editors that are the last ones for the document.
    Returns the list of editors that were actually removed from the document model and
    need to be deleted with \c EditorManagerPrivate::deleteEditors.
    \internal
*/
const QList<IEditor *> EditorManagerPrivate::emptyView(EditorView *view)
{
    if (!view)
        return {};
    const QList<IEditor *> editors = view->editors();
    QList<IEditor *> removedEditors;
    for (IEditor *editor : editors) {
        if (DocumentModel::editorsForDocument(editor->document()).size() == 1) {
            // it's the only editor for that file
            // so we need to keep it around (--> in the editor model)
            if (EditorManager::currentEditor() == editor) {
                // we don't want a current editor that is not open in a view
                setCurrentView(view);
                setCurrentEditor(nullptr);
            }
            removeEditorsFromViews({{view, editor}}, EditorView::RemoveTab, RemoveEditorFlag::None);
        } else {
            emit m_instance->editorAboutToClose(editor);
            removeEditor(editor, true /*=removeSuspendedEntry, but doesn't matter since it's not the last editor anyhow*/);
            removeEditorsFromViews({{view, editor}}, EditorView::RemoveTab, RemoveEditorFlag::None);
            removedEditors.append(editor);
        }
    }
    return removedEditors;
}

/*!
    Signals editorsClosed() and deletes the editors.
    \internal
*/
void EditorManagerPrivate::deleteEditors(const QList<IEditor *> &editors)
{
    if (!editors.isEmpty()) {
        emit m_instance->editorsClosed(editors);
        qDeleteAll(editors);
    }
}

void EditorManagerPrivate::setShowingTabs(bool visible)
{
    const QList<EditorView *> allViews = allEditorViews();
    for (EditorView *view : allViews)
        view->setTabsVisible(visible);
    d->m_gotoNextTabAction->setVisible(visible);
    d->m_gotoPreviousTabAction->setVisible(visible);
    if (visible) {
        d->m_gotoNextTabCommand->removeAttribute(Command::CA_Hide);
        d->m_gotoPreviousTabCommand->removeAttribute(Command::CA_Hide);
    } else {
        d->m_gotoNextTabCommand->setAttribute(Command::CA_Hide);
        d->m_gotoPreviousTabCommand->setAttribute(Command::CA_Hide);
    }
}

EditorWindow *EditorManagerPrivate::createEditorWindow()
{
    auto win = new EditorWindow;
    addEditorArea(win->editorArea());
    return win;
}

void EditorManagerPrivate::addEditorArea(EditorArea *area)
{
    d->m_editorAreas.append(area);
    connect(area, &QObject::destroyed, d, &EditorManagerPrivate::editorAreaDestroyed);
    connect(
        area,
        &EditorArea::hidden,
        d,
        [area = QPointer<EditorArea>(area)] {
            if (ExtensionSystem::PluginManager::isShuttingDown())
                return;
            // The connection is queued, because the hiding might be very short term, e.g.
            // when switching between Edit and Debug modes. Check if it is still hidden.
            const auto isReallyVisibile = [](QWidget *w) {
                return w && w->isVisible() && !w->window()->isMinimized();
            };
            if (isReallyVisibile(area))
                return;

            // Hack for the case that the hidden area has the current editor,
            // and that is currently shown in Design mode. We may not switch the
            // current editor (so not switch the current view) in that case.
            // QTCREATORBUG-31378
            // It would be good if the Design mode didn't rely on the current
            // editor, instead.
            if (area->currentView() == currentEditorView()
                && ModeManager::currentModeId() == Constants::MODE_DESIGN) {
                return;
            }

            // In case the hidden editor area has the current view, look for a view
            // that is not hidden, iterating through the history of current views.
            // This could be the first==current view (which results in a no-op).
            for (const QPointer<EditorView> &view : std::as_const(d->m_currentView)) {
                if (isReallyVisibile(view)) {
                    setCurrentView(view);
                    return;
                }
            }
            // If we didn't find a better view, so be it
        },
        Qt::QueuedConnection);
    connect(area, &EditorArea::splitStateChanged, d, &EditorManagerPrivate::viewCountChanged);
    emit d->viewCountChanged();
}

void EditorManagerPrivate::splitNewWindow(EditorView *view)
{
    QTC_ASSERT(view, return);
    IEditor *editor = view->currentEditor();
    IEditor *newEditor = nullptr;
    const QByteArray state = editor ? editor->saveState() : QByteArray();
    if (editor && editor->duplicateSupported())
        newEditor = EditorManagerPrivate::duplicateEditor(editor);
    else
        newEditor = editor; // move to the new view

    EditorWindow *win = createEditorWindow();
    win->show();
    ICore::raiseWindow(win);
    if (newEditor) {
        activateEditor(
            win->editorArea()->currentView(), newEditor, EditorManager::IgnoreNavigationHistory);
        // possibly adapts old state to new layout
        newEditor->restoreState(state);
    } else {
        win->editorArea()->currentView()->setFocus();
    }
    updateActions();
}

IEditor *EditorManagerPrivate::pickUnusedEditor(EditorView **foundView)
{
    const QList<IEditor *> editors = DocumentModel::editorsForOpenedDocuments();
    for (IEditor *editor : editors) {
        EditorView *view = viewForEditor(editor);
        if (!view || view->currentEditor() != editor) {
            if (foundView)
                *foundView = view;
            return editor;
        }
    }
    return nullptr;
}

/* Adds the file name to the recent files if there is at least one non-temporary editor for it */
void EditorManagerPrivate::addDocumentToRecentFiles(IDocument *document)
{
    if (document->isTemporary())
        return;
    DocumentModel::Entry *entry = DocumentModel::entryForDocument(document);
    if (!entry)
        return;
    DocumentManager::addToRecentFiles(document->filePath(), entry->id());
}

void EditorManagerPrivate::updateAutoSave()
{
    if (systemSettings().autoSaveModifiedFiles())
        d->m_autoSaveTimer->start(systemSettings().autoSaveInterval() * (60 * 1000));
    else
        d->m_autoSaveTimer->stop();
}

void EditorManagerPrivate::handleFileRenamed(
    const Utils::FilePath &originalFilePath, const Utils::FilePath &newFilePath, Id originalType)
{
    if (!originalType.isValid()) {
        // check if the file is open in an editor, if not, nothing to do
        const QList<IEditor *> editorsOpenForFile = DocumentModel::editorsForFilePath(newFilePath);
        if (editorsOpenForFile.isEmpty())
            return;
        originalType = editorsOpenForFile.first()->document()->id();
    }

    // If the MIME type changed, it might make more sense to re-open the file
    // in a different editor type. (Things like undo history are not transferred then though.)
    // If the original editor was not the default editor, try to stay with that, though.
    // E.g. if a .md file is open in the base text editor, and then renamed to .cpp,
    // just stay with the base text editor, because that was not the default editor and is
    // still a valid editor for the new MIME type.
    const MimeType originalMt = Utils::mimeTypeForFile(originalFilePath);
    const MimeType newMt = Utils::mimeTypeForFile(newFilePath);
    if (newMt != originalMt) {
        const EditorFactories originalFactories = Utils::filtered(
            IEditorFactory::preferredEditorTypes(originalFilePath),
            &IEditorFactory::isInternalEditor);
        const bool originalIsDefault = originalFactories.isEmpty()
                                           ? true
                                           : originalFactories.first()->id() == originalType;

        const EditorFactories newFactories = Utils::filtered(
            IEditorFactory::preferredEditorTypes(newFilePath), &IEditorFactory::isInternalEditor);
        const Id newDefault = newFactories.isEmpty() ? Id() : newFactories.first()->id();
        const bool defaultIsDifferent = newDefault != originalType;
        const auto newContainsOriginal = [originalType, newFactories] {
            return Utils::contains(newFactories, [originalType](IEditorFactory *f) {
                return f->id() == originalType;
            });
        };

        if ((originalIsDefault && defaultIsDifferent)
            || (!originalIsDefault && !newContainsOriginal())) {
            QMetaObject::invokeMethod(m_instance, [newFilePath] {
                EditorManagerPrivate::openEditorWith(newFilePath, {});
            });
        }
    }
}

void EditorManagerPrivate::updateMakeWritableWarning()
{
    IDocument *document = EditorManager::currentDocument();
    QTC_ASSERT(document, return);
    bool ww = document->isModified() && document->isFileReadOnly();
    if (ww != document->hasWriteWarning()) {
        document->setWriteWarning(ww);

        // Do this after setWriteWarning so we don't re-evaluate this part even
        // if we do not really show a warning.
        bool promptVCS = false;
        const FilePath directory = document->filePath().parentDir();
        IVersionControl *versionControl = VcsManager::findVersionControlForDirectory(directory);
        if (versionControl && versionControl->openSupportMode(document->filePath()) != IVersionControl::NoOpen) {
            if (versionControl->settingsFlags() & IVersionControl::AutoOpen) {
                vcsOpenCurrentEditor();
                ww = false;
            } else {
                promptVCS = true;
            }
        }

        if (ww) {
            // we are about to change a read-only file, warn user
            if (promptVCS) {
                InfoBarEntry info(Id(kMakeWritableWarning),
                                  ::Core::Tr::tr("<b>Warning:</b> This file was not opened in %1 yet.")
                                  .arg(versionControl->displayName()));
                info.addCustomButton(::Core::Tr::tr("Open"), &vcsOpenCurrentEditor);
                document->infoBar()->addInfo(info);
            } else {
                InfoBarEntry info(Id(kMakeWritableWarning),
                                  ::Core::Tr::tr("<b>Warning:</b> You are changing a read-only file."));
                info.addCustomButton(::Core::Tr::tr("Make Writable"), &makeCurrentEditorWritable);
                document->infoBar()->addInfo(info);
            }
        } else {
            document->infoBar()->removeInfo(Id(kMakeWritableWarning));
        }
    }
}

void EditorManagerPrivate::setupSaveActions(IDocument *document, QAction *saveAction,
                                            QAction *saveAsAction, QAction *revertToSavedAction)
{
    const bool hasFile = document && !document->filePath().isEmpty();
    saveAction->setEnabled(document && (document->isModified() || !hasFile));
    saveAsAction->setEnabled(document && document->isSaveAsAllowed());
    revertToSavedAction->setEnabled(hasFile);

    if (document && !document->displayName().isEmpty()) {
        const QString quotedName = QLatin1Char('"')
                + Utils::quoteAmpersands(document->displayName()) + QLatin1Char('"');
        saveAction->setText(::Core::Tr::tr("&Save %1").arg(quotedName));
        saveAsAction->setText(::Core::Tr::tr("Save %1 &As...").arg(quotedName));
        revertToSavedAction->setText(document->isModified()
                                     ? ::Core::Tr::tr("Revert %1 to Saved").arg(quotedName)
                                     : ::Core::Tr::tr("Reload %1").arg(quotedName));
    } else {
        saveAction->setText(::Core::Tr::tr("&Save"));
        saveAsAction->setText(::Core::Tr::tr("Save &As..."));
        revertToSavedAction->setText(::Core::Tr::tr("Revert to Saved"));
    }
}

void EditorManagerPrivate::updateActions()
{
    IDocument *curDocument = EditorManager::currentDocument();
    const int viewCount = allEditorViews().size();
    const int openedCount = DocumentModel::entryCount();

    if (curDocument)
        updateMakeWritableWarning();

    QString quotedName;
    if (curDocument)
        quotedName = QLatin1Char('"') + Utils::quoteAmpersands(curDocument->displayName())
                + QLatin1Char('"');
    setupSaveActions(curDocument, d->m_saveAction, d->m_saveAsAction, d->m_revertToSavedAction);

    d->m_closeCurrentEditorAction->setEnabled(curDocument);
    d->m_closeCurrentEditorAction->setText(::Core::Tr::tr("Close %1").arg(quotedName));
    d->m_closeAllEditorsAction->setEnabled(openedCount > 0);
    d->m_closeOtherDocumentsAction->setEnabled(openedCount > 1);
    d->m_closeOtherDocumentsAction->setText((openedCount > 1 ? ::Core::Tr::tr("Close All Except %1").arg(quotedName)
                                                             : ::Core::Tr::tr("Close Others")));

    d->m_closeAllEditorsExceptVisibleAction->setEnabled(visibleDocumentsCount() < openedCount);

    d->m_gotoNextDocHistoryAction->setEnabled(openedCount != 0);
    d->m_gotoPreviousDocHistoryAction->setEnabled(openedCount != 0);
    EditorView *view  = currentEditorView();
    const bool isMultiTab = view ? (view->tabs().size() > 1) : false;
    d->m_gotoPreviousTabAction->setEnabled(isMultiTab);
    d->m_gotoNextTabAction->setEnabled(isMultiTab);
    d->m_goBackAction->setEnabled(view ? view->canGoBack() : false);
    d->m_goForwardAction->setEnabled(view ? view->canGoForward() : false);
    d->m_nextDocAction->setEnabled(DocumentModel::entryCount() > 1);
    d->m_prevDocAction->setEnabled(DocumentModel::entryCount() > 1);
    d->m_reopenLastClosedDocumenAction->setEnabled(view ? view->canReopen() : false);

    const bool hasSplitter = view && view->isInSplit();
    d->m_removeCurrentSplitAction->setEnabled(hasSplitter);
    d->m_removeAllSplitsAction->setEnabled(hasSplitter);
    d->m_gotoNextSplitAction->setEnabled(hasSplitter || d->m_editorAreas.size() > 1);
    const bool splitActionsEnabled = viewCount < kMaxViews;
    d->m_splitAction->setEnabled(splitActionsEnabled);
    d->m_splitNewWindowAction->setEnabled(splitActionsEnabled);
    d->m_splitSideBySideAction->setEnabled(splitActionsEnabled);
}

void EditorManagerPrivate::updateWindowTitleForDocument(IDocument *document, QWidget *window)
{
    QTC_ASSERT(window, return);
    QString windowTitle;
    const QString dashSep(" - ");

    const QString documentName = document ? document->displayName() : QString();
    if (!documentName.isEmpty())
        windowTitle.append(documentName);

    const Utils::FilePath filePath = document ? document->filePath().absoluteFilePath()
                                              : Utils::FilePath();
    const QString windowTitleAddition = d->m_titleAdditionHandler
            ? d->m_titleAdditionHandler(filePath)
            : QString();
    if (!windowTitleAddition.isEmpty()) {
        if (!windowTitle.isEmpty())
            windowTitle.append(" ");
        windowTitle.append(windowTitleAddition);
    }

    const QString windowTitleVcsTopic = d->m_titleVcsTopicHandler
           ? d->m_titleVcsTopicHandler(filePath)
           : QString();
    if (!windowTitleVcsTopic.isEmpty()) {
        if (!windowTitle.isEmpty())
            windowTitle.append(" ");
        windowTitle.append(QStringLiteral("[") + windowTitleVcsTopic + QStringLiteral("]"));
    }

    const QString sessionTitle = d->m_sessionTitleHandler
           ? d->m_sessionTitleHandler(filePath)
           : QString();
    if (!sessionTitle.isEmpty()) {
        if (!windowTitle.isEmpty())
            windowTitle.append(dashSep);
        windowTitle.append(sessionTitle);
    }

    if (!windowTitle.isEmpty())
        windowTitle.append(dashSep);
    windowTitle.append(QGuiApplication::applicationDisplayName());
    window->window()->setWindowTitle(windowTitle);
    window->window()->setWindowFilePath(filePath.path());

    if (HostOsInfo::isMacHost()) {
        if (document)
            window->window()->setWindowModified(document->isModified());
        else
            window->window()->setWindowModified(false);
    }
}

void EditorManagerPrivate::updateWindowTitle()
{
    EditorArea *mainArea = mainEditorArea();
    IDocument *document = mainArea->currentDocument();
    updateWindowTitleForDocument(document, mainArea->window());
}

void EditorManagerPrivate::gotoNextDocHistory()
{
    OpenEditorsWindow *dialog = windowPopup();
    if (dialog->isVisible()) {
        dialog->selectNextEditor();
    } else {
        EditorView *view = currentEditorView();
        QTC_ASSERT(view, return);
        dialog->setEditors(d->m_globalHistory, view);
        dialog->selectNextEditor();
        showPopupOrSelectDocument();
    }
}

void EditorManagerPrivate::gotoPreviousDocHistory()
{
    OpenEditorsWindow *dialog = windowPopup();
    if (dialog->isVisible()) {
        dialog->selectPreviousEditor();
    } else {
        EditorView *view = currentEditorView();
        QTC_ASSERT(view, return);
        dialog->setEditors(d->m_globalHistory, view);
        dialog->selectPreviousEditor();
        showPopupOrSelectDocument();
    }
}

void EditorManagerPrivate::gotoNextTab()
{
    EditorView *view = currentEditorView();
    QTC_ASSERT(view, return);
    view->gotoNextTab();
}

void EditorManagerPrivate::gotoPreviousTab()
{
    EditorView *view = currentEditorView();
    QTC_ASSERT(view, return);
    view->gotoPreviousTab();
}

void EditorManagerPrivate::gotoLastEditLocation()
{
    EditorView *view = currentEditorView();
    QTC_ASSERT(view, return);
    view->goToEditLocation(d->m_globalLastEditLocation);
}

void EditorManagerPrivate::gotoNextSplit()
{
    EditorView *view = currentEditorView();
    QTC_ASSERT(view, return);
    EditorView *nextView = view->findNextView();
    if (!nextView) {
        // we are in the "last" view in this editor area
        int index = -1;
        EditorArea *area = findEditorArea(view, &index);
        QTC_ASSERT(area, return);
        QTC_ASSERT(index >= 0 && index < d->m_editorAreas.size(), return);
        // find next editor area. this might be the same editor area if there's only one.
        int nextIndex = index + 1;
        if (nextIndex >= d->m_editorAreas.size())
            nextIndex = 0;
        nextView = d->m_editorAreas.at(nextIndex)->findFirstView();
    }

    if (QTC_GUARD(nextView))
        activateView(nextView);
}

void EditorManagerPrivate::gotoPreviousSplit()
{
    EditorView *view = currentEditorView();
    QTC_ASSERT(view, return);
    EditorView *prevView = view->findPreviousView();
    if (!prevView) {
        // we are in the "first" view in this editor area
        int index = -1;
        EditorArea *area = findEditorArea(view, &index);
        QTC_ASSERT(area, return);
        QTC_ASSERT(index >= 0 && index < d->m_editorAreas.size(), return);
        // find previous editor area. this might be the same editor area if there's only one.
        int nextIndex = index - 1;
        if (nextIndex < 0)
            nextIndex = d->m_editorAreas.count() - 1;
        prevView = d->m_editorAreas.at(nextIndex)->findLastView();
    }

    if (QTC_GUARD(prevView))
        activateView(prevView);
}

void EditorManagerPrivate::addClosedDocumentToCloseHistory(IEditor *editor)
{
    EditorView *view = EditorManagerPrivate::viewForEditor(editor);
    // Editors can be owned by the document model.
    // E.g. split, open some document there, modify it, close the split
    // -> we do not close the editor, because we'd lose the modifications.
    if (!view)
        return;
    view->addClosedEditorToCloseHistory(editor);
    EditorManagerPrivate::updateActions();
}

void EditorManagerPrivate::reopenLastClosedDocument()
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    view->reopenLastClosedDocument();
    EditorManagerPrivate::updateActions();
}

void EditorManagerPrivate::makeCurrentEditorWritable()
{
    if (IDocument* doc = EditorManager::currentDocument())
        makeFileWritable(doc);
}

void EditorManagerPrivate::setPlaceholderText(const QString &text)
{
    if (d->m_placeholderText == text)
        return;
    d->m_placeholderText = text;
    emit d->placeholderTextChanged(d->m_placeholderText);
}

QString EditorManagerPrivate::placeholderText()
{
    return d->m_placeholderText;
}

void EditorManagerPrivate::vcsOpenCurrentEditor()
{
    IDocument *document = EditorManager::currentDocument();
    if (!document)
        return;

    const FilePath directory = document->filePath().parentDir();
    IVersionControl *versionControl = VcsManager::findVersionControlForDirectory(directory);
    if (!versionControl || versionControl->openSupportMode(document->filePath()) == IVersionControl::NoOpen)
        return;

    if (!versionControl->vcsOpen(document->filePath())) {
        QMessageBox::warning(ICore::dialogParent(), ::Core::Tr::tr("Cannot Open File"),
                             ::Core::Tr::tr("Cannot open the file for editing with VCS."));
    }
}

void EditorManagerPrivate::handleDocumentStateChange(IDocument *document)
{
    updateActions();
    if (!document->isModified())
        document->removeAutoSaveFile();
    if (EditorManager::currentDocument() == document)
        emit m_instance->currentDocumentStateChanged();
    emit m_instance->documentStateChanged(document);
}

void EditorManagerPrivate::editorAreaDestroyed(QObject *area)
{
    QWidget *activeWin = QApplication::activeWindow();
    EditorArea *newActiveArea = nullptr;
    for (int i = 0; i < d->m_editorAreas.size(); ++i) {
        EditorArea *r = d->m_editorAreas.at(i);
        if (r == area) {
            d->m_editorAreas.removeAt(i);
            --i; // we removed the current one
        } else if (r->window() == activeWin) {
            // TODO this doesn't work well in case of multiple areas in the same window
            // e.g if Edit, Design, and Debug mode have their own editor areas
            newActiveArea = r;
        }
    }
    // check if the destroyed editor area had the current view or current editor
    if (!currentEditorView()) {
        // we need to set a new current editor or view
        if (!newActiveArea) {
            // some window managers behave weird and don't activate another window
            // or there might be a Qt Creator toplevel activated that doesn't have editor windows
            newActiveArea = d->m_editorAreas.first();
        }

        // check if the focusWidget points to some view
        EditorView *focusView = nullptr;
        QWidget *candidate = newActiveArea->focusWidget();
        while (candidate && candidate != newActiveArea) {
            if ((focusView = qobject_cast<EditorView *>(candidate)))
                break;
            candidate = candidate->parentWidget();
        }
        // focusWidget might have been 0
        if (!focusView)
            focusView = newActiveArea->findFirstView();
        if (QTC_GUARD(focusView))
            EditorManagerPrivate::activateView(focusView);
    }
    emit viewCountChanged();
}

void EditorManagerPrivate::autoSave()
{
    QStringList errors;
    // FIXME: the saving should be staggered
    const QList<IDocument *> documents = DocumentModel::openedDocuments();
    for (IDocument *document : documents) {
        if (!document->isModified() || !document->shouldAutoSave())
            continue;
        const FilePath saveName = autoSaveName(document->filePath());
        const FilePath savePath = saveName.absolutePath();
        if (document->filePath().isEmpty()
                || !savePath.isWritableDir()) // FIXME: save them to a dedicated directory
            continue;
        if (Result<> res = document->autoSave(saveName); !res)
            errors << res.error();
    }
    if (!errors.isEmpty())
        QMessageBox::critical(ICore::dialogParent(),
                              ::Core::Tr::tr("File Error"),
                              errors.join(QLatin1Char('\n')));
    emit m_instance->autoSaved();
}

void EditorManagerPrivate::handleContextChange(const QList<IContext *> &context)
{
    if (debugEditorManager)
        qDebug() << Q_FUNC_INFO;
    d->m_scheduledCurrentEditor = nullptr;
    IEditor *editor = nullptr;
    for (IContext *c : context)
        if ((editor = qobject_cast<IEditor*>(c)))
            break;
    if (editor && editor != d->m_currentEditor) {
        d->m_scheduledCurrentEditor = editor;
        // Delay actually setting the current editor to after the current event queue has been handled
        // Without doing this, e.g. clicking into projects tree or locator would always open editors
        // in the main window. That is because clicking anywhere in the main window (even over e.g.
        // the locator line edit) first activates the window and sets focus to its focus widget.
        // Only afterwards the focus is shifted to the widget that received the click.

        // 1) During this event handling, focus landed in the editor.
        // 2) During the following event handling, focus might change to the project tree.
        // So, delay setting the current editor by two events.
        // If focus changes to e.g. the project tree in (2), then m_scheduledCurrentEditor is set to
        // nullptr, and the setCurrentEditorFromContextChange call becomes a no-op.
        QMetaObject::invokeMethod(
            d,
            [] {
                QMetaObject::invokeMethod(d,
                                          &EditorManagerPrivate::setCurrentEditorFromContextChange,
                                          Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    } else {
        updateActions();
    }
}

bool EditorManagerPrivate::saveDocument(IDocument *document)
{
    if (!document)
        return false;

    document->checkPermissions();

    if (document->filePath().isEmpty())
        return saveDocumentAs(document);

    bool success = false;
    bool isReadOnly;

    emit m_instance->aboutToSave(document);
    // try saving, no matter what isReadOnly tells us
    success = DocumentManager::saveDocument(document, FilePath(), &isReadOnly);

    if (!success && isReadOnly) {
        MakeWritableResult answer = makeFileWritable(document);
        if (answer == Failed)
            return false;
        if (answer == SavedAs)
            return true;

        document->checkPermissions();

        success = DocumentManager::saveDocument(document);
    }

    if (success) {
        addDocumentToRecentFiles(document);
        emit m_instance->saved(document);
    }

    return success;
}

bool EditorManagerPrivate::saveDocumentAs(IDocument *document)
{
    if (!document)
        return false;

    const FilePath originalFilePath = document->filePath();

    const FilePath absoluteFilePath = DocumentManager::getSaveAsFileName(document);
    if (absoluteFilePath.isEmpty())
        return false;

    if (DocumentManager::filePathKey(absoluteFilePath, DocumentManager::ResolveLinks)
        != DocumentManager::filePathKey(document->filePath(), DocumentManager::ResolveLinks)) {
        // close existing editors for the new file name
        IDocument *otherDocument = DocumentModel::documentForFilePath(absoluteFilePath);
        if (otherDocument)
            EditorManager::closeDocuments({otherDocument}, false);
    }

    emit m_instance->aboutToSave(document);
    const bool success = DocumentManager::saveDocument(document, absoluteFilePath);
    document->checkPermissions();

    if (success) {
        // if document had been temporary before (scratch buffer) - remove the temporary flag
        document->setTemporary(false);

        addDocumentToRecentFiles(document);
        emit m_instance->saved(document);
    }

    updateActions();

    handleFileRenamed(originalFilePath, document->filePath(), document->id());
    return success;
}

void EditorManagerPrivate::closeAllEditorsExceptVisible()
{
    DocumentModelPrivate::removeAllSuspendedEntries(DocumentModelPrivate::DoNotRemovePinnedFiles);
    QList<IDocument *> documentsToClose = DocumentModel::openedDocuments();
    // Remove all pinned files from the list of files to close.
    documentsToClose = Utils::filtered(documentsToClose, [](IDocument *document) {
        DocumentModel::Entry *entry = DocumentModel::entryForDocument(document);
        return !entry->pinned;
    });
    const QList<IEditor *> editors = EditorManager::visibleEditors();
    for (const IEditor *editor : editors)
        documentsToClose.removeAll(editor->document());
    EditorManager::closeDocuments(documentsToClose, true);
}

void EditorManagerPrivate::revertToSaved(IDocument *document)
{
    if (!document)
        return;
    const FilePath filePath =  document->filePath();
    if (filePath.isEmpty())
        return;
    if (document->isModified()) {
        QMessageBox msgBox(QMessageBox::Question,
                           ::Core::Tr::tr("Revert to Saved"),
                           ::Core::Tr::tr("You will lose your current changes if you proceed reverting %1.")
                               .arg(filePath.toUserOutput()),
                           QMessageBox::Yes | QMessageBox::No,
                           ICore::dialogParent());
        msgBox.button(QMessageBox::Yes)->setText(::Core::Tr::tr("Proceed"));
        msgBox.button(QMessageBox::No)->setText(::Core::Tr::tr("Cancel"));

        QPushButton *diffButton = nullptr;
        auto diffService = DiffService::instance();
        if (diffService)
            diffButton = msgBox.addButton(::Core::Tr::tr("Cancel && &Diff"), QMessageBox::RejectRole);

        msgBox.setDefaultButton(QMessageBox::No);
        msgBox.setEscapeButton(QMessageBox::No);
        if (msgBox.exec() == QMessageBox::No)
            return;

        if (diffService && msgBox.clickedButton() == diffButton) {
            diffService->diffModifiedFiles({filePath});
            return;
        }
    }

    if (Result<> res = document->reload(IDocument::FlagReload, IDocument::TypeContents); !res)
        QMessageBox::critical(ICore::dialogParent(), ::Core::Tr::tr("File Error"), res.error());
}

void EditorManagerPrivate::autoSuspendDocuments()
{
    if (!systemSettings().autoSuspendEnabled())
        return;

    auto visibleDocuments = Utils::transform<QSet>(EditorManager::visibleEditors(),
                                                   &IEditor::document);
    int keptEditorCount = 0;
    QList<IDocument *> documentsToSuspend;
    const int minDocumentCount = systemSettings().autoSuspendMinDocumentCount();
    for (const EditLocation &editLocation : std::as_const(d->m_globalHistory)) {
        IDocument *document = editLocation.document;
        if (!document || !document->isSuspendAllowed() || document->isModified()
                || document->isTemporary() || document->filePath().isEmpty()
                || visibleDocuments.contains(document))
            continue;
        if (keptEditorCount >= minDocumentCount)
            documentsToSuspend.append(document);
        else
            ++keptEditorCount;
    }
    closeEditors(DocumentModel::editorsForDocuments(documentsToSuspend), CloseFlag::Suspend);
}

void EditorManagerPrivate::split(Qt::Orientation orientation)
{
    EditorView *view = currentEditorView();

    if (view)
        activateView(view->split(orientation));

    updateActions();
}

void EditorManagerPrivate::removeCurrentSplit()
{
    EditorView *viewToClose = currentEditorView();

    QTC_ASSERT(viewToClose, return);
    QTC_ASSERT(viewToClose->isInSplit(), return);

    closeView(viewToClose);
    updateActions();
}

void EditorManagerPrivate::removeAllSplits()
{
    EditorView *view = currentEditorView();
    QTC_ASSERT(view, return);
    EditorArea *currentArea = view->editorArea();
    QTC_ASSERT(currentArea, return);
    // Check if there are documents that should be closed because there are no other views
    // with tabs open for it.
    QSet<EditorView *> viewsToClose; // all views from the area except the current one
    EditorView *current = currentArea->findFirstView();
    while (current) {
        if (current != view)
            viewsToClose.insert(current);
        current = current->findNextView();
    }
    const QSet<DocumentModel::Entry *> entriesToClose
        = EditorManagerPrivate::entriesToCloseForTabbedViews(viewsToClose);
    if (!entriesToClose.isEmpty() && !EditorManager::closeDocuments(toList(entriesToClose)))
        return;
    currentArea->unsplitAll(view);
}

void EditorManagerPrivate::setCurrentEditorFromContextChange()
{
    if (!d->m_scheduledCurrentEditor)
        return;
    IEditor *newCurrent = d->m_scheduledCurrentEditor;
    d->m_scheduledCurrentEditor = nullptr;
    setCurrentEditor(newCurrent);
}

EditorView *EditorManagerPrivate::currentEditorView()
{
    QTC_ASSERT(d->m_currentView.size() > 0, return nullptr);
    return d->m_currentView.constFirst();
}

QList<EditorView *> EditorManagerPrivate::allEditorViews()
{
    QList<EditorView *> views;
    for (const EditorArea *area : std::as_const(d->m_editorAreas)) {
        EditorView *firstView = area->findFirstView();
        EditorView *view = firstView;
        if (view) {
            do {
                views.append(view);
                view = view->findNextView();
                // we start with firstView and shouldn't have cycles
                QTC_ASSERT(view != firstView, break);
            } while (view);
        }
    }
    return views;
}

bool EditorManagerPrivate::hasMoreThanOneview()
{
    if (d->m_editorAreas.size() > 1)
        return true;
    QTC_ASSERT(d->m_editorAreas.size() > 0, return false);
    return d->m_editorAreas.constFirst()->hasSplits();
}

} // namespace Internal

/*!
    Returns the pointer to the instance. Only use for connecting to signals.
*/
EditorManager *EditorManager::instance()
{
    return m_instance;
}

/*!
    \internal
*/
EditorManager::EditorManager(QObject *parent) :
    QObject(parent)
{
    m_instance = this;
    d = new EditorManagerPrivate(this);
    d->init();
}

/*!
    \internal
*/
EditorManager::~EditorManager()
{
    delete d;
    m_instance = nullptr;
}

/*!
    Returns the document of the currently active editor.

    \sa currentEditor()
*/
IDocument *EditorManager::currentDocument()
{
    return d->m_currentEditor ? d->m_currentEditor->document() : nullptr;
}

/*!
    Returns the currently active editor.

    \sa currentDocument()
*/
IEditor *EditorManager::currentEditor()
{
    return d->m_currentEditor;
}

/*!
    Closes all open editors. If \a askAboutModifiedEditors is \c true, prompts
    users to save their changes before closing the editors.

    Returns whether all editors were closed.
*/
bool EditorManager::closeAllEditors(bool askAboutModifiedEditors)
{
    DocumentModelPrivate::removeAllSuspendedEntries();
    return closeDocuments(DocumentModel::openedDocuments(), askAboutModifiedEditors);
}

/*!
    Closes all open documents except \a document and pinned files.
*/
void EditorManager::closeOtherDocuments(IDocument *document)
{
    DocumentModelPrivate::removeAllSuspendedEntries(DocumentModelPrivate::DoNotRemovePinnedFiles);
    QList<IDocument *> documentsToClose = DocumentModel::openedDocuments();
    // Remove all pinned files from the list of files to close.
    documentsToClose = Utils::filtered(documentsToClose, [](IDocument *document) {
        DocumentModel::Entry *entry = DocumentModel::entryForDocument(document);
        return !entry->pinned;
    });
    documentsToClose.removeAll(document);
    closeDocuments(documentsToClose, true);
}

/*!
    Closes all open documents except pinned files.

    Returns whether all editors were closed.
*/
bool EditorManager::closeAllDocuments()
{
    // Only close the files that aren't pinned.
    const QList<DocumentModel::Entry *> entriesToClose
            = Utils::filtered(DocumentModel::entries(), Utils::equal(&DocumentModel::Entry::pinned, false));
    return EditorManager::closeDocuments(entriesToClose);
}

/*!
    \internal
*/
void EditorManager::slotCloseCurrentEditorOrDocument()
{
    if (!d->m_currentEditor)
        return;
    d->closeEditorOrDocument(d->m_currentEditor);
}

/*!
    Closes all open documents except the current document.
*/
void EditorManager::closeOtherDocuments()
{
    closeOtherDocuments(currentDocument());
}

static void assignAction(QAction *self, QAction *other)
{
    self->setText(other->text());
    self->setIcon(other->icon());
    self->setShortcut(other->shortcut());
    self->setEnabled(other->isEnabled());
    self->setIconVisibleInMenu(other->isIconVisibleInMenu());
}

/*!
    Adds save, close and other editor context menu items for the document
    \a entry and editor \a editor to the context menu \a contextMenu.
*/
void EditorManager::addSaveAndCloseEditorActions(QMenu *contextMenu, DocumentModel::Entry *entry,
                                                 IEditor *editor)
{
    EditorManagerPrivate::addSaveAndCloseEditorActions(contextMenu, entry, editor);
}

void EditorManagerPrivate::addCopyFilePathActions(
    QMenu *contextMenu, const Utils::FilePath &filePath, Core::IEditor *editor)
{
    QTC_ASSERT(contextMenu, return);
    const bool copyActionsEnabled = !filePath.isEmpty();

    // Copy Full Path
    addMenuAction(contextMenu, ::Core::Tr::tr("Copy Full Path"), copyActionsEnabled, d, [filePath] {
        setClipboardAndSelection(filePath.toUserOutput());
    });

    // Copy Path and Line Number
    if (editor) {
        if (const int lineNumber = editor->currentLine()) {
            addMenuAction(
                contextMenu,
                ::Core::Tr::tr("Copy Path and Line Number"),
                copyActionsEnabled,
                d,
                [filePath, lineNumber] {
                    const QString text = filePath.toUserOutput() + ':'
                                         + QString::number(lineNumber);
                    setClipboardAndSelection(text);
                });
        }
    }

    // Copy File Name
    addMenuAction(contextMenu, ::Core::Tr::tr("Copy File Name"), copyActionsEnabled, d, [filePath] {
        setClipboardAndSelection(filePath.fileName());
    });
}

void EditorManagerPrivate::addSaveAndCloseEditorActions(
    QMenu *contextMenu, DocumentModel::Entry *entry, IEditor *editor, EditorView *view)
{
    QTC_ASSERT(contextMenu, return);

    QPointer<IDocument> contextDocument = entry ? entry->document : nullptr;
    QPointer<IEditor> contextEditor = editor;
    QPointer<EditorView> contextView = view;

    const FilePath filePath = filePathFor(entry, editor);

    addCopyFilePathActions(contextMenu, filePath, editor);

    contextMenu->addSeparator();

    // Save
    QAction *save = addMenuAction(contextMenu, "" /* set below */, true, d, [contextDocument] {
        if (contextDocument)
            d->saveDocument(contextDocument);
    });
    assignAction(save, ActionManager::command(Constants::SAVE)->action());

    // Save As
    QAction *saveAs = addMenuAction(contextMenu, "" /* set below */, true, d, [contextDocument] {
        if (contextDocument)
            d->saveDocumentAs(contextDocument);
    });
    assignAction(saveAs, ActionManager::command(Constants::SAVEAS)->action());

    // Save All
    contextMenu->addAction(ActionManager::command(Constants::SAVEALL)->action());

    // Reload
    QAction *revertToSaved
        = addMenuAction(contextMenu, "" /* set below */, true, d, [contextDocument] {
              if (contextDocument)
                  d->revertToSaved(contextDocument);
          });
    assignAction(revertToSaved, ActionManager::command(Constants::REVERTTOSAVED)->action());

    EditorManagerPrivate::setupSaveActions(contextDocument, save, saveAs, revertToSaved);

    contextMenu->addSeparator();

    const QString quotedDisplayName = entry ? Utils::quoteAmpersands(entry->displayName())
                                            : QString();

    // Close
    addMenuAction(
        contextMenu,
        entry ? ::Core::Tr::tr("Close \"%1\"").arg(quotedDisplayName)
              : ::Core::Tr::tr("Close Editor"),
        entry != nullptr,
        d,
        [contextEditor, contextDocument, contextView] {
            if (contextEditor) {
                d->closeEditorOrDocument(contextEditor);
            } else if (contextDocument) {
                DocumentModel::Entry *entry = DocumentModel::entryForDocument(contextDocument);
                if (entry && contextView && contextView->isShowingTabs()) // editor tab
                    contextView->closeTab(entry);
                else if (entry) // Open Documents view
                    EditorManager::closeDocuments({entry});
                // else the entry vanished somehow, do nothing
            }
        });

    // Close All
    contextMenu->addAction(ActionManager::command(Constants::CLOSEALL)->action());

    // Close Other
    addMenuAction(
        contextMenu,
        entry ? ::Core::Tr::tr("Close All Except \"%1\"").arg(quotedDisplayName)
              : ::Core::Tr::tr("Close Other Editors"),
        contextDocument != nullptr,
        d,
        [contextDocument] {
            if (contextDocument)
                EditorManager::closeOtherDocuments(contextDocument);
        });

    // Close All Except Visible
    contextMenu->addAction(ActionManager::command(Constants::CLOSEALLEXCEPTVISIBLE)->action());
}

/*!
    Adds the pin editor menu items for the document \a entry to the context menu
    \a contextMenu.
*/
void EditorManager::addPinEditorActions(QMenu *contextMenu, DocumentModel::Entry *entry)
{
    const QString quotedDisplayName = entry ? Utils::quoteAmpersands(entry->displayName()) : QString();
    QString text;
    if (entry) {
        text = entry->pinned ? ::Core::Tr::tr("Unpin \"%1\"").arg(quotedDisplayName)
                             : ::Core::Tr::tr("Pin \"%1\"").arg(quotedDisplayName);
    } else {
        text = ::Core::Tr::tr("Pin Editor");
    }
    addMenuAction(contextMenu, text, entry != nullptr, DocumentModel::model(), [entry] {
        DocumentModelPrivate::setPinned(entry, !entry->pinned);
    });
}

/*!
    Adds the native directory handling and open with menu items for the document
    \a entry to the context menu \a contextMenu.
*/
void EditorManager::addNativeDirAndOpenWithActions(QMenu *contextMenu, DocumentModel::Entry *entry)
{
    EditorManagerPrivate::addNativeDirAndOpenWithActions(contextMenu, filePathFor(entry));
}

void EditorManagerPrivate::addNativeDirAndOpenWithActions(
    QMenu *contextMenu, const FilePath &filePath)
{
    QTC_ASSERT(contextMenu, return);
    bool enabled = !filePath.isEmpty();

    // Open in Finder/Explorer
    addMenuAction(contextMenu, FileUtils::msgGraphicalShellAction(), enabled, d, [filePath] {
        FileUtils::showInGraphicalShell(filePath);
    });

    // Show in File System View
    addMenuAction(contextMenu, FileUtils::msgFileSystemAction(), enabled, d, [filePath] {
        FileUtils::showInFileSystemView(filePath);
    });

    // Open Terminal Here
    addMenuAction(contextMenu, FileUtils::msgTerminalHereAction(), enabled, d, [filePath] {
        FileUtils::openTerminal(filePath.parentDir(), {});
    });

    // Find in This Directory
    addMenuAction(contextMenu, FileUtils::msgFindInDirectory(), enabled, m_instance, [filePath] {
        emit m_instance->findOnFileSystemRequest(filePath);
    });

    // Properties
    addMenuAction(contextMenu, ::Core::Tr::tr("Properties..."), enabled, d, [filePath] {
        DocumentManager::showFilePropertiesDialog(filePath);
    });

    // Open With
    QMenu *openWith = contextMenu->addMenu(::Core::Tr::tr("Open With"));
    openWith->setEnabled(enabled);
    if (enabled)
        populateOpenWithMenu(openWith, filePath);
}

void EditorManager::addContextMenuActions(
    QMenu *contextMenu, DocumentModel::Entry *entry, IEditor *editor)
{
    EditorManagerPrivate::addContextMenuActions(contextMenu, entry, editor);
}

void EditorManager::addContextMenuActions(QMenu *contextMenu, const Utils::FilePath &filePath)
{
    EditorManagerPrivate::addCopyFilePathActions(contextMenu, filePath);
    contextMenu->addSeparator();
    EditorManagerPrivate::addNativeDirAndOpenWithActions(contextMenu, filePath);
}

void EditorManagerPrivate::addContextMenuActions(
    QMenu *contextMenu, DocumentModel::Entry *entry, IEditor *editor, EditorView *view)
{
    EditorManagerPrivate::addSaveAndCloseEditorActions(contextMenu, entry, editor, view);
    contextMenu->addSeparator();
    EditorManager::addPinEditorActions(contextMenu, entry);
    contextMenu->addSeparator();
    EditorManagerPrivate::addNativeDirAndOpenWithActions(contextMenu, filePathFor(entry, editor));
}

/*!
    Populates the \uicontrol {Open With} menu \a menu with editors that are
    suitable for opening the document \a filePath.
*/
void EditorManager::populateOpenWithMenu(QMenu *menu, const FilePath &filePath)
{
    EditorManagerPrivate::populateOpenWithMenu(menu, filePath);
}

void EditorManagerPrivate::populateOpenWithMenu(QMenu *menu, const FilePath &filePath)
{
    menu->clear();

    const EditorFactories factories = IEditorFactory::preferredEditorTypes(filePath);
    const bool anyMatches = !factories.empty();
    if (anyMatches) {
        // Add all suitable editors
        for (IEditorFactory *editorType : factories) {
            const Id editorId = editorType->id();
            // Add action to open with this very editor factory
            QString const actionTitle = editorType->displayName();
            QAction *action = menu->addAction(actionTitle);
            // Below we need QueuedConnection because otherwise, if a qrc file
            // is inside of a qrc file itself, and the qrc editor opens the Open with menu,
            // crashes happen, because the editor instance is deleted by openEditorWith
            // while the menu is still being processed.
            connect(action, &QAction::triggered, d, [filePath, editorId] {
                    IEditorFactory *type = IEditorFactory::editorFactoryForId(editorId);
                if (type && type->isExternalEditor())
                    EditorManager::openExternalEditor(filePath, editorId);
                else
                    EditorManagerPrivate::openEditorWith(filePath, editorId);
            }, Qt::QueuedConnection);
        }
    }
    menu->setEnabled(anyMatches);
}

void EditorManager::runWithTemporaryEditor(const FilePath &filePath,
                                           const std::function<void (IEditor *)> &callback)
{
    const MimeType mt = mimeTypeForFile(filePath, MimeMatchMode::MatchDefaultAndRemote);
    const QList<IEditorFactory *> factories = IEditorFactory::defaultEditorFactories(mt);
    for (IEditorFactory * const factory : factories) {
        QTC_ASSERT(factory, continue);
        if (!factory->isInternalEditor())
            continue;
        std::unique_ptr<IEditor> editor(factory->createEditor());
        if (!editor)
            continue;
        editor->document()->setTemporary(true);
        if (!editor->document()->open(filePath, filePath))
            continue;
        callback(editor.get());
        break;
    }
}

/*!
    Returns reload behavior settings.
*/
IDocument::ReloadSetting EditorManager::reloadSetting()
{
    // FIXME: Used TypedSelectionAspect once we have that.
    return IDocument::ReloadSetting(systemSettings().reloadSetting.value());
}

/*!
    \internal

    Sets editor reaload behavior settings to \a behavior.
*/
void EditorManager::setReloadSetting(IDocument::ReloadSetting behavior)
{
    systemSettings().reloadSetting.setValue(behavior);
}

/*!
    Saves the current document.
*/
void EditorManager::saveDocument()
{
    EditorManagerPrivate::saveDocument(currentDocument());
}

/*!
    Saves the current document under a different file name.
*/
void EditorManager::saveDocumentAs()
{
    EditorManagerPrivate::saveDocumentAs(currentDocument());
}

/*!
    Reverts the current document to its last saved state.
*/
void EditorManager::revertToSaved()
{
    EditorManagerPrivate::revertToSaved(currentDocument());
}

/*!
    Closes the documents specified by \a entries.

    Returns whether all documents were closed.
*/
bool EditorManager::closeDocuments(const QList<DocumentModel::Entry *> &entries)
{
    QList<IDocument *> documentsToClose;
    for (DocumentModel::Entry *entry : entries) {
        if (!entry)
            continue;
        // Pinned files shouldn't be removed from Open Documents, even when pressing the "x" button.
        if (!entry->pinned && entry->isSuspended)
            DocumentModelPrivate::removeEntry(entry);
        else
            documentsToClose << entry->document;
    }
    return closeDocuments(documentsToClose);
}

/*!
    Closes the editors specified by \a editorsToClose. If
    \a askAboutModifiedEditors is \c true, prompts users
    to save their changes before closing the editor.

    Returns whether all editors were closed.

    Usually closeDocuments() is the better alternative.

    \sa closeDocuments()
*/
bool EditorManager::closeEditors(const QList<IEditor*> &editorsToClose, bool askAboutModifiedEditors)
{
    for (IEditor *editor : editorsToClose)
        EditorManagerPrivate::addClosedDocumentToCloseHistory(editor);

    return EditorManagerPrivate::closeEditors(editorsToClose,
                                              askAboutModifiedEditors ? EditorManagerPrivate::CloseFlag::CloseWithAsking
                                                                      : EditorManagerPrivate::CloseFlag::CloseWithoutAsking);
}

/*!
    Activates an editor for the document specified by \a entry in the active
    split using the specified \a flags.
*/
void EditorManager::activateEditorForEntry(DocumentModel::Entry *entry, OpenEditorFlags flags)
{
    QTC_CHECK(!(flags & EditorManager::AllowExternalEditor));

    EditorManagerPrivate::activateEditorForEntry(EditorManagerPrivate::currentEditorView(),
                                                 entry,
                                                 flags);
}

/*!
    Activates the \a editor in the active split using the specified \a flags.

    \sa currentEditor()
*/
void EditorManager::activateEditor(IEditor *editor, OpenEditorFlags flags)
{
    QTC_CHECK(!(flags & EditorManager::AllowExternalEditor));

    QTC_ASSERT(editor, return );
    EditorView *view = EditorManagerPrivate::viewForEditor(editor);
    // an IEditor doesn't have to belong to a view, it might be kept in storage by the editor model
    if (!view)
        view = EditorManagerPrivate::currentEditorView();
    EditorManagerPrivate::activateEditor(view, editor, flags);
}

/*!
    Activates an editor for the \a document in the active split using the
    specified \a flags.
*/
IEditor *EditorManager::activateEditorForDocument(IDocument *document, OpenEditorFlags flags)
{
    QTC_CHECK(!(flags & EditorManager::AllowExternalEditor));

    return EditorManagerPrivate::activateEditorForDocument(EditorManagerPrivate::currentEditorView(),
                                                           document,
                                                           flags);
}

/*!
    Makes an IEditor instance \a editor known to the EditorManager that it did
    not know before, and activates it using \a flags.
*/
void EditorManager::addEditor(IEditor *editor, OpenEditorFlags flags)
{
    QTC_ASSERT(!DocumentModel::editorsForDocument(editor->document()).contains(editor), return);
    d->addEditor(editor);
    activateEditor(editor, flags);
}

/*!
    Opens the document specified by \a filePath using the editor type \a
    editorId and the specified \a flags.

    If \a editorId is \c Id(), the editor type is derived from the file's MIME
    type.

    If \a newEditor is not \c nullptr, and a new editor instance was created,
    it is set to \c true. If an existing editor instance was used, it is set
    to \c false.

    \sa openEditorAt()
    \sa openEditorWithContents()
    \sa openExternalEditor()
*/
IEditor *EditorManager::openEditor(const FilePath &filePath, Id editorId,
                                   OpenEditorFlags flags, bool *newEditor)
{
    checkEditorFlags(flags);
    if (flags & EditorManager::OpenInOtherSplit)
        EditorManager::gotoOtherSplit();

    return EditorManagerPrivate::openEditor(EditorManagerPrivate::currentEditorView(),
                                            filePath, editorId, flags, newEditor);
}

/*!
    Opens the document specified by \a link using the editor type \a
    editorId and the specified \a flags.

    Moves the text cursor to the \e line and \e column specified in \a link.

    If \a editorId is \c Id(), the editor type is derived from the file's MIME
    type.

    If \a newEditor is not \c nullptr, and a new editor instance was created,
    it is set to \c true. If an existing editor instance was used, it is set
    to \c false.

    \sa openEditor()
    \sa openEditorAtSearchResult()
    \sa openEditorWithContents()
    \sa openExternalEditor()
    \sa IEditor::gotoLine()
*/
IEditor *EditorManager::openEditorAt(const Link &link,
                                     Id editorId,
                                     OpenEditorFlags flags,
                                     bool *newEditor)
{
    checkEditorFlags(flags);
    if (flags & EditorManager::OpenInOtherSplit)
        EditorManager::gotoOtherSplit();

    return EditorManagerPrivate::openEditorAt(EditorManagerPrivate::currentEditorView(),
                                              link,
                                              editorId,
                                              flags,
                                              newEditor);
}

IEditor *EditorManager::openEditor(const LocatorFilterEntry &entry)
{
    const OpenEditorFlags defaultFlags = EditorManager::AllowExternalEditor;
    if (entry.linkForEditor)
        return EditorManager::openEditorAt(*entry.linkForEditor, {}, defaultFlags);
    else if (!entry.filePath.isEmpty())
        return EditorManager::openEditor(entry.filePath, {}, defaultFlags);
    return nullptr;
}


/*!
    Opens the document at the position of the search result \a item using the
    editor type \a editorId and the specified \a flags.

    If \a editorId is \c Id(), the editor type is derived from the file's MIME
    type.

    If \a newEditor is not \c nullptr, and a new editor instance was created,
    it is set to \c true. If an existing editor instance was used, it is set to
    \c false.

    \sa openEditorAt()
*/
void EditorManager::openEditorAtSearchResult(const SearchResultItem &item,
                                             Id editorId,
                                             OpenEditorFlags flags,
                                             bool *newEditor)
{
    const QStringList &path = item.path();
    if (path.isEmpty()) {
        openEditor(FilePath::fromUserInput(item.lineText()), editorId, flags, newEditor);
        return;
    }
    const Text::Position position = item.mainRange().begin;
    openEditorAt({FilePath::fromUserInput(path.first()), position.line, position.column},
                 editorId, flags, newEditor);
}

/*!
    Returns whether \a filePath is an auto-save file created by \QC.
*/
bool EditorManager::isAutoSaveFile(const QString &filePath)
{
    return filePath.endsWith(".autosave");
}

bool EditorManager::autoSaveAfterRefactoring()
{
    return systemSettings().autoSaveAfterRefactoring();
}

/*!
    Opens the document specified by \a filePath in the external editor specified
    by \a editorId.

    Returns \c false and displays an error message if \a editorId is not the ID
    of an external editor or the external editor cannot be opened.

    \sa openEditor()
*/
bool EditorManager::openExternalEditor(const FilePath &filePath, Id editorId)
{
    IEditorFactory *ee = Utils::findOrDefault(IEditorFactory::allEditorFactories(),
        [editorId](IEditorFactory *factory) {
            return factory->isExternalEditor() && factory->id() == editorId;
        });

    if (!ee)
        return false;

    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    const Result<> res = ee->startEditor(filePath);
    QApplication::restoreOverrideCursor();
    if (!res)
        QMessageBox::critical(ICore::dialogParent(), ::Core::Tr::tr("Opening File"), res.error());
    return res.has_value();
}

/*!
    Adds \a listener to the hooks that are asked if editors may be closed.

    When an editor requests to close, all listeners are called. If one of the
    calls returns \c false, the process is aborted and the event is ignored. If
    all calls return \c true, editorAboutToClose() is emitted and the event is
    accepted.
*/
void EditorManager::addCloseEditorListener(const std::function<bool (IEditor *)> &listener)
{
    d->m_closeEditorListeners.append(listener);
}

/*!
    Asks the user for a list of files to open and returns the choice.

    The \a options argument holds various options about how to run the dialog.
    See the QFileDialog::Options enum for more information about the flags you
    can pass.

    \sa DocumentManager::getOpenFileNames(), QFileDialog::Options
*/
FilePaths EditorManager::getOpenFilePaths(QFileDialog::Options options)
{
    QString selectedFilter;
    const QString &fileFilters = DocumentManager::fileDialogFilter(&selectedFilter);
    return DocumentManager::getOpenFileNames(fileFilters, {}, &selectedFilter, options);
}

static QString makeTitleUnique(QString *titlePattern)
{
    QString title;
    if (titlePattern) {
        const QChar dollar = QLatin1Char('$');

        QString base = *titlePattern;
        if (base.isEmpty())
            base = "unnamed$";
        if (base.contains(dollar)) {
            int i = 1;
            QSet<QString> docnames;
            const QList<DocumentModel::Entry *> entries = DocumentModel::entries();
            for (const DocumentModel::Entry *entry : entries) {
                QString name = entry->filePath().toUrlishString();
                if (name.isEmpty())
                    name = entry->displayName();
                else
                    name = QFileInfo(name).completeBaseName();
                docnames << name;
            }

            do {
                title = base;
                title.replace(QString(dollar), QString::number(i++));
            } while (docnames.contains(title));
        } else {
            title = *titlePattern;
        }
        *titlePattern = title;
    }
    return title;
}

/*!
    Opens \a contents in an editor of the type \a editorId using the specified
    \a flags.

    The editor is given a display name based on \a titlePattern. If a non-empty
    \a uniqueId is specified and an editor with that unique ID is found, it is
    re-used. Otherwise, a new editor with that unique ID is created.

    Returns the new or re-used editor.

    \sa clearUniqueId()
*/
IEditor *EditorManager::openEditorWithContents(Id editorId,
                                        QString *titlePattern,
                                        const QByteArray &contents,
                                        const QString &uniqueId,
                                        OpenEditorFlags flags)
{
    QTC_CHECK(!(flags & EditorManager::AllowExternalEditor));
    checkEditorFlags(flags);

    if (debugEditorManager)
        qDebug() << Q_FUNC_INFO << editorId.name() << titlePattern << uniqueId << contents;

    if (flags & EditorManager::OpenInOtherSplit)
            EditorManager::gotoOtherSplit();

    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    const QScopeGuard cleanup(&QApplication::restoreOverrideCursor);

    const QString title = makeTitleUnique(titlePattern);

    IEditor *edt = nullptr;
    if (!uniqueId.isEmpty()) {
        const QList<IDocument *> documents = DocumentModel::openedDocuments();
        for (IDocument *document : documents)
            if (document->property(scratchBufferKey).toString() == uniqueId) {
                edt = DocumentModel::editorsForDocument(document).constFirst();

                document->setContents(contents);
                if (!title.isEmpty())
                    edt->document()->setPreferredDisplayName(title);

                activateEditor(edt, flags);
                return edt;
            }
    }

    const FilePath filePath = FilePath::fromString(title);
    EditorFactories factories = EditorManagerPrivate::findFactories(editorId, filePath);
    if (factories.isEmpty())
        return nullptr;

    edt = EditorManagerPrivate::createEditor(factories.first(), filePath);
    if (!edt)
        return nullptr;
    if (!edt->document()->setContents(contents)) {
        delete edt;
        edt = nullptr;
        return nullptr;
    }

    if (!uniqueId.isEmpty())
        edt->document()->setProperty(scratchBufferKey, uniqueId);

    if (!title.isEmpty())
        edt->document()->setPreferredDisplayName(title);

    EditorManagerPrivate::addEditor(edt);
    activateEditor(edt, flags);
    return edt;
}

/*!
    Returns whether the document specified by \a filePath should be opened even
    though it is big. Depending on the settings this might ask the user to
    decide whether the file should be opened.
*/
bool EditorManager::skipOpeningBigTextFile(const FilePath &filePath)
{
    return EditorManagerPrivate::skipOpeningBigTextFile(filePath);
}

/*!
    Clears the unique ID of \a document.

    \sa openEditorWithContents()
*/
void EditorManager::clearUniqueId(IDocument *document)
{
    document->setProperty(scratchBufferKey, QVariant());
}

/*!
    Saves the changes in \a document.

    Returns whether the operation was successful.
*/
bool EditorManager::saveDocument(IDocument *document)
{
    return EditorManagerPrivate::saveDocument(document);
}

/*!
    \internal
*/
bool EditorManager::hasSplitter()
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return false);
    EditorArea *area = EditorManagerPrivate::findEditorArea(view);
    QTC_ASSERT(area, return false);
    return area->hasSplits();
}

/*!
    Returns the list of visible editors.
*/
QList<IEditor*> EditorManager::visibleEditors()
{
    const QList<EditorView *> allViews = EditorManagerPrivate::allEditorViews();
    QList<IEditor *> editors;
    for (const EditorView *view : allViews) {
        if (view->currentEditor())
            editors.append(view->currentEditor());
    }
    return editors;
}

/*!
    Closes \a documents. If \a askAboutModifiedEditors is \c true, prompts
    users to save their changes before closing the documents.

    Returns whether the documents were closed.
*/
bool EditorManager::closeDocuments(const QList<IDocument *> &documents, bool askAboutModifiedEditors)
{
    return m_instance->closeEditors(DocumentModel::editorsForDocuments(documents), askAboutModifiedEditors);
}

/*!
    Adds the current cursor position specified by \a saveState to the
    navigation history. If \a saveState is \l{QByteArray::isNull()}{null} (the
    default), the current state of the active editor is used. Otherwise \a
    saveState must be a valid state of the active editor.

    \sa IEditor::saveState()
*/
void EditorManager::addCurrentPositionToNavigationHistory(const QByteArray &saveState)
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    view->addCurrentPositionToNavigationHistory(saveState);
    EditorManagerPrivate::updateActions();
}

/*!
    Sets the location that was last modified to \a editor.
    Used for \uicontrol{Window} > \uicontrol{Go to Last Edit}.
*/
void EditorManager::setLastEditLocation(const IEditor* editor)
{
    d->m_globalLastEditLocation = EditLocation::forEditor(editor);
}

/*!
    Cuts the forward part of the navigation history, so the user cannot
    \uicontrol{Go Forward} anymore (until the user goes backward again).

    \sa goForwardInNavigationHistory()
    \sa addCurrentPositionToNavigationHistory()
*/
void EditorManager::cutForwardNavigationHistory()
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    view->cutForwardNavigationHistory();
    EditorManagerPrivate::updateActions();
}

/*!
    Goes back in the navigation history.

    \sa goForwardInNavigationHistory()
    \sa addCurrentPositionToNavigationHistory()
*/
void EditorManager::goBackInNavigationHistory()
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    view->goBackInNavigationHistory();
    EditorManagerPrivate::updateActions();
}

/*!
    Goes forward in the navigation history.

    \sa goBackInNavigationHistory()
    \sa addCurrentPositionToNavigationHistory()
*/
void EditorManager::goForwardInNavigationHistory()
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    view->goForwardInNavigationHistory();
    EditorManagerPrivate::updateActions();
}

EditorWindow *windowForEditorArea(EditorArea *area)
{
    return qobject_cast<EditorWindow *>(area->window());
}

QVector<EditorWindow *> editorWindows(const QList<EditorArea *> &areas)
{
    QVector<EditorWindow *> result;
    for (EditorArea *area : areas)
        if (EditorWindow *window = windowForEditorArea(area))
            result.append(window);
    return result;
}

/*!
    \internal

    Returns the serialized state of all non-temporary editors, the split layout
    and external editor windows.

    \sa restoreState()
*/
QByteArray EditorManager::saveState()
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);

    stream << QByteArray("EditorManagerV5");

    // TODO: In case of split views it's not possible to restore these for all correctly with this
    const QList<IDocument *> documents = DocumentModel::openedDocuments();
    for (IDocument *document : documents) {
        if (!document->filePath().isEmpty() && !document->isTemporary()) {
            IEditor *editor = DocumentModel::editorsForDocument(document).constFirst();
            QByteArray state = editor->saveState();
            if (!state.isEmpty())
                d->m_editorStates.insert(document->filePath().toUrlishString(), QVariant(state));
        }
    }

    stream << d->m_editorStates;

    const QList<DocumentModel::Entry *> entries = DocumentModel::entries();
    int entriesCount = 0;
    for (const DocumentModel::Entry *entry : entries) {
        // The editor may be 0 if it was not loaded yet: In that case it is not temporary
        if (!entry->document->isTemporary())
            ++entriesCount;
    }

    stream << entriesCount;

    for (const DocumentModel::Entry *entry : entries) {
        if (!entry->document->isTemporary()) {
            stream << entry->filePath().toUrlishString() << entry->plainDisplayName() << entry->id()
                   << entry->pinned;
        }
    }

    stream << d->m_editorAreas.first()->saveState(); // TODO

    // windows
    const QVector<EditorWindow *> windows = editorWindows(d->m_editorAreas);
    const QVector<QVariantHash> windowStates = Utils::transform(windows, &EditorWindow::saveState);
    stream << windowStates;
    return bytes;
}

class FileStateEntry
{
public:
    QString filePath;
    QString displayName;
    Id id;
    bool pinned = false;
};

/*
    Calls the "handler"s with the extracted data.
    If the fileHandler returns false, the parsing is aborted.
*/
static void restore(
    const QByteArray &state,
    const std::function<void(QMap<QString, QVariant>)> &editorStatesHandler,
    const std::function<bool(FileStateEntry)> &fileHandler,
    const std::function<void(QByteArray)> &splitterStateHandler,
    const std::function<void(QVector<QVariantHash>)> &windowStateHandler)
{
    QDataStream stream(state);
    QByteArray version;
    stream >> version;
    const bool isVersion5 = version == "EditorManagerV5";
    if (version != "EditorManagerV4" && !isVersion5)
        return;

    QMap<QString, QVariant> editorStates;
    stream >> editorStates;
    if (editorStatesHandler)
        editorStatesHandler(editorStates);

    int editorCount = 0;
    stream >> editorCount;
    while (--editorCount >= 0) {
        FileStateEntry file;
        stream >> file.filePath;
        stream >> file.displayName;
        stream >> file.id;
        if (isVersion5)
            stream >> file.pinned;

        if (fileHandler && !fileHandler(file))
            return;
    }

    QByteArray splitterstates;
    stream >> splitterstates;
    if (splitterStateHandler)
        splitterStateHandler(splitterstates);

    if (!stream.atEnd()) { // safety for settings from Qt Creator 4.5 and earlier
        // restore windows
        QVector<QVariantHash> windowStates;
        stream >> windowStates;
        if (windowStateHandler)
            windowStateHandler(windowStates);
    }
}

/*!
    \internal

    Restores the \a state of the split layout, editor windows and editors.

    Returns \c true if the state can be restored.

    \sa saveState()
*/
void EditorManager::restoreState(const QByteArray &state)
{
    closeAllEditors(true);
    // remove extra windows
    for (int i = d->m_editorAreas.count() - 1; i > 0 /* keep first alive */; --i)
        delete d->m_editorAreas.at(i); // automatically removes it from list
    if (d->m_editorAreas.first()->hasSplits())
        EditorManagerPrivate::removeAllSplits();

    QApplication::setOverrideCursor(Qt::WaitCursor);

    const auto setEditorStates = [](const QMap<QString, QVariant> &s) { d->m_editorStates = s; };
    const auto openFile = [](const FileStateEntry &file) {
        if (!file.filePath.isEmpty() && !file.displayName.isEmpty()) {
            const FilePath filePath = FilePath::fromUserInput(file.filePath);
            if (!filePath.exists())
                return true;
            const FilePath rfp = autoSaveName(filePath);
            if (rfp.exists() && filePath.lastModified() < rfp.lastModified()) {
                if (IEditor *editor = openEditor(filePath, file.id, DoNotMakeVisible))
                    DocumentModelPrivate::setPinned(
                        DocumentModel::entryForDocument(editor->document()), file.pinned);
            } else {
                if (DocumentModel::Entry *entry
                    = DocumentModelPrivate::addSuspendedDocument(filePath, file.displayName, file.id))
                    DocumentModelPrivate::setPinned(entry, file.pinned);
            }
        }
        return true;
    };
    const auto restoreSplitterState = [](const QByteArray &state) {
        d->m_editorAreas.first()->restoreState(state);
    };
    const auto restoreWindows = [](const QVector<QVariantHash> &states) {
        for (const QVariantHash &windowState : std::as_const(states)) {
            EditorWindow *window = d->createEditorWindow();
            window->restoreState(windowState);
            window->show();
        }
    };
    restore(state, setEditorStates, openFile, restoreSplitterState, restoreWindows);

    // splitting and stuff results in focus trouble, that's why we set the focus again after restoration
    if (d->m_currentEditor) {
        d->m_currentEditor->widget()->setFocus();
    } else if (Internal::EditorView *view = EditorManagerPrivate::currentEditorView()) {
        if (IEditor *e = view->currentEditor())
            e->widget()->setFocus();
        else
            view->setFocus();
    }

    QApplication::restoreOverrideCursor();
}

FilePaths EditorManagerPrivate::openFilesForState(const QByteArray &state, int max)
{
    FilePaths result;
    restore(
        state,
        {},
        [&result, max](const FileStateEntry &entry) {
            result << FilePath::fromUserInput(entry.filePath);
            return max < 0 || result.size() <= max;
        },
        {},
        {});
    return result;
}

/*!
    \internal
*/
void EditorManager::showEditorStatusBar(const QString &id,
                                        const QString &infoText,
                                        const QString &buttonText,
                                        QObject *object,
                                        const std::function<void()> &function)
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    view->showEditorStatusBar(id, infoText, buttonText, object, function);
}

/*!
    \internal
*/
void EditorManager::hideEditorStatusBar(const QString &id)
{
    // TODO: what if the current editor view betwenn show and hideEditorStatusBar changed?
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    EditorManagerPrivate::currentEditorView()->hideEditorStatusBar(id);
}

/*!
    Returns the default text encoding as the user specified in the settings.
*/
TextEncoding EditorManager::defaultTextEncoding()
{
    QtcSettings *settings = ICore::settings();
    const QByteArray codecName =
            settings->value(Constants::SETTINGS_DEFAULTTEXTENCODING).toByteArray();
    if (!codecName.isEmpty()) {
        const TextEncoding candidate(codecName);
        if (candidate.isValid())
            return candidate;
    }
    // Qt5 doesn't return a valid codec when looking up the "System" codec, but will return
    // such a codec when asking for the codec for locale and no matching codec is available.
    // So check whether such a codec was saved to the settings.
    const TextEncoding localeCodec = TextEncoding::encodingForLocale();
    if (codecName == localeCodec.name())
        return localeCodec;
    return TextEncoding::Utf8;
}

/*!
    Returns the default line ending as the user specified in the settings.
*/
TextFileFormat::LineTerminationMode EditorManager::defaultLineEnding()
{
    QtcSettings *settings = ICore::settings();
    const int defaultLineTerminator = settings->value(Constants::SETTINGS_DEFAULT_LINE_TERMINATOR,
            TextFileFormat::LineTerminationMode::NativeLineTerminator).toInt();

    return static_cast<TextFileFormat::LineTerminationMode>(defaultLineTerminator);
}

/*!
    Splits the editor view horizontally into adjacent views.
*/
void EditorManager::splitSideBySide()
{
    EditorManagerPrivate::split(Qt::Horizontal);
}

/*!
 * Moves focus to another split, creating it if necessary.
 * If there's no split and no other window, a side-by-side split is created.
 * If the current window is split, focus is moved to the next split within this window, cycling.
 * If the current window is not split, focus is moved to the next window.
 */
void EditorManager::gotoOtherSplit()
{
    EditorView *view = EditorManagerPrivate::currentEditorView();
    QTC_ASSERT(view, return);
    EditorView *nextView = view->findNextView();
    if (!nextView) {
        // we are in the "last" view in this editor area
        int index = -1;
        EditorArea *area = EditorManagerPrivate::findEditorArea(view, &index);
        QTC_ASSERT(area, return);
        QTC_ASSERT(index >= 0 && index < d->m_editorAreas.size(), return);
        // stay in same window if it is split
        if (area->hasSplits()) {
            nextView = area->findFirstView();
            QTC_CHECK(nextView != view);
        } else {
            // find next editor area. this might be the same editor area if there's only one.
            int nextIndex = index + 1;
            if (nextIndex >= d->m_editorAreas.size())
                nextIndex = 0;
            nextView = d->m_editorAreas.at(nextIndex)->findFirstView();
            QTC_CHECK(nextView);
            // if we had only one editor area with only one view, we end up at the startpoint
            // in that case we need to split
            if (nextView == view) {
                QTC_CHECK(!area->hasSplits());
                splitSideBySide(); // that deletes 'view'
                view = area->findFirstView();
                nextView = view->findNextView();
                QTC_CHECK(nextView != view);
                QTC_CHECK(nextView);
            }
        }
    }

    if (nextView)
        EditorManagerPrivate::activateView(nextView);
}

/*!
    Returns the maximum file size that should be opened in a text editor.
*/
qint64 EditorManager::maxTextFileSize()
{
    return qint64(3) << 24;
}

/*!
    \internal

    Sets the window title addition handler to \a handler.
*/
void EditorManager::setWindowTitleAdditionHandler(WindowTitleHandler handler)
{
    d->m_titleAdditionHandler = handler;
}

/*!
    \internal

    Sets the session title addition handler to \a handler.
*/
void EditorManager::setSessionTitleHandler(WindowTitleHandler handler)
{
    d->m_sessionTitleHandler = handler;
}

/*!
    \internal
*/
void EditorManager::updateWindowTitles()
{
    for (EditorArea *area : std::as_const(d->m_editorAreas))
        emit area->windowTitleNeedsUpdate();
}

/*!
    \internal
*/
void EditorManager::setWindowTitleVcsTopicHandler(WindowTitleHandler handler)
{
    d->m_titleVcsTopicHandler = handler;
}

} // Core

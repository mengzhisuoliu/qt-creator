// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "runcontrol.h"

#include "appoutputpane.h"
#include "buildconfiguration.h"
#include "buildsystem.h"
#include "customparser.h"
#include "devicesupport/devicekitaspects.h"
#include "devicesupport/devicemanager.h"
#include "devicesupport/idevice.h"
#include "devicesupport/idevicefactory.h"
#include "devicesupport/sshparameters.h"
#include "devicesupport/sshsettings.h"
#include "project.h"
#include "projectexplorer.h"
#include "projectexplorerconstants.h"
#include "projectexplorersettings.h"
#include "projectexplorertr.h"
#include "runconfigurationaspects.h"
#include "target.h"
#include "utils/url.h"
#include "windebuginterface.h"

#include <coreplugin/icore.h>

#include <solutions/tasking/tasktree.h>
#include <solutions/tasking/tasktreerunner.h>

#include <utils/algorithm.h>
#include <utils/checkablemessagebox.h>
#include <utils/fileinprojectfinder.h>
#include <utils/outputformatter.h>
#include <utils/qtcprocess.h>
#include <utils/processinterface.h>
#include <utils/qtcassert.h>
#include <utils/terminalinterface.h>
#include <utils/utilsicons.h>

#include <coreplugin/icontext.h>
#include <coreplugin/icore.h>

#include <QLoggingCategory>
#include <QPushButton>
#include <QTimer>

#if defined (WITH_JOURNALD)
#include "journaldwatcher.h"
#endif

using namespace ProjectExplorer::Internal;
using namespace Tasking;
using namespace Utils;

namespace ProjectExplorer {

static Q_LOGGING_CATEGORY(statesLog, "qtc.projectmanager.states", QtWarningMsg)

static QList<RunWorkerFactory *> g_runWorkerFactories;

static QSet<Id> g_runModes;
static QSet<Id> g_runConfigs;

// RunWorkerFactory

RunWorkerFactory::RunWorkerFactory()
{
    g_runWorkerFactories.append(this);
}

RunWorkerFactory::~RunWorkerFactory()
{
    g_runWorkerFactories.removeOne(this);
}

void RunWorkerFactory::setProducer(const WorkerCreator &producer)
{
    m_producer = producer;
}

void RunWorkerFactory::setRecipeProducer(const RecipeCreator &producer)
{
    setProducer([producer](RunControl *runControl) {
        return new RecipeRunner(runControl, producer(runControl));
    });
}

void RunWorkerFactory::setSupportedRunConfigs(const QList<Id> &runConfigs)
{
    for (Id runConfig : runConfigs)
        g_runConfigs.insert(runConfig); // Debugging only.
    m_supportedRunConfigurations = runConfigs;
}

void RunWorkerFactory::addSupportedRunMode(Id runMode)
{
    g_runModes.insert(runMode); // Debugging only.
    m_supportedRunModes.append(runMode);
}

void RunWorkerFactory::addSupportedRunConfig(Id runConfig)
{
    g_runConfigs.insert(runConfig); // Debugging only.
    m_supportedRunConfigurations.append(runConfig);
}

void RunWorkerFactory::addSupportedDeviceType(Id deviceType)
{
    m_supportedDeviceTypes.append(deviceType);
}

void RunWorkerFactory::addSupportForLocalRunConfigs()
{
    addSupportedRunConfig(ProjectExplorer::Constants::QMAKE_RUNCONFIG_ID);
    addSupportedRunConfig(ProjectExplorer::Constants::QBS_RUNCONFIG_ID);
    addSupportedRunConfig(ProjectExplorer::Constants::CMAKE_RUNCONFIG_ID);
    addSupportedRunConfig(ProjectExplorer::Constants::CUSTOM_EXECUTABLE_RUNCONFIG_ID);
}

void RunWorkerFactory::cloneProduct(Id exitstingStepId, Id overrideId)
{
    for (RunWorkerFactory *factory : std::as_const(g_runWorkerFactories)) {
        if (factory->m_id == exitstingStepId) {
            m_producer = factory->m_producer;
            // Other bits are intentionally not copied as they are unlikely to be
            // useful in the cloner's context. The cloner can/has to finish the
            // setup on its own.
            break;
        }
    }
    // Existence should be guaranteed by plugin dependencies. In case it fails,
    // bark and keep the factory in a state where the invalid m_stepId keeps it
    // inaction.
    QTC_ASSERT(m_producer, return);
    if (overrideId.isValid())
        m_id = overrideId;
}

bool RunWorkerFactory::canCreate(Id runMode, Id deviceType, const QString &runConfigId) const
{
    if (!m_supportedRunModes.contains(runMode))
        return false;

    if (!m_supportedRunConfigurations.isEmpty()) {
        // FIXME: That's to be used after mangled ids are gone.
        //if (!m_supportedRunConfigurations.contains(runConfigId)
        // return false;
        bool ok = false;
        for (const Id &id : m_supportedRunConfigurations) {
            if (runConfigId.startsWith(id.toString())) {
                ok = true;
                break;
            }
        }

        if (!ok)
            return false;
    }

    if (!m_supportedDeviceTypes.isEmpty())
        return m_supportedDeviceTypes.contains(deviceType);

    return true;
}

RunWorker *RunWorkerFactory::create(RunControl *runControl) const
{
    QTC_ASSERT(m_producer, return nullptr);
    return m_producer(runControl);
}

void RunWorkerFactory::dumpAll()
{
    const QList<Id> devices =
            transform(IDeviceFactory::allDeviceFactories(), &IDeviceFactory::deviceType);

    for (Id runMode : std::as_const(g_runModes)) {
        qDebug() << "";
        for (Id device : devices) {
            for (Id runConfig : std::as_const(g_runConfigs)) {
                const auto check = std::bind(&RunWorkerFactory::canCreate,
                                             std::placeholders::_1,
                                             runMode,
                                             device,
                                             runConfig.toString());
                const auto factory = findOrDefault(g_runWorkerFactories, check);
                qDebug() << "MODE:" << runMode << device << runConfig << factory;
            }
        }
    }
}

/*!
    \class ProjectExplorer::RunControl
    \brief The RunControl class instances represent one item that is run.
*/

/*!
    \fn QIcon ProjectExplorer::RunControl::icon() const
    Returns the icon to be shown in the Outputwindow.

    TODO the icon differs currently only per "mode", so this is more flexible
    than it needs to be.
*/


namespace Internal {

enum class RunWorkerState
{
    Initialized, Starting, Running, Stopping, Done
};

static QString stateName(RunWorkerState s)
{
#    define SN(x) case x: return QLatin1String(#x);
    switch (s) {
        SN(RunWorkerState::Initialized)
        SN(RunWorkerState::Starting)
        SN(RunWorkerState::Running)
        SN(RunWorkerState::Stopping)
        SN(RunWorkerState::Done)
    }
    return QString("<unknown: %1>").arg(int(s));
#    undef SN
}

class RunWorkerPrivate : public QObject
{
public:
    RunWorkerPrivate(RunWorker *runWorker, RunControl *runControl);

    bool canStart() const;
    bool canStop() const;

    RunWorker *q;
    RunWorkerState state = RunWorkerState::Initialized;
    const QPointer<RunControl> runControl;
    QList<RunWorker *> startDependencies;
    QList<RunWorker *> stopDependencies;
    QString id;
};

enum class RunControlState
{
    Initialized,      // Default value after creation.
    Starting,         // Actual process/tool starts.
    Running,          // All good and running.
    Stopping,         // initiateStop() was called, stop application/tool
    Stopped           // all good, but stopped. Can possibly be re-started
};

static QString stateName(RunControlState s)
{
#    define SN(x) case x: return QLatin1String(#x);
    switch (s) {
        SN(RunControlState::Initialized)
        SN(RunControlState::Starting)
        SN(RunControlState::Running)
        SN(RunControlState::Stopping)
        SN(RunControlState::Stopped)
    }
    return QString("<unknown: %1>").arg(int(s));
#    undef SN
}

class RunControlPrivateData
{
public:
    QString displayName;
    ProcessRunData runnable;
    QVariantHash extraData;
    IDevice::ConstPtr device;
    Icon icon;
    const MacroExpander *macroExpander = nullptr;
    AspectContainerData aspectData;
    QString buildKey;
    QMap<Id, Store> settingsData;
    Id runConfigId;
    BuildTargetInfo buildTargetInfo;
    FilePath buildDirectory;
    Environment buildEnvironment;
    Kit *kit = nullptr; // Not owned.
    QPointer<BuildConfiguration> buildConfiguration; // Not owned.
    QPointer<Project> project; // Not owned.
    std::function<bool(bool*)> promptToStop;
    std::vector<RunWorkerFactory> m_factories;

    // A handle to the actual application process.
    ProcessHandle applicationProcessHandle;

    QList<QPointer<RunWorker>> m_workers;
    RunControlState state = RunControlState::Initialized;
    bool printEnvironment = false;
    bool m_supportsReRunning = true;
    std::optional<Group> m_runRecipe;

    bool useDebugChannel = false;
    bool useQmlChannel = false;
    bool usePerfChannel = false;
    bool useWorkerChannel = false;
    QUrl debugChannel;
    QUrl qmlChannel;
    QUrl perfChannel;
    QUrl workerChannel;
    Utils::ProcessHandle m_attachPid;
};

class RunControlPrivate : public QObject, public RunControlPrivateData
{
    Q_OBJECT

public:
    RunControlPrivate(RunControl *parent, Id mode)
        : q(parent), runMode(mode)
    {
        icon = Icons::RUN_SMALL_TOOLBAR;
        connect(&m_taskTreeRunner, &TaskTreeRunner::aboutToStart, q, &RunControl::started);
        connect(&m_taskTreeRunner, &TaskTreeRunner::done, this, &RunControlPrivate::emitStopped);
    }

    ~RunControlPrivate() override
    {
        QTC_CHECK(state == RunControlState::Stopped || state == RunControlState::Initialized);
        disconnect();
        q = nullptr;
        qDeleteAll(m_workers);
        m_workers.clear();
    }

    void copyData(RunControlPrivateData *other) { RunControlPrivateData::operator=(*other); }

    Q_ENUM(RunControlState)

    void checkState(RunControlState expectedState);
    void setState(RunControlState state);

    void debugMessage(const QString &msg) const;

    void initiateStart();
    void startPortsGathererIfNeededAndContinueStart();
    void initiateReStart();
    void continueStart();
    void initiateStop();
    void forceStop();
    void continueStopOrFinish();
    void initiateFinish();

    void onWorkerStarted(RunWorker *worker);
    void onWorkerStopped(RunWorker *worker);
    void onWorkerFailed(RunWorker *worker, const QString &msg);

    void showError(const QString &msg);

    static bool isAllowedTransition(RunControlState from, RunControlState to);
    bool isUsingTaskTree() const { return bool(m_runRecipe); }
    void startTaskTree();
    void emitStopped();

    bool isPortsGatherer() const
    { return useDebugChannel || useQmlChannel || usePerfChannel || useWorkerChannel; }
    QUrl getNextChannel(PortList *portList, const QList<Port> &usedPorts);

    RunControl *q;
    Id runMode;
    TaskTreeRunner m_taskTreeRunner;
    TaskTreeRunner m_portsGathererRunner;
};

} // Internal

using namespace Internal;

RunControl::RunControl(Id mode) :
    d(std::make_unique<RunControlPrivate>(this,  mode))
{
}

void RunControl::copyDataFromRunControl(RunControl *runControl)
{
    QTC_ASSERT(runControl, return);
    d->copyData(runControl->d.get());
}

void RunControl::start()
{
    ProjectExplorerPlugin::startRunControl(this);
}

void RunControl::resetDataForAttachToCore()
{
    d->m_workers.clear();
    d->state = RunControlState::Initialized;
}

void RunControl::copyDataFromRunConfiguration(RunConfiguration *runConfig)
{
    QTC_ASSERT(runConfig, return);
    d->runConfigId = runConfig->id();
    d->runnable = runConfig->runnable();
    d->extraData = runConfig->extraData();
    d->displayName = runConfig->expandedDisplayName();
    d->buildKey = runConfig->buildKey();
    d->settingsData = runConfig->settingsData();
    d->aspectData = runConfig->aspectData();
    d->printEnvironment = runConfig->isPrintEnvironmentEnabled();

    setBuildConfiguration(runConfig->buildConfiguration());

    d->macroExpander = runConfig->macroExpander();
}

void RunControl::setBuildConfiguration(BuildConfiguration *bc)
{
    QTC_ASSERT(bc, return);
    QTC_CHECK(!d->buildConfiguration);
    d->buildConfiguration = bc;

    if (!d->buildKey.isEmpty())
        d->buildTargetInfo = bc->buildSystem()->buildTarget(d->buildKey);

    d->buildDirectory = bc->buildDirectory();
    d->buildEnvironment = bc->environment();

    setKit(bc->kit());
    d->macroExpander = bc->macroExpander();
    d->project = bc->project();
}

void RunControl::setKit(Kit *kit)
{
    QTC_ASSERT(kit, return);
    QTC_CHECK(!d->kit);
    d->kit = kit;
    d->macroExpander = kit->macroExpander();

    if (!d->runnable.command.isEmpty()) {
        setDevice(DeviceManager::deviceForPath(d->runnable.command.executable()));
        QTC_ASSERT(device(), setDevice(RunDeviceKitAspect::device(kit)));
    } else {
        setDevice(RunDeviceKitAspect::device(kit));
    }
}

void RunControl::setDevice(const IDevice::ConstPtr &device)
{
    QTC_CHECK(!d->device);
    d->device = device;
#ifdef WITH_JOURNALD
    if (device && device->type() == ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE) {
        JournaldWatcher::instance()->subscribe(this, [this](const JournaldWatcher::LogEntry &entry) {

            if (entry.value("_MACHINE_ID") != JournaldWatcher::instance()->machineId())
                return;

            const QByteArray pid = entry.value("_PID");
            if (pid.isEmpty())
                return;

            const qint64 pidNum = static_cast<qint64>(QString::fromLatin1(pid).toInt());
            if (pidNum != d->applicationProcessHandle.pid())
                return;

            const QString message = QString::fromUtf8(entry.value("MESSAGE")) + "\n";
            appendMessage(message, OutputFormat::LogMessageFormat);
        });
    }
#endif
}

RunControl::~RunControl()
{
#ifdef WITH_JOURNALD
    JournaldWatcher::instance()->unsubscribe(this);
#endif
}

void RunControl::setRunRecipe(const Group &group)
{
    d->m_runRecipe = group;
}

void RunControl::initiateStart()
{
    emit aboutToStart();
    if (d->isUsingTaskTree())
        d->startTaskTree();
    else
        d->initiateStart();
}

void RunControl::initiateReStart()
{
    emit aboutToStart();
    if (d->isUsingTaskTree())
        d->startTaskTree();
    else
        d->initiateReStart();
}

void RunControl::initiateStop()
{
    if (d->isUsingTaskTree()) {
        d->m_taskTreeRunner.reset();
        d->emitStopped();
    } else {
        d->initiateStop();
    }
}

void RunControl::forceStop()
{
    if (d->isUsingTaskTree()) {
        d->m_taskTreeRunner.reset();
        d->emitStopped();
    } else {
        d->forceStop();
    }
}

RunWorker *RunControl::createWorker(Id runMode)
{
    const Id deviceType = RunDeviceTypeKitAspect::deviceTypeId(d->kit);
    for (RunWorkerFactory *factory : std::as_const(g_runWorkerFactories)) {
        if (factory->canCreate(runMode, deviceType, d->runConfigId.toString()))
            return factory->create(this);
    }
    return nullptr;
}

bool RunControl::createMainWorker()
{
    const QList<RunWorkerFactory *> candidates
        = filtered(g_runWorkerFactories, [this](RunWorkerFactory *factory) {
              return factory->canCreate(d->runMode,
                                        RunDeviceTypeKitAspect::deviceTypeId(d->kit),
                                        d->runConfigId.toString());
          });

    // There might be combinations that cannot run. But that should have been checked
    // with canRun below.
    QTC_ASSERT(!candidates.empty(), return false);

    // There should be at most one top-level producer feeling responsible per combination.
    // Breaking a tie should be done by tightening the restrictions on one of them.
    QTC_CHECK(candidates.size() == 1);
    return candidates.front()->create(this) != nullptr;
}

bool RunControl::canRun(Id runMode, Id deviceType, Utils::Id runConfigId)
{
    for (const RunWorkerFactory *factory : std::as_const(g_runWorkerFactories)) {
        if (factory->canCreate(runMode, deviceType, runConfigId.toString()))
            return true;
    }
    return false;
}

void RunControl::postMessage(const QString &msg, Utils::OutputFormat format, bool appendNewLine)
{
    emit appendMessage((appendNewLine && !msg.endsWith('\n')) ? msg + '\n': msg, format);
}

void RunControlPrivate::initiateStart()
{
    checkState(RunControlState::Initialized);
    setState(RunControlState::Starting);
    debugMessage("Queue: Starting");

    startPortsGathererIfNeededAndContinueStart();
}

void RunControlPrivate::initiateReStart()
{
    checkState(RunControlState::Stopped);

    // Re-set worked on re-runs.
    for (RunWorker *worker : std::as_const(m_workers)) {
        if (worker->d->state == RunWorkerState::Done)
            worker->d->state = RunWorkerState::Initialized;
    }

    setState(RunControlState::Starting);
    debugMessage("Queue: ReStarting");

    startPortsGathererIfNeededAndContinueStart();
}

void RunControlPrivate::startPortsGathererIfNeededAndContinueStart()
{
    if (!isPortsGatherer()) {
        continueStart();
        return;
    }

    QTC_ASSERT(device, initiateStop(); return);

    const Storage<PortsOutputData> portsStorage;

    const auto onDone = [this, portsStorage] {
        const auto ports = *portsStorage;
        if (!ports) {
            onWorkerFailed(nullptr, ports.error());
            return;
        }
        PortList portList = device->freePorts();
        const QList<Port> usedPorts = *ports;
        q->appendMessage(Tr::tr("Found %n free ports.", nullptr, portList.count()) + '\n',
                         NormalMessageFormat);
        if (useDebugChannel)
            debugChannel = getNextChannel(&portList, usedPorts);
        if (useQmlChannel)
            qmlChannel = getNextChannel(&portList, usedPorts);
        if (usePerfChannel)
            perfChannel = getNextChannel(&portList, usedPorts);
        if (useWorkerChannel)
            workerChannel = getNextChannel(&portList, usedPorts);

        continueStart();
    };

    const Group recipe {
        portsStorage,
        device->portsGatheringRecipe(portsStorage),
        onGroupDone(onDone)
    };

    q->appendMessage(Tr::tr("Checking available ports...") + '\n', NormalMessageFormat);
    m_portsGathererRunner.start(recipe);
}

QUrl RunControlPrivate::getNextChannel(PortList *portList, const QList<Port> &usedPorts)
{
    QUrl result;
    if (q->device()->extraData(Constants::SSH_FORWARD_DEBUGSERVER_PORT).toBool()) {
        result.setScheme(urlTcpScheme());
        result.setHost("localhost");
    } else {
        result = q->device()->toolControlChannel(IDevice::ControlChannelHint());
    }
    result.setPort(portList->getNextFreePort(usedPorts).number());
    return result;
}

void RunControl::requestDebugChannel()
{
    d->useDebugChannel = true;
}

bool RunControl::usesDebugChannel() const
{
    return d->useDebugChannel;
}

QUrl RunControl::debugChannel() const
{
    return d->debugChannel;
}

void RunControl::requestQmlChannel()
{
    d->useQmlChannel = true;
}

bool RunControl::usesQmlChannel() const
{
    return d->useQmlChannel;
}

QUrl RunControl::qmlChannel() const
{
    return d->qmlChannel;
}

void RunControl::setQmlChannel(const QUrl &channel)
{
    d->qmlChannel = channel;
}

void RunControl::requestPerfChannel()
{
    d->usePerfChannel = true;
}

bool RunControl::usesPerfChannel() const
{
    return d->usePerfChannel;
}

QUrl RunControl::perfChannel() const
{
    return d->perfChannel;
}

void RunControl::requestWorkerChannel()
{
    d->useWorkerChannel = true;
}

QUrl RunControl::workerChannel() const
{
    return d->workerChannel;
}

void RunControl::setAttachPid(ProcessHandle pid)
{
    d->m_attachPid = pid;
}

ProcessHandle RunControl::attachPid() const
{
    return d->m_attachPid;
}

void RunControl::showOutputPane()
{
    appOutputPane().showOutputPaneForRunControl(this);
}

void RunControlPrivate::continueStart()
{
    checkState(RunControlState::Starting);
    bool allDone = true;
    debugMessage("Looking for next worker");
    for (RunWorker *worker : std::as_const(m_workers)) {
        if (worker) {
            const QString &workerId = worker->d->id;
            debugMessage("  Examining worker " + workerId);
            switch (worker->d->state) {
                case RunWorkerState::Initialized:
                    debugMessage("  " + workerId + " is not done yet.");
                    if (worker->d->canStart()) {
                        debugMessage("Starting " + workerId);
                        worker->d->state = RunWorkerState::Starting;
                        QTimer::singleShot(0, worker, &RunWorker::initiateStart);
                        return;
                    }
                    allDone = false;
                    debugMessage("  " + workerId + " cannot start.");
                    break;
                case RunWorkerState::Starting:
                    debugMessage("  " + workerId + " currently starting");
                    allDone = false;
                    break;
                case RunWorkerState::Running:
                    debugMessage("  " + workerId + " currently running");
                    break;
                case RunWorkerState::Stopping:
                    debugMessage("  " + workerId + " currently stopping");
                    continue;
                case RunWorkerState::Done:
                    debugMessage("  " + workerId + " was done before");
                    break;
            }
        } else {
            debugMessage("Found unknown deleted worker while starting");
        }
    }
    if (allDone)
        setState(RunControlState::Running);
}

void RunControlPrivate::initiateStop()
{
    if (state == RunControlState::Initialized)
        qDebug() << "Unexpected initiateStop() in state" << stateName(state);

    setState(RunControlState::Stopping);
    debugMessage("Queue: Stopping for all workers");

    continueStopOrFinish();
}

void RunControlPrivate::continueStopOrFinish()
{
    bool allDone = true;

    auto queueStop = [this](RunWorker *worker, const QString &message) {
        if (worker->d->canStop()) {
            debugMessage(message);
            worker->d->state = RunWorkerState::Stopping;
            QTimer::singleShot(0, worker, &RunWorker::initiateStop);
        } else {
            debugMessage(" " + worker->d->id + " is waiting for dependent workers to stop");
        }
    };

    for (RunWorker *worker : std::as_const(m_workers)) {
        if (worker) {
            const QString &workerId = worker->d->id;
            debugMessage("  Examining worker " + workerId);
            switch (worker->d->state) {
                case RunWorkerState::Initialized:
                    debugMessage("  " + workerId + " was Initialized, setting to Done");
                    worker->d->state = RunWorkerState::Done;
                    break;
                case RunWorkerState::Stopping:
                    debugMessage("  " + workerId + " was already Stopping. Keeping it that way");
                    allDone = false;
                    break;
                case RunWorkerState::Starting:
                    queueStop(worker, "  " + workerId + " was Starting, queuing stop");
                    allDone = false;
                    break;
                case RunWorkerState::Running:
                    queueStop(worker, "  " + workerId + " was Running, queuing stop");
                    allDone = false;
                    break;
                case RunWorkerState::Done:
                    debugMessage("  " + workerId + " was Done. Good.");
                    break;
            }
        } else {
            debugMessage("Found unknown deleted worker");
        }
    }

    if (allDone) {
        debugMessage("All Stopped");
        setState(RunControlState::Stopped);
    } else {
        debugMessage("Not all workers Stopped. Waiting...");
    }
}

void RunControlPrivate::forceStop()
{
    if (state == RunControlState::Stopped) {
        debugMessage("Was finished, too late to force Stop");
        return;
    }
    for (RunWorker *worker : std::as_const(m_workers)) {
        if (worker) {
            const QString &workerId = worker->d->id;
            debugMessage("  Examining worker " + workerId);
            switch (worker->d->state) {
                case RunWorkerState::Initialized:
                    debugMessage("  " + workerId + " was Initialized, setting to Done");
                    break;
                case RunWorkerState::Stopping:
                    debugMessage("  " + workerId + " was already Stopping. Set it forcefully to Done.");
                    break;
                case RunWorkerState::Starting:
                    debugMessage("  " + workerId + " was Starting. Set it forcefully to Done.");
                    break;
                case RunWorkerState::Running:
                    debugMessage("  " + workerId + " was Running. Set it forcefully to Done.");
                    break;
                case RunWorkerState::Done:
                    debugMessage("  " + workerId + " was Done. Good.");
                    break;
            }
            worker->d->state = RunWorkerState::Done;
        } else {
            debugMessage("Found unknown deleted worker");
        }
    }

    setState(RunControlState::Stopped);
    debugMessage("All Stopped");
}

void RunControlPrivate::onWorkerStarted(RunWorker *worker)
{
    worker->d->state = RunWorkerState::Running;

    if (state == RunControlState::Starting) {
        debugMessage(worker->d->id + " start succeeded");
        continueStart();
        return;
    }
    showError(Tr::tr("Unexpected run control state %1 when worker %2 started.")
                  .arg(stateName(state))
                  .arg(worker->d->id));
}

void RunControlPrivate::onWorkerFailed(RunWorker *worker, const QString &msg)
{
    if (worker)
        worker->d->state = RunWorkerState::Done;

    showError(msg);
    switch (state) {
    case RunControlState::Initialized:
        // FIXME 1: We don't have an output pane yet, so use some other mechanism for now.
        // FIXME 2: Translation...
        QMessageBox::critical(Core::ICore::dialogParent(), Tr::tr("Error"),
             QString("Failure during startup. Aborting.") + "<p>" + msg);
        continueStopOrFinish();
        break;
    case RunControlState::Starting:
    case RunControlState::Running:
        initiateStop();
        break;
    case RunControlState::Stopping:
        continueStopOrFinish();
        break;
    case RunControlState::Stopped:
        QTC_CHECK(false); // Should not happen.
        continueStopOrFinish();
        break;
    }
}

void RunControlPrivate::onWorkerStopped(RunWorker *worker)
{
    const QString &workerId = worker->d->id;
    switch (worker->d->state) {
    case RunWorkerState::Running:
        // That was a spontaneous stop.
        worker->d->state = RunWorkerState::Done;
        debugMessage(workerId + " stopped spontaneously.");
        break;
    case RunWorkerState::Stopping:
        worker->d->state = RunWorkerState::Done;
        debugMessage(workerId + " stopped expectedly.");
        break;
    case RunWorkerState::Done:
        worker->d->state = RunWorkerState::Done;
        debugMessage(workerId + " stopped twice. Huh? But harmless.");
        return; // Sic!
    default:
        debugMessage(workerId + " stopped unexpectedly in state"
                     + stateName(worker->d->state));
        worker->d->state = RunWorkerState::Done;
        break;
    }

    if (state == RunControlState::Stopping) {
        continueStopOrFinish();
        return;
    }

    for (RunWorker *dependent : std::as_const(worker->d->stopDependencies)) {
        switch (dependent->d->state) {
        case RunWorkerState::Done:
            break;
        case RunWorkerState::Initialized:
            dependent->d->state = RunWorkerState::Done;
            break;
        default:
            debugMessage("Killing " + dependent->d->id + " as it depends on stopped " + workerId);
            dependent->d->state = RunWorkerState::Stopping;
            QTimer::singleShot(0, dependent, &RunWorker::initiateStop);
            break;
        }
    }

    debugMessage("Checking whether all stopped");
    bool allDone = true;
    for (RunWorker *worker : std::as_const(m_workers)) {
        if (worker) {
            const QString &workerId = worker->d->id;
            debugMessage("  Examining worker " + workerId);
            switch (worker->d->state) {
                case RunWorkerState::Initialized:
                    debugMessage("  " + workerId + " was Initialized.");
                    break;
                case RunWorkerState::Starting:
                    debugMessage("  " + workerId + " was Starting, waiting for its response");
                    allDone = false;
                    break;
                case RunWorkerState::Running:
                    debugMessage("  " + workerId + " was Running, waiting for its response");
                    allDone = false;
                    break;
                case RunWorkerState::Stopping:
                    debugMessage("  " + workerId + " was already Stopping. Keeping it that way");
                    allDone = false;
                    break;
                case RunWorkerState::Done:
                    debugMessage("  " + workerId + " was Done. Good.");
                    break;
            }
        } else {
            debugMessage("Found unknown deleted worker");
        }
    }

    if (allDone) {
        if (state == RunControlState::Stopped) {
            debugMessage("All workers stopped, but runControl was already stopped.");
        } else {
            debugMessage("All workers stopped. Set runControl to Stopped");
            setState(RunControlState::Stopped);
        }
    } else {
        debugMessage("Not all workers stopped. Waiting...");
    }
}

void RunControlPrivate::showError(const QString &msg)
{
    if (q && !msg.isEmpty())
        q->postMessage(msg + '\n', ErrorMessageFormat);
}

void RunControl::setupFormatter(OutputFormatter *formatter) const
{
    QList<Utils::OutputLineParser *> parsers = createOutputParsers(target());
    if (const auto customParsersAspect = aspectData<CustomParsersAspect>()) {
        for (const Id id : std::as_const(customParsersAspect->parsers)) {
            if (auto parser = createCustomParserFromId(id))
                parsers << parser;
        }
    }
    formatter->setLineParsers(parsers);
    if (project()) {
        Utils::FileInProjectFinder fileFinder;
        fileFinder.setProjectDirectory(project()->projectDirectory());
        fileFinder.setProjectFiles(project()->files(Project::AllFiles));
        formatter->setFileFinder(fileFinder);
    }
}

Utils::Id RunControl::runMode() const
{
    return d->runMode;
}

bool RunControl::isPrintEnvironmentEnabled() const
{
    return d->printEnvironment;
}

const ProcessRunData &RunControl::runnable() const
{
    return d->runnable;
}

const CommandLine &RunControl::commandLine() const
{
    return d->runnable.command;
}

void RunControl::setCommandLine(const CommandLine &command)
{
    d->runnable.command = command;
}

const FilePath &RunControl::workingDirectory() const
{
    return d->runnable.workingDirectory;
}

void RunControl::setWorkingDirectory(const FilePath &workingDirectory)
{
    d->runnable.workingDirectory = workingDirectory;
}

const Environment &RunControl::environment() const
{
    return d->runnable.environment;
}

void RunControl::setEnvironment(const Environment &environment)
{
    d->runnable.environment = environment;
}

const QVariantHash &RunControl::extraData() const
{
    return d->extraData;
}

void RunControl::setExtraData(const QVariantHash &extraData)
{
    d->extraData = extraData;
}

QString RunControl::displayName() const
{
    if (d->displayName.isEmpty())
        return d->runnable.command.executable().toUserOutput();
    return d->displayName;
}

void RunControl::setDisplayName(const QString &displayName)
{
    d->displayName = displayName;
}

void RunControl::setIcon(const Utils::Icon &icon)
{
    d->icon = icon;
}

Utils::Icon RunControl::icon() const
{
    return d->icon;
}

IDevice::ConstPtr RunControl::device() const
{
   return d->device;
}

BuildConfiguration *RunControl::buildConfiguration() const
{
    return d->buildConfiguration;
}

Target *RunControl::target() const
{
    return buildConfiguration() ? buildConfiguration()->target() : nullptr;
}

Project *RunControl::project() const
{
    return d->project;
}

Kit *RunControl::kit() const
{
    return d->kit;
}

const MacroExpander *RunControl::macroExpander() const
{
    return d->macroExpander;
}

const BaseAspect::Data *RunControl::aspectData(Id instanceId) const
{
    return d->aspectData.aspect(instanceId);
}

const BaseAspect::Data *RunControl::aspectData(BaseAspect::Data::ClassId classId) const
{
    return d->aspectData.aspect(classId);
}

Store RunControl::settingsData(Id id) const
{
    return d->settingsData.value(id);
}

QString RunControl::buildKey() const
{
    return d->buildKey;
}

FilePath RunControl::buildDirectory() const
{
    return d->buildDirectory;
}

Environment RunControl::buildEnvironment() const
{
    return d->buildEnvironment;
}

FilePath RunControl::targetFilePath() const
{
    return d->buildTargetInfo.targetFilePath;
}

FilePath RunControl::projectFilePath() const
{
    return d->buildTargetInfo.projectFilePath;
}

/*!
    A handle to the application process.

    This is typically a process id, but should be treated as
    opaque handle to the process controled by this \c RunControl.
*/

ProcessHandle RunControl::applicationProcessHandle() const
{
    return d->applicationProcessHandle;
}

void RunControl::setApplicationProcessHandle(const ProcessHandle &handle)
{
    if (d->applicationProcessHandle != handle) {
        d->applicationProcessHandle = handle;
        emit applicationProcessHandleChanged(QPrivateSignal());
    }
}

/*!
    Prompts to stop. If \a optionalPrompt is passed, a \gui {Do not ask again}
    checkbox is displayed and the result is returned in \a *optionalPrompt.
*/

bool RunControl::promptToStop(bool *optionalPrompt) const
{
    QTC_ASSERT(isRunning(), return true);
    if (optionalPrompt && !*optionalPrompt)
        return true;

    // Overridden.
    if (d->promptToStop)
        return d->promptToStop(optionalPrompt);

    const QString msg = Tr::tr("<html><head/><body><center><i>%1</i> is still running.<center/>"
                           "<center>Force it to quit?</center></body></html>").arg(displayName());
    return showPromptToStopDialog(Tr::tr("Application Still Running"), msg,
                                  Tr::tr("Force &Quit"), Tr::tr("&Keep Running"),
                                  optionalPrompt);
}

void RunControl::setPromptToStop(const std::function<bool (bool *)> &promptToStop)
{
    d->promptToStop = promptToStop;
}

void RunControl::setSupportsReRunning(bool reRunningSupported)
{
    d->m_supportsReRunning = reRunningSupported;
}

bool RunControl::supportsReRunning() const
{
    return d->m_supportsReRunning;
}

void RunControlPrivate::startTaskTree()
{
    m_taskTreeRunner.start(*m_runRecipe);
}

void RunControlPrivate::emitStopped()
{
    if (!q)
        return;

    q->setApplicationProcessHandle(Utils::ProcessHandle());
    emit q->stopped();
}

bool RunControl::isRunning() const
{
    if (d->isUsingTaskTree())
        return d->m_taskTreeRunner.isRunning();
    return d->state == RunControlState::Running;
}

bool RunControl::isStarting() const
{
    if (d->isUsingTaskTree())
        return false;
    return d->state == RunControlState::Starting;
}

bool RunControl::isStopped() const
{
    if (d->isUsingTaskTree())
        return !d->m_taskTreeRunner.isRunning();
    return d->state == RunControlState::Stopped;
}

/*!
    Prompts to terminate the application with the \gui {Do not ask again}
    checkbox.
*/

bool RunControl::showPromptToStopDialog(const QString &title,
                                        const QString &text,
                                        const QString &stopButtonText,
                                        const QString &cancelButtonText,
                                        bool *prompt)
{
    // Show a question message box where user can uncheck this
    // question for this class.
    QMap<QMessageBox::StandardButton, QString> buttonTexts;
    if (!stopButtonText.isEmpty())
        buttonTexts[QMessageBox::Yes] = stopButtonText;
    if (!cancelButtonText.isEmpty())
        buttonTexts[QMessageBox::Cancel] = cancelButtonText;

    CheckableDecider decider;
    if (prompt)
        decider = CheckableDecider(prompt);

    auto selected = CheckableMessageBox::question(title,
                                                  text,
                                                  decider,
                                                  QMessageBox::Yes | QMessageBox::Cancel,
                                                  QMessageBox::Yes,
                                                  QMessageBox::Yes,
                                                  buttonTexts);

    return selected == QMessageBox::Yes;
}

void RunControl::provideAskPassEntry(Environment &env)
{
    const FilePath askpass = SshSettings::askpassFilePath();
    if (askpass.exists())
        env.setFallback("SUDO_ASKPASS", askpass.toUserOutput());
}

bool RunControlPrivate::isAllowedTransition(RunControlState from, RunControlState to)
{
    switch (from) {
    case RunControlState::Initialized:
        return to == RunControlState::Starting;
    case RunControlState::Starting:
        return to == RunControlState::Running || to == RunControlState::Stopping;
    case RunControlState::Running:
        return to == RunControlState::Stopping || to == RunControlState::Stopped;
    case RunControlState::Stopping:
        return to == RunControlState::Stopped;
    case RunControlState::Stopped:
        return to != RunControlState::Initialized;
    }
    return false;
}

void RunControlPrivate::checkState(RunControlState expectedState)
{
    if (state != expectedState)
        qDebug() << "Unexpected run control state " << stateName(expectedState)
                 << " have: " << stateName(state);
}

void RunControlPrivate::setState(RunControlState newState)
{
    if (!isAllowedTransition(state, newState))
        qDebug() << "Invalid run control state transition from" << stateName(state)
                 << "to" << stateName(newState);

    state = newState;

    debugMessage("Entering state " + stateName(newState));

    // Extra reporting.
    switch (state) {
    case RunControlState::Running:
        if (q)
            emit q->started();
        break;
    case RunControlState::Stopped:
        emitStopped();
        break;
    default:
        break;
    }
}

void RunControlPrivate::debugMessage(const QString &msg) const
{
    qCDebug(statesLog()) << msg;
}


// ProcessRunnerPrivate

namespace Internal {

class ProcessRunnerPrivate : public QObject
{
public:
    explicit ProcessRunnerPrivate(ProcessRunner *parent);
    ~ProcessRunnerPrivate() override;

    void start();
    void stop();

    void postMessage(const QString &msg, OutputFormat format, bool appendNewLine = true)
    {
        q->runControl()->postMessage(msg, format, appendNewLine);
    }
    Utils::ProcessHandle applicationPID() const;

    void handleStandardOutput();
    void handleStandardError();

    // Local
    qint64 privateApplicationPID() const;
    bool isRunning() const;

    ProcessRunner *q = nullptr;

    Process m_process;
    QTimer m_waitForDoneTimer;

    ProcessResultData m_resultData;

    std::function<void(Process &)> m_startModifier;

    bool m_stopReported = false;
    bool m_stopForced = false;
    bool m_suppressDefaultStdOutHandling = false;

    void forwardStarted();
    void forwardDone();
};

} // Internal

static QProcess::ProcessChannelMode defaultProcessChannelMode()
{
    return appOutputPane().settings().mergeChannels
            ? QProcess::MergedChannels : QProcess::SeparateChannels;
}

ProcessRunnerPrivate::ProcessRunnerPrivate(ProcessRunner *parent)
    : q(parent)
{
    m_process.setProcessChannelMode(defaultProcessChannelMode());
    connect(&m_process, &Process::started, this, &ProcessRunnerPrivate::forwardStarted);
    connect(&m_process, &Process::done, this, [this] {
        m_resultData = m_process.resultData();
        forwardDone();
    });
    connect(&m_process, &Process::readyReadStandardError,
                this, &ProcessRunnerPrivate::handleStandardError);
    connect(&m_process, &Process::readyReadStandardOutput,
                this, &ProcessRunnerPrivate::handleStandardOutput);
    connect(&m_process, &Process::requestingStop, this, [this] {
        postMessage(Tr::tr("Requesting process to stop ...."), NormalMessageFormat);
    });
    connect(&m_process, &Process::stoppingForcefully, this, [this] {
        postMessage(Tr::tr("Stopping process forcefully ...."), NormalMessageFormat);
    });

    m_waitForDoneTimer.setSingleShot(true);
    connect(&m_waitForDoneTimer, &QTimer::timeout, this, [this] {
        postMessage(Tr::tr("Process unexpectedly did not finish."), ErrorMessageFormat);
        if (!m_process.commandLine().executable().isLocal())
            postMessage(Tr::tr("Connectivity lost?"), ErrorMessageFormat);
        m_process.close();
        forwardDone();
    });

    if (WinDebugInterface::instance()) {
        connect(WinDebugInterface::instance(), &WinDebugInterface::cannotRetrieveDebugOutput,
            this, [this] {
                disconnect(WinDebugInterface::instance(), nullptr, this, nullptr);
                postMessage(Tr::tr("Cannot retrieve debugging output.")
                          + QLatin1Char('\n'), ErrorMessageFormat);
        });

        connect(WinDebugInterface::instance(), &WinDebugInterface::debugOutput,
                this, [this](qint64 pid, const QList<QString> &messages) {
            if (privateApplicationPID() != pid)
                return;
            for (const QString &message : messages)
                postMessage(message, DebugFormat);
        });
    }
}

ProcessRunnerPrivate::~ProcessRunnerPrivate()
{
    if (m_process.state() != QProcess::NotRunning)
        forwardDone();
}

void ProcessRunnerPrivate::stop()
{
    if (m_stopForced || m_process.state() == QProcess::NotRunning)
        return;

    m_stopForced = true;
    m_resultData.m_exitStatus = QProcess::CrashExit;
    m_waitForDoneTimer.setInterval(2 * m_process.reaperTimeout());
    m_waitForDoneTimer.start();
    m_process.stop();
}

bool ProcessRunnerPrivate::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

qint64 ProcessRunnerPrivate::privateApplicationPID() const
{
    if (!isRunning())
        return 0;

    return m_process.processId();
}

void ProcessRunnerPrivate::handleStandardOutput()
{
    if (m_suppressDefaultStdOutHandling)
        emit q->runControl()->stdOutData(m_process.readAllRawStandardOutput());
    else
        postMessage(m_process.readAllStandardOutput(), StdOutFormat, false);
}

void ProcessRunnerPrivate::handleStandardError()
{
    const QString msg = m_process.readAllStandardError();
    postMessage(msg, StdErrFormat, false);
}

void ProcessRunnerPrivate::start()
{
    CommandLine cmdLine = m_process.commandLine();
    Environment env = m_process.environment();

    m_resultData = {};
    QTC_ASSERT(m_process.state() == QProcess::NotRunning, return);

    if (cmdLine.executable().isLocal()) {
        // Running locally.
        bool runAsRoot = false;
        if (auto runAsRootAspect = q->runControl()->aspectData<RunAsRootAspect>())
            runAsRoot = runAsRootAspect->value;

        if (runAsRoot)
            RunControl::provideAskPassEntry(env);

        WinDebugInterface::startIfNeeded();

        if (HostOsInfo::isMacHost()) {
            CommandLine disclaim(Core::ICore::libexecPath("disclaim"));
            disclaim.addCommandLineAsArgs(cmdLine);
            cmdLine = disclaim;
        }

        m_process.setRunAsRoot(runAsRoot);
    }

    const IDevice::ConstPtr device = DeviceManager::deviceForPath(cmdLine.executable());
    if (device && !device->allowEmptyCommand() && cmdLine.isEmpty()) {
        m_resultData.m_errorString = Tr::tr("Cannot run: No command given.");
        m_resultData.m_error = QProcess::FailedToStart;
        m_resultData.m_exitStatus = QProcess::CrashExit;
        forwardDone();
        return;
    }

    QVariantHash extraData = q->runControl()->extraData();
    if (const auto rc = q->runControl()) {
        QString shellName = rc->displayName();

        if (rc->buildConfiguration()) {
            if (BuildConfiguration *buildConfig = rc->buildConfiguration())
                shellName += " - " + buildConfig->displayName();
        }

        extraData[TERMINAL_SHELL_NAME] = shellName;
    } else {
        extraData[TERMINAL_SHELL_NAME] = cmdLine.executable().fileName();
    }

    m_process.setCommand(cmdLine);
    m_process.setEnvironment(env);
    m_process.setExtraData(extraData);
    m_process.setForceDefaultErrorModeOnWindows(true);
    m_process.start();
}

/*!
    \class ProjectExplorer::ProcessRunner

    \brief The ProcessRunner class is the application launcher of the
    ProjectExplorer plugin.

    Encapsulates processes running in a console or as GUI processes,
    captures debug output of GUI processes on Windows (outputDebugString()).

    \sa Utils::Process
*/
ProcessRunner::ProcessRunner(RunControl *runControl)
    : RunWorker(runControl), d(new Internal::ProcessRunnerPrivate(this))
{
    setId("ProcessRunner");
}

ProcessRunner::~ProcessRunner() = default;

void ProcessRunnerPrivate::forwardDone()
{
    if (m_stopReported)
        return;
    m_waitForDoneTimer.stop();
    const CommandLine command = m_process.commandLine();
    const QString executable = command.executable().displayName();
    QString msg = Tr::tr("%1 exited with code %2").arg(executable).arg(m_resultData.m_exitCode);
    if (m_resultData.m_exitStatus == QProcess::CrashExit) {
        if (m_stopForced)
            msg = Tr::tr("The process was ended forcefully.");
        else
            msg = Tr::tr("The process crashed.");
    } else if (m_resultData.m_error != QProcess::UnknownError) {
        msg = RunWorker::userMessageForProcessError(m_resultData.m_error, command.executable());
    }
    postMessage(msg, NormalMessageFormat);
    m_stopReported = true;
    q->reportStopped();
}

void ProcessRunnerPrivate::forwardStarted()
{
    const bool isDesktop = m_process.commandLine().executable().isLocal();
    if (isDesktop) {
        // Console processes only know their pid after being started
        ProcessHandle pid{privateApplicationPID()};
        q->runControl()->setApplicationProcessHandle(pid);
        pid.activate();
    }

    q->reportStarted();
}

void ProcessRunner::start()
{
    d->m_process.setCommand(runControl()->commandLine());
    d->m_process.setWorkingDirectory(runControl()->workingDirectory());
    d->m_process.setEnvironment(runControl()->environment());

    if (d->m_startModifier)
        d->m_startModifier(d->m_process);

    bool useTerminal = false;
    if (auto terminalAspect = runControl()->aspectData<TerminalAspect>())
        useTerminal = terminalAspect->useTerminal;

    const CommandLine command = d->m_process.commandLine();
    const Environment environment = d->m_process.environment();
    d->m_stopForced = false;
    d->m_stopReported = false;
    d->disconnect(this);
    d->m_process.setTerminalMode(useTerminal ? Utils::TerminalMode::Run : Utils::TerminalMode::Off);
    d->m_process.setReaperTimeout(
        std::chrono::seconds(projectExplorerSettings().reaperTimeoutInSeconds));

    const QString msg = Tr::tr("Starting %1...").arg(command.displayName());
    d->postMessage(msg, NormalMessageFormat);
    if (runControl()->isPrintEnvironmentEnabled()) {
        d->postMessage(Tr::tr("Environment:"), NormalMessageFormat);
        environment.forEachEntry([this](const QString &key, const QString &value, bool enabled) {
            if (enabled)
                d->postMessage(key + '=' + value, StdOutFormat);
        });
        d->postMessage({}, StdOutFormat);
    }

    const bool isDesktop = command.executable().isLocal();
    if (isDesktop && command.isEmpty()) {
        reportFailure(Tr::tr("No executable specified."));
        return;
    }
    d->start();
}

void ProcessRunner::stop()
{
    d->stop();
}

void ProcessRunner::setStartModifier(const std::function<void(Process &)> &startModifier)
{
    d->m_startModifier = startModifier;
}

void ProcessRunner::suppressDefaultStdOutHandling()
{
    d->m_suppressDefaultStdOutHandling = true;
}

// RunWorkerPrivate

RunWorkerPrivate::RunWorkerPrivate(RunWorker *runWorker, RunControl *runControl)
    : q(runWorker), runControl(runControl)
{
    runControl->d->m_workers.append(runWorker);
}

bool RunWorkerPrivate::canStart() const
{
    if (state != RunWorkerState::Initialized)
        return false;
    for (RunWorker *worker : startDependencies) {
        QTC_ASSERT(worker, continue);
        if (worker->d->state != RunWorkerState::Done
                && worker->d->state != RunWorkerState::Running)
            return false;
    }
    return true;
}

bool RunWorkerPrivate::canStop() const
{
    if (state != RunWorkerState::Starting && state != RunWorkerState::Running)
        return false;
    for (RunWorker *worker : stopDependencies) {
        QTC_ASSERT(worker, continue);
        if (worker->d->state != RunWorkerState::Done)
            return false;
    }
    return true;
}

/*!
    \class ProjectExplorer::RunWorker

    \brief The RunWorker class encapsulates a task that forms part, or
    the whole of the operation of a tool for a certain \c RunConfiguration
    according to some \c RunMode.

    A typical example for a \c RunWorker is a process, either the
    application process itself, or a helper process, such as a watchdog
    or a log parser.

    A \c RunWorker has a simple state model covering the \c Initialized,
    \c Starting, \c Running, \c Stopping, and \c Done states.

    In the course of the operation of tools several \c RunWorkers
    may co-operate and form a combined state that is presented
    to the user as \c RunControl, with direct interaction made
    possible through the buttons in the \uicontrol{Application Output}
    pane.

    RunWorkers are typically created together with their RunControl.
    The startup order of RunWorkers under a RunControl can be
    specified by making a RunWorker dependent on others.

    When a RunControl starts, it calls \c initiateStart() on RunWorkers
    with fulfilled dependencies until all workers are \c Running, or in case
    of short-lived helper tasks, \c Done.

    A RunWorker can stop spontaneously, for example when the main application
    process ends. In this case, it typically calls \c initiateStop()
    on its RunControl, which in turn passes this to all sibling
    RunWorkers.

    Pressing the stop button in the \uicontrol{Application Output} pane
    also calls \c initiateStop on the RunControl.
*/

RunWorker::RunWorker(RunControl *runControl)
    : d(std::make_unique<RunWorkerPrivate>(this, runControl))
{ }

RunWorker::~RunWorker() = default;

/*!
 * This function is called by the RunControl once all dependencies
 * are fulfilled.
 */
void RunWorker::initiateStart()
{
    d->runControl->d->debugMessage("Initiate start for " + d->id);
    start();
}

/*!
 * This function has to be called by a RunWorker implementation
 * to notify its RunControl about the successful start of this RunWorker.
 *
 * The RunControl may start other RunWorkers in response.
 */
void RunWorker::reportStarted()
{
    d->runControl->d->onWorkerStarted(this);
    emit started();
}

/*!
 * This function is called by the RunControl in its own \c initiateStop
 * implementation, which is triggered in response to pressing the
 * stop button in the \uicontrol{Application Output} pane or on direct
 * request of one of the sibling RunWorkers.
 */
void RunWorker::initiateStop()
{
    d->runControl->d->debugMessage("Initiate stop for " + d->id);
    emit stopping();
    stop();
}

/*!
 * This function has to be called by a RunWorker implementation
 * to notify its RunControl about this RunWorker having stopped.
 *
 * The stop can be spontaneous, or in response to an initiateStop()
 * or an initiateFinish() call.
 *
 * The RunControl will adjust its global state in response.
 */
void RunWorker::reportStopped()
{
    QTC_ASSERT(d && d->runControl && d->runControl->d, return);
    d->runControl->d->onWorkerStopped(this);
    emit stopped();
}

/*!
 * This function can be called by a RunWorker implementation for short-lived
 * tasks to notify its RunControl about this task being successful finished.
 * Dependent startup tasks can proceed, in cases of spontaneous or scheduled
 * stops, the effect is the same as \c reportStopped().
 *
 */
void RunWorker::reportDone()
{
    switch (d->state) {
        case RunWorkerState::Initialized:
            QTC_CHECK(false);
            d->state = RunWorkerState::Done;
            break;
        case RunWorkerState::Starting:
            reportStarted();
            reportStopped();
            break;
        case RunWorkerState::Running:
        case RunWorkerState::Stopping:
            reportStopped();
            break;
        case RunWorkerState::Done:
            break;
    }
}

/*!
 * This function can be called by a RunWorker implementation to
 * signal a problem in the operation in this worker. The
 * RunControl will start to ramp down through initiateStop().
 */
void RunWorker::reportFailure(const QString &msg)
{
    d->runControl->d->onWorkerFailed(this, msg);
}

/*!
 * Appends a message in the specified \a format to
 * the owning RunControl's \uicontrol{Application Output} pane.
 */
void RunWorker::appendMessage(const QString &msg, OutputFormat format, bool appendNewLine)
{
    d->runControl->postMessage(msg, format, appendNewLine);
}

void RunWorker::addStartDependency(RunWorker *dependency)
{
    d->startDependencies.append(dependency);
}

void RunWorker::addStopDependency(RunWorker *dependency)
{
    d->stopDependencies.append(dependency);
}

RunControl *RunWorker::runControl() const
{
    return d->runControl;
}

void RunWorker::setId(const QString &id)
{
    d->id = id;
}

QString RunWorker::userMessageForProcessError(QProcess::ProcessError error, const FilePath &program)
{
    QString failedToStart = Tr::tr("The process failed to start.");
    QString msg = Tr::tr("An unknown error in the process occurred.");
    switch (error) {
        case QProcess::FailedToStart:
            msg = failedToStart + ' ' + Tr::tr("Either the "
                "invoked program \"%1\" is missing, or you may have insufficient "
                "permissions to invoke the program.").arg(program.toUserOutput());
            break;
        case QProcess::Crashed:
            msg = Tr::tr("The process crashed.");
            break;
        case QProcess::Timedout:
            // "The last waitFor...() function timed out. "
            //   "The state of QProcess is unchanged, and you can try calling "
            // "waitFor...() again."
            return {}; // sic!
        case QProcess::WriteError:
            msg = Tr::tr("An error occurred when attempting to write "
                "to the process. For example, the process may not be running, "
                "or it may have closed its input channel.");
            break;
        case QProcess::ReadError:
            msg = Tr::tr("An error occurred when attempting to read from "
                "the process. For example, the process may not be running.");
            break;
        case QProcess::UnknownError:
            break;
    }
    return msg;
}

void RunWorker::start()
{
    reportStarted();
}

void RunWorker::stop()
{
    reportStopped();
}

// Output parser factories

static QList<std::function<OutputLineParser *(Target *)>> g_outputParserFactories;

QList<OutputLineParser *> createOutputParsers(Target *target)
{
    QList<OutputLineParser *> formatters;
    for (auto factory : std::as_const(g_outputParserFactories)) {
        if (OutputLineParser *parser = factory(target))
            formatters << parser;
    }
    return formatters;
}

void addOutputParserFactory(const std::function<Utils::OutputLineParser *(Target *)> &factory)
{
    g_outputParserFactories.append(factory);
}

// ProcessRunnerFactory

ProcessRunnerFactory::ProcessRunnerFactory(const QList<Id> &runConfigs)
{
    setProduct<ProcessRunner>();
    addSupportedRunMode(ProjectExplorer::Constants::NORMAL_RUN_MODE);
    setSupportedRunConfigs(runConfigs);
}

Storage<RunInterface> runStorage()
{
    static Storage<RunInterface> theRunStorage;
    return theRunStorage;
}

Canceler canceler()
{
    return [] { return std::make_pair(runStorage().activeStorage(), &RunInterface::canceled); };
}

void RecipeRunner::start()
{
    QTC_CHECK(!m_taskTreeRunner.isRunning());

    const auto onSetup = [this] {
        connect(this, &RecipeRunner::canceled,
                runStorage().activeStorage(), &RunInterface::canceled);
        connect(runStorage().activeStorage(), &RunInterface::started,
                this, &RecipeRunner::reportStarted);
    };

    const Group recipe {
        runStorage(),
        onGroupSetup(onSetup),
        m_recipe
    };

    m_taskTreeRunner.start(recipe, {}, [this](DoneWith result) {
        if (result == DoneWith::Success)
            reportStopped();
        else
            reportFailure();
    });
}

void RecipeRunner::stop()
{
    emit canceled();
}

} // namespace ProjectExplorer

#include "runcontrol.moc"

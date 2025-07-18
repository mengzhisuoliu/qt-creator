// Copyright (C) 2016 Denis Shienkov <denis.shienkov@gmail.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <baremetal/idebugserverprovider.h>

#include <utils/commandline.h>

QT_BEGIN_NAMESPACE
class QComboBox;
QT_END_NAMESPACE

namespace Utils { class PathChooser; }

namespace BareMetal::Internal {

// GdbServerProvider

class GdbServerProvider : public IDebugServerProvider
{
public:
    enum StartupMode {
        StartupOnNetwork,
        StartupOnPipe
    };

    StartupMode startupMode() const;
    Utils::FilePath peripheralDescriptionFile() const;
    QString initCommands() const;
    QString resetCommands() const;
    bool useExtendedRemote() const;

    bool operator==(const IDebugServerProvider &other) const override;

    void toMap(Utils::Store &data) const override;

    virtual Utils::CommandLine command() const;

    Utils::Result<> setupDebuggerRunParameters(Debugger::DebuggerRunParameters &rp,
        ProjectExplorer::RunControl *runControl) const final;
    std::optional<Utils::ProcessTask> targetProcess(
        ProjectExplorer::RunControl *runControl) const final;

    bool isValid() const override;
    virtual QSet<StartupMode> supportedStartupModes() const = 0;

protected:
    explicit GdbServerProvider(const QString &id);

    void setStartupMode(StartupMode);
    void setPeripheralDescriptionFile(const Utils::FilePath &file);
    void setInitCommands(const QString &);
    void setResetCommands(const QString &);
    void setUseExtendedRemote(bool);

    void fromMap(const Utils::Store &data) override;

    StartupMode m_startupMode = StartupOnNetwork;
    Utils::FilePath m_peripheralDescriptionFile;
    QString m_initCommands;
    QString m_resetCommands;
    bool m_useExtendedRemote = false;
    Utils::FilePath m_executableFile;
    QString m_additionalArguments;

    friend class GdbServerProviderConfigWidget;
};

// GdbServerProviderConfigWidget

class GdbServerProviderConfigWidget : public IDebugServerProviderConfigWidget
{
public:
    explicit GdbServerProviderConfigWidget(GdbServerProvider *provider);
    void apply() override;
    void discard() override;

protected:
    GdbServerProvider::StartupMode startupModeFromIndex(int idx) const;
    GdbServerProvider::StartupMode startupMode() const;
    void setStartupMode(GdbServerProvider::StartupMode mode);
    void populateStartupModes();
    Utils::FilePath peripheralDescriptionFile() const;
    void setPeripheralDescriptionFile(const Utils::FilePath &file);
    void setFromProvider();

    static QString defaultInitCommandsTooltip();
    static QString defaultResetCommandsTooltip();

    QComboBox *m_startupModeComboBox = nullptr;
    Utils::PathChooser *m_peripheralDescriptionFileChooser = nullptr;
};

} // BareMetal::Internal

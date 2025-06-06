// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <extensionsystem/iplugin.h>

#if defined(PLUGIN1_LIBRARY)
#  define PLUGIN1_EXPORT Q_DECL_EXPORT
#elif defined(PLUGIN1_STATIC_LIBRARY)
#  define PLUGIN1_EXPORT
#else
#  define PLUGIN1_EXPORT Q_DECL_IMPORT
#endif

namespace Plugin1 {

class PLUGIN1_EXPORT MyPlugin1 final : public ExtensionSystem::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "plugin" FILE "plugin1.json")

public:
    MyPlugin1() = default;
    ~MyPlugin1() final;

    Utils::Result<> initialize(const QStringList &arguments) final;
    void extensionsInitialized() final;

private:
    bool initializeCalled = false;
    QObject *object1 = nullptr;
    QObject *object2 = nullptr;
};

} // namespace Plugin1

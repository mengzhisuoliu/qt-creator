// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <QtGlobal>

namespace QbsProjectManager {
namespace Constants {

// Contexts
const char PROJECT_ID[] = "Qbs.QbsProject";

// Actions:
const char ACTION_REPARSE_QBS[] = "Qbs.Reparse";
const char ACTION_REPARSE_QBS_CONTEXT[] = "Qbs.ReparseCtx";
const char ACTION_BUILD_FILE_CONTEXT[] = "Qbs.BuildFileCtx";
const char ACTION_BUILD_FILE[] = "Qbs.BuildFile";
const char ACTION_BUILD_PRODUCT_CONTEXT[] = "Qbs.BuildProductCtx";
const char ACTION_BUILD_PRODUCT[] = "Qbs.BuildProduct";
const char ACTION_BUILD_SUBPROJECT_CONTEXT[] = "Qbs.BuildSubprojectCtx";
const char ACTION_BUILD_SUBPROJECT[] = "Qbs.BuildSubproduct";
const char ACTION_CLEAN_PRODUCT_CONTEXT[] = "Qbs.CleanProductCtx";
const char ACTION_CLEAN_PRODUCT[] = "Qbs.CleanProduct";
const char ACTION_CLEAN_SUBPROJECT_CONTEXT[] = "Qbs.CleanSubprojectCtx";
const char ACTION_CLEAN_SUBPROJECT[] = "Qbs.CleanSubproject";
const char ACTION_REBUILD_PRODUCT_CONTEXT[] = "Qbs.RebuildProductCtx";
const char ACTION_REBUILD_PRODUCT[] = "Qbs.RebuildProduct";
const char ACTION_REBUILD_SUBPROJECT_CONTEXT[] = "Qbs.RebuildSubprojectCtx";
const char ACTION_REBUILD_SUBPROJECT[] = "Qbs.RebuildSubproject";

// Ids:
const char QBS_BUILDSTEP_ID[] = "Qbs.BuildStep";
const char QBS_CLEANSTEP_ID[] = "Qbs.CleanStep";
const char QBS_INSTALLSTEP_ID[] = "Qbs.InstallStep";
const char QBS_BC_ID[] = "Qbs.QbsBuildConfiguration";

// QBS strings:
const char QBS_VARIANT_DEBUG[] = "debug";
const char QBS_VARIANT_RELEASE[] = "release";
const char QBS_VARIANT_PROFILING[] = "profiling";

const char QBS_CONFIG_VARIANT_KEY[] = "qbs.defaultBuildVariant";
const char QBS_CONFIG_PROFILE_KEY[] = "qbs.profile";
const char QBS_INSTALL_ROOT_KEY[] = "qbs.installRoot";
const char QBS_CONFIG_DECLARATIVE_DEBUG_KEY[] = "modules.Qt.declarative.qmlDebugging";
const char QBS_CONFIG_QUICK_DEBUG_KEY[] = "modules.Qt.quick.qmlDebugging";
const char QBS_CONFIG_QUICK_COMPILER_KEY[] = "modules.Qt.quick.useCompiler";
const char QBS_CONFIG_SEPARATE_DEBUG_INFO_KEY[] = "modules.cpp.separateDebugInformation";
const char QBS_FORCE_PROBES_KEY[] = "qbspm.forceProbes";
const char QBS_RESTORE_BEHAVIOR_KEY[] = "restore-behavior";

// Toolchain related settings:
const char QBS_TARGETPLATFORM[] = "qbs.targetPlatform";
const char QBS_SYSROOT[] = "qbs.sysroot";
const char QBS_ARCHITECTURES[] = "qbs.architectures";
const char QBS_ARCHITECTURE[] = "qbs.architecture";
const char CPP_TOOLCHAINPATH[] = "cpp.toolchainInstallPath";
const char CPP_TOOLCHAINPREFIX[] = "cpp.toolchainPrefix";
const char CPP_COMPILERNAME[] = "cpp.compilerName";
const char CPP_CCOMPILERNAME[] = "cpp.cCompilerName";
const char CPP_CXXCOMPILERNAME[] = "cpp.cxxCompilerName";
const char CPP_PLATFORMCOMMONCOMPILERFLAGS[] = "cpp.platformCommonCompilerFlags";
const char CPP_PLATFORMLINKERFLAGS[] = "cpp.platformLinkerFlags";
const char CPP_VCVARSALLPATH[] = "cpp.vcvarsallPath";
const char XCODE_DEVELOPERPATH[] = "xcode.developerPath";
const char XCODE_SDK[] = "xcode.sdk";
const char JAVA_ADDITIONAL_CLASSPATHS[] = "java.additionalClassPaths";

// Settings page
const char QBS_SETTINGS_CATEGORY[]  = "K.Qbs";
const char QBS_SETTINGS_TR_CATEGORY[] = QT_TRANSLATE_NOOP("QtC::QbsProjectManager", "Qbs");
const char QBS_SETTINGS_CATEGORY_ICON[]  = ":/projectexplorer/images/build.png";

const char QBS_PROFILING_ENV[] = "QTC_QBS_PROFILING";

} // namespace Constants
} // namespace QbsProjectManager

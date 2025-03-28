// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "tst_testcore.h"

#include <designersettings.h>
#include <externaldependenciesinterface.h>
#include <model.h>
#include <modelmerger.h>
#include <modelnode.h>
#include <nodeinstance.h>
#include <nodeinstanceview.h>
#include <rewritingexception.h>
#include <stylesheetmerger.h>
#include <qmlanchors.h>
#include <qmlmodelnodefacade.h>

#ifndef QDS_USE_PROJECTSTORAGE
#include <metainfo.h>
#include <subcomponentmanager.h>
#endif

#include "../testconnectionmanager.h"
#include "../testview.h"
#include <abstractproperty.h>
#include <bindingproperty.h>
#include <nodeproperty.h>
#include <variantproperty.h>

#include <nodelistproperty.h>
#include <nodeabstractproperty.h>
#include <componenttextmodifier.h>

#include <bytearraymodifier.h>
#include "testrewriterview.h"

#include <qmljs/qmljsinterpreter.h>
#include <qmljs/qmljssimplereader.h>
#include <extensionsystem/pluginmanager.h>

#include <utils/fileutils.h>
#include <utils/qtcsettings.h>

#include <QDebug>
#include <QGraphicsObject>
#include <QPlainTextEdit>
#include <QQueue>
#include <QScopedPointer>
#include <QTest>
#include <QVariant>

//TESTED_COMPONENT=src/plugins/qmldesigner/designercore

using namespace QmlDesigner;
#include <cstdio>
#include "../common/statichelpers.cpp"

#include <qmljstools/qmljsmodelmanager.h>
#include <qmljs/qmljsinterpreter.h>

#include <memory>

QT_BEGIN_NAMESPACE

//Allow comparison of QByteArray and QString. We always assume utf8 as the encoding.
namespace QTest {
 bool qCompare(const QString &string, const QByteArray &array, const char *actual,
               const char *expected, const char *file, int line)
 {
     return qCompare(string.toUtf8(), array, actual, expected, file, line);
 }

 bool qCompare(const QByteArray &array, const QString &string, const char *actual,
               const char *expected, const char *file, int line)
 {
     return qCompare(array, string.toUtf8(), actual, expected, file, line);
 }


}

QT_END_NAMESPACE

static QString stripWhiteSpaces(const QString &str)
{
    QStringList list = str.split("\n", Qt::SkipEmptyParts);

    QString result;
    for (auto &str : list)
        result.append(str.trimmed() + "\n");

    return result;
}

class TestModelManager : public QmlJSTools::Internal::ModelManager
{
public:
    TestModelManager() : QmlJSTools::Internal::ModelManager()
    {
        //loadQmlTypeDescriptions(resourcePath());
    }
    void updateSourceFiles(const QList<Utils::FilePath> &files, bool emitDocumentOnDiskChanged)
    {
        refreshSourceFiles(files, emitDocumentOnDiskChanged).waitForFinished();
    }

    QmlJS::LibraryInfo builtins(const QmlJS::Document::Ptr &) const
    {
        return QmlJS::LibraryInfo();
    }
};

static void initializeMetaTypeSystem(const QString &resourcePath)
{
    const QDir typeFileDir(resourcePath + QLatin1String("/qml-type-descriptions"));
    const QStringList qmlExtensions = QStringList() << QLatin1String("*.qml");
    const QFileInfoList qmlFiles = typeFileDir.entryInfoList(qmlExtensions,
                                                             QDir::Files,
                                                             QDir::Name);

    QStringList errorsAndWarnings;
    QmlJS::CppQmlTypesLoader::loadQmlTypes(qmlFiles, &errorsAndWarnings, &errorsAndWarnings);
    for (const QString &errorAndWarning : std::as_const(errorsAndWarnings))
        qWarning() << qPrintable(errorAndWarning);
}


class ExternalDependenciesFake : public QObject, public ExternalDependenciesInterface
{
public:
    ExternalDependenciesFake(Model *model)
        : model{model}
    {}

    double formEditorDevicePixelRatio() const override { return 1.; }
    QString currentProjectDirPath() const override
    {
        return QFileInfo(model->fileUrl().toLocalFile()).absolutePath();
    }

    QUrl currentResourcePath() const override
    {
        return QUrl::fromLocalFile(QFileInfo(model->fileUrl().toLocalFile()).absolutePath());
    }

    QString defaultPuppetFallbackDirectory() const override { return {}; }
    QString defaultPuppetToplevelBuildDirectory() const override { return {}; }
    QString qmlPuppetFallbackDirectory() const override { return {}; }
    QUrl projectUrl() const override { return {}; }
    QString projectName() const override { return {}; }
    void parseItemLibraryDescriptions() override {}
    const QmlDesigner::DesignerSettings &designerSettings() const override { return settings; }
    void undoOnCurrentDesignDocument() override {}
    bool viewManagerUsesRewriterView(class RewriterView *) const override { return true; }
    void viewManagerDiableWidgets() override {}
    QString itemLibraryImportUserComponentsTitle() const override { return {}; }
    bool isQt6Import() const override { return true; }
    bool hasStartupTarget() const override { return true; }
    PuppetStartData puppetStartData(const class Model &) const override { return {}; }
    bool instantQmlTextUpdate() const override { return true; }
    Utils::FilePath qmlPuppetPath() const override { return {}; }
    QStringList modulePaths() const override { return {}; }
    QStringList projectModulePaths() const override { return {}; }
    bool isQt6Project() const override { return {}; }
    bool isQtForMcusProject() const override { return {}; }
    QString qtQuickVersion() const override { return {}; }

    Utils::FilePath resourcePath(const QString &) const override { return {}; }

    QString userResourcePath(QStringView) const override { return {}; }

public:
    Utils::QtcSettings qsettings;
    QmlDesigner::DesignerSettings settings{&qsettings};
    Model *model;
};

namespace {

ModelPointer createModel(const QString &typeName,
                         int major = 2,
                         int minor = 1,
                         Model *metaInfoPropxyModel = 0)
{
    QApplication::processEvents();

#ifdef QDS_USE_PROJECTSTORAGE
    auto model = metaInfoPropxyModel->createModel(typeName.toUtf8());
#else
    auto model = QmlDesigner::Model::create(typeName.toUtf8(), major, minor, metaInfoPropxyModel);
#endif
    QPlainTextEdit *textEdit = new QPlainTextEdit;
    QObject::connect(model.get(), &QObject::destroyed, textEdit, &QObject::deleteLater);
    textEdit->setPlainText(QString("import %1 %3.%4; %2{}")
                               .arg(typeName.split(".").first())
                               .arg(typeName.split(".").last())
                               .arg(major)
                               .arg(minor));

    NotIndentingTextEditModifier *modifier = new NotIndentingTextEditModifier(textEdit);
    modifier->setParent(textEdit);

    auto externalDependencies = new ExternalDependenciesFake{model.get()};
    externalDependencies->setParent(model.get());

    auto rewriterView = new QmlDesigner::RewriterView(*externalDependencies,
                                                      QmlDesigner::RewriterView::Validate);
    rewriterView->setParent(model.get());
    rewriterView->setCheckSemanticErrors(false);
    rewriterView->setTextModifier(modifier);

    model->attachView(rewriterView);

    return model;
}

auto createTextRewriterView(
    Model &model, RewriterView::DifferenceHandling differenceHandling = RewriterView::Amend)
{
    auto externalDependencies = new ExternalDependenciesFake{&model};
    auto rewriter = std::make_unique<TestRewriterView>(*externalDependencies, differenceHandling);
    externalDependencies->setParent(rewriter.get());

    return rewriter;
}

} // namespace

tst_TestCore::tst_TestCore()
    : externalDependencies{std::make_unique<ExternalDependenciesFake>(nullptr)}
{
    QLoggingCategory::setFilterRules(QStringLiteral("qtc.qmljs.imports=false"));
    //QLoggingCategory::setFilterRules(QStringLiteral("*.info=false\n*.debug=false\n*.warning=false"));
    QLoggingCategory::setFilterRules(QStringLiteral("*.warning=false"));
}

tst_TestCore::~tst_TestCore() = default;

void tst_TestCore::initTestCase()
{
    QmlModelNodeFacade::enableUglyWorkaroundForIsValidQmlModelNodeFacadeInTests();
#ifndef QDS_USE_PROJECTSTORAGE
    MetaInfo::disableParseItemLibraryDescriptionsUgly();
#endif
    Exception::setShouldAssert(false);

    if (!QmlJS::ModelManagerInterface::instance())
        new TestModelManager;

    initializeMetaTypeSystem(IDE_DATA_PATH);

    QStringList basePaths;
    basePaths.append(QLibraryInfo::path(QLibraryInfo::Qml2ImportsPath));
    QmlJS::PathsAndLanguages lPaths;

    lPaths.maybeInsert(Utils::FilePath::fromString(basePaths.first()), QmlJS::Dialect::Qml);
    QmlJS::ModelManagerInterface::importScan(QmlJS::ModelManagerInterface::workingCopy(),
        lPaths, QmlJS::ModelManagerInterface::instance(), false);

   // Load plugins

#ifdef Q_OS_MAC
    const QString pluginPath = IDE_PLUGIN_PATH "/QmlDesigner";
#else
    const QString pluginPath = IDE_PLUGIN_PATH "/qmldesigner";
#endif

    qDebug() << pluginPath;
    Q_ASSERT(QFileInfo::exists(pluginPath));

#ifndef QDS_USE_PROJECTSTORAGE
    MetaInfo::initializeGlobal({pluginPath}, *externalDependencies);
#endif

    QFileInfo builtins(IDE_DATA_PATH "/qml-type-descriptions/builtins.qmltypes");
    QStringList errors, warnings;
    QmlJS::CppQmlTypesLoader::defaultQtObjects()
        = QmlJS::CppQmlTypesLoader::loadQmlTypes(QFileInfoList{builtins}, &errors, &warnings);
}

void tst_TestCore::cleanupTestCase()
{
}

void tst_TestCore::init()
{
}
void tst_TestCore::cleanup()
{
}

void tst_TestCore::testModelCreateCoreModel()
{
    auto model(createModel("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> testView(new TestView);
    QVERIFY(testView.data());
    model->attachView(testView.data());
}

// TODO: this need to e updated for states
void tst_TestCore::loadEmptyCoreModel()
{
    QFile file(":/fx/empty.qml");
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QPlainTextEdit textEdit2;
    textEdit2.setPlainText("import QtQuick 1.1; Item{}");
    NotIndentingTextEditModifier modifier2(&textEdit2);

    auto model2(Model::create("QtQuick.Item"));

    auto testRewriterView2 = createTextRewriterView(*model2);
    testRewriterView2->setTextModifier(&modifier2);
    model2->attachView(testRewriterView2.get());

    QVERIFY(compareTree(testRewriterView1->rootModelNode(), testRewriterView2->rootModelNode()));
}

void tst_TestCore::testRewriterView2()
{
    try {
        QPlainTextEdit textEdit;
        textEdit.setPlainText("import QtQuick 2.15;\n\nRectangle {\n}\n");
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Rectangle", 2, 1));
        QVERIFY(model.get());

        QScopedPointer<TestView> view(new TestView);
        QVERIFY(view.data());
        model->attachView(view.data());

        ModelNode rootModelNode(view->rootModelNode());
        QVERIFY(rootModelNode.isValid());
        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        while (testRewriterView->hasIncompleteTypeInformation()) {
            QApplication::processEvents(QEventLoop::AllEvents, 1000);
        }

        ModelNode childNode(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 11, "data"));

        QVERIFY(childNode.isValid());
        childNode.setIdWithoutRefactoring("childNode");

        ModelNode childNode2(addNodeListChild(childNode, "QtQuick.Rectangle", 2, 11, "data"));
        childNode2.setIdWithoutRefactoring("childNode2");
        ModelNode childNode3(addNodeListChild(childNode2, "QtQuick.Rectangle", 2, 11, "data"));
        childNode3.setIdWithoutRefactoring("childNode3");
        ModelNode childNode4(addNodeListChild(childNode3, "QtQuick.Rectangle", 2, 11, "data"));
        childNode4.setIdWithoutRefactoring("childNode4");

        QVERIFY(childNode.isValid());
        QVERIFY(childNode2.isValid());
        QVERIFY(childNode3.isValid());
        QVERIFY(childNode4.isValid());

        testRewriterView->setModificationGroupActive(true);

        childNode.destroy();

        QVERIFY(!childNode.isValid());
        QVERIFY(!childNode2.isValid());
        QVERIFY(!childNode3.isValid());
        QVERIFY(!childNode4.isValid());

        QVERIFY(testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode));
        QVERIFY(!testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode2));
        QVERIFY(!testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode3));
        QVERIFY(!testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode4));

        QVERIFY(!rootModelNode.hasProperty("data"));

        testRewriterView->modelToTextMerger()->applyChanges();

        childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 11, "data");
        QVERIFY(testRewriterView->modelToTextMerger()->isNodeScheduledForAddition(childNode));

        testRewriterView->modelToTextMerger()->applyChanges();

        childNode.variantProperty("x").setValue(70);
        childNode.variantProperty("y").setValue(90);

        QCOMPARE(testRewriterView->modelToTextMerger()
                     ->findAddedVariantProperty(childNode.variantProperty("x"))
                     .value(),
                 QVariant(70));
        QCOMPARE(testRewriterView->modelToTextMerger()
                     ->findAddedVariantProperty(childNode.variantProperty("y"))
                     .value(),
                 QVariant(90));

        model->detachView(testRewriterView.get());
    } catch (Exception &e) {
        QFAIL(qPrintable(e.description()));
    }
}

void tst_TestCore::testRewriterView()
{
    try {
        const QLatin1String qmlString("import QtQuick 2.15\n"
                                      "Rectangle {\n"
                                      "}\n");

        QPlainTextEdit textEdit;
        textEdit.setPlainText(qmlString);
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 15));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        testRewriterView->setCheckSemanticErrors(true);
        model->attachView(testRewriterView.get());

        while (testRewriterView->hasIncompleteTypeInformation()) {
            QApplication::processEvents(QEventLoop::AllEvents, 1000);
        }

        textEdit.setPlainText(qmlString);

        ModelNode rootModelNode = testRewriterView->rootModelNode();

        ModelNode childNode(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 11, "data"));

        QVERIFY(childNode.isValid());
        childNode.setIdWithoutRefactoring("childNode");

        ModelNode childNode2(addNodeListChild(childNode, "QtQuick.Rectangle", 2, 11, "data"));
        childNode2.setIdWithoutRefactoring("childNode2");
        ModelNode childNode3(addNodeListChild(childNode2, "QtQuick.Rectangle", 2, 11, "data"));
        childNode3.setIdWithoutRefactoring("childNode3");
        ModelNode childNode4(addNodeListChild(childNode3, "QtQuick.Rectangle", 2, 11, "data"));
        childNode4.setIdWithoutRefactoring("childNode4");

        QVERIFY(childNode.isValid());
        QVERIFY(childNode2.isValid());
        QVERIFY(childNode3.isValid());
        QVERIFY(childNode4.isValid());

        testRewriterView->setModificationGroupActive(true);

        childNode.destroy();

        QVERIFY(!childNode.isValid());
        QVERIFY(!childNode2.isValid());
        QVERIFY(!childNode3.isValid());
        QVERIFY(!childNode4.isValid());

        QVERIFY(testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode));
        QVERIFY(!testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode2));
        QVERIFY(!testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode3));
        QVERIFY(!testRewriterView->modelToTextMerger()->isNodeScheduledForRemoval(childNode4));

        QVERIFY(!rootModelNode.hasProperty("data"));

        testRewriterView->modelToTextMerger()->applyChanges();

        childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 11, "data");
        QVERIFY(testRewriterView->modelToTextMerger()->isNodeScheduledForAddition(childNode));
        QVERIFY(childNode.isValid());

        testRewriterView->modelToTextMerger()->applyChanges();

        QVERIFY(childNode.isValid());
        childNode.variantProperty("x").setValue(70);
        childNode.variantProperty("y").setValue(90);

        QCOMPARE(testRewriterView->modelToTextMerger()->findAddedVariantProperty(childNode.variantProperty("x")).value(), QVariant(70));
        QCOMPARE(testRewriterView->modelToTextMerger()->findAddedVariantProperty(childNode.variantProperty("y")).value(), QVariant(90));

        model->detachView(testRewriterView.get());
    } catch (Exception &e) {
        QFAIL(qPrintable(e.description()));
    }
}

void tst_TestCore::testRewriterErrors()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText("import QtQuick 2.1;\n\nItem {\n}\n");
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());
    testRewriterView->setCheckSemanticErrors(true);
    textEdit.setPlainText("import QtQuick 2.1;\nRectangle {\ntest: blah\n}\n");

    QVERIFY(!testRewriterView->errors().isEmpty());

    textEdit.setPlainText("import QtQuick 2.1;\n\nItem {\n}\n");
    QVERIFY(testRewriterView->errors().isEmpty());
}

void tst_TestCore::saveEmptyCoreModel()
{
    QFile file(":/fx/empty.qml");
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QPlainTextEdit textEdit2;
    textEdit2.setPlainText("import QtQuick 1.1; Item{}");
    NotIndentingTextEditModifier modifier2(&textEdit2);

    auto model2(Model::create("QtQuick.Item"));

    auto testRewriterView2 = createTextRewriterView(*model2);
    testRewriterView2->setTextModifier(&modifier2);
    model2->attachView(testRewriterView2.get());

    QVERIFY(compareTree(testRewriterView1->rootModelNode(), testRewriterView2->rootModelNode()));
}

void tst_TestCore::loadAttributesInCoreModel()
{
    QFile file(":/fx/attributes.qml");
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QPlainTextEdit textEdit2;
    textEdit2.setPlainText("import QtQuick 1.1; Item{}");
    NotIndentingTextEditModifier modifier2(&textEdit2);

    auto model2(Model::create("QtQuick.Item"));

    auto testRewriterView2 = createTextRewriterView(*model2);
    testRewriterView2->setTextModifier(&modifier2);
    model2->attachView(testRewriterView2.get());

    ModelNode rootModelNode = testRewriterView2->rootModelNode();

    rootModelNode.setIdWithoutRefactoring("theItem");
    rootModelNode.variantProperty("x").setValue(QVariant(300));
    rootModelNode.variantProperty("visible").setValue(true);
    rootModelNode.variantProperty("scale").setValue(0.5);

    QVERIFY(compareTree(testRewriterView1->rootModelNode(), testRewriterView2->rootModelNode()));
}

void tst_TestCore::saveAttributesInCoreModel()
{
    QFile file(":/fx/attributes.qml");
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite | QIODevice::Text);
    buffer.write(modifier1.textDocument()->toPlainText().toUtf8());

    QPlainTextEdit textEdit2;
    textEdit2.setPlainText(QString::fromUtf8(buffer.data()));
    NotIndentingTextEditModifier modifier2(&textEdit2);

    auto model2(Model::create("QtQuick.Item"));

    auto testRewriterView2 = createTextRewriterView(*model2);
    testRewriterView2->setTextModifier(&modifier2);
    model2->attachView(testRewriterView2.get());

    QVERIFY(testRewriterView2->errors().isEmpty());

    QVERIFY(compareTree(testRewriterView1->rootModelNode(), testRewriterView2->rootModelNode()));
}

void tst_TestCore::testModelCreateRect()
{
    try {
        auto model(createModel("QtQuick.Item", 2, 0));
        QVERIFY(model.get());

        QScopedPointer<TestView> view(new TestView);
        QVERIFY(view.data());
        model->attachView(view.data());

        QVERIFY(view->rootModelNode().isValid());
        ModelNode childNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
        QVERIFY(childNode.isValid());
        QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode));
        QVERIFY(childNode.parentProperty().parentModelNode() == view->rootModelNode());
        QCOMPARE(childNode.simplifiedTypeName(), QmlDesigner::TypeName("Rectangle"));

        QVERIFY(childNode.id().isEmpty());

        childNode.setIdWithoutRefactoring("rect01");
        QCOMPARE(childNode.id(), QString("rect01"));

        childNode.variantProperty("x").setValue(100);
        childNode.variantProperty("y").setValue(100);
        childNode.variantProperty("width").setValue(100);
        childNode.variantProperty("height").setValue(100);

        QCOMPARE(childNode.propertyNames().count(), 4);
        //QCOMPARE(childNode.variantProperty("scale").value(), QVariant());

    } catch (Exception &) {
        QFAIL("Exception thrown");
    }
}


void tst_TestCore::testRewriterDynamicProperties()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  property int i\n"
                                  "  property int ii: 1\n"
                                  "  property bool b\n"
                                  "  property bool bb: true\n"
                                  "  property double d\n"
                                  "  property double dd: 1.1\n"
                                  "  property real r\n"
                                  "  property real rr: 1.1\n"
                                  "  property string s\n"
                                  "  property string ss: \"hello\"\n"
                                  "  property url u\n"
                                  "  property url uu: \"www\"\n"
                                  "  property color c\n"
                                  "  property color cc: \"#ffffff\"\n"
                                  "  property date t\n"
                                  "  property date tt: \"2000-03-20\"\n"
                                  "  property variant v\n"
                                  "  property variant vv: \"Hello\"\n"
                                  "}");

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    //
    // text2model
    //
    ModelNode rootModelNode = testRewriterView1->rootModelNode();
    QCOMPARE(rootModelNode.properties().count(), 18);
    QVERIFY(rootModelNode.hasVariantProperty("i"));
    QCOMPARE(rootModelNode.variantProperty("i").dynamicTypeName(), QmlDesigner::TypeName("int"));
    QCOMPARE(rootModelNode.variantProperty("i").value().typeId(), QMetaType::Int);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("i").value().toInt(), 0);

    QVERIFY(rootModelNode.hasVariantProperty("ii"));
    QCOMPARE(rootModelNode.variantProperty("ii").dynamicTypeName(), QmlDesigner::TypeName("int"));
    QCOMPARE(rootModelNode.variantProperty("ii").value().typeId(), QMetaType::Int);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("ii").value().toInt(), 1);

    QVERIFY(rootModelNode.hasVariantProperty("b"));
    QCOMPARE(rootModelNode.variantProperty("b").dynamicTypeName(), QmlDesigner::TypeName("bool"));
    QCOMPARE(rootModelNode.variantProperty("b").value().typeId(), QMetaType::Bool);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("b").value().toBool(), false);

    QVERIFY(rootModelNode.hasVariantProperty("bb"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("bb").value().toBool(), true);

    QVERIFY(rootModelNode.hasVariantProperty("d"));
    QCOMPARE(rootModelNode.variantProperty("d").dynamicTypeName(), QmlDesigner::TypeName("double"));
    QCOMPARE(rootModelNode.variantProperty("d").value().typeId(), QMetaType::Double);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("d").value().toDouble(), 0.0);

    QVERIFY(rootModelNode.hasVariantProperty("dd"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("dd").value().toDouble(), 1.1);

    QVERIFY(rootModelNode.hasVariantProperty("r"));
    QCOMPARE(rootModelNode.variantProperty("r").dynamicTypeName(), QmlDesigner::TypeName("real"));
    QCOMPARE(rootModelNode.variantProperty("r").value().typeId(), QMetaType::Double);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("r").value().toDouble(), 0.0);

    QVERIFY(rootModelNode.hasVariantProperty("rr"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("rr").value().toDouble(), 1.1);

    QVERIFY(rootModelNode.hasVariantProperty("s"));
    QCOMPARE(rootModelNode.variantProperty("s").dynamicTypeName(), QmlDesigner::TypeName("string"));
    QCOMPARE(rootModelNode.variantProperty("s").value().typeId(), QMetaType::QString);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("s").value().toString(), QString());

    QVERIFY(rootModelNode.hasVariantProperty("ss"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("ss").value().toString(), QString("hello"));

    QVERIFY(rootModelNode.hasVariantProperty("u"));
    QCOMPARE(rootModelNode.variantProperty("u").dynamicTypeName(), QmlDesigner::TypeName("url"));
    QCOMPARE(rootModelNode.variantProperty("u").value().typeId(), QMetaType::QUrl);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("u").value().toUrl(), QUrl());

    QVERIFY(rootModelNode.hasVariantProperty("uu"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("uu").value().toUrl(), QUrl("www"));

    QVERIFY(rootModelNode.hasVariantProperty("c"));
    QCOMPARE(rootModelNode.variantProperty("c").dynamicTypeName(), QmlDesigner::TypeName("color"));
    QCOMPARE(rootModelNode.variantProperty("c").value().typeId(), QMetaType::QColor);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("c").value().value<QColor>(), QColor());

    QVERIFY(rootModelNode.hasVariantProperty("cc"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("cc").value().value<QColor>(), QColor(255, 255, 255));

    QVERIFY(rootModelNode.hasVariantProperty("t"));
    QCOMPARE(rootModelNode.variantProperty("t").dynamicTypeName(), QmlDesigner::TypeName("date"));
    QCOMPARE(rootModelNode.variantProperty("t").value().typeId(), QMetaType::QDate);
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("t").value().value<QDate>(), QDate());

    QVERIFY(rootModelNode.hasVariantProperty("tt"));
    QCOMPARE(testRewriterView1->rootModelNode().variantProperty("tt").value().value<QDate>(), QDate(2000, 3, 20));

    QVERIFY(rootModelNode.hasVariantProperty("v"));
    QCOMPARE(rootModelNode.variantProperty("v").dynamicTypeName(), QmlDesigner::TypeName("variant"));
    const int type = rootModelNode.variantProperty("v").value().typeId();
    QCOMPARE(type, QMetaType::fromName("QVariant").id());

    QVERIFY(rootModelNode.hasVariantProperty("vv"));
    const QString inThere = testRewriterView1->rootModelNode().variantProperty("vv").value().value<QString>();
    QCOMPARE(inThere, QString("Hello"));

    rootModelNode.variantProperty("vv").setDynamicTypeNameAndValue("variant", "hallo2");

    // test model2text
    //    QPlainTextEdit textEdit2;
    //    textEdit2.setPlainText("import QtQuick 1.1; Item{}");
    //    NotIndentingTextEditModifier modifier2(&textEdit2);
    //
    //   auto model2(Model::create("QtQuick.Item"));
    //
    //    auto testRewriterView2 = createTextRewriterView(*model2);
    //    testRewriterView2->setTextModifier(&modifier2);
    //    model2->attachView(testRewriterView2.get());
    //
    //    testRewriterView2->rootModelNode().variantProperty("pushed").setDynamicTypeNameAndValue("bool", QVariant(false));
    //
    //    QVERIFY(compareTree(testRewriterView1->rootModelNode(), testRewriterView2->rootModelNode()));
}

void tst_TestCore::testRewriterGroupedProperties()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Text {\n"
                                  "  font {\n"
                                  "    pointSize: 10\n"
                                  "    underline: true\n"
                                  "  }\n"
                                  "}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier1(&textEdit);

    auto model1(Model::create("QtQuick.Text"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    //
    // text2model
    //
    ModelNode rootModelNode = testRewriterView1->rootModelNode();
    QCOMPARE(rootModelNode.property("font.pointSize").toVariantProperty().value().toDouble(), 10.0);
    QCOMPARE(rootModelNode.property("font.underline").toVariantProperty().value().toBool(), true);

    rootModelNode.removeProperty("font.underline");
    QCOMPARE(rootModelNode.property("font.pointSize").toVariantProperty().value().toDouble(), 10.0);
    QVERIFY(!rootModelNode.hasProperty("font.underline"));

    rootModelNode.variantProperty("font.pointSize").setValue(20.0);
    QCOMPARE(rootModelNode.property("font.pointSize").toVariantProperty().value().toDouble(), 20.0);

    rootModelNode.removeProperty("font.pointSize");
    const QLatin1String expected("\n"
                                 "import QtQuick 2.1\n"
                                 "\n"
                                 "Text {\n"
                                 "}\n");

    QCOMPARE(textEdit.toPlainText(), expected);
}

void tst_TestCore::testRewriterPreserveOrder()
{
    const QLatin1String qmlString1("\n"
                                   "import QtQuick 2.1\n"
                                   "\n"
                                   "Rectangle {\n"
                                   "width: 640\n"
                                   "height: 480\n"
                                   "\n"
                                   "states: [\n"
                                   "State {\n"
                                   "name: \"State1\"\n"
                                   "}\n"
                                   "]\n"
                                   "\n"
                                   "Rectangle {\n"
                                   "id: rectangle1\n"
                                   "x: 18\n"
                                   "y: 19\n"
                                   "width: 100\n"
                                   "height: 100\n"
                                   "}\n"
                                   "}\n");
    const QLatin1String qmlString2("\n"
                                   "import QtQuick 2.1\n"
                                   "\n"
                                   "Rectangle {\n"
                                   "width: 640\n"
                                   "height: 480\n"
                                   "\n"
                                   "Rectangle {\n"
                                   "id: rectangle1\n"
                                   "x: 18\n"
                                   "y: 19\n"
                                   "width: 100\n"
                                   "height: 100\n"
                                   "}\n"
                                   "states: [\n"
                                   "State {\n"
                                   "name: \"State1\"\n"
                                   "}\n"
                                   "]\n"
                                   "\n"
                                   "}\n");

    {
        QPlainTextEdit textEdit;
        textEdit.setPlainText(qmlString2);
        NotIndentingTextEditModifier modifier(&textEdit);

        auto model(Model::create("QtQuick.Text"));

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&modifier);
        model->attachView(testRewriterView.get());

        QVERIFY(testRewriterView->errors().isEmpty());

        ModelNode rootModelNode = testRewriterView->rootModelNode();
        QVERIFY(rootModelNode.isValid());

        RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");

        ModelNode newModelNode = testRewriterView->createModelNode("QtQuick.Rectangle", 1, 0);

        newModelNode.setParentProperty(rootModelNode.nodeAbstractProperty("data"));

        newModelNode.setIdWithoutRefactoring("rectangle2");
        QCOMPARE(newModelNode.id(), QString("rectangle2"));

        newModelNode.variantProperty("x").setValue(10);
        newModelNode.variantProperty("y").setValue(10);
        newModelNode.variantProperty("width").setValue(100);
        newModelNode.variantProperty("height").setValue(100);

        transaction.commit();

        QCOMPARE(newModelNode.id(), QString("rectangle2"));
    }

    {
        QPlainTextEdit textEdit;
        textEdit.setPlainText(qmlString1);
        NotIndentingTextEditModifier modifier(&textEdit);

        auto model(Model::create("QtQuick.Text"));

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&modifier);
        model->attachView(testRewriterView.get());

        QVERIFY(testRewriterView->errors().isEmpty());

        ModelNode rootModelNode = testRewriterView->rootModelNode();
        QVERIFY(rootModelNode.isValid());

        RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");

        ModelNode newModelNode = testRewriterView->createModelNode("QtQuick.Rectangle", 1, 0);

        newModelNode.setParentProperty(rootModelNode.nodeAbstractProperty("data"));

        newModelNode.setIdWithoutRefactoring("rectangle2");
        QCOMPARE(newModelNode.id(), QString("rectangle2"));

        newModelNode.variantProperty("x").setValue(10);
        newModelNode.variantProperty("y").setValue(10);
        newModelNode.variantProperty("width").setValue(100);
        newModelNode.variantProperty("height").setValue(100);

        transaction.commit();

        QCOMPARE(newModelNode.id(), QString("rectangle2"));
    }
}

void tst_TestCore::testRewriterActionCompression()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  id: root\n"
                                  "  Rectangle {\n"
                                  "    id: rect1\n"
                                  "    x: 10\n"
                                  "    y: 10\n"
                                  "  }\n"
                                  "  Rectangle {\n"
                                  "    id: rect2\n"
                                  "    x: 10\n"
                                  "    y: 10\n"
                                  "  }\n"
                                  "}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier1(&textEdit);

    auto model1(Model::create("QtQuick.Rectangle"));

    auto testRewriterView = createTextRewriterView(*model1);
    testRewriterView->setTextModifier(&modifier1);
    model1->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    ModelNode rect1 = rootModelNode.property("data").toNodeListProperty().toModelNodeList().at(0);
    ModelNode rect2 = rootModelNode.property("data").toNodeListProperty().toModelNodeList().at(1);

    QVERIFY(rect1.isValid());
    QVERIFY(rect2.isValid());

    RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");
    rect1.nodeListProperty("data").reparentHere(rect2);
    rect2.variantProperty("x").setValue(1.0);
    rect2.variantProperty("y").setValue(1.0);

    rootModelNode.nodeListProperty("data").reparentHere(rect2);
    rect2.variantProperty("x").setValue(9.0);
    rect2.variantProperty("y").setValue(9.0);
    transaction.commit();

    const QLatin1String expected("\n"
                                 "import QtQuick 2.1\n"
                                 "\n"
                                 "Rectangle {\n"
                                 "  id: root\n"
                                 "  Rectangle {\n"
                                 "    id: rect1\n"
                                 "    x: 10\n"
                                 "    y: 10\n"
                                 "  }\n"
                                 "  Rectangle {\n"
                                 "    id: rect2\n"
                                 "    x: 9\n"
                                 "    y: 9\n"
                                 "  }\n"
                                 "}\n");
    QCOMPARE(textEdit.toPlainText(), expected);
}

void tst_TestCore::testRewriterImports()
{
    QString fileName = QString(TESTSRCDIR) + "/../data/fx/imports.qml";
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    model->setFileUrl(QUrl::fromLocalFile(fileName));

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    QVERIFY(model->imports().size() == 3);

    // import QtQuick 1.1
    Import import = model->imports().at(0);
    QVERIFY(import.isLibraryImport());
    QCOMPARE(import.url(), QString("QtQuick"));
    QVERIFY(import.hasVersion());
    QCOMPARE(import.version(), QString("2.15"));
    QVERIFY(!import.hasAlias());

    // import "subitems"
    import = model->imports().at(1);
    QVERIFY(import.isFileImport());
    QCOMPARE(import.file(), QString("subitems"));
    QVERIFY(!import.hasVersion());
    QVERIFY(!import.hasAlias());

    // import QtWebKit 1.0 as Web
    import = model->imports().at(2);
    QVERIFY(import.isLibraryImport());
    QCOMPARE(import.url(), QString("QtWebKit"));
    QVERIFY(import.hasVersion());
    QCOMPARE(import.version(), QString("1.0"));
    QVERIFY(import.hasAlias());
    QCOMPARE(import.alias(), QString("Web"));
}

void tst_TestCore::testRewriterChangeImports()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Rectangle"));

    auto testRewriterView = createTextRewriterView(*model, RewriterView::Amend);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    //
    // Add / Remove an import in the model
    //
    Import webkitImport = Import::createLibraryImport("QtWebKit", "1.0");

    Imports importList;
    importList << webkitImport;

    model->changeImports(importList, Imports());

    const QLatin1String qmlWithImport("\n"
                                      "import QtQuick 2.1\n"
                                      "import QtWebKit 1.0\n"
                                      "\n"
                                      "Rectangle {}\n");
    QCOMPARE(textEdit.toPlainText(), qmlWithImport);

    model->changeImports(Imports(), importList);

    QCOMPARE(model->imports().size(), 1);
    QCOMPARE(model->imports().first(), Import::createLibraryImport("QtQuick", "2.1"));

    QCOMPARE(textEdit.toPlainText(), qmlString);


    //
    // Add / Remove an import in the model (with alias)
    //

    Import webkitImportAlias = Import::createLibraryImport("QtWebKit", "1.0", "Web");

    model->changeImports(Imports() << webkitImportAlias, Imports() <<  webkitImport);

    const QLatin1String qmlWithAliasImport("\n"
                                           "import QtQuick 2.1\n"
                                           "import QtWebKit 1.0 as Web\n"
                                           "\n"
                                           "Rectangle {}\n");
    QCOMPARE(textEdit.toPlainText(), qmlWithAliasImport);

    model->changeImports(Imports(), Imports() << webkitImportAlias);
    QCOMPARE(model->imports().first(), Import::createLibraryImport("QtQuick", "2.1"));

    QCOMPARE(textEdit.toPlainText(), qmlString);

    //
    // Add / Remove an import in text
    //
    textEdit.setPlainText(qmlWithImport);
    QCOMPARE(model->imports().size(), 2);
    QCOMPARE(model->imports().first(), Import::createLibraryImport("QtQuick", "2.1"));
    QCOMPARE(model->imports().last(), Import::createLibraryImport("QtWebKit", "1.0"));

    textEdit.setPlainText(qmlWithAliasImport);
    QCOMPARE(model->imports().size(), 2);
    QCOMPARE(model->imports().first(), Import::createLibraryImport("QtQuick", "2.1"));
    QCOMPARE(model->imports().last(), Import::createLibraryImport("QtWebKit", "1.0", "Web"));

    textEdit.setPlainText(qmlString);
    QCOMPARE(model->imports().size(), 1);
    QCOMPARE(model->imports().first(), Import::createLibraryImport("QtQuick", "2.1"));
}

void tst_TestCore::testRewriterUnicodeChars()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Text {\n"
                                  "    text: \"test\""
                                  "}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Rectangle"));

    auto testRewriterView = createTextRewriterView(*model, RewriterView::Amend);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    rootModelNode.variantProperty("text").setValue("\\u2795");

    const QLatin1String unicodeChar("\nimport QtQuick 2.1\n\nText {\n    text: \"\\u2795\"}\n");

     QCOMPARE(textEdit.toPlainText(), unicodeChar);
}

void tst_TestCore::testRewriterTransactionAddingAfterReparenting()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Rectangle"));

    auto testRewriterView = createTextRewriterView(*model, RewriterView::Amend);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    {
        /* Regression test
         * If a node is not a direct child node of the root item we did get an exception when adding properties
         * after the reparent */

        RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");
        ModelNode rectangle = testRewriterView->createModelNode("QtQuick.Rectangle", 2, 0);
        rootModelNode.nodeListProperty("data").reparentHere(rectangle);
        ModelNode rectangle2 = testRewriterView->createModelNode("QtQuick.Rectangle", 2, 0);
        rectangle.nodeListProperty("data").reparentHere(rectangle2);

        rectangle2.variantProperty("width").setValue(100);
    }
}

void tst_TestCore::testRewriterReparentToNewNode()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "  Item {}\n"
                                  "  Item {}\n"
                                  "  Item {}\n"
                                  "  Item {}\n"
                                  "}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Rectangle"));

    auto testRewriterView = createTextRewriterView(*model, RewriterView::Amend);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    const QList<ModelNode> children = rootModelNode.directSubModelNodes();

    ModelNode rectangle = testRewriterView->createModelNode("QtQuick.Rectangle");
    rootModelNode.nodeListProperty("data").reparentHere(rectangle);

    rectangle.setIdWithoutRefactoring("newParent");

    QVERIFY(rectangle.isValid());

    for (const ModelNode &child : children)
        rectangle.nodeListProperty("data").reparentHere(child);

    {
        RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");
        ModelNode rectangle = testRewriterView->createModelNode("QtQuick.Rectangle");
        rootModelNode.nodeListProperty("data").reparentHere(rectangle);

        rectangle.setIdWithoutRefactoring("newParent2");

        for (const ModelNode &child : children)
            rectangle.nodeListProperty("data").reparentHere(child);
    }

    QCOMPARE(testRewriterView->allModelNodes().count(), 7);

    const QLatin1String expectedOutcome("\nimport QtQuick 2.0\n\n"
                                        "Item {\n\n"
                                        "  Rectangle {\n"
                                        "  id: newParent\n"
                                        "  }\n\n"
                                        "  Rectangle {\n"
                                        "  id: newParent2\n"
                                        "  Item {\n"
                                        "  }\n\n"
                                        "  Item {\n"
                                        "  }\n\n"
                                        "  Item {\n"
                                        "  }\n\n"
                                        "  Item {\n"
                                        "  }\n"
                                        "  }\n}\n");


    QCOMPARE(textEdit.toPlainText(), expectedOutcome);

    rectangle.destroy();

    QCOMPARE(testRewriterView->allModelNodes().count(), 6);

    {
        RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");

        ModelNode newChild = testRewriterView->createModelNode("QtQuick.Rectangle");
        rootModelNode.nodeListProperty("data").reparentHere(newChild);
        newChild.setIdWithoutRefactoring("newChild");
        ModelNode newParent = testRewriterView->createModelNode("QtQuick.Rectangle");
        rootModelNode.nodeListProperty("data").reparentHere(newParent);

        newParent.setIdWithoutRefactoring("newParent3");

        for (const ModelNode &child : children)
            newParent.nodeListProperty("data").reparentHere(child);

        newParent.nodeListProperty("data").reparentHere(newChild);
    }

    QCOMPARE(testRewriterView->allModelNodes().count(), 8);
}

void tst_TestCore::testRewriterBehaivours()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "  Item {}\n"
                                  "  Item {}\n"
                                  "  Behavior on width {\n"
                                  "      NumberAnimation { duration: 1000 }\n"
                                  "  }\n"
                                  "  Item {}\n"
                                  "}\n");


    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Rectangle"));

    auto testRewriterView = createTextRewriterView(*model, RewriterView::Amend);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    //qDebug() << testRewriterView->errors().first().toString();
    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    const QList<ModelNode> children = rootModelNode.directSubModelNodes();

    QCOMPARE(children.count(), 4);

    ModelNode behavior;
    for (const ModelNode &child : children) {
        if (child.type() == "QtQuick.Behavior")
            behavior = child;
    }

    QVERIFY(behavior.isValid());
    QVERIFY(!behavior.behaviorPropertyName().isEmpty());
    QCOMPARE(behavior.behaviorPropertyName(), "width");

    QVERIFY(!behavior.directSubModelNodes().isEmpty());

    ModelNode animation = behavior.directSubModelNodes().first();

    QVERIFY(animation.isValid());

    NodeMetaInfo metaInfo = behavior.metaInfo();

    QVERIFY(metaInfo.isValid());

#ifdef QDS_USE_PROJECTSTORAGE
    ModelNode newBehavior = testRewriterView->createModelNode("Behavior",
                                                              {},
                                                              {},
                                                              {},
                                                              ModelNode::NodeWithoutSource,
                                                              "height");
#else
    ModelNode newBehavior = testRewriterView->createModelNode("QtQuick.Behavior",
                                                              metaInfo.majorVersion(),
                                                              metaInfo.minorVersion(),
                                                              {},
                                                              {},
                                                              {},
                                                              ModelNode::NodeWithoutSource,
                                                              "height");
#endif
    rootModelNode.defaultNodeListProperty().reparentHere(newBehavior);

    QCOMPARE(newBehavior.behaviorPropertyName(), "height");

    metaInfo = animation.metaInfo();
    QVERIFY(metaInfo.isValid());
#ifdef QDS_USE_PROJECTSTORAGE
    ModelNode newAnimation = testRewriterView->createModelNode(model->exportedTypeNameForMetaInfo(metaInfo).name.toQByteArray());
#else
    ModelNode newAnimation = testRewriterView->createModelNode(metaInfo.typeName(),
                                                               metaInfo.majorVersion(),
                                                               metaInfo.minorVersion());
#endif
    newBehavior.defaultNodeListProperty().reparentHere(newAnimation);

    newAnimation.variantProperty("duration").setValue(500);

    const QLatin1String expextedQmlCode(
        "\nimport QtQuick 2.0\n\n"
        "Item {\n  Item {}\n  Item {}\n"
        "  Behavior on width {\n  "
        "    NumberAnimation { duration: 1000 }\n"
        "  }\n"
        "  Item {}\n\n"
        "  Behavior on height {\n"
        "  NumberAnimation {\n"
        "  duration: 500\n"
        "  }\n  }\n}\n");


    QCOMPARE(textEdit.toPlainText(), expextedQmlCode);

    newBehavior.destroy();

    QVERIFY(!newBehavior.isValid());
}

void tst_TestCore::testRewriterSignalDefinition()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: root\n"
                                  "    x: 10;\n"
                                  "    y: 10;\n"
                                  "    signal testSignal()\n"
                                  "    signal testSignal2(float i)\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle1\n"
                                  "        x: 10;\n"
                                  "        y: 10;\n"
                                  "        signal testSignal\n"
                                  "    }\n"
                                  "}");


    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Text"));

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    QVERIFY(rootModelNode.hasProperty("testSignal"));
    QVERIFY(rootModelNode.hasProperty("testSignal2"));

    QVERIFY(rootModelNode.property("testSignal").isSignalDeclarationProperty());
    QVERIFY(rootModelNode.signalDeclarationProperty("testSignal").isValid());
    QCOMPARE(rootModelNode.signalDeclarationProperty("testSignal").signature(), "()");

    QVERIFY(rootModelNode.property("testSignal2").isSignalDeclarationProperty());
    QVERIFY(rootModelNode.signalDeclarationProperty("testSignal2").isValid());
    QCOMPARE(rootModelNode.signalDeclarationProperty("testSignal2").signature(), "(float i)");

    ModelNode rectangle = testRewriterView->modelNodeForId("rectangle1");
    QVERIFY(rectangle.isValid());

    QVERIFY(rectangle.hasProperty("testSignal"));

    QVERIFY(rectangle.property("testSignal").isSignalDeclarationProperty());
    QVERIFY(rectangle.signalDeclarationProperty("testSignal").isValid());
    QCOMPARE(rectangle.signalDeclarationProperty("testSignal").signature(), "()");

    rootModelNode.signalDeclarationProperty("addedSignal").setSignature("()");
    rootModelNode.signalDeclarationProperty("addedSignal").setSignature("(real i)");

    const QLatin1String expectedQmlCode(
        "\nimport QtQuick 2.1\n\n"
        "Rectangle {\n"
        "    id: root\n"
        "    x: 10;\n"
        "    y: 10;\n"
        "    signal addedSignal (real i)\n"
        "    signal testSignal()\n"
        "    signal testSignal2(float i)\n"
        "    Rectangle {\n"
        "        id: rectangle1\n"
        "        x: 10;\n"
        "        y: 10;\n"
        "        signal testSignal\n"
        "    }\n}");

    QCOMPARE(textEdit.toPlainText(), expectedQmlCode);

    rootModelNode.removeProperty("addedSignal");

    const QLatin1String expectedQmlCode2(
        "\nimport QtQuick 2.1\n\n"
        "Rectangle {\n"
        "    id: root\n"
        "    x: 10;\n"
        "    y: 10;\n"
        "    signal testSignal()\n"
        "    signal testSignal2(float i)\n"
        "    Rectangle {\n"
        "        id: rectangle1\n"
        "        x: 10;\n"
        "        y: 10;\n"
        "        signal testSignal\n"
        "    }\n}");

    QCOMPARE(textEdit.toPlainText(), expectedQmlCode2);

    testRewriterView->executeInTransaction("identifer", [rectangle](){
        ModelNode newRectangle = rectangle.view()->createModelNode(rectangle.type(),
                                                                   rectangle.majorVersion(),
                                                                   rectangle.minorVersion());

        QVERIFY(newRectangle.isValid());
        newRectangle.signalDeclarationProperty("newSignal").setSignature("()");
        newRectangle.setIdWithoutRefactoring("newRect");
        newRectangle.view()->rootModelNode().defaultNodeListProperty().reparentHere(newRectangle);
    });

    const QString expectedQmlCode3
        = expectedQmlCode2.chopped(1)
          + QLatin1String("\n    Rectangle {\n    id: newRect\n    signal newSignal ()\n    }\n}");

    QCOMPARE(textEdit.toPlainText(), expectedQmlCode3);
}

void tst_TestCore::testRewriterForGradientMagic()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: root\n"
                                  "    x: 10;\n"
                                  "    y: 10;\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle1\n"
                                  "        x: 10;\n"
                                  "        y: 10;\n"
                                  "    }\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle2\n"
                                  "        x: 100;\n"
                                  "        y: 100;\n"
                                  "        anchors.fill: root\n"
                                  "    }\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle3\n"
                                  "        x: 140;\n"
                                  "        y: 180;\n"
                                  "        gradient: Gradient {\n"
                                  "             GradientStop {\n"
                                  "                 position: 0\n"
                                  "                 color: \"white\"\n"
                                  "             }\n"
                                  "             GradientStop {\n"
                                  "                  position: 1\n"
                                  "                  color: \"black\"\n"
                                  "             }\n"
                                  "        }\n"
                                  "    }\n"
                                  "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Text"));

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    ModelNode myRect = testRewriterView->modelNodeForId(QLatin1String("rectangle3"));
    QVERIFY(myRect.isValid());
    myRect.variantProperty("rotation").setValue(QVariant(45));
    QVERIFY(myRect.isValid());

    auto model1(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model1.get());

    QScopedPointer<TestView> view1(new TestView);
    model1->attachView(view1.data());

    auto testRewriterView1 = createTextRewriterView(*model1);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item {}");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    RewriterTransaction transaction(view1->beginRewriterTransaction("TEST"));

    ModelMerger merger(view1.data());

    QVERIFY(myRect.isValid());
    merger.replaceModel(myRect);
    transaction.commit();
}

void tst_TestCore::testStatesVersionFailing()
{
    char qmlString[] = "import QtQuick\n"
                       "Rectangle {\n"
                       "id: root;\n"
                       "Rectangle {\n"
                       "id: rect1;\n"
                       "}\n"
                       "Rectangle {\n"
                       "id: rect2;\n"
                       "}\n"
                       "states: [\n"
                       "State {\n"
                       "name: \"state1\"\n"
                       "PropertyChanges {\n"
                       "target: rect1\n"
                       "}\n"
                       "}\n"
                       "]\n"
                       "}\n";

    Exception::setShouldAssert(true);

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 1);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().count(), 1);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().first(), QString("state1"));

    QmlModelState state = QmlItemNode(rootModelNode).states().state("state1");
    ModelNode stateNode = QmlItemNode(rootModelNode).states().state("state1").modelNode();
    QVERIFY(stateNode.isValid());

    NodeMetaInfo stateInfo = stateNode.metaInfo();

    QVERIFY(stateInfo.isValid());
    QmlModelState newState = state.duplicate("state2");

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().last(), QString("state2"));

    QCOMPARE(QmlItemNode(rootModelNode).states().state("state2"), newState);

#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(stateInfo.majorVersion(), newState.modelNode().majorVersion());
    QCOMPARE(stateInfo.minorVersion(), newState.modelNode().minorVersion());
#endif

    ModelNode rect1Node = view->modelNodeForId("rect1");
    QVERIFY(rect1Node.isValid());
    QmlObjectNode rect1(rect1Node);
    QmlPropertyChanges changes = newState.propertyChanges(rect1Node);
    QVERIFY(changes.isValid());

    NodeMetaInfo changeInfo = changes.modelNode().metaInfo();
    QVERIFY(changeInfo.isValid());

    QVERIFY(!changes.modelNode().hasProperty("x"));

    view->setCurrentState(newState);

    QVERIFY(view->currentState() == newState);

    QString oldText = textEdit.toPlainText();

    rect1.setVariantProperty("x", 100);
    QVERIFY(changes.modelNode().hasProperty("x"));
    QVERIFY(oldText != textEdit.toPlainText());

    ModelNode rect2Node = view->modelNodeForId("rect2");
    QVERIFY(rect2Node.isValid());
    QmlObjectNode rect2(rect2Node);
    QVERIFY(!newState.hasPropertyChanges(rect2Node));
    QmlPropertyChanges changes2 = newState.propertyChanges(rect2Node);

    QVERIFY(changes2.isValid());

    QVERIFY(view->currentState() == newState);

    oldText = textEdit.toPlainText();

    rect2.setVariantProperty("x", 100);
    changes2 = newState.propertyChanges(rect2Node);
    QVERIFY(changes2.isValid());
    QVERIFY(changes2.modelNode().hasProperty("x"));
    QVERIFY(oldText != textEdit.toPlainText());

#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(changeInfo.majorVersion(), changes2.modelNode().majorVersion());
    QCOMPARE(changeInfo.minorVersion(), changes2.modelNode().minorVersion());
#endif
}

void tst_TestCore::loadSubItems()
{
    QFile file(QString(TESTSRCDIR) + "/../data/fx/topitem.qml");
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item", 2, 0));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());
}

void tst_TestCore::createInvalidCoreModel()
{
    QSKIP("no direct type checking in model atm", SkipAll);
    auto invalidModel(createModel("ItemSUX"));
    QVERIFY(!invalidModel.get());

    auto invalidModel2(createModel("InvalidNode"));
    QVERIFY(!invalidModel2.get());
}

void tst_TestCore::testModelCreateSubNode()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    QList<TestView::MethodCall> expectedCalls;
    expectedCalls << TestView::MethodCall("modelAttached",
                                          QStringList() << QString::number(
                                              reinterpret_cast<qint64>(model.get())));
    QCOMPARE(view->methodCalls(), expectedCalls);

    QVERIFY(view->rootModelNode().isValid());
    ModelNode childNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(childNode.isValid());
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode));
    QVERIFY(childNode.parentProperty().parentModelNode() == view->rootModelNode());
    QCOMPARE(childNode.simplifiedTypeName(), QmlDesigner::TypeName("Rectangle"));

    expectedCalls << TestView::MethodCall("nodeCreated", QStringList() << "");
    expectedCalls << TestView::MethodCall("nodeAboutToBeReparented", QStringList() << "" << "data" << "" << "PropertiesAdded");
    expectedCalls << TestView::MethodCall("nodeReparented", QStringList() << "" << "data" << "" << "PropertiesAdded");
    QCOMPARE(view->methodCalls(), expectedCalls);

    QVERIFY(childNode.id().isEmpty());
    childNode.setIdWithoutRefactoring("blah");
    QCOMPARE(childNode.id(), QString("blah"));

    expectedCalls << TestView::MethodCall("nodeIdChanged", QStringList() << "blah" << "blah" << "");
    QCOMPARE(view->methodCalls(), expectedCalls);

    QCOMPARE(childNode.id(), QString("blah"));
    QCOMPARE(view->methodCalls(), expectedCalls);

    childNode.setIdWithoutRefactoring(QString());
    QVERIFY(childNode.id().isEmpty());
}

void tst_TestCore::testTypicalRewriterOperations()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode = view->rootModelNode();
    QCOMPARE(rootModelNode.directSubModelNodes().count(), 0);

    QVERIFY(rootModelNode.property("x").isValid());
    QVERIFY(!rootModelNode.property("x").isVariantProperty());
    QVERIFY(!rootModelNode.property("x").isBindingProperty());

    QVERIFY(rootModelNode.variantProperty("x").isValid());
    QVERIFY(!rootModelNode.hasProperty("x"));

    rootModelNode.variantProperty("x").setValue(70);

    QVERIFY(rootModelNode.hasProperty("x"));
    QVERIFY(rootModelNode.property("x").isVariantProperty());
    QCOMPARE(rootModelNode.variantProperty("x").value(), QVariant(70));

    rootModelNode.bindingProperty("x").setExpression("parent.x");
    QVERIFY(!rootModelNode.property("x").isVariantProperty());
    QVERIFY(rootModelNode.property("x").isBindingProperty());

    QCOMPARE(rootModelNode.bindingProperty("x").expression(), QString("parent.x"));

    ModelNode childNode(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2 ,0, "data"));
    rootModelNode.nodeListProperty("data").reparentHere(childNode);
    QCOMPARE(childNode.parentProperty(), rootModelNode.nodeAbstractProperty("data"));
    QVERIFY(rootModelNode.property("data").isNodeAbstractProperty());
    QVERIFY(rootModelNode.property("data").isNodeListProperty());
    QVERIFY(!rootModelNode.property("data").isBindingProperty());
    QVERIFY(childNode.parentProperty().isNodeListProperty());
    QCOMPARE(childNode, childNode.parentProperty().toNodeListProperty().toModelNodeList().first());
    QCOMPARE(rootModelNode, childNode.parentProperty().parentModelNode());
    QCOMPARE(childNode.parentProperty().name(), QmlDesigner::PropertyName("data"));

    QVERIFY(!rootModelNode.property("x").isVariantProperty());
    rootModelNode.variantProperty("x").setValue(90);
    QVERIFY(rootModelNode.property("x").isVariantProperty());
    QCOMPARE(rootModelNode.variantProperty("x").value(), QVariant(90));
}

void tst_TestCore::testBasicStates()
{
    char qmlString[] = "import QtQuick 2.1\n"
                       "Rectangle {\n"
                       "id: root;\n"
                            "Rectangle {\n"
                                "id: rect1;\n"
                            "}\n"
                            "Rectangle {\n"
                                "id: rect2;\n"
                            "}\n"
                            "states: [\n"
                                "State {\n"
                                    "name: \"state1\"\n"
                                    "PropertyChanges {\n"
                                        "target: rect1\n"
                                    "}\n"
                                    "PropertyChanges {\n"
                                        "target: rect2\n"
                                    "}\n"
                                "}\n"
                                ","
                                "State {\n"
                                    "name: \"state2\"\n"
                                    "PropertyChanges {\n"
                                        "target: rect1\n"
                                    "}\n"
                                    "PropertyChanges {\n"
                                        "target: rect2;\n"
                                        "x: 10;\n"
                                    "}\n"
                                "}\n"
                            "]\n"
                       "}\n";

    Exception::setShouldAssert(true);

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QVERIFY(rootModelNode.hasProperty("data"));

    QVERIFY(rootModelNode.property("data").isNodeListProperty());

    QCOMPARE(rootModelNode.nodeListProperty("data").toModelNodeList().count(), 2);

    ModelNode rect1 = rootModelNode.nodeListProperty("data").toModelNodeList().first();
    ModelNode rect2 = rootModelNode.nodeListProperty("data").toModelNodeList().last();

    QVERIFY(QmlItemNode(rect1).isValid());
    QVERIFY(QmlItemNode(rect2).isValid());

    QVERIFY(QmlItemNode(rootModelNode).isValid());

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().first(), QString("state1"));
    QCOMPARE(QmlItemNode(rootModelNode).states().names().last(), QString("state2"));

    //
    // QmlModelState API tests
    //
    QmlModelState state1 = QmlItemNode(rootModelNode).states().state("state1");
    QmlModelState state2 = QmlItemNode(rootModelNode).states().state("state2");

    QVERIFY(state1.isValid());
    QVERIFY(state2.isValid());

    QCOMPARE(state1.propertyChanges().count(), 2);
    QCOMPARE(state2.propertyChanges().count(), 2);

    QVERIFY(state1.propertyChanges().first().modelNode().metaInfo().isQtQuickStateOperation());
    QVERIFY(!state1.hasPropertyChanges(rootModelNode));

    QVERIFY(state1.propertyChanges(rect1).isValid());
    QVERIFY(state1.propertyChanges(rect2).isValid());

    state1.propertyChanges(rect2).modelNode().hasProperty("x");

    /*TODO
    QCOMPARE(QmlItemNode(rect1).allAffectingStates().count(), 2);
    QCOMPARE(QmlItemNode(rect2).allAffectingStates().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).allAffectingStates().count(), 0);
    QCOMPARE(QmlItemNode(rect1).allAffectingStatesOperations().count(), 2);
    QCOMPARE(QmlItemNode(rect2).allAffectingStatesOperations().count(), 2);
    */
}

void tst_TestCore::testBasicStatesQtQuick20()
{
    char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                       "id: root;\n"
                            "Rectangle {\n"
                                "id: rect1;\n"
                            "}\n"
                            "Rectangle {\n"
                                "id: rect2;\n"
                            "}\n"
                            "states: [\n"
                                "State {\n"
                                    "name: \"state1\"\n"
                                    "PropertyChanges {\n"
                                        "target: rect1\n"
                                    "}\n"
                                    "PropertyChanges {\n"
                                        "target: rect2\n"
                                    "}\n"
                                "}\n"
                                ","
                                "State {\n"
                                    "name: \"state2\"\n"
                                    "PropertyChanges {\n"
                                        "target: rect1\n"
                                    "}\n"
                                    "PropertyChanges {\n"
                                        "target: rect2;\n"
                                        "x: 10;\n"
                                    "}\n"
                                "}\n"
                            "]\n"
                       "}\n";

    Exception::setShouldAssert(true);

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rootModelNode.majorVersion(), 2);
    //QCOMPARE(rootModelNode.majorQtQuickVersion(), 2);

#ifndef QDS_USE_PROJECTSTORAGE
    qDebug() << rootModelNode.nodeListProperty("states").toModelNodeList().first().metaInfo().majorVersion();
    qDebug() << rootModelNode.nodeListProperty("states").toModelNodeList().first().metaInfo().typeName();
#endif

    QSKIP("No QML Puppet");

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    QVERIFY(rootModelNode.hasProperty("data"));

    QVERIFY(rootModelNode.property("data").isNodeListProperty());

    QCOMPARE(rootModelNode.nodeListProperty("data").toModelNodeList().count(), 2);

    ModelNode rect1 = rootModelNode.nodeListProperty("data").toModelNodeList().first();
    ModelNode rect2 = rootModelNode.nodeListProperty("data").toModelNodeList().last();

    QVERIFY(QmlItemNode(rect1).isValid());
    QVERIFY(QmlItemNode(rect2).isValid());

    QVERIFY(QmlItemNode(rootModelNode).isValid());

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).states().names().first(), QString("state1"));
    QCOMPARE(QmlItemNode(rootModelNode).states().names().last(), QString("state2"));

    //
    // QmlModelState API tests
    //
    QmlModelState state1 = QmlItemNode(rootModelNode).states().state("state1");
    QmlModelState state2 = QmlItemNode(rootModelNode).states().state("state2");

    QVERIFY(state1.isValid());
    QVERIFY(state2.isValid());

    QCOMPARE(state1.propertyChanges().count(), 2);
    QCOMPARE(state2.propertyChanges().count(), 2);

    QVERIFY(!state1.hasPropertyChanges(rootModelNode));

    QVERIFY(state1.propertyChanges(rect1).isValid());
    QVERIFY(state1.propertyChanges(rect2).isValid());

    state1.propertyChanges(rect2).modelNode().hasProperty("x");

    QCOMPARE(QmlItemNode(rect1).allAffectingStates().count(), 2);
    QCOMPARE(QmlItemNode(rect2).allAffectingStates().count(), 2);
    QCOMPARE(QmlItemNode(rootModelNode).allAffectingStates().count(), 0);
    QCOMPARE(QmlItemNode(rect1).allAffectingStatesOperations().count(), 2);
    QCOMPARE(QmlItemNode(rect2).allAffectingStatesOperations().count(), 2);
}

void tst_TestCore::testModelBasicOperations()
{
    auto model(createModel("QtQuick.Flipable", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode = view->rootModelNode();
    QCOMPARE(rootModelNode.directSubModelNodes().count(), 0);

    rootModelNode.variantProperty("width").setValue(10);
    rootModelNode.variantProperty("height").setValue(10);

    QCOMPARE(rootModelNode.variantProperty("width").value().toInt(), 10);
    QCOMPARE(rootModelNode.variantProperty("height").value().toInt(), 10);

    QVERIFY(rootModelNode.hasProperty("width"));
    rootModelNode.removeProperty("width");
    QVERIFY(!rootModelNode.hasProperty("width"));

    QVERIFY(!rootModelNode.hasProperty("children"));
    ModelNode childNode1(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "children"));
    ModelNode childNode2(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));

    QVERIFY(childNode1.isValid());
    QVERIFY(childNode2.isValid());

    QVERIFY(childNode1.parentProperty().parentModelNode().isValid());
    QVERIFY(childNode2.parentProperty().parentModelNode().isValid());

    QVERIFY(rootModelNode.hasProperty("children"));
    childNode2.setParentProperty(rootModelNode, "children");
    QVERIFY(!rootModelNode.hasProperty("data"));
    QCOMPARE(childNode2.parentProperty().parentModelNode(), rootModelNode);
    QVERIFY(rootModelNode.hasProperty("children"));
    childNode2.setParentProperty(rootModelNode, "data");
    childNode1.setParentProperty(rootModelNode, "data");
    QCOMPARE(childNode2.parentProperty().parentModelNode(), rootModelNode);
    QVERIFY(!rootModelNode.hasProperty("children"));
    QVERIFY(rootModelNode.hasProperty("data"));

    QVERIFY(childNode2.isValid());
    QVERIFY(childNode2.parentProperty().parentModelNode().isValid());
    QCOMPARE(childNode2.parentProperty().parentModelNode(), rootModelNode);

    childNode1.setParentProperty(rootModelNode, "children");
    rootModelNode.removeProperty("data");
    QVERIFY(!childNode2.isValid());
    QVERIFY(!rootModelNode.hasProperty("data"));

    QVERIFY(childNode1.isValid());
    rootModelNode.nodeProperty("front").setModelNode(childNode1);
    QVERIFY(rootModelNode.hasProperty("front"));
    QCOMPARE(childNode1.parentProperty().parentModelNode(), rootModelNode);
    rootModelNode.removeProperty("front");
    QVERIFY(!childNode1.isValid());
}

void tst_TestCore::testModelResolveIds()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootNode = view->rootModelNode();
    rootNode.setIdWithoutRefactoring("rootNode");

    ModelNode childNode1(addNodeListChild(rootNode, "QtQuick.Rectangle", 2, 0, "children"));

    ModelNode childNode2(addNodeListChild(childNode1, "QtQuick.Flipable", 2, 0, "children"));
    childNode2.setIdWithoutRefactoring("childNode2");
    childNode2.bindingProperty("anchors.fill").setExpression("parent.parent");

    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), rootNode);
    childNode1.setIdWithoutRefactoring("childNode1");
    childNode2.bindingProperty("anchors.fill").setExpression("childNode1.parent");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), rootNode);
    childNode2.bindingProperty("anchors.fill").setExpression("rootNode");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), rootNode);

    ModelNode childNode3(addNodeListChild(childNode2, "QtQuick.Rectangle", 2, 0, "children"));
    childNode3.setIdWithoutRefactoring("childNode3");
    childNode2.nodeProperty("front").setModelNode(childNode3);
    childNode2.bindingProperty("anchors.fill").setExpression("childNode3.parent");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), childNode2);
    childNode2.bindingProperty("anchors.fill").setExpression("childNode3.parent.parent");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), childNode1);
    childNode2.bindingProperty("anchors.fill").setExpression("childNode3.parent.parent.parent");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), rootNode);
    childNode2.bindingProperty("anchors.fill").setExpression("childNode3");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), childNode3);
    childNode2.bindingProperty("anchors.fill").setExpression("front");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), childNode3);
    childNode2.bindingProperty("anchors.fill").setExpression("back");
    QVERIFY(!childNode2.bindingProperty("anchors.fill").resolveToModelNode().isValid());
    childNode2.bindingProperty("anchors.fill").setExpression("childNode3.parent.front");
    QCOMPARE(childNode2.bindingProperty("anchors.fill").resolveToModelNode(), childNode3);

    childNode2.variantProperty("x").setValue(10);
    QCOMPARE(childNode2.variantProperty("x").value().toInt(), 10);

    childNode2.bindingProperty("width").setExpression("childNode3.parent.x");
    QVERIFY(childNode2.bindingProperty("width").resolveToProperty().isVariantProperty());
    QCOMPARE(childNode2.bindingProperty("width").resolveToProperty().toVariantProperty().value().toInt(), 10);

    childNode2.bindingProperty("width").setExpression("childNode3.parent.width");
    QVERIFY(childNode2.bindingProperty("width").resolveToProperty().isBindingProperty());
    QCOMPARE(childNode2.bindingProperty("width").resolveToProperty().toBindingProperty().expression(), QString("childNode3.parent.width"));
}

void tst_TestCore::testModelNodeListProperty()
{
    //
    // Test NodeListProperty API
    //
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootNode = view->rootModelNode();

    //
    // Item {}
    //
    NodeListProperty rootChildren = rootNode.nodeListProperty("children");
    QVERIFY(!rootNode.hasProperty("children"));
    QVERIFY(rootChildren.isValid());
    QVERIFY(!rootChildren.isNodeListProperty());
    QVERIFY(rootChildren.isEmpty());

    ModelNode rectNode = view->createModelNode("QtQuick.Rectangle", 2, 0);
    rootChildren.reparentHere(rectNode);

    //
    // Item { children: [ Rectangle {} ] }
    //
    QVERIFY(rootNode.hasProperty("children"));
    QVERIFY(rootChildren.isValid());
    QVERIFY(rootChildren.isNodeListProperty());
    QVERIFY(!rootChildren.isEmpty());

    ModelNode mouseAreaNode = view->createModelNode("QtQuick.Item", 2, 0);
    NodeListProperty rectChildren = rectNode.nodeListProperty("children");
    rectChildren.reparentHere(mouseAreaNode);

    //
    // Item { children: [ Rectangle { children : [ MouseArea {} ] } ] }
    //
    QVERIFY(rectNode.hasProperty("children"));
    QVERIFY(rectChildren.isValid());
    QVERIFY(rectChildren.isNodeListProperty());
    QVERIFY(!rectChildren.isEmpty());

    rectNode.destroy();

    //
    // Item { }
    //
    QVERIFY(!rectNode.isValid());
    QVERIFY(!mouseAreaNode.isValid());
    QVERIFY(!rootNode.hasProperty("children"));
    QVERIFY(rootChildren.isValid());
    QVERIFY(!rootChildren.isNodeListProperty());
    QVERIFY(rootChildren.isEmpty());
    QVERIFY(!rectChildren.isValid());
}

void tst_TestCore::testModelNodePropertyDynamic()
{
    //
    // Test NodeListProperty API
    //
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    model->rewriterView()->setCheckSemanticErrors(false); //This test needs this

    ModelNode rootNode = view->rootModelNode();

    //
    // Item {}
    //
    NodeProperty nodeProperty = rootNode.nodeProperty("gradient1");
    QVERIFY(!rootNode.hasProperty("gradient1"));
    QVERIFY(nodeProperty.isValid());
    QVERIFY(!nodeProperty.isNodeProperty());
    QVERIFY(nodeProperty.isEmpty());

    ModelNode rectNode = view->createModelNode("QtQuick.Rectangle", 2, 0);
    nodeProperty.reparentHere(rectNode);

    QVERIFY(rootNode.hasProperty("gradient1"));
    QVERIFY(nodeProperty.isValid());
    QVERIFY(nodeProperty.isNodeProperty());
    QVERIFY(!nodeProperty.isEmpty());
    QVERIFY(!nodeProperty.isDynamic());


    NodeProperty dynamicNodeProperty = rootNode.nodeProperty("gradient2");
    QVERIFY(!rootNode.hasProperty("gradient2"));
    QVERIFY(dynamicNodeProperty.isValid());
    QVERIFY(!dynamicNodeProperty.isNodeProperty());
    QVERIFY(dynamicNodeProperty.isEmpty());

    ModelNode rectNode2 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    dynamicNodeProperty.setDynamicTypeNameAndsetModelNode("Gradient", rectNode2);

    QVERIFY(rootNode.hasProperty("gradient2"));
    QVERIFY(dynamicNodeProperty.isValid());
    QVERIFY(dynamicNodeProperty.isNodeProperty());
    QVERIFY(!dynamicNodeProperty.isEmpty());
    QVERIFY(dynamicNodeProperty.isDynamic());


    rectNode2.setIdWithRefactoring("test");
    QCOMPARE(rectNode2.id(), QString("test"));

    rectNode2.variantProperty("x").setValue(10);

    QCOMPARE(rectNode2.variantProperty("x").value(), QVariant(10));

    rootNode.removeProperty("gradient2");
    QVERIFY(!rootNode.hasProperty("gradient2"));

    QVERIFY(dynamicNodeProperty.isValid());
    QVERIFY(!rootNode.hasProperty("gradient2"));

    QVERIFY(!dynamicNodeProperty.isNodeProperty());
}

static QString dynmaicNodePropertySource =  "import QtQuick 2.1\n"
                                            "Rectangle {\n"
                                            "    property Gradient gradient1: Gradient {\n"
                                            "        id: pGradient\n"
                                            "    }\n"
                                            "    gradient: Gradient {\n"
                                            "        id: secondGradient\n"
                                            "        GradientStop { id: nOne; position: 0.0; color: \"blue\" }\n"
                                            "        GradientStop { id: nTwo; position: 1.0; color: \"lightsteelblue\" }\n"
                                            "    }\n"
                                            "}";


void tst_TestCore::testModelNodePropertyDynamicSource()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText(dynmaicNodePropertySource);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QVERIFY(model.get());
    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.directSubModelNodes().size(), 2);

    QVERIFY(rootModelNode.hasProperty("gradient"));
    QVERIFY(rootModelNode.hasProperty("gradient1"));

    QVERIFY(rootModelNode.property("gradient").isNodeProperty());
    QVERIFY(rootModelNode.property("gradient1").isNodeProperty());
    QVERIFY(rootModelNode.property("gradient1").isDynamic());

    rootModelNode.removeProperty("gradient1");
    QVERIFY(!rootModelNode.hasProperty("gradient1"));
}

void tst_TestCore::testBasicOperationsWithView()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    //NodeInstanceView *nodeInstanceView = new NodeInstanceView(model.get(), NodeInstanceServerInterface::TestModus);
    //model->attachView(nodeInstanceView);

    ModelNode rootModelNode = view->rootModelNode();
    QCOMPARE(rootModelNode.directSubModelNodes().count(), 0);
    //NodeInstance rootInstance = nodeInstanceView->instanceForModelNode(rootModelNode);

    //QVERIFY(rootInstance.isValid());

    QVERIFY(rootModelNode.isValid());

    rootModelNode.variantProperty("width").setValue(10);
    rootModelNode.variantProperty("height").setValue(10);

    QCOMPARE(rootModelNode.variantProperty("width").value().toInt(), 10);
    QCOMPARE(rootModelNode.variantProperty("height").value().toInt(), 10);

    //QCOMPARE(rootInstance.size().width(), 10.0);
    //QCOMPARE(rootInstance.size().height(), 10.0);

    ModelNode childNode(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));
    ModelNode childNode2(addNodeListChild(childNode, "QtQuick.Rectangle", 2, 0, "data"));
    QVERIFY(childNode2.parentProperty().parentModelNode() == childNode);

    QVERIFY(childNode.isValid());


    {
        /*
        NodeInstance childInstance2 = nodeInstanceView->instanceForModelNode(childNode2);
        NodeInstance childInstance = nodeInstanceView->instanceForModelNode(childNode);
        */

        /*
        QVERIFY(childInstance.isValid());
//        QVERIFY(qobject_cast<QGraphicsObject*>(childInstance2.testHandle())->parentItem()->toGraphicsObject() == childInstance.testHandle());
//        QVERIFY(qobject_cast<QGraphicsObject*>(childInstance.testHandle())->parentItem()->toGraphicsObject() == rootInstance.testHandle());
        QCOMPARE(childInstance.size().width(), 0.0);
        QCOMPARE(childInstance.size().height(), 0.0);
        */


        childNode.variantProperty("width").setValue(100);
        childNode.variantProperty("height").setValue(100);

        QCOMPARE(childNode.variantProperty("width").value().toInt(), 100);
        QCOMPARE(childNode.variantProperty("height").value().toInt(), 100);

        //QCOMPARE(childInstance.size().width(), 100.0);
        //QCOMPARE(childInstance.size().height(), 100.0);

        childNode.destroy();
        QVERIFY(!childNode.isValid());
        QVERIFY(!childNode2.isValid());
        /*
        QVERIFY(childInstance.instanceId() == -1);
        QVERIFY(childInstance2.instanceId() == -1);
        QVERIFY(!childInstance.isValid());
        QVERIFY(!childInstance2.isValid());
        */
    }

    childNode = addNodeListChild(rootModelNode, "QtQuick.Image", 2, 0, "data");
    QVERIFY(childNode.isValid());
    QCOMPARE(childNode.type(), QmlDesigner::TypeName("QtQuick.Image"));
    childNode2 = addNodeListChild(childNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(childNode2.isValid());
    childNode2.setParentProperty(rootModelNode, "data");
    QVERIFY(childNode2.isValid());

    {
        /*
        NodeInstance childInstance2 = nodeInstanceView->instanceForModelNode(childNode2);
        NodeInstance childInstance = nodeInstanceView->instanceForModelNode(childNode);


        QVERIFY(childInstance.isValid());
//        QVERIFY(qobject_cast<QGraphicsObject*>(childInstance2.testHandle())->parentItem()->toGraphicsObject() == rootInstance.testHandle());
//        QVERIFY(qobject_cast<QGraphicsObject*>(childInstance.testHandle())->parentItem()->toGraphicsObject() == rootInstance.testHandle());
        QCOMPARE(childInstance.size().width(), 0.0);
        QCOMPARE(childInstance.size().height(), 0.0);
        */

        QCOMPARE(rootModelNode, childNode2.parentProperty().parentModelNode());

        childNode.variantProperty("width").setValue(20);
        //QCOMPARE(childInstance.property("width").toInt(), 20);

        QCOMPARE(childNode.variantProperty("width").value().toInt(), 20);
        childNode.removeProperty("width");
        QVERIFY(!childNode.hasProperty("width"));

        rootModelNode.removeProperty("data");
        QVERIFY(!childNode.isValid());
        QVERIFY(!childNode2.isValid());
        /*
        QVERIFY(childInstance.instanceId() == -1);
        QVERIFY(childInstance2.instanceId() == -1);
        QVERIFY(!childInstance.isValid());
        QVERIFY(!childInstance2.isValid());
        */
    }

    //model->detachView(nodeInstanceView);
}

void tst_TestCore::testQmlModelView()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    auto view = std::make_unique<TestView>();
    QVERIFY(view);
    model->attachView(view.get());
    QVERIFY(view->model());
    QVERIFY(view->rootQmlObjectNode().isValid());

    QVERIFY(!view->rootQmlObjectNode().hasNodeParent());


    QVERIFY(view->rootQmlObjectNode().isRootModelNode());


    PropertyListType propertyList;
    propertyList.append(qMakePair(QmlDesigner::PropertyName("x"), QVariant(20)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("y"), QVariant(20)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("width"), QVariant(20)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("height"), QVariant(20)));

    QmlObjectNode node1 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);

    QVERIFY(node1.isValid());
    QVERIFY(!node1.hasNodeParent());

    node1.setParentProperty(view->rootQmlObjectNode().nodeAbstractProperty("children"));

    QVERIFY(node1.hasNodeParent());

    QmlObjectNode node2 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);

    QVERIFY(node2.isValid());
    QVERIFY(!node2.hasNodeParent());

    node2.setParentProperty(view->rootQmlObjectNode().nodeAbstractProperty("children"));

    QVERIFY(node2.hasNodeParent());

    node2.setParentProperty(node1.nodeAbstractProperty("children"));

    QVERIFY(node2.hasNodeParent());
    //QVERIFY(node2.hasInstanceParent());
    //QVERIFY(node2.instanceParent() == node1);

        node2.setParentProperty(view->rootQmlObjectNode().nodeAbstractProperty("children"));

    //QCOMPARE(node1.instanceValue("x").toInt(), 20);

    node1.setVariantProperty("x", 2);

    //QCOMPARE(node1.instanceValue("x").toInt(), 2);


    QmlObjectNode node3 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    QmlObjectNode node4 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    QmlObjectNode node5 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    QmlObjectNode node6 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    QmlObjectNode node7 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    QmlObjectNode node8 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);

    node3.setParentProperty(node2.nodeAbstractProperty("children"));
    node4.setParentProperty(node3.nodeAbstractProperty("children"));
    node5.setParentProperty(node4.nodeAbstractProperty("children"));
    node6.setParentProperty(node5.nodeAbstractProperty("children"));
    node7.setParentProperty(node6.nodeAbstractProperty("children"));
    node8.setParentProperty(node7.nodeAbstractProperty("children"));

    QCOMPARE(node2.nodeAbstractProperty("children").allSubNodes().count(), 6);
    QVERIFY(node8.isValid());
    node3.destroy();
    QVERIFY(!node8.isValid());
    QCOMPARE(node2.nodeAbstractProperty("children").allSubNodes().count(), 0);

    node1.setId("node1");

    node2.setVariantProperty("x", 20);
    node2.setBindingProperty("x", "node1.x");
    //QCOMPARE(node2.instanceValue("x").toInt(), 2);
    node1.setVariantProperty("x", 10);
    //QCOMPARE(node2.instanceValue("x").toInt(), 10);

    node1.destroy();
    QVERIFY(!node1.isValid());

    //QCOMPARE(node2.instanceValue("x").toInt(), 10); // is this right? or should it be a invalid qvariant?

    node1 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    node1.setId("node1");

    //QCOMPARE(node2.instanceValue("x").toInt(), 20);

    node3 = view->createQmlObjectNode("QtQuick.Rectangle", 2, 0, propertyList);
    node3.setParentProperty(node2.nodeAbstractProperty("children"));
    //QCOMPARE(node3.instanceValue("width").toInt(), 20);
    node3.setVariantProperty("width", 0);
    //QCOMPARE(node3.instanceValue("width").toInt(), 0);

    //QCOMPARE(node3.instanceValue("x").toInt(), 20);
    //QVERIFY(!QDeclarativeMetaType::toQObject(node3.instanceValue("anchors.fill")));
    node3.setBindingProperty("anchors.fill", "parent");
    //QCOMPARE(node3.instanceValue("x").toInt(), 0);
    //QCOMPARE(node3.instanceValue("width").toInt(), 20);

    node3.setParentProperty(node1.nodeAbstractProperty("children"));
    node1.setVariantProperty("width", 50);
    node2.setVariantProperty("width", 100);
    //QCOMPARE(node3.instanceValue("width").toInt(), 50);
}

void tst_TestCore::testModelRemoveNode()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    TestConnectionManager connectionManager;
    ExternalDependenciesFake externalDependenciesFake{model.get()};
    NodeInstanceView nodeInstanceView{connectionManager, externalDependenciesFake};
    model->attachView(&nodeInstanceView);

    QCOMPARE(view->rootModelNode().directSubModelNodes().count(), 0);


    ModelNode childNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(childNode.isValid());
    QCOMPARE(view->rootModelNode().directSubModelNodes().count(), 1);
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode));
    QVERIFY(childNode.parentProperty().parentModelNode() == view->rootModelNode());

    {
        //#### NodeInstance childInstance = nodeInstanceView->instanceForNode(childNode);
        //#### QVERIFY(childInstance.isValid());
        //#### QVERIFY(childInstance.parentId() == view->rootModelNode().internalId());
    }

    ModelNode subChildNode = addNodeListChild(childNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(subChildNode.isValid());
    QCOMPARE(childNode.directSubModelNodes().count(), 1);
    QVERIFY(childNode.directSubModelNodes().contains(subChildNode));
    QVERIFY(subChildNode.parentProperty().parentModelNode() == childNode);

    {
        //#### NodeInstance subChildInstance = nodeInstanceView->instanceForNode(subChildNode);
        //#### QVERIFY(subChildInstance.isValid());
        //#### QVERIFY(subChildInstance.parentId() == childNode.internalId());
    }

    childNode.destroy();

    //#### QVERIFY(!nodeInstanceView->hasInstanceForNode(childNode));

    QCOMPARE(view->rootModelNode().directSubModelNodes().count(), 0);
    QVERIFY(!view->rootModelNode().directSubModelNodes().contains(childNode));
    QVERIFY(!childNode.isValid());
    QVERIFY(!subChildNode.isValid());

    view->rootModelNode().destroy();
    QVERIFY(view->rootModelNode().isValid());

    QVERIFY(view->rootModelNode().isValid());

    // delete node not in hierarchy
    childNode = view->createModelNode("QtQuick.Item", 1, 1);
    childNode.destroy();

    model->detachView(&nodeInstanceView);
}

void tst_TestCore::reparentingNode()
{
    auto model(createModel("QtQuick.Item", 2, 0));

    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode = view->rootModelNode();
    QVERIFY(rootModelNode.isValid());
    rootModelNode.setIdWithoutRefactoring("rootModelNode");
    QCOMPARE(rootModelNode.id(), QString("rootModelNode"));

    //NodeInstanceView *nodeInstanceView = new NodeInstanceView(model.get(), NodeInstanceServerInterface::TestModus);
    //model->attachView(nodeInstanceView);

    ModelNode childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    QCOMPARE(childNode.parentProperty().parentModelNode(), rootModelNode);
    QVERIFY(rootModelNode.directSubModelNodes().contains(childNode));

    {
        //NodeInstance childInstance = nodeInstanceView->instanceForModelNode(childNode);
        //QVERIFY(childInstance.isValid());
        //QVERIFY(childInstance.parentId() == view->rootModelNode().internalId());
    }

    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.Item", 2, 0, "data");
    QCOMPARE(childNode2.parentProperty().parentModelNode(), rootModelNode);
    QVERIFY(rootModelNode.directSubModelNodes().contains(childNode2));

    {
        //NodeInstance childIstance2 = nodeInstanceView->instanceForModelNode(childNode2);
        //QVERIFY(childIstance2.isValid());
        //QVERIFY(childIstance2.parentId() == view->rootModelNode().internalId());
    }

    childNode.setParentProperty(childNode2, "data");

    QCOMPARE(childNode.parentProperty().parentModelNode(), childNode2);
    QVERIFY(childNode2.directSubModelNodes().contains(childNode));
    QVERIFY(!rootModelNode.directSubModelNodes().contains(childNode));
    QVERIFY(rootModelNode.directSubModelNodes().contains(childNode2));

    {
        //NodeInstance childIstance = nodeInstanceView->instanceForModelNode(childNode);
        //QVERIFY(childIstance.isValid());
        //QVERIFY(childIstance.parentId() == childNode2.internalId());
    }

    childNode2.setParentProperty(rootModelNode, "data");
    QCOMPARE(childNode2.parentProperty().parentModelNode(), rootModelNode);
    QVERIFY(rootModelNode.directSubModelNodes().contains(childNode2));

    {
        //NodeInstance childIstance2 = nodeInstanceView->instanceForModelNode(childNode2);
        //QVERIFY(childIstance2.isValid());
        //QVERIFY(childIstance2.parentId() == rootModelNode.internalId());
    }

    QCOMPARE(childNode.parentProperty().parentModelNode(), childNode2);

    QApplication::processEvents();
    //model->detachView(nodeInstanceView);
}

void tst_TestCore::reparentingNodeLikeDragAndDrop()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText("import QtQuick 2.0;\n\nItem {\n}\n");
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    //NodeInstanceView *nodeInstanceView = new NodeInstanceView(model.get(), NodeInstanceServerInterface::TestModus);
    //model->attachView(nodeInstanceView);
    view->rootModelNode().setIdWithoutRefactoring("rootModelNode");
    QCOMPARE(view->rootModelNode().id(), QString("rootModelNode"));

    ModelNode rectNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
    rectNode.setIdWithoutRefactoring("rect_1");
    rectNode.variantProperty("x").setValue(20);
    rectNode.variantProperty("y").setValue(30);
    rectNode.variantProperty("width").setValue(40);
    rectNode.variantProperty("height").setValue(50);

    RewriterTransaction transaction(view->beginRewriterTransaction("TEST"));

    ModelNode textNode = addNodeListChild(view->rootModelNode(), "QtQuick.Text", 2, 0, "data");
    QCOMPARE(textNode.parentProperty().parentModelNode(), view->rootModelNode());
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(textNode));

    textNode.setIdWithoutRefactoring("rext_1");
    textNode.variantProperty("x").setValue(10);
    textNode.variantProperty("y").setValue(10);
    textNode.variantProperty("width").setValue(50);
    textNode.variantProperty("height").setValue(20);

    textNode.variantProperty("x").setValue(30);
    textNode.variantProperty("y").setValue(30);

    {
        /*
        NodeInstance textInstance = nodeInstanceView->instanceForModelNode(textNode);
        QVERIFY(textInstance.isValid());
        QVERIFY(textInstance.parentId() == view->rootModelNode().internalId());
        QCOMPARE(textInstance.position().x(), 30.0);
        QCOMPARE(textInstance.position().y(), 30.0);
        QCOMPARE(textInstance.size().width(), 50.0);
        QCOMPARE(textInstance.size().height(), 20.0);
        */
    }

    textNode.setParentProperty(rectNode, "data");
    QCOMPARE(textNode.parentProperty().parentModelNode(), rectNode);
    QVERIFY(rectNode.directSubModelNodes().contains(textNode));

    {
        /*
        NodeInstance textInstance = nodeInstanceView->instanceForModelNode(textNode);
        QVERIFY(textInstance.isValid());
        QVERIFY(textInstance.parentId() == rectNode.internalId());
        QCOMPARE(textInstance.position().x(), 30.0);
        QCOMPARE(textInstance.position().y(), 30.0);
        QCOMPARE(textInstance.size().width(), 50.0);
        QCOMPARE(textInstance.size().height(), 20.0);
        */
    }

    textNode.setParentProperty(view->rootModelNode(), "data");
    QCOMPARE(textNode.parentProperty().parentModelNode(), view->rootModelNode());
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(textNode));

    {
        // #####
        /*NodeInstance textInstance = nodeInstanceView->instanceForNode(textNode);
        QVERIFY(textInstance.isValid());
        QVERIFY(textInstance.parentId() == view->rootModelNode().internalId());
        QCOMPARE(textInstance.position().x(), 30.0);
        QCOMPARE(textInstance.position().y(), 30.0);
        QCOMPARE(textInstance.size().width(), 50.0);
        QCOMPARE(textInstance.size().height(), 20.0);*/
    }

    textNode.setParentProperty(rectNode, "data");
    QCOMPARE(textNode.parentProperty().parentModelNode(), rectNode);
    QVERIFY(rectNode.directSubModelNodes().contains(textNode));

    {
        // #####
        /*NodeInstance textInstance = nodeInstanceView->instanceForNode(textNode);
        QVERIFY(textInstance.isValid());
        QVERIFY(textInstance.parentId() == rectNode.internalId());
        QCOMPARE(textInstance.position().x(), 30.0);
        QCOMPARE(textInstance.position().y(), 30.0);
        QCOMPARE(textInstance.size().width(), 50.0);
        QCOMPARE(textInstance.size().height(), 20.0);*/
    }

    {
        // #####
        /*NodeInstance textInstance = nodeInstanceView->instanceForNode(textNode);
        QVERIFY(textInstance.isValid());
        QVERIFY(textInstance.parentId() == rectNode.internalId());
        QCOMPARE(textInstance.position().x(), 30.0);
        QCOMPARE(textInstance.position().y(), 30.0);
        QCOMPARE(textInstance.size().width(), 50.0);
        QCOMPARE(textInstance.size().height(), 20.0);*/
    }

    transaction.commit();

    if(!textNode.isValid())
        QFAIL("textnode node is not valid anymore");
    else
        QCOMPARE(textNode.parentProperty().parentModelNode(), rectNode);
    QVERIFY(rectNode.directSubModelNodes().contains(textNode));

    QApplication::processEvents();

    //model->detachView(nodeInstanceView);
}

void tst_TestCore::testModelReorderSiblings()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    //NodeInstanceView *nodeInstanceView = new NodeInstanceView(model.get(), NodeInstanceServerInterface::TestModus);
    //model->attachView(nodeInstanceView);

    ModelNode rootModelNode = view->rootModelNode();
    QVERIFY(rootModelNode.isValid());

    ModelNode a = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(a.isValid());
    ModelNode b = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(b.isValid());
    ModelNode c = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(c.isValid());

    {
        // ####
        /*QVERIFY(nodeInstanceView->instanceForNode(a).parentId() == rootModelNode.internalId());
        QVERIFY(nodeInstanceView->instanceForNode(b).parentId() == rootModelNode.internalId());
        QVERIFY(nodeInstanceView->instanceForNode(c).parentId() == rootModelNode.internalId());*/
    }

    NodeListProperty listProperty(rootModelNode.nodeListProperty("data"));

    listProperty.slide(listProperty.toModelNodeList().indexOf(a), 2); //a.slideToIndex(2);

    QVERIFY(a.isValid()); QCOMPARE(listProperty.toModelNodeList().indexOf(a), 2);
    QVERIFY(b.isValid()); QCOMPARE(listProperty.toModelNodeList().indexOf(b), 0);
    QVERIFY(c.isValid()); QCOMPARE(listProperty.toModelNodeList().indexOf(c), 1);

    listProperty.slide(listProperty.toModelNodeList().indexOf(c), 0); //c.slideToIndex(0);

    QVERIFY(a.isValid()); QCOMPARE(listProperty.toModelNodeList().indexOf(a), 2);
    QVERIFY(b.isValid()); QCOMPARE(listProperty.toModelNodeList().indexOf(b), 1);
    QVERIFY(c.isValid()); QCOMPARE(listProperty.toModelNodeList().indexOf(c), 0);

    /*
    {
        QVERIFY(nodeInstanceView->instanceForModelNode(a).parentId() == rootModelNode.internalId());
        QVERIFY(nodeInstanceView->instanceForModelNode(b).parentId() == rootModelNode.internalId());
        QVERIFY(nodeInstanceView->instanceForModelNode(c).parentId() == rootModelNode.internalId());
    }*/

    QApplication::processEvents();

    //model->detachView(nodeInstanceView);
}

void tst_TestCore::testModelRootNode()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    try {
        ModelNode rootModelNode = view->rootModelNode();
        QVERIFY(rootModelNode.isValid());
        QVERIFY(rootModelNode.isRootNode());
        ModelNode topChildNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
        QVERIFY(topChildNode.isValid());
        QVERIFY(rootModelNode.isRootNode());
        ModelNode childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
        QVERIFY(childNode.isValid());
        QVERIFY(rootModelNode.isValid());
        QVERIFY(rootModelNode.isRootNode());
        childNode.setParentProperty(topChildNode, "data");
        QVERIFY(topChildNode.isValid());
        QVERIFY(childNode.isValid());
        QVERIFY(rootModelNode.isValid());
        QVERIFY(rootModelNode.isRootNode());
    } catch (const QmlDesigner::Exception &exception) {
        const QString errorMsg = QString("Exception: %1 %2 %3:%4")
                .arg(exception.type(), exception.function(), exception.file()).arg(exception.line());
        QFAIL(errorMsg.toLatin1().constData());
    }
    QApplication::processEvents();
}

void tst_TestCore::reparentingNodeInModificationGroup()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode childNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
    ModelNode childNode2 = addNodeListChild(view->rootModelNode(), "QtQuick.Item", 2, 0, "data");
    childNode.variantProperty("x").setValue(10);
    childNode.variantProperty("y").setValue(10);

    QCOMPARE(childNode.parentProperty().parentModelNode(), view->rootModelNode());
    QCOMPARE(childNode2.parentProperty().parentModelNode(), view->rootModelNode());
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode));
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode2));

//    ModificationGroupToken token = model->beginModificationGroup();
    childNode.variantProperty("x").setValue(20);
    childNode.variantProperty("y").setValue(20);
    childNode.setParentProperty(childNode2, "data");
    childNode.variantProperty("x").setValue(30);
    childNode.variantProperty("y").setValue(30);
    childNode.variantProperty("x").setValue(1000000);
    childNode.variantProperty("y").setValue(1000000);
//    model->endModificationGroup(token);

    QCOMPARE(childNode.parentProperty().parentModelNode(), childNode2);
    QVERIFY(childNode2.directSubModelNodes().contains(childNode));
    QVERIFY(!view->rootModelNode().directSubModelNodes().contains(childNode));
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode2));

    childNode.setParentProperty(view->rootModelNode(), "data");
    QVERIFY(childNode.isValid());
    QCOMPARE(childNode2.parentProperty().parentModelNode(), view->rootModelNode());

    childNode2.setParentProperty(childNode, "data");
    QCOMPARE(childNode2.parentProperty().parentModelNode(), childNode);
    QVERIFY(childNode2.isValid());
    QVERIFY(childNode.directSubModelNodes().contains(childNode2));

    QCOMPARE(childNode2.parentProperty().parentModelNode(), childNode);
    childNode2.setParentProperty(view->rootModelNode(), "data");
    QCOMPARE(childNode2.parentProperty().parentModelNode(), view->rootModelNode());
    QVERIFY(childNode2.isValid());
    QVERIFY(view->rootModelNode().directSubModelNodes().contains(childNode2));

    QApplication::processEvents();
}

void tst_TestCore::testModelAddAndRemoveProperty()
{
    auto model(createModel("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->rewriterView()->setCheckSemanticErrors(false); //This test needs this
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode node = view->rootModelNode();
    QVERIFY(node.isValid());

    //NodeInstanceView *nodeInstanceView = new NodeInstanceView(model.get(), NodeInstanceServerInterface::TestModus);
    //model->attachView(nodeInstanceView);

    node.variantProperty("blah").setValue(-1);
    QCOMPARE(node.variantProperty("blah").value().toInt(), -1);
    QVERIFY(node.hasProperty("blah"));
    QCOMPARE(node.variantProperty("blah").value().toInt(), -1);

    node.variantProperty("customValue").setValue(42);
    QCOMPARE(node.variantProperty("customValue").value().toInt(), 42);

    node.variantProperty("x").setValue(42);
    QCOMPARE(node.variantProperty("x").value().toInt(), 42);

    /*
    {
        NodeInstance nodeInstance = nodeInstanceView->instanceForModelNode(node);
        QCOMPARE(nodeInstance.property("x").toInt(), 42);
    }*/

    node.removeProperty("customValue");
    QVERIFY(!node.hasProperty("customValue"));

    node.variantProperty("foo").setValue("bar");
    QVERIFY(node.hasProperty("foo"));
    QCOMPARE(node.variantProperty("foo").value().toString(), QString("bar"));

    QApplication::processEvents();

    //model->detachView(nodeInstanceView);
}

void tst_TestCore::testModelViewNotification()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view1(new TestView);
    QVERIFY(view1.data());

    QScopedPointer<TestView> view2(new TestView);
    QVERIFY(view2.data());

    model->attachView(view2.data());
    model->attachView(view1.data());

    QCOMPARE(view1->methodCalls().size(), 1);
    QCOMPARE(view1->methodCalls().at(0).name,QString("modelAttached"));
    QCOMPARE(view2->methodCalls().size(), 1);
    QCOMPARE(view2->methodCalls().at(0).name,QString("modelAttached"));

    QList<TestView::MethodCall> expectedCalls;
    expectedCalls << TestView::MethodCall("modelAttached",
                                          QStringList() << QString::number(
                                              reinterpret_cast<qint64>(model.get())));
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    ModelNode childNode = addNodeListChild(view2->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
    expectedCalls << TestView::MethodCall("nodeCreated", QStringList() << "");
    expectedCalls << TestView::MethodCall("nodeAboutToBeReparented", QStringList() << "" << "data" << "" << "PropertiesAdded");
    expectedCalls << TestView::MethodCall("nodeReparented", QStringList() << "" << "data" << "" << "PropertiesAdded");


    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.setIdWithoutRefactoring("supernode");
    expectedCalls << TestView::MethodCall("nodeIdChanged", QStringList() << "supernode" << "supernode" << "");
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.variantProperty("visible").setValue(false);
    expectedCalls << TestView::MethodCall("variantPropertiesChanged", QStringList() << "visible" << "PropertiesAdded");
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.setIdWithoutRefactoring("supernode2");
    expectedCalls << TestView::MethodCall("nodeIdChanged", QStringList() << "supernode2" << "supernode2" << "supernode");
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.variantProperty("visible").setValue(false); // its tae some value so no notification should be sent
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.variantProperty("visible").setValue(true);
    expectedCalls << TestView::MethodCall("variantPropertiesChanged", QStringList() << "visible" << "");
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.bindingProperty("visible").setExpression("false && true");
    expectedCalls << TestView::MethodCall("propertiesAboutToBeRemoved", QStringList() << "visible");
    expectedCalls << TestView::MethodCall("propertiesRemoved", QStringList() << "visible");
    expectedCalls << TestView::MethodCall("bindingPropertiesChanged", QStringList() << "visible" << "PropertiesAdded");
    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    childNode.destroy();
    expectedCalls << TestView::MethodCall("nodeAboutToBeRemoved", QStringList() << "supernode2");
    expectedCalls << TestView::MethodCall("nodeRemoved", QStringList() << "1" << "data" << "EmptyPropertiesRemoved");

    QCOMPARE(view1->methodCalls(), expectedCalls);
    QCOMPARE(view2->methodCalls(), expectedCalls);

    model->detachView(view1.data());
    expectedCalls << TestView::MethodCall("modelAboutToBeDetached",
                                          QStringList() << QString::number(
                                              reinterpret_cast<qint64>(model.get())));
    QCOMPARE(view1->methodCalls(), expectedCalls);

    QApplication::processEvents();
}

void tst_TestCore::testRewriterTransaction()
{
    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    RewriterTransaction transaction = view->beginRewriterTransaction("TEST");
    QVERIFY(transaction.isValid());

    ModelNode childNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(childNode.isValid());

    childNode.destroy();
    QVERIFY(!childNode.isValid());

    {
        RewriterTransaction transaction2 = view->beginRewriterTransaction("TEST");
        QVERIFY(transaction2.isValid());

        ModelNode childNode = addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data");
        QVERIFY(childNode.isValid());

        childNode.destroy();
        QVERIFY(!childNode.isValid());

        RewriterTransaction transaction3(transaction2);
        QVERIFY(!transaction2.isValid());
        QVERIFY(transaction3.isValid());

        transaction2 = transaction3;
        QVERIFY(transaction2.isValid());
        QVERIFY(!transaction3.isValid());

        transaction3 = transaction3;
        transaction2 = transaction2;
        QVERIFY(transaction2.isValid());
        QVERIFY(!transaction3.isValid());
    }

    QVERIFY(transaction.isValid());
    transaction.commit();
    QVERIFY(!transaction.isValid());
}

void tst_TestCore::testRewriterId()
{
    char qmlString[] = "import QtQuick 2.1\n"
                       "Rectangle {\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());
    QCOMPARE(rootModelNode.type(),  QmlDesigner::TypeName("QtQuick.Rectangle"));

    QVERIFY(rootModelNode.isValid());

    ModelNode newNode(view->createModelNode("QtQuick.Rectangle", 2, 0));
    newNode.setIdWithoutRefactoring("testId");

    rootModelNode.nodeListProperty("data").reparentHere(newNode);

    const QLatin1String expected("import QtQuick 2.1\n"
                                 "Rectangle {\n"
                                 "Rectangle {\n"
                                 "    id: testId\n"
                                 "}\n"
                                 "}\n");

    QCOMPARE(stripWhiteSpaces(textEdit.toPlainText()), stripWhiteSpaces(expected));
}

void tst_TestCore::testRewriterNodeReparentingTransaction1()
{
     char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());

    QVERIFY(rootModelNode.isValid());

    ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    ModelNode childNode3 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    ModelNode childNode4 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");

    ModelNode reparentNode = addNodeListChild(childNode1, "QtQuick.Rectangle", 2, 0, "data");

    RewriterTransaction rewriterTransaction = view->beginRewriterTransaction("TEST");

    childNode2.nodeListProperty("data").reparentHere(reparentNode);
    childNode3.nodeListProperty("data").reparentHere(reparentNode);
    childNode4.nodeListProperty("data").reparentHere(reparentNode);

    reparentNode.destroy();

    rewriterTransaction.commit();
}

void tst_TestCore::testRewriterNodeReparentingTransaction2()
{
     char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(),  QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());

    QVERIFY(rootModelNode.isValid());

    ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");

    childNode2.variantProperty("x").setValue(200);
    childNode2.variantProperty("y").setValue(50);
    childNode2.variantProperty("color").setValue(QColor(Qt::red));
    childNode2.setIdWithoutRefactoring("childNode2");

    childNode1.variantProperty("x").setValue(100);
    childNode1.variantProperty("y").setValue(10);
    childNode1.variantProperty("color").setValue(QColor(Qt::blue));
    childNode1.setIdWithoutRefactoring("childNode1");

    RewriterTransaction rewriterTransaction = view->beginRewriterTransaction("TEST");

    childNode1.nodeListProperty("data").reparentHere(childNode2);
    childNode2.variantProperty("x").setValue(300);
    childNode2.variantProperty("y").setValue(150);

    rewriterTransaction.commit();

    rewriterTransaction = view->beginRewriterTransaction("TEST");

    rootModelNode.nodeListProperty("data").reparentHere(childNode2);
    childNode2.variantProperty("x").setValue(100);
    childNode2.variantProperty("y").setValue(200);

    rewriterTransaction.commit();

    rewriterTransaction = view->beginRewriterTransaction("TEST");

    rootModelNode.nodeListProperty("data").reparentHere(childNode2);
    childNode2.variantProperty("x").setValue(150);
    childNode2.variantProperty("y").setValue(250);
    childNode1.nodeListProperty("data").reparentHere(childNode2);

    rewriterTransaction.commit();
}

void tst_TestCore::testRewriterNodeReparentingTransaction3()
{
    char qmlString[] = "import QtQuick 2.0\n"
                      "Rectangle {\n"
                      "}\n";

   QPlainTextEdit textEdit;
   textEdit.setPlainText(QLatin1String(qmlString));
   NotIndentingTextEditModifier textModifier(&textEdit);

   auto model(Model::create("QtQuick.Item", 2, 0));
   QVERIFY(model.get());

   QScopedPointer<TestView> view(new TestView);
   QVERIFY(view.data());
   model->attachView(view.data());

   ModelNode rootModelNode(view->rootModelNode());
   QVERIFY(rootModelNode.isValid());
   QCOMPARE(rootModelNode.type(),  QmlDesigner::TypeName("QtQuick.Item"));
   auto testRewriterView = createTextRewriterView(*model);
   testRewriterView->setTextModifier(&textModifier);

   model->attachView(testRewriterView.get());

   QVERIFY(rootModelNode.isValid());

   ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode3 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode4 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");

   RewriterTransaction rewriterTransaction = view->beginRewriterTransaction("TEST");

   childNode1.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(151);
   childNode4.variantProperty("y").setValue(251);
   childNode2.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(152);
   childNode4.variantProperty("y").setValue(252);
   childNode3.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(153);
   childNode4.variantProperty("y").setValue(253);
   rootModelNode.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(154);
   childNode4.variantProperty("y").setValue(254);

   rewriterTransaction.commit();
}

void tst_TestCore::testRewriterNodeReparentingTransaction4()
{
    char qmlString[] = "import QtQuick 2.0\n"
                      "Rectangle {\n"
                      "}\n";

   QPlainTextEdit textEdit;
   textEdit.setPlainText(QLatin1String(qmlString));
   NotIndentingTextEditModifier textModifier(&textEdit);

   auto model(Model::create("QtQuick.Item", 2, 0));
   QVERIFY(model.get());

   QScopedPointer<TestView> view(new TestView);
   QVERIFY(view.data());
   model->attachView(view.data());

   ModelNode rootModelNode(view->rootModelNode());
   QVERIFY(rootModelNode.isValid());
   QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
   auto testRewriterView = createTextRewriterView(*model);
   testRewriterView->setTextModifier(&textModifier);

   model->attachView(testRewriterView.get());

   QVERIFY(rootModelNode.isValid());

   ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode3 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode4 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
   ModelNode childNode5 = addNodeListChild(childNode2, "QtQuick.Rectangle", 2, 0, "data");

   RewriterTransaction rewriterTransaction = view->beginRewriterTransaction("TEST");

   childNode1.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(151);
   childNode4.variantProperty("y").setValue(251);
   childNode2.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(152);
   childNode4.variantProperty("y").setValue(252);
   childNode3.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(153);
   childNode4.variantProperty("y").setValue(253);
   childNode5.nodeListProperty("data").reparentHere(childNode4);
   childNode4.variantProperty("x").setValue(154);
   childNode4.variantProperty("y").setValue(254);

   rewriterTransaction.commit();
}

void tst_TestCore::testRewriterAddNodeTransaction()
{
    char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());

    QVERIFY(rootModelNode.isValid());


    ModelNode childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");

    RewriterTransaction rewriterTransaction = view->beginRewriterTransaction("TEST");

    ModelNode newNode = view->createModelNode("QtQuick.Rectangle", 2, 0);
    newNode.variantProperty("x").setValue(100);
    newNode.variantProperty("y").setValue(100);

    rootModelNode.nodeListProperty("data").reparentHere(newNode);
    childNode.nodeListProperty("data").reparentHere(newNode);

    rewriterTransaction.commit();
}

void tst_TestCore::testRewriterComponentId()
{
    char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                       "   Component {\n"
                       "       id: testComponent\n"
                       "       Item {\n"
                       "       }\n"
                       "   }\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    QVERIFY(!model->rewriterView());
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());
    QVERIFY(model->rewriterView());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Item", 2, 1));

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode component(rootModelNode.directSubModelNodes().first());
    QVERIFY(component.isValid());
    QCOMPARE(component.simplifiedTypeName(), QmlDesigner::TypeName("Component"));
    QCOMPARE(component.id(), QString("testComponent"));
}

void tst_TestCore::testRewriterTransactionRewriter()
{
    char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());

    QVERIFY(rootModelNode.isValid());

    ModelNode childNode1;
    ModelNode childNode2;

    {
        RewriterTransaction transaction = view->beginRewriterTransaction("TEST");
        childNode1 = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
        childNode1.variantProperty("x").setValue(10);
        childNode1.variantProperty("y").setValue(10);
    }

    QVERIFY(childNode1.isValid());
    QCOMPARE(childNode1.variantProperty("x").value(), QVariant(10));
    QCOMPARE(childNode1.variantProperty("y").value(), QVariant(10));


    {
        RewriterTransaction transaction = view->beginRewriterTransaction("TEST");
        childNode2 = addNodeListChild(childNode1, "QtQuick.Rectangle", 2, 0, "data");
        childNode2.destroy();
    }

    QVERIFY(!childNode2.isValid());
    QVERIFY(childNode1.isValid());
}

void tst_TestCore::testRewriterPropertyDeclarations()
{
    // Work in progress.

    //
    // test properties defined in qml
    //
    // [default] property <type> <name>[: defaultValue]
    //
    // where type is (int | bool | double | real | string | url | color | date | variant)
    //

    // Unsupported:
    //  property variant varProperty2: boolProperty
    //  property variant myArray: [ Rectangle {} ]
    //  property variant someGradient: Gradient {}

    char qmlString[] = "import QtQuick 2.1\n"
        "Item {\n"
        "   property int intProperty\n"
        "   property bool boolProperty: true\n"
        "   property variant varProperty1\n"
        "   default property url urlProperty\n"
        "   intProperty: 2\n"
        "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    //
    // parsing
    //
    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));

    QCOMPARE(rootModelNode.properties().size(), 4);

    VariantProperty intProperty = rootModelNode.property("intProperty").toVariantProperty();
    QVERIFY(intProperty.isValid());
    QVERIFY(intProperty.isVariantProperty());

    VariantProperty boolProperty = rootModelNode.property("boolProperty").toVariantProperty();
    QVERIFY(boolProperty.isValid());
    QVERIFY(boolProperty.isVariantProperty());
    QCOMPARE(boolProperty.value(), QVariant(true));

    VariantProperty varProperty1 = rootModelNode.property("varProperty1").toVariantProperty();
    QVERIFY(varProperty1.isValid());
    QVERIFY(varProperty1.isVariantProperty());

    VariantProperty urlProperty = rootModelNode.property("urlProperty").toVariantProperty();
    QVERIFY(urlProperty.isValid());
    QVERIFY(urlProperty.isVariantProperty());
    QCOMPARE(urlProperty.value(), QVariant(QUrl()));
}

void tst_TestCore::testRewriterPropertyAliases()
{
    //
    // test property aliases defined in qml, i.e.
    //
    // [default] property alias <name>: <alias reference>
    //
    // where type is (int | bool | double | real | string | url | color | date | variant)
    //

    char qmlString[] = "import QtQuick 2.1\n"
        "Item {\n"
        "   property alias theText: t.text\n"
        "   default alias property yPos: t.y\n"
        "   Text { id: t; text: \"zoo\"}\n"
        "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));

    QList<AbstractProperty> properties  = rootModelNode.properties();
    QCOMPARE(properties.size(), 0); // TODO: How to represent alias properties? As Bindings?
}

void tst_TestCore::testRewriterPositionAndOffset()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: root\n"
                                  "    x: 10;\n"
                                  "    y: 10;\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle1\n"
                                  "        x: 10;\n"
                                  "        y: 10;\n"
                                  "    }\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle2\n"
                                  "        x: 100;\n"
                                  "        y: 100;\n"
                                  "        anchors.fill: root\n"
                                  "    }\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle3\n"
                                  "        x: 140;\n"
                                  "        y: 180;\n"
                                  "        gradient: Gradient {\n"
                                  "             GradientStop {\n"
                                  "                 position: 0\n"
                                  "                 color: \"white\"\n"
                                  "             }\n"
                                  "             GradientStop {\n"
                                  "                  position: 1\n"
                                  "                  color: \"black\"\n"
                                  "             }\n"
                                  "        }\n"
                                  "    }\n"
                                  "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QString string = QString(qmlString).mid(testRewriterView->nodeOffset(rootNode), testRewriterView->nodeLength(rootNode));
    const QString qmlExpected0("Rectangle {\n"
                               "    id: root\n"
                               "    x: 10;\n"
                               "    y: 10;\n"
                               "    Rectangle {\n"
                               "        id: rectangle1\n"
                               "        x: 10;\n"
                               "        y: 10;\n"
                               "    }\n"
                               "    Rectangle {\n"
                               "        id: rectangle2\n"
                               "        x: 100;\n"
                               "        y: 100;\n"
                               "        anchors.fill: root\n"
                               "    }\n"
                               "    Rectangle {\n"
                               "        id: rectangle3\n"
                               "        x: 140;\n"
                               "        y: 180;\n"
                               "        gradient: Gradient {\n"
                               "             GradientStop {\n"
                               "                 position: 0\n"
                               "                 color: \"white\"\n"
                               "             }\n"
                               "             GradientStop {\n"
                               "                  position: 1\n"
                               "                  color: \"black\"\n"
                               "             }\n"
                               "        }\n"
                               "    }\n"
                               "}");
    QCOMPARE(string, qmlExpected0);

    ModelNode lastRectNode = rootNode.directSubModelNodes().last();
    ModelNode gradientNode = lastRectNode.directSubModelNodes().first();
    ModelNode gradientStop = gradientNode.directSubModelNodes().first();

    int offset = testRewriterView->nodeOffset(gradientNode);
    int length = testRewriterView->nodeLength(gradientNode);
    string = QString(qmlString).mid(offset, length);
    const QString qmlExpected1(     "Gradient {\n"
                               "             GradientStop {\n"
                               "                 position: 0\n"
                               "                 color: \"white\"\n"
                               "             }\n"
                               "             GradientStop {\n"
                               "                  position: 1\n"
                               "                  color: \"black\"\n"
                               "             }\n"
                               "        }");
    QCOMPARE(string, qmlExpected1);

    string = QString(qmlString).mid(testRewriterView->nodeOffset(gradientStop), testRewriterView->nodeLength(gradientStop));
    const QString qmlExpected2(             "GradientStop {\n"
                               "                 position: 0\n"
                               "                 color: \"white\"\n"
                               "             }");

    QCOMPARE(string, qmlExpected2);
}

void tst_TestCore::testRewriterComponentTextModifier()
{
    const QString qmlString("import QtQuick 2.1\n"
                            "Rectangle {\n"
                            "    id: root\n"
                            "    x: 10;\n"
                            "    y: 10;\n"
                            "    Rectangle {\n"
                            "        id: rectangle1\n"
                            "        x: 10;\n"
                            "        y: 10;\n"
                            "    }\n"
                            "    Component {\n"
                            "        id: rectangleComponent\n"
                            "        Rectangle {\n"
                            "            x: 100;\n"
                            "            y: 100;\n"
                            "        }\n"
                            "    }\n"
                            "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode componentNode  = rootNode.directSubModelNodes().last();

    int componentStartOffset = testRewriterView->nodeOffset(componentNode);
    int componentEndOffset = componentStartOffset + testRewriterView->nodeLength(componentNode);

    int rootStartOffset = testRewriterView->nodeOffset(rootNode);

    ComponentTextModifier componentTextModifier(&textModifier, componentStartOffset, componentEndOffset, rootStartOffset);

     const QString qmlExpected("import QtQuick 2.1\n"
                            "            "
                            "             "
                            "           "
                            "           "
                            "                "
                            "                       "
                            "               "
                            "               "
                            "      "
                            "    Component {\n"
                            "        id: rectangleComponent\n"
                            "        Rectangle {\n"
                            "            x: 100;\n"
                            "            y: 100;\n"
                            "        }\n"
                            "    } "
                            " ");

    QCOMPARE(componentTextModifier.text(), qmlExpected);

    auto componentModel(Model::create("QtQuick.Item", 1, 1));
    auto testRewriterViewComponent = createTextRewriterView(*componentModel);
    testRewriterViewComponent->setTextModifier(&componentTextModifier);
    componentModel->attachView(testRewriterViewComponent.get());

    ModelNode componentrootNode = testRewriterViewComponent->rootModelNode();
    QVERIFY(componentrootNode.isValid());
    //The <Component> node is skipped
    QCOMPARE(componentrootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
}

void tst_TestCore::testRewriterPreserveType()
{
    const QString qmlString("import QtQuick 2.1\n"
                            "Rectangle {\n"
                            "    id: root\n"
                            "    Text {\n"
                            "        font.bold: true\n"
                            "        font.pointSize: 14\n"
                            "    }\n"
                            "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode textNode = rootNode.directSubModelNodes().first();
    QCOMPARE(QMetaType::Bool, textNode.variantProperty("font.bold").value().typeId());
    QCOMPARE(QMetaType::Double, textNode.variantProperty("font.pointSize").value().typeId());
    textNode.variantProperty("font.bold").setValue(QVariant(false));
    textNode.variantProperty("font.bold").setValue(QVariant(true));
    textNode.variantProperty("font.pointSize").setValue(QVariant(13.0));

    ModelNode newTextNode = addNodeListChild(rootNode, "QtQuick.Text", 2, 0, "data");

    newTextNode.variantProperty("font.bold").setValue(QVariant(true));
    newTextNode.variantProperty("font.pointSize").setValue(QVariant(13.0));

    QCOMPARE(QMetaType::Bool, newTextNode.variantProperty("font.bold").value().typeId());
    QCOMPARE(QMetaType::Double, newTextNode.variantProperty("font.pointSize").value().typeId());
}

void tst_TestCore::testRewriterForArrayMagic()
{
    try {
        const QLatin1String qmlString("import QtQuick 2.1\n"
                                      "\n"
                                      "Rectangle {\n"
                                      "    states: State {\n"
                                      "        name: \"s1\"\n"
                                      "    }\n"
                                      "}\n");
        QPlainTextEdit textEdit;
        textEdit.setPlainText(qmlString);
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        QScopedPointer<TestView> view(new TestView);
        model->attachView(view.data());

        // read in
        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        ModelNode rootNode = view->rootModelNode();
        QVERIFY(rootNode.isValid());
        QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
        QVERIFY(rootNode.property("states").isNodeListProperty());

        QmlItemNode rootItem(rootNode);
        QVERIFY(rootItem.isValid());

        QmlModelState state1(rootItem.states().addState("s2"));
        QCOMPARE(state1.modelNode().type(), QmlDesigner::TypeName("QtQuick.State"));

        const QLatin1String expected("import QtQuick 2.1\n"
                                     "\n"
                                     "Rectangle {\n"
                                     "    states: [\n"
                                     "    State {\n"
                                     "        name: \"s1\"\n"
                                     "    },\n"
                                     "    State {\n"
                                     "        name: \"s2\"\n"
                                     "    }\n"
                                     "    ]\n"
                                     "}\n");
        QCOMPARE(stripWhiteSpaces(textEdit.toPlainText()), stripWhiteSpaces(expected));
    } catch (Exception &e) {
        qDebug() << "Exception:" << e.description() << "at line" << e.line() << "in function" << e.function() << "in file" << e.file();
        QFAIL(qPrintable(e.description()));
    }
}

void tst_TestCore::testRewriterWithSignals()
{
    const QLatin1String qmlString("import QtQuick 2.1\n"
                                  "\n"
                                  "TextEdit {\n"
                                  "    onTextChanged: { print(\"foo\"); }\n"
                                  "}\n");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.TextEdit"));

    QmlItemNode rootItem(rootNode);
    QVERIFY(rootItem.isValid());

    QCOMPARE(rootNode.properties().count(), 1);
}

void tst_TestCore::testRewriterNodeSliding()
{
    const QLatin1String qmlString("import QtQuick 2.1\n"
                                  "Rectangle {\n"
                                  "    id: root\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle1\n"
                                  "        x: 10;\n"
                                  "        y: 10;\n"
                                  "    }\n"
                                  "\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle2\n"
                                  "        x: 100;\n"
                                  "        y: 100;\n"
                                  "    }\n"
                                  "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rootNode.id(), QLatin1String("root"));

    QCOMPARE(rootNode.nodeListProperty("data").toModelNodeList().at(0).id(), QLatin1String("rectangle1"));
    QCOMPARE(rootNode.nodeListProperty("data").toModelNodeList().at(1).id(), QLatin1String("rectangle2"));

    rootNode.nodeListProperty("data").slide(0, 1);

    QCOMPARE(rootNode.nodeListProperty(("data")).toModelNodeList().at(0).id(), QLatin1String("rectangle2"));
    QCOMPARE(rootNode.nodeListProperty(("data")).toModelNodeList().at(1).id(), QLatin1String("rectangle1"));

    rootNode.nodeListProperty("data").slide(1, 0);

    QCOMPARE(rootNode.nodeListProperty("data").toModelNodeList().at(0).id(), QLatin1String("rectangle1"));
    QCOMPARE(rootNode.nodeListProperty("data").toModelNodeList().at(1).id(), QLatin1String("rectangle2"));
}

void tst_TestCore::testRewriterExceptionHandling()
{
    const QLatin1String qmlString("import QtQuick 2.1\n"
                                  "Text {\n"
                                  "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Text", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    testRewriterView->setCheckSemanticErrors(true);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Text"));

    try
    {
        RewriterTransaction transaction = view->beginRewriterTransaction("TEST");
        rootNode.variantProperty("text").setValue(QVariant("text"));
        rootNode.variantProperty("bla").setValue(QVariant("blah"));
        transaction.commit();
        QFAIL("RewritingException should be thrown");
    } catch (RewritingException &) {
        QVERIFY(rootNode.isValid());
        QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Text"));

        QVERIFY(!rootNode.hasProperty("bla"));
        QVERIFY(!rootNode.hasProperty("text"));
    }
}

void tst_TestCore::testRewriterFirstDefinitionInside()
{
    const QString qmlString("import QtQuick 2.1\n"
                            "Rectangle {\n"
                            "    id: root\n"
                            "    x: 10;\n"
                            "    y: 10;\n"
                            "    Rectangle {\n"
                            "        id: rectangle1\n"
                            "        x: 10;\n"
                            "        y: 10;\n"
                            "    }\n"
                            "    Component {\n"
                            "        id: rectangleComponent\n"
                            "        Rectangle {\n"
                            "            x: 100;\n"
                            "            y: 100;\n"
                            "        }\n"
                            "    }\n"
                            "}");


    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode componentNode  = rootNode.directSubModelNodes().last();

    QString string = "";
    for (int i = testRewriterView->firstDefinitionInsideOffset(componentNode); i < testRewriterView->firstDefinitionInsideOffset(componentNode) +  testRewriterView->firstDefinitionInsideLength(componentNode);i++)
        string +=QString(qmlString)[i];

    const QString qmlExpected("Rectangle {\n"
                              "            x: 100;\n"
                              "            y: 100;\n"
                              "        }");
    QCOMPARE(string, qmlExpected);

}

void tst_TestCore::testCopyModelRewriter1()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: root\n"
                                  "    x: 10;\n"
                                  "    y: 10;\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle1\n"
                                  "        x: 10;\n"
                                  "        y: 10;\n"
                                  "    }\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle2\n"
                                  "        x: 100;\n"
                                  "        y: 100;\n"
                                  "        anchors.fill: root\n"
                                  "    }\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle3\n"
                                  "        x: 140;\n"
                                  "        y: 180;\n"
                                  "        gradient: Gradient {\n"
                                  "             GradientStop {\n"
                                  "                 position: 0\n"
                                  "                 color: \"white\"\n"
                                  "             }\n"
                                  "             GradientStop {\n"
                                  "                  position: 1\n"
                                  "                  color: \"black\"\n"
                                  "             }\n"
                                  "        }\n"
                                  "    }\n"
                                  "}");

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model1.get());

    QScopedPointer<TestView> view1(new TestView);
    model1->attachView(view1.data());

    // read in 1
    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&textModifier1);
    model1->attachView(testRewriterView1.get());

    ModelNode rootNode1 = view1->rootModelNode();
    QVERIFY(rootNode1.isValid());
    QCOMPARE(rootNode1.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QPlainTextEdit textEdit2;
    textEdit2.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier2(&textEdit2);

    auto model2(Model::create("QtQuick.Item", 1, 1));
    QVERIFY(model2.get());

    QScopedPointer<TestView> view2(new TestView);
    model2->attachView(view2.data());

    // read in 2
    auto testRewriterView2 = createTextRewriterView(*model2);
    testRewriterView2->setTextModifier(&textModifier2);
    model2->attachView(testRewriterView2.get());

    ModelNode rootNode2 = view2->rootModelNode();
    QVERIFY(rootNode2.isValid());
    QCOMPARE(rootNode2.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));


    //

    ModelNode childNode(rootNode1.directSubModelNodes().first());
    QVERIFY(childNode.isValid());

    RewriterTransaction transaction(view1->beginRewriterTransaction("TEST"));

    ModelMerger merger(view1.data());

    ModelNode insertedNode(merger.insertModel(rootNode2));
    transaction.commit();

    QCOMPARE(insertedNode.id(), QString("root1"));

    QVERIFY(insertedNode.isValid());
    childNode.nodeListProperty("data").reparentHere(insertedNode);

    const QLatin1String expected(

        "\n"
        "import QtQuick 2.1\n"
        "\n"
        "Rectangle {\n"
        "    id: root\n"
        "    x: 10;\n"
        "    y: 10;\n"
        "    Rectangle {\n"
        "        id: rectangle1\n"
        "        x: 10;\n"
        "        y: 10;\n"
        "\n"
        "        Rectangle {\n"
        "            id: root1\n"
        "            x: 10\n"
        "            y: 10\n"
        "            Rectangle {\n"
        "                id: rectangle4\n"
        "                x: 10\n"
        "                y: 10\n"
        "            }\n"
        "\n"
        "            Rectangle {\n"
        "                id: rectangle5\n"
        "                x: 100\n"
        "                y: 100\n"
        "                anchors.fill: root1\n"
        "            }\n"
        "\n"
        "            Rectangle {\n"
        "                id: rectangle6\n"
        "                x: 140\n"
        "                y: 180\n"
        "                gradient: Gradient {\n"
        "                    GradientStop {\n"
        "                        position: 0\n"
        "                        color: \"#ffffff\"\n"
        "                    }\n"
        "\n"
        "                    GradientStop {\n"
        "                        position: 1\n"
        "                        color: \"#000000\"\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    Rectangle {\n"
        "        id: rectangle2\n"
        "        x: 100;\n"
        "        y: 100;\n"
        "        anchors.fill: root\n"
        "    }\n"
        "    Rectangle {\n"
        "        id: rectangle3\n"
        "        x: 140;\n"
        "        y: 180;\n"
        "        gradient: Gradient {\n"
        "             GradientStop {\n"
        "                 position: 0\n"
        "                 color: \"white\"\n"
        "             }\n"
        "             GradientStop {\n"
        "                  position: 1\n"
        "                  color: \"black\"\n"
        "             }\n"
        "        }\n"
        "    }\n"
        "}");

    QCOMPARE(stripWhiteSpaces(textEdit1.toPlainText()), stripWhiteSpaces(expected));
}

static QString readQmlFromFile(const QString& fileName)
{
    QFile qmlFile(fileName);
    qmlFile.open(QIODevice::ReadOnly | QIODevice::Text);
    QString qmlFileContents = QString::fromUtf8(qmlFile.readAll());
    return qmlFileContents;
}

void tst_TestCore::testMergeModelRewriter1_data()
{
    QTest::addColumn<QString>("qmlTemplateString");
    QTest::addColumn<QString>("qmlStyleString");
    QTest::addColumn<QString>("qmlExpectedString");

    QString simpleTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SimpleTemplate.qml");
    QString simpleStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SimpleStyle.qml");
    QString simpleExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SimpleExpected.qml");

    QString complexTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ComplexTemplate.qml");
    QString complexStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ComplexStyle.qml");
    QString complexExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ComplexExpected.qml");

    QString emptyTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/EmptyTemplate.qml");
    QString emptyStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/EmptyStyle.qml");
    QString emptyExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/EmptyExpected.qml");

    QString rootReplacementTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/RootReplacementTemplate.qml");
    QString rootReplacementStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/RootReplacementStyle.qml");
    QString rootReplacementExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/RootReplacementExpected.qml");

    QString switchTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SwitchTemplate.qml");
    QString switchStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SwitchStyle.qml");
    QString switchExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SwitchExpected.qml");

    QString sliderTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SliderTemplate.qml");
    QString sliderStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SliderStyle.qml");
    QString sliderExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/SliderExpected.qml");

    QString listViewTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ListViewTemplate.qml");
    QString listViewStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ListViewStyle.qml");
    QString listViewExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ListViewExpected.qml");

    QString buttonTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonTemplate.qml");
    QString buttonInlineStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonStyleInline.qml");
    QString buttonInlineExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonInlineExpected.qml");

    QString buttonAbsoluteTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonAbsoluteTemplate.qml");
    QString buttonOutlineStyleQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonStyleOutline.qml");
    QString buttonOutlineExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonOutlineExpected.qml");

    QString buttonStyleUiQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonStyle.ui.qml");
    QString buttonStyleUiExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonStyle.ui.Expected.qml");

    QString buttonAbsoluteTemplateWithOptionsQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonAbsoluteTemplateWithOptions.qml");
    QString buttonStyleUiWithOptionsExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging/ButtonStyleWithOptions.ui.Expected.qml");

    QString testExportButtonTemplateQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging//test_export_button_template.ui.qml");
    QString testExportButtonStylesheetQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging//test_export_button_stylesheet.ui.qml");
    QString testExportButtonExpectedQmlContents = readQmlFromFile(QString(TESTSRCDIR) + "/../data/merging//test_export_button_expected.ui.qml");


    QTest::newRow("Simple style replacement") << simpleTemplateQmlContents << simpleStyleQmlContents << simpleExpectedQmlContents;
    QTest::newRow("Complex style replacement") << complexTemplateQmlContents << complexStyleQmlContents << complexExpectedQmlContents;
    QTest::newRow("Empty stylesheet") << emptyTemplateQmlContents << emptyStyleQmlContents << emptyExpectedQmlContents;
    QTest::newRow("Root node replacement") << rootReplacementTemplateQmlContents << rootReplacementStyleQmlContents << rootReplacementExpectedQmlContents;
    QTest::newRow("Switch styling") << switchTemplateQmlContents << switchStyleQmlContents << switchExpectedQmlContents;
    QTest::newRow("Slider styling") << sliderTemplateQmlContents << sliderStyleQmlContents << sliderExpectedQmlContents;
    QTest::newRow("List View styling") << listViewTemplateQmlContents << listViewStyleQmlContents << listViewExpectedQmlContents;
    QTest::newRow("Button Inline styling") << buttonTemplateQmlContents << buttonInlineStyleQmlContents << buttonInlineExpectedQmlContents;

    QTest::newRow("Button Outline styling") << buttonAbsoluteTemplateQmlContents << buttonOutlineStyleQmlContents << buttonOutlineExpectedQmlContents;
    QTest::newRow("Button Designer styling") << buttonAbsoluteTemplateQmlContents << buttonStyleUiQmlContents << buttonStyleUiExpectedQmlContents;

    QTest::newRow("Button Designer styling with options") << buttonAbsoluteTemplateWithOptionsQmlContents << buttonStyleUiQmlContents << buttonStyleUiWithOptionsExpectedQmlContents;
    QTest::newRow("Button styling with info screen test case ") << testExportButtonTemplateQmlContents << testExportButtonStylesheetQmlContents << testExportButtonExpectedQmlContents;

}

void tst_TestCore::testMergeModelRewriter1()
{
    QFETCH(QString, qmlTemplateString);
    QFETCH(QString, qmlStyleString);
    QFETCH(QString, qmlExpectedString);

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(qmlTemplateString);
    NotIndentingTextEditModifier textModifier1(&textEdit1);

    auto templateModel(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(templateModel.get());

    QScopedPointer<TestView> templateView(new TestView);
    templateModel->attachView(templateView.data());

    // read in 1
    auto templateRewriterView = createTextRewriterView(*templateModel);
    templateRewriterView->setTextModifier(&textModifier1);
    templateModel->attachView(templateRewriterView.get());

    ModelNode templateRootNode = templateView->rootModelNode();
    QVERIFY(templateRootNode.isValid());

    QPlainTextEdit textEdit2;
    textEdit2.setPlainText(qmlStyleString);
    NotIndentingTextEditModifier textModifier2(&textEdit2);

    auto styleModel(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(styleModel.get());

    QScopedPointer<TestView> styleView(new TestView);
    styleModel->attachView(styleView.data());

    // read in 2
    auto styleRewriterView = createTextRewriterView(*styleModel);
    styleRewriterView->setTextModifier(&textModifier2);
    styleModel->attachView(styleRewriterView.get());

    StylesheetMerger merger(templateView.data(), styleView.data());
    merger.merge();

    QString trimmedActual = stripWhiteSpaces(textEdit1.toPlainText());
    QString trimmedExpected = stripWhiteSpaces(qmlExpectedString);

    QCOMPARE(trimmedActual, trimmedExpected);
}

void tst_TestCore::testCopyModelRewriter2()
{
   const QLatin1String qmlString1("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "id: root\n"
                                  "x: 10\n"
                                  "y: 10\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: rectangle1\n"
                                  "    x: 10\n"
                                  "    y: 10\n"
                                  "}\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: rectangle2\n"
                                  "    x: 100\n"
                                  "    y: 100\n"
                                  "    anchors.fill: rectangle1\n"
                                  "}\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    id: rectangle3\n"
                                  "    x: 140\n"
                                  "    y: 180\n"
                                  "    gradient: Gradient {\n"
                                  "        GradientStop {\n"
                                  "            position: 0\n"
                                  "            color: \"#ffffff\"\n"
                                  "        }\n"
                                  "\n"
                                  "        GradientStop {\n"
                                  "            position: 1\n"
                                  "            color: \"#000000\"\n"
                                  "        }\n"
                                  "    }\n"
                                  "}\n"
                                  "}");


    const QLatin1String qmlString2("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");

    QPlainTextEdit textEdit1;
    textEdit1.setPlainText(qmlString1);
    NotIndentingTextEditModifier textModifier1(&textEdit1);

    auto templateModel(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(templateModel.get());

    QScopedPointer<TestView> view1(new TestView);
    templateModel->attachView(view1.data());

    // read in 1
    auto testRewriterView1 = createTextRewriterView(*templateModel);
    testRewriterView1->setTextModifier(&textModifier1);
    templateModel->attachView(testRewriterView1.get());

    ModelNode rootNode1 = view1->rootModelNode();
    QVERIFY(rootNode1.isValid());
    QCOMPARE(rootNode1.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    // read in 2
    QPlainTextEdit textEdit2;
    textEdit2.setPlainText(qmlString2);
    NotIndentingTextEditModifier textModifier2(&textEdit2);

    auto styleModel(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(styleModel.get());

    QScopedPointer<TestView> view2(new TestView);
    styleModel->attachView(view2.data());

    auto styleRewriterView = createTextRewriterView(*styleModel);
    styleRewriterView->setTextModifier(&textModifier2);
    styleModel->attachView(styleRewriterView.get());

    ModelNode rootNode2 = view2->rootModelNode();
    QVERIFY(rootNode2.isValid());
    QCOMPARE(rootNode2.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));


    //

    ModelMerger merger(view2.data());

    merger.replaceModel(rootNode1);
    QVERIFY(rootNode2.isValid());
    QCOMPARE(rootNode2.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QCOMPARE(stripWhiteSpaces(textEdit2.toPlainText()), stripWhiteSpaces(qmlString1));
}

namespace {
bool contains(const QmlDesigner::PropertyMetaInfos &properties, QUtf8StringView propertyName)
{
    auto found = std::find_if(properties.begin(), properties.end(), [&](const auto &property) {
        return property.name() == propertyName;
    });

    return found != properties.end();
}
} // namespace

#ifndef QDS_USE_PROJECTSTORAGE
void tst_TestCore::testSubComponentManager()
{
    const QString qmlString("import QtQuick 2.15\n"
                        "Rectangle {\n"
                        "    id: root\n"
                        "    x: 10;\n"
                        "    y: 10;\n"
                        "    Rectangle {\n"
                        "        id: rectangle1\n"
                        "        x: 10;\n"
                        "        y: 10;\n"
                        "    }\n"
                        "    Component {\n"
                        "        id: rectangleComponent\n"
                        "        Rectangle {\n"
                        "            x: 100;\n"
                        "            y: 100;\n"
                        "        }\n"
                        "    }\n"
                        "}");

    QString fileName = QString(TESTSRCDIR) + "/../data/fx/usingmybutton.qml";
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(file.readAll()));
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(createModel("QtQuick.Rectangle", 2, 15));
    model->setFileUrl(QUrl::fromLocalFile(fileName));
    ExternalDependenciesFake externalDependenciesFake{model.get()};

    QScopedPointer<SubComponentManager> subComponentManager(
        new SubComponentManager(model.get(), externalDependenciesFake));
    subComponentManager->update(QUrl::fromLocalFile(fileName), model->imports());
    QVERIFY(model->hasNodeMetaInfo("QtQuick.Rectangle", 2, 15));
    QVERIFY(contains(model->metaInfo("QtQuick.Rectangle").properties(), "border.width"));

    model->rewriterView()->setTextModifier(&modifier);

    QVERIFY(model->rewriterView()->errors().isEmpty());

    QVERIFY(model->rewriterView()->rootModelNode().isValid());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Rectangle", 2, 15));
    QVERIFY(contains(model->metaInfo("QtQuick.Rectangle").properties(), "border.width"));

    QVERIFY(model->metaInfo("<cpp>.QQuickPen").isValid());

    QSKIP("File components not working TODO", SkipAll);
    NodeMetaInfo myButtonMetaInfo = model->metaInfo("MyButton");
    QVERIFY(myButtonMetaInfo.isValid());
    QVERIFY(contains(myButtonMetaInfo.properties(), "border.width"));
    QVERIFY(myButtonMetaInfo.hasProperty("border.width"));
}
#endif

void tst_TestCore::testAnchorsAndRewriting()
{
    const QString qmlString("import QtQuick 2.15\n"
                            "Rectangle {\n"
                            "    id: root\n"
                            "    x: 10;\n"
                            "    y: 10;\n"
                            "    Rectangle {\n"
                            "        id: rectangle1\n"
                            "        x: 10;\n"
                            "        y: 10;\n"
                            "    }\n"
                            "    Component {\n"
                            "        id: rectangleComponent\n"
                            "        Rectangle {\n"
                            "            x: 100;\n"
                            "            y: 100;\n"
                            "        }\n"
                            "    }\n"
                            "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QmlItemNode rootItemNode = view->rootQmlItemNode();
    QVERIFY(rootItemNode.isValid());

    QmlItemNode childNode = rootItemNode.allDirectSubNodes().first().toQmlItemNode();

    QVERIFY(childNode.modelNode().isValid());

    QVERIFY(childNode.isValid());

    childNode.anchors().setMargin(AnchorLineLeft, 280);
    //childNode.anchors().setAnchor(AnchorLineLeft, rootItemNode, AnchorLineLeft); Not working without instances
    childNode.anchors().setMargin(AnchorLineRight, 200);
    //childNode.anchors().setAnchor(AnchorLineRight, rootItemNode, AnchorLineRight); Not working without instances
    childNode.anchors().setMargin(AnchorLineBottom, 50);
    //childNode.anchors().setAnchor(AnchorLineBottom, rootItemNode, AnchorLineBottom); Not working without instances

    /*
    {
        RewriterTransaction transaction = view->beginRewriterTransaction("TEST");

        childNode.anchors().setMargin(AnchorLineTop, 100);
        childNode.anchors().setAnchor(AnchorLineTop, rootItemNode, AnchorLineTop);
    }
    */
}

void tst_TestCore::testAnchorsAndRewritingCenter()
{
    const QString qmlString("import QtQuick 2.15\n"
                            "Rectangle {\n"
                            "    id: root\n"
                            "    x: 10;\n"
                            "    y: 10;\n"
                            "    Rectangle {\n"
                            "        id: rectangle1\n"
                            "        x: 10;\n"
                            "        y: 10;\n"
                            "    }\n"
                            "    Component {\n"
                            "        id: rectangleComponent\n"
                            "        Rectangle {\n"
                            "            x: 100;\n"
                            "            y: 100;\n"
                            "        }\n"
                            "    }\n"
                            "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QmlItemNode rootItemNode = view->rootQmlItemNode();
    QVERIFY(rootItemNode.isValid());

    QmlItemNode childNode = rootItemNode.allDirectSubNodes().first().toQmlItemNode();
    QVERIFY(childNode.isValid());

    //childNode.anchors().setAnchor(AnchorLineVerticalCenter, rootItemNode, AnchorLineVerticalCenter); Not working without instances
    //childNode.anchors().setAnchor(AnchorLineHorizontalCenter, rootItemNode, AnchorLineHorizontalCenter); Not working without instances
}

void tst_TestCore::loadQml()
{
    char qmlString[] = "import QtQuick 2.15\n"
                       "Rectangle {\n"
                       "id: root;\n"
                       "width: 200;\n"
                       "height: 200;\n"
                       "color: \"white\";\n"
                       "Text {\n"
                       "id: text1\n"
                       "text: \"Hello World\"\n"
                       "anchors.centerIn: parent\n"
                       "Item {\n"
                       "id: item01\n"
                       "}\n"
                       "}\n"
                       "Rectangle {\n"
                       "id: rectangle;\n"
                       "gradient: Gradient {\n"
                       "GradientStop {\n"
                       "position: 0\n"
                       "color: \"white\"\n"
                       "}\n"
                       "GradientStop {\n"
                       "position: 1\n"
                       "color: \"black\"\n"
                       "}\n"
                       "}\n"
                       "}\n"
                       "Text {\n"
                       "text: \"text\"\n"
                       "x: 66\n"
                       "y: 43\n"
                       "width: 80\n"
                       "height: 20\n"
                       "id: text2\n"
                       "}\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setCheckSemanticErrors(true);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());

    while (testRewriterView->hasIncompleteTypeInformation()) {
        QApplication::processEvents(QEventLoop::AllEvents, 1000);
    }

    QVERIFY(testRewriterView->errors().isEmpty());

    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rootModelNode.majorVersion(), 2);

    QCOMPARE(rootModelNode.id(), QString("root"));
    QCOMPARE(rootModelNode.variantProperty("width").value().toInt(), 200);
    QCOMPARE(rootModelNode.variantProperty("height").value().toInt(), 200);
    QVERIFY(rootModelNode.hasProperty("data"));
    QVERIFY(rootModelNode.property("data").isNodeListProperty());
    QVERIFY(!rootModelNode.nodeListProperty("data").toModelNodeList().isEmpty());
    QCOMPARE(rootModelNode.nodeListProperty("data").toModelNodeList().count(), 3);
    ModelNode textNode1 = rootModelNode.nodeListProperty("data").toModelNodeList().first();
    QVERIFY(textNode1.isValid());
    QCOMPARE(textNode1.type(), QmlDesigner::TypeName("QtQuick.Text"));
    QCOMPARE(textNode1.id(), QString("text1"));
    QCOMPARE(textNode1.variantProperty("text").value().toString(), QString("Hello World"));
    QVERIFY(textNode1.hasProperty("anchors.centerIn"));
    QVERIFY(textNode1.property("anchors.centerIn").isBindingProperty());
    QCOMPARE(textNode1.bindingProperty("anchors.centerIn").expression(), QString("parent"));
    QVERIFY(textNode1.hasProperty("data"));
    QVERIFY(textNode1.property("data").isNodeListProperty());
    QVERIFY(!textNode1.nodeListProperty("data").toModelNodeList().isEmpty());
    ModelNode itemNode = textNode1.nodeListProperty("data").toModelNodeList().first();
    QVERIFY(itemNode.isValid());
    QCOMPARE(itemNode.id(), QString("item01"));
    QVERIFY(!itemNode.hasProperty("data"));

    ModelNode rectNode = rootModelNode.nodeListProperty("data").toModelNodeList().at(1);
    QVERIFY(rectNode.isValid());
    QCOMPARE(rectNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rectNode.id(), QString("rectangle"));
    QVERIFY(rectNode.hasProperty("gradient"));
    QVERIFY(rectNode.property("gradient").isNodeProperty());
    ModelNode gradientNode = rectNode.nodeProperty("gradient").modelNode();
    QVERIFY(gradientNode.isValid());
    QCOMPARE(gradientNode.type(), QmlDesigner::TypeName("QtQuick.Gradient"));
    QVERIFY(gradientNode.hasProperty("stops"));
    QVERIFY(gradientNode.property("stops").isNodeListProperty());
    QCOMPARE(gradientNode.nodeListProperty("stops").toModelNodeList().count(), 2);

    ModelNode stop1 = gradientNode.nodeListProperty("stops").toModelNodeList().first();
    ModelNode stop2 = gradientNode.nodeListProperty("stops").toModelNodeList().last();

    QVERIFY(stop1.isValid());
    QVERIFY(stop2.isValid());

    QCOMPARE(stop1.type(), QmlDesigner::TypeName("QtQuick.GradientStop"));
    QCOMPARE(stop2.type(), QmlDesigner::TypeName("QtQuick.GradientStop"));

    QCOMPARE(stop1.variantProperty("position").value().toInt(), 0);
    QCOMPARE(stop2.variantProperty("position").value().toInt(), 1);

    ModelNode textNode2 = rootModelNode.nodeListProperty("data").toModelNodeList().last();
    QVERIFY(textNode2.isValid());
    QCOMPARE(textNode2.type(), QmlDesigner::TypeName("QtQuick.Text"));
    QCOMPARE(textNode2.id(), QString("text2"));
    QCOMPARE(textNode2.variantProperty("width").value().toInt(), 80);
    QCOMPARE(textNode2.variantProperty("height").value().toInt(), 20);
    QCOMPARE(textNode2.variantProperty("x").value().toInt(), 66);
    QCOMPARE(textNode2.variantProperty("y").value().toInt(), 43);
    QCOMPARE(textNode2.variantProperty("text").value().toString(), QString("text"));
}

void tst_TestCore::testMetaInfo()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    // test whether default type is registered
    QVERIFY(model->metaInfo("QtQuick.Item", -1, -1).isValid());

    // test whether non-qml type is registered
    QVERIFY(model->hasNodeMetaInfo("<cpp>.QObject", -1, -1));
}

void tst_TestCore::testMetaInfoSimpleType()
{
    auto model(createModel("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Item", 2, 1));
    QVERIFY(model->hasNodeMetaInfo("QtQuick.Item", 2, 1));

    NodeMetaInfo itemMetaInfo = model->metaInfo("QtQuick.Item", 2, 1);

    QVERIFY(itemMetaInfo.isValid());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(itemMetaInfo.typeName(), QmlDesigner::TypeName("QtQuick.Item"));
    QCOMPARE(itemMetaInfo.majorVersion(), 2);
    QCOMPARE(itemMetaInfo.minorVersion(), 1);
#endif

    // super classes
    NodeMetaInfo qobject = itemMetaInfo.prototypes()[1];
    QVERIFY(qobject.isValid());
    QVERIFY(qobject.isQtObject());

    QCOMPARE(itemMetaInfo.prototypes().size(), 2); // Item, QtQuick.QtObject
    QVERIFY(itemMetaInfo.isQtQuickItem());
    QVERIFY(itemMetaInfo.isQtObject());
}

void tst_TestCore::testMetaInfoUncreatableType()
{
    auto model(createModel("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Animation"));
    NodeMetaInfo animationTypeInfo = model->metaInfo("QtQuick.Animation", 2, 1);
    QVERIFY(animationTypeInfo.isValid());

    QVERIFY(animationTypeInfo.isValid());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(animationTypeInfo.typeName(), QmlDesigner::TypeName("QtQuick.Animation"));
    QCOMPARE(animationTypeInfo.majorVersion(), 2);
    QCOMPARE(animationTypeInfo.minorVersion(), 1);
#endif

    NodeMetaInfo qObjectTypeInfo = animationTypeInfo.prototypes()[1];
    QVERIFY(qObjectTypeInfo.isValid());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(qObjectTypeInfo.simplifiedTypeName(), QmlDesigner::TypeName("QtObject"));
#endif

    QCOMPARE(animationTypeInfo.prototypes().size(), 2);
}

void tst_TestCore::testMetaInfoExtendedType()
{
    auto model(createModel("QtQuick.Item"));
    QVERIFY(model.get());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Text"));
    NodeMetaInfo typeInfo = model->metaInfo("QtQuick.Text", 2, 1);
    QVERIFY(typeInfo.isValid());
    QVERIFY(typeInfo.hasProperty("font")); // from QGraphicsWidget
    QVERIFY(typeInfo.hasProperty("enabled")); // from QGraphicsItem

    NodeMetaInfo graphicsObjectTypeInfo = typeInfo.prototypes()[1];
    QVERIFY(graphicsObjectTypeInfo.isValid());
}

void tst_TestCore::testMetaInfoInterface()
{
    // Test type registered with qmlRegisterInterface
    //
}

void tst_TestCore::testMetaInfoCustomType()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.PropertyChanges"));
    NodeMetaInfo propertyChangesInfo = model->metaInfo("QtQuick.PropertyChanges", -1, -1);
    QVERIFY(propertyChangesInfo.isValid());
    QVERIFY(propertyChangesInfo.hasProperty("target"));
    QVERIFY(propertyChangesInfo.hasProperty("restoreEntryValues"));
    QVERIFY(propertyChangesInfo.hasProperty("explicit"));

    NodeMetaInfo stateOperationInfo = propertyChangesInfo.prototypes()[1];
    QVERIFY(stateOperationInfo.isValid());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(stateOperationInfo.typeName(), QmlDesigner::TypeName("QtQuick.QQuickStateOperation"));
    QCOMPARE(stateOperationInfo.majorVersion(), -1);
    QCOMPARE(stateOperationInfo.minorVersion(), -1);
#endif
    QCOMPARE(propertyChangesInfo.prototypes().size(), 3);

    // DeclarativePropertyChanges just has 3 properties
    QCOMPARE(propertyChangesInfo.properties().size() - stateOperationInfo.properties().size(), 3);

    QApplication::processEvents();
}

void tst_TestCore::testMetaInfoEnums()
{
    auto model(createModel("QtQuick.Text"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(view->rootModelNode().metaInfo().typeName(), QmlDesigner::TypeName("QtQuick.Text"));
#endif

    QVERIFY(view->rootModelNode().metaInfo().hasProperty("transformOrigin"));

    QVERIFY(view->rootModelNode().metaInfo().property("transformOrigin").isEnumType());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(view->rootModelNode()
                 .metaInfo()
                 .property("transformOrigin")
                 .propertyType()
                 .simplifiedTypeName(),
             QmlDesigner::TypeName("TransformOrigin"));
#endif

    QVERIFY(view->rootModelNode().metaInfo().property("horizontalAlignment").isEnumType());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(view->rootModelNode()
                 .metaInfo()
                 .property("horizontalAlignment")
                 .propertyType()
                 .simplifiedTypeName(),
             QmlDesigner::TypeName("HAlignment"));
#endif

    QApplication::processEvents();
}

void tst_TestCore::testMetaInfoQtQuickVersion2()
{
    char qmlString[] = "import QtQuick 2.0\n"
                       "Rectangle {\n"
                            "id: root;\n"
                            "width: 200;\n"
                            "height: 200;\n"
                            "color: \"white\";\n"
                            "Text {\n"
                                "id: text1\n"
                                "text: \"Hello World\"\n"
                                "anchors.centerIn: parent\n"
                                "Item {\n"
                                    "id: item01\n"
                                 "}\n"
                            "}\n"
                            "Rectangle {\n"
                                "id: rectangle;\n"
                                "gradient: Gradient {\n"
                                    "GradientStop {\n"
                                        "position: 0\n"
                                        "color: \"white\"\n"
                                     "}\n"
                                     "GradientStop {\n"
                                        "position: 1\n"
                                        "color: \"black\"\n"
                                     "}\n"
                                "}\n"
                            "}\n"
                             "Text {\n"
                                "text: \"text\"\n"
                                "x: 66\n"
                                "y: 43\n"
                                "width: 80\n"
                                "height: 20\n"
                                "id: text2\n"
                            "}\n"
                       "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);

    model->attachView(testRewriterView.get());

    ModelNode rootModelNode = testRewriterView->rootModelNode();
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QVERIFY(model->metaInfo("Rectangle", -1, -1).isValid());
    QVERIFY(model->metaInfo("Rectangle", 2, 0).isValid());

    QVERIFY(model->metaInfo("QtQuick.Rectangle", -1, -1).isValid());
    QVERIFY(model->metaInfo("QtQuick.Rectangle", 2, 0).isValid());
}

void tst_TestCore::testMetaInfoProperties()
{
    auto model(createModel("QtQuick.Text"));
    QVERIFY(model.get());

    NodeMetaInfo textNodeMetaInfo = model->metaInfo("QtQuick.TextEdit", 2, 1);
    QVERIFY(textNodeMetaInfo.hasProperty("text"));
    QVERIFY(textNodeMetaInfo.hasProperty("parent"));
    QVERIFY(textNodeMetaInfo.hasProperty("x"));
    QVERIFY(textNodeMetaInfo.hasProperty("objectName")); // QtQuick.QObject
    QVERIFY(!textNodeMetaInfo.hasProperty("bla"));

    QVERIFY(textNodeMetaInfo.property("text").isWritable());
    QVERIFY(textNodeMetaInfo.property("x").isWritable());

    QApplication::processEvents();
}

void tst_TestCore::testMetaInfoDotProperties()
{
    auto model(createModel("QtQuick.Text", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Text"));

    QVERIFY(model->metaInfo("QtQuick.Rectangle").hasProperty("border"));
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(model->metaInfo("QtQuick.Rectangle").property("border").propertyType().typeName(),
             QmlDesigner::TypeName("<cpp>.QQuickPen"));

    QCOMPARE(view->rootModelNode().metaInfo().typeName(), QmlDesigner::TypeName("QtQuick.Text"));
#endif
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("font"));

    QVERIFY(view->rootModelNode().metaInfo().hasProperty("font.bold"));
    QVERIFY(contains(view->rootModelNode().metaInfo().properties(), "font.bold"));
    QVERIFY(contains(view->rootModelNode().metaInfo().properties(), "font.pointSize"));
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("font.pointSize"));

    ModelNode rectNode(addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data"));

    QVERIFY(contains(rectNode.metaInfo().properties(), "implicitHeight"));
    QVERIFY(contains(rectNode.metaInfo().properties(), "implicitWidth"));
    QVERIFY(contains(rectNode.metaInfo().properties(), "anchors.topMargin"));
    QVERIFY(contains(rectNode.metaInfo().properties(), "border.width"));
    QVERIFY(rectNode.metaInfo().hasProperty("border"));
    QVERIFY(rectNode.metaInfo().hasProperty("border.width"));

    QApplication::processEvents();
}

void tst_TestCore::testMetaInfoListProperties()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    QVERIFY(model->hasNodeMetaInfo("QtQuick.Item"));
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(view->rootModelNode().metaInfo().typeName(), QmlDesigner::TypeName("QtQuick.Item"));
#endif

    QVERIFY(view->rootModelNode().metaInfo().hasProperty("states"));
    QVERIFY(view->rootModelNode().metaInfo().property("states").isListProperty());
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("children"));
    QVERIFY(view->rootModelNode().metaInfo().property("children").isListProperty());
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("data"));
    QVERIFY(view->rootModelNode().metaInfo().property("data").isListProperty());
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("resources"));
    QVERIFY(view->rootModelNode().metaInfo().property("resources").isListProperty());
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("transitions"));
    QVERIFY(view->rootModelNode().metaInfo().property("transitions").isListProperty());
    QVERIFY(view->rootModelNode().metaInfo().hasProperty("transform"));
    QVERIFY(view->rootModelNode().metaInfo().property("transform").isListProperty());

    QVERIFY(view->rootModelNode().metaInfo().hasProperty("parent"));
    QVERIFY(!view->rootModelNode().metaInfo().property("parent").isListProperty());

    QApplication::processEvents();
}

void tst_TestCore::testQtQuick20Basic()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText("\nimport QtQuick 2.0\n\nItem {\n}\n");
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());
    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(rootModelNode.metaInfo().majorVersion(), 2);
    QCOMPARE(rootModelNode.metaInfo().minorVersion(), 0);
    //QCOMPARE(rootModelNode.majorQtQuickVersion(), 2);
    QCOMPARE(rootModelNode.majorVersion(), 2);
#endif
}

void tst_TestCore::testQtQuick20BasicRectangle()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText("\nimport QtQuick 2.0\nRectangle {\n}\n");
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QTest::qSleep(1000);
    QApplication::processEvents();

    QVERIFY(testRewriterView->errors().isEmpty());
    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());
#ifndef QDS_USE_PROJECTSTORAGE
    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rootModelNode.metaInfo().majorVersion(), 2);
    QCOMPARE(rootModelNode.metaInfo().minorVersion(), 0);
    //QCOMPARE(rootModelNode.majorQtQuickVersion(), 2);
    QCOMPARE(rootModelNode.majorVersion(), 2);
#endif
}

void tst_TestCore::testQtQuickControls2()
{

    const char* qmlString
            =   "import QtQuick 2.7\n"
                "import QtQuick.Controls 2.0\n"
                "import QtQuick.Layouts 1.0\n"
                "\n"
                "ApplicationWindow {\n"
                     "visible: true\n"
                     "width: 640\n"
                     "height: 480\n"
                     "title: qsTr(\"Hello World\")\n"
                    "Button {\n"
                    "}\n"
                    "Layout {\n"
                    "}\n"
                "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode(view->rootModelNode());

    QVERIFY(rootModelNode.isValid());

    QVERIFY(rootModelNode.metaInfo().isGraphicalItem());
    QVERIFY(rootModelNode.metaInfo().isQtQuickWindowWindow());

    QVERIFY(!contains(rootModelNode.metaInfo().localProperties(), "visible"));
    QVERIFY(contains(rootModelNode.metaInfo().properties(), "visible"));

    QVERIFY(!rootModelNode.allSubModelNodes().isEmpty());
    ModelNode button = rootModelNode.allSubModelNodes().first();
    QVERIFY(button.isValid());
    QVERIFY(button.metaInfo().isValid());
    QVERIFY(button.metaInfo().isGraphicalItem());
    QVERIFY(button.metaInfo().isQtQuickItem());

    QCOMPARE(rootModelNode.allSubModelNodes().count(), 2);
    ModelNode layout = rootModelNode.allSubModelNodes().last();
    QVERIFY(layout.isValid());
    QVERIFY(layout.metaInfo().isValid());
    QVERIFY(layout.metaInfo().isGraphicalItem());
    QVERIFY(layout.metaInfo().isQtQuickLayoutsLayout());
    QVERIFY(layout.metaInfo().isQtQuickItem());
}

void tst_TestCore::testImplicitComponents()
{
    const char* qmlString
            =   "import QtQuick 2.0\n"
                "import QtQuick.Controls 1.2\n"
                "Item {\n"
                    "ListView {\n"
                    "id: listView\n"
                    "x: 200\n"
                    "y: 100\n"
                    "width: 110\n"
                    "height: 160\n"
                    "delegate: Item {\n"
                        "x: 5\n"
                        "width: 80\n"
                        "height: 40\n"
                        "Row {\n"
                            "id: row1\n"
                            "spacing: 10\n"
                            "Rectangle {\n"
                                "width: 40\n"
                                "height: 40\n"
                                "color: colorCode\n"
                            "}\n"
                            "Text {\n"
                                "text: name\n"
                                "font.bold: true\n"
                                "anchors.verticalCenter: parent.verticalCenter\n"
                            "}\n"
                        "}\n"
                    "}\n"
                    "model: ListModel {\n"
                        "ListElement {\n"
                            "name: \"Grey\"\n"
                            "colorCode: \"grey\"\n"
                        "}\n"
                        "ListElement {\n"
                            "name: \"Red\"\n"
                            "colorCode: \"red\"\n"
                        "}\n"
                        "ListElement {\n"
                            "name: \"Blue\"\n"
                            "colorCode: \"blue\"\n"
                        "}\n"
                        "ListElement {\n"
                            "name: \"Green\"\n"
                            "colorCode: \"green\"\n"
                        "}\n"
                    "}\n"
                "}\n"
            "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode(view->rootModelNode());

    QVERIFY(rootModelNode.isValid());

    ModelNode listView = rootModelNode.directSubModelNodes().first();

    QVERIFY(listView.isValid());
    QCOMPARE(listView.id(), QLatin1String("listView"));

    NodeProperty delegateProperty = listView.nodeProperty("delegate");

    QVERIFY(delegateProperty.isValid());

    ModelNode delegate = delegateProperty.modelNode();

    QVERIFY(delegate.isValid());

    QVERIFY(delegate.isComponent()); //The delegate is an implicit component
    QCOMPARE(delegate.nodeSourceType(), ModelNode::NodeWithComponentSource);
}

void tst_TestCore::testRevisionedProperties()
{
    const char* qmlString
            =   "import QtQuick 2.12\n"
                "import QtQuick.Controls 2.0\n"
                "import QtQuick.Layouts 1.0\n"
                "\n"
                "Item {\n"
                     "width: 640\n"
                     "height: 480\n"
                     "Rectangle {\n"
                       "gradient: Gradient {\n"
                       "orientation: Qt.Vertical\n"
                       "}\n"
                     "}\n"
                     "TextEdit {\n"
                       "leftPadding: 10\n"
                       "rightPadding: 10\n"
                       "topPadding: 10\n"
                       "bottomPadding: 10\n"
                     "}\n"
                "}\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QLatin1String(qmlString));
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setCheckSemanticErrors(true);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode(view->rootModelNode());

    QVERIFY(rootModelNode.isValid());

    NodeMetaInfo metaInfo12 = model->metaInfo("QtQuick.Gradient", 2, 12);
    NodeMetaInfo metaInfo11 = model->metaInfo("QtQuick.Gradient", -1, -1);
    NodeMetaInfo metaInfoU = model->metaInfo("QtQuick.Gradient", -1, -1);

    QVERIFY(metaInfo12.isValid());
    QVERIFY(metaInfoU.isValid());

    QVERIFY(metaInfo12.hasProperty("orientation"));
    QVERIFY(metaInfoU.hasProperty("orientation"));
}

void tst_TestCore::testStatesRewriter()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText("import QtQuick 2.1; Item {}\n");
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode(view->rootModelNode());

    QVERIFY(rootModelNode.isValid());

    qDebug() << rootModelNode.metaInfo().isQtQuickItem();

    QVERIFY(QmlItemNode(rootModelNode).isValid());

    QVERIFY(QmlItemNode(rootModelNode).states().allStates().isEmpty());

    QmlModelState state1 = QmlItemNode(rootModelNode).states().addState("state 1");
    QVERIFY(state1.isValid());
    QVERIFY(!QmlItemNode(rootModelNode).states().allStates().isEmpty());
    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 1);

    QmlModelState state2 = QmlItemNode(rootModelNode).states().addState("state 2");
    QVERIFY(state2.isValid());

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 2);

    QmlItemNode(rootModelNode).states().removeState("state 1");
    QVERIFY(!state1.isValid());

    QmlItemNode(rootModelNode).states().removeState("state 2");
    QVERIFY(!state2.isValid());

    QVERIFY(QmlItemNode(rootModelNode).states().allStates().isEmpty());

}


void tst_TestCore::testGradientsRewriter()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText("\nimport QtQuick 2.0\n\nItem {\n}\n");
    NotIndentingTextEditModifier modifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&modifier);
    model->attachView(testRewriterView.get());

    QVERIFY(testRewriterView->errors().isEmpty());

    ModelNode rootModelNode(view->rootModelNode());

    QVERIFY(rootModelNode.isValid());

    ModelNode rectNode(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));

    const QLatin1String expected1("\nimport QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "Rectangle {\n"
                                  "}\n"
                                  "}\n");
    QCOMPARE(textEdit.toPlainText(), expected1);

    ModelNode gradientNode(addNodeChild(rectNode, "QtQuick.Gradient", 2, 0, "gradient"));

    QVERIFY(rectNode.hasNodeProperty("gradient"));

    const QLatin1String expected2("\nimport QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "Rectangle {\n"
                                  "gradient: Gradient {\n"
                                  "}\n"
                                  "}\n"
                                  "}\n");
    QCOMPARE(textEdit.toPlainText(), expected2);

    NodeListProperty stops(gradientNode.nodeListProperty("stops"));

    QmlDesigner::PropertyListType propertyList;

    propertyList.append(qMakePair(QmlDesigner::PropertyName("position"), QVariant::fromValue(0)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("color"), QVariant::fromValue(QColor(Qt::red))));

    ModelNode gradientStop1(gradientNode.view()->createModelNode("QtQuick.GradientStop", 2, 0, propertyList));
    QVERIFY(gradientStop1.isValid());
    stops.reparentHere(gradientStop1);

    const QLatin1String expected3("\nimport QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "Rectangle {\n"
                                  "gradient: Gradient {\n"
                                  "GradientStop {\n"
                                  "position: 0\n"
                                  "color: \"#ff0000\"\n"
                                  "}\n"
                                  "}\n"
                                  "}\n"
                                  "}\n");

    QCOMPARE(textEdit.toPlainText(), expected3);

    propertyList.clear();
    propertyList.append(qMakePair(QmlDesigner::PropertyName("position"), QVariant::fromValue(0.5)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("color"), QVariant::fromValue(QColor(Qt::blue))));

    ModelNode gradientStop2(gradientNode.view()->createModelNode("QtQuick.GradientStop", 2, 0, propertyList));
    QVERIFY(gradientStop2.isValid());
    stops.reparentHere(gradientStop2);

    const QLatin1String expected4("\nimport QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "Rectangle {\n"
                                  "gradient: Gradient {\n"
                                  "GradientStop {\n"
                                  "position: 0\n"
                                  "color: \"#ff0000\"\n"
                                  "}\n"
                                  "\n"
                                  "GradientStop {\n"
                                  "position: 0.5\n"
                                  "color: \"#0000ff\"\n"
                                  "}\n"
                                  "}\n"
                                  "}\n"
                                  "}\n");

    QCOMPARE(textEdit.toPlainText(), expected4);

    propertyList.clear();
    propertyList.append(qMakePair(QmlDesigner::PropertyName("position"), QVariant::fromValue(0.8)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("color"), QVariant::fromValue(QColor(Qt::yellow))));

    ModelNode gradientStop3(gradientNode.view()->createModelNode("QtQuick.GradientStop", 2, 0, propertyList));
    QVERIFY(gradientStop3.isValid());
    stops.reparentHere(gradientStop3);

    const QLatin1String expected5("\nimport QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "Rectangle {\n"
                                  "gradient: Gradient {\n"
                                  "GradientStop {\n"
                                  "    position: 0\n"
                                  "    color: \"#ff0000\"\n"
                                  "}\n"
                                  "\n"
                                  "GradientStop {\n"
                                  "    position: 0.5\n"
                                  "    color: \"#0000ff\"\n"
                                  "}\n"
                                  "\n"
                                  "GradientStop {\n"
                                  "    position: 0.8\n"
                                  "    color: \"#ffff00\"\n"
                                  "}\n"
                                  "}\n"
                                  "}\n"
                                  "}\n");

    QCOMPARE(stripWhiteSpaces(textEdit.toPlainText()), stripWhiteSpaces(expected5));

    gradientNode.removeProperty("stops");

    const QLatin1String expected6("\nimport QtQuick 2.0\n"
                                  "\n"
                                  "Item {\n"
                                  "Rectangle {\n"
                                  "gradient: Gradient {\n"
                                  "}\n"
                                  "}\n"
                                  "}\n");

    QCOMPARE(textEdit.toPlainText(), expected6);

    QVERIFY(!gradientNode.hasProperty("stops"));

    propertyList.clear();
    propertyList.append(qMakePair(QmlDesigner::PropertyName("position"), QVariant::fromValue(0)));
    propertyList.append(qMakePair(QmlDesigner::PropertyName("color"), QVariant::fromValue(QColor(Qt::blue))));

    gradientStop1 = gradientNode.view()->createModelNode("QtQuick.GradientStop", 2, 0, propertyList);
    QVERIFY(gradientStop1.isValid());

    stops.reparentHere(gradientStop1);
}

void tst_TestCore::testQmlModelStates()
{
    auto model(createModel("QtQuick.Item"));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());



    ModelNode rootModelNode(view->rootModelNode());

    QVERIFY(rootModelNode.isValid());

    QVERIFY(QmlItemNode(rootModelNode).isValid());

    QVERIFY(QmlItemNode(rootModelNode).states().allStates().isEmpty());

    QmlModelState newState = QmlItemNode(rootModelNode).states().addState("testState");
    QVERIFY(newState.isValid());
    QVERIFY(!QmlItemNode(rootModelNode).states().allStates().isEmpty());
    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 1);

    QCOMPARE(newState.name(), QString("testState"));
    QCOMPARE(newState, QmlItemNode(rootModelNode).states().state("testState"));

    QmlItemNode(rootModelNode).states().removeState("testState");
    QVERIFY(!newState.isValid());
    QVERIFY(QmlItemNode(rootModelNode).states().allStates().isEmpty());
}

void tst_TestCore::testStatesWasInstances()
{
//    import QtQuick 2.0
//
//    Rectangle {
//      Text {
//        id: targetObject
//        text: "base state"
//      }
//
//      states: [
//        State {
//          name: "state1"
//          PropertyChanges {
//            target: targetObj
//            x: 10
//            text: "state1"
//          }
//        }
//        State {
//          name: "state2"
//          PropertyChanges {
//            target: targetObj
//            text: "state2"
//          }
//        }
//      ]
//    }
//

    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    //
    // build up model
    //
    ModelNode rootNode = view->rootModelNode();

    ModelNode textNode = view->createModelNode("QtQuick.Text", 2, 0);
    textNode.setIdWithoutRefactoring("targetObject");
    textNode.variantProperty("text").setValue("base state");

    rootNode.nodeListProperty("data").reparentHere(textNode);

    ModelNode propertyChanges1Node = view->createModelNode("QtQuick.PropertyChanges", 2, 0);
    propertyChanges1Node.bindingProperty("target").setExpression("targetObject");
    propertyChanges1Node.variantProperty("x").setValue(10);
    propertyChanges1Node.variantProperty("text").setValue("state1");

    ModelNode state1Node = view->createModelNode("QtQuick.State", 2, 0);
    state1Node.variantProperty("name").setValue("state1");
    state1Node.nodeListProperty("changes").reparentHere(propertyChanges1Node);

    rootNode.nodeListProperty("states").reparentHere(state1Node);

    ModelNode propertyChanges2Node = view->createModelNode("QtQuick.PropertyChanges", 2, 0);
    propertyChanges2Node.bindingProperty("target").setExpression("targetObject");
    propertyChanges2Node.variantProperty("text").setValue("state2");

    ModelNode state2Node = view->createModelNode("QtQuick.State", 2, 0);
    state2Node.variantProperty("name").setValue("state2");
    state2Node.nodeListProperty("changes").reparentHere(propertyChanges2Node);

    rootNode.nodeListProperty("states").reparentHere(state2Node);
}

void tst_TestCore::testStates()
{
//
//    import QtQuick 1.1
//
//    Rectangle {
//      Text {
//        id: targetObject
//        text: "base state"
//      }
//      states: [
//        State {
//          name: "state 1"
//          PropertyChanges {
//            target: targetObj
//            text: "state 1"
//          }
//        }
//      ]
//    }
//

//   auto model(createModel("QtQuick.Rectangle", 1, 1));
//    QVERIFY(model.get());
//    QScopedPointer<TestView> view(new TestView);
//    QVERIFY(view.data());
//    model->attachView(view.data());

//    // build up model
//    ModelNode rootNode = view->rootModelNode();

//    ModelNode textNode = view->createModelNode("QtQuick.Text", 1, 1);
//    textNode.setId("targetObject");
//    textNode.variantProperty("text").setValue("base state");

//    rootNode.nodeListProperty("data").reparentHere(textNode);

//    QVERIFY(rootNode.isValid());
//    QVERIFY(textNode.isValid());

//    QmlItemNode rootItem(rootNode);
//    QmlItemNode textItem(textNode);

//    QVERIFY(rootItem.isValid());
//    QVERIFY(textItem.isValid());

//    QmlModelState state1(rootItem.states().addState("state 1")); //add state "state 1"
//    QCOMPARE(state1.modelNode().type(), QString("QtQuick.State"));

//    QVERIFY(view->currentState().isBaseState());

//    NodeInstance textInstance = view->nodeInstanceView()->instanceForNode(textNode);
//    QVERIFY(textInstance.isValid());

//    NodeInstance state1Instance = view->nodeInstanceView()->instanceForNode(state1);
//    QVERIFY(state1Instance.isValid());
//    QCOMPARE(state1Instance == view->nodeInstanceView()->activeStateInstance(), false);
//    QCOMPARE(state1Instance.property("name").toString(), QString("state 1"));

//    view->setCurrentState(state1); //set currentState "state 1"

//    QCOMPARE(view->currentState(), state1);

//    QCOMPARE(state1Instance == view->nodeInstanceView()->activeStateInstance(), true);

//    QVERIFY(!textItem.propertyAffectedByCurrentState("text"));

//    textItem.setVariantProperty("text", QVariant("state 1")); //set text in state !

//    QDeclarativeState *qmlState1 = const_cast<QDeclarativeState*>(qobject_cast<const QDeclarativeState*>(state1Instance.testHandle()));
//    QVERIFY(qmlState1);
//    QDeclarativeListProperty<QDeclarativeStateOperation> state1Changes = qmlState1->changes();
//    QCOMPARE(state1Changes.count(&state1Changes), 1);
//    QCOMPARE(state1Changes.at(&state1Changes, 0)->actions().size(), 1);

//    QmlPropertyChanges changes(state1.propertyChanges(textNode));
//    QVERIFY(changes.modelNode().hasProperty("text"));
//    QCOMPARE(changes.modelNode().variantProperty("text").value(), QVariant("state 1"));
//    QCOMPARE(changes.modelNode().bindingProperty("target").expression(), QString("targetObject"));
//    QCOMPARE(changes.target(), textNode);
//    QCOMPARE(changes.modelNode().type(), QString("QtQuick.PropertyChanges"));

//    QCOMPARE(changes.modelNode().parentProperty().name(), QString("changes"));
//    QCOMPARE(changes.modelNode().parentProperty().parentModelNode(), state1.modelNode());

//    QCOMPARE(state1Instance == view->nodeInstanceView()->activeStateInstance(), true);

//    QVERIFY(textItem.propertyAffectedByCurrentState("text"));

//    QCOMPARE(textInstance.property("text").toString(), QString("state 1"));
//    QCOMPARE(textItem.instanceValue("text"), QVariant("state 1"));

//    QVERIFY(textNode.hasProperty("text"));
//    QCOMPARE(textNode.variantProperty("text").value(), QVariant("base state"));
}

void tst_TestCore::testStatesBaseState()
{
//
//    import QtQuick 1.1
//
//    Rectangle {
//      Text {
//        id: targetObject
//        text: "base state"
//      }
//      states: [
//        State {
//          name: "state 1"
//          PropertyChanges {
//            target: targetObj
//            text: "state 1"
//          }
//        }
//      ]
//    }
//

    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    // build up model
    ModelNode rootNode = view->rootModelNode();

    ModelNode textNode = view->createModelNode("QtQuick.Text", 2, 0);
    textNode.setIdWithoutRefactoring("targetObject");
    textNode.variantProperty("text").setValue("base state");

    rootNode.nodeListProperty("data").reparentHere(textNode);

    QVERIFY(rootNode.isValid());
    QVERIFY(textNode.isValid());

    QmlItemNode rootItem(rootNode);
    QmlItemNode textItem(textNode);

    QSKIP("MetaInfo needs rewriter now ", SkipAll);

    QVERIFY(rootItem.isValid());
    QVERIFY(textItem.isValid());

    QmlModelState state1(rootItem.states().addState("state 1")); //add state "state 1"
    QCOMPARE(state1.modelNode().type(), QmlDesigner::TypeName("QtQuick.State"));

    QVERIFY(view->currentState().isBaseState());

    view->setCurrentState(state1); //set currentState "state 1"
    QCOMPARE(view->currentState(), state1);
    QApplication::processEvents();
    textItem.setVariantProperty("text", QVariant("state 1")); //set text in state !
    QVERIFY(textItem.propertyAffectedByCurrentState("text"));
    QApplication::processEvents();

    ModelNode newNode = view->createModelNode("QtQuick.Rectangle", 2, 0);
    QVERIFY(!QmlObjectNode(newNode).currentState().isBaseState());

    view->setCurrentState(view->baseState()); //set currentState base state
    QVERIFY(view->currentState().isBaseState());

    textNode.variantProperty("text").setValue("base state changed");

    view->setCurrentState(state1); //set currentState "state 1"
    QCOMPARE(view->currentState(), state1);
    QVERIFY(!view->currentState().isBaseState());

    view->setCurrentState(view->baseState()); //set currentState base state
    QVERIFY(view->currentState().isBaseState());
}

void tst_TestCore::testInstancesIdResolution()
{
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    //    import QtQuick 2.0
    //
    //    Rectangle {
    //      id: root
    //      width: 100
    //      height: 100
    //      Rectangle {
    //        id: item1
    //        width: root.width
    //        height: item2.height
    //      }
    //    }

    ModelNode rootNode = view->rootModelNode();
    rootNode.setIdWithoutRefactoring("root");
    rootNode.variantProperty("width").setValue(100);
    rootNode.variantProperty("height").setValue(100);

    ModelNode item1Node = view->createModelNode("QtQuick.Rectangle", 2, 0);
    item1Node.setIdWithoutRefactoring("item1");
    item1Node.bindingProperty("width").setExpression("root.width");
    item1Node.bindingProperty("height").setExpression("item2.height");

    rootNode.nodeListProperty("data").reparentHere(item1Node);

    //NodeInstance item1Instance = view->nodeInstanceView()->instanceForModelNode(item1Node);
    //QVERIFY(item1Instance.isValid());

    //QCOMPARE(item1Instance.property("width").toInt(), 100);
    //QCOMPARE(item1Instance.property("height").toInt(), 0); // item2 is still unknown

    // Add item2:
    //      Rectangle {
    //        id: item2
    //        width: root.width / 2
    //        height: root.height
    //      }

    ModelNode item2Node = view->createModelNode("QtQuick.Rectangle", 2, 0);
    item2Node.setIdWithoutRefactoring("item2");
    item2Node.bindingProperty("width").setExpression("root.width / 2");
    item2Node.bindingProperty("height").setExpression("root.height");

    rootNode.nodeListProperty("data").reparentHere(item2Node);

    //NodeInstance item2Instance = view->nodeInstanceView()->instanceForModelNode(item2Node);
    //QVERIFY(item2Instance.isValid());

    //QCOMPARE(item2Instance.property("width").toInt(), 50);
    //QCOMPARE(item2Instance.property("height").toInt(), 100);
    //QCOMPARE(item1Instance.property("height").toInt(), 100);

    // Remove item2 again
    item2Node.destroy();
    //QVERIFY(!item2Instance.isValid());
    //QVERIFY(item2Instance.instanceId() >= 0);

    // Add item3:
    //      Rectangle {
    //        id: item3
    //        height: 80
    //      }

    ModelNode item3Node = view->createModelNode("QtQuick.Rectangle", 2, 0);
    item3Node.setIdWithoutRefactoring("item3");
    item3Node.variantProperty("height").setValue(80);
    rootNode.nodeListProperty("data").reparentHere(item3Node);

    // Change item1.height: item3.height

    item1Node.bindingProperty("height").setExpression("item3.height");
    //QCOMPARE(item1Instance.property("height").toInt(), 80);
}

void tst_TestCore::testInstancesNotInScene()
{
    //
    // test whether deleting an instance which is not in the scene crashes
    //

    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode node1 = view->createModelNode("QtQuick.Item", 2, 0);
    node1.setIdWithoutRefactoring("node1");

    ModelNode node2 = view->createModelNode("QtQuick.Item", 2, 0);
    node2.setIdWithoutRefactoring("node2");

    node1.nodeListProperty("children").reparentHere(node2);

    node1.destroy();
}

void tst_TestCore::testInstancesBindingsInStatesStress()
{
    QSKIP("Instances are not working", SkipAll);
    //This is a stress test to provoke a crash
    for (int j = 0; j < 20; j++) {
        auto model(createModel("QtQuick.Rectangle", 2, 0));
        QVERIFY(model.get());
        QScopedPointer<TestView> view(new TestView);
        QVERIFY(view.data());
        model->attachView(view.data());

        ModelNode node1 = view->createModelNode("QtQuick.Item", 2, 0);
        node1.setIdWithoutRefactoring("node1");

        view->rootModelNode().nodeListProperty("children").reparentHere(node1);

        ModelNode node2 = view->createModelNode("QtQuick.Rectangle", 2, 0);
        node2.setIdWithoutRefactoring("node2");

        ModelNode node3 = view->createModelNode("QtQuick.Rectangle", 2, 0);
        node3.setIdWithoutRefactoring("node3");

        node1.nodeListProperty("children").reparentHere(node2);
        node1.nodeListProperty("children").reparentHere(node3);

        QmlItemNode(node1).states().addState("state1");
        QmlItemNode(node1).states().addState("state2");

        QmlItemNode(node1).setVariantProperty("x", "100");
        QmlItemNode(node1).setVariantProperty("y", "100");


        for (int i=0;i<4;i++) {
            view->setCurrentState(view->baseState());

            QmlItemNode(node2).setBindingProperty("x", "parent.x + 10");
            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 110);
            view->setCurrentState(QmlItemNode(node1).states().state("state1"));
            QmlItemNode(node2).setBindingProperty("x", "parent.x + 20");
            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 120);
            QmlItemNode(node2).setBindingProperty("y", "parent.x + 20");
            QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 120);

            view->setCurrentState(QmlItemNode(node1).states().state("state2"));
            QmlItemNode(node2).setBindingProperty("x", "parent.x + 30");
            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 130);
            QmlItemNode(node2).setBindingProperty("y", "parent.x + 30");
            QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 130);
            QmlItemNode(node2).setBindingProperty("height", "this.is.no.proper.expression / 12 + 4");
            QmlItemNode(node2).setVariantProperty("height", 0);


            for (int c=0;c<10;c++) {
                view->setCurrentState(QmlItemNode(node1).states().state("state1"));
                QmlItemNode(node2).setBindingProperty("x", "parent.x + 20");
                QmlItemNode(node2).setVariantProperty("x", "90");
                QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 90);
                QmlItemNode(node2).setBindingProperty("y", "parent.x + 20");
                QmlItemNode(node2).setVariantProperty("y", "90");
                view->setCurrentState(QmlItemNode(node1).states().state("state2"));
                view->setCurrentState(view->baseState());
                view->setCurrentState(QmlItemNode(node1).states().state("state1"));
                QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 90);
                QmlItemNode(node2).setBindingProperty("width", "parent.x + 30");
                QCOMPARE(QmlItemNode(node2).instanceValue("width").toInt(), 130);
                QmlItemNode(node2).setVariantProperty("width", "0");
                view->setCurrentState(QmlItemNode(node1).states().state("state2"));
                view->setCurrentState(view->baseState());
                QmlItemNode(node1).setVariantProperty("x", "80");
                QmlItemNode(node1).setVariantProperty("y", "80");
                QmlItemNode(node1).setVariantProperty("x", "100");
                QmlItemNode(node1).setVariantProperty("y", "100");
            }

            QmlItemNode(node3).setBindingProperty("width", "parent.x + 30");
            QVERIFY(QmlItemNode(node3).instanceHasBinding("width"));
            QCOMPARE(QmlItemNode(node3).instanceValue("width").toInt(), 130);

            view->setCurrentState(view->baseState());
            QmlItemNode(node1).setVariantProperty("x", "80");
            QmlItemNode(node1).setVariantProperty("y", "80");

            view->setCurrentState(QmlItemNode(node1).states().state("state2"));

            view->setCurrentState(QmlItemNode(node1).states().state("state1"));
            QmlItemNode(node3).setVariantProperty("width", "90");

            view->setCurrentState(QmlItemNode(node1).states().state(""));
            view->setCurrentState(view->baseState());
            QVERIFY(view->currentState().isBaseState());

            QmlItemNode(node1).setVariantProperty("x", "100");
            QmlItemNode(node1).setVariantProperty("y", "100");

            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 110);
            QmlItemNode(node2).setBindingProperty("x", "parent.x + 20");
            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 120);
            QmlItemNode(node2).setVariantProperty("x", "80");
            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 80);
            view->setCurrentState(QmlItemNode(node1).states().state("state1"));
            QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 90);
        }
    }
}

void tst_TestCore::testInstancesPropertyChangeTargets()
{
    QSKIP("Instances are not working", SkipAll);
    //this tests checks if a change of the target of a CropertyChange
    //node is handled correctly

    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode node1 = view->createModelNode("QtQuick.Item", 2, 0);
    node1.setIdWithoutRefactoring("node1");

    view->rootModelNode().nodeListProperty("children").reparentHere(node1);

    ModelNode node2 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    node2.setIdWithoutRefactoring("node2");

    ModelNode node3 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    node3.setIdWithoutRefactoring("node3");

    node1.nodeListProperty("children").reparentHere(node2);
    node1.nodeListProperty("children").reparentHere(node3);

    QmlItemNode(node1).states().addState("state1");
    QmlItemNode(node1).states().addState("state2");

    QmlItemNode(node1).setVariantProperty("x", 10);
    QmlItemNode(node1).setVariantProperty("y", 10);

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 10);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 10);

    QmlItemNode(node2).setVariantProperty("x", 50);
    QmlItemNode(node2).setVariantProperty("y", 50);

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 50);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 50);

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QmlItemNode(node1).setVariantProperty("x", 20);
    QmlItemNode(node1).setVariantProperty("y", 20);

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 20);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 20);

    view->setCurrentState(view->baseState());

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 10);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 10);

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 20);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 20);

    QmlPropertyChanges propertyChanges = QmlItemNode(node1).states().state("state1").propertyChanges(
        node1);
    QVERIFY(propertyChanges.isValid());
    QCOMPARE(propertyChanges.target().id(), QLatin1String("node1"));

    view->setCurrentState(view->baseState()); //atm we only support this in the base state

    propertyChanges.setTarget(node2);
    QCOMPARE(propertyChanges.target().id(), QLatin1String("node2"));

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 10);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 10);

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 20);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 20);

    QmlItemNode(node2).setBindingProperty("x", "node1.x + 20");
    QmlItemNode(node2).setBindingProperty("y", "node1.y + 20");

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 30);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 30);

    view->setCurrentState(view->baseState());

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 50);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 50);

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 30);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 30);

    view->setCurrentState(view->baseState()); //atm we only support this in the base state

    QCOMPARE(propertyChanges.target().id(), QLatin1String("node2"));

    propertyChanges.setTarget(node3);
    QCOMPARE(propertyChanges.target().id(), QLatin1String("node3"));

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    //node3 is now the target of the PropertyChanges with the bindings

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 50);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 50);

    QCOMPARE(QmlItemNode(node3).instanceValue("x").toInt(), 30);
    QCOMPARE(QmlItemNode(node3).instanceValue("y").toInt(), 30);
}

void tst_TestCore::testInstancesDeletePropertyChanges()
{
    QSKIP("Instances are not working", SkipAll);
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode node1 = view->createModelNode("QtQuick.Item", 2, 0);
    node1.setIdWithoutRefactoring("node1");

    view->rootModelNode().nodeListProperty("children").reparentHere(node1);

    ModelNode node2 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    node2.setIdWithoutRefactoring("node2");

    ModelNode node3 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    node3.setIdWithoutRefactoring("node3");

    node1.nodeListProperty("children").reparentHere(node2);
    node1.nodeListProperty("children").reparentHere(node3);

    QmlItemNode(node1).states().addState("state1");
    QmlItemNode(node1).states().addState("state2");

    QmlItemNode(node1).setVariantProperty("x", 10);
    QmlItemNode(node1).setVariantProperty("y", 10);

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 10);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 10);

    QmlItemNode(node2).setVariantProperty("x", 50);
    QmlItemNode(node2).setVariantProperty("y", 50);

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 50);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 50);

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QmlItemNode(node1).setVariantProperty("x", 20);
    QmlItemNode(node1).setVariantProperty("y", 20);

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 20);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 20);

    view->setCurrentState(view->baseState());

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 10);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 10);

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 20);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 20);

    QmlPropertyChanges propertyChanges = QmlItemNode(node1).states().state("state1").propertyChanges(
        node1);
    QVERIFY(propertyChanges.isValid());
    QCOMPARE(propertyChanges.target().id(), QLatin1String("node1"));

    view->setCurrentState(view->baseState()); //atm we only support this in the base state

    propertyChanges.modelNode().destroy();

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QCOMPARE(QmlItemNode(node1).instanceValue("x").toInt(), 10);
    QCOMPARE(QmlItemNode(node1).instanceValue("y").toInt(), 10);

    QmlItemNode(node2).setBindingProperty("x", "node1.x + 20");
    QmlItemNode(node2).setBindingProperty("y", "node1.y + 20");

    propertyChanges = QmlItemNode(node1).states().state("state1").propertyChanges(node2);
    QVERIFY(propertyChanges.isValid());
    QCOMPARE(propertyChanges.target().id(), QLatin1String("node2"));

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 30);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 30);

    view->setCurrentState(view->baseState()); //atm we only support this in the base state

    propertyChanges.modelNode().destroy();

    view->setCurrentState(QmlItemNode(node1).states().state("state1"));

    QCOMPARE(QmlItemNode(node2).instanceValue("x").toInt(), 50);
    QCOMPARE(QmlItemNode(node2).instanceValue("y").toInt(), 50);
}

void tst_TestCore::testInstancesChildrenLowLevel()
{
    QSKIP("Instances are not working", SkipAll);
//    QScopedPointer<Model> model(createModel("QtQuick.Rectangle", 1, 1));
//    QVERIFY(model.data());

//    QScopedPointer<NodeInstanceView> view(new NodeInstanceView);
//    QVERIFY(view.data());
//    model->attachView(view.data());

//    ModelNode rootModelNode(view->rootModelNode());
//    QVERIFY(rootModelNode.isValid());

//    rootModelNode.setId("rootModelNode");

//    ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Text", 1, 1, "data");
//    QVERIFY(childNode1.isValid());
//    childNode1.setId("childNode1");

//    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.TextEdit", 1, 1, "data");
//    QVERIFY(childNode2.isValid());
//    childNode2.setId("childNode2");

//    NodeInstance rootInstance = view->instanceForNode(rootModelNode);
//    QDeclarativeItem *rootItem = qobject_cast<QDeclarativeItem*>(rootInstance.testHandle());
//    QVERIFY(rootItem);
//    NodeInstance child1Instance = view->instanceForNode(childNode1);
//    QDeclarativeItem *child1Item = qobject_cast<QDeclarativeItem*>(child1Instance.testHandle());
//    QVERIFY(child1Item);
//    NodeInstance child2Instance = view->instanceForNode(childNode2);
//    QDeclarativeItem *child2Item = qobject_cast<QDeclarativeItem*>(child2Instance.testHandle());
//    QVERIFY(child2Item);

//     QDeclarativeContext *context = rootInstance.internalInstance()->context();
//     QDeclarativeEngine *engine = rootInstance.internalInstance()->engine();
//     QDeclarativeProperty childrenProperty(rootItem, "children", context);
//     QVERIFY(childrenProperty.isValid());

//     QDeclarativeListReference listReference(childrenProperty.object(), childrenProperty.name().toLatin1(), engine);
//     QVERIFY(listReference.isValid());

//     QVERIFY(listReference.canAppend());
//     QVERIFY(listReference.canAt());
//     QVERIFY(listReference.canClear());
//     QVERIFY(listReference.canCount());

//     QCOMPARE(listReference.count(), 2);

//     QCOMPARE(listReference.at(0), child1Item);
//     QCOMPARE(listReference.at(1), child2Item);

//     listReference.clear();

//     QCOMPARE(listReference.count(), 0);

//     listReference.append(child2Item);
//     listReference.append(child1Item);

//     QCOMPARE(listReference.at(0), child2Item);
//     QCOMPARE(listReference.at(1), child1Item);

//     QDeclarativeProperty dataProperty(rootItem, "data", context);
//     QDeclarativeListReference listReferenceData(dataProperty.object(), dataProperty.name().toLatin1(), engine);

//     QVERIFY(listReferenceData.canAppend());
//     QVERIFY(listReferenceData.canAt());
//     QVERIFY(listReferenceData.canClear());
//     QVERIFY(listReferenceData.canCount());

//     QCOMPARE(listReferenceData.count(), 2);

//     QCOMPARE(listReferenceData.at(0), child2Item);
//     QCOMPARE(listReferenceData.at(1), child1Item);

//     listReferenceData.clear();

//     QCOMPARE(listReference.count(), 0);
//     QCOMPARE(listReferenceData.count(), 0);

//     listReferenceData.append(child1Item);
//     listReferenceData.append(child2Item);

//     QCOMPARE(listReferenceData.count(), 2);

//     QCOMPARE(listReference.at(0), child1Item);
//     QCOMPARE(listReference.at(1), child2Item);

//     QCOMPARE(listReferenceData.at(0), child1Item);
//     QCOMPARE(listReferenceData.at(1), child2Item);
}

void tst_TestCore::testInstancesResourcesLowLevel()
{
    QSKIP("Instances are not working", SkipAll);
//    QScopedPointer<Model> model(createModel("QtQuick.Rectangle", 1, 1));
//    QVERIFY(model.data());

//    QScopedPointer<NodeInstanceView> view(new NodeInstanceView);
//    QVERIFY(view.data());
//    model->attachView(view.data());

//    ModelNode rootModelNode(view->rootModelNode());
//    QVERIFY(rootModelNode.isValid());

//    rootModelNode.setId("rootModelNode");

//    ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Text", 1, 1, "data");
//    QVERIFY(childNode1.isValid());
//    childNode1.setId("childNode1");

//    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.TextEdit", 1, 1, "data");
//    QVERIFY(childNode2.isValid());
//    childNode2.setId("childNode2");

//    ModelNode listModel = addNodeListChild(rootModelNode, "QtQuick.ListModel", 1, 1, "data");
//    QVERIFY(listModel.isValid());
//    listModel.setId("listModel");

//    NodeInstance rootInstance = view->instanceForNode(rootModelNode);
//    QDeclarativeItem *rootItem = qobject_cast<QDeclarativeItem*>(rootInstance.testHandle());
//    QVERIFY(rootItem);
//    NodeInstance child1Instance = view->instanceForNode(childNode1);
//    QDeclarativeItem *child1Item = qobject_cast<QDeclarativeItem*>(child1Instance.testHandle());
//    QVERIFY(child1Item);
//    NodeInstance child2Instance = view->instanceForNode(childNode2);
//    QDeclarativeItem *child2Item = qobject_cast<QDeclarativeItem*>(child2Instance.testHandle());
//    QVERIFY(child2Item);

//    NodeInstance listModelInstance = view->instanceForNode(listModel);
//    QObject *listModelObject = listModelInstance.testHandle();
//    QVERIFY(listModelObject);

//    QDeclarativeContext *context = rootInstance.internalInstance()->context();
//    QDeclarativeEngine *engine = rootInstance.internalInstance()->engine();
//    QDeclarativeProperty childrenProperty(rootItem, "children", context);
//    QDeclarativeProperty resourcesProperty(rootItem, "resources", context);
//    QDeclarativeProperty dataProperty(rootItem, "data", context);

//    QDeclarativeListReference listReferenceData(dataProperty.object(), dataProperty.name().toLatin1(), engine);
//    QDeclarativeListReference listReferenceChildren(childrenProperty.object(), childrenProperty.name().toLatin1(), engine);
//    QDeclarativeListReference listReferenceResources(resourcesProperty.object(), resourcesProperty.name().toLatin1(), engine);

//    QVERIFY(listReferenceData.isValid());
//    QVERIFY(listReferenceChildren.isValid());
//    QVERIFY(listReferenceResources.isValid());

//    QCOMPARE(listReferenceData.count(), 3);
//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 1);

//    QCOMPARE(listReferenceResources.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(0), listModelObject);

//    QSKIP("This crashes", SkipAll); //### todo might be critical in the future

//    listReferenceResources.clear();

//    QCOMPARE(listReferenceData.count(), 2);
//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 0);

//    listReferenceData.append(listModelObject);

//    QCOMPARE(listReferenceData.count(), 3);
//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 1);

//    QCOMPARE(listReferenceResources.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(1), child1Item);
//    QCOMPARE(listReferenceData.at(2), child2Item);

//    listReferenceChildren.clear();

//    QCOMPARE(listReferenceData.count(), 1);
//    QCOMPARE(listReferenceChildren.count(), 0);
//    QCOMPARE(listReferenceResources.count(), 1);

//    QCOMPARE(listReferenceResources.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(0), listModelObject);

//    listReferenceData.append(child1Item);
//    listReferenceData.append(child2Item);

//    QCOMPARE(listReferenceData.count(), 3);
//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 1);

//    QCOMPARE(listReferenceResources.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(1), child1Item);
//    QCOMPARE(listReferenceData.at(2), child2Item);

//    ModelNode listModel2 = addNodeListChild(rootModelNode, "QtQuick.ListModel", 1, 1, "data");
//    QVERIFY(listModel2.isValid());
//    listModel2.setId("listModel2");

//    NodeInstance listModelInstance2 = view->instanceForNode(listModel2);
//    QObject *listModelObject2 = listModelInstance2.testHandle();
//    QVERIFY(listModelObject2);

//    QCOMPARE(listReferenceData.count(), 4);
//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 2);

//    QCOMPARE(listReferenceResources.at(0), listModelObject);
//    QCOMPARE(listReferenceResources.at(1), listModelObject2);
//    QCOMPARE(listReferenceData.at(0), listModelObject);
//    QCOMPARE(listReferenceData.at(1), listModelObject2);
//    QCOMPARE(listReferenceData.at(2), child1Item);
//    QCOMPARE(listReferenceData.at(3), child2Item);

//    listReferenceResources.clear();

//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceData.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 0);

//    listReferenceResources.append(listModelObject2);
//    listReferenceResources.append(listModelObject);

//    QCOMPARE(listReferenceData.count(), 4);
//    QCOMPARE(listReferenceChildren.count(), 2);
//    QCOMPARE(listReferenceResources.count(), 2);

//    QCOMPARE(listReferenceResources.at(0), listModelObject2);
//    QCOMPARE(listReferenceResources.at(1), listModelObject);
//    QCOMPARE(listReferenceData.at(0), listModelObject2);
//    QCOMPARE(listReferenceData.at(1), listModelObject);
//    QCOMPARE(listReferenceData.at(2), child1Item);
//    QCOMPARE(listReferenceData.at(3), child2Item);

//    listReferenceData.clear();

//    QCOMPARE(listReferenceChildren.count(), 0);
//    QCOMPARE(listReferenceData.count(), 0);
//    QCOMPARE(listReferenceResources.count(), 0);
}

void tst_TestCore::testInstancesFlickableLowLevel()
{
    QSKIP("Instances are not working", SkipAll);
    //   auto model(createModel("QtQuick.Flickable", 1, 1));
    //    QVERIFY(model.get());

    //    QScopedPointer<NodeInstanceView> view(new NodeInstanceView);
    //    QVERIFY(view.data());
    //    model->attachView(view.data());

    //    ModelNode rootModelNode(view->rootModelNode());
    //    QVERIFY(rootModelNode.isValid());

    //    rootModelNode.setId("rootModelNode");

    //    ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Text", 1, 1, "flickableData");
    //    QVERIFY(childNode1.isValid());
    //    childNode1.setId("childNode1");

    //    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.TextEdit", 1, 1, "flickableData");
    //    QVERIFY(childNode2.isValid());
    //    childNode2.setId("childNode2");

    //    ModelNode listModel = addNodeListChild(rootModelNode, "QtQuick.ListModel", 1, 1, "flickableData");
    //    QVERIFY(listModel.isValid());
    //    listModel.setId("listModel");

    //    NodeInstance rootInstance = view->instanceForNode(rootModelNode);
    //    QDeclarativeItem *rootItem = qobject_cast<QDeclarativeItem*>(rootInstance.testHandle());
    //    QVERIFY(rootItem);
    //    NodeInstance child1Instance = view->instanceForNode(childNode1);
    //    QDeclarativeItem *child1Item = qobject_cast<QDeclarativeItem*>(child1Instance.testHandle());
    //    QVERIFY(child1Item);
    //    NodeInstance child2Instance = view->instanceForNode(childNode2);
    //    QDeclarativeItem *child2Item = qobject_cast<QDeclarativeItem*>(child2Instance.testHandle());
    //    QVERIFY(child2Item);

    //    NodeInstance listModelInstance = view->instanceForNode(listModel);
    //    QObject *listModelObject = listModelInstance.testHandle();
    //    QVERIFY(listModelObject);

    //    QDeclarativeContext *context = rootInstance.internalInstance()->context();
    //    QDeclarativeEngine *engine = rootInstance.internalInstance()->engine();
    //    QDeclarativeProperty flickableChildrenProperty(rootItem, "flickableChildren", context);
    //    QDeclarativeProperty flickableDataProperty(rootItem, "flickableData", context);
    //    QDeclarativeProperty dataProperty(rootItem, "data", context);
    //    QDeclarativeProperty resourcesProperty(rootItem, "resources", context);
    //    QDeclarativeProperty childrenProperty(rootItem, "children", context);

    //    QDeclarativeListReference listReferenceData(dataProperty.object(), dataProperty.name().toLatin1(), engine);
    //    QDeclarativeListReference listReferenceResources(resourcesProperty.object(), resourcesProperty.name().toLatin1(), engine);
    //    QDeclarativeListReference listReferenceChildren(childrenProperty.object(), childrenProperty.name().toLatin1(), engine);
    //    QDeclarativeListReference listReferenceFlickableData(flickableDataProperty.object(), flickableDataProperty.name().toLatin1(), engine);
    //    QDeclarativeListReference listReferenceFlickableChildren(flickableChildrenProperty.object(), flickableChildrenProperty.name().toLatin1(), engine);

    //    QVERIFY(listReferenceData.isValid());
    //    QVERIFY(listReferenceChildren.isValid());
    //    QVERIFY(listReferenceResources.isValid());
    //    QVERIFY(listReferenceFlickableChildren.isValid());
    //    QVERIFY(listReferenceFlickableData.isValid());

    //    QCOMPARE(listReferenceChildren.count(), 1);
    //    QCOMPARE(listReferenceFlickableChildren.count(), 2);
    //    QCOMPARE(listReferenceData.count(), listReferenceResources.count() + listReferenceChildren.count());
    //    QCOMPARE(listReferenceFlickableData.count(), listReferenceResources.count() + listReferenceFlickableChildren.count());
    //    int oldResourcesCount = listReferenceResources.count();

    //    QCOMPARE(listReferenceFlickableChildren.at(0), child1Item);
    //    QCOMPARE(listReferenceFlickableChildren.at(1), child2Item);

    //    listReferenceFlickableChildren.clear();

    //    QCOMPARE(listReferenceFlickableChildren.count(), 0);
    //    QCOMPARE(listReferenceResources.count(), oldResourcesCount);
    //    QCOMPARE(listReferenceData.count(), listReferenceResources.count() + listReferenceChildren.count());
    //    QCOMPARE(listReferenceFlickableData.count(), listReferenceResources.count() + listReferenceFlickableChildren.count());

    //    listReferenceFlickableChildren.append(child2Item);
    //    listReferenceFlickableChildren.append(child1Item);

    //    QCOMPARE(listReferenceFlickableChildren.count(), 2);
    //    QCOMPARE(listReferenceResources.count(), oldResourcesCount);
    //    QCOMPARE(listReferenceData.count(), listReferenceResources.count() + listReferenceChildren.count());
    //    QCOMPARE(listReferenceFlickableData.count(), listReferenceResources.count() + listReferenceFlickableChildren.count());

    //    QCOMPARE(listReferenceFlickableChildren.at(0), child2Item);
    //    QCOMPARE(listReferenceFlickableChildren.at(1), child1Item);
}

void tst_TestCore::testInstancesReorderChildrenLowLevel()
{
    QSKIP("Instances are not working", SkipAll);
//    QScopedPointer<Model> model(createModel("QtQuick.Rectangle", 1, 1));
//    QVERIFY(model.data());

//    QScopedPointer<NodeInstanceView> view(new NodeInstanceView);
//    QVERIFY(view.data());
//    model->attachView(view.data());

//    ModelNode rootModelNode(view->rootModelNode());
//    QVERIFY(rootModelNode.isValid());

//    rootModelNode.setId("rootModelNode");

//    ModelNode childNode1 = addNodeListChild(rootModelNode, "QtQuick.Text", 1, 1, "data");
//    QVERIFY(childNode1.isValid());
//    childNode1.setId("childNode1");

//    ModelNode childNode2 = addNodeListChild(rootModelNode, "QtQuick.TextEdit", 1, 1, "data");
//    QVERIFY(childNode2.isValid());
//    childNode2.setId("childNode2");

//    ModelNode listModel = addNodeListChild(rootModelNode, "QtQuick.ListModel", 1, 1, "data");
//    QVERIFY(listModel.isValid());
//    listModel.setId("listModel");

//    ModelNode childNode3 = addNodeListChild(rootModelNode, "QtQuick.TextEdit", 1, 1, "data");
//    QVERIFY(childNode3.isValid());
//    childNode3.setId("childNode3");

//    ModelNode childNode4 = addNodeListChild(rootModelNode, "QtQuick.TextEdit", 1, 1, "data");
//    QVERIFY(childNode4.isValid());
//    childNode4.setId("childNode4");

//    NodeInstance rootInstance = view->instanceForNode(rootModelNode);
//    QDeclarativeItem *rootItem = qobject_cast<QDeclarativeItem*>(rootInstance.testHandle());
//    QVERIFY(rootItem);
//    NodeInstance child1Instance = view->instanceForNode(childNode1);
//    QDeclarativeItem *child1Item = qobject_cast<QDeclarativeItem*>(child1Instance.testHandle());
//    QVERIFY(child1Item);
//    NodeInstance child2Instance = view->instanceForNode(childNode2);
//    QDeclarativeItem *child2Item = qobject_cast<QDeclarativeItem*>(child2Instance.testHandle());
//    QVERIFY(child2Item);
//    NodeInstance child3Instance = view->instanceForNode(childNode3);
//    QDeclarativeItem *child3Item = qobject_cast<QDeclarativeItem*>(child3Instance.testHandle());
//    QVERIFY(child3Item);
//    NodeInstance child4Instance = view->instanceForNode(childNode4);
//    QDeclarativeItem *child4Item = qobject_cast<QDeclarativeItem*>(child4Instance.testHandle());
//    QVERIFY(child4Item);

//    NodeInstance listModelInstance = view->instanceForNode(listModel);
//    QObject *listModelObject = listModelInstance.testHandle();
//    QVERIFY(listModelObject);

//    QDeclarativeContext *context = rootInstance.internalInstance()->context();
//    QDeclarativeEngine *engine = rootInstance.internalInstance()->engine();
//    QDeclarativeProperty childrenProperty(rootItem, "children", context);
//    QDeclarativeProperty resourcesProperty(rootItem, "resources", context);
//    QDeclarativeProperty dataProperty(rootItem, "data", context);

//    QDeclarativeListReference listReferenceData(dataProperty.object(), dataProperty.name().toLatin1(), engine);
//    QDeclarativeListReference listReferenceChildren(childrenProperty.object(), childrenProperty.name().toLatin1(), engine);
//    QDeclarativeListReference listReferenceResources(resourcesProperty.object(), resourcesProperty.name().toLatin1(), engine);

//    QVERIFY(listReferenceData.isValid());
//    QVERIFY(listReferenceChildren.isValid());
//    QVERIFY(listReferenceResources.isValid());

//    QCOMPARE(listReferenceResources.count(), 1);
//    QCOMPARE(listReferenceChildren.count(), 4);
//    QCOMPARE(listReferenceData.count(), 5);

//    QCOMPARE(listReferenceChildren.at(0), child1Item);
//    QCOMPARE(listReferenceChildren.at(1), child2Item);
//    QCOMPARE(listReferenceChildren.at(2), child3Item);
//    QCOMPARE(listReferenceChildren.at(3), child4Item);

//    listReferenceChildren.clear();

//    QCOMPARE(listReferenceResources.count(), 1);
//    QCOMPARE(listReferenceChildren.count(), 0);
//    QCOMPARE(listReferenceData.count(), 1);

//    listReferenceChildren.append(child4Item);
//    listReferenceChildren.append(child3Item);
//    listReferenceChildren.append(child2Item);
//    listReferenceChildren.append(child1Item);


//    QCOMPARE(listReferenceResources.count(), 1);
//    QCOMPARE(listReferenceChildren.count(), 4);
//    QCOMPARE(listReferenceData.count(), 5);

//    QCOMPARE(listReferenceChildren.at(0), child4Item);
//    QCOMPARE(listReferenceChildren.at(1), child3Item);
//    QCOMPARE(listReferenceChildren.at(2), child2Item);
//    QCOMPARE(listReferenceChildren.at(3), child1Item);
}

void tst_TestCore::testQmlModelStatesInvalidForRemovedNodes()
{
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QVERIFY(QmlItemNode(rootModelNode).isValid());

    rootModelNode.setIdWithoutRefactoring("rootModelNode");

    QmlModelState state1 = QmlItemNode(rootModelNode).states().addState("state1");
    QVERIFY(state1.isValid());
    QCOMPARE(state1.name(), QString("state1"));

    ModelNode childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(childNode.isValid());
    childNode.setIdWithoutRefactoring("childNode");

    ModelNode subChildNode = addNodeListChild(childNode, "QtQuick.Rectangle", 2, 0, "data");
    QVERIFY(subChildNode.isValid());
    subChildNode.setIdWithoutRefactoring("subChildNode");

    QVERIFY(!state1.hasPropertyChanges(subChildNode));
    QmlPropertyChanges changeSet = state1.propertyChanges(subChildNode);
    QVERIFY(changeSet.isValid());

    QmlItemNode(childNode).destroy();
    QVERIFY(!childNode.isValid());
    QVERIFY(!subChildNode.isValid());
    //QVERIFY(!changeSet.isValid()); update to QtQuick 2
}

void tst_TestCore::testInstancesAttachToExistingModel()
{
    //
    // Test attaching nodeinstanceview to an existing model
    //

    QSKIP("Instances are not working", SkipAll);

    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootNode = view->rootModelNode();
    ModelNode rectangleNode = addNodeListChild(rootNode, "QtQuick.Rectangle", 2, 0, "data");

    rectangleNode.variantProperty("width").setValue(100);

    QVERIFY(rectangleNode.isValid());
    QVERIFY(rectangleNode.hasProperty("width"));

    // Attach NodeInstanceView

    TestConnectionManager connectionManager;
    ExternalDependenciesFake externalDependenciesFake{model.get()};
    NodeInstanceView instanceView{connectionManager, externalDependenciesFake};

    model->attachView(&instanceView);

    NodeInstance rootInstance = instanceView.instanceForModelNode(rootNode);
    NodeInstance rectangleInstance = instanceView.instanceForModelNode(rectangleNode);
    QVERIFY(rootInstance.isValid());
    QVERIFY(rectangleInstance.isValid());
    QCOMPARE(QVariant(100), rectangleInstance.property("width"));
    QVERIFY(rootInstance.instanceId() >= 0);
    QVERIFY(rectangleInstance.instanceId() >= 0);

    model->detachView(&instanceView);
}

void tst_TestCore::testQmlModelAddMultipleStates()
{
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QVERIFY(QmlItemNode(rootModelNode).isValid());

    QmlModelState state1 = QmlItemNode(rootModelNode).states().addState("state1");
    QVERIFY(state1.isValid());
    QCOMPARE(state1.name(), QString("state1"));

    QmlModelState state2 = QmlItemNode(rootModelNode).states().addState("state2");
    QVERIFY(state2.isValid());
    QCOMPARE(state1.name(), QString("state1"));
    QCOMPARE(state2.name(), QString("state2"));

    QmlModelState state3 = QmlItemNode(rootModelNode).states().addState("state3");
    QVERIFY(state3.isValid());
    QCOMPARE(state1.name(), QString("state1"));
    QCOMPARE(state2.name(), QString("state2"));
    QCOMPARE(state3.name(), QString("state3"));

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 3);
}

void tst_TestCore::testQmlModelRemoveStates()
{
    auto model(createModel("QtQuick.Rectangle", 2, 0));

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QVERIFY(QmlItemNode(rootModelNode).isValid());

    QmlModelState state1 = QmlItemNode(rootModelNode).states().addState("state1");
    QmlModelState state2 = QmlItemNode(rootModelNode).states().addState("state2");
    QmlModelState state3 = QmlItemNode(rootModelNode).states().addState("state3");

    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 3);

    QmlItemNode(rootModelNode).states().removeState("state2");
    QVERIFY(!state2.isValid());
    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 2);

    QmlItemNode(rootModelNode).states().removeState("state3");
    QVERIFY(!state3.isValid());
    state1.modelNode().destroy();
    QVERIFY(!state1.isValid());
    QCOMPARE(QmlItemNode(rootModelNode).states().allStates().count(), 0);
}

void tst_TestCore::testQmlModelStateWithName()
{
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Rectangle { id: theRect; width: 100; states: [ State { name: \"a\"; PropertyChanges { target: theRect; width: 200; } } ] }\n");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    auto view = std::make_unique<TestView>();
    model1->attachView(view.get());

    QmlItemNode rootNode = view->rootModelNode();

    qDebug() << rootNode;

    QCOMPARE(rootNode.states().allStates().size(), 1);

    QmlModelState modelState = rootNode.states().allStates().at(0);
    QVERIFY(!modelState.isBaseState());
    //QCOMPARE(rootNode.instanceValue("width").toInt(), 100);

    QVERIFY(rootNode.isInBaseState());
    QVERIFY(rootNode.propertyAffectedByCurrentState("width"));
    view->setCurrentState(rootNode.states().allStates().at(0));

    qDebug() << rootNode.modelValue("width");

    rootNode.setVariantProperty("width", 112);

    const QLatin1String expected1("import QtQuick 2.1; Rectangle { id: theRect; width: 100; states: [ State { name: \"a\"; PropertyChanges { target: theRect; width: 112 } } ] }\n");
    QCOMPARE(textEdit1.toPlainText(), expected1);

    QVERIFY(!rootNode.isInBaseState());
    //QCOMPARE(rootNode.instanceValue("width").toInt(), 112);

    view->setCurrentState(view->baseState());
    //QCOMPARE(rootNode.instanceValue("width").toInt(), 100);

    view->setCurrentState(rootNode.states().allStates().at(0));
    //QCOMPARE(rootNode.instanceValue("width").toInt(), 112);


    modelState.destroy();
    QCOMPARE(rootNode.states().allStates().size(), 0);
}

void tst_TestCore::testRewriterAutomaticSemicolonAfterChangedProperty()
{
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Rectangle {\n    width: 640\n    height: 480\n}\n");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    ModelNode rootModelNode = testRewriterView1->rootModelNode();
    rootModelNode.variantProperty("height").setValue(1480);
    QCOMPARE(rootModelNode.variantProperty("height").value().toInt(), 1480);

    QVERIFY(testRewriterView1->errors().isEmpty());
}

void tst_TestCore::defaultPropertyValues()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    QCOMPARE(view->rootModelNode().variantProperty("x").value().toDouble(), 0.0);
    QCOMPARE(view->rootModelNode().variantProperty("width").value().toDouble(), 0.0);

    ModelNode rectNode(addNodeListChild(view->rootModelNode(), "QtQuick.Rectangle", 2, 0, "data"));

    QCOMPARE(rectNode.variantProperty("y").value().toDouble(), 0.0);
    QCOMPARE(rectNode.variantProperty("width").value().toDouble(), 0.0);

    ModelNode imageNode(addNodeListChild(view->rootModelNode(), "QtQuick.Image", 2, 0, "data"));

    QCOMPARE(imageNode.variantProperty("y").value().toDouble(), 0.0);
    QCOMPARE(imageNode.variantProperty("width").value().toDouble(), 0.0);
}

void tst_TestCore::testModelPropertyValueTypes()
{
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Rectangle { width: 100; radius: 1.5; color: \"red\"; }");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model1(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model1);
    testRewriterView1->setTextModifier(&modifier1);
    model1->attachView(testRewriterView1.get());

    QVERIFY(testRewriterView1->errors().isEmpty());

    ModelNode rootModelNode(testRewriterView1->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.variantProperty("width").value().typeId(), QMetaType::Double);
    QCOMPARE(rootModelNode.variantProperty("radius").value().typeId(), QMetaType::Double);
    QCOMPARE(rootModelNode.variantProperty("color").value().typeId(), QMetaType::QColor);
}

void tst_TestCore::testModelNodeInHierarchy()
{
    auto model(createModel("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    QVERIFY(view->rootModelNode().isInHierarchy());
    ModelNode node1 = addNodeListChild(view->rootModelNode(), "QtQuick.Item", 2, 1, "data");
    QVERIFY(node1.isInHierarchy());
    ModelNode node2 = view->createModelNode("QtQuick.Item", 2, 1);
    QVERIFY(!node2.isInHierarchy());
    node2.nodeListProperty("data").reparentHere(node1);
    QVERIFY(!node2.isInHierarchy());
    view->rootModelNode().nodeListProperty("data").reparentHere(node2);
    QVERIFY(node1.isInHierarchy());
    QVERIFY(node2.isInHierarchy());
}

void tst_TestCore::testModelNodeIsAncestorOf()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    //
    //  import QtQuick 2.0
    //  Item {
    //    Item {
    //      id: item2
    //    }
    //    Item {
    //      id: item3
    //      Item {
    //        id: item4
    //      }
    //    }
    //  }
    //
    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    view->rootModelNode().setIdWithoutRefactoring("item1");
    ModelNode item2 = addNodeListChild(view->rootModelNode(), "QtQuick.Item", 2, 0, "data");
    item2.setIdWithoutRefactoring("item2");
    ModelNode item3 = addNodeListChild(view->rootModelNode(), "QtQuick.Item", 2, 0, "data");
    item3.setIdWithoutRefactoring("item3");
    ModelNode item4 = addNodeListChild(item3, "QtQuick.Item", 2, 0, "data");
    item4.setIdWithoutRefactoring("item4");

    QVERIFY(view->rootModelNode().isAncestorOf(item2));
    QVERIFY(view->rootModelNode().isAncestorOf(item3));
    QVERIFY(view->rootModelNode().isAncestorOf(item4));
    QVERIFY(!item2.isAncestorOf(view->rootModelNode()));
    QVERIFY(!item2.isAncestorOf(item4));
    QVERIFY(item3.isAncestorOf(item4));
}

void tst_TestCore::testModelChangeType()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  id: rootItem\n"
                                  "  Item {\n"
                                  "    id: firstItem\n"
                                  "    x: 10\n"
                                  "  }\n"
                                  "  Item {\n"
                                  "    id: secondItem\n"
                                  "    x: 20\n"
                                  "  }\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rootNode.id(), QLatin1String("rootItem"));

    ModelNode childNode = rootNode.nodeListProperty(("data")).toModelNodeList().at(0);
    QVERIFY(childNode.isValid());
    QCOMPARE(childNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    QCOMPARE(childNode.id(), QLatin1String("firstItem"));

    childNode.changeType("QtQuick.Rectangle", 2, 0);

    QCOMPARE(childNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    const QLatin1String expectedQmlCode1("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  id: rootItem\n"
                                  "  Rectangle {\n"
                                  "    id: firstItem\n"
                                  "    x: 10\n"
                                  "  }\n"
                                  "  Item {\n"
                                  "    id: secondItem\n"
                                  "    x: 20\n"
                                  "  }\n"
                                  "}");

    QCOMPARE(textEdit.toPlainText(), expectedQmlCode1);

    childNode = rootNode.nodeListProperty(("data")).toModelNodeList().at(1);
    QVERIFY(childNode.isValid());
    QCOMPARE(childNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
    QCOMPARE(childNode.id(), QLatin1String("secondItem"));

    childNode.changeType("QtQuick.Rectangle", 2, 0);

    QCOMPARE(childNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    const QLatin1String expectedQmlCode2("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  id: rootItem\n"
                                  "  Rectangle {\n"
                                  "    id: firstItem\n"
                                  "    x: 10\n"
                                  "  }\n"
                                  "  Rectangle {\n"
                                  "    id: secondItem\n"
                                  "    x: 20\n"
                                  "  }\n"
                                  "}");

    QCOMPARE(textEdit.toPlainText(), expectedQmlCode2);
}

void tst_TestCore::testModelSignalDefinition()
{
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QVERIFY(!rootModelNode.hasProperty("mySignal"));

    rootModelNode.signalDeclarationProperty("mySignal").setSignature("()");

    QVERIFY(rootModelNode.hasProperty("mySignal"));

    QVERIFY(rootModelNode.signalDeclarationProperty("mySignal").isValid());
    QVERIFY(rootModelNode.property("mySignal").isSignalDeclarationProperty());
    QCOMPARE(rootModelNode.signalDeclarationProperty("mySignal").signature(), "()");
}

void tst_TestCore::testModelDefaultProperties()
{
    auto model(createModel("QtQuick.Rectangle", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.metaInfo().defaultPropertyName(), QmlDesigner::TypeName("data"));
}

void tst_TestCore::loadAnchors()
{
    QSKIP("Crashing because of instance accesss", SkipAll);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item { width: 100; height: 100; Rectangle { anchors.left: parent.left; anchors.horizontalCenter: parent.horizontalCenter; anchors.rightMargin: 20; }}");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model);
    testRewriterView1->setTextModifier(&modifier1);
    model->attachView(testRewriterView1.get());

    auto testView = std::make_unique<TestView>();
    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());
    QmlItemNode anchoredNode(rootQmlItemNode.children().first());
    QVERIFY(anchoredNode.isValid());

    QmlAnchors anchors(anchoredNode.anchors());
    QVERIFY(anchors.instanceHasAnchor(AnchorLineLeft));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineTop));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineRight));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineBottom));
    QVERIFY(anchors.instanceHasAnchor(AnchorLineHorizontalCenter));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineVerticalCenter));

    QCOMPARE(anchors.instanceMargin(AnchorLineRight), 20.0);


    QCOMPARE(anchoredNode.instancePosition().x(), 0.0);
    QCOMPARE(anchoredNode.instancePosition().y(), 0.0);
    QCOMPARE(anchoredNode.instanceBoundingRect().width(), 100.0);
    QCOMPARE(anchoredNode.instanceBoundingRect().height(), 0.0);

    model->detachView(testView.get());
}

void tst_TestCore::changeAnchors()
{
    QSKIP("Requires instances", SkipAll);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item { width: 100; height: 100; Rectangle { anchors.left: parent.left; anchors.horizontalCenter: parent.horizontalCenter; anchors.rightMargin: 20; }}");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model);
    testRewriterView1->setTextModifier(&modifier1);
    model->attachView(testRewriterView1.get());

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());
    QmlItemNode anchoredNode(rootQmlItemNode.children().first());
    QVERIFY(anchoredNode.isValid());

    QmlAnchors anchors(anchoredNode.anchors());
    QVERIFY(anchors.instanceHasAnchor(AnchorLineLeft));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineTop));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineRight));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineBottom));
    QVERIFY(anchors.instanceHasAnchor(AnchorLineHorizontalCenter));
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineVerticalCenter));
    QCOMPARE(anchors.instanceMargin(AnchorLineRight), 20.0);


    QCOMPARE(anchoredNode.instancePosition().x(), 0.0);
    QCOMPARE(anchoredNode.instancePosition().y(), 0.0);
    QCOMPARE(anchoredNode.instanceBoundingRect().width(), 100.0);
    QCOMPARE(anchoredNode.instanceBoundingRect().height(), 0.0);


    anchors.removeAnchor(AnchorLineTop);

    anchoredNode.setBindingProperty("anchors.bottom", "parent.bottom");
    QVERIFY(anchors.instanceHasAnchor(AnchorLineBottom));
    anchors.setAnchor(AnchorLineRight, rootQmlItemNode, AnchorLineRight);
    QVERIFY(!anchors.instanceHasAnchor(AnchorLineRight));
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).type(), AnchorLineInvalid);
    QVERIFY(!anchors.instanceAnchor(AnchorLineRight).qmlItemNode().isValid());

    anchors.setMargin(AnchorLineRight, 10.0);
    QCOMPARE(anchors.instanceMargin(AnchorLineRight), 10.0);

    anchors.setMargin(AnchorLineRight, 0.0);
    QCOMPARE(anchors.instanceMargin(AnchorLineRight), 0.0);

    anchors.setMargin(AnchorLineRight, 20.0);
    QCOMPARE(anchors.instanceMargin(AnchorLineRight), 20.0);

    QCOMPARE(anchoredNode.instancePosition().x(), 0.0);
    QCOMPARE(anchoredNode.instancePosition().y(), 100.0);
    QCOMPARE(anchoredNode.instanceBoundingRect().width(), 100.0);
    QCOMPARE(anchoredNode.instanceBoundingRect().height(), 0.0);

    model->detachView(testView.get());
}

void tst_TestCore::anchorToSibling()
{
    QSKIP("Requires instances", SkipAll);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item { Rectangle {} Rectangle { id: secondChild } }");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model);
    testRewriterView1->setTextModifier(&modifier1);
    model->attachView(testRewriterView1.get());

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());
    QmlItemNode anchoredNode(rootQmlItemNode.children().first());
    QVERIFY(anchoredNode.isValid());

    QmlAnchors anchors(anchoredNode.anchors());

    QmlItemNode firstChild(rootQmlItemNode.children().first());
    QVERIFY(firstChild.isValid());
    QCOMPARE(firstChild.validId(), QString("rectangle1"));

    QmlItemNode secondChild(rootQmlItemNode.children().at(1));
    QVERIFY(secondChild.isValid());
    QCOMPARE(secondChild.validId(), QString("secondChild"));

    secondChild.anchors().setAnchor(AnchorLineTop, firstChild, AnchorLineBottom);

    QmlItemNode secondChildTopAnchoredNode = secondChild.anchors().instanceAnchor(AnchorLineTop).qmlItemNode();
    QVERIFY(secondChildTopAnchoredNode.isValid());
    QVERIFY2(secondChildTopAnchoredNode == firstChild, QString("expected %1 got %2").arg(firstChild.id(), secondChildTopAnchoredNode.id()).toLatin1().data());

    secondChild.anchors().setMargin(AnchorLineTop, 10.0);
    QCOMPARE(secondChild.anchors().instanceMargin(AnchorLineTop), 10.0);

    QVERIFY(firstChild.anchors().possibleAnchorLines(AnchorLineBottom, secondChild) == AnchorLineInvalid);
    QVERIFY(firstChild.anchors().possibleAnchorLines(AnchorLineLeft, secondChild) == AnchorLineHorizontalMask);
    QVERIFY(secondChild.anchors().possibleAnchorLines(AnchorLineTop, firstChild) == AnchorLineVerticalMask);
    QVERIFY(secondChild.anchors().possibleAnchorLines(AnchorLineLeft, firstChild) == AnchorLineHorizontalMask);
}

void tst_TestCore::removeFillAnchorByDetaching()
{
    QSKIP("Requires instances", SkipAll);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item { width: 100; height: 100; Rectangle { id: child; anchors.fill: parent } }");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model);
    testRewriterView1->setTextModifier(&modifier1);
    model->attachView(testRewriterView1.get());

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());

    QmlItemNode firstChild(rootQmlItemNode.children().first());
    QVERIFY(firstChild.isValid());
    QCOMPARE(firstChild.id(), QString("child"));

    // Verify the synthesized anchors:
    QmlAnchors anchors(firstChild);
    QVERIFY(anchors.instanceHasAnchor(AnchorLineLeft));
    QVERIFY(anchors.instanceAnchor(AnchorLineLeft).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).type(), AnchorLineLeft);

    QVERIFY(anchors.instanceAnchor(AnchorLineTop).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineTop).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineTop).type(), AnchorLineTop);

    QVERIFY(anchors.instanceAnchor(AnchorLineRight).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).type(), AnchorLineRight);

    QVERIFY(anchors.instanceAnchor(AnchorLineBottom).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineBottom).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineBottom).type(), AnchorLineBottom);

    QVERIFY(!anchors.instanceAnchor(AnchorLineHorizontalCenter).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineVerticalCenter).isValid());

    QCOMPARE(firstChild.instancePosition().x(), 0.0);
    QCOMPARE(firstChild.instancePosition().y(), 0.0);
    QCOMPARE(firstChild.instanceBoundingRect().width(), 100.0);
    QCOMPARE(firstChild.instanceBoundingRect().height(), 100.0);

    // Remove 2 anchors:
    RewriterTransaction transaction = testView->beginRewriterTransaction("TEST");
    anchors.removeAnchor(AnchorLineBottom);
    anchors.removeAnchor(AnchorLineTop);
    transaction.commit();

    // Verify again:
    QVERIFY(anchors.instanceAnchor(AnchorLineLeft).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).type(), AnchorLineLeft);

    QVERIFY(!anchors.instanceHasAnchor(AnchorLineTop));
    QVERIFY(!anchors.instanceAnchor(AnchorLineTop).isValid());

    QVERIFY(anchors.instanceAnchor(AnchorLineRight).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).type(), AnchorLineRight);

    QVERIFY(!anchors.instanceAnchor(AnchorLineBottom).isValid());

    QVERIFY(!anchors.instanceAnchor(AnchorLineHorizontalCenter).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineVerticalCenter).isValid());

    QCOMPARE(firstChild.instancePosition().x(), 0.0);
    QCOMPARE(firstChild.instancePosition().y(), 0.0);
    QCOMPARE(firstChild.instanceBoundingRect().width(), 100.0);
    QCOMPARE(firstChild.instanceBoundingRect().height(), 0.0);

    model->detachView(testView.get());
}

void tst_TestCore::removeFillAnchorByChanging()
{
    QSKIP("Requires instances", SkipAll);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item { width: 100; height: 100; Rectangle { id: child; anchors.fill: parent } }");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model);
    testRewriterView1->setTextModifier(&modifier1);
    model->attachView(testRewriterView1.get());

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());

    QmlItemNode firstChild(rootQmlItemNode.children().first());
    QVERIFY(firstChild.isValid());
    QCOMPARE(firstChild.id(), QString("child"));

    // Verify the synthesized anchors:
    QmlAnchors anchors(firstChild);
    QVERIFY(anchors.instanceAnchor(AnchorLineLeft).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).type(), AnchorLineLeft);

    QVERIFY(anchors.instanceAnchor(AnchorLineTop).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineTop).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineTop).type(), AnchorLineTop);

    QVERIFY(anchors.instanceAnchor(AnchorLineRight).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).type(), AnchorLineRight);

    QVERIFY(anchors.instanceAnchor(AnchorLineBottom).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineBottom).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineBottom).type(), AnchorLineBottom);

    QVERIFY(!anchors.instanceAnchor(AnchorLineHorizontalCenter).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineVerticalCenter).isValid());

    QCOMPARE(firstChild.instancePosition().x(), 0.0);
    QCOMPARE(firstChild.instancePosition().y(), 0.0);
    QCOMPARE(firstChild.instanceBoundingRect().width(), 100.0);
    QCOMPARE(firstChild.instanceBoundingRect().height(), 100.0);

    // Change 2 anchors:
    anchors.setAnchor(AnchorLineBottom, rootQmlItemNode, AnchorLineTop);
    anchors.setAnchor(AnchorLineTop, rootQmlItemNode, AnchorLineBottom);

    // Verify again:
    QVERIFY(anchors.instanceAnchor(AnchorLineLeft).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineLeft).type(), AnchorLineLeft);

    QVERIFY(anchors.instanceAnchor(AnchorLineTop).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineTop).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineTop).type(), AnchorLineBottom);

    QVERIFY(anchors.instanceAnchor(AnchorLineRight).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineRight).type(), AnchorLineRight);

    QVERIFY(anchors.instanceAnchor(AnchorLineBottom).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineBottom).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineBottom).type(), AnchorLineTop);

    QVERIFY(!anchors.instanceAnchor(AnchorLineHorizontalCenter).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineVerticalCenter).isValid());


    QCOMPARE(firstChild.instancePosition().x(), 0.0);
    QCOMPARE(firstChild.instancePosition().y(), 100.0);
    QCOMPARE(firstChild.instanceBoundingRect().width(), 100.0);
    QCOMPARE(firstChild.instanceBoundingRect().height(), -100.0);

    model->detachView(testView.get());
}

void tst_TestCore::testModelBindings()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    //NodeInstanceView *nodeInstanceView = new NodeInstanceView(model.get(), NodeInstanceServerInterface::TestModus);
    //model->attachView(nodeInstanceView);

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    ModelNode rootModelNode = testView->rootModelNode();
    QCOMPARE(rootModelNode.directSubModelNodes().count(), 0);
    //NodeInstance rootInstance = nodeInstanceView->instanceForModelNode(rootModelNode);

    // default width/height is 0
    //QCOMPARE(rootInstance.size().width(), 0.0);
    //QCOMPARE(rootInstance.size().height(), 0.0);

    rootModelNode.variantProperty("width").setValue(200);
    rootModelNode.variantProperty("height").setValue(100);

    //QCOMPARE(rootInstance.size().width(), 200.0);
    //QCOMPARE(rootInstance.size().height(), 100.0);

    ModelNode childNode = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");

    childNode.variantProperty("width").setValue(100);
    childNode.variantProperty("height").setValue(100);

    childNode.variantProperty("x").setValue(10);
    childNode.variantProperty("y").setValue(10);

    //NodeInstance childInstance = nodeInstanceView->instanceForModelNode(childNode);

    //QCOMPARE(childInstance.size().width(), 100.0);
    //QCOMPARE(childInstance.size().height(), 100.0);

    //QCOMPARE(childInstance.position().x(), 10.0);
    //QCOMPARE(childInstance.position().y(), 10.0);

    childNode.bindingProperty("width").setExpression("parent.width");

    //QCOMPARE(childInstance.size().width(), 200.0);

    rootModelNode.setIdWithoutRefactoring("root");
    QCOMPARE(rootModelNode.id(), QString("root"));

    childNode.setIdWithoutRefactoring("child");
    QCOMPARE(childNode.id(), QString("child"));
    childNode.variantProperty("width").setValue(100);

    //QCOMPARE(childInstance.size().width(), 100.0);

    childNode.bindingProperty("width").setExpression("child.height");
    //QCOMPARE(childInstance.size().width(), 100.0);

    childNode.bindingProperty("width").setExpression("root.width");
    //QCOMPARE(childInstance.size().width(), 200.0);

    model->detachView(testView.get());
}

void tst_TestCore::testModelDynamicProperties()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());
    ModelNode rootModelNode = rootQmlItemNode.modelNode();

    rootModelNode.variantProperty("x").setValue(10);
    rootModelNode.variantProperty("myColor").setDynamicTypeNameAndValue("color", QVariant(QColor(Qt::red)));
    rootModelNode.variantProperty("myDouble").setDynamicTypeNameAndValue("real", 10);

    QVERIFY(!rootModelNode.property("x").isDynamic());
    QVERIFY(rootModelNode.property("myColor").isDynamic());
    QVERIFY(rootModelNode.property("myDouble").isDynamic());

    QCOMPARE(rootModelNode.property("myColor").dynamicTypeName(), QmlDesigner::TypeName("color"));
    QCOMPARE(rootModelNode.variantProperty("myColor").value(), QVariant(QColor(Qt::red)));
    QCOMPARE(rootModelNode.property("myDouble").dynamicTypeName(), QmlDesigner::TypeName("real"));
    QCOMPARE(rootModelNode.variantProperty("myDouble").value(), QVariant(10));

    QVERIFY(rootModelNode.property("x").dynamicTypeName().isEmpty());

    model->rewriterView()->setCheckSemanticErrors(false);

    rootModelNode.removeProperty("myDouble");
    rootModelNode.variantProperty("myDouble").setValue(QVariant(10));
    QVERIFY(!rootModelNode.property("myDouble").isDynamic());

    rootModelNode.bindingProperty("myBindingDouble").setDynamicTypeNameAndExpression("real", QString("myDouble"));

    rootModelNode.bindingProperty("myBindingColor").setDynamicTypeNameAndExpression("color", "myColor");

    QCOMPARE(rootModelNode.property("myBindingColor").dynamicTypeName(), QmlDesigner::TypeName("color"));
    QCOMPARE(rootModelNode.property("myBindingDouble").dynamicTypeName(), QmlDesigner::TypeName("real"));

    QVERIFY(rootModelNode.property("myBindingDouble").isDynamic());
    QVERIFY(rootModelNode.property("myBindingColor").isDynamic());

    QCOMPARE(rootModelNode.bindingProperty("myBindingDouble").expression(), QString("myDouble"));
    QCOMPARE(rootModelNode.bindingProperty("myBindingColor").expression(), QString("myColor"));
}

void tst_TestCore::testModelSliding()
{
    auto model(createModel("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());

    ModelNode rect00(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));
    ModelNode rect01(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));
    ModelNode rect02(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));
    ModelNode rect03(addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data"));

    QVERIFY(rect00.isValid());
    QVERIFY(rect01.isValid());
    QVERIFY(rect02.isValid());
    QVERIFY(rect03.isValid());

    NodeListProperty listProperty(rootModelNode.nodeListProperty("data"));
    QVERIFY(!listProperty.isEmpty());

    QList<ModelNode> nodeList(listProperty.toModelNodeList());

    QCOMPARE(rect00, nodeList.at(0));
    QCOMPARE(rect01, nodeList.at(1));
    QCOMPARE(rect02, nodeList.at(2));
    QCOMPARE(rect03, nodeList.at(3));

    listProperty.slide(3,0);

    nodeList = listProperty.toModelNodeList();

    QCOMPARE(rect03, nodeList.at(0));
    QCOMPARE(rect00, nodeList.at(1));
    QCOMPARE(rect01, nodeList.at(2));
    QCOMPARE(rect02, nodeList.at(3));

    NodeListProperty childrenProperty(rootModelNode.nodeListProperty("children"));

    childrenProperty.reparentHere(rect00);
    childrenProperty.reparentHere(rect01);
    childrenProperty.reparentHere(rect02);
    childrenProperty.reparentHere(rect03);

    QVERIFY(listProperty.isEmpty());

    QVERIFY(!childrenProperty.isEmpty());

    nodeList = childrenProperty.toModelNodeList();

    QCOMPARE(rect00, nodeList.at(0));
    QCOMPARE(rect01, nodeList.at(1));
    QCOMPARE(rect02, nodeList.at(2));
    QCOMPARE(rect03, nodeList.at(3));

    childrenProperty.slide(0,3);

    nodeList = childrenProperty.toModelNodeList();

    QCOMPARE(rect01, nodeList.at(0));
    QCOMPARE(rect02, nodeList.at(1));
    QCOMPARE(rect03, nodeList.at(2));
    QCOMPARE(rect00, nodeList.at(3));
}

void tst_TestCore::testRewriterChangeId()
{
    const char* qmlString = "import QtQuick 2.1\nRectangle { }\n";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.simplifiedTypeName(), QString("Rectangle"));
    QCOMPARE(rootModelNode.id(), QString());

    rootModelNode.setIdWithoutRefactoring("rectId");

    QCOMPARE(rootModelNode.id(), QString("rectId"));

    QString expected = "import QtQuick 2.1\n"
                       "Rectangle { id: rectId }\n";

    QCOMPARE(textEdit.toPlainText(), expected);

    // change id for node outside of hierarchy
    ModelNode node = view->createModelNode("QtQuick.Item", 2, 0);
    node.setIdWithoutRefactoring("myId");
}

void tst_TestCore::testRewriterRemoveId()
{
    const char* qmlString = "import QtQuick 2.1\nRectangle { id: myRect }";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model, TestRewriterView::Amend);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.id(), QString("myRect"));

    //
    // remove id in text
    //
    const char* qmlString2 = "import QtQuick 2.1\nRectangle { }";
    textEdit.setPlainText(QString::fromUtf8(qmlString2));

    QCOMPARE(rootModelNode.id(), QString());
}

void tst_TestCore::testRewriterChangeValueProperty()
{
    const char* qmlString = "import QtQuick 2.1\nRectangle { x: 10; y: 10 }";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.property("x").toVariantProperty().value().toInt(), 10);

    rootModelNode.property("x").toVariantProperty().setValue(2);

    QCOMPARE(textEdit.toPlainText(), QString(QLatin1String(qmlString)).replace("x: 10;", "x: 2;"));

    // change property for node outside of hierarchy
    PropertyListType properties;
    properties.append(QPair<QmlDesigner::PropertyName,QVariant>("x", 10));
    ModelNode node = view->createModelNode("QtQuick.Item", 1, 1, properties);
    node.variantProperty("x").setValue(20);
}

void tst_TestCore::testRewriterRemoveValueProperty()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "Rectangle {\n"
                                  "  x: 10\n"
                                  "  y: 10;\n"
                                  "}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.property("x").toVariantProperty().value().toInt(), 10);

    rootModelNode.property("x").toVariantProperty().setValue(2);

    rootModelNode.removeProperty("x");

    const QLatin1String expected("\n"
                                 "import QtQuick 2.1\n"
                                 "Rectangle {\n"
                                 "  y: 10;\n"
                                 "}\n");
    QCOMPARE(textEdit.toPlainText(), expected);

    // remove property for node outside of hierarchy
    PropertyListType properties;
    properties.append(QPair<QmlDesigner::PropertyName,QVariant>("x", 10));
    ModelNode node = view->createModelNode("QtQuick.Item", 1, 1, properties);
    node.removeProperty("x");
}

void tst_TestCore::testRewriterSignalProperty()
{
    const char* qmlString = "import QtQuick 2.1\nRectangle { onColorChanged: {} }";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.properties().size(), 1);
}

void tst_TestCore::testRewriterObjectTypeProperty()
{
    const char* qmlString = "import QtQuick 2.1\nRectangle { x: 10; y: 10 }";

    QPlainTextEdit textEdit;
    textEdit.setPlainText(QString::fromUtf8(qmlString));
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item"));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    QVERIFY(view.data());
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootModelNode(view->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    view->changeRootNodeType("QtQuick.Text", 2, 0);

    QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Text"));
}

void tst_TestCore::testRewriterPropertyChanges()
{
    try {
        // PropertyChanges uses a custom parser

        // Use a slightly more complicated example so that target properties are not resolved in default scope
        const char* qmlString
                = "import QtQuick 2.1\n"
                  "Rectangle {\n"
                  "  Text {\n"
                  "    id: targetObj\n"
                  "    text: \"base State\"\n"
                  "  }\n"
                  "  states: [\n"
                  "    State {\n"
                  "      PropertyChanges {\n"
                  "        target: targetObj\n"
                  "        text: \"State 1\"\n"
                  "      }\n"
                  "    }\n"
                  "  ]\n"
                  "}\n";

        QPlainTextEdit textEdit;
        textEdit.setPlainText(QString::fromUtf8(qmlString));
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        QScopedPointer<TestView> view(new TestView);
        model->attachView(view.data());

        // read in
        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        ModelNode rootNode = view->rootModelNode();
        QVERIFY(rootNode.isValid());
        QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
        QVERIFY(rootNode.propertyNames().contains(("data")));
        QVERIFY(rootNode.propertyNames().contains(("states")));
        QCOMPARE(rootNode.propertyNames().count(), 2);

        NodeListProperty statesProperty = rootNode.nodeListProperty("states");
        QVERIFY(statesProperty.isValid());
        QCOMPARE(statesProperty.toModelNodeList().size(), 1);

        ModelNode stateNode = statesProperty.toModelNodeList().first();
        QVERIFY(stateNode.isValid());
        QCOMPARE(stateNode.type(), QmlDesigner::TypeName("QtQuick.State"));
        //QCOMPARE(stateNode.propertyNames(), QStringList("changes"));

        NodeListProperty stateChangesProperty = stateNode.property("changes").toNodeListProperty();
        QVERIFY(stateChangesProperty.isValid());

        ModelNode propertyChangesNode = stateChangesProperty.toModelNodeList().first();
        QVERIFY(propertyChangesNode.isValid());
        QCOMPARE(propertyChangesNode.properties().size(), 2);

        BindingProperty targetProperty = propertyChangesNode.bindingProperty("target");
        QVERIFY(targetProperty.isValid());

        VariantProperty textProperty = propertyChangesNode.variantProperty("text");
        QVERIFY(textProperty.isValid());
        QCOMPARE(textProperty.value().toString(), QLatin1String("State 1"));
    } catch (Exception &e) {
        QFAIL(e.description().toLatin1().data());
    }
}

void tst_TestCore::testRewriterListModel()
{
    try {
        // ListModel uses a custom parser
        const char* qmlString = "import QtQuick 2.1; ListModel {\n ListElement {\n age: 12\n} \n}";

        QPlainTextEdit textEdit;
        textEdit.setPlainText(QString::fromUtf8(qmlString));
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        QScopedPointer<TestView> view(new TestView);
        model->attachView(view.data());

        ModelNode listModelNode = view->rootModelNode();

        NodeListProperty elementListProperty = listModelNode.defaultNodeListProperty();
        QCOMPARE(elementListProperty.toModelNodeList().size(), 1);

        ModelNode elementNode = elementListProperty.toModelNodeList().at(0);
        QVERIFY(elementNode.isValid());
        QCOMPARE(elementNode.properties().size(), 1);
        QVERIFY(elementNode.variantProperty("age").isValid());
        QCOMPARE(elementNode.variantProperty("age").value().toInt(), 12);
    } catch (Exception &e) {
        QFAIL(qPrintable(e.description()));
    }
}

void tst_TestCore::testRewriterAddProperty()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(),  QmlDesigner::TypeName("QtQuick.Rectangle"));

    rootNode.variantProperty(("x")).setValue(123);

    QVERIFY(rootNode.hasProperty(("x")));
    QVERIFY(rootNode.property(("x")).isVariantProperty());
    QCOMPARE(rootNode.variantProperty(("x")).value(), QVariant(123));

    const QLatin1String expected("\n"
                                 "import QtQuick 2.1\n"
                                 "\n"
                                 "Rectangle {\n"
                                 "x: 123\n"
                                 "}");
    QCOMPARE(textEdit.toPlainText(), expected);
}

void tst_TestCore::testRewriterAddPropertyInNestedObject()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    Rectangle {\n"
                                  "        id: rectangle1\n"
                                  "    }\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode childNode = rootNode.nodeListProperty(("data")).toModelNodeList().at(0);
    QVERIFY(childNode.isValid());
    QCOMPARE(childNode.type(),  QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(childNode.id(), QLatin1String("rectangle1"));

    childNode.variantProperty(("x")).setValue(10);
    childNode.variantProperty(("y")).setValue(10);

    const QLatin1String expected("\n"
                                 "import QtQuick 2.1\n"
                                 "\n"
                                 "Rectangle {\n"
                                 "    Rectangle {\n"
                                 "        id: rectangle1\n"
                                 "        x: 10\n"
                                 "        y: 10\n"
                                 "    }\n"
                                 "}");
    QCOMPARE(textEdit.toPlainText(), expected);
}

void tst_TestCore::testRewriterAddObjectDefinition()
{
    const QLatin1String qmlString("import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode childNode = view->createModelNode("QtQuick.MouseArea", 2, 0);
    rootNode.nodeAbstractProperty("data").reparentHere(childNode);

    QCOMPARE(rootNode.nodeProperty("data").toNodeListProperty().toModelNodeList().size(), 1);
    childNode = rootNode.nodeProperty("data").toNodeListProperty().toModelNodeList().at(0);
    QCOMPARE(childNode.type(), QmlDesigner::TypeName(("QtQuick.MouseArea")));
}

void tst_TestCore::testRewriterAddStatesArray()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(),  QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode stateNode = view->createModelNode("QtQuick.State", 2, 0);
    rootNode.nodeListProperty(("states")).reparentHere(stateNode);

    const QString expected1 = QLatin1String("\n"
                                           "import QtQuick 2.1\n"
                                           "\n"
                                           "Rectangle {\n"
                                           "states: [\n"
                                           "    State {\n"
                                           "    }\n"
                                           "]\n"
                                           "}");
    QCOMPARE(textEdit.toPlainText(), expected1);

    ModelNode stateNode2 = view->createModelNode("QtQuick.State", 2, 0);
    rootNode.nodeListProperty(("states")).reparentHere(stateNode2);

    const QString expected2 = QLatin1String("\n"
                                           "import QtQuick 2.1\n"
                                           "\n"
                                           "Rectangle {\n"
                                           "states: [\n"
                                           "    State {\n"
                                           "    },\n"
                                           "    State {\n"
                                           "    }\n"
                                           "]\n"
                                           "}");
    QCOMPARE(textEdit.toPlainText(), expected2);
}

void tst_TestCore::testRewriterRemoveStates()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    states: [\n"
                                  "        State {\n"
                                  "        },\n"
                                  "        State {\n"
                                  "        }\n"
                                  "    ]\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    NodeListProperty statesProperty = rootNode.nodeListProperty("states");
    QVERIFY(statesProperty.isValid());
    QCOMPARE(statesProperty.toModelNodeList().size(), 2);

    ModelNode state = statesProperty.toModelNodeList().at(1);
    state.destroy();

    const QLatin1String expected1("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "    states: [\n"
                                  "        State {\n"
                                  "        }\n"
                                  "    ]\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected1);

    state = statesProperty.toModelNodeList().at(0);
    state.destroy();

    const QLatin1String expected2("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected2);
}

void tst_TestCore::testRewriterRemoveObjectDefinition()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  MouseArea {\n"
                                  "  }\n"
                                  "  MouseArea {\n"
                                  "  } // some comment here\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QCOMPARE(rootNode.nodeProperty("data").toNodeListProperty().toModelNodeList().size(), 2);
    ModelNode childNode = rootNode.nodeProperty("data").toNodeListProperty().toModelNodeList().at(1);
    QCOMPARE(childNode.type(), QmlDesigner::TypeName("QtQuick.MouseArea"));

    childNode.destroy();

    QCOMPARE(rootNode.nodeProperty("data").toNodeListProperty().toModelNodeList().size(), 1);
    childNode = rootNode.nodeProperty("data").toNodeListProperty().toModelNodeList().at(0);
    QCOMPARE(childNode.type(),  QmlDesigner::TypeName("QtQuick.MouseArea"));

    childNode.destroy();

    QVERIFY(!rootNode.hasProperty(("data")));

    const QString expected = QLatin1String("\n"
                                           "import QtQuick 2.1\n"
                                           "\n"
                                           "Rectangle {\n"
                                           "  // some comment here\n"
                                           "}");
    QCOMPARE(textEdit.toPlainText(), expected);

    // don't crash when deleting nodes not in any hierarchy
    ModelNode node1 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    ModelNode node2 = addNodeListChild(node1, "QtQuick.Item", 2, 1, "data");
    ModelNode node3 = addNodeListChild(node2, "QtQuick.Item", 2, 1, "data");

    node3.destroy();
    node1.destroy();
}

void tst_TestCore::testRewriterRemoveScriptBinding()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "   x: 10; // some comment\n"
                                  "  y: 20\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    QCOMPARE(rootNode.properties().size(), 2);
    QVERIFY(rootNode.hasProperty("x"));
    QVERIFY(rootNode.hasProperty("y"));

    rootNode.removeProperty("y");

    QCOMPARE(rootNode.properties().size(), 1);
    QVERIFY(rootNode.hasProperty("x"));
    QVERIFY(!rootNode.hasProperty("y"));

    rootNode.removeProperty("x");

    QCOMPARE(rootNode.properties().size(), 0);

    const QString expected = QLatin1String("\n"
                                           "import QtQuick 2.1\n"
                                           "\n"
                                           "Rectangle {\n"
                                           "   // some comment\n"
                                           "}");
    QCOMPARE(textEdit.toPlainText(), expected);
}

void tst_TestCore::testRewriterNodeReparenting()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "    MouseArea {\n"
                                  "    }\n"
                                  "  }\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode itemNode = rootNode.nodeListProperty("data").toModelNodeList().at(0);
    QVERIFY(itemNode.isValid());
    QCOMPARE(itemNode.type(), QmlDesigner::TypeName("QtQuick.Item"));

    ModelNode mouseArea = itemNode.nodeListProperty("data").toModelNodeList().at(0);
    QVERIFY(mouseArea.isValid());
    QCOMPARE(mouseArea.type(), QmlDesigner::TypeName("QtQuick.MouseArea"));

    rootNode.nodeListProperty("data").reparentHere(mouseArea);

    QVERIFY(mouseArea.isValid());
    QCOMPARE(rootNode.nodeListProperty("data").toModelNodeList().size(), 2);

    QString expected =  "\n"
                        "import QtQuick 2.1\n"
                        "\n"
                        "Rectangle {\n"
                        "  Item {\n"
                        "  }\n"
                        "\n"
                        "MouseArea {\n"
                        "    }\n"
                        "}";
    QCOMPARE(textEdit.toPlainText(), expected);

    // reparenting outside of the hierarchy
    ModelNode node1 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    ModelNode node2 = view->createModelNode("QtQuick.Item", 2, 1);
    ModelNode node3 = view->createModelNode("QtQuick.Item", 2, 1);
    node2.nodeListProperty("data").reparentHere(node3);
    node1.nodeListProperty("data").reparentHere(node2);

    // reparent into the hierarchy
    rootNode.nodeListProperty("data").reparentHere(node1);

    expected = "\n"
               "import QtQuick 2.1\n"
               "\n"
               "Rectangle {\n"
               "  Item {\n"
               "  }\n"
               "\n"
               "MouseArea {\n"
               "    }\n"
               "\n"
               "    Rectangle {\n"
               "        Item {\n"
               "            Item {\n"
               "            }\n"
               "        }\n"
               "    }\n"
               "}";

    QCOMPARE(stripWhiteSpaces(textEdit.toPlainText()), stripWhiteSpaces(expected));

    // reparent out of the hierarchy
    ModelNode node4 = view->createModelNode("QtQuick.Rectangle", 2, 0);
    node4.nodeListProperty("data").reparentHere(node1);

    expected =  "\n"
                "import QtQuick 2.1\n"
                "\n"
                "Rectangle {\n"
                "  Item {\n"
                "  }\n"
                "\n"
                "MouseArea {\n"
                "    }\n"
                "}";

    QCOMPARE(textEdit.toPlainText(), expected);
}

void tst_TestCore::testRewriterNodeReparentingWithTransaction()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  id: rootItem\n"
                                  "  Item {\n"
                                  "    id: firstItem\n"
                                  "    x: 10\n"
                                  "  }\n"
                                  "  Item {\n"
                                  "    id: secondItem\n"
                                  "    x: 20\n"
                                  "  }\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    QCOMPARE(rootNode.id(), QLatin1String("rootItem"));

    ModelNode item1Node = rootNode.nodeListProperty("data").toModelNodeList().at(0);
    QVERIFY(item1Node.isValid());
    QCOMPARE(item1Node.type(), QmlDesigner::TypeName("QtQuick.Item"));
    QCOMPARE(item1Node.id(), QLatin1String("firstItem"));

    ModelNode item2Node = rootNode.nodeListProperty("data").toModelNodeList().at(1);
    QVERIFY(item2Node.isValid());
    QCOMPARE(item2Node.type(), QmlDesigner::TypeName("QtQuick.Item"));
    QCOMPARE(item2Node.id(), QLatin1String("secondItem"));

    RewriterTransaction transaction = testRewriterView->beginRewriterTransaction("TEST");

    item1Node.nodeListProperty("data").reparentHere(item2Node);
    item2Node.variantProperty("x").setValue(0);

    transaction.commit();

    const QLatin1String expected("\n"
                                 "import QtQuick 2.1\n"
                                 "\n"
                                 "Rectangle {\n"
                                 "  id: rootItem\n"
                                 "  Item {\n"
                                 "    id: firstItem\n"
                                 "    x: 10\n"
                                 "\n"
                                 "Item {\n"
                                 "    id: secondItem\n"
                                 "    x: 0\n"
                                 "  }\n"
                                 "  }\n"
                                 "}");
    QCOMPARE(textEdit.toPlainText(), expected);
}
void tst_TestCore::testRewriterMovingInOut()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    ModelNode newNode = view->createModelNode("QtQuick.MouseArea", 2, 0);
    rootNode.nodeListProperty("data").reparentHere(newNode);

    const QLatin1String expected1("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "MouseArea {\n"
                                  "}\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected1);

#define move(node, x, y) {\
    node.variantProperty("x").setValue(x);\
    node.variantProperty("y").setValue(y);\
}
    move(newNode, 1, 1);
    move(newNode, 2, 2);
    move(newNode, 3, 3);
#undef move
    newNode.destroy();

    const QLatin1String expected2("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected2);

    QApplication::processEvents();
}

void tst_TestCore::testRewriterMovingInOutWithTransaction()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));

    RewriterTransaction transaction = view->beginRewriterTransaction("TEST");

    ModelNode newNode = view->createModelNode("QtQuick.MouseArea", 2, 1);
    rootNode.nodeListProperty("data").reparentHere(newNode);

#define move(node, x, y) {\
    node.variantProperty("x").setValue(x);\
    node.variantProperty("y").setValue(y);\
}
    move(newNode, 1, 1);
    move(newNode, 2, 2);
    move(newNode, 3, 3);
#undef move
    newNode.destroy();

    transaction.commit();

    const QLatin1String expected2("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected2);
    QApplication::processEvents();
}

void tst_TestCore::testRewriterComplexMovingInOut()
{
    const QLatin1String qmlString("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "  }\n"
                                  "}");
    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlString);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    // read in
    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    ModelNode rootNode = view->rootModelNode();
    QVERIFY(rootNode.isValid());
    QCOMPARE(rootNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
    ModelNode itemNode = rootNode.nodeListProperty("data").toModelNodeList().at(0);

    ModelNode newNode = view->createModelNode("QtQuick.MouseArea", 2, 0);
    rootNode.nodeListProperty("data").reparentHere(newNode);

    const QLatin1String expected1("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "  }\n"
                                  "\n"
                                  "  MouseArea {\n"
                                  "  }\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected1);

#define move(node, x, y) {\
    node.variantProperty(("x")).setValue(x);\
    node.variantProperty(("y")).setValue(y);\
}
    move(newNode, 1, 1);
    move(newNode, 2, 2);
    move(newNode, 3, 3);

    const QLatin1String expected2("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "  }\n"
                                  "\n"
                                  "  MouseArea {\n"
                                  "  x: 3\n"
                                  "  y: 3\n"
                                  "  }\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected2);

    itemNode.nodeListProperty(("data")).reparentHere(newNode);

    const QLatin1String expected3("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "MouseArea {\n"
                                  "  x: 3\n"
                                  "  y: 3\n"
                                  "  }\n"
                                  "  }\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected3);

    move(newNode, 0, 0);
    move(newNode, 1, 1);
    move(newNode, 2, 2);
    move(newNode, 3, 3);
#undef move
    newNode.destroy();

    const QLatin1String expected4("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "  }\n"
                                  "}");
    QCOMPARE(textEdit.toPlainText(), expected4);
    QApplication::processEvents();
}

void tst_TestCore::removeCenteredInAnchorByDetaching()
{
    QSKIP("Crashing for unkwown reasons", SkipAll);
    QPlainTextEdit textEdit1;
    textEdit1.setPlainText("import QtQuick 2.1; Item { Rectangle { id: child; anchors.centerIn: parent } }");
    NotIndentingTextEditModifier modifier1(&textEdit1);

    auto model(Model::create("QtQuick.Item"));

    auto testRewriterView1 = createTextRewriterView(*model);
    testRewriterView1->setTextModifier(&modifier1);
    model->attachView(testRewriterView1.get());

    auto testView = std::make_unique<TestView>();

    model->attachView(testView.get());

    QmlItemNode rootQmlItemNode(testView->rootQmlItemNode());


    QmlItemNode firstChild(rootQmlItemNode.children().first());
    QVERIFY(firstChild.isValid());
    QCOMPARE(firstChild.id(), QString("child"));

    // Verify the synthesized anchors:
    QmlAnchors anchors(firstChild);
    QVERIFY(!anchors.instanceAnchor(AnchorLineLeft).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineTop).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineRight).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineBottom).isValid());

    QVERIFY(anchors.instanceAnchor(AnchorLineHorizontalCenter).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineHorizontalCenter).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineHorizontalCenter).type(), AnchorLineHorizontalCenter);

    QVERIFY(anchors.instanceAnchor(AnchorLineVerticalCenter).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineVerticalCenter).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineVerticalCenter).type(), AnchorLineVerticalCenter);

    // Remove horizontal anchor:
    anchors.removeAnchor(AnchorLineHorizontalCenter);

    // Verify again:
    QVERIFY(!anchors.instanceAnchor(AnchorLineLeft).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineTop).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineRight).isValid());
    QVERIFY(!anchors.instanceAnchor(AnchorLineBottom).isValid());

    QVERIFY(!anchors.instanceAnchor(AnchorLineHorizontalCenter).isValid());

    QVERIFY(anchors.instanceAnchor(AnchorLineVerticalCenter).isValid());
    QCOMPARE(anchors.instanceAnchor(AnchorLineVerticalCenter).qmlItemNode(), rootQmlItemNode);
    QCOMPARE(anchors.instanceAnchor(AnchorLineVerticalCenter).type(), AnchorLineVerticalCenter);
}


void tst_TestCore::changePropertyBinding()
{
    auto model(createModel("QtQuick.Item", 2, 0));
    QVERIFY(model.get());

    QScopedPointer<TestView> view(new TestView);
    model->attachView(view.data());

    ModelNode rootModelNode(view->rootModelNode());
    rootModelNode.variantProperty("width").setValue(20);

    ModelNode firstChild = addNodeListChild(rootModelNode, "QtQuick.Rectangle", 2, 0, "data");
    firstChild.bindingProperty("width").setExpression(QString("parent.width"));
    firstChild.variantProperty("height").setValue(10);
    QVERIFY(firstChild.isValid());

    {
        QVERIFY(firstChild.hasProperty("width"));

        BindingProperty widthBinding = firstChild.bindingProperty("width");
        QVERIFY(widthBinding.isValid());
        QCOMPARE(widthBinding.expression(), QString("parent.width"));
        QVERIFY(firstChild.variantProperty("height").value().toInt() == 10);
    }

    firstChild.variantProperty("width").setValue(400);
    firstChild.bindingProperty("height").setExpression("parent.width / 2");

    {
        QVERIFY(firstChild.hasProperty("width"));
        QVariant width = firstChild.variantProperty("width").value();
        QVERIFY(width.isValid() && !width.isNull());
        QVERIFY(width == 400);
        QVERIFY(firstChild.hasProperty("height"));
        BindingProperty heightBinding = firstChild.bindingProperty("height");
        QVERIFY(heightBinding.isValid());
        QCOMPARE(heightBinding.expression(), QString("parent.width / 2"));
    }
}

void tst_TestCore::loadTestFiles()
{
    { //empty.qml
        QFile file(":/fx/empty.qml");
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

        QPlainTextEdit textEdit;
        textEdit.setPlainText(QString(QString::fromUtf8(file.readAll())));
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 1, 1));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        QVERIFY(model.get());
        ModelNode rootModelNode(testRewriterView->rootModelNode());
        QVERIFY(rootModelNode.isValid());
        QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Item"));
        QVERIFY(rootModelNode.directSubModelNodes().isEmpty());
    }

    { //helloworld.qml
        QFile file(":/fx/helloworld.qml");
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

        QPlainTextEdit textEdit;
        textEdit.setPlainText(QString(QString::fromUtf8(file.readAll())));
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        QVERIFY(model.get());
        ModelNode rootModelNode(testRewriterView->rootModelNode());
        QVERIFY(rootModelNode.isValid());
        QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
        QCOMPARE(rootModelNode.directSubModelNodes().count(), 1);
        QCOMPARE(rootModelNode.variantProperty("width").value().toInt(), 200);
        QCOMPARE(rootModelNode.variantProperty("height").value().toInt(), 200);

        ModelNode textNode(rootModelNode.directSubModelNodes().first());
        QVERIFY(textNode.isValid());
        QCOMPARE(textNode.type(), QmlDesigner::TypeName("QtQuick.Text"));
        QCOMPARE(textNode.variantProperty("x").value().toInt(), 66);
        QCOMPARE(textNode.variantProperty("y").value().toInt(), 93);
    }
    { //states.qml
        QFile file(":/fx/states.qml");
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

        QPlainTextEdit textEdit;
        textEdit.setPlainText(QString(QString::fromUtf8(file.readAll())));
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        QVERIFY(model.get());
        ModelNode rootModelNode(testRewriterView->rootModelNode());
        QVERIFY(rootModelNode.isValid());
        QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
        QCOMPARE(rootModelNode.directSubModelNodes().count(), 4);
        QCOMPARE(rootModelNode.variantProperty("width").value().toInt(), 200);
        QCOMPARE(rootModelNode.variantProperty("height").value().toInt(), 200);
        QCOMPARE(rootModelNode.id(), QLatin1String("myRect"));
        QVERIFY(rootModelNode.hasProperty("data"));
        QVERIFY(rootModelNode.property("data").isDefaultProperty());

        ModelNode textNode(rootModelNode.nodeListProperty("data").toModelNodeList().first());
        QVERIFY(textNode.isValid());
        QCOMPARE(textNode.id(), QLatin1String("textElement"));
        QCOMPARE(textNode.type(), QmlDesigner::TypeName("QtQuick.Text"));
        QCOMPARE(textNode.variantProperty("x").value().toInt(), 66);
        QCOMPARE(textNode.variantProperty("y").value().toInt(), 93);

        ModelNode imageNode(rootModelNode.nodeListProperty("data").toModelNodeList().last());
        QVERIFY(imageNode.isValid());
        QCOMPARE(imageNode.id(), QLatin1String("image1"));
        QCOMPARE(imageNode.type(), QmlDesigner::TypeName("QtQuick.Image"));
        QCOMPARE(imageNode.variantProperty("x").value().toInt(), 41);
        QCOMPARE(imageNode.variantProperty("y").value().toInt(), 46);
        QCOMPARE(imageNode.variantProperty("source").value().toUrl(), QUrl("images/qtcreator.png"));

        QVERIFY(rootModelNode.hasProperty("states"));
        QVERIFY(!rootModelNode.property("states").isDefaultProperty());

        QCOMPARE(rootModelNode.nodeListProperty("states").toModelNodeList().count(), 2);
    }

    QSKIP("Fails because the text editor model doesn't know about components");
    { //usingbutton.qml
        QFile file(":/fx/usingbutton.qml");
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));

        QPlainTextEdit textEdit;
        textEdit.setPlainText(QString(QString::fromUtf8(file.readAll())));
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        QVERIFY(testRewriterView->errors().isEmpty());

        QVERIFY(model.get());
        ModelNode rootModelNode(testRewriterView->rootModelNode());
        QVERIFY(rootModelNode.isValid());
        QCOMPARE(rootModelNode.type(), QmlDesigner::TypeName("QtQuick.Rectangle"));
        QVERIFY(!rootModelNode.directSubModelNodes().isEmpty());
    }
}

static QString rectWithGradient = "import QtQuick 2.1\n"
                                  "Rectangle {\n"
                                  "    gradient: Gradient {\n"
                                  "        id: pGradient\n"
                                  "        GradientStop { id: pOne; position: 0.0; color: \"lightsteelblue\" }\n"
                                  "        GradientStop { id: pTwo; position: 1.0; color: \"blue\" }\n"
                                  "    }\n"
                                  "    Gradient {\n"
                                  "        id: secondGradient\n"
                                  "        GradientStop { id: nOne; position: 0.0; color: \"blue\" }\n"
                                  "        GradientStop { id: nTwo; position: 1.0; color: \"lightsteelblue\" }\n"
                                  "    }\n"
                                  "}";

void tst_TestCore::loadGradient()
{
    QPlainTextEdit textEdit;
    textEdit.setPlainText(rectWithGradient);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QVERIFY(model.get());
    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());
    QCOMPARE(rootModelNode.directSubModelNodes().size(), 2);

    {
        QVERIFY(rootModelNode.hasProperty("gradient"));
        AbstractProperty gradientProperty = rootModelNode.property("gradient");
        QVERIFY(gradientProperty.isNodeProperty());
        ModelNode gradientPropertyModelNode = gradientProperty.toNodeProperty().modelNode();
        QVERIFY(gradientPropertyModelNode.isValid());
        QCOMPARE(gradientPropertyModelNode.type(),  QmlDesigner::TypeName("QtQuick.Gradient"));
        QCOMPARE(gradientPropertyModelNode.directSubModelNodes().size(), 2);

        AbstractProperty stopsProperty = gradientPropertyModelNode.property("stops");
        QVERIFY(stopsProperty.isValid());

        QVERIFY(stopsProperty.isNodeListProperty());
        QList<ModelNode>  stops = stopsProperty.toNodeListProperty().toModelNodeList();
        QCOMPARE(stops.size(), 2);

        ModelNode pOne = stops.first();
        ModelNode pTwo = stops.last();

        QCOMPARE(pOne.type(), QmlDesigner::TypeName("QtQuick.GradientStop"));
        QCOMPARE(pOne.id(), QString("pOne"));
        QCOMPARE(pOne.directSubModelNodes().size(), 0);
        QCOMPARE(pOne.propertyNames().size(), 2);
        QCOMPARE(pOne.variantProperty("position").value().typeId(), QMetaType::Double);
        QCOMPARE(pOne.variantProperty("position").value().toDouble(), 0.0);
        QCOMPARE(pOne.variantProperty("color").value().typeId(), QMetaType::QColor);
        QCOMPARE(pOne.variantProperty("color").value().value<QColor>(), QColor("lightsteelblue"));

        QCOMPARE(pTwo.type(), QmlDesigner::TypeName("QtQuick.GradientStop"));
        QCOMPARE(pTwo.id(), QString("pTwo"));
        QCOMPARE(pTwo.directSubModelNodes().size(), 0);
        QCOMPARE(pTwo.propertyNames().size(), 2);
        QCOMPARE(pTwo.variantProperty("position").value().typeId(), QMetaType::Double);
        QCOMPARE(pTwo.variantProperty("position").value().toDouble(), 1.0);
        QCOMPARE(pTwo.variantProperty("color").value().typeId(), QMetaType::QColor);
        QCOMPARE(pTwo.variantProperty("color").value().value<QColor>(), QColor("blue"));
    }

    {
        QCOMPARE(rootModelNode.directSubModelNodes().count(), 2);
        QVERIFY(rootModelNode.defaultNodeListProperty().isValid());
        QCOMPARE(rootModelNode.defaultNodeListProperty().toModelNodeList().count(), 1);
        ModelNode gradientNode = rootModelNode.defaultNodeListProperty().toModelNodeList().first();
        QVERIFY(gradientNode.isValid());
        QVERIFY(!gradientNode.metaInfo().isQtQuickItem());
        QCOMPARE(gradientNode.type(), QmlDesigner::TypeName("QtQuick.Gradient"));
        QCOMPARE(gradientNode.id(), QString("secondGradient"));
        QCOMPARE(gradientNode.directSubModelNodes().size(), 2);

        AbstractProperty stopsProperty = gradientNode.property("stops");
        QVERIFY(stopsProperty.isValid());

        QVERIFY(stopsProperty.isNodeListProperty());
        QList<ModelNode>  stops = stopsProperty.toNodeListProperty().toModelNodeList();
        QCOMPARE(stops.size(), 2);

        ModelNode nOne = stops.first();
        ModelNode nTwo = stops.last();

        QCOMPARE(nOne.type(), QmlDesigner::TypeName("QtQuick.GradientStop"));
        QCOMPARE(nOne.id(), QString("nOne"));
        QCOMPARE(nOne.directSubModelNodes().size(), 0);
        QCOMPARE(nOne.propertyNames().size(), 2);
        QCOMPARE(nOne.variantProperty("position").value().typeId(), QMetaType::Double);
        QCOMPARE(nOne.variantProperty("position").value().toDouble(), 0.0);
        QCOMPARE(nOne.variantProperty("color").value().typeId(), QMetaType::QColor);
        QCOMPARE(nOne.variantProperty("color").value().value<QColor>(), QColor("blue"));

        QCOMPARE(nTwo.type(), QmlDesigner::TypeName("QtQuick.GradientStop"));
        QCOMPARE(nTwo.id(), QString("nTwo"));
        QCOMPARE(nTwo.directSubModelNodes().size(), 0);
        QCOMPARE(nTwo.propertyNames().size(), 2);
        QCOMPARE(nTwo.variantProperty("position").value().typeId(), QMetaType::Double);
        QCOMPARE(nTwo.variantProperty("position").value().toDouble(), 1.0);
        QCOMPARE(nTwo.variantProperty("color").value().typeId(), QMetaType::QColor);
        QCOMPARE(nTwo.variantProperty("color").value().value<QColor>(), QColor("lightsteelblue"));
    }
}

void tst_TestCore::changeGradientId()
{
    try {
        QPlainTextEdit textEdit;
        textEdit.setPlainText(rectWithGradient);
        NotIndentingTextEditModifier textModifier(&textEdit);

        auto model(Model::create("QtQuick.Item", 2, 1));
        QVERIFY(model.get());

        auto testRewriterView = createTextRewriterView(*model);
        testRewriterView->setTextModifier(&textModifier);
        model->attachView(testRewriterView.get());

        QVERIFY(model.get());
        ModelNode rootModelNode(testRewriterView->rootModelNode());
        QVERIFY(rootModelNode.isValid());
        QCOMPARE(rootModelNode.directSubModelNodes().size(), 2);

        AbstractProperty gradientProperty = rootModelNode.property("gradient");
        QVERIFY(gradientProperty.isNodeProperty());
        ModelNode gradientNode = gradientProperty.toNodeProperty().modelNode();
        QVERIFY(gradientNode.isValid());
        QCOMPARE(gradientNode.id(), QString("pGradient"));

        gradientNode.setIdWithoutRefactoring("firstGradient");
        QCOMPARE(gradientNode.id(), QString("firstGradient"));

        AbstractProperty stopsProperty = gradientNode.property("stops");
        QVERIFY(stopsProperty.isValid());

        QVERIFY(stopsProperty.isNodeListProperty());
        QList<ModelNode>  stops = stopsProperty.toNodeListProperty().toModelNodeList();
        QCOMPARE(stops.size(), 2);

        ModelNode firstStop = stops.at(0);
        QVERIFY(firstStop.isValid());
        firstStop.destroy();
        QVERIFY(!firstStop.isValid());

        ModelNode gradientStop  = addNodeListChild(gradientNode, "QtQuick.GradientStop", 2, 0, "stops");
        gradientStop.variantProperty("position").setValue(0.5);
        gradientStop.variantProperty("color").setValue(QColor("yellow"));

        gradientStop.setIdWithoutRefactoring("newGradientStop");

        QCOMPARE(gradientNode.directSubModelNodes().size(), 2);
        QCOMPARE(gradientNode.nodeListProperty("stops").toModelNodeList().size(), 2);
        QCOMPARE(gradientStop.id(), QString("newGradientStop"));
        QCOMPARE(gradientStop.variantProperty("position").value().toDouble(), 0.5);
        QCOMPARE(gradientStop.variantProperty("color").value().value<QColor>(), QColor("yellow"));

    } catch (Exception &e) {
        qDebug() << e;
        QFAIL(e.description().toLatin1().data());
    }
}

static void checkNode(QmlJS::SimpleReaderNode::Ptr node, TestRewriterView *view);

static void checkChildNodes(QmlJS::SimpleReaderNode::Ptr node, TestRewriterView *view)
{
    for (auto child : node->children())
        checkNode(child, view);
}

static void checkNode(QmlJS::SimpleReaderNode::Ptr node, TestRewriterView *view)
{
    QVERIFY(node);
    QVERIFY(node->propertyNames().contains("i"));
    const int internalId = node->property("i").value.toInt();
    const ModelNode modelNode = view->modelNodeForInternalId(internalId);
    QVERIFY(modelNode.isValid());
    auto properties = node->properties();

    for (auto i = properties.begin(); i != properties.end(); ++i) {
        if (i.key() != "i") {
            QCOMPARE(i.value().value,
                     modelNode.auxiliaryData(QmlDesigner::AuxiliaryDataType::Temporary,
                                             i.key().toUtf8()));
        }
    }

    checkChildNodes(node, view);
}

void tst_TestCore::writeAnnotations()
{
    QSKIP("We have to improve handling of emtpy lines.", SkipAll);

    const QLatin1String qmlCode("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "  }\n"
                                  "\n"
                                  "  MouseArea {\n"
                                  "  x: 3\n"
                                  "  y: 3\n"
                                  "  }\n"
                                  "}");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlCode);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QVERIFY(model.get());
    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    rootModelNode.setAuxiliaryData(AuxiliaryDataType::Document, "x", 10);
    for (const auto &child : rootModelNode.allSubModelNodes()) {
        child.setAuxiliaryData(AuxiliaryDataType::Document, "x", 10);
        child.setAuxiliaryData(AuxiliaryDataType::Document, "test", true);
        child.setAuxiliaryData(AuxiliaryDataType::Document, "test2", "string");
    }

    const QString metaSource = testRewriterView->auxiliaryDataAsQML();

    QmlJS::SimpleReader reader;
    checkChildNodes(reader.readFromSource(metaSource), testRewriterView.get());

    testRewriterView->writeAuxiliaryData();
    const QString textWithMeta = testRewriterView->textModifier()->text();
    testRewriterView->writeAuxiliaryData();
    QCOMPARE(textWithMeta.length(), testRewriterView->textModifier()->text().length());
}

void tst_TestCore::readAnnotations()
{
    const QLatin1String qmlCode("\n"
                                  "import QtQuick 2.1\n"
                                  "\n"
                                  "Rectangle {\n"
                                  "  Item {\n"
                                  "  }\n"
                                  "\n"
                                  "  MouseArea {\n"
                                  "  x: 3\n"
                                  "  y: 3\n"
                                  "  }\n"
                                  "}");

    const QLatin1String metaCode("\n/*##^## Designer {\n    D{i:0;x:10}D{i:1;test:true;x:10;test2:\"string\"}"
                                 "D{i:2;test:true;x:10;test2:\"string\"}\n}\n ##^##*/\n");

    const QLatin1String metaCodeQmlCode("Designer {\n    D{i:0;x:10}D{i:1;test2:\"string\";x:10;test:true}"
                                 "D{i:2;test2:\"string\";x:10;test:true}\n}\n");

    QPlainTextEdit textEdit;
    textEdit.setPlainText(qmlCode + metaCode);
    NotIndentingTextEditModifier textModifier(&textEdit);

    auto model(Model::create("QtQuick.Item", 2, 1));
    QVERIFY(model.get());

    auto testRewriterView = createTextRewriterView(*model);
    testRewriterView->setTextModifier(&textModifier);
    model->attachView(testRewriterView.get());

    QVERIFY(model.get());
    ModelNode rootModelNode(testRewriterView->rootModelNode());
    QVERIFY(rootModelNode.isValid());

    testRewriterView->restoreAuxiliaryData();

    const QString metaSource = testRewriterView->auxiliaryDataAsQML();
    QCOMPARE(metaSource.length(), QString(metaCodeQmlCode).length());
}


QTEST_MAIN(tst_TestCore);

// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "textureeditorview.h"

#include "textureeditorqmlbackend.h"
#include "textureeditorcontextobject.h"
#include "textureeditordynamicpropertiesproxymodel.h"
#include "propertyeditorvalue.h"
#include "textureeditortransaction.h"
#include "assetslibrarywidget.h"

#include <auxiliarydataproperties.h>
#include <bindingproperty.h>
#include <createtexture.h>
#include <dynamicpropertiesmodel.h>
#include <externaldependenciesinterface.h>
#include <nodeinstanceview.h>
#include <nodelistproperty.h>
#include <nodemetainfo.h>
#include <nodeproperty.h>
#include <qmldesignerconstants.h>
#include <qmldesignerplugin.h>
#include <qmltimeline.h>
#include <rewritingexception.h>
#include <sourcepathcacheinterface.h>
#include <variantproperty.h>

#include <sourcepathstorage/sourcepathcache.h>

#include <theme.h>

#include <coreplugin/icore.h>
#include <coreplugin/messagebox.h>
#include <designdocument.h>
#include <designmodewidget.h>
#include <propertyeditorqmlbackend.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>
#include <utils3d.h>
#include <qmldesignerplugin.h>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QQuickWidget>
#include <QQuickItem>
#include <QStackedWidget>
#include <QShortcut>
#include <QTimer>
#include <QColorDialog>

using namespace Qt::StringLiterals;

namespace QmlDesigner {

TextureEditorView::TextureEditorView(AsynchronousImageCache &imageCache,
                                     ExternalDependenciesInterface &externalDependencies)
    : AbstractView{externalDependencies}
    , m_imageCache(imageCache)
    , m_stackedWidget(new QStackedWidget)
    , m_propertyComponentGenerator{PropertyEditorQmlBackend::propertyEditorResourcesPath(), model()}
    , m_dynamicPropertiesModel(new DynamicPropertiesModel(true, this))
{
    m_updateShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F12), m_stackedWidget);
    connect(m_updateShortcut, &QShortcut::activated, this, &TextureEditorView::reloadQml);

    m_stackedWidget->setStyleSheet(Theme::replaceCssColors(
        QString::fromUtf8(Utils::FileReader::fetchQrc(":/qmldesigner/stylesheet.css"))));
    m_stackedWidget->setMinimumWidth(250);
    QmlDesignerPlugin::trackWidgetFocusTime(m_stackedWidget, Constants::EVENT_TEXTUREEDITOR_TIME);

    TextureEditorDynamicPropertiesProxyModel::registerDeclarativeType();
}

TextureEditorView::~TextureEditorView()
{
    qDeleteAll(m_qmlBackendHash);
}

// from texture editor to model
void TextureEditorView::changeValue(const QString &name)
{
    PropertyName propertyName = name.toUtf8();

    if (propertyName.isNull() || locked() || noValidSelection() || propertyName == "id"
        || propertyName == Constants::PROPERTY_EDITOR_CLASSNAME_PROPERTY) {
        return;
    }

    PropertyName underscoreName(propertyName);
    underscoreName.replace('.', '_');
    PropertyEditorValue *value = m_qmlBackEnd->propertyValueForName(QString::fromLatin1(underscoreName));

    if (!value)
        return;

    if (propertyName == "objectName") {
        QTC_ASSERT(m_selectedTexture.isValid(), return);
        QmlObjectNode(m_selectedTexture).setNameAndId(value->value().toString(), "texture");
        return;
    }

    if (propertyName.endsWith("__AUX")) {
        commitAuxValueToModel(propertyName, value->value());
        return;
    }

    const NodeMetaInfo metaInfo = m_selectedTexture.metaInfo();

    QVariant castedValue;

    if (auto property = metaInfo.property(propertyName)) {
        castedValue = property.castedValue(value->value());
    } else {
        qWarning() << __FUNCTION__ << propertyName << "cannot be casted (metainfo)";
        return;
    }

    if (value->value().isValid() && !castedValue.isValid()) {
        qWarning() << __FUNCTION__ << propertyName << "not properly casted (metainfo)";
        return;
    }

    bool propertyTypeUrl = false;

    if (auto property = metaInfo.property(propertyName)) {
        if (property.propertyType().isUrl()) {
            // turn absolute local file paths into relative paths
            propertyTypeUrl = true;
            QString filePath = castedValue.toUrl().toString();
            QFileInfo fi(filePath);
            if (fi.exists() && fi.isAbsolute()) {
                QDir fileDir(QFileInfo(model()->fileUrl().toLocalFile()).absolutePath());
                castedValue = QUrl(fileDir.relativeFilePath(filePath));
            }
        }
    }

    if (name == "state" && castedValue.toString() == "base state")
        castedValue = "";

    if (castedValue.typeId() == QMetaType::QColor) {
        QColor color = castedValue.value<QColor>();
        QColor newColor = QColor(color.name());
        newColor.setAlpha(color.alpha());
        castedValue = QVariant(newColor);
    }

    if (!value->value().isValid() || (propertyTypeUrl && value->value().toString().isEmpty())) { // reset
        removePropertyFromModel(propertyName);
    } else {
        // QVector*D(0, 0, 0) detects as null variant though it is valid value
        if (castedValue.isValid()
            && (!castedValue.isNull() || castedValue.typeId() == QMetaType::QVector2D
                || castedValue.typeId() == QMetaType::QVector3D
                || castedValue.typeId() == QMetaType::QVector4D)) {
            commitVariantValueToModel(propertyName, castedValue);
        }
    }
}

static bool isTrueFalseLiteral(const QString &expression)
{
    return (expression.compare("false", Qt::CaseInsensitive) == 0)
           || (expression.compare("true", Qt::CaseInsensitive) == 0);
}

void TextureEditorView::changeExpression(const QString &propertyName)
{
    PropertyName name = propertyName.toUtf8();

    if (name.isNull() || locked() || noValidSelection())
        return;

    executeInTransaction("TextureEditorView::changeExpression", [this, name] {
        PropertyName underscoreName(name);
        underscoreName.replace('.', '_');

        QmlObjectNode qmlObjectNode(m_selectedTexture);
        PropertyEditorValue *value = m_qmlBackEnd->propertyValueForName(QString::fromLatin1(underscoreName));

        if (!value) {
            qWarning() << __FUNCTION__ << "no value for " << underscoreName;
            return;
        }

        if (auto property = m_selectedTexture.metaInfo().property(name)) {
            auto propertyType = property.propertyType();
            if (propertyType.isColor()) {
                if (QColor(value->expression().remove('"')).isValid()) {
                    qmlObjectNode.setVariantProperty(name, QColor(value->expression().remove('"')));
                    return;
                }
            } else if (propertyType.isBool()) {
                if (isTrueFalseLiteral(value->expression())) {
                    if (value->expression().compare("true", Qt::CaseInsensitive) == 0)
                        qmlObjectNode.setVariantProperty(name, true);
                    else
                        qmlObjectNode.setVariantProperty(name, false);
                    return;
                }
            } else if (propertyType.isInteger()) {
                bool ok;
                int intValue = value->expression().toInt(&ok);
                if (ok) {
                    qmlObjectNode.setVariantProperty(name, intValue);
                    return;
                }
            } else if (propertyType.isFloat()) {
                bool ok;
                qreal realValue = value->expression().toDouble(&ok);
                if (ok) {
                    qmlObjectNode.setVariantProperty(name, realValue);
                    return;
                }
            } else if (propertyType.isVariant()) {
                bool ok;
                qreal realValue = value->expression().toDouble(&ok);
                if (ok) {
                    qmlObjectNode.setVariantProperty(name, realValue);
                    return;
                } else if (isTrueFalseLiteral(value->expression())) {
                    if (value->expression().compare("true", Qt::CaseInsensitive) == 0)
                        qmlObjectNode.setVariantProperty(name, true);
                    else
                        qmlObjectNode.setVariantProperty(name, false);
                    return;
                }
            }
        }

        if (value->expression().isEmpty()) {
            value->resetValue();
            return;
        }

        if (qmlObjectNode.expression(name) != value->expression() || !qmlObjectNode.propertyAffectedByCurrentState(name))
            qmlObjectNode.setBindingProperty(name, value->expression());
    });
}

void TextureEditorView::exportPropertyAsAlias(const QString &name)
{
    if (name.isNull() || locked() || noValidSelection())
        return;

    executeInTransaction("TextureEditorView::exportPopertyAsAlias", [this, name] {
        const QString id = m_selectedTexture.validId();
        QString upperCasePropertyName = name;
        upperCasePropertyName.replace(0, 1, upperCasePropertyName.at(0).toUpper());
        QString aliasName = id + upperCasePropertyName;
        aliasName.replace(".", ""); //remove all dots

        PropertyName propertyName = aliasName.toUtf8();
        if (rootModelNode().hasProperty(propertyName)) {
            Core::AsynchronousMessageBox::warning(tr("Cannot Export Property as Alias"),
                                                  tr("Property %1 does already exist for root component.").arg(aliasName));
            return;
        }
        rootModelNode().bindingProperty(propertyName).setDynamicTypeNameAndExpression("alias", id + "." + name);
    });
}

void TextureEditorView::removeAliasExport(const QString &name)
{
    if (name.isNull() || locked() || noValidSelection())
        return;

    executeInTransaction("TextureEditorView::removeAliasExport", [this, name] {
        const QString id = m_selectedTexture.validId();

        const QList<BindingProperty> bindingProps = rootModelNode().bindingProperties();
        for (const BindingProperty &property : bindingProps) {
            if (property.expression() == (id + "." + name)) {
                rootModelNode().removeProperty(property.name());
                break;
            }
        }
    });
}

bool TextureEditorView::locked() const
{
    return m_locked;
}

void TextureEditorView::currentTimelineChanged(const ModelNode &)
{
    if (m_qmlBackEnd)
        m_qmlBackEnd->contextObject()->setHasActiveTimeline(QmlTimeline::hasActiveTimeline(this));
}

void TextureEditorView::refreshMetaInfos(const TypeIds &deletedTypeIds)
{
    m_propertyComponentGenerator.refreshMetaInfos(deletedTypeIds);
}

DynamicPropertiesModel *TextureEditorView::dynamicPropertiesModel() const
{
    return m_dynamicPropertiesModel;
}

TextureEditorView *TextureEditorView::instance()
{
    static TextureEditorView *s_instance = nullptr;

    if (s_instance)
        return s_instance;

    const auto views = QmlDesignerPlugin::instance()->viewManager().views();
    for (auto *view : views) {
        TextureEditorView *myView = qobject_cast<TextureEditorView *>(view);
        if (myView)
            s_instance =  myView;
    }

    QTC_ASSERT(s_instance, return nullptr);
    return s_instance;
}

void TextureEditorView::timerEvent(QTimerEvent *timerEvent)
{
    if (m_timerId == timerEvent->timerId()) {
        if (m_selectedTextureChanged) {
            m_selectedTextureChanged = false;
            Utils3D::selectTexture(m_newSelectedTexture);
            m_newSelectedTexture = {};
        } else {
            resetView();
        }
    }
}

void TextureEditorView::resetView()
{
    if (!model())
        return;

    m_locked = true;

    if (m_timerId)
        killTimer(m_timerId);

    setupQmlBackend();

    if (m_qmlBackEnd)
        m_qmlBackEnd->emitSelectionChanged();

    m_locked = false;

    if (m_timerId)
        m_timerId = 0;
}

// static
QString TextureEditorView::textureEditorResourcesPath()
{
#ifdef SHARE_QML_PATH
    if (Utils::qtcEnvironmentVariableIsSet("LOAD_QML_FROM_SOURCE"))
        return QLatin1String(SHARE_QML_PATH) + "/textureEditorQmlSource";
#endif
    return Core::ICore::resourcePath("qmldesigner/textureEditorQmlSource").toUrlishString();
}

void TextureEditorView::applyTextureToSelectedModel(const ModelNode &texture)
{
    if (!m_selectedModel.isValid())
        return;

    QTC_ASSERT(texture.isValid(), return);

    QmlDesignerPlugin::instance()->mainWidget()->showDockWidget("MaterialBrowser");
    emitCustomNotification("apply_texture_to_model3D", {m_selectedModel, m_selectedTexture});
}

void TextureEditorView::handleToolBarAction(int action)
{
    QTC_ASSERT(m_hasQuick3DImport, return);

    switch (action) {
    case TextureEditorContextObject::ApplyToSelected: {
        applyTextureToSelectedModel(m_selectedTexture);
        break;
    }

    case TextureEditorContextObject::AddNewTexture: {
        if (!model())
            break;
        CreateTexture(this).execute();
        break;
    }

    case TextureEditorContextObject::DeleteCurrentTexture: {
        if (m_selectedTexture.isValid()) {
            executeInTransaction(__FUNCTION__, [&] {
                m_selectedTexture.destroy();
            });
        }
        break;
    }

    case TextureEditorContextObject::OpenMaterialBrowser: {
        QmlDesignerPlugin::instance()->mainWidget()->showDockWidget("MaterialBrowser", true);
        break;
    }
    }
}

namespace {

#ifdef QDS_USE_PROJECTSTORAGE
QUrl getSpecificsUrl(const NodeMetaInfos &prototypes, const SourcePathCacheInterface &pathCache)
{
    Utils::PathString specificsPath;

    for (const NodeMetaInfo &prototype : prototypes) {
        auto sourceId = prototype.propertyEditorPathId();
        if (sourceId) {
            auto path = pathCache.sourcePath(sourceId);
            if (path.endsWith("Specifics.qml")) {
                specificsPath = path;
                break;
            }
        }
    }

    return QUrl::fromLocalFile(specificsPath.toQString());
}
#endif

} // namespace

TextureEditorQmlBackend *TextureEditorView::getQmlBackend(const QUrl &qmlFileUrl)
{
    auto qmlFileName = qmlFileUrl.toString();
    TextureEditorQmlBackend *currentQmlBackend = m_qmlBackendHash.value(qmlFileName);

    if (!currentQmlBackend) {
        currentQmlBackend = new TextureEditorQmlBackend(this, m_imageCache);

        m_stackedWidget->addWidget(currentQmlBackend->widget());
        m_qmlBackendHash.insert(qmlFileName, currentQmlBackend);

        currentQmlBackend->setSource(qmlFileUrl);

        QObject *rootObj = currentQmlBackend->widget()->rootObject();
        QObject::connect(rootObj, SIGNAL(toolBarAction(int)), this, SLOT(handleToolBarAction(int)));
    }

    return currentQmlBackend;
}

QUrl TextureEditorView::getPaneUrl()
{
    QString panePath = textureEditorResourcesPath();
    if (m_selectedTexture.isValid() && m_hasQuick3DImport
        && (Utils3D::materialLibraryNode(this).isValid() || m_hasTextureRoot))
        panePath.append("/TextureEditorPane.qml");
    else {
        panePath.append("/EmptyTextureEditorPane.qml");
    }

    return QUrl::fromLocalFile(panePath);
}

void TextureEditorView::setupCurrentQmlBackend(const ModelNode &selectedNode,
                                               const QUrl &qmlSpecificsFile,
                                               const QString &specificQmlData)
{
    QmlModelState currState = currentStateNode();
    QString currStateName = currState.isBaseState() ? QStringLiteral("invalid state")
                                                    : currState.name();
    QmlObjectNode qmlObjectNode{selectedNode};
    m_qmlBackEnd->setup(qmlObjectNode, currStateName, qmlSpecificsFile, this);
    m_qmlBackEnd->contextObject()->setSpecificQmlData(specificQmlData);
}

void TextureEditorView::setupWidget()
{
    m_qmlBackEnd->widget()->installEventFilter(this);
    m_stackedWidget->setCurrentWidget(m_qmlBackEnd->widget());
    m_stackedWidget->setMinimumSize({400, 300});
}

void TextureEditorView::setupQmlBackend()
{
    QUrl qmlPaneUrl = getPaneUrl();
    QUrl qmlSpecificsUrl;
    QString specificQmlData;

#ifdef QDS_USE_PROJECTSTORAGE
    auto selfAndPrototypes = m_selectedTexture.metaInfo().selfAndPrototypes();
    bool isEditableComponent = m_selectedTexture.isComponent()
                               && !QmlItemNode(m_selectedTexture).isEffectItem();
    specificQmlData = m_propertyEditorComponentGenerator.create(selfAndPrototypes, isEditableComponent);
    qmlSpecificsUrl = getSpecificsUrl(selfAndPrototypes, model()->pathCache());
#else
    TypeName diffClassName;
    if (NodeMetaInfo metaInfo = m_selectedTexture.metaInfo()) {
        diffClassName = metaInfo.typeName();
        for (const NodeMetaInfo &metaInfo : metaInfo.selfAndPrototypes()) {
            if (PropertyEditorQmlBackend::checkIfUrlExists(qmlSpecificsUrl))
                break;
            qmlSpecificsUrl = PropertyEditorQmlBackend::getQmlFileUrl(metaInfo.typeName() + "Specifics", metaInfo);
            diffClassName = metaInfo.typeName();
        }

        if (diffClassName != m_selectedTexture.type()) {
            specificQmlData = PropertyEditorQmlBackend::templateGeneration(
                metaInfo, model()->metaInfo(diffClassName), m_selectedTexture);
        }
    }
#endif

    m_qmlBackEnd = getQmlBackend(qmlPaneUrl);
    setupCurrentQmlBackend(m_selectedTexture, qmlSpecificsUrl, specificQmlData);
    setupWidget();

    m_qmlBackEnd->contextObject()->setHasQuick3DImport(m_hasQuick3DImport);
    m_qmlBackEnd->contextObject()->setHasMaterialLibrary(Utils3D::materialLibraryNode(this).isValid());
    bool hasValidSelection = QmlObjectNode(m_selectedModel).hasBindingProperty("materials");
    m_qmlBackEnd->contextObject()->setHasSingleModelSelection(hasValidSelection);
    m_qmlBackEnd->contextObject()->setIsQt6Project(externalDependencies().isQt6Project());

    m_dynamicPropertiesModel->setSelectedNode(m_selectedTexture);
}

void TextureEditorView::commitVariantValueToModel(PropertyNameView propertyName, const QVariant &value)
{
    m_locked = true;
    executeInTransaction("TextureEditorView:commitVariantValueToModel", [&] {
        QmlObjectNode(m_selectedTexture).setVariantProperty(propertyName, value);
    });
    m_locked = false;
}

void TextureEditorView::commitAuxValueToModel(PropertyNameView propertyName, const QVariant &value)
{
    m_locked = true;

    PropertyNameView name = propertyName;
    name.chop(5);

    try {
        if (value.isValid())
            m_selectedTexture.setAuxiliaryData(AuxiliaryDataType::Document, name, value);
        else
            m_selectedTexture.removeAuxiliaryData(AuxiliaryDataType::Document, name);
    }
    catch (const Exception &e) {
        e.showException();
    }
    m_locked = false;
}

void TextureEditorView::removePropertyFromModel(PropertyNameView propertyName)
{
    m_locked = true;
    executeInTransaction("MaterialEditorView:removePropertyFromModel", [&] {
        QmlObjectNode(m_selectedTexture).removeProperty(propertyName);
    });
    m_locked = false;
}

bool TextureEditorView::noValidSelection() const
{
    QTC_ASSERT(m_qmlBackEnd, return true);
    return !QmlObjectNode::isValidQmlObjectNode(m_selectedTexture);
}

void TextureEditorView::asyncResetView()
{
    if (m_timerId)
        killTimer(m_timerId);
    m_timerId = startTimer(0);
}

void TextureEditorView::modelAttached(Model *model)
{
    AbstractView::modelAttached(model);

    if constexpr (useProjectStorage())
        m_propertyComponentGenerator.setModel(model);

    m_locked = true;

    m_hasQuick3DImport = model->hasImport("QtQuick3D");
    m_hasTextureRoot = rootModelNode().metaInfo().isQtQuick3DTexture();

    if (m_hasTextureRoot) {
        m_selectedTexture = rootModelNode();
    } else if (m_hasQuick3DImport) {
        // Creating the material library node on model attach causes errors as long as the type
        // information is not complete yet, so we keep checking until type info is complete.
        m_selectedTexture = Utils3D::selectedTexture(this);
    }

    if (!m_setupCompleted) {
        reloadQml();
        m_setupCompleted = true;
    }
    resetView();

    m_locked = false;
}

void TextureEditorView::modelAboutToBeDetached(Model *model)
{
    AbstractView::modelAboutToBeDetached(model);
    m_dynamicPropertiesModel->reset();
    if (m_qmlBackEnd) {
        m_qmlBackEnd->textureEditorTransaction()->end();
        m_qmlBackEnd->contextObject()->setHasMaterialLibrary(false);
    }
}

void TextureEditorView::propertiesRemoved(const QList<AbstractProperty> &propertyList)
{
    if (noValidSelection())
        return;

    for (const AbstractProperty &property : propertyList) {
        ModelNode node(property.parentModelNode());

        if (node.isRootNode())
            m_qmlBackEnd->contextObject()->setHasAliasExport(QmlObjectNode(m_selectedTexture).isAliasExported());

        auto propertyName = property.name().toByteArray();
        if (node == m_selectedTexture || QmlObjectNode(m_selectedTexture).propertyChangeForCurrentState() == node) {
            // TODO: workaround for bug QDS-8539. To be removed once it is fixed.
            if (node.metaInfo().property(property.name()).propertyType().isUrl()) {
                resetPuppet();
            } else {
                m_locked = true;

                const PropertyName propertyName = property.name().toByteArray();
                PropertyName convertedpropertyName = propertyName;

                convertedpropertyName.replace('.', '_');

                PropertyEditorValue *value = m_qmlBackEnd->propertyValueForName(
                    QString::fromUtf8(convertedpropertyName));

                if (value) {
                    value->resetValue();
                    m_qmlBackEnd
                        ->setValue(m_selectedTexture,
                                   propertyName,
                                   QmlObjectNode(m_selectedTexture).instanceValue(propertyName));
                }
                m_locked = false;
            }
        }

        if (propertyName == "materials"
            && (node == m_selectedModel
                || QmlObjectNode(m_selectedModel).propertyChangeForCurrentState() == node)) {
            m_qmlBackEnd->contextObject()->setHasSingleModelSelection(false);
        }

        dynamicPropertiesModel()->dispatchPropertyChanges(property);
    }
}

void TextureEditorView::variantPropertiesChanged(const QList<VariantProperty> &propertyList, PropertyChangeFlags /*propertyChange*/)
{
    if (noValidSelection())
        return;

    for (const VariantProperty &property : propertyList) {
        ModelNode node(property.parentModelNode());
        if (node == m_selectedTexture || QmlObjectNode(m_selectedTexture).propertyChangeForCurrentState() == node) {
            if (property.isDynamic())
                m_dynamicPropertiesModel->updateItem(property);
            auto propertyName = property.name().toByteArray();
            if (m_selectedTexture.property(propertyName).isBindingProperty()) {
                setValue(m_selectedTexture,
                         propertyName,
                         QmlObjectNode(m_selectedTexture).instanceValue(propertyName));
            } else {
                setValue(m_selectedTexture,
                         propertyName,
                         QmlObjectNode(m_selectedTexture).modelValue(propertyName));
            }
        }

        dynamicPropertiesModel()->dispatchPropertyChanges(property);
    }
}

void TextureEditorView::bindingPropertiesChanged(const QList<BindingProperty> &propertyList, PropertyChangeFlags /*propertyChange*/)
{
    if (noValidSelection())
        return;

    for (const BindingProperty &property : propertyList) {
        ModelNode node(property.parentModelNode());

        if (property.isAliasExport())
            m_qmlBackEnd->contextObject()->setHasAliasExport(QmlObjectNode(m_selectedTexture).isAliasExported());

        auto propertyName = property.name().toByteArray();

        if (node == m_selectedTexture || QmlObjectNode(m_selectedTexture).propertyChangeForCurrentState() == node) {
            if (property.isDynamic())
                m_dynamicPropertiesModel->updateItem(property);
            m_locked = true;
            QString exp = QmlObjectNode(m_selectedTexture).bindingProperty(property.name()).expression();
            m_qmlBackEnd->setExpression(property.name(), exp);
            m_locked = false;
        }

        if (propertyName == "materials"
            && (node == m_selectedModel
                || QmlObjectNode(m_selectedModel).propertyChangeForCurrentState() == node)) {
            bool hasMaterials = QmlObjectNode(m_selectedModel).hasBindingProperty("materials");
            m_qmlBackEnd->contextObject()->setHasSingleModelSelection(hasMaterials);
        }

        dynamicPropertiesModel()->dispatchPropertyChanges(property);
    }
}

void TextureEditorView::auxiliaryDataChanged(const ModelNode &node,
                                              AuxiliaryDataKeyView key,
                                              const QVariant &)
{
    if (!noValidSelection() && node.isSelected())
        m_qmlBackEnd->setValueforAuxiliaryProperties(m_selectedTexture, key);

    if (!m_hasTextureRoot) {
        if (key == Utils3D::matLibSelectedTextureProperty) {
            if (ModelNode selNode = Utils3D::selectedTexture(this)) {
                m_selectedTexture = selNode;
                m_dynamicPropertiesModel->setSelectedNode(m_selectedTexture);
                asyncResetView();
            }
        }
    }
}

void TextureEditorView::propertiesAboutToBeRemoved(const QList<AbstractProperty> &propertyList)
{
    for (const auto &property : propertyList)
        m_dynamicPropertiesModel->removeItem(property);
}

void TextureEditorView::nodeReparented(const ModelNode &node,
                                       [[maybe_unused]] const NodeAbstractProperty &newPropertyParent,
                                       [[maybe_unused]] const NodeAbstractProperty &oldPropertyParent,
                                       [[maybe_unused]] PropertyChangeFlags propertyChange)
{
    if (node.id() == Constants::MATERIAL_LIB_ID && m_qmlBackEnd && m_qmlBackEnd->contextObject()) {
        m_qmlBackEnd->contextObject()->setHasMaterialLibrary(true);
        asyncResetView();
    } else {
        if (!m_selectedTexture && node.metaInfo().isQtQuick3DTexture()
            && node.parentProperty().parentModelNode() == Utils3D::materialLibraryNode(this)) {
            ModelNode currentSelection = Utils3D::selectedTexture(this);
            if (currentSelection) {
                m_selectedTexture = currentSelection;
                asyncResetView();
            } else {
                QTimer::singleShot(0, this, [node]() {
                    Utils3D::selectTexture(node);
                });
            }
        }
    }
}

void TextureEditorView::nodeAboutToBeRemoved(const ModelNode &removedNode)
{
    if (removedNode.id() == Constants::MATERIAL_LIB_ID && m_qmlBackEnd && m_qmlBackEnd->contextObject()) {
        m_qmlBackEnd->contextObject()->setHasMaterialLibrary(false);
        asyncResetView();
    } else if (removedNode == m_selectedTexture) {
        ModelNode matLib = Utils3D::materialLibraryNode(this);
        QTC_ASSERT(matLib.isValid(), return);

        const QList<ModelNode> textures = matLib.directSubModelNodesOfType(
            model()->qtQuick3DTextureMetaInfo());
        bool selectedNodeFound = false;
        m_newSelectedTexture = {};
        for (const ModelNode &tex : textures) {
            if (selectedNodeFound) {
                m_newSelectedTexture = tex;
                break;
            }
            if (m_selectedTexture == tex)
                selectedNodeFound = true;
            else
                m_newSelectedTexture = tex;
        }
        m_selectedTextureChanged = true;
    }
}

void TextureEditorView::nodeRemoved([[maybe_unused]] const ModelNode &removedNode,
                                    [[maybe_unused]] const NodeAbstractProperty &parentProperty,
                                    [[maybe_unused]] PropertyChangeFlags propertyChange)
{
    if (m_selectedTextureChanged)
        asyncResetView();

    m_qmlBackEnd->refreshBackendModel();
}

void TextureEditorView::nodeIdChanged([[maybe_unused]] const ModelNode &node,
                                      [[maybe_unused]] const QString &newId,
                                      [[maybe_unused]] const QString &oldId)
{
    m_qmlBackEnd->refreshBackendModel();
}

bool TextureEditorView::hasWidget() const
{
    return true;
}

WidgetInfo TextureEditorView::widgetInfo()
{
    return createWidgetInfo(m_stackedWidget,
                            "TextureEditor",
                            WidgetInfo::RightPane,
                            tr("Texture Editor"),
                            tr("Texture Editor view"));
}

void TextureEditorView::selectedNodesChanged(const QList<ModelNode> &selectedNodeList,
                                             [[maybe_unused]] const QList<ModelNode> &lastSelectedNodeList)
{
    m_selectedModel = {};

    if (selectedNodeList.size() == 1 && selectedNodeList.at(0).metaInfo().isQtQuick3DModel())
        m_selectedModel = selectedNodeList.at(0);

    bool hasValidSelection = QmlObjectNode(m_selectedModel).hasBindingProperty("materials");
    if (m_qmlBackEnd)
        m_qmlBackEnd->contextObject()->setHasSingleModelSelection(hasValidSelection);
}

void TextureEditorView::currentStateChanged(const ModelNode &node)
{
    QmlModelState newQmlModelState(node);
    Q_ASSERT(newQmlModelState.isValid());
    resetView();
}

void TextureEditorView::instancePropertyChanged(const QList<QPair<ModelNode, PropertyName>> &propertyList)
{
    if (!m_selectedTexture.isValid() || !m_qmlBackEnd)
        return;

    m_locked = true;

    for (const QPair<ModelNode, PropertyName> &propertyPair : propertyList) {
        const ModelNode modelNode = propertyPair.first;
        const QmlObjectNode qmlObjectNode(modelNode);
        const PropertyName propertyName = propertyPair.second;

        if (qmlObjectNode.isValid() && modelNode == m_selectedTexture && qmlObjectNode.currentState().isValid()) {
            const AbstractProperty property = modelNode.property(propertyName);
            if (!modelNode.hasProperty(propertyName) || modelNode.property(property.name()).isBindingProperty())
                setValue(modelNode, property.name(), qmlObjectNode.instanceValue(property.name()));
            else
                setValue(modelNode, property.name(), qmlObjectNode.modelValue(property.name()));
        }
    }

    m_locked = false;
}

void TextureEditorView::importsChanged([[maybe_unused]] const Imports &addedImports,
                                       [[maybe_unused]] const Imports &removedImports)
{
    m_hasQuick3DImport = model()->hasImport("QtQuick3D");
    m_qmlBackEnd->contextObject()->setHasQuick3DImport(m_hasQuick3DImport);

    resetView();
}

void TextureEditorView::duplicateTexture(const ModelNode &texture)
{
    QTC_ASSERT(texture.isValid(), return);
    CreateTexture(this).execute(texture);
}

void TextureEditorView::customNotification([[maybe_unused]] const AbstractView *view,
                                           const QString &identifier,
                                           const QList<ModelNode> &nodeList,
                                           [[maybe_unused]] const QList<QVariant> &data)
{
    if (identifier == "add_new_texture")
        handleToolBarAction(TextureEditorContextObject::AddNewTexture);
    else if (identifier == "duplicate_texture")
        duplicateTexture(nodeList.first());
}

void QmlDesigner::TextureEditorView::highlightSupportedProperties(bool highlight)
{
    if (!m_selectedTexture.isValid())
        return;

    DesignerPropertyMap &propMap = m_qmlBackEnd->backendValuesPropertyMap();
    const QStringList propNames = propMap.keys();
    NodeMetaInfo metaInfo = m_selectedTexture.metaInfo();
    QTC_ASSERT(metaInfo.isValid(), return);

    for (const QString &propName : propNames) {
        if (metaInfo.property(propName.toUtf8()).propertyType().isQtQuick3DTexture()) {
            QObject *propEditorValObj = propMap.value(propName).value<QObject *>();
            PropertyEditorValue *propEditorVal = qobject_cast<PropertyEditorValue *>(propEditorValObj);
            propEditorVal->setHasActiveDrag(highlight);
        } else if (metaInfo.property(propName.toUtf8()).propertyType().isUrl()) {
            QObject *propEditorValObj = propMap.value(propName).value<QObject *>();
            PropertyEditorValue *propEditorVal = qobject_cast<PropertyEditorValue *>(propEditorValObj);
            if (propEditorVal)
                propEditorVal->setHasActiveDrag(highlight);
        }
    }
}

void TextureEditorView::dragStarted(QMimeData *mimeData)
{
    if (!mimeData->hasFormat(Constants::MIME_TYPE_ASSETS))
        return;

    const QString assetPath = QString::fromUtf8(mimeData->data(Constants::MIME_TYPE_ASSETS)).split(',')[0];
    QString assetType = AssetsLibraryWidget::getAssetTypeAndData(assetPath).first;

    // Currently only image assets have dnd-supported properties
    if (assetType != Constants::MIME_TYPE_ASSET_IMAGE
        && assetType != Constants::MIME_TYPE_ASSET_TEXTURE3D) {
        return;
    }

    highlightSupportedProperties();

    const QString suffix = "*." + assetPath.split('.').last().toLower();
    m_qmlBackEnd->contextObject()->setActiveDragSuffix(suffix);
}

void TextureEditorView::dragEnded()
{
    highlightSupportedProperties(false);
    m_qmlBackEnd->contextObject()->setActiveDragSuffix("");
}

// from model to texture editor
void TextureEditorView::setValue(const QmlObjectNode &qmlObjectNode,
                                 PropertyNameView name,
                                 const QVariant &value)
{
    m_locked = true;
    m_qmlBackEnd->setValue(qmlObjectNode, name, value);
    m_locked = false;
}

bool TextureEditorView::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::FocusOut) {
        if (m_qmlBackEnd && m_qmlBackEnd->widget() == obj)
            QMetaObject::invokeMethod(m_qmlBackEnd->widget()->rootObject(), "closeContextMenu");
    }
    return QObject::eventFilter(obj, event);
}

void TextureEditorView::reloadQml()
{
    m_qmlBackendHash.clear();
    while (QWidget *widget = m_stackedWidget->widget(0)) {
        m_stackedWidget->removeWidget(widget);
        delete widget;
    }
    m_qmlBackEnd = nullptr;

    resetView();
}

} // namespace QmlDesigner

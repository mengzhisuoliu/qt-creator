// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "abstractview.h"
#include "modelcache.h"
#include "qmldesigner_global.h"
#include "rewritertransaction.h"

#include <modelnode.h>
#include <nodeinstance.h>
#include <nodeinstanceclientinterface.h>
#include <nodeinstanceserverinterface.h>

#include <utils/filepath.h>

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QTime>
#include <QTimer>
#include <QtGui/qevent.h>

#include <memory>

QT_FORWARD_DECLARE_CLASS(QFileSystemWatcher)

namespace ProjectExplorer {
class Target;
}

namespace Utils {
class Process;
}

namespace QmlDesigner {

class NodeInstanceServerProxy;
class CreateSceneCommand;
class CreateInstancesCommand;
class ClearSceneCommand;
class ReparentInstancesCommand;
class ChangeFileUrlCommand;
class ChangeValuesCommand;
class ChangeBindingsCommand;
class ChangeIdsCommand;
class RemoveInstancesCommand;
class ChangeSelectionCommand;
class RemovePropertiesCommand;
class CompleteComponentCommand;
class InformationContainer;
class TokenCommand;
class ConnectionManagerInterface;
class ExternalDependenciesInterface;

class QMLDESIGNER_EXPORT NodeInstanceView : public AbstractView,
                                            public NodeInstanceClientInterface
{
    Q_OBJECT

    friend NodeInstance;

public:
    using Pointer = QWeakPointer<NodeInstanceView>;

    explicit NodeInstanceView(ConnectionManagerInterface &connectionManager,
                              ExternalDependenciesInterface &externalDependencies,
                              bool qsbEnabled = false);
    ~NodeInstanceView() override;

    void modelAttached(Model *model) override;
    void modelAboutToBeDetached(Model *model) override;
    void nodeCreated(const ModelNode &createdNode) override;
    void nodeAboutToBeRemoved(const ModelNode &removedNode) override;
    void propertiesAboutToBeRemoved(const QList<AbstractProperty>& propertyList) override;
    void variantPropertiesChanged(const QList<VariantProperty>& propertyList, PropertyChangeFlags propertyChange) override;
    void bindingPropertiesChanged(const QList<BindingProperty>& propertyList, PropertyChangeFlags propertyChange) override;
    void nodeReparented(const ModelNode &node, const NodeAbstractProperty &newPropertyParent,
                        const NodeAbstractProperty &oldPropertyParent,
                        AbstractView::PropertyChangeFlags propertyChange) override;
    void rootNodeTypeChanged(const QString &type, int majorVersion, int minorVersion) override;
    void nodeTypeChanged(const ModelNode& node, const TypeName &type, int majorVersion, int minorVersion) override;
    void fileUrlChanged(const QUrl &oldUrl, const QUrl &newUrl) override;
    void nodeIdChanged(const ModelNode& node, const QString& newId, const QString& oldId) override;
    void nodeOrderChanged(const NodeListProperty &listProperty) override;
    void importsChanged(const Imports &addedImports, const Imports &removedImports) override;
    void auxiliaryDataChanged(const ModelNode &node,
                              AuxiliaryDataKeyView key,
                              const QVariant &data) override;
    void customNotification(const AbstractView *view,
                            const QString &identifier,
                            const QList<ModelNode> &nodeList,
                            const QList<QVariant> &data) override;
    void customNotification(const CustomNotificationPackage &) override;
    void nodeSourceChanged(const ModelNode &modelNode, const QString &newNodeSource) override;
    void capturedData(const CapturedDataCommand &capturedData) override;
    void currentStateChanged(const ModelNode &node) override;
    void sceneCreated(const SceneCreatedCommand &command) override;

    QList<NodeInstance> instances() const;
    NodeInstance instanceForModelNode(const ModelNode &node) const ;
    bool hasInstanceForModelNode(const ModelNode &node) const;

    NodeInstance instanceForId(qint32 id) const;
    bool hasInstanceForId(qint32 id) const;

    QRectF sceneRect() const;

    NodeInstance activeStateInstance() const;

    void updateChildren(const NodeAbstractProperty &newPropertyParent);
    void updatePosition(const QList<VariantProperty>& propertyList);

    void valuesChanged(const ValuesChangedCommand &command) override;
    void valuesModified(const ValuesModifiedCommand &command) override;
    void pixmapChanged(const PixmapChangedCommand &command) override;
    void informationChanged(const InformationChangedCommand &command) override;
    void childrenChanged(const ChildrenChangedCommand &command) override;
    void statePreviewImagesChanged(const StatePreviewImageChangedCommand &command) override;
    void componentCompleted(const ComponentCompletedCommand &command) override;
    void token(const TokenCommand &command) override;
    void debugOutput(const DebugOutputCommand &command) override;

    QImage statePreviewImage(const ModelNode &stateNode) const;

    void setTarget(ProjectExplorer::Target *newTarget);
    ProjectExplorer::Target *target() const;

    void sendToken(const QString &token, int number, const QVector<ModelNode> &nodeVector);

    void selectionChanged(const ChangeSelectionCommand &command) override;

    void selectedNodesChanged(const QList<ModelNode> &selectedNodeList,
                              const QList<ModelNode> &lastSelectedNodeList) override;

    void view3DAction(View3DActionType type, const QVariant &value) override;
    void requestModelNodePreviewImage(const ModelNode &node,
                                      const ModelNode &renderNode,
                                      const QSize &size = {},
                                      const QByteArray &requestId = {}) const;

    void handlePuppetToCreatorCommand(const PuppetToCreatorCommand &command) override;

    QVariant previewImageDataForGenericNode(const ModelNode &modelNode,
                                            const ModelNode &renderNode,
                                            const QSize &size = {},
                                            const QByteArray &requestId = {}) const;
    QVariant previewImageDataForImageNode(const ModelNode &modelNode) const;

    void setCrashCallback(std::function<void()> crashCallback)
    {
        m_crashCallback = std::move(crashCallback);
    }

    void startNanotrace();
    void endNanotrace();

protected:
    void timerEvent(QTimerEvent *event) override;

    void emitInstancePropertyChange(const QList<QPair<ModelNode, PropertyName>> &propertyList);
    void emitInstanceErrorChange(const QVector<qint32> &instanceIds);
    void emitInstancesCompleted(const QVector<ModelNode> &nodeList);
    void emitInstanceInformationsChange(
        const QMultiHash<ModelNode, InformationName> &informationChangeHash);
    void emitInstancesRenderImageChanged(const QVector<ModelNode> &nodeList);
    void emitInstancesPreviewImageChanged(const QVector<ModelNode> &nodeList);
    void emitInstancesChildrenChanged(const QVector<ModelNode> &nodeList);

private: // functions
    void sendInputEvent(QEvent *e);
    void edit3DViewResized(const QSize &size);

    std::unique_ptr<NodeInstanceServerProxy> createNodeInstanceServerProxy();
    void activateState(const NodeInstance &instance);
    void activateBaseState();

    NodeInstance rootNodeInstance() const;

    NodeInstance loadNode(const ModelNode &node);

    void clearErrors();

    void removeAllInstanceNodeRelationships();

    void removeRecursiveChildRelationship(const ModelNode &removedNode);

    void insertInstanceRelationships(const NodeInstance &instance);
    void removeInstanceNodeRelationship(const ModelNode &node);

    void removeInstanceAndSubInstances(const ModelNode &node);

    void setStateInstance(const NodeInstance &stateInstance);
    void clearStateInstance();

    QMultiHash<ModelNode, InformationName> informationChanged(
        const QVector<InformationContainer> &containerVector);

    CreateSceneCommand createCreateSceneCommand();
    ClearSceneCommand createClearSceneCommand() const;
    CreateInstancesCommand createCreateInstancesCommand(const QList<NodeInstance> &instanceList) const;
    CompleteComponentCommand createComponentCompleteCommand(const QList<NodeInstance> &instanceList) const;
    ComponentCompletedCommand createComponentCompletedCommand(const QList<NodeInstance> &instanceList) const;
    ReparentInstancesCommand createReparentInstancesCommand(const QList<NodeInstance> &instanceList) const;
    ReparentInstancesCommand createReparentInstancesCommand(const ModelNode &node, const NodeAbstractProperty &newPropertyParent, const NodeAbstractProperty &oldPropertyParent) const;
    ChangeFileUrlCommand createChangeFileUrlCommand(const QUrl &fileUrl) const;
    ChangeValuesCommand createChangeValueCommand(const QList<VariantProperty> &propertyList) const;
    ChangeBindingsCommand createChangeBindingCommand(const QList<BindingProperty> &propertyList) const;
    ChangeIdsCommand createChangeIdsCommand(const QList<NodeInstance> &instanceList) const;
    RemoveInstancesCommand createRemoveInstancesCommand(const QList<ModelNode> &nodeList) const;
    ChangeSelectionCommand createChangeSelectionCommand(const QList<ModelNode> &nodeList) const;
    RemoveInstancesCommand createRemoveInstancesCommand(const ModelNode &node) const;
    RemovePropertiesCommand createRemovePropertiesCommand(const QList<AbstractProperty> &propertyList) const;
    RemoveSharedMemoryCommand createRemoveSharedMemoryCommand(const QString &sharedMemoryTypeName, quint32 keyNumber);
    RemoveSharedMemoryCommand createRemoveSharedMemoryCommand(const QString &sharedMemoryTypeName, const QList<ModelNode> &nodeList);

    void resetHorizontalAnchors(const ModelNode &node);
    void resetVerticalAnchors(const ModelNode &node);

    void restartProcess();
    void delayedRestartProcess();

    void handleCrash();
    void startPuppetTransaction();
    void endPuppetTransaction();

    // QML Puppet to creator command handlers
    void handlePuppetKeyPress(int key, Qt::KeyboardModifiers modifiers);

    struct ModelNodePreviewImageData {
        QDateTime time;
        QPixmap pixmap;
        QString type;
        QString id;
        QString info;
    };
    QVariant modelNodePreviewImageDataToVariant(const ModelNodePreviewImageData &imageData) const;
    void updatePreviewImageForNode(const ModelNode &modelNode,
                                   const QImage &image,
                                   const QByteArray &requestId);

    void updateWatcher(const QString &path);
    void handleShaderChanges();
    void handleQsbProcessExit(Utils::Process *qsbProcess, const QString &shader);
    void updateQsbPathToFilterMap();
    void updateRotationBlocks();
    void maybeResetOnPropertyChange(PropertyNameView name,
                                    const ModelNode &node,
                                    PropertyChangeFlags flags);

private:
    struct NodeInstanceCacheData
    {
        NodeInstanceCacheData(const QHash<ModelNode, NodeInstance> &i,
                              const QHash<ModelNode, QImage> &p)
            : instances(i)
            , previewImages(p)
        {}

        NodeInstanceCacheData() = default;

        QHash<ModelNode, NodeInstance> instances;
        QHash<ModelNode, QImage> previewImages;
    };

    QList<NodeInstance> loadInstancesFromCache(const QList<ModelNode> &nodeList,
                                               const NodeInstanceCacheData &cache);

    QString fullyQualifyPropertyIfApplies(const BindingProperty &property) const;

    mutable QHash<QString, ModelNodePreviewImageData> m_imageDataMap;

    NodeInstance m_rootNodeInstance;
    NodeInstance m_activeStateInstance;
    QHash<ModelNode, NodeInstance> m_nodeInstanceHash;
    ModelCache<NodeInstanceCacheData> m_nodeInstanceCache;
    QHash<ModelNode, QImage> m_statePreviewImage;
    ConnectionManagerInterface &m_connectionManager;
    ExternalDependenciesInterface &m_externalDependencies;
    std::unique_ptr<NodeInstanceServerProxy> m_nodeInstanceServer;
    QImage m_baseStatePreviewImage;
    QElapsedTimer m_lastCrashTime;
    QPointer<ProjectExplorer::Target> m_currentTarget;
    int m_restartProcessTimerId;
    RewriterTransaction m_puppetTransaction;

    // key: fileUrl value: (key: instance qml id, value: related tool states)
    QHash<QUrl, QHash<QString, QVariantMap>> m_edit3DToolStates;

    std::function<void()> m_crashCallback{[this] { handleCrash(); }};

    // We use QFileSystemWatcher directly instead of Utils::FileSystemWatcher as we want
    // shader changes to be applied immediately rather than requiring reactivation of
    // the creator application.
    QFileSystemWatcher *m_fileSystemWatcher;
    QTimer m_resetTimer;
    QTimer m_updateWatcherTimer;
    QTimer m_generateQsbFilesTimer;
    Utils::FilePath m_qsbPath;
    QSet<QString> m_pendingUpdateDirs;
    QHash<QString, bool> m_qsbTargets; // Value indicates if target is pending qsb generation
    QHash<QString, QStringList> m_qsbPathToFilterMap;
    int m_remainingQsbTargets = 0;
    QTimer m_rotBlockTimer;
    bool m_qsbEnabled = false;
};

} // namespace ProxyNodeInstanceView

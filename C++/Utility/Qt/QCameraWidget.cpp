#include "QCameraWidget.h"
#ifdef QT_GUI_LIB
#include <QToolButton>
#include <QAction>
#include <QDebug>
#include <QLoggingCategory>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
Q_LOGGING_CATEGORY(cameraDiagnosticLog, "diagnostics.Camera", QtInfoMsg)
#include <QHBoxLayout>
#include <QMetaObject>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QThread>
#include <QVariant>
#include <exception>
#include <memory>

namespace
{
void expandToDepth(QTreeWidgetItem* item, const int depth, const int maxExpandedDepth)
{
    if(!item) return;

    item->setExpanded(depth <= maxExpandedDepth);
    for(int childIndex = 0; childIndex < item->childCount(); ++childIndex){
        expandToDepth(item->child(childIndex), depth + 1, maxExpandedDepth);
    }
}

}

QCameraWidget::QCameraWidget(QWidget *parent, Camera *camera) : QWidget(parent), _camera(camera)
{
    setWindowTitle("Basler pylon Camera Configuration");
    setMinimumSize(300, 350);
    // Create the camera list combobox
    _cameraListComboBox = new QComboBox;
    _cameraListComboBox->setMinimumWidth(120);
    _cameraListComboBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Create the features widget
    _featuresWidget = new QTreeWidget;
    _featuresWidget->setHeaderLabels(QStringList() << "Feature" << "Value");
    _featuresWidget->setObjectName(QStringLiteral("CameraFeaturesTree"));
    _featuresWidget->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));

    // Create the toolbuttons
    _toolRefresh = new QToolButton(this);
    _toolRefresh->setIcon(QIcon(":/Resources/Icons/icons8-refresh-48.png"));
    _toolRefresh->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolRefresh->setIconSize(QSize(16, 16));
    _toolConnect = new QToolButton(this);
    _toolConnect->setCheckable(true);
    _toolConnect->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolConnect->setIconSize(QSize(16, 16));
    {
        QIcon icon;
        icon.addFile(":/Resources/Icons/icons8-connect-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/Icons/icons8-disconnected-48.png", QSize(), QIcon::Normal, QIcon::On);
        _toolConnect->setIcon(icon);
    }
    _toolGrabOne = new QToolButton(this);
    _toolGrabOne->setIcon(QIcon(":/Resources/Icons/icons8-camera-48.png"));
    _toolGrabOne->setEnabled(false);
    _toolGrabOne->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolGrabOne->setIconSize(QSize(16, 16));
    _toolGrabLive = new QToolButton(this);
    _toolGrabLive->setCheckable(true);
    _toolGrabLive->setEnabled(false);
    _toolGrabLive->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolGrabLive->setIconSize(QSize(16, 16));
    {
        QIcon icon;
        icon.addFile(":/Resources/Icons/icons8-cameras-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/Icons/icons8-pause-48.png", QSize(), QIcon::Normal, QIcon::On);
        _toolGrabLive->setIcon(icon);
    }

    QHBoxLayout *cameraListLayout = new QHBoxLayout;
    cameraListLayout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
    cameraListLayout->addWidget(_cameraListComboBox);
    cameraListLayout->addWidget(_toolRefresh);

    QHBoxLayout *toolButtonLayout = new QHBoxLayout;
    toolButtonLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolButtonLayout->addWidget(_toolConnect);
    toolButtonLayout->addWidget(_toolGrabOne);
    toolButtonLayout->addWidget(_toolGrabLive);

    auto *listAndButtonLayout = new QHBoxLayout;
    listAndButtonLayout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
    listAndButtonLayout->addLayout(cameraListLayout);
    listAndButtonLayout->addLayout(toolButtonLayout);

    auto *featuresWidgetLayout = new QVBoxLayout;
    featuresWidgetLayout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
    featuresWidgetLayout->addWidget(_featuresWidget);

    auto *layout = new QVBoxLayout;
    layout->setObjectName(QStringLiteral("DeviceRootLayout"));
    layout->addLayout(listAndButtonLayout);
    layout->addLayout(featuresWidgetLayout);

    _statusBar = new QStatusBar(this);
    _statusBar->setObjectName(QStringLiteral("CameraStatusBar"));
    _statusBar->setSizeGripEnabled(false);

    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("CameraStatusLabel"));
    _statusLabel->setAlignment(Qt::AlignCenter);
    _statusBar->addWidget(_statusLabel);

    _nodeUpdateTimer = new QTimer(this);
    _nodeUpdateTimer->setSingleShot(true);
    _nodeUpdateTimer->setInterval(100);
    connect(_nodeUpdateTimer, &QTimer::timeout, this, &QCameraWidget::drainNodeUpdates);

    layout->addWidget(_statusBar);
    setLayout(layout);

    updateGrabState(false);
    applyConnectionState(_camera && _camera->isOpened());

    if(!_camera){
        _cameraListComboBox->setEnabled(false);
        _toolRefresh->setEnabled(false);
        _toolConnect->setEnabled(false);
        _toolGrabOne->setEnabled(false);
        _toolGrabLive->setEnabled(false);
        logMessage(tr("Camera instance is not configured."), true);
        return;
    }

    QPointer<QCameraWidget> guard(this);

    // Configure the details of toolbuttons
    connect(_toolRefresh, &QToolButton::clicked, this, [=]{
        startRefreshOperation();
    });
    _statusCallbackId = _camera->registerStatusCallback([guard](Camera::Status status, bool on){
        if(!guard) return;
        QMetaObject::invokeMethod(guard, [guard, status, on]{
            if(!guard) return;
            switch(status){
            case Camera::GrabbingStatus:{
                if(on){ // Grabbing started
                    QSignalBlocker grabbingBlock(guard->_toolGrabLive);
                    guard->_toolGrabLive->setChecked(true);
                }else{ // Grabbing stopped
                    QSignalBlocker grabbingBlock(guard->_toolGrabLive);
                    guard->_toolGrabLive->setChecked(false);
                }
                guard->updateGrabState(on);
            }break;
            case Camera::ConnectionStatus:{
                if(guard->_connectionOperationActive) return;
                guard->applyConnectionState(on);
            }break;
            }
        }, Qt::QueuedConnection);
    });
    _nodeCallbackId = _camera->registerNodeUpdatedCallback([guard](const std::string& nodeName){
        if(!guard) return;
        if(nodeName.empty()) return;
        guard->enqueueNodeUpdate(QString::fromStdString(nodeName));
    });
    connect(_toolConnect, &QToolButton::toggled, this, [=](bool toggled){
        // Request to open the camera
        if(toggled){
            startConnectionOperation(true, _cameraListComboBox->currentText());
        }else{
            startConnectionOperation(false);
        }
    });
    connect(_toolGrabOne, &QToolButton::clicked, this, [=]{
        // Request to start a single grabbing
        _camera->grab(1);
        logMessage(tr("Single grab triggered."), false);
    });
    connect(_toolGrabLive, &QToolButton::toggled, this, [=](bool toggled){
        // Request to start a continuous grabbing
        if(toggled) {
            _camera->grab();
            logMessage(tr("Live grabbing started."), false);
        } else {
            _camera->requestStop();
            logMessage(tr("Live grabbing stopped."), false);
        }
    });

    if (_camera) {
        _cameraListComboBox->clear();
        for (const auto& cameraName : _camera->getCachedCameraList()) {
            _cameraListComboBox->addItem(QString::fromStdString(cameraName));
        }
        if (_camera->isOpened()) {
            _cameraListComboBox->setCurrentText(QString::fromStdString(_camera->getConnectedCameraName()));
        }
        updateCameraSelectorState();
    }
}

QCameraWidget::~QCameraWidget()
{
    prepareForShutdown();
}

void QCameraWidget::setDiscoveredCameraNames(const QStringList& cameraNames)
{
    if (_shuttingDown || !_cameraListComboBox) return;
    const QSignalBlocker blocker(_cameraListComboBox);
    _cameraListComboBox->clear();
    _cameraListComboBox->addItems(cameraNames);
    updateCameraSelectorState();
}

void QCameraWidget::updateCameraSelectorState()
{
    if (!_cameraListComboBox) return;

    const bool enabled = _camera
        && !_shuttingDown
        && !_connectionOperationActive
        && !_refreshOperationActive
        && !_camera->isOpened()
        && _cameraListComboBox->count() > 0;
    _cameraListComboBox->setEnabled(enabled);
}

void QCameraWidget::prepareForShutdown()
{
    _shuttingDown.store(true, std::memory_order_release);
    if(_nodeUpdateTimer) _nodeUpdateTimer->stop();
    {
        std::lock_guard<std::mutex> lock(_nodeUpdateMutex);
        _pendingNodeUpdates.clear();
        _nodeUpdateDrainScheduled = false;
    }

    if(_connectionThread){
        _connectionThread->wait();
        _connectionThread = nullptr;
    }

    if(_refreshThread){
        _refreshThread->wait();
        _refreshThread = nullptr;
    }

    const auto parameterThreads = _parameterThreads;
    for(QThread* worker : parameterThreads){
        if(worker){
            worker->wait();
        }
    }
    _parameterThreads.clear();

    if(_camera){
        if(_statusCallbackId != 0){
            _camera->deregisterStatusCallback(_statusCallbackId);
            _statusCallbackId = 0;
        }
        if(_nodeCallbackId != 0){
            _camera->deregisterNodeUpdatedCallback(_nodeCallbackId);
            _nodeCallbackId = 0;
        }
    }
    _camera = nullptr;
}

void QCameraWidget::startConnectionOperation(const bool open, const QString& cameraName)
{
    if(!_camera || _shuttingDown || _connectionThread) return;

    const std::string selectedCameraName = cameraName.toStdString();
    const auto result = std::make_shared<bool>(false);
    auto* worker = QThread::create([camera = _camera, open, selectedCameraName, result]{
        if(open){
            *result = camera->open(selectedCameraName);
        }else{
            camera->close();
            *result = true;
        }
    });

    _connectionThread = worker;
    worker->setParent(this);

    setConnectionOperationActive(true);
    logMessage(open ? tr("Connecting camera...") : tr("Disconnecting camera..."), false);

    QPointer<QCameraWidget> guard(this);
    connect(worker, &QThread::finished, this, [guard, worker, open, result]{
        if(!guard) return;

        if(guard->_connectionThread == worker){
            guard->_connectionThread = nullptr;
        }
        worker->deleteLater();

        guard->setConnectionOperationActive(false);
        const bool opened = guard->_camera && guard->_camera->isOpened();
        guard->applyConnectionState(opened);

        if(open){
            if(*result && opened){
                guard->logMessage(tr("Camera connected successfully."), false);
            }else{
                guard->logMessage(tr("Camera connection failed."), true);
            }
        }else{
            guard->logMessage(tr("Camera disconnected successfully."), false);
        }
    });
    worker->start();
}

void QCameraWidget::setConnectionOperationActive(const bool active)
{
    if(_shuttingDown) return;

    _connectionOperationActive = active;

    const bool opened = _camera && _camera->isOpened();
    _toolConnect->setEnabled(!active);
    updateCameraSelectorState();
    _toolRefresh->setEnabled(!active && !opened);
    _toolGrabOne->setEnabled(!active && opened);
    _toolGrabLive->setEnabled(!active && opened);

    updateStatusLabel();
}

void QCameraWidget::startRefreshOperation()
{
    if (!_camera || _shuttingDown || _refreshThread) return;

    setRefreshOperationActive(true);
    logMessage(tr("Scanning for cameras..."), false);

    struct RefreshResult {
        std::vector<std::string> cameraList;
        std::string connectedName;
        bool isOpened = false;
    };
    const auto result = std::make_shared<RefreshResult>();

    auto* worker = QThread::create([camera = _camera, result] {
        if (camera) {
            result->cameraList = camera->getUpdatedCameraList();
            result->isOpened = camera->isOpened();
            if (result->isOpened) {
                result->connectedName = camera->getConnectedCameraName();
            }
        }
    });

    _refreshThread = worker;
    worker->setParent(this);

    QPointer<QCameraWidget> guard(this);
    connect(worker, &QThread::finished, this, [guard, worker, result] {
        if (!guard) return;

        if (guard->_refreshThread == worker) {
            guard->_refreshThread = nullptr;
        }
        worker->deleteLater();

        if (guard->_camera) {
            guard->_cameraListComboBox->clear();
            for (const auto& cameraName : result->cameraList) {
                guard->_cameraListComboBox->addItem(QString::fromStdString(cameraName));
            }
            if (result->isOpened) {
                guard->_cameraListComboBox->setCurrentText(QString::fromStdString(result->connectedName));
            }
        }

        guard->setRefreshOperationActive(false);
        guard->logMessage(tr("Camera list updated."), false);
    });
    worker->start();
}

void QCameraWidget::setRefreshOperationActive(const bool active)
{
    if (_shuttingDown) return;

    _refreshOperationActive = active;

    _toolRefresh->setEnabled(!active);
    updateCameraSelectorState();
    _toolConnect->setEnabled(!active);

    updateStatusLabel();
}

void QCameraWidget::applyConnectionState(const bool opened)
{
    if(_shuttingDown) return;

    if(opened){

    }

    {
        QSignalBlocker connectBlock(_toolConnect);
        _toolConnect->setChecked(opened);
    }

    updateCameraSelectorState();
    _toolRefresh->setEnabled(!opened);
    _toolConnect->setEnabled(true);
    _toolGrabOne->setEnabled(opened);
    _toolGrabLive->setEnabled(opened);

    updateStatusLabel();

    if(opened){
        rebuildFeaturesIfReady();
    }else{
        {
            QSignalBlocker grabBlock(_toolGrabLive);
            _toolGrabLive->setChecked(false);
        }
        updateGrabState(false);
        _featuresWidget->clear();
    }
}

bool QCameraWidget::isCameraReady() const
{
    return _camera && !_shuttingDown && _camera->isOpened();
}

GenApi::INode* QCameraWidget::resolveNode(const QString& nodeName) const
{
    if(nodeName.isEmpty() || !isCameraReady()) return nullptr;

    try{
        return _camera->getNodeMap().GetNode(nodeName.toStdString().c_str());
    }catch(const Pylon::GenericException&){
        return nullptr;
    }
}

void QCameraWidget::rebuildFeaturesIfReady()
{
    if(!isCameraReady()) return;

    try{
        generateFeaturesWidget(_camera->getNodeMap());
    }catch(const Pylon::GenericException&){
        _featuresWidget->clear();
    }
}

void QCameraWidget::generateFeaturesWidget(GenApi::INodeMap &nodemap)
{
    QSet<QString> expandedNodeNames;
    for(int i = 0; i < _featuresWidget->topLevelItemCount(); ++i){
        collectExpandedNodeNames(_featuresWidget->topLevelItem(i), expandedNodeNames);
    }

    QString selectedNodeName;
    if(auto* currentItem = _featuresWidget->currentItem()){
        selectedNodeName = currentItem->data(0, Qt::UserRole).toString();
    }
    const int scrollValue = _featuresWidget->verticalScrollBar()->value();

    _featuresWidget->clear();
    try{
        GenApi::NodeList_t nodes;
        nodemap.GetNodes(nodes);
        if (cameraDiagnosticLog().isDebugEnabled() && !_grabbing.load(std::memory_order_acquire)) {
            QElapsedTimer timing; timing.start();
            QJsonArray snapshot;
            for (auto* node : nodes) {
                QJsonObject record{{"name", QString::fromUtf8(node->GetName().c_str())}};
                try {
                    record.insert("readable", GenApi::IsReadable(node));
                    record.insert("writable", GenApi::IsWritable(node));
                    record.insert("kind", static_cast<int>(node->GetPrincipalInterfaceType()));
                    switch (node->GetPrincipalInterfaceType()) {
                    case GenApi::intfIInteger: case GenApi::intfIFloat: case GenApi::intfIBoolean:
                    case GenApi::intfIEnumeration: case GenApi::intfIString:
                        if (GenApi::IsReadable(node)) {
                            record.insert("value", QString::fromUtf8(GenApi::CValuePtr(node)->ToString(false, true).c_str()));
                            record.insert("outcome", "read");
                        } else record.insert("outcome", "unreadable");
                        break;
                    default: record.insert("outcome", "non-scalar-not-read"); break;
                    }
                } catch (const GenericException& error) {
                    record.insert("outcome", "read-failed"); record.insert("error", QString::fromUtf8(error.what()));
                }
                snapshot.append(record);
            }
            qCDebug(cameraDiagnosticLog).noquote() << "@diagnostic " + QString::fromUtf8(QJsonDocument(QJsonObject{
                {"event", "feature_snapshot"}, {"fields", QJsonObject{
                {"camera", QString::fromStdString(_camera->getConnectedCameraName())},
                {"coverage", "all-node-map-scalars/current-selector-state"},
                {"elapsedMs", timing.elapsed()}, {"nodes", snapshot}}}}).toJson(QJsonDocument::Compact));
        }

        QTreeWidgetItem *cameraFeatures = new QTreeWidgetItem(_featuresWidget, QStringList() << _camera->getConnectedCameraName().c_str());
        cameraFeatures->setData(0, Qt::UserRole, QStringLiteral("__camera_root__"));
        for(auto cat : nodes){
            if(cat->GetName() == "Root") continue;
            if(!GenApi::IsAvailable(cat)) continue;
            if(cat->GetPrincipalInterfaceType() != GenApi::EInterfaceType::intfICategory) continue;

            GenApi::NodeList_t parentsList;
            cat->GetParents(parentsList);
            if(!parentsList.empty() && parentsList.at(0)->GetDisplayName() == "Events Generation") continue;

            QTreeWidgetItem* item = new QTreeWidgetItem(cameraFeatures, QStringList() << cat->GetDisplayName().c_str());
            item->setData(0, Qt::UserRole, QString::fromStdString(cat->GetName().c_str()));

            GenApi::NodeList_t children;
            cat->GetChildren(children);
            generateChildrenItem(item, children);
        }
        if(expandedNodeNames.isEmpty()){
            expandToDepth(cameraFeatures, 0, 0);
        }else{
            restoreExpandedNodeNames(cameraFeatures, expandedNodeNames);
        }

        if(selectedNodeName.isEmpty()){
            _featuresWidget->setCurrentItem(cameraFeatures);
        }else{
            const auto selectedItems = findItemsByNodeName(selectedNodeName);
            if(!selectedItems.isEmpty()) _featuresWidget->setCurrentItem(selectedItems.front());
        }

        _featuresWidget->verticalScrollBar()->setValue(scrollValue);
    }catch(const GenericException &e){
        qDebug() << e.what();
    }
}

void QCameraWidget::generateChildrenItem(QTreeWidgetItem *parent, GenApi::NodeList_t children)
{
    for(auto sub : children){
        if(!GenApi::IsAvailable(sub)) continue;
        if(sub->GetAccessMode() == GenApi::WO && sub->GetPrincipalInterfaceType() != GenApi::intfICommand) continue;

        auto nodeWidget = createNodeWidget(sub);
        if(!nodeWidget) continue;

        QTreeWidgetItem* subItem = new QTreeWidgetItem(parent, QStringList() << sub->GetDisplayName().c_str());
        subItem->setData(0, Qt::UserRole, QString::fromStdString(sub->GetName().c_str()));
        _featuresWidget->setItemWidget(subItem, 1, nodeWidget);
    }
}

QList<QTreeWidgetItem*> QCameraWidget::findItemsByNodeName(const QString& nodeName) const
{
    QList<QTreeWidgetItem*> matches;
    for(int i = 0; i < _featuresWidget->topLevelItemCount(); ++i){
        auto* top = _featuresWidget->topLevelItem(i);
        QList<QTreeWidgetItem*> stack{top};
        while(!stack.isEmpty()){
            auto* current = stack.takeLast();
            if(current->data(0, Qt::UserRole).toString() == nodeName){
                matches.append(current);
            }
            for(int childIndex = 0; childIndex < current->childCount(); ++childIndex){
                stack.append(current->child(childIndex));
            }
        }
    }
    return matches;
}

bool QCameraWidget::refreshNodeWidget(GenApi::INode *node)
{
    if(!node) return false;

    const auto items = findItemsByNodeName(QString::fromStdString(node->GetName().c_str()));
    if(items.isEmpty()) return false;

    bool handled = false;
    for(const auto item : items){
        auto cur = _featuresWidget->itemWidget(item, 1);
        switch(node->GetPrincipalInterfaceType()){
        case GenApi::intfIInteger:{
            if(auto* spinBox = qobject_cast<QSpinBox*>(cur)){
                GenApi::CIntegerPtr ptr = node;

                QSignalBlocker block(spinBox);
                spinBox->setEnabled(GenApi::IsWritable(ptr));
                spinBox->setRange(ptr->GetMin(), ptr->GetMax());
                spinBox->setValue(ptr->GetValue());
                handled = true;
            }
        } break;
        case GenApi::intfIFloat:{
            if(auto* spinBox = qobject_cast<QDoubleSpinBox*>(cur)){
                GenApi::CFloatPtr ptr = node;

                QSignalBlocker block(spinBox);
                spinBox->setEnabled(GenApi::IsWritable(ptr));
                spinBox->setRange(ptr->GetMin(), ptr->GetMax());
                spinBox->setValue(ptr->GetValue());
                handled = true;
            }
        } break;
        case GenApi::intfIBoolean:{
            if(auto* checkBox = qobject_cast<QCheckBox*>(cur)){
                GenApi::CBooleanPtr ptr = node;

                QSignalBlocker block(checkBox);
                checkBox->setEnabled(GenApi::IsWritable(ptr));
                checkBox->setChecked(ptr->GetValue());
                handled = true;
            }
        } break;
        case GenApi::intfIString:{
            if(auto *lineEdit = qobject_cast<QLineEdit*>(cur)){
                GenApi::CStringPtr ptr = node;

                QSignalBlocker block(lineEdit);
                lineEdit->setEnabled(GenApi::IsWritable(ptr));
                lineEdit->setText(ptr->GetValue().c_str());
                handled = true;
            }
        } break;
        case GenApi::intfIEnumeration:{
            if(auto *comboBox = qobject_cast<QComboBox*>(cur)){
                GenApi::CEnumerationPtr ptr = node;

                QSignalBlocker block(comboBox);
                comboBox->setEnabled(GenApi::IsWritable(ptr));
                comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
                handled = true;
            }
        } break;
        case GenApi::intfICommand:{
            if(auto *button = qobject_cast<QPushButton*>(cur)){
                GenApi::CCommandPtr ptr = node;

                button->setEnabled(GenApi::IsWritable(ptr));
                handled = true;
            }
        } break;
        case GenApi::intfIRegister:
        case GenApi::intfICategory:
        case GenApi::intfIEnumEntry:
        case GenApi::intfIPort:
        case GenApi::intfIValue:
        case GenApi::intfIBase:
            break;
        }
    }

    return handled;
}

void QCameraWidget::enqueueNodeUpdate(const QString& nodeName)
{
    if(nodeName.isEmpty() || _shuttingDown.load(std::memory_order_acquire)) return;
    if(_grabbing.load(std::memory_order_acquire)){
        _suppressedGrabNodeUpdates.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    bool postDrain = false;
    {
        std::lock_guard<std::mutex> lock(_nodeUpdateMutex);
        if(_shuttingDown.load(std::memory_order_acquire)) return;
        _pendingNodeUpdates.insert(nodeName);
        if(!_nodeUpdateDrainScheduled){
            _nodeUpdateDrainScheduled = true;
            postDrain = true;
        }
    }
    if(!postDrain) return;

    const QPointer<QCameraWidget> guard(this);
    QMetaObject::invokeMethod(this, [guard]{
        if(guard) guard->scheduleNodeUpdateDrain();
    }, Qt::QueuedConnection);
}

void QCameraWidget::scheduleNodeUpdateDrain()
{
    if(_shuttingDown.load(std::memory_order_acquire)) return;
    if(_grabbing.load(std::memory_order_acquire)){
        std::lock_guard<std::mutex> lock(_nodeUpdateMutex);
        _suppressedGrabNodeUpdates.fetch_add(
            static_cast<std::size_t>(_pendingNodeUpdates.size()),
            std::memory_order_relaxed);
        _pendingNodeUpdates.clear();
        _nodeUpdateDrainScheduled = false;
        return;
    }
    if(_nodeUpdateTimer && !_nodeUpdateTimer->isActive()) _nodeUpdateTimer->start();
}

void QCameraWidget::drainNodeUpdates()
{
    QSet<QString> updates;
    {
        std::lock_guard<std::mutex> lock(_nodeUpdateMutex);
        updates.swap(_pendingNodeUpdates);
        _nodeUpdateDrainScheduled = false;
    }

    if(_shuttingDown.load(std::memory_order_acquire) || updates.isEmpty()) return;
    if(_grabbing.load(std::memory_order_acquire)){
        _suppressedGrabNodeUpdates.fetch_add(
            static_cast<std::size_t>(updates.size()),
            std::memory_order_relaxed);
        return;
    }

    for(const QString& nodeName : updates) handleNodeUpdated(nodeName);
}

void QCameraWidget::handleNodeUpdated(const QString& nodeName)
{
    try{
        auto* node = resolveNode(nodeName);
        if(!node || !GenApi::IsAvailable(node)){
            scheduleFeaturesRebuild();
            return;
        }

        if(!refreshNodeWidget(node)){
            scheduleFeaturesRebuild();
        }
    }catch(const GenericException&){
        scheduleFeaturesRebuild();
    }
}

void QCameraWidget::scheduleFeaturesRebuild()
{
    if(_shuttingDown.load(std::memory_order_acquire) || !isCameraReady()) return;
    if(_grabbing.load(std::memory_order_acquire)){
        return;
    }
    if(_rebuildScheduled) return;

    _rebuildScheduled = true;
    QTimer::singleShot(50, this, [this]{
        if(_shuttingDown.load(std::memory_order_acquire)) return;
        _rebuildScheduled = false;
        if(_grabbing.load(std::memory_order_acquire)){
            return;
        }
        rebuildFeaturesIfReady();
    });
}

void QCameraWidget::collectExpandedNodeNames(QTreeWidgetItem *item, QSet<QString>& expandedNodeNames) const
{
    if(!item) return;

    const auto nodeName = item->data(0, Qt::UserRole).toString();
    if(item->isExpanded() && !nodeName.isEmpty()){
        expandedNodeNames.insert(nodeName);
    }

    for(int childIndex = 0; childIndex < item->childCount(); ++childIndex){
        collectExpandedNodeNames(item->child(childIndex), expandedNodeNames);
    }
}

void QCameraWidget::restoreExpandedNodeNames(QTreeWidgetItem *item, const QSet<QString>& expandedNodeNames)
{
    if(!item) return;

    const auto nodeName = item->data(0, Qt::UserRole).toString();
    if(expandedNodeNames.contains(nodeName)){
        item->setExpanded(true);
    }

    for(int childIndex = 0; childIndex < item->childCount(); ++childIndex){
        restoreExpandedNodeNames(item->child(childIndex), expandedNodeNames);
    }
}

QWidget *QCameraWidget::createNodeWidget(GenApi::INode *node)
{
    QWidget *widget = nullptr;
    const QString nodeName = QString::fromStdString(node->GetName().c_str());
    switch(node->GetPrincipalInterfaceType()){
    case GenApi::intfIInteger:{
        GenApi::CIntegerPtr ptr = node;
        auto spinBox = new QSpinBox;
        widget = spinBox;
        try{
            QSignalBlocker block(spinBox);
            spinBox->setSingleStep(ptr->GetInc());
            spinBox->setRange(ptr->GetMin(), ptr->GetMax());
            spinBox->setValue(ptr->GetValue());
        }catch (const Pylon::GenericException &e){
            logMessage(e.GetDescription(), true);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value){
            spinBox->setEnabled(false);
            auto* errorMsg = new QString();
            runAsyncWrite(
                [=]() {
                    auto* currentNode = resolveNode(nodeName);
                    if(!currentNode) return false;
                    try {
                        GenApi::CIntegerPtr ptr = currentNode;
                        ptr->SetValue(value);
                        return true;
                    } catch(const Pylon::GenericException &e) {
                        *errorMsg = QString::fromStdString(e.GetDescription());
                        return false;
                    }
                },
                [=](bool success) {
                    spinBox->setEnabled(true);
                    if (!success) {
                        QSignalBlocker block(spinBox);
                        auto* currentNode = resolveNode(nodeName);
                        if (currentNode) {
                            try {
                                GenApi::CIntegerPtr ptr = currentNode;
                                if (GenApi::IsReadable(ptr)) {
                                    spinBox->setValue(ptr->GetValue());
                                }
                            } catch(...) {}
                        }
                        logMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(*errorMsg), true);
                    } else {
                        logMessage(tr("Parameter '%1' updated to %2.").arg(nodeName).arg(value), false);
                        scheduleFeaturesRebuild();
                    }
                    delete errorMsg;
                }
            );
        });
    } break;
    case GenApi::intfIFloat:{
        GenApi::CFloatPtr ptr = node;
        auto spinBox = new QDoubleSpinBox;
        widget = spinBox;
        try{
            QSignalBlocker block(spinBox);
            spinBox->setSingleStep(0.1);
            spinBox->setRange(ptr->GetMin(), ptr->GetMax());
            spinBox->setValue(ptr->GetValue());
        }catch (const Pylon::GenericException &e){
            logMessage(e.GetDescription(), true);
            qWarning() << e.GetDescription() << node->GetName().c_str() << "Float";
        }
        connect(spinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double value){
            spinBox->setEnabled(false);
            auto* errorMsg = new QString();
            runAsyncWrite(
                [=]() {
                    auto* currentNode = resolveNode(nodeName);
                    if(!currentNode) return false;
                    try {
                        GenApi::CFloatPtr ptr = currentNode;
                        ptr->SetValue(value);
                        return true;
                    } catch(const Pylon::GenericException &e) {
                        *errorMsg = QString::fromStdString(e.GetDescription());
                        return false;
                    }
                },
                [=](bool success) {
                    spinBox->setEnabled(true);
                    if (!success) {
                        QSignalBlocker block(spinBox);
                        auto* currentNode = resolveNode(nodeName);
                        if (currentNode) {
                            try {
                                GenApi::CFloatPtr ptr = currentNode;
                                if (GenApi::IsReadable(ptr)) {
                                    spinBox->setValue(ptr->GetValue());
                                }
                            } catch(...) {}
                        }
                        logMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(*errorMsg), true);
                    } else {
                        logMessage(tr("Parameter '%1' updated to %2.").arg(nodeName).arg(value), false);
                        scheduleFeaturesRebuild();
                    }
                    delete errorMsg;
                }
            );
        });
    } break;
    case GenApi::intfIBoolean:{
        GenApi::CBooleanPtr ptr = node;
        auto checkBox = new QCheckBox;
        widget = checkBox;
        try{
            QSignalBlocker block(checkBox);
            checkBox->setChecked(ptr->GetValue());
        }catch (const Pylon::GenericException &e){
            logMessage(e.GetDescription(), true);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        const auto updateBooleanNode = [=](const Qt::CheckState state){
            checkBox->setEnabled(false);
            bool val = (state == Qt::Checked) ? true : false;
            auto* errorMsg = new QString();
            runAsyncWrite(
                [=]() {
                    auto* currentNode = resolveNode(nodeName);
                    if(!currentNode) return false;
                    try {
                        GenApi::CBooleanPtr ptr = currentNode;
                        ptr->SetValue(val);
                        return true;
                    } catch(const Pylon::GenericException &e) {
                        *errorMsg = QString::fromStdString(e.GetDescription());
                        return false;
                    }
                },
                [=](bool success) {
                    checkBox->setEnabled(true);
                    if (!success) {
                        QSignalBlocker block(checkBox);
                        auto* currentNode = resolveNode(nodeName);
                        if (currentNode) {
                            try {
                                GenApi::CBooleanPtr ptr = currentNode;
                                if (GenApi::IsReadable(ptr)) {
                                    checkBox->setChecked(ptr->GetValue());
                                }
                            } catch(...) {}
                        }
                        logMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(*errorMsg), true);
                    } else {
                        logMessage(tr("Parameter '%1' updated to %2.").arg(nodeName).arg(val ? "True" : "False"), false);
                        scheduleFeaturesRebuild();
                    }
                    delete errorMsg;
                }
            );
        };
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        connect(checkBox, &QCheckBox::checkStateChanged, this, updateBooleanNode);
#else
        connect(checkBox, &QCheckBox::stateChanged, this, [=](const int state){
            updateBooleanNode(static_cast<Qt::CheckState>(state));
        });
#endif
    } break;
    case GenApi::intfIString:{
        GenApi::CStringPtr ptr = node;
        auto lineEdit = new QLineEdit;
        widget = lineEdit;

        try{
            QSignalBlocker block(lineEdit);
            lineEdit->setText(ptr->GetValue().c_str());
        }catch (const Pylon::GenericException &e){
            logMessage(e.GetDescription(), true);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(lineEdit, &QLineEdit::editingFinished, this, [=](){
            lineEdit->setEnabled(false);
            QString text = lineEdit->text();
            auto* errorMsg = new QString();
            runAsyncWrite(
                [=]() {
                    auto* currentNode = resolveNode(nodeName);
                    if(!currentNode) return false;
                    try {
                        GenApi::CStringPtr ptr = currentNode;
                        ptr->SetValue(text.toStdString().c_str());
                        return true;
                    } catch(const Pylon::GenericException &e) {
                        *errorMsg = QString::fromStdString(e.GetDescription());
                        return false;
                    }
                },
                [=](bool success) {
                    lineEdit->setEnabled(true);
                    if (!success) {
                        QSignalBlocker block(lineEdit);
                        auto* currentNode = resolveNode(nodeName);
                        if (currentNode) {
                            try {
                                GenApi::CStringPtr ptr = currentNode;
                                if (GenApi::IsReadable(ptr)) {
                                    lineEdit->setText(ptr->GetValue().c_str());
                                }
                            } catch(...) {}
                        }
                        logMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(*errorMsg), true);
                    } else {
                        logMessage(tr("Parameter '%1' updated to '%2'.").arg(nodeName).arg(text), false);
                        scheduleFeaturesRebuild();
                    }
                    delete errorMsg;
                }
            );
        });
    } break;
    case GenApi::intfIEnumeration:{
        GenApi::CEnumerationPtr ptr = node;
        auto comboBox = new QComboBox;
        widget = comboBox;
        try{
            QSignalBlocker block(comboBox);

            Pylon::StringList_t list;
            ptr->GetSymbolics(list);

            for(const auto &item : list){
                comboBox->addItem(QString::fromStdString(ptr->GetEntryByName(item)->GetNode()->GetDisplayName().c_str()), QVariant::fromValue((QString)item));
            }
            try{
                QSignalBlocker block(comboBox);
                comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
            }catch (const Pylon::GenericException &e){
                logMessage(e.GetDescription(), true);
                qWarning() << e.GetDescription() << node->GetName().c_str();
            }
        }catch (const Pylon::GenericException &e){
            logMessage(e.GetDescription(), true);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(comboBox, &QComboBox::currentTextChanged, this, [=](QString text){
            comboBox->setEnabled(false);
            auto* errorMsg = new QString();
            QString comboData = comboBox->currentData().toString();
            runAsyncWrite(
                [=]() {
                    auto* currentNode = resolveNode(nodeName);
                    if(!currentNode) return false;
                    try {
                        GenApi::CEnumerationPtr ptr = currentNode;
                        auto val = ptr->GetEntryByName(comboData.toStdString().c_str());
                        ptr->SetIntValue(val->GetNumericValue());
                        return true;
                    } catch(const Pylon::GenericException &e) {
                        *errorMsg = QString::fromStdString(e.GetDescription());
                        return false;
                    }
                },
                [=](bool success) {
                    comboBox->setEnabled(true);
                    if (!success) {
                        QSignalBlocker block(comboBox);
                        auto* currentNode = resolveNode(nodeName);
                        if (currentNode) {
                            try {
                                GenApi::CEnumerationPtr ptr = currentNode;
                                if (GenApi::IsReadable(ptr) && ptr->GetCurrentEntry()) {
                                    comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
                                }
                            } catch(...) {}
                        }
                        logMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(*errorMsg), true);
                    } else {
                        logMessage(tr("Parameter '%1' updated to '%2'.").arg(nodeName).arg(text), false);
                        scheduleFeaturesRebuild();
                    }
                    delete errorMsg;
                }
            );
        });
    } break;
    case GenApi::intfICommand:{
        GenApi::CCommandPtr ptr = node;
        auto button = new QPushButton("Execute");
        widget = button;
        connect(button, &QPushButton::clicked, this, [=]{
            button->setEnabled(false);
            auto* errorMsg = new QString();
            logMessage(tr("Executing command '%1'...").arg(nodeName), false);
            
            runAsyncWrite(
                [=]() {
                    auto* currentNode = resolveNode(nodeName);
                    if(!currentNode) return false;
                    try {
                        GenApi::CCommandPtr ptr = currentNode;
                        if(!GenApi::IsWritable(ptr)) return false;
                        ptr->Execute();
                        return true;
                    } catch(const Pylon::GenericException &e) {
                        *errorMsg = QString::fromStdString(e.GetDescription());
                        return false;
                    } catch(const std::exception &e) {
                        *errorMsg = QString::fromStdString(e.what());
                        return false;
                    }
                },
                [=](bool success) {
                    button->setEnabled(true);
                    scheduleFeaturesRebuild();
                    if (!success) {
                        if (errorMsg->isEmpty()) {
                            logMessage(tr("Failed to execute command '%1'.").arg(nodeName), true);
                        } else {
                            logMessage(tr("Failed to execute command '%1': %2").arg(nodeName).arg(*errorMsg), true);
                        }
                    } else {
                        logMessage(tr("Command '%1' executed successfully.").arg(nodeName), false);
                    }
                    delete errorMsg;
                }
            );
        });
    } break;
    case GenApi::intfIRegister:{
        GenApi::CRegisterPtr ptr = node;
        auto label = new QLabel("0x" + QString::number(ptr->GetAddress(), 16));
        widget = label;
    } break;
    case GenApi::intfICategory:
    case GenApi::intfIEnumEntry:
    case GenApi::intfIPort:
    case GenApi::intfIValue:
    case GenApi::intfIBase:
        break;
    }

    if(widget){
        widget->setAccessibleName(node->GetName().c_str());
        widget->setEnabled(GenApi::IsWritable(node));
    }
    return widget;
}

void QCameraWidget::logMessage(const QString& message, bool error)
{
    if (message.isEmpty()) return;
    if (error) qWarning().noquote() << "[Camera UI]" << message;
    else qInfo().noquote() << "[Camera UI]" << message;
}

void QCameraWidget::updateGrabState(bool grabbing)
{
    const bool wasGrabbing = _grabbing.exchange(grabbing, std::memory_order_acq_rel);
    if(grabbing){
        if(_nodeUpdateTimer) _nodeUpdateTimer->stop();
        std::lock_guard<std::mutex> lock(_nodeUpdateMutex);
        _suppressedGrabNodeUpdates.fetch_add(
            static_cast<std::size_t>(_pendingNodeUpdates.size()),
            std::memory_order_relaxed);
        _pendingNodeUpdates.clear();
        _nodeUpdateDrainScheduled = false;
    }else if(wasGrabbing){
        const std::size_t suppressed = _suppressedGrabNodeUpdates.exchange(0, std::memory_order_acq_rel);
        qInfo() << "[Camera UI] Live feature updates deferred during grab:"
                << suppressed << "event(s); rebuilding once after stop.";
        scheduleFeaturesRebuild();
    }
    updateStatusLabel();
}

void QCameraWidget::updateStatusLabel()
{
    if (!_statusLabel || _shuttingDown) return;

    const bool opened = _camera && _camera->isOpened();
    _statusLabel->setText(!opened ? QStringLiteral("Idle")
        : _grabbing ? QStringLiteral("Live") : QStringLiteral("Connected"));
}
#endif

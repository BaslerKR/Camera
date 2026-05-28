#include "QCameraWidget.h"
#ifdef QT_GUI_LIB
#include <QToolButton>
#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMetaObject>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QThread>
#include <QVariant>
#include <QStyle>
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

void repolish(QWidget* widget)
{
    if(!widget) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
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
    _featuresWidget->setRootIsDecorated(true);
    _featuresWidget->setAnimated(false);
    _featuresWidget->setAlternatingRowColors(true);
    _featuresWidget->setUniformRowHeights(false);
    _featuresWidget->setIndentation(18);
    _featuresWidget->header()->setStretchLastSection(true);
    _featuresWidget->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    _featuresWidget->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    _featuresWidget->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    _featuresWidget->header()->setMinimumSectionSize(60);
    _featuresWidget->header()->resizeSection(0, 200);

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
    cameraListLayout->setContentsMargins(0, 0, 0, 0);
    cameraListLayout->setSpacing(8);
    cameraListLayout->addWidget(_cameraListComboBox);
    cameraListLayout->addWidget(_toolRefresh);

    QHBoxLayout *toolButtonLayout = new QHBoxLayout;
    toolButtonLayout->setContentsMargins(0, 0, 0, 0);
    toolButtonLayout->setSpacing(6);
    toolButtonLayout->addWidget(_toolConnect);
    toolButtonLayout->addWidget(_toolGrabOne);
    toolButtonLayout->addWidget(_toolGrabLive);

    auto *listAndButtonLayout = new QHBoxLayout;
    listAndButtonLayout->setContentsMargins(12, 12, 12, 12);
    listAndButtonLayout->setSpacing(10);
    listAndButtonLayout->addLayout(cameraListLayout);
    listAndButtonLayout->addLayout(toolButtonLayout);

    auto *featuresWidgetLayout = new QVBoxLayout;
    featuresWidgetLayout->setContentsMargins(12, 0, 12, 12);
    featuresWidgetLayout->setSpacing(8);
    featuresWidgetLayout->addWidget(_featuresWidget);

    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(listAndButtonLayout);
    layout->addLayout(featuresWidgetLayout);

    _statusBar = new QStatusBar(this);
    _statusBar->setObjectName(QStringLiteral("CameraStatusBar"));
    _statusBar->setContentsMargins(0, 0, 0, 0);

    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("CameraStatusLabel"));
    _statusLabel->setAlignment(Qt::AlignCenter);
    _statusBar->addWidget(_statusLabel);

    _messageLabel = new QLabel(this);
    _messageLabel->setObjectName(QStringLiteral("CameraMessageLabel"));
    _messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _messageLabel->setProperty("messageState", "normal");
    _statusBar->addWidget(_messageLabel, 1);

    _messageTimer = new QTimer(this);
    _messageTimer->setSingleShot(true);
    connect(_messageTimer, &QTimer::timeout, this, [this]() {
        if (_messageLabel) _messageLabel->clear();
    });

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
        showStatusMessage(tr("Camera instance is not configured."), true);
        return;
    }

    QPointer<QCameraWidget> guard(this);

    // Configure the details of toolbuttons
    connect(_toolRefresh, &QToolButton::clicked, this, [=]{
        _cameraListComboBox->clear();
        for(const auto &camera: _camera->getUpdatedCameraList()){
            _cameraListComboBox->addItem(camera.c_str());
        }
        if(_camera->isOpened()) _cameraListComboBox->setCurrentText(_camera->getConnectedCameraName().c_str());
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

        const QString qtNodeName = QString::fromStdString(nodeName);
        QMetaObject::invokeMethod(guard, [guard, qtNodeName]{
            if(!guard) return;
            guard->handleNodeUpdated(qtNodeName);
        }, Qt::QueuedConnection);
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
        showStatusMessage(tr("Single grab triggered."), false, 3000);
    });
    connect(_toolGrabLive, &QToolButton::toggled, this, [=](bool toggled){
        // Request to start a continuous grabbing
        if(toggled) {
            _camera->grab();
            showStatusMessage(tr("Live grabbing started."), false, 0);
        } else {
            _camera->requestStop();
            showStatusMessage(tr("Live grabbing stopped."), false, 3000);
        }
    });

    emit _toolRefresh->clicked();
}

QCameraWidget::~QCameraWidget()
{
    prepareForShutdown();
}

void QCameraWidget::prepareForShutdown()
{
    _shuttingDown = true;

    if(_connectionThread){
        _connectionThread->wait();
        _connectionThread = nullptr;
    }

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
    _connectionAttempted = true;
    setConnectionOperationActive(true);
    showStatusMessage(open ? tr("Connecting camera...") : tr("Disconnecting camera..."), false, 0);

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
                guard->showStatusMessage(tr("Camera connected successfully."), false, 3000);
            }else{
                guard->showStatusMessage(tr("Camera connection failed."), true, 5000);
            }
        }else{
            guard->showStatusMessage(tr("Camera disconnected successfully."), false, 3000);
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
    _cameraListComboBox->setEnabled(!active && !opened);
    _toolRefresh->setEnabled(!active && !opened);
    _toolGrabOne->setEnabled(!active && opened);
    _toolGrabLive->setEnabled(!active && opened);
}

void QCameraWidget::applyConnectionState(const bool opened)
{
    if(_shuttingDown) return;

    if(opened){
        _connectionAttempted = true;
    }

    {
        QSignalBlocker connectBlock(_toolConnect);
        _toolConnect->setChecked(opened);
    }

    _cameraListComboBox->setEnabled(!opened);
    _toolRefresh->setEnabled(!opened);
    _toolConnect->setEnabled(true);
    _toolGrabOne->setEnabled(opened);
    _toolGrabLive->setEnabled(opened);

    updateStatusBubble();

    if(opened){
        rebuildFeaturesIfReady();
    }else{
        {
            QSignalBlocker grabBlock(_toolGrabLive);
            _toolGrabLive->setChecked(false);
        }
        updateGrabState(false);
        _featuresWidget->clear();
        emit _toolRefresh->clicked();
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

        QTreeWidgetItem *cameraFeatures = new QTreeWidgetItem(_featuresWidget, QStringList() << _camera->getConnectedCameraName().c_str());
        cameraFeatures->setData(0, Qt::UserRole, QStringLiteral("__camera_root__"));
        cameraFeatures->setSizeHint(0, QSize(0, 22));
        cameraFeatures->setSizeHint(1, QSize(0, 22));
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

        auto nodeWidget = createNodeWidget(sub);
        if(!nodeWidget) continue;

        QTreeWidgetItem* subItem = new QTreeWidgetItem(parent, QStringList() << sub->GetDisplayName().c_str());
        subItem->setData(0, Qt::UserRole, QString::fromStdString(sub->GetName().c_str()));
        const int rowHeight = nodeWidget->sizeHint().height();
        subItem->setSizeHint(0, QSize(0, rowHeight));
        subItem->setSizeHint(1, QSize(0, rowHeight));
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
    if(_shuttingDown || _rebuildScheduled || !isCameraReady()) return;

    _rebuildScheduled = true;
    QTimer::singleShot(50, this, [this]{
        if(_shuttingDown) return;
        _rebuildScheduled = false;
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
            showStatusMessage(e.GetDescription(), true, 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CIntegerPtr ptr = currentNode;
            try{
                QSignalBlocker block(spinBox);
                ptr->SetValue(value);
                showStatusMessage(tr("Parameter '%1' updated to %2.").arg(nodeName).arg(value), false, 3000);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(spinBox);
                try{
                    if(GenApi::IsReadable(ptr)){
                        spinBox->setValue(ptr->GetValue());
                    }
                }catch(const Pylon::GenericException&){}
                showStatusMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(e.GetDescription()), true, 5000);
                qWarning() << e.GetDescription() << nodeName;
            }
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
            showStatusMessage(e.GetDescription(), true, 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str() << "Float";
        }
        connect(spinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double value){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CFloatPtr ptr = currentNode;
            try{
                QSignalBlocker block(spinBox);
                ptr->SetValue(value);
                showStatusMessage(tr("Parameter '%1' updated to %2.").arg(nodeName).arg(value), false, 3000);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(spinBox);
                try{
                    if(GenApi::IsReadable(ptr)){
                        spinBox->setValue(ptr->GetValue());
                    }
                }catch(const Pylon::GenericException&){}
                showStatusMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(e.GetDescription()), true, 5000);
                qWarning() << e.GetDescription() << nodeName;
            }
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
            showStatusMessage(e.GetDescription(), true, 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        const auto updateBooleanNode = [=](const Qt::CheckState state){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CBooleanPtr ptr = currentNode;
            try{
                QSignalBlocker block(checkBox);
                bool val = (state == Qt::Checked) ? true : false;
                ptr->SetValue(val);
                showStatusMessage(tr("Parameter '%1' updated to %2.").arg(nodeName).arg(val ? "True" : "False"), false, 3000);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(checkBox);
                try{
                    if(GenApi::IsReadable(ptr)){
                        checkBox->setChecked(ptr->GetValue());
                    }
                }catch(const Pylon::GenericException&){}
                showStatusMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(e.GetDescription()), true, 5000);
                qWarning() << e.GetDescription() << nodeName;
            }
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
            showStatusMessage(e.GetDescription(), true, 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(lineEdit, &QLineEdit::editingFinished, this, [=](){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CStringPtr ptr = currentNode;
            try{
                QSignalBlocker block(lineEdit);
                QString text = lineEdit->text();
                ptr->SetValue(text.toStdString().c_str());
                showStatusMessage(tr("Parameter '%1' updated to '%2'.").arg(nodeName).arg(text), false, 3000);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(lineEdit);
                try{
                    if(GenApi::IsReadable(ptr)){
                        lineEdit->setText(ptr->GetValue().c_str());
                    }
                }catch(const Pylon::GenericException&){}
                showStatusMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(e.GetDescription()), true, 5000);
                qWarning() << e.GetDescription() << nodeName;
            }
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
                showStatusMessage(e.GetDescription(), true, 5000);
                qWarning() << e.GetDescription() << node->GetName().c_str();
            }
        }catch (const Pylon::GenericException &e){
            showStatusMessage(e.GetDescription(), true, 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(comboBox, &QComboBox::currentTextChanged, this, [=](QString text){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CEnumerationPtr ptr = currentNode;
            try{
                QSignalBlocker block(comboBox);
                auto val = ptr->GetEntryByName(comboBox->currentData().toString().toStdString().c_str());
                ptr->SetIntValue(val->GetNumericValue());
                showStatusMessage(tr("Parameter '%1' updated to '%2'.").arg(nodeName).arg(text), false, 3000);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(comboBox);
                try{
                    if(GenApi::IsReadable(ptr) && ptr->GetCurrentEntry()){
                        comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
                    }
                }catch(const Pylon::GenericException&){}

                showStatusMessage(tr("Failed to update '%1': %2").arg(nodeName).arg(e.GetDescription()), true, 5000);
                qWarning() << e.GetDescription() << nodeName;
            }
        });
    } break;
    case GenApi::intfICommand:{
        GenApi::CCommandPtr ptr = node;
        auto button = new QPushButton("Execute");
        widget = button;
        connect(button, &QPushButton::clicked, this, [=]{
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            try{
                GenApi::CCommandPtr ptr = currentNode;
                if(!GenApi::IsWritable(ptr)){
                    button->setEnabled(false);
                    scheduleFeaturesRebuild();
                    return;
                }
                showStatusMessage(tr("Executing command '%1'...").arg(nodeName), false, 0);
                ptr->Execute();
                showStatusMessage(tr("Command '%1' executed successfully.").arg(nodeName), false, 3000);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                scheduleFeaturesRebuild();
                showStatusMessage(tr("Failed to execute command '%1': %2").arg(nodeName).arg(e.GetDescription()), true, 5000);
                qWarning() << e.GetDescription() << nodeName;
            }catch(const std::exception &e){
                scheduleFeaturesRebuild();
                showStatusMessage(tr("Failed to execute command '%1': %2").arg(nodeName).arg(e.what()), true, 5000);
                qWarning() << e.what() << nodeName;
            }
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

void QCameraWidget::showStatusMessage(const QString& msg, bool isError, int timeout)
{
    if (!_messageLabel || _shuttingDown) return;

    _messageTimer->stop();
    _messageLabel->setText(msg);
    _messageLabel->setProperty("messageState", isError ? "error" : "normal");
    repolish(_messageLabel);

    if (timeout > 0) {
        _messageTimer->start(timeout);
    }
}

void QCameraWidget::updateGrabState(bool grabbing)
{
    _grabbing = grabbing;
    updateStatusBubble();
}

void QCameraWidget::updateStatusBubble()
{
    if (!_statusLabel || _shuttingDown) return;

    const bool opened = _camera && _camera->isOpened();

    if (!opened && !_connectionAttempted) {
        _statusLabel->setText(tr("Idle"));
        _statusLabel->setProperty("status", "idle");
    } else if (!opened) {
        _statusLabel->setText(tr("Disconnected"));
        _statusLabel->setProperty("status", "disconnected");
    } else if (_grabbing) {
        _statusLabel->setText(tr("Live"));
        _statusLabel->setProperty("status", "grabbing");
    } else {
        _statusLabel->setText(tr("Connected"));
        _statusLabel->setProperty("status", "connected");
    }
    _statusLabel->style()->unpolish(_statusLabel);
    _statusLabel->style()->polish(_statusLabel);
}
#endif

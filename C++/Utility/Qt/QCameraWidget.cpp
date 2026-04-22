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
#include <QVariant>

QCameraWidget::QCameraWidget(QWidget *parent, Camera *camera) : QWidget(parent), _camera(camera)
{
    setWindowTitle("Basler pylon Camera Configuration");
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
    layout->addWidget(_statusBar);
    setLayout(layout);

    if(!_camera){
        _cameraListComboBox->setEnabled(false);
        _toolRefresh->setEnabled(false);
        _toolConnect->setEnabled(false);
        _toolGrabOne->setEnabled(false);
        _toolGrabLive->setEnabled(false);
        _statusBar->showMessage("Camera instance is not configured.", 0);
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
            }break;
            case Camera::ConnectionStatus:{
                QSignalBlocker connectBlock(guard->_toolConnect);
                guard->_toolConnect->setChecked(on);

                guard->_cameraListComboBox->setEnabled(!on);
                guard->_toolRefresh->setEnabled(!on);
                guard->_toolGrabOne->setEnabled(on);
                guard->_toolGrabLive->setEnabled(on);

                if(on){ // Camera Connected
                    guard->rebuildFeaturesIfReady();
                }else{ // Camera Disconnected
                    guard->_toolGrabLive->setChecked(false);
                    guard->_featuresWidget->clear();
                }
                if(!on) emit guard->_toolRefresh->clicked();
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
            if(!_camera->open(_cameraListComboBox->currentText().toStdString())){
                QSignalBlocker connectBlock(_toolConnect);
                _toolConnect->setChecked(false);
            }
        }else{
            _camera->close();
        }
    });
    connect(_toolGrabOne, &QToolButton::clicked, this, [=]{
        // Request to start a single grabbing
        _camera->grab(1);
    });
    connect(_toolGrabLive, &QToolButton::toggled, this, [=](bool toggled){
        // Request to start a continuous grabbing
        if(toggled) _camera->grab();
        else _camera->stop();
    });

    emit _toolRefresh->clicked();
}

QCameraWidget::~QCameraWidget()
{
    if(_camera){
        if(_statusCallbackId != 0){
            _camera->deregisterStatusCallback(_statusCallbackId);
        }
        if(_nodeCallbackId != 0){
            _camera->deregisterNodeUpdatedCallback(_nodeCallbackId);
        }
    }
}

bool QCameraWidget::isCameraReady() const
{
    return _camera && _camera->isOpened();
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
        restoreExpandedNodeNames(cameraFeatures, expandedNodeNames);

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
        _featuresWidget->setItemWidget(subItem, parent->columnCount(), nodeWidget);
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
    if(_rebuildScheduled || !isCameraReady()) return;

    _rebuildScheduled = true;
    QTimer::singleShot(50, this, [this]{
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
            _statusBar->showMessage(e.GetDescription(), 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CIntegerPtr ptr = currentNode;
            try{
                QSignalBlocker block(spinBox);
                ptr->SetValue(value);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(spinBox);
                try{
                    if(GenApi::IsReadable(ptr)){
                        spinBox->setValue(ptr->GetValue());
                    }
                }catch(const Pylon::GenericException&){}
                _statusBar->showMessage(e.GetDescription(), 5000);
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
            _statusBar->showMessage(e.GetDescription(), 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str() << "Float";
        }
        connect(spinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double value){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CFloatPtr ptr = currentNode;
            try{
                QSignalBlocker block(spinBox);
                ptr->SetValue(value);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(spinBox);
                try{
                    if(GenApi::IsReadable(ptr)){
                        spinBox->setValue(ptr->GetValue());
                    }
                }catch(const Pylon::GenericException&){}
                _statusBar->showMessage(e.GetDescription(), 5000);
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
            _statusBar->showMessage(e.GetDescription(), 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(checkBox, &QCheckBox::checkStateChanged, this, [=](Qt::CheckState state){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CBooleanPtr ptr = currentNode;
            try{
                QSignalBlocker block(checkBox);
                ptr->SetValue((state == Qt::CheckState::Checked) ? true : false);
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(checkBox);
                try{
                    if(GenApi::IsReadable(ptr)){
                        checkBox->setChecked(ptr->GetValue());
                    }
                }catch(const Pylon::GenericException&){}
                _statusBar->showMessage(e.GetDescription(), 5000);
                qWarning() << e.GetDescription() << nodeName;
            }
        });
    } break;
    case GenApi::intfIString:{
        GenApi::CStringPtr ptr = node;
        auto lineEdit = new QLineEdit;
        widget = lineEdit;

        try{
            QSignalBlocker block(lineEdit);
            lineEdit->setText(ptr->GetValue().c_str());
        }catch (const Pylon::GenericException &e){
            _statusBar->showMessage(e.GetDescription(), 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(lineEdit, &QLineEdit::editingFinished, this, [=](){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CStringPtr ptr = currentNode;
            try{
                QSignalBlocker block(lineEdit);
                ptr->SetValue(lineEdit->text().toStdString().c_str());
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(lineEdit);
                try{
                    if(GenApi::IsReadable(ptr)){
                        lineEdit->setText(ptr->GetValue().c_str());
                    }
                }catch(const Pylon::GenericException&){}
                _statusBar->showMessage(e.GetDescription(), 5000);
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
            comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
        }catch (const Pylon::GenericException &e){
            _statusBar->showMessage(e.GetDescription(), 5000);
            qWarning() << e.GetDescription() << node->GetName().c_str();
        }
        connect(comboBox, &QComboBox::currentTextChanged, this, [=](QString){
            auto* currentNode = resolveNode(nodeName);
            if(!currentNode) return;

            GenApi::CEnumerationPtr ptr = currentNode;
            try{
                QSignalBlocker block(comboBox);
                auto val = ptr->GetEntryByName(comboBox->currentData().toString().toStdString().c_str());
                ptr->SetIntValue(val->GetNumericValue());
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(comboBox);
                try{
                    if(GenApi::IsReadable(ptr) && ptr->GetCurrentEntry()){
                        comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
                    }
                }catch(const Pylon::GenericException&){}

                _statusBar->showMessage(e.GetDescription(), 5000);
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

            GenApi::CCommandPtr ptr = currentNode;
            try{
                ptr->Execute();
                scheduleFeaturesRebuild();
            }catch(const Pylon::GenericException &e){
                _statusBar->showMessage(e.GetDescription(), 5000);
                qWarning() << e.GetDescription() << nodeName;
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
#endif

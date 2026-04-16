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
#include <QVariant>

QCameraWidget::QCameraWidget(QWidget *parent, Camera *camera) : QWidget(parent), _camera(camera)
{
    setWindowTitle("Basler pylon Camera Configuration");

    // Create the camera list combobox
    _cameraListComboBox = new QComboBox;
    _cameraListComboBox->setMinimumWidth(120);

    // Create the features widget
    _featuresWidget = new QTreeWidget;
    _featuresWidget->setHeaderLabels(QStringList() << "Feature" << "Value");

    // Create the toolbuttons
    _toolRefresh = new QToolButton(this);
    _toolRefresh->setIcon(QIcon(":/Resources/Icons/icons8-refresh-48.png"));
    _toolConnect = new QToolButton(this);
    _toolConnect->setCheckable(true);
    {
        QIcon icon;
        icon.addFile(":/Resources/Icons/icons8-connect-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/Icons/icons8-disconnected-48.png", QSize(), QIcon::Normal, QIcon::On);
        _toolConnect->setIcon(icon);
    }
    _toolGrabOne = new QToolButton(this);
    _toolGrabOne->setIcon(QIcon(":/Resources/Icons/icons8-camera-48.png"));
    _toolGrabOne->setEnabled(false);
    _toolGrabLive = new QToolButton(this);
    _toolGrabLive->setCheckable(true);
    _toolGrabLive->setEnabled(false);
    {
        QIcon icon;
        icon.addFile(":/Resources/Icons/icons8-cameras-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/Icons/icons8-pause-48.png", QSize(), QIcon::Normal, QIcon::On);
        _toolGrabLive->setIcon(icon);
    }

    QHBoxLayout *cameraListLayout = new QHBoxLayout;
    cameraListLayout->addWidget(_cameraListComboBox);
    cameraListLayout->addWidget(_toolRefresh);
    cameraListLayout->setSpacing(-1);

    QHBoxLayout *toolButtonLayout = new QHBoxLayout;
    toolButtonLayout->addWidget(_toolConnect);
    toolButtonLayout->setSpacing(-1);
    toolButtonLayout->addSpacerItem(new QSpacerItem(5,5));
    toolButtonLayout->addWidget(_toolGrabOne);
    toolButtonLayout->addWidget(_toolGrabLive);

    auto *listAndButtonLayout = new QHBoxLayout;
    listAndButtonLayout->setContentsMargins(9,9,9,9);
    listAndButtonLayout->addLayout(cameraListLayout);
    listAndButtonLayout->addLayout(toolButtonLayout);

    auto *featuresWidgetLayout = new QVBoxLayout;
    featuresWidgetLayout->setContentsMargins(9,0,9,0);
    featuresWidgetLayout->addWidget(_featuresWidget);

    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(0,0,0,0);
    layout->addLayout(listAndButtonLayout);
    layout->addLayout(featuresWidgetLayout);

    _statusBar = new QStatusBar(this);
    _statusBar->setContentsMargins(0,0,0,0);
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
                    guard->generateFeaturesWidget(guard->_camera->getNodeMap());
                }else{ // Camera Disconnected
                    guard->_toolGrabLive->setChecked(false);
                    guard->_featuresWidget->clear();
                }
                if(!on) emit guard->_toolRefresh->clicked();
            }break;
            }
        }, Qt::QueuedConnection);
    });
    _nodeCallbackId = _camera->registerNodeUpdatedCallback([guard](GenApi::INode* node){
        if(!guard) return;
        QMetaObject::invokeMethod(guard, [guard, node]{
            if(!guard) return;
            if(!node) return;
            if(!guard->refreshNodeWidget(node)){
                qDebug() << "Rebuilding feature tree for dynamic node" << node->GetName().c_str();
                guard->scheduleFeaturesRebuild();
            }
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
        _featuresWidget->header()->resizeSection(0,200);

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

void QCameraWidget::scheduleFeaturesRebuild()
{
    if(_rebuildScheduled || !_camera || !_camera->isOpened()) return;

    _rebuildScheduled = true;
    QTimer::singleShot(50, this, [this]{
        _rebuildScheduled = false;
        if(!_camera || !_camera->isOpened()) return;
        generateFeaturesWidget(_camera->getNodeMap());
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
                qWarning() << e.GetDescription() << node->GetName().c_str();
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
                qWarning() << e.GetDescription() << node->GetName().c_str();
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
                qWarning() << e.GetDescription() << node->GetName().c_str();
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
                qWarning() << e.GetDescription() << node->GetName().c_str();
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
                qWarning() << e.GetDescription() << node->GetName().c_str();
            }
        });
    } break;
    case GenApi::intfICommand:{
        GenApi::CCommandPtr ptr = node;
        auto button = new QPushButton("Execute");
        widget = button;
        connect(button, &QPushButton::clicked, this, [=]{
            try{
                ptr->Execute();
            }catch(const Pylon::GenericException &e){
                _statusBar->showMessage(e.GetDescription(), 5000);
                qWarning() << e.GetDescription() << node->GetName().c_str();
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

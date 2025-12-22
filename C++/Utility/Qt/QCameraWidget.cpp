#include "QCameraWidget.h"
#ifdef QT_GUI_LIB
#include <QToolButton>
#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>

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

    // Configure the details of toolbuttons
    connect(_toolRefresh, &QToolButton::clicked, this, [=]{
        _cameraListComboBox->clear();
        for(const auto &camera: _camera->getUpdatedCameraList()){
            _cameraListComboBox->addItem(camera.c_str());
        }
        if(_camera->isOpened()) _cameraListComboBox->setCurrentText(_camera->getConnectedCameraName().c_str());
    });
    _camera->onCameraStatus([=](Camera::Status status, bool on){
        switch(status){
        case Camera::GrabbingStatus:{
            if(on){ // Grabbing started
                QSignalBlocker grabbingBlock(_toolGrabLive);
                _toolGrabLive->setChecked(true);
            }else{ // Grabbing stopped
                QSignalBlocker grabbingBlock(_toolGrabLive);
                _toolGrabLive->setChecked(false);
            }
        }break;
        case Camera::ConnectionStatus:{
            QSignalBlocker connectBlock(_toolConnect);
            _toolConnect->setChecked(on);

            _cameraListComboBox->setEnabled(!on);
            _toolRefresh->setEnabled(!on);
            _toolGrabOne->setEnabled(on);
            _toolGrabLive->setEnabled(on);

            if(on){ // Camera Connected
                generateFeaturesWidget(_camera->getNodeMap());
            }else{ // Camera Disconnected
                _toolGrabLive->setChecked(false);
                _featuresWidget->clear();
            }
            if(!on) emit _toolRefresh->clicked();
        }break;
        }
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
    emit _toolRefresh->clicked();

    camera->onNodeUpdated([=](GenApi::INode* node){
        auto items = _featuresWidget->findItems(node->GetDisplayName().c_str(), Qt::MatchFlag::MatchRecursive);
        if(items.size() ==0){
            qDebug() << "Need to create something to fill" << node->GetDisplayName().c_str();
        }
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
                }
            } break;
            case GenApi::intfIFloat:{
                if(auto* spinBox = qobject_cast<QDoubleSpinBox*>(cur)){
                    GenApi::CFloatPtr ptr = node;

                    QSignalBlocker block(spinBox);
                    spinBox->setEnabled(GenApi::IsWritable(ptr));
                    spinBox->setRange(ptr->GetMin(), ptr->GetMax());
                    spinBox->setValue(ptr->GetValue());
                }
            } break;
            case GenApi::intfIBoolean:{
                if(auto* checkBox = qobject_cast<QCheckBox*>(cur)){
                    GenApi::CBooleanPtr ptr = node;

                    QSignalBlocker block(checkBox);
                    checkBox->setEnabled(GenApi::IsWritable(ptr));
                    checkBox->setChecked(ptr->GetValue());
                }
            } break;
            case GenApi::intfIString:{
                if(auto *lineEdit = qobject_cast<QLineEdit*>(cur)){
                    GenApi::CStringPtr ptr = node;

                    QSignalBlocker block(lineEdit);
                    lineEdit->setEnabled(GenApi::IsWritable(ptr));
                    lineEdit->setText(ptr->GetValue().c_str());
                }
            } break;
            case GenApi::intfIEnumeration:{
                if(auto *comboBox = qobject_cast<QComboBox*>(cur)){
                    GenApi::CEnumerationPtr ptr = node;

                    QSignalBlocker block(comboBox);
                    comboBox->setEnabled(GenApi::IsWritable(ptr));
                    comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
                }
            } break;
            case GenApi::intfICommand:{
                if(auto *button = qobject_cast<QPushButton*>(cur)){
                    GenApi::CCommandPtr ptr = node;

                    button->setEnabled(GenApi::IsWritable(ptr));
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
    });
}

QCameraWidget::~QCameraWidget()
{
    if(_camera->isOpened()) _camera->close();
}

void QCameraWidget::generateFeaturesWidget(GenApi::INodeMap &nodemap)
{
    _featuresWidget->clear();
    try{
        GenApi::NodeList_t nodes;
        nodemap.GetNodes(nodes);

        QTreeWidgetItem *cameraFeatures = new QTreeWidgetItem(_featuresWidget, QStringList() << _camera->getConnectedCameraName().c_str());
        for(auto cat : nodes){
            if(cat->GetName() == "Root") continue;
            if(!GenApi::IsAvailable(cat)) continue;
            if(cat->GetPrincipalInterfaceType() != GenApi::EInterfaceType::intfICategory) continue;

            GenApi::NodeList_t parentsList;
            cat->GetParents(parentsList);
            if(parentsList.at(0)->GetDisplayName() == "Events Generation") continue;

            QTreeWidgetItem* item = new QTreeWidgetItem(cameraFeatures, QStringList() << cat->GetDisplayName().c_str());

            GenApi::NodeList_t children;
            cat->GetChildren(children);
            generateChildrenItem(item, children);
        }
        _featuresWidget->expandToDepth(0);
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
        _featuresWidget->setItemWidget(subItem, parent->columnCount(), nodeWidget);
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
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(spinBox);
                auto currentValue = ptr->GetValue();
                spinBox->setValue(currentValue);
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
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(spinBox);
                auto currentValue = ptr->GetValue();
                spinBox->setValue(currentValue);
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
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(checkBox);
                auto currentValue = ptr->GetValue();
                checkBox->setChecked(currentValue);
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
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(lineEdit);
                auto currentValue = ptr->GetValue().c_str();
                lineEdit->setText(currentValue);
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
            }catch(const Pylon::GenericException &e){
                QSignalBlocker block(comboBox);
                auto currentValue = ptr->GetCurrentEntry()->GetNumericValue();
                ptr->SetIntValue(currentValue);

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

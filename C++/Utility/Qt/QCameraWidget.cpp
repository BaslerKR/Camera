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
    _toolConnect->setIcon(QIcon(":/Resources/Icons/icons8-connect-48.png"));
    _toolGrabOne = new QToolButton(this);
    _toolGrabOne->setIcon(QIcon(":/Resources/Icons/icons8-camera-48.png"));
    _toolGrabLive = new QToolButton(this);
    _toolGrabLive->setIcon(QIcon(":/Resources/Icons/icons8-cameras-48.png"));

    // Configure the details of toolbuttons
    connect(_toolRefresh, &QToolButton::clicked, this, [=]{
        _cameraListComboBox->clear();
        for(const auto &camera: _camera->getUpdatedCameraList()){
            _cameraListComboBox->addItem(camera.c_str());
        }
    });
    connect(_toolConnect, &QToolButton::clicked, this, [=]{
        // Request to open the camera
        if(_camera->open(_cameraListComboBox->currentText().toStdString())){
            generateFeaturesWidget(_camera->getNodeMap());
        }
    });
    connect(_toolGrabOne, &QToolButton::clicked, this, [=]{
        // Request to start a single grabbing
        _camera->grab(1);
    });
    _toolGrabLive->setCheckable(true);
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

    camera->onConfiguration([](GenApi::INode* node){
        qDebug() << "Node changed." << node->GetDisplayName().c_str();
    });
}

void QCameraWidget::generateFeaturesWidget(GenApi::INodeMap &nodemap)
{
    _featuresWidget->clear();

    GenApi::NodeList_t nodes;
    nodemap.GetNodes(nodes);

    QTreeWidgetItem *cameraFeatures = new QTreeWidgetItem(_featuresWidget, QStringList() << QString("Camera Name"));
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


}

void QCameraWidget::generateChildrenItem(QTreeWidgetItem *parent, GenApi::NodeList_t children)
{
    for(auto sub : children){
        if(!GenApi::IsAvailable(sub)) continue;

        QTreeWidgetItem* subItem = new QTreeWidgetItem(parent, QStringList() << sub->GetDisplayName().c_str());
        switch (sub->GetPrincipalInterfaceType()){
            if(!GenApi::IsReadable(sub)) continue;
        case GenApi::intfIInteger:{
            GenApi::CIntegerPtr ptr = sub;
            auto spinBox = new QSpinBox;
            spinBox->setEnabled(GenApi::IsWritable(ptr));

            try{
                spinBox->blockSignals(true);
                spinBox->setAccessibleName(sub->GetName().c_str());
                spinBox->setRange(ptr->GetMin(), ptr->GetMax());
                spinBox->setValue(ptr->GetValue());
                spinBox->setSingleStep(ptr->GetInc());
                spinBox->setEnabled(GenApi::IsWritable(ptr));
                spinBox->blockSignals(false);
            }catch (const Pylon::GenericException &e){
                _statusBar->showMessage(e.GetDescription(), 5000);
            }
            // connect(this, &CameraWidget::nodeUpdated, spinBox, [=](){
            //     spinBox->blockSignals(true);
            //     try{
            //         spinBox->setValue(ptr->GetValue());
            //     }catch(const Pylon::GenericException &e){
            //         statusMessage(e.GetDescription());
            //     }
            //     spinBox->blockSignals(false);
            // });
            // connect(this, &CameraWidget::grabbingState, spinBox, [=](bool){
            //     try{
            //         spinBox->setEnabled(GenApi::IsWritable(ptr));
            //     }catch(const Pylon::GenericException &e){
            //         statusMessage(e.GetDescription());
            //     }
            // });
            connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value){
                try{
                    ptr->SetValue(value);
                    auto currentValue = ptr->GetValue();
                    spinBox->blockSignals(true);
                    spinBox->setValue(currentValue);
                    spinBox->blockSignals(false);

                    _statusBar->showMessage(QString(ptr->GetNode()->GetDisplayName()) + " is set to " + QString::number(currentValue), 5000);
                }catch(const Pylon::GenericException &e){
                    auto currentValue = ptr->GetValue();
                    spinBox->blockSignals(true);
                    spinBox->setValue(currentValue);
                    spinBox->blockSignals(false);

                    _statusBar->showMessage(e.GetDescription(), 5000);
                }
            });
            _featuresWidget->setItemWidget(subItem, parent->columnCount(), spinBox);
        }break;
        case GenApi::intfIValue:
        case GenApi::intfIBase:
        case GenApi::intfIBoolean:
        case GenApi::intfICommand:
        case GenApi::intfIFloat:
        case GenApi::intfIString:
        case GenApi::intfIRegister:
        case GenApi::intfICategory:
        case GenApi::intfIEnumeration:
        case GenApi::intfIEnumEntry:
        case GenApi::intfIPort:
            break;
        }
    }
}

/*
void Qylon::CameraWidget::generateChildrenWidgetItem(QTreeWidgetItem *parent, GenApi::NodeList_t children)
{
    for(auto sub : children){
        if(!GenApi::IsAvailable(sub)) continue;

        QTreeWidgetItem* subItem = new QTreeWidgetItem(parent, QStringList() << sub->GetDisplayName().c_str());
        manageItems.push_back(subItem);

        switch (sub->GetPrincipalInterfaceType()){
        case GenApi::EInterfaceType::intfIInteger:{
            if(!GenApi::IsReadable(sub)) continue;

            GenApi::CIntegerPtr ptr = sub;
            auto spinBox = new QSpinBox;
            spinBox->setEnabled(GenApi::IsWritable(ptr));
            try{
                spinBox->setAccessibleName(sub->GetName().c_str());
                spinBox->setRange(ptr->GetMin(), ptr->GetMax());
                spinBox->setValue(ptr->GetValue());
                spinBox->setSingleStep(ptr->GetInc());
                spinBox->setEnabled(GenApi::IsWritable(ptr));
            }catch (const Pylon::GenericException &e){
                statusMessage(e.GetDescription());
            }

            connect(this, &CameraWidget::nodeUpdated, spinBox, [=](){
                spinBox->blockSignals(true);
                try{
                    spinBox->setValue(ptr->GetValue());
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                spinBox->blockSignals(false);
            });
            connect(this, &CameraWidget::grabbingState, spinBox, [=](bool){
                try{
                    spinBox->setEnabled(GenApi::IsWritable(ptr));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value){
                try{
                    ptr->SetValue(value);
                    statusMessage(QString(ptr->GetNode()->GetDisplayName()) + " sets to " + QString::number(ptr->GetValue()));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                emit nodeUpdated();
            });
            widget->setItemWidget(subItem, parent->columnCount(), spinBox);
            break;
        }
        case GenApi::EInterfaceType::intfIFloat:{
            if(!GenApi::IsReadable(sub)) continue;

            GenApi::CFloatPtr ptr = sub;
            auto spinBox = new QDoubleSpinBox;
            spinBox->setEnabled(GenApi::IsWritable(ptr));
            try{
                spinBox->setAccessibleName(sub->GetName().c_str());
                spinBox->setDecimals(ptr->GetDisplayPrecision());
                spinBox->setRange(ptr->GetMin(), ptr->GetMax());
                spinBox->setValue(ptr->GetValue());
                // spinBox->setSingleStep(ptr->GetInc());
                spinBox->setSingleStep(0.1);
                spinBox->setEnabled(GenApi::IsWritable(ptr));
            }catch(const Pylon::GenericException &e){
                statusMessage(e.GetDescription());
            }
            connect(this, &CameraWidget::nodeUpdated, spinBox, [=](){
                spinBox->blockSignals(true);
                try{
                    spinBox->setValue(ptr->GetValue());
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                spinBox->blockSignals(false);
            });
            connect(this, &CameraWidget::grabbingState, spinBox, [=](bool){
                try{
                    spinBox->setEnabled(GenApi::IsWritable(ptr));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            connect(spinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double value){
                try{
                    ptr->SetValue(value);
                    statusMessage(QString(ptr->GetNode()->GetDisplayName()) + " sets to " + QString::number(ptr->GetValue())) ;
                    emit nodeUpdated();
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            widget->setItemWidget(subItem, parent->columnCount(), spinBox);
            break;
        }
        case GenApi::EInterfaceType::intfIBoolean:{
            if(!GenApi::IsReadable(sub)) continue;

            GenApi::CBooleanPtr ptr = sub;
            auto checkBox = new QCheckBox;
            checkBox->setEnabled(GenApi::IsWritable(ptr));
            try{
                checkBox->setChecked(ptr->GetValue());
                checkBox->setAccessibleName(sub->GetName().c_str());
                checkBox->setEnabled(GenApi::IsWritable(ptr));
            }catch(const Pylon::GenericException &e){
                statusMessage(e.GetDescription());
            }
            connect(this, &CameraWidget::nodeUpdated, checkBox, [=](){
                checkBox->blockSignals(true);
                try{
                    checkBox->setChecked(ptr->GetValue());
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                checkBox->blockSignals(false);
            });
            connect(this, &CameraWidget::grabbingState, checkBox, [=](bool){
                try{
                    checkBox->setEnabled(GenApi::IsWritable(ptr));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            connect(checkBox, &QCheckBox::clicked, this, [=](bool on){
                try{
                    ptr->SetValue(on);
                    statusMessage(QString(ptr->GetNode()->GetDisplayName()) + " is " + (ptr->GetValue() ? "On" : "Off"));
                    emit nodeUpdated();
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            widget->setItemWidget(subItem, parent->columnCount(), checkBox);
            break;
        }
        case GenApi::EInterfaceType::intfIString:{
            if(!GenApi::IsReadable(sub)) continue;

            GenApi::CStringPtr ptr = sub;
            QLineEdit *lineEdit = new QLineEdit;
            lineEdit->setEnabled(GenApi::IsWritable(ptr));
            lineEdit->setFrame(false);
            try{
                lineEdit->setText(ptr->GetValue().c_str());
            }catch(const Pylon::GenericException &e){
                statusMessage(e.GetDescription());
            }

            connect(this, &CameraWidget::nodeUpdated, lineEdit, [=](){
                lineEdit->blockSignals(true);
                try{
                    lineEdit->setText(ptr->GetValue().c_str());
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                lineEdit->blockSignals(false);
            });
            connect(this, &CameraWidget::grabbingState, lineEdit, [=](bool){
                try{
                    lineEdit->setEnabled(GenApi::IsWritable(ptr));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            connect(lineEdit, &QLineEdit::editingFinished, this, [=]{
                try{
                    ptr->SetValue(lineEdit->text().toStdString().c_str());
                    statusMessage(QString(ptr->GetNode()->GetDisplayName()) + " sets to " + ptr->GetValue());
                    emit nodeUpdated();
                }catch(const Pylon::GenericException &e){
                    qDebug() << "string 3 error";
                    statusMessage(e.GetDescription());
                }
            });
            widget->setItemWidget(subItem, parent->columnCount(), lineEdit);
            break;
        }
        case GenApi::EInterfaceType::intfIEnumeration:{
            if(!GenApi::IsReadable(sub)) continue;

            GenApi::CEnumerationPtr ptr = sub;
            auto comboBox = new QComboBox;
            comboBox->setEnabled(GenApi::IsWritable(ptr));

            try{
                Pylon::StringList_t enuList;
                ptr->GetSymbolics(enuList);

                comboBox->setAccessibleName(sub->GetName().c_str());
                comboBox->setEnabled(GenApi::IsWritable(ptr));

                for(const auto &current : enuList){
                    comboBox->addItem(QString::fromStdString(ptr->GetEntryByName(current)->GetNode()->GetDisplayName().c_str()), QVariant::fromValue((QString)current));
                }
                comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
            }catch(const Pylon::GenericException &e){
                qDebug() << "Enum 1 error";
                statusMessage(e.GetDescription());
            }

            connect(this, &CameraWidget::nodeUpdated, comboBox, [=](){
                comboBox->blockSignals(true);
                try{
                    GenApi::CEnumerationPtr ptr = camera->getNode(comboBox->accessibleName().toStdString().c_str());
                    comboBox->setCurrentText(ptr->GetCurrentEntry()->GetNode()->GetDisplayName().c_str());
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                comboBox->blockSignals(false);
            });
            connect(this, &CameraWidget::grabbingState, comboBox, [=](bool){
                try{
                    comboBox->setEnabled(GenApi::IsWritable(ptr));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            connect(comboBox, &QComboBox::currentTextChanged, this, [=](QString){
                comboBox->blockSignals(true);
                try{
                    auto val = ptr->GetEntryByName(comboBox->currentData().toString().toStdString().c_str());
                    ptr->SetIntValue(val->GetNumericValue());
                    statusMessage(QString(ptr->GetNode()->GetDisplayName().c_str()) + " sets to " + val->GetSymbolic() );

                    emit nodeUpdated();
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
                comboBox->blockSignals(false);
            });
            widget->setItemWidget(subItem, parent->columnCount(), comboBox);
            break;
        }
        case GenApi::EInterfaceType::intfICommand:{
            GenApi::CCommandPtr ptr = sub;
            auto button = new QPushButton("Execute");
            button->setEnabled(GenApi::IsWritable(ptr));

            button->setAccessibleName(sub->GetName().c_str());
            connect(this, &CameraWidget::grabbingState, button, [=](bool){
                try{
                    button->setEnabled(GenApi::IsWritable(ptr));
                }catch(const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            connect(button, &QPushButton::clicked, this, [=](){
                try{
                    ptr->Execute();
                    statusMessage("Executed " + QString(ptr->GetNode()->GetDisplayName().c_str()));
                    emit nodeUpdated();
                }catch (const Pylon::GenericException &e){
                    statusMessage(e.GetDescription());
                }
            });
            widget->setItemWidget(subItem, parent->columnCount(), button);

            break;
        }
        case GenApi::EInterfaceType::intfIRegister:{
            GenApi::CRegisterPtr ptr = sub;
            widget->setItemWidget(subItem, parent->columnCount(), new QLabel(QString::number(ptr->GetAddress())));
            break;
        }
        case GenApi::EInterfaceType::intfICategory:{
            GenApi::NodeList_t subChildren;
            sub->GetChildren(subChildren);
            generateChildrenWidgetItem(parent, subChildren);
            break;
        }
        case GenApi::EInterfaceType::intfIEnumEntry:
        case GenApi::EInterfaceType::intfIBase:
        case GenApi::EInterfaceType::intfIValue:
        case GenApi::EInterfaceType::intfIPort:{
            delete subItem;
            manageItems.pop_back();
            break;
        }}
    }
}*/
#endif

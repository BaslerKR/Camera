#include "CameraControlUi.h"
#include <QtWidgets>


void CameraControlUi::create(QWidget* owner) {

    owner->setWindowTitle("Basler pylon Camera Configuration");
    owner->setMinimumSize(300, 350);
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
    _toolRefresh = new QToolButton(owner);
    _toolRefresh->setIcon(QIcon(":/Resources/icon/refresh-48.png"));
    _toolRefresh->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolRefresh->setIconSize(QSize(16, 16));
    _toolConnect = new QToolButton(owner);
    _toolConnect->setCheckable(true);
    _toolConnect->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolConnect->setIconSize(QSize(16, 16));
    {
        QIcon icon;
        icon.addFile(":/Resources/icon/connect-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/icon/disconnected-48.png", QSize(), QIcon::Normal, QIcon::On);
        _toolConnect->setIcon(icon);
    }
    _toolGrabOne = new QToolButton(owner);
    _toolGrabOne->setIcon(QIcon(":/Resources/icon/camera-48.png"));
    _toolGrabOne->setEnabled(false);
    _toolGrabOne->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolGrabOne->setIconSize(QSize(16, 16));
    _toolGrabLive = new QToolButton(owner);
    _toolGrabLive->setCheckable(true);
    _toolGrabLive->setEnabled(false);
    _toolGrabLive->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _toolGrabLive->setIconSize(QSize(16, 16));
    {
        QIcon icon;
        icon.addFile(":/Resources/icon/cameras-48.png", QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(":/Resources/icon/pause-48.png", QSize(), QIcon::Normal, QIcon::On);
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

    _statusBar = new QStatusBar(owner);
    _statusBar->setObjectName(QStringLiteral("CameraStatusBar"));
    _statusBar->setSizeGripEnabled(false);

    _statusLabel = new QLabel(owner);
    _statusLabel->setObjectName(QStringLiteral("CameraStatusLabel"));
    _statusLabel->setAlignment(Qt::AlignCenter);
    _statusBar->addWidget(_statusLabel);

    layout->addWidget(_statusBar);
    owner->setLayout(layout);

}

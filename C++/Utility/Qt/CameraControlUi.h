#pragma once
#include <QWidget>
#include <QString>
class QComboBox; class QToolButton; class QTreeWidget; class QStatusBar; class QLabel;
class QLineEdit; class QTabWidget; class OneGenFeatureTree;

/** @brief Owns the device control composition without SDK or acquisition dependencies. */
struct CameraControlUi {
    /** @brief Creates the same controls used by the runtime widget. */
    void create(QWidget* owner);
    QTreeWidget* _featuresWidget = nullptr;
    QComboBox* _cameraListComboBox = nullptr;
    QToolButton* _toolRefresh = nullptr;
    QToolButton* _toolConnect = nullptr;
    QToolButton* _toolGrabOne = nullptr;
    QToolButton* _toolGrabLive = nullptr;
    QStatusBar* _statusBar = nullptr;
    QLabel* _statusLabel = nullptr;
};

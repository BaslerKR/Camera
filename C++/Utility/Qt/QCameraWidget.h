#ifndef QCAMERAWIDGET_H
#define QCAMERAWIDGET_H

/**
 * @file QCameraWidget.h
 * @brief Qt widget for Basler camera connection, grab control, and GenApi feature tree editing.
 *
 * Acts as the Camera submodule control panel and forwards only grab callback
 * results to GraphicsEngine.
 */

#ifdef QT_GUI_LIB
#include <QWidget>
#include <QObject>
#include <QTreeWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QToolButton>
#include <QStatusBar>
#include <QCheckBox>
#include <QLineEdit>
#include <QTimer>
#include "Camera.h"

class QThread;

class QCameraWidget : public QWidget
{
    Q_OBJECT
public:
    QCameraWidget(QWidget *parent=nullptr, Camera *camera=nullptr);
    ~QCameraWidget();
    void prepareForShutdown();

    void generateFeaturesWidget(GenApi::INodeMap& nodemap);
    void generateChildrenItem(QTreeWidgetItem *parent, GenApi::NodeList_t children);

    QWidget *createNodeWidget(GenApi::INode* node);
    QList<QTreeWidgetItem*> findItemsByNodeName(const QString& nodeName) const;
    bool refreshNodeWidget(GenApi::INode* node);
    void handleNodeUpdated(const QString& nodeName);
    void scheduleFeaturesRebuild();
    void collectExpandedNodeNames(QTreeWidgetItem* item, QSet<QString>& expandedNodeNames) const;
    void restoreExpandedNodeNames(QTreeWidgetItem* item, const QSet<QString>& expandedNodeNames);

private:
    bool isCameraReady() const;
    GenApi::INode* resolveNode(const QString& nodeName) const;
    void rebuildFeaturesIfReady();
    void startConnectionOperation(bool open, const QString& cameraName = {});
    void setConnectionOperationActive(bool active);
    void applyConnectionState(bool opened);

    Camera *_camera;
    Camera::CallbackId _statusCallbackId = 0;
    Camera::CallbackId _nodeCallbackId = 0;
    QThread *_connectionThread = nullptr;
    bool _connectionOperationActive = false;
    bool _shuttingDown = false;
    bool _grabbing = false;
    QTreeWidget *_featuresWidget;
    QComboBox *_cameraListComboBox;

    QToolButton *_toolRefresh;
    QToolButton *_toolConnect;
    QToolButton *_toolGrabOne;
    QToolButton *_toolGrabLive;

    QStatusBar *_statusBar;
    bool _rebuildScheduled = false;
    QLabel *_messageLabel = nullptr;
    QLabel *_statusLabel = nullptr;
    QTimer *_messageTimer = nullptr;

    void showStatusMessage(const QString& msg, bool isError = false, int timeout = 0);
    void updateGrabState(bool grabbing);
    void updateStatusBubble();
};
#endif
#endif // QCAMERAWIDGET_H

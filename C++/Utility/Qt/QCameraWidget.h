#ifndef QCAMERAWIDGET_H
#define QCAMERAWIDGET_H
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

#include "CameraSystem.h"

class QCameraWidget : public QWidget
{
    Q_OBJECT
public:
    QCameraWidget(QWidget *parent=nullptr, Camera *camera=nullptr);
    ~QCameraWidget();

    void generateFeaturesWidget(GenApi::INodeMap& nodemap);
    void generateChildrenItem(QTreeWidgetItem *parent, GenApi::NodeList_t children);

    QWidget *createNodeWidget(GenApi::INode* node);

private:
    Camera *_camera;
    QTreeWidget *_featuresWidget;
    QComboBox *_cameraListComboBox;

    QToolButton *_toolRefresh;
    QToolButton *_toolConnect;
    QToolButton *_toolGrabOne;
    QToolButton *_toolGrabLive;

    QStatusBar *_statusBar;
};
#endif
#endif // QCAMERAWIDGET_H

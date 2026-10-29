#include "Utility/Qt/QCameraWidget.h"
#include "CameraSystem.h"
#include <QtTest>

namespace {
class ObservedCamera final : public Camera {
public:
    using Camera::Camera;
    void publishGrabState(bool grabbing) {
        Pylon::CInstantCamera source;
        if (grabbing) OnGrabStarted(source);
        else OnGrabStopped(source);
    }
};
class ControlWidget final : public QCameraWidget {
public:
    using QCameraWidget::QCameraWidget;
    QToolButton* connection() const { return _toolConnect; }
    QToolButton* single() const { return _toolGrabOne; }
    QToolButton* live() const { return _toolGrabLive; }
};
}

class AcquisitionControlsTest final : public QObject {
    Q_OBJECT
private slots:
    void observedGrabLocksConnectionAndSingleUntilStop() {
        CameraSystem system;
        ObservedCamera camera(&system);
        ControlWidget widget(nullptr, &camera);
        QVERIFY(widget.connection()->isEnabled());
        // Seed an available single command, then drive the actual queued status callback.
        widget.single()->setEnabled(true);
        camera.publishGrabState(true);
        QTRY_VERIFY(widget.live()->isChecked());
        QVERIFY(!widget.connection()->isEnabled());
        QVERIFY(!widget.single()->isEnabled());
        widget.setDiscoveredCameraNames({"Synthetic camera"});
        QVERIFY(!widget.connection()->isEnabled());
        // A programmatic toggle must not launch a connection worker during acquisition.
        widget.connection()->setChecked(true);
        QVERIFY(!widget.connection()->isChecked());
        QVERIFY(widget.findChildren<QThread*>().isEmpty());
        camera.publishGrabState(false);
        QTRY_VERIFY(!widget.live()->isChecked());
        QVERIFY(widget.connection()->isEnabled());
        // This test has no opened device; single acquisition remains unavailable.
        QVERIFY(!widget.single()->isEnabled());
    }
    void unconfiguredWidgetKeepsCommandsDisabled() {
        ControlWidget widget;
        QVERIFY(!widget.connection()->isEnabled());
        QVERIFY(!widget.single()->isEnabled());
        QVERIFY(!widget.live()->isEnabled());
    }
};
QTEST_MAIN(AcquisitionControlsTest)
#include "AcquisitionControlsTest.moc"

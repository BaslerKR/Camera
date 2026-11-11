#include "Utility/Qt/QCameraWidget.h"
#include "CameraSystem.h"
#include <QLabel>
#include <QLoggingCategory>
#include <QStringList>
#include <QtTest>

namespace {
// Mirrors the host Debug switch, which toggles the diagnostics.* debug level.
void setHostDebug(const bool enabled)
{
    QLoggingCategory::setFilterRules(enabled
        ? QStringLiteral("diagnostics.*.debug=true")
        : QStringLiteral("diagnostics.*.debug=false"));
}

QStringList* capturedMessages = nullptr;
void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message)
{
    if (capturedMessages) capturedMessages->append(message);
}

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
    QLabel* failedBubble() const { return _failedLabel; }
    QLabel* fpsBubble() const { return _fpsLabel; }
    void setFakeDiagnostics(const CameraRuntimeDiagnostics& diagnostics) { _fakeDiagnostics = diagnostics; }
    void refreshDiagnostics() { updateDiagnostics(); }

protected:
    CameraRuntimeDiagnostics runtimeDiagnostics() const override { return _fakeDiagnostics; }

private:
    CameraRuntimeDiagnostics _fakeDiagnostics;
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
    void diagnosticsBubblesFollowLiveAndFailureState() {
        setHostDebug(true);
        CameraSystem system;
        ObservedCamera camera(&system);
        ControlWidget widget(nullptr, &camera);
        camera.publishGrabState(true);
        QTRY_VERIFY(widget.live()->isChecked());

        widget.setFakeDiagnostics({true, 0, 0});
        widget.refreshDiagnostics();
        QVERIFY(!widget.fpsBubble()->isHidden());
        QVERIFY(widget.failedBubble()->isHidden());

        widget.setFakeDiagnostics({true, 10, 3});
        widget.refreshDiagnostics();
        QVERIFY(!widget.fpsBubble()->isHidden());
        QVERIFY(!widget.failedBubble()->isHidden());
        QCOMPARE(widget.failedBubble()->property("failedState").toString(),
                 QStringLiteral("warning"));

        widget.setFakeDiagnostics({true, 20, 0});
        widget.refreshDiagnostics();
        QVERIFY(widget.failedBubble()->isHidden());

        camera.publishGrabState(false);
        QTRY_VERIFY(!widget.live()->isChecked());
        QVERIFY(widget.fpsBubble()->isHidden());
        QVERIFY(widget.failedBubble()->isHidden());
    }
    void diagnosticsBubblesFollowHostDebug() {
        setHostDebug(false);
        CameraSystem system;
        ObservedCamera camera(&system);
        ControlWidget widget(nullptr, &camera);
        camera.publishGrabState(true);
        QTRY_VERIFY(widget.live()->isChecked());
        QVERIFY(widget.fpsBubble()->isHidden());

        // Debug off: failures and FPS are still sampled but stay hidden.
        widget.setFakeDiagnostics({true, 10, 3});
        widget.refreshDiagnostics();
        QVERIFY(widget.fpsBubble()->isHidden());
        QVERIFY(widget.failedBubble()->isHidden());

        // Enabling Debug mid-live shows the current values on the next tick.
        setHostDebug(true);
        widget.setFakeDiagnostics({true, 20, 3});
        widget.refreshDiagnostics();
        QVERIFY(!widget.fpsBubble()->isHidden());
        QVERIFY(!widget.failedBubble()->isHidden());
        QCOMPARE(widget.failedBubble()->text(), QStringLiteral("3 failed"));
        QVERIFY(widget.fpsBubble()->text() != QStringLiteral("0.0 fps"));

        setHostDebug(false);
        widget.refreshDiagnostics();
        QVERIFY(widget.fpsBubble()->isHidden());
        QVERIFY(widget.failedBubble()->isHidden());

        camera.publishGrabState(false);
        QTRY_VERIFY(!widget.live()->isChecked());
    }
    void periodicDiagnosticsLogIsDebugOnly() {
        setHostDebug(false);
        CameraSystem system;
        ObservedCamera camera(&system);
        ControlWidget widget(nullptr, &camera);
        camera.publishGrabState(true);
        QTRY_VERIFY(widget.live()->isChecked());
        widget.setFakeDiagnostics({true, 0, 0});

        QStringList messages;
        capturedMessages = &messages;
        const QtMessageHandler previous = qInstallMessageHandler(captureMessage);
        const auto periodic = [&messages]() {
            return messages.filter(QStringLiteral("[Camera UI] diagnostics:")).size();
        };
        // The periodic line is due after five seconds of live acquisition.
        QTest::qWait(5100);
        widget.refreshDiagnostics();
        const int withDebugOff = periodic();
        setHostDebug(true);
        widget.refreshDiagnostics();
        const int withDebugOn = periodic();
        qInstallMessageHandler(previous);
        capturedMessages = nullptr;
        setHostDebug(false);

        QCOMPARE(withDebugOff, 0);
        QCOMPARE(withDebugOn, 1);
        camera.publishGrabState(false);
        QTRY_VERIFY(!widget.live()->isChecked());
    }
};
QTEST_MAIN(AcquisitionControlsTest)
#include "AcquisitionControlsTest.moc"

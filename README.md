# Camera

Basler pylon 기반 카메라 acquisition layer.

`Camera`는 장치 검색, 연결/해제, grab loop, callback dispatch, GenApi feature editing을 하나의 C++17 모듈로 묶는다. GraphicsEngine에서는 일반 2D Basler 카메라와 blaze/stereo ace 계열 multipart 3D 카메라 입력을 받아 2D image 또는 3D scene payload로 전달하는 역할을 맡는다.

## Design Goals

- pylon lifetime을 `CameraSystem` 한 곳에서 관리
- 카메라별 grab thread와 callback registry 분리
- 2D image path와 3D multipart path 명확히 분리
- GenApi node pointer를 장시간 보관하지 않고 node name 기준으로 재해석
- Qt UI는 optional utility로 유지
- 외부 vision stack(PCL/OpenCV 등)은 core camera layer에서 제외

## Capability Matrix

| Area | Entry Point | Notes |
| --- | --- | --- |
| Runtime | `CameraSystem` | `PylonInitialize()` / `PylonTerminate()` 소유 |
| Discovery | `CameraSystem::getCameraList()` | 호출 시점마다 최신 enumerate |
| Device access | `Camera::open(name)` | pylon friendly name 기반 |
| 2D acquisition | `registerGrabCallback()` | `Pylon::CPylonImage` 전달 |
| 3D acquisition | `registerGrab3DCallback()` | `Pylon::CPylonDataContainer` 전달 |
| Feature UI | `QCameraWidget` | Qt available 시 빌드 |
| Image conversion | `QtConverter.h` | pylon image -> `QImage` |

## Scope

- pylon runtime 초기화/종료: `CameraSystem`
- 전체 장치 조회/접근성 확인: `CameraSystem`
- 카메라 인스턴스 생성/소유: `CameraSystem::addCamera()`
- 단일 카메라 연결/해제/취득: `Camera`
- 2D frame callback: `CPylonImage`
- 3D multipart callback: `Pylon::CPylonDataContainer`
- GenApi node 변경 callback: node name 전달
- Qt feature tree/control widget: `QCameraWidget`
- pylon image -> `QImage` 변환: `QtConverter.h`

Out of scope:

- PCL 변환 유틸
- OpenCV 연동
- pylonDataProcessing 연동
- 앱 레벨 다중 화면/다중 dock 구성

## Build

루트 프로젝트에서 `Camera/C++`를 subdirectory로 추가한다.

```cmake
add_subdirectory(Camera/C++)
target_link_libraries(${PROJECT_NAME} PRIVATE Camera)
```

Required:

- C++17
- Basler pylon SDK

Optional:

- Qt Core/Gui/Widgets/Xml
- Qt가 있으면 `QCameraWidget`, `QtConverter.h` 기능이 같이 빌드된다.
- Qt가 없으면 core camera wrapper만 빌드된다.

## Layout

```text
Camera/
  C++/
    CameraSystem.h/.cpp        pylon runtime, device enumerate, Camera ownership
    Camera.h/.cpp              one camera connection, grab thread, callbacks
    Utility/Qt/QtConverter.h   CPylonImage -> QImage conversion
    Utility/Qt/QCameraWidget.* Qt camera control + GenApi feature tree
```

## CameraSystem

`CameraSystem`은 pylon runtime, device discovery, `Camera` 인스턴스 소유권을 관리한다.

```cpp
CameraSystem system;
auto names = system.getCameraList();
```

Discovery contract:

- `getCameraList()`는 호출 시점에 pylon enumerate를 수행하고 최신 friendly name 목록을 반환한다.
- `getCameraInfo(name)`도 최신 enumerate 후 이름을 찾는다.
- `isAccessible(name)`도 최신 enumerate 기준으로 접근 가능 여부를 반환한다.
- 내부 `_devices`, `_cameraList` 접근은 mutex로 보호한다.
- `addCamera()`는 제거/재생성 후에도 중복되지 않는 allotted number를 부여한다.
- `getCamera(number)`는 vector index가 아니라 allotted number 기준으로 검색한다.

Identity rule:

- 반환되는 camera list key는 pylon friendly name이다.
- 같은 friendly name이 중복될 수 있는 환경이면 serial 기반 API가 추가로 필요하다.

## 2D Acquisition

```cpp
#include "CameraSystem.h"

#include <pylon/PylonImage.h>
#include <atomic>
#include <condition_variable>
#include <mutex>

int main()
{
    CameraSystem system;

    auto* camera = system.addCamera();
    if (!camera->open()) {
        return 1;
    }

    std::atomic<size_t> received = 0;
    std::mutex mutex;
    std::condition_variable done;

    const auto grabId = camera->registerGrabCallback(
        [&](const Pylon::CPylonImage& image, size_t frame) {
            // image.GetWidth(), image.GetHeight(), image.GetBuffer() 등 사용
            if (++received >= 10) {
                done.notify_one();
            }

            // 처리 완료 후 다음 non-trigger frame 허가
            camera->ready();
        });

    camera->grab(10);
    {
        std::unique_lock<std::mutex> lock(mutex);
        done.wait(lock, [&] { return received >= 10; });
    }
    camera->stop();

    camera->deregisterGrabCallback(grabId);
    camera->close();
    return 0;
}
```

`grab(frames)`:

- `frames == 0`: 연속 취득
- `frames > 0`: 지정 프레임 수 취득 후 자동 정지
- 이미 grab 중이면 중복 시작하지 않는다.

`ready()`:

- non-trigger mode에서 다음 frame 처리를 허가하는 backpressure 신호다.
- callback 내부 처리나 GUI queue 반영이 끝난 뒤 호출한다.
- 호출하지 않으면 다음 frame 전달이 대기할 수 있다.

## Device Selection

```cpp
CameraSystem system;
auto cameras = system.getCameraList();

for (const auto& name : cameras) {
    if (system.isAccessible(name)) {
        auto* camera = system.addCamera();
        if (camera->open(name)) {
            // 연결 성공
        }
    }
}
```

`open(name)`:

- 최신 장치 목록에서 `name`과 일치하는 friendly name을 찾는다.
- 장치가 없거나 접근 불가면 실패한다.
- 빈 이름 `open()`은 접근 가능한 장치 하나를 선택한다.

## Multi-Camera Pattern

```cpp
CameraSystem system;

std::vector<Camera*> cameras;
for (const auto& name : system.getCameraList()) {
    if (!system.isAccessible(name)) {
        continue;
    }

    auto* camera = system.addCamera();
    if (!camera->open(name)) {
        continue;
    }

    camera->registerGrabCallback([camera, name](const Pylon::CPylonImage& image, size_t frame) {
        // name 또는 camera 포인터 기준으로 frame routing
        camera->ready();
    });

    cameras.push_back(camera);
}

for (auto* camera : cameras) {
    camera->grab();
}
```

Operational notes:

- 각 `Camera`는 자체 grab thread와 callback registry를 가진다.
- callback에서 공유 출력 버퍼/GUI/엔진에 접근하면 호출자 쪽에서 routing과 동기화가 필요하다.
- 현재 GraphicsEngine `src/main.cpp`는 카메라 1개 dock과 중앙 `GraphicsEngine` 1개만 연결한다. 동시 다중 표시 UI는 앱 레벨 확장 사항이다.

## 3D Multipart Acquisition

blaze/stereo ace/STA 계열은 모델명 기준으로 3D multipart stream으로 route된다.

```cpp
auto* camera = system.addCamera();
camera->open("Basler blaze ...");

const auto grab3DId = camera->registerGrab3DCallback(
    [camera](const Pylon::CPylonDataContainer& container, size_t frame) {
        for (size_t i = 0; i < container.GetDataComponentCount(); ++i) {
            auto component = container.GetDataComponent(i);
            if (!component.IsValid()) {
                continue;
            }

            // ComponentType_Range, ComponentType_Intensity,
            // ComponentType_Confidence 등 확인 후 변환
        }

        camera->ready();
    });

camera->grab();
```

Stream setup:

- blaze: `Range` + `Coord3D_ABC32f`, `Intensity` + `Mono16`, `Confidence` + `Confidence16`
- stereo ace/STA: `Intensity`, `Disparity`
- 3D로 판별된 카메라는 2D callback으로 fallback하지 않고 `registerGrab3DCallback()`으로 전달한다.

## Qt Utilities

### QCameraWidget

`QCameraWidget`은 카메라 선택, 연결, 단일 grab, live grab, GenApi feature tree 편집을 제공한다.

```cpp
CameraSystem system;
auto* camera = system.addCamera();

auto* dock = new QDockWidget("Camera", &mainWindow);
auto* cameraWidget = new QCameraWidget(dock, camera);
dock->setWidget(cameraWidget);
mainWindow.addDockWidget(Qt::RightDockWidgetArea, dock);
```

Threading behavior:

- refresh 시 `Camera::getUpdatedCameraList()` 호출
- 내부적으로 최신 enumerate 후 friendly name list 갱신
- node update callback은 raw `GenApi::INode*`가 아니라 node name을 받는다.
- GUI thread에서 현재 node map 기준으로 다시 resolve한다.
- camera close/disconnect 후 남은 queued update가 detached node를 만지지 않게 guard한다.

### CPylonImage -> QImage

```cpp
#include "Utility/Qt/QtConverter.h"

camera->registerGrabCallback([camera](const Pylon::CPylonImage& pylonImage, size_t frame) {
    QImage image = convertPylonImageToQImage(pylonImage);
    if (!image.isNull()) {
        // GraphicsEngine::setImage(image) 등
    }
    camera->ready();
});
```

`QtConverter.h`는 Mono/RGB/Bayer/YUV 계열 pylon pixel type을 Qt에서 표시 가능한 `QImage`로 변환한다.

## Status Callback

```cpp
const auto statusId = camera->registerStatusCallback(
    [](Camera::Status status, bool on) {
        if (status == Camera::GrabbingStatus) {
            // on=true: grabbing started, on=false: stopped
        }
        if (status == Camera::ConnectionStatus) {
            // on=true: opened, on=false: closed/removed/destroyed
        }
    });
```

Physical disconnect handling:

- grab loop stop request
- permit wait wake
- `_deviceAvailable=false`
- `GrabbingStatus=false`
- `ConnectionStatus=false`

## Node Callback

```cpp
const auto nodeId = camera->registerNodeUpdatedCallback(
    [camera](const std::string& nodeName) {
        if (nodeName.empty() || !camera->isOpened()) {
            return;
        }

        auto* node = camera->getNodeMap().GetNode(nodeName.c_str());
        if (!node || !GenApi::IsAvailable(node)) {
            return;
        }

        // 현재 node map 기준으로 값 재조회
    });
```

Rules:

- callback은 node pointer가 아니라 node name만 전달한다.
- queued UI나 다른 thread에서 raw `GenApi::INode*`를 보관하지 않는다.
- 필요 시 호출 시점의 현재 node map에서 다시 resolve한다.

## Shutdown Sequence

```cpp
camera->deregisterGrabCallback(grabId);
camera->deregisterGrab3DCallback(grab3DId);
camera->deregisterStatusCallback(statusId);
camera->deregisterNodeUpdatedCallback(nodeId);

camera->stop();
camera->close();
```

`CameraSystem` destructor는 남은 `Camera` 인스턴스를 삭제하고 `PylonTerminate()`를 호출한다.

## Constraints

- public API에 pylon SDK 타입이 노출된다. 현재 Camera submodule 내부 용도라 허용한다.
- friendly name 기반 연결은 중복 friendly name 환경에서 모호할 수 있다.
- callback은 grab thread 또는 pylon event thread에서 호출될 수 있다.
- GUI 조작은 반드시 Qt queued connection 등으로 GUI thread에서 수행한다.
- `ready()` 호출 누락은 live grab 정체로 이어질 수 있다.
- 현재 앱 main은 단일 카메라 표시 구조다. Camera submodule은 다중 인스턴스를 지원하지만, 동시 표시/저장/동기화 정책은 앱 레벨에서 구현해야 한다.

## GraphicsEngine Integration

현재 `src/main.cpp`는 다음 흐름으로 Camera input을 GraphicsEngine에 연결한다.

```cpp
CameraSystem sys;
sys.updateCameraList();
auto* cam = sys.addCamera();

auto* cameraWidget = new QCameraWidget(dock, cam);

cam->registerGrabCallback([engineGuard, cam](const Pylon::CPylonImage& pylonImage, size_t frame) {
    QImage image = convertPylonImageToQImage(pylonImage);
    QMetaObject::invokeMethod(engineGuard, [engineGuard, cam, image]() {
        if (engineGuard) {
            engineGuard->setImage(image);
        }
        cam->ready();
    }, Qt::QueuedConnection);
});

cam->registerGrab3DCallback([engineGuard, cam](const Pylon::CPylonDataContainer& container, size_t frame) {
    auto payload = BlazeAdapter::toScenePayload(container, frame);
    if (!payload.has_value()) {
        cam->ready();
        return;
    }

    QMetaObject::invokeMethod(engineGuard, [engineGuard, cam, payload = std::move(*payload)]() mutable {
        if (engineGuard) {
            engineGuard->setSceneData(payload);
        }
        cam->ready();
    }, Qt::QueuedConnection);
});
```

Integration rule:

- 2D path는 `setImage()`
- 3D path는 `BlazeAdapter::toScenePayload()` 후 `setSceneData()`
- GUI 반영 후 `ready()`

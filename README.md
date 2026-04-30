# Camera 사용 안내

Basler pylon 카메라를 C++에서 다루기 위한 작은 acquisition 라이브러리입니다.

이 모듈은 카메라 검색, 연결, 프레임 수신, callback 전달을 담당합니다. 화면 표시, 저장, 분석, 다중 장치 동기화는 이 라이브러리를 쓰는 application에서 결정합니다.

## 한눈에 보기

```cpp
#include "CameraSystem.h"

int main()
{
    CameraSystem system;

    auto* camera = system.addCamera();
    if (!camera->open()) {
        return 1;
    }

    const auto grabId = camera->registerGrabCallback(
        [camera](const Pylon::CPylonImage& image, size_t frame) {
            // image.GetWidth(), image.GetHeight(), image.GetBuffer()
            camera->ready();
        });

    camera->grab();

    // ...

    camera->deregisterGrabCallback(grabId);
    camera->stop();
    camera->close();
}
```

기본 흐름은 다음과 같습니다.

1. `CameraSystem`을 생성합니다.
2. `addCamera()`로 `Camera` 인스턴스를 만듭니다.
3. `open()` 또는 `open(name)`으로 장치를 연결합니다.
4. frame callback을 등록합니다.
5. `grab()`으로 수신을 시작합니다.
6. callback 안에서 frame을 처리한 뒤 `ready()`를 호출합니다.
7. 종료 시 callback 해제, `stop()`, `close()` 순서로 정리합니다.

## 빌드

상위 CMake project에서 다음과 같이 연결합니다.

```cmake
add_subdirectory(Camera/C++)
target_link_libraries(<target> PRIVATE Camera)
```

필수 항목:

- CMake 3.16+
- C++17
- Basler pylon SDK

선택 항목:

- Qt5 또는 Qt6 `Core`, `Gui`, `Widgets`, `Xml`
- Qt가 있으면 `QCameraWidget`, `QtConverter.h`가 함께 빌드됩니다.
- Qt가 없으면 core camera wrapper만 빌드됩니다.

pylon CMake module은 다음 경로에서 찾습니다.

- `$PYLON_DEV_DIR/lib/cmake`
- `$PYLON_ROOT/lib/cmake`
- `$PYLON_ROOT/Development/lib/cmake`
- `/opt/pylon/lib/cmake`
- Windows pylon 7/8 기본 설치 경로

## 구성

```text
Camera/
  C++/
    CameraSystem.h/.cpp
    Camera.h/.cpp
    Utility/Qt/QtConverter.h
    Utility/Qt/QCameraWidget.h/.cpp
```

`CameraSystem`은 다음을 담당합니다.

- pylon runtime 초기화와 종료
- 장치 목록 갱신
- `Camera` 인스턴스 소유

`Camera`는 다음을 담당합니다.

- 단일 장치 연결과 해제
- 2D grab callback
- 3D multipart grab callback
- status callback
- GenApi node update callback

Qt utility는 다음 기능을 제공합니다.

- `QCameraWidget`: 장치 선택, open/close, single/live grab, GenApi feature tree
- `QtConverter.h`: `Pylon::CPylonImage`를 `QImage`로 변환

## 장치 찾기

```cpp
CameraSystem system;

auto names = system.getCameraList();
for (const auto& name : names) {
    if (system.isAccessible(name)) {
        auto info = system.getCameraInfo(name);
    }
}
```

동작 기준은 다음과 같습니다.

- `getCameraList()`는 호출할 때마다 pylon enumerate를 다시 수행합니다.
- 장치 이름은 pylon friendly name입니다.
- 같은 friendly name이 여러 개이면 현재 API에서는 구분이 모호합니다.
- 해당 환경에서는 serial number 기반 선택 API를 별도로 추가해야 합니다.

## 장치 열기

```cpp
auto* camera = system.addCamera();

if (!camera->open()) {
    return;
}
```

`open()`은 접근 가능한 첫 번째 장치를 엽니다.

`open(name)`은 friendly name이 같은 장치를 찾아서 엽니다.

연결 실패 조건은 다음과 같습니다.

- 장치가 없습니다.
- 장치에 접근할 수 없습니다.
- pylon 예외가 발생했습니다.

닫을 때는 다음 순서를 사용합니다.

```cpp
camera->stop();
camera->close();
```

## 2D 프레임 받기

```cpp
const auto id = camera->registerGrabCallback(
    [camera](const Pylon::CPylonImage& image, size_t frame) {
        const auto width = image.GetWidth();
        const auto height = image.GetHeight();
        const void* data = image.GetBuffer();

        // 여기서 복사, 변환, queue 전달 등을 처리합니다.

        camera->ready();
    });

camera->grab();
```

`grab()` 동작은 다음과 같습니다.

- `grab()` 또는 `grab(0)`: 계속 수신합니다.
- `grab(n)`: n frame을 받은 뒤 정지합니다.
- 이미 grab 중이면 다시 시작하지 않습니다.

callback 안에서는 다음 흐름을 권장합니다.

- frame 처리에 쓸 data만 복사하거나 queue로 넘깁니다.
- 오래 걸리는 작업은 다른 thread나 queue로 넘깁니다.
- frame 처리가 끝나면 `ready()`를 호출합니다.

## 3D multipart 프레임 받기

```cpp
const auto id = camera->registerGrab3DCallback(
    [camera](const Pylon::CPylonDataContainer& container, size_t frame) {
        for (size_t i = 0; i < container.GetDataComponentCount(); ++i) {
            auto component = container.GetDataComponent(i);
            if (!component.IsValid()) {
                continue;
            }

            const auto type = component.GetComponentType();
            // ComponentType_Range, ComponentType_Intensity 등으로 분기합니다.
        }

        camera->ready();
    });

camera->grab();
```

3D stream 동작은 다음과 같습니다.

- 3D로 판별된 장치는 `registerGrab3DCallback()`으로 전달됩니다.
- 3D stream 설정은 `Camera::configureStreamForConnectedCamera()` 내부에서 처리됩니다.
- 3D stream은 2D callback으로 fallback하지 않습니다.

## `ready()` 규칙

`ready()`는 non-trigger continuous grab에서 다음 frame 처리를 허가하는 신호입니다.

다음 경우에는 반드시 호출합니다.

- frame 처리가 끝났습니다.
- frame을 drop했습니다.
- frame을 queue로 넘겼고 더 기다리지 않아도 됩니다.
- 변환 실패로 frame을 버렸습니다.

호출하지 않으면 다음 문제가 생길 수 있습니다.

- 다음 frame 전달이 막힙니다.
- live grab이 멈춘 것처럼 보입니다.

## 상태 받기

```cpp
const auto id = camera->registerStatusCallback(
    [](Camera::Status status, bool on) {
        if (status == Camera::GrabbingStatus) {
            // on == true: grab 시작
            // on == false: grab 정지
        }

        if (status == Camera::ConnectionStatus) {
            // on == true: 연결됨
            // on == false: 닫힘, 제거됨, 파괴됨
        }
    });
```

status callback은 UI 표시, log, reconnect 판단에 사용할 수 있습니다.

## GenApi node 변경 받기

```cpp
const auto id = camera->registerNodeUpdatedCallback(
    [camera](const std::string& nodeName) {
        if (nodeName.empty() || !camera->isOpened()) {
            return;
        }

        auto* node = camera->getNodeMap().GetNode(nodeName.c_str());
        if (!node || !GenApi::IsAvailable(node)) {
            return;
        }

        // 현재 node map에서 값을 다시 읽습니다.
    });
```

node callback 규칙은 다음과 같습니다.

- callback은 node pointer가 아니라 node name을 전달합니다.
- `GenApi::INode*`를 queue나 다른 thread에 저장하지 않습니다.
- 값을 읽을 때 현재 node map에서 다시 찾습니다.

## Qt에서 쓰기

### Camera control widget

```cpp
auto* widget = new QCameraWidget(parent, camera);
```

제공 기능은 다음과 같습니다.

- 장치 목록 새로고침
- open / close
- single grab / live grab
- GenApi feature tree
- close/disconnect 뒤 남은 queued node update 방어

### QImage 변환

```cpp
#include "Utility/Qt/QtConverter.h"

QImage image = convertPylonImageToQImage(pylonImage);
```

Mono, RGB, Bayer, YUV 계열 pylon pixel format을 Qt 표시용 `QImage`로 변환합니다.

## 종료 순서

```cpp
camera->deregisterGrabCallback(grabId);
camera->deregisterGrab3DCallback(grab3DId);
camera->deregisterStatusCallback(statusId);
camera->deregisterNodeUpdatedCallback(nodeId);

camera->stop();
camera->close();
```

정리 순서는 다음과 같습니다.

- callback을 먼저 해제합니다.
- grab loop를 정지합니다.
- 장치를 닫습니다.
- 마지막에 `CameraSystem` lifetime을 끝냅니다.

## Threading 주의

- grab callback은 내부 grab thread에서 호출될 수 있습니다.
- pylon event callback은 GUI thread가 아닐 수 있습니다.
- 공유 data 접근은 호출자 쪽에서 동기화합니다.
- Qt widget update는 GUI thread로 넘깁니다.
- callback 등록/해제 map은 내부 mutex로 보호됩니다.

## 문제 확인

장치 목록이 비어 있으면 다음을 확인합니다.

- pylon SDK 설치 상태
- camera 전원과 cable 연결 상태
- pylon Viewer에서 장치가 보이는지 여부
- `PYLON_ROOT` 또는 `PYLON_DEV_DIR` 설정

`open(name)`이 실패하면 다음을 확인합니다.

- friendly name이 실제 목록과 같은지 여부
- 다른 process가 장치를 잡고 있는지 여부
- `isAccessible(name)` 결과

grab이 멈춘 것처럼 보이면 다음을 확인합니다.

- callback에서 `ready()`를 호출하는지 여부
- callback 안에서 오래 걸리는 작업을 실행하는지 여부
- physical disconnect status callback 발생 여부

Qt UI가 불안정하면 다음을 확인합니다.

- GUI thread 밖에서 widget을 만지는지 여부
- node pointer를 저장하지 않고 node name으로 다시 resolve하는지 여부

## 제한

- API에는 pylon SDK 타입이 노출됩니다.
- friendly name 기반 선택은 중복 이름 환경에 약합니다.
- 저장, 화면 표시, 후처리, 다중 장치 동기화는 이 모듈 밖의 책임입니다.

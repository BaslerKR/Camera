# Camera

> Basler pylon 카메라를 C++ 환경에서 제어하고 이미지를 수신하기 위한 경량 취득(Acquisition) 라이브러리입니다.

---

## 📌 주요 특징
- **카메라 제어**: Basler Pylon SDK를 래핑하여 검색, 연결 수립, Single/Continuous Grabbing을 관리합니다.
- **3D 멀티파트 스트림 지원**: pylon 3D 데이터 수신을 위한 3D grab 콜백 인터페이스를 제공합니다.
- **Qt 확장 위젯 제공**: 장치 선택, 연결 및 파라미터 튜닝이 가능한 `QCameraWidget` 위젯과 Pylon 이미지를 `QImage`로 고속 변환하는 `QtConverter`를 내장하고 있습니다.

## 🛠️ 요구 사양 및 의존성
- **OS**: macOS / Windows
- **언어 표준**: C++17 이상
- **의존 라이브러리**:
  - Basler Pylon SDK (환경변수 `PYLON_ROOT` 또는 `PYLON_DEV_DIR` 설정 필요)
  - Qt5 또는 Qt6 (Core, Gui, Widgets, Xml - 선택 사항, UI 활성화 시 필요)

## 🚀 빌드 및 사용 예제

### 1. 빌드 방법
상위 CMake 프로젝트에서 하위 디렉토리로 추가하여 링크합니다.
```cmake
add_subdirectory(Camera/C++)
target_link_libraries(<target> PRIVATE Camera)
```

### 2. 사용 예제
```cpp
#include "CameraSystem.h"

int main()
{
    CameraSystem system;
    Camera* camera = system.addCamera();

    if (!camera->open()) return 1;

    camera->registerGrabCallback([](const Pylon::CPylonImage& image, size_t frame) {
        // 이미지 처리 ...
        camera->ready(); // 다음 프레임 수신 허가
    });

    camera->grab();

    // ... 사용 중지 시 ...
    camera->stop();
    camera->close();
}
```

## ⚠️ 개발 주의사항
- **`ready()` 호출**: 실시간 Grabbing 루프의 백프레셔 제어를 위해 콜백 내부 처리가 끝나면 반드시 `camera->ready()`를 호출해야 합니다.
- **스레드 안전성**: 프레임 취득 콜백은 내부 Grab 스레드에서 직접 실행되므로 GUI 리소스 수정 시 Qt 스레드 큐잉(`QMetaObject::invokeMethod`) 처리가 필요합니다.

# Camera

> Basler pylon 카메라를 C++ 환경에서 다루기 위한 경량 이미지 취득(Acquisition) 라이브러리 모듈입니다.

이 모듈은 다중 카메라 검색, 연결 수립, 실시간 프레임 수신(2D 및 3D) 및 비동기 콜백 디스패치를 전담합니다. 화면 렌더링, 저장, 알고리즘 분석 및 다중 장치 동기화는 이 라이브러리를 가져다 쓰는 상위 Application에서 책임집니다.

---

## 📌 주요 특징 (Key Features)
- **카메라 제어 및 수신**: Basler Pylon SDK를 추상화하여 카메라 검색, 연결 제어, Single/Continuous Grabbing을 편리하게 관리합니다.
- **멀티파트 3D 스트림 지원**: pylon 3D 데이터 컨테이너를 직접 획득할 수 있는 multipart 3D grab 콜백 인터페이스를 제공합니다.
- **Qt6 통합 위젯 탑재**: 장치 탐색, 연결 제어, Live/Single Grabbing 및 GenApi 파라미터 트리를 렌더링할 수 있는 독립 위젯(`QCameraWidget`)과 Pylon 이미지를 `QImage`로 실시간 변환하는 `QtConverter`를 제공합니다.
- **아키텍처 경계**: 렌더링 엔진인 `GraphicsEngine`과의 직접적인 순환 의존성을 피하기 위해, 호스트가 GraphicsEngine 컴파일을 수행할 때에만 빌드 스위치에 의해 `BlazeScene3DAdapter` 컴파일이 추가되도록 유연하게 설계되었습니다.

## 🛠️ 요구 사양 및 의존성 (Prerequisites & Dependencies)
- **OS**: macOS / Windows
- **언어 표준**: C++17 이상
- **필수 의존성**:
  - **Basler Pylon SDK**: 버전 7/8 이상 (시스템에 적절히 설치되어 있어야 하며 환경변수 `PYLON_ROOT` 또는 `PYLON_DEV_DIR` 설정 필요)
- **선택적 의존성**:
  - **Qt5 또는 Qt6** (Core, Gui, Widgets, Xml): 연동 시 GUI 위젯(`QCameraWidget`) 및 `QtConverter`가 활성화됩니다. Qt 부재 시 핵심 C++ 카메라 래퍼 라이브러리만 빌드됩니다.

## 🚀 시작하기 (Quick Start & Build)

### 1. 빌드 방법
상위 CMake 프로젝트에서 다음과 같이 하위 디렉토리를 연결하여 라이브러리를 링크할 수 있습니다.

```cmake
# CMakeLists.txt 예시
add_subdirectory(Camera/C++)
target_link_libraries(<your_target> PRIVATE Camera)
```

### 2. 사용 예제 (API Usage)
```cpp
#include "CameraSystem.h"
#include <iostream>

int main()
{
    CameraSystem system;

    // 장치 추가 및 열기
    Camera* camera = system.addCamera();
    if (!camera->open()) {
        std::cerr << "카메라를 열 수 없습니다." << std::endl;
        return 1;
    }

    // 2D 프레임 Grab 콜백 등록
    const auto grabId = camera->registerGrabCallback(
        [camera](const Pylon::CPylonImage& image, size_t frameIndex) {
            std::cout << "수신 프레임 #" << frameIndex
                      << " (크기: " << image.GetWidth() << "x" << image.GetHeight() << ")" << std::endl;

            // 프레임 처리 완료 신호 필수 전달
            camera->ready();
        });

    // Grabbing 시작
    camera->grab();

    // ... 수신 대기 및 작업 수행 ...

    // 리소스 정리 및 정리 순서
    camera->deregisterGrabCallback(grabId);
    camera->stop();
    camera->close();
}
```

---

## 📂 디렉토리 구조 (Directory Structure)

```text
Camera/
└── C++/
    ├── CameraSystem.h/.cpp       # Pylon 런타임 수명주기 및 장치 탐색 관리
    ├── Camera.h/.cpp             # 개별 카메라 연결, 파라미터 제어 및 프레임 취득 루프
    ├── CMakeLists.txt            # 모듈 CMake 빌드 구성 및 GraphicsEngine 결합 판별
    └── Utility/
        ├── Qt/
        │   ├── QCameraWidget.h/.cpp  # Qt 카메라 제어 및 피처 트리 위젯
        │   └── QtConverter.h         # Pylon::CPylonImage -> QImage 고성능 변환 유틸리티
        └── GraphicsEngine/
            └── BlazeScene3DAdapter.h/.cpp # GraphicsEngine 3D 씬 데이터로 변환하는 어댑터
```

---

## ⚠️ 아키텍처 규칙 및 제약 (Boundaries & Rules)
- **`ready()` 규칙**: Non-trigger continuous grab 상태에서 다음 프레임 수신 백프레셔를 허가하기 위해, Grab 콜백 내부 처리가 끝나면(혹은 프레임을 드롭하는 상황이라도) **반드시 `camera->ready()`를 호출해야 합니다.** 호출이 누락되면 실시간 스트리밍이 멈춘 상태로 대기하게 됩니다.
- **스레딩 제약 (Threading Boundaries)**:
  * 프레임 획득 콜백은 내부 Grab 전용 워커 스레드에서 직접 호출될 수 있습니다.
  * 콜백 내부에서 멤버 변수나 공유 데이터에 액세스할 때는 반드시 호출자 측에서 동기화 처리를 수행해야 합니다.
  * GUI 위젯을 직접 갱신하는 등의 모든 UI 연동 처리는 반드시 Qt GUI 스레드로 이벤트(`QMetaObject::invokeMethod`)를 큐잉하여 전달해야 합니다.
- **종료 순서 규칙**: 리소스를 해제하고 카메라를 닫을 때는 반드시 **[콜백 해제(Deregister)] ➡️ [루프 중지(Stop)] ➡️ [장치 종료(Close)]** 순서대로 안전하게 해제하여 메모리 해제 시점의 크래시를 방지하십시오.

## 📝 라이선스 (License)
본 모듈은 상용 독점 라이선스를 따르며 권한이 없는 재배포를 금지합니다.

# Pure C++ Class for Basler pylon camera API
Basler **pylon API**를 하나의 C++ 클래스 인터페이스로 간단히 다루기 위한 프로젝트입니다.  
pylon API에 익숙하지 않은 사용자도 빠르게 카메라 제어를 시작할 수 있도록 설계되었습니다.

> [!WARNING]  
> 본 프로젝트는 여러분의 애플리케이션에 **정확히 맞도록 설계되지 않았으며**, 이에 대한 **보증을 제공하지 않습니다**.  
> 통합 전/후로 코드 및 설정을 검토하여 **상황에 맞게 수정**하십시오.

> [!NOTE]
> 다양한 이벤트/로그를 **자세히 출력**합니다. 이는 **성능 저하**를 유발할 수 있으니 운영환경에서는 로그 레벨/출력 빈도를 조정하는 것을 권장합니다.

---

## 다운로드
GitHub 페이지 상단의 **`<> Code`** 버튼에서 ZIP 다운로드가 가능하며, Git을 사용한다면 **submodule**로 추가할 수 있습니다.

```bash
git submodule add https://github.com/BaslerKR/Camera.git
git submodule update
```

---

## 통합(CMake)
프로젝트 내 `CMakeLists.txt`를 참고하여 여러분의 CMake 구성에 통합할 수 있습니다.  
예: 해당 디렉토리를 리포지토리 내부에 두고 아래처럼 추가:

```cmake
# 예시: 프로젝트 루트 CMakeLists.txt에서
add_subdirectory(CameraSystem/C++)
target_link_libraries(${PROJECT_NAME} PUBLIC CameraSystem)
```

---

## 사용 방법
```cpp
#include "CameraSystem.h"

int main() {
    CameraSystem system;
    auto camera = system.addCamera();   // 보유 카메라 중 하나를 시스템에 추가

    // 카메라 연결
    // 인자 미지정: 검색된 첫 번째 카메라에 연결
    // 인자 지정: FriendlyName 일치 카메라에 연결
    camera->open(); 

    // 이미지 프레임 수신 콜백
    camera->onGrabbed([&](const CPylonImage& pylonImage, size_t frameCnt) {
        // 여기서 사용자의 이미지 처리 절차 수행
        convertPylonImageToYourFormat(pylonImage);
        doProcessingYourWay();

        // 처리 완료 후 다음 프레임 수신 허가
        camera->ready();
    });

    // 카메라 상태 변경 콜백
    camera->onCameraStatus([&](Camera::Status status, bool on) {
        // Camera::Status::GrabbingStatus: 촬영 중이면 on=true, 정지로 전환 시 false
        // Camera::Status::ConnectionStatus: 연결 시 on=true, 연결 해제 시 false
    });

    // 파라미터 변경/정보 전달 콜백
    camera->onNodeUpdated([&](GenApi::INode* node) {
        // 상세 사용법은 Basler 공식 문서 참조
        // 또는 본 프로젝트의 Utility/Qt/QCameraWidget.h 참고
    });

    // 필요 시 그랩 시작/정지 등의 제어를 수행
    // camera->startGrabbing();
    // ...
    // camera->stopGrabbing();

    return 0;
}
```

---

## 이벤트 콜백
- **`onGrabbed`**  
  새 이미지 프레임을 수신하면 호출됩니다. 내부에서 이미지 변환/처리 후 `camera->ready()` 를 호출해 다음 프레임 준비를 알립니다.
- **`onCameraStatus`**  
  연결/촬영 상태 변경 시 호출됩니다.  
  - `GrabbingStatus`: 촬영 중 `true`, 정지 시 `false`  
  - `ConnectionStatus`: 연결됨 `true`, 해제됨 `false`
- **`onNodeUpdated`**  
  파라미터 변경 또는 관련 정보 전달 시 호출됩니다. 자세한 노드/GenApi 사용은 Basler 공식 문서 또는 프로젝트 내 `Utility/Qt/QCameraWidget.h`를 참고하세요.

---

## 문의
본 프로젝트는 지속적으로 업데이트되지만 **보증하지 않습니다**.  
문의/버그/제안은 **이 저장소의 Issues**에 등록할 수 있습니다.

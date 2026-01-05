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
add_subdirectory(Camera/C++)
target_link_libraries(${PROJECT_NAME} PUBLIC Camera)
```

---

## 사용 방법
```cpp
#include "CameraSystem.h"

int main() {
    CameraSystem system;
    system.updateCameraList();
    auto camera = system.addCamera();   // 보유 카메라 중 하나를 시스템에 추가

    // 카메라 연결
    // cameraName = "" -> 첫번째 검색된 카메라
    // cameraName = "Basler acA1300-60gm SERIALNUMBER" -> 특정 카메라의 Freindly name
    camera->open(std::string cameraName);

    // 촬영
    // frameCnt = 0 -> 연속 촬영
    // frameCnt = NUM -> 정해진 수 촬영
    camera->grab(size_t frameCnt);

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

    // ...
    camera->stop();

    return 0;
}
```
### 카메라 인스턴스 생성
모든 카메라들은 `CameraSystem`Class에서 관리됩니다.
연결에 필요한 카메라 개수만큼 아래의 코드로 Camera를 생성합니다. 
```cpp
CameraSystem system;
system.addCamera();
```
### 연결 가능한 리스트 업데이트
`CameraSystem`클래스를 생성한 후에는 명시적으로 카메라 리스트를 업데이트 합니다.
```cpp
system.updateCameraList();
```
또는 프로그램 실행 후 카메라의 연결 상태가 바뀐 경우(추가, 제거 등) `Camera`클래스에서 아래 함수를 호출하여 리스트를 업데이트할 수 있습니다.
```cpp
// auto camera = system.addCamera(); 이미 선언되어 있는 경우에는 제외
camera->getUpdatedCameraList();
```

### 카메라 인스턴스에서 카메라 연결
```cpp
// auto camera = system.addCamera(); 이미 선언되어 있는 경우에는 제외
// 카메라의 FriendlyName을 매개변수로 입력합니다.
camera.open("Basler acA1300-60gm (24070434)");
// 또는 시스템에 연결된 아무 카메라나 연결하고자 하는 경우
camera.open();
```

### 카메라 동작
```cpp
// 카메라 연결 후
// 연속 취득 모드 (Continuous Grab)
camera.grab();
// 또는 일정한 프레임 수 획득 (예로 10장)
camera.grab(10);
// 카메라 동작 정지
camera.stop();


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





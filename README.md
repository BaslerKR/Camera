# pylon Camera API Wrapper (pure C++)
>[!IMPORTANT]
>해당 예제는 버그, 구조 상 한계, 그 외 기능을 제한하는 여러 요소가 있을 수 있으며, 이에 대해서 보증하지 않습니다.
이 프로젝트를 이용하여 프로그램을 개발하시는 경우에는 이 점을 유의하셔서 사용하시기 바랍니다.

>[!TIP]
>Qt로 개발하시는 경우에서는 [Qylon](https://github.com/minus-monster/Qylon)을 참고하시기 바랍니다.


처음 pylon Camera API를 사용하시는 분들을 위해, 더 쉽게 접근하여 사용하실 수 있도록 하기 위한 예제입니다.
해당 프로젝트는 정기적으로 업데이트될 예정이며, 이용 중 불편 사항 또는 문제 발생 시 해당 페이지 내 Issues 항목을 이용하시기 바랍니다.


## 구조
```
CameraSystem (여러 카메라 클래스를 제어하는 시스템 클래스)
├── Camera (단일 카메라 클래스)
└── Utility 
    └── Qt (QImage 변환, Qt Widget 예제)
```

## 다운로드
### Github 페이지를 통한 다운로드

-  해당 페이지 우측 상단에 있는 [<> Code] 탭을 클릭하여 소스 코드 및 통합 방법을 안내받을 수 있습니다.

### Git을 이용한 다운로드
  
-  프로젝트가 이미 Git으로 관리되고 있는 경우
```
git submodule add https://github.com/BaslerKR/Camera
```
 
-  새로운 프로젝트로, Git 관리가 되어 있지 않은 경우
```
git clone https://github.com/BaslerKR/Camera
```

## 통합 방법

해당 프로젝트를 CMake를 통해 귀하의 프로젝트에 통합하시기를 권장하고 있습니다.

하기 제공되는 코드를 귀하의 CMakeLists.txt 안에 붙여넣기 합니다.
```cmake
add_subdirectory(CameraSystem/C++)
target_link_libraries(${PROJECT_NAME} PUBLIC CameraSystem)
```

이후 CMake를 Build하여 해당 프로젝트가 귀하의 프로젝트 내에 올바르게 통합되었는 지 확인하시기 바랍니다.

## 사용 방법
```c++
#include "CameraSystem.h"

CameraSystem system;

// 필요한 경우, 사용 가능한 전체 카메라 리스트 호출
system.updateCameraList();

// 카메라 클래스 추가
auto camera = system.addCamera();

// 활성화된 카메라 리스트 확인 (카메라 이름 확인용)
for(const auto &current: camera->getUpdatedCameraList()){
    std::cout << current << std::endl;
}

/// 카메라 연결
/// CAMERA_NAME: 카메라의 FriendlyName
/// 입력이 없을 시 가장 먼저 검색된 카메라 연결
camera.open(CAMERA_NAME);

/// 영상 취득
/// NUM: 취득할 이미지 개수
/// 변수가 없는 경우 연속 취득
camera.grab(NUM);

// 영상 취득 정지
camera.stop();

// 카메라 이미지 획득 시 콜백 함수
camera->onGrabbed([=](const CPylonImage &image, size_t frame){
  // CPylonImage를 원하는 형태로 변환합니다.
  auto width = image.GetWidth();
  auto height = image.GetHeight();
  // 필요한 타입으로 변환합니다.
  const uchar* buffer = static_cast<const uchar*>(image.GetBuffer());
  auto imageForamt = image.GetPixelType();
  switch(imageFormat){
  case Pylon::PixelType_Mono8:
    /// 8비트 모노 이미지 출력
    /// 원하는 포맷으로 변환
    /// Example: Image outputImage = Image(buffer, width, height, imageFormat);
    break;
  }
  // 이후 프로세싱 처리

  /*
  *
  *  Do Something;
  *
  */

  // 다음 프레임 대기
  cam->ready();
}

```










# ascii-player

> 영상이면 일단 터미널에 구겨 넣습니다. 소리도 납니다. 진짜임.

`ascii-player`는 FFmpeg가 읽을 수 있는 영상을 Windows 콘솔에서 실시간 ASCII
아트로 재생하는 C++20 플레이어입니다.

Bad Apple 전용 장난감 아닙니다. `mp4`, `mkv`, `avi`, `mov`, `webm`, `gif` 등
영상 파일이면 대충 다 던져 보세요.

```powershell
ascii-player.exe video.mp4
ascii-player.exe "C:\Videos\movie.mp4"
ascii-player.exe --color --charset dense video.mkv
```

프레임을 이미지 파일 수천 장으로 뽑는 무식한 짓은 안 합니다. FFmpeg로 바로
디코딩하고, bounded queue에 당장 필요한 프레임만 올려서 메모리도 얌전히 씁니다.

## 뭐가 되는데?

- FFmpeg 스트리밍 방식으로 영상과 오디오를 실시간 디코딩
- 밝기 256단계 LUT로 픽셀을 ASCII 문자로 빠르게 변환
- 디테일 빡센 classic 문자셋과 깔끔한 `@%#*+=-:. ` 문자셋 제공
- 터미널 크기에 맞춰 영상 자동 리사이즈
- 글자는 세로로 길쭉하다는 현실까지 반영한 화면 비율 보정
- 콘솔 크기를 재생 중에 바꿔도 알아서 다시 맞춤
- 이전 프레임과 달라진 글자만 쓰는 Dirty Rendering
- 영상 PTS에 맞춘 재생 타이밍과 늦어 버린 프레임 자동 드롭
- Windows 콘솔 16색 컬러 ASCII 모드
- FFmpeg 리샘플링 + Windows `waveOut` 오디오 출력
- 일시정지, 재개, FPS, 현재 시간/전체 시간 표시
- Decode / Convert / Render / Audio 4스레드 파이프라인

그리고 당연히 `system("cls")` 같은 화면 번쩍번쩍 대참사는 없습니다.

## 일단 빌드부터 ㄱㄱ

준비물은 이렇습니다.

- Windows 10 이상
- CMake 3.24 이상
- C++20 컴파일러
- 컴파일러 ABI와 맞는 FFmpeg shared development bundle
  (`include`, `lib`, `bin` 폴더가 들어 있어야 함)

MinGW라면 [BtbN FFmpeg Builds](https://github.com/BtbN/FFmpeg-Builds)의
`win64-gpl-shared` 또는 `win64-lgpl-shared` 번들이 편합니다.

MSVC를 쓴다면 MSVC용 import library가 들어 있는 FFmpeg 패키지를 준비해 주세요.
FFmpeg 빌드와 코덱별 라이선스는 배포 전에 한 번 확인하는 센스도 챙깁시다.

### MinGW + Ninja

```powershell
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DFFMPEG_ROOT="C:\Libraries\ffmpeg"

cmake --build build-ninja --parallel
```

프로젝트 경로에 한글이 있다면 `MinGW Makefiles`가 가끔 경로를 맛있게 말아먹습니다.
그냥 Ninja 쓰면 마음이 편합니다.

### Visual Studio

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 `
  -DFFMPEG_ROOT="C:\Libraries\ffmpeg-msvc"

cmake --build build-msvc --config Release
```

빌드가 끝나면 CMake가 `FFMPEG_ROOT\bin`의 DLL도 실행 파일 옆으로 챙겨 줍니다.

## 재생하기

제일 평범하게는 영상 경로만 넣으면 끝입니다.

```powershell
ascii-player.exe "C:\Videos\movie.mp4"
```

흑백 터미널 감성으로 보기:

```powershell
ascii-player.exe --charset classic video.mp4
```

컬러까지 야무지게 보기:

```powershell
ascii-player.exe --color --charset dense video.mp4
```

### 옵션 모음

```text
ascii-player.exe [options] <video>

--charset classic|dense  ASCII 문자셋 선택
--color                  Windows 16색 컬러 출력
--no-audio               조용히 보기
--no-status              하단 상태 표시줄 숨기기
--benchmark              화면/소리/실시간 대기 없이 파이프라인 검사
--max-frames N           N프레임만 처리하고 종료
-h, --help               도움말
```

재생 중에는:

- `Space`: 잠깐 멈춤 / 다시 재생
- `Q` 또는 `Esc`: 탈출

## 속은 어떻게 생겼노

```text
                         ┌─ decoded AVFrame queue (6) ─ Convert ─ ASCII queue (3) ─ Render
FFmpeg Demux/Video Decode│
                         └─ encoded audio queue (128) ─ FFmpeg Audio Decode ─ waveOut
```

각 큐는 크기가 고정되어 있습니다. 뒤쪽 스레드가 버거워하면 앞쪽 스레드가 잠시
기다리기 때문에 영상 전체가 RAM으로 우르르 들어가는 일은 없습니다.

`AVFrame`과 `AVPacket`은 FFmpeg reference counting + RAII smart pointer로
관리합니다. 끝날 때 메모리 누수 파티가 열리지 않게 해뒀습니다.

Renderer는 바뀐 셀이 적으면 달라진 연속 구간만 쓰고, 화면이 크게 바뀌면 행
단위로 한 번에 씁니다. 콘솔 출력 호출을 줄이면서도 장면 전환은 빠르게 처리하는
작은 잔머리입니다.

## 벤치마크도 해보자고

`--benchmark`는 화면과 소리를 끄고 Decode → Convert → Consume 파이프라인만
전속력으로 돌립니다. CI나 성능 확인할 때 쓰면 됩니다.

```powershell
ascii-player.exe --benchmark --max-frames 300 samples\badapple.mp4
```

## Bad Apple로 국룰 테스트

이런 걸 만들었으면 Bad Apple을 돌려보는 게 예의입니다.

테스트 영상은
[AlexandreSenpai/Bad-Apple의 `assets/badapple.mp4`](https://github.com/AlexandreSenpai/Bad-Apple/blob/main/assets/badapple.mp4)를
사용했습니다. 약 9.8 MB이고 저장소에는 올라가지 않도록 `.gitignore`에 넣었습니다.

```powershell
curl.exe -L -o samples\badapple.mp4 `
  https://raw.githubusercontent.com/AlexandreSenpai/Bad-Apple/main/assets/badapple.mp4

build-ninja\ascii-player.exe samples\badapple.mp4
```

로컬 검증에서는 MP4, MKV, AVI, MOV, WebM, GIF까지 전부 통과했습니다. Bad Apple
H.264/AAC 영상도 300프레임 Decode / Convert / Consume 올클리어했습니다.

## 아직 안 되는 거

- Seek
- 재생 속도 변경

둘 다 언젠가 넣으면 맛있을 기능입니다. 지금은 재생, 일시정지, 오디오, 컬러,
리사이즈, FPS 유지까지는 제대로 굴러갑니다.

## 한 줄 요약

```text
영상 넣기 → FFmpeg가 해체하기 → ASCII로 조립하기 → 터미널에서 춤추기
```

이제 영상 하나 던져 보십쇼. 터미널이 갑자기 영화관 됩니다 ㅋㅋ

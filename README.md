# ascii-player

Windows 콘솔에서 FFmpeg가 지원하는 영상을 실시간 ASCII 아트로 재생하는
C++20 플레이어입니다. 프레임 이미지를 디스크에 만들지 않으며, bounded queue에
현재 처리 중인 소수의 프레임과 압축 오디오 패킷만 유지합니다.

```powershell
ascii-player.exe video.mp4
ascii-player.exe "C:\Videos\movie.mp4"
ascii-player.exe --color --charset dense video.mkv
```

## 기능

- FFmpeg 스트리밍 demux 및 영상/오디오 디코딩
- 256-entry luminance-to-character lookup table
- 고밀도 classic 문자셋과 `@%#*+=-:. ` 문자셋
- 문자 셀의 가로세로 비율을 보정한 자동 리사이즈
- 콘솔 뷰포트 변경 자동 감지
- `WriteConsoleOutputCharacterA`/`WriteConsoleOutputAttribute` 기반 렌더링
- 이전/현재 프레임을 비교해 변경된 연속 구간만 쓰는 Dirty Rendering
- 프레임 PTS 기반 타이밍 및 늦은 프레임 드롭
- Windows 16색 ASCII 모드
- FFmpeg resampling + Windows `waveOut` 오디오
- 일시정지/재개, FPS 및 재생 시간 표시
- Decode → Convert → Render, Decode → Audio bounded producer/consumer pipeline

## 빌드 요구사항

- Windows 10 이상
- CMake 3.24 이상
- C++20 컴파일러
- 컴파일러와 ABI가 일치하는 FFmpeg shared development bundle
  (`include`, `lib`, `bin` 디렉터리 필요)

이 프로젝트를 MinGW로 빌드할 때는
[BtbN FFmpeg Builds](https://github.com/BtbN/FFmpeg-Builds)의
`win64-gpl-shared` 또는 `win64-lgpl-shared` 번들을 사용할 수 있습니다.
MSVC에서는 MSVC용 import library가 포함된 FFmpeg 패키지를 사용해야 합니다.
선택한 FFmpeg 빌드와 코덱의 라이선스 조건은 배포 전에 별도로 확인하세요.

### MinGW + Ninja

```powershell
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DFFMPEG_ROOT="C:\Libraries\ffmpeg"
cmake --build build-ninja --parallel
```

한글이 포함된 경로에서는 오래된 `MinGW Makefiles`가 경로를 ANSI로 변환할 수
있으므로 Ninja generator를 권장합니다.

### Visual Studio

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 `
  -DFFMPEG_ROOT="C:\Libraries\ffmpeg-msvc"
cmake --build build-msvc --config Release
```

빌드 후 CMake가 `FFMPEG_ROOT\bin`의 DLL을 실행 파일 디렉터리에 복사합니다.

## 사용법

```text
ascii-player.exe [options] <video>

--charset classic|dense  문자셋 선택
--color                  Windows 16색 출력
--no-audio               오디오 비활성화
--no-status              상태 표시줄 숨김
--benchmark              콘솔/오디오/실시간 대기 없이 파이프라인 검사
--max-frames N           N개 영상 프레임 처리 후 종료
-h, --help               도움말
```

재생 중 `Space`는 일시정지/재개, `Q` 또는 `Esc`는 종료입니다.

`--benchmark`는 stdout이 실제 콘솔이 아닌 CI나 리디렉션 환경에서도 디코더와
ASCII 변환 파이프라인을 검사하기 위한 모드입니다.

```powershell
ascii-player.exe --benchmark --max-frames 300 samples\badapple.mp4
```

## 스레드와 메모리 구조

```text
                         ┌─ decoded AVFrame queue (6) ─ Convert ─ ASCII queue (3) ─ Render
FFmpeg Demux/Video Decode│
                         └─ encoded audio queue (128) ─ FFmpeg Audio Decode ─ waveOut
```

큐 용량은 고정되어 있습니다. 소비자가 늦으면 producer가 대기하므로 영상 전체가
메모리에 적재되지 않습니다. `AVFrame`과 `AVPacket`은 FFmpeg reference counting을
RAII smart pointer로 감쌌습니다.

Renderer는 40% 이상 셀이 변한 프레임은 행 단위로 쓰고, 그보다 적게 변하면
변경된 연속 문자 구간만 씁니다. `system("cls")`나 프레임 파일 생성은 사용하지
않습니다.

## 구현 단계

1. CMake 프로젝트와 공용 bounded queue/재생 시계
2. FFmpeg demux 및 영상 디코더
3. swscale 기반 ASCII LUT 변환기
4. Windows Console Dirty Renderer
5. FFmpeg 오디오 디코더와 `waveOut` 버퍼 링
6. 4-thread 메인 파이프라인, 키 입력과 종료 처리
7. Release 빌드 및 MP4/MKV/AVI/MOV/WebM/GIF 회귀 검사

## 현재 제한

- Seek와 재생 속도 변경은 아직 제공하지 않습니다.
- 컬러 모드는 ANSI escape sequence가 아니라 Windows Console 16색 attribute를
  사용합니다. 이 방식이 Dirty Rendering과 더 잘 결합됩니다.
- 오디오 출력은 Windows 기본 wave mapper를 사용합니다.

## Bad Apple 테스트

로컬 테스트에 사용한 파일은
[AlexandreSenpai/Bad-Apple의 `assets/badapple.mp4`](https://github.com/AlexandreSenpai/Bad-Apple/blob/main/assets/badapple.mp4)입니다.
샘플은 약 9.8 MB이며 `.gitignore`로 제외됩니다.

```powershell
curl.exe -L -o samples\badapple.mp4 `
  https://raw.githubusercontent.com/AlexandreSenpai/Bad-Apple/main/assets/badapple.mp4

build-ninja\ascii-player.exe samples\badapple.mp4
```

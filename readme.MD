# MP Media Player

SDL3 + FFmpeg + BASS 기반의 멀티미디어 플레이어입니다.  
비디오 · 오디오 · 이미지(애니메이션 GIF 포함)를 하나의 플레이리스트로 재생하며,  
외부/내장 자막과 OSD를 지원합니다.

---

## 기능

| 범주 | 내용 |
|------|------|
| **비디오** | FFmpeg 디코딩, PTS 기반 A/V 동기화, Letterbox 렌더링 |
| **오디오** | BASS 라이브러리, FFT 스펙트럼 시각화 |
| **이미지** | 정적 이미지 (JPG/PNG/BMP/WebP/TIFF), 애니메이션 GIF |
| **자막** | 외부 SRT / ASS / SSA, FFmpeg 내장 자막 스트림 |
| **OSD** | 파일명 · 재생시간 · 볼륨 정보 오버레이 (O 키 토글) |
| **플레이리스트** | 파일 · 디렉터리 · 와일드카드 지정, 자연어 정렬 |

### 지원 포맷

| 종류 | 확장자 |
|------|--------|
| 비디오 | mp4, mkv, avi, mov, webm, flv, mpeg, mpg |
| 오디오 | mp3, wav, flac, ogg, aac, ape, m4a, opus, wma, wv, mpc … |
| 이미지 | jpg, jpeg, png, bmp, gif, webp, tif, tiff |
| 자막 | srt, ass, ssa (+ FFmpeg 내장 스트림) |

> BASS 플러그인이 로드되면 MOD/XM/IT/SID/SPC 등 레트로 포맷도 재생됩니다.

---

## 키보드 조작

| 키 | 동작 |
|----|------|
| `Space` | 일시정지 / 재개 |
| `N` / `→` | 다음 파일 (비디오·오디오: +5초, 애니메이션: +5프레임, 정적 이미지: 다음 파일) |
| `P` / `←` | 이전 파일 (비디오·오디오: -5초, 애니메이션: -5프레임, 정적 이미지: 이전 파일) |
| `R` | 처음으로 |
| `↑` / `↓` | 볼륨 +5% / -5% |
| `O` | OSD 켜기 / 끄기 |
| `F11` | 전체화면 토글 |
| `ESC` | 종료 |

마우스로 하단 진행바를 클릭하거나 드래그해 탐색할 수 있습니다.

---

## 자막

미디어 파일과 **같은 이름**의 `.srt` / `.ass` / `.ssa` 파일을 자동으로 인식합니다.

```
video.mp4
video.srt   ← 자동 로드
```

외부 자막 파일이 없으면 `.mkv` 등에 포함된 FFmpeg 내장 자막 스트림을 사용합니다.

---

## 설치 및 빌드

### 의존 라이브러리

| 라이브러리 | 용도 |
|-----------|------|
| [SDL3](https://github.com/libsdl-org/SDL) | 창·렌더링·오디오 스트림·이벤트 |
| [SDL3_image](https://github.com/libsdl-org/SDL_image) | 이미지 / 애니메이션 GIF 로딩 |
| [SDL3_ttf](https://github.com/libsdl-org/SDL_ttf) | 자막·OSD 폰트 렌더링 |
| [FFmpeg](https://ffmpeg.org/) | 비디오·오디오 디코딩, 내장 자막 |
| [BASS](https://www.un4seen.com/) | 고급 오디오 재생 + 플러그인 |

### Windows (MinGW / MSYS2)

```bash
# MSYS2 패키지 설치
pacman -S mingw-w64-x86_64-SDL3 mingw-w64-x86_64-SDL3_image \
          mingw-w64-x86_64-SDL3_ttf mingw-w64-x86_64-ffmpeg

# BASS는 un4seen.com에서 수동 다운로드 후 bass/ 폴더에 배치
make
```

### Linux (Ubuntu / Debian)

```bash
sudo apt install libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev \
                 libavcodec-dev libavformat-dev libswscale-dev libavutil-dev

# BASS는 un4seen.com에서 수동 다운로드 후 bass/ 폴더에 배치
make
```

### macOS (Homebrew)

```bash
brew install sdl3 sdl3_image sdl3_ttf ffmpeg

# BASS는 un4seen.com에서 수동 다운로드 후 bass/ 폴더에 배치
make
```

### 디렉터리 구조

```
mp/
├── mp.cpp
├── media.h / media.cpp
├── subtitle.h / subtitle.cpp
├── args.hpp
├── bass3.hpp
├── fnutil.hpp
├── util.hpp
├── bass/                  ← BASS 헤더 및 라이브러리
│   ├── bass.h
│   ├── bass.dll / libbass.so / libbass.dylib
│   ├── bassflac.dll / ...
│   └── ...
├── mp.conf                ← 설정 파일 (선택)
└── Makefile
```

---

## 사용법

```bash
# 단일 파일
mp video.mp4

# 디렉터리 전체
mp /mnt/media/videos/

# 와일드카드
mp *.mp4

# 옵션
mp --volume 0.8 --geometry 1920x1080+0+0 --fullscreen video.mp4
```

### 커맨드라인 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--geometry WxH+X+Y` | 창 크기 및 위치 | `1280x720+100+100` |
| `--width N` / `--height N` | 창 너비 / 높이 | |
| `--x N` / `--y N` | 창 위치 | |
| `-wh WxH` / `-xy X,Y` | 창 크기 / 위치 (단축) | |
| `--volume 0.0~1.0` | 초기 볼륨 | `1.0` |
| `--delay N` | 오디오 종료 후 다음 트랙까지 대기 시간(초) | `2.5` |
| `--image-display N` | 이미지 표시 시간(초) | `5.0` |
| `--short-threshold N` | 이 길이(초) 미만의 오디오는 반복 재생 | `15.0` |
| `--subtitle-font <경로>` | 자막·OSD 폰트 파일 | 시스템 자동 탐색 |
| `--subtitle-size N` | 자막·OSD 폰트 크기(pt) | `28` |
| `--fullscreen` | 전체화면으로 시작 | |

---

## 설정 파일 (mp.conf)

실행 파일과 같은 디렉터리에 `mp.conf`를 두면 기본값으로 적용됩니다.

```ini
# mp.conf 예시
fullscreen      = false
window_width    = 1280
window_height   = 720
window_x        = 100
window_y        = 100
volume          = 0.8
delay_after     = 3.0
image_display   = 6.0
short_threshold = 20.0
subtitle_font   = C:/Windows/Fonts/malgun.ttf
subtitle_size   = 30

# 확장자 목록 (쉼표 구분, 다음 줄 계속은 \)
image_exts = jpg,jpeg,png,bmp,gif,webp
audio_exts = mp3,wav,flac,ogg,aac,ape,\
             m4a,opus,wma,wv,mpc
video_exts = mp4,mkv,avi,mov,webm,flv
```

---

## 라이선스

- 이 프로젝트의 소스코드: MIT License
- [BASS](https://www.un4seen.com/): 비상업적 무료 / 상업적 유료 라이선스
- [FFmpeg](https://ffmpeg.org/legal.html): LGPL / GPL
- [SDL3](https://www.libsdl.org/): zlib License
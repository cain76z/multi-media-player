# ============================================================
#  MP Media Player – Makefile
#  지원 플랫폼: Windows (MinGW/MSYS2), Linux, macOS
# ============================================================

# ── 타겟 이름 ────────────────────────────────────────────────
TARGET  := mp

# ── 소스 파일 ────────────────────────────────────────────────
SRCS    := mp.cpp media.cpp subtitle.cpp

# ── 플랫폼 감지 ──────────────────────────────────────────────
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
else
    UNAME := $(shell uname -s)
    ifeq ($(UNAME),Darwin)
        PLATFORM := macos
    else
        PLATFORM := linux
    endif
endif

# ── 컴파일러 설정 ────────────────────────────────────────────
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra \
            -Wno-unused-parameter \
            -Wno-deprecated-declarations

# ── 공통 include ─────────────────────────────────────────────
CXXFLAGS += -I.

# ── 플랫폼별 설정 ────────────────────────────────────────────

# ──── Windows (MinGW / MSYS2) ────────────────────────────────
ifeq ($(PLATFORM),windows)
    TARGET  := mp.exe

    # pkg-config 경로 (MSYS2 MinGW64 기준)
    PKG_FLAGS := $(shell pkg-config --cflags sdl3 SDL3_image SDL3_ttf \
                     libavformat libavcodec libswscale libavutil 2>/dev/null)
    PKG_LIBS  := $(shell pkg-config --libs   sdl3 SDL3_image SDL3_ttf \
                     libavformat libavcodec libswscale libavutil 2>/dev/null)

    # BASS (헤더: bass/, 라이브러리: bass/bass.lib 또는 libbass.a)
    BASS_LIB  := -Lbass -lbass
    EXTRA_LIBS := $(BASS_LIB) -lshlwapi -lwinmm -lole32 -luuid

    LDFLAGS   := $(PKG_LIBS) $(EXTRA_LIBS)
    CXXFLAGS  += $(PKG_FLAGS) -DNOMINMAX

    # Windows 콘솔 창 억제 (GUI 앱으로 빌드하려면 -mwindows 추가)
    # LDFLAGS  += -mwindows
endif

# ──── Linux ──────────────────────────────────────────────────
ifeq ($(PLATFORM),linux)
    PKG_FLAGS := $(shell pkg-config --cflags sdl3 SDL3_image SDL3_ttf \
                     libavformat libavcodec libswscale libavutil 2>/dev/null)
    PKG_LIBS  := $(shell pkg-config --libs   sdl3 SDL3_image SDL3_ttf \
                     libavformat libavcodec libswscale libavutil 2>/dev/null)

    # BASS (헤더: bass/, 공유 라이브러리: bass/libbass.so)
    BASS_LIB  := -Lbass -lbass
    EXTRA_LIBS := $(BASS_LIB) -ldl -lpthread

    LDFLAGS   := $(PKG_LIBS) $(EXTRA_LIBS)
    CXXFLAGS  += $(PKG_FLAGS)
endif

# ──── macOS ──────────────────────────────────────────────────
ifeq ($(PLATFORM),macos)
    PKG_FLAGS := $(shell pkg-config --cflags sdl3 SDL3_image SDL3_ttf \
                     libavformat libavcodec libswscale libavutil 2>/dev/null)
    PKG_LIBS  := $(shell pkg-config --libs   sdl3 SDL3_image SDL3_ttf \
                     libavformat libavcodec libswscale libavutil 2>/dev/null)

    # BASS (헤더: bass/, dylib: bass/libbass.dylib)
    BASS_LIB  := -Lbass -lbass
    EXTRA_LIBS := $(BASS_LIB)

    LDFLAGS   := $(PKG_LIBS) $(EXTRA_LIBS) \
                 -framework CoreAudio -framework AudioToolbox \
                 -framework CoreFoundation

    CXXFLAGS  += $(PKG_FLAGS)
endif

# ── 오브젝트 파일 목록 ───────────────────────────────────────
OBJS := $(SRCS:.cpp=.o)

# ── 빌드 규칙 ────────────────────────────────────────────────
.PHONY: all clean run info

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "빌드 완료: $(TARGET)  [$(PLATFORM)]"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ── 의존 관계 ────────────────────────────────────────────────
mp.o:       mp.cpp media.h subtitle.h args.hpp fnutil.hpp util.hpp bass3.hpp
media.o:    media.cpp media.h subtitle.h util.hpp bass3.hpp
subtitle.o: subtitle.cpp subtitle.h

# ── 유틸 타겟 ────────────────────────────────────────────────
run: all
	./$(TARGET)

clean:
ifeq ($(PLATFORM),windows)
	del /Q $(subst /,\,$(OBJS)) $(TARGET) 2>nul || true
else
	rm -f $(OBJS) $(TARGET)
endif

# 디버그 빌드 (최적화 끄고 심볼 포함)
debug: CXXFLAGS += -O0 -g -DDEBUG
debug: clean all

# pkg-config 및 플랫폼 정보 출력
info:
	@echo "플랫폼  : $(PLATFORM)"
	@echo "컴파일러: $(CXX) $(shell $(CXX) --version | head -1)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LDFLAGS : $(LDFLAGS)"
	@echo ""
	@echo "── pkg-config 확인 ──────────────────────────────────"
	@pkg-config --modversion sdl3         2>/dev/null && echo "  SDL3         OK" || echo "  SDL3         NOT FOUND"
	@pkg-config --modversion SDL3_image   2>/dev/null && echo "  SDL3_image   OK" || echo "  SDL3_image   NOT FOUND"
	@pkg-config --modversion SDL3_ttf     2>/dev/null && echo "  SDL3_ttf     OK" || echo "  SDL3_ttf     NOT FOUND"
	@pkg-config --modversion libavformat  2>/dev/null && echo "  FFmpeg       OK" || echo "  FFmpeg       NOT FOUND"
	@echo ""
	@echo "── BASS 라이브러리 확인 ─────────────────────────────"
	@test -f bass/bass.h    && echo "  bass.h       OK" || echo "  bass.h       NOT FOUND (bass/ 폴더에 배치 필요)"
ifeq ($(PLATFORM),windows)
	@test -f bass/bass.lib  && echo "  bass.lib     OK" || echo "  bass.lib     NOT FOUND"
	@test -f bass/bass.dll  && echo "  bass.dll     OK" || echo "  bass.dll     NOT FOUND"
else ifeq ($(PLATFORM),macos)
	@test -f bass/libbass.dylib && echo "  libbass.dylib OK" || echo "  libbass.dylib NOT FOUND"
else
	@test -f bass/libbass.so    && echo "  libbass.so   OK" || echo "  libbass.so   NOT FOUND"
endif
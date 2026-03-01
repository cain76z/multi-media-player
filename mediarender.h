#pragma once

/**
 * @file mediarender.h
 * @brief MP Media Player – 렌더러 인터페이스 및 구현 계층 구조
 *
 *  계층 구조:
 *    IRenderer        : 순수 가상 인터페이스 (외부 호출용)
 *    BaseRenderer     : SDL 윈도우, 전체화면, 진행바 계산 등 공통 로직 구현
 *    MediaRenderer    : SDL3 기본 렌더러 (SDL_Renderer, TTF 자막)
 *    MediaGLRenderer  : OpenGL 렌더러 (glad 사용 - 여러가지 shader 적용)
 *    MediaGPURenderer : SDL3 GPU API 렌더러 (향후 구현 예정 - 뼈대만 구현)
 */

// ──────────────────────────────────────────────────────────────────
//  Third-party
// ──────────────────────────────────────────────────────────────────

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <glad/glad.h>
#include "mediaplayer.h"
#include "subtitle.h"


// ──────────────────────────────────────────────────────────────────
//  UI 상수
// ──────────────────────────────────────────────────────────────────

inline constexpr float BAR_H      =  8.0f;   ///< 진행바 높이
inline constexpr float BAR_MARGIN =  0.0f;   ///< 하단 여백
inline constexpr float HIT_MARGIN = 10.0f;   ///< 마우스 클릭 감지 여유

// ══════════════════════════════════════════════════════════════════
//  IRenderer – 순수 가상 인터페이스
// ══════════════════════════════════════════════════════════════════

/**
 * @class IRenderer
 * @brief 모든 렌더러가 구현해야 하는 순수 가상 인터페이스
 *
 *  외부 코드(mp.cpp 등)는 이 인터페이스만 의존하도록 설계합니다.
 *  렌더러 교체(SDL ↔ OpenGL ↔ GPU)가 인터페이스 변경 없이 가능합니다.
 */
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // ── 핵심 렌더링 ────────────────────────────────────────────────
    /**
     * @brief 현재 플레이어 상태를 화면에 렌더링
     * @param player      현재 활성 MediaPlayer (nullptr 허용)
     * @param filename    OSD에 표시할 파일명 (확장자 제외)
     * @param bar_dragging 마우스로 진행바 드래그 중이면 강조 표시
     */
    virtual void render(MediaPlayer* player,
                        const std::string& filename = {},
                        bool bar_dragging = false) = 0;

    // ── 창 제어 ────────────────────────────────────────────────────
    virtual void toggle_fullscreen()                  = 0;
    virtual void set_title(const std::string& title)  = 0;
    virtual bool is_fullscreen() const                = 0;

    // ── OSD 제어 ───────────────────────────────────────────────────
    virtual void toggle_osd()          = 0;  ///< O 키: OSD 켜고 끄기
    virtual bool is_osd_enabled() const = 0;

    // ── 진행바 유틸리티 ────────────────────────────────────────────
    /// @brief 주어진 Y 좌표가 진행바 영역에 속하는지 검사
    virtual bool  is_over_bar(float mouse_y)    const = 0;
    /// @brief 마우스 X 좌표를 진행률(0.0~1.0)로 변환
    virtual float x_to_progress(float mouse_x)  const = 0;

    // ── SDL 창 접근 ────────────────────────────────────────────────
    virtual SDL_Window* get_window() const = 0;
};


// ══════════════════════════════════════════════════════════════════
//  BaseRenderer – SDL 윈도우 & 공통 상태 관리
// ══════════════════════════════════════════════════════════════════

/**
 * @class BaseRenderer
 * @brief SDL 윈도우 생성/소멸, 전체화면 전환, 진행바 좌표 계산 등
 *        모든 구체 렌더러가 공유하는 공통 로직을 구현합니다.
 *
 *  파생 클래스에서 구현해야 할 순수 가상 함수:
 *    render()  – 실제 그리기 로직
 *
 *  파생 클래스에서 사용 가능한 protected 멤버:
 *    window_      – SDL_Window 포인터
 *    fullscreen_  – 현재 전체화면 상태
 *    osd_enabled_ – OSD 표시 여부
 */
class BaseRenderer : public IRenderer {
public:
    /**
     * @param title      창 제목
     * @param w,h        창 크기
     * @param x,y        창 위치
     * @param fullscreen 전체화면 여부
     */
    BaseRenderer(const std::string& title,
                 int w, int h, int x, int y,
                 bool fullscreen = false,
                const char * sdl_backend = nullptr);

    ~BaseRenderer() override;

    // IRenderer 구현 (비-렌더링 공통 메서드)
    void toggle_fullscreen()                 override;
    void set_title(const std::string& title) override;
    bool is_fullscreen() const               override { return fullscreen_; }

    void toggle_osd()           override { osd_enabled_ = !osd_enabled_; }
    bool is_osd_enabled() const override { return osd_enabled_; }

    bool  is_over_bar(float mouse_y)    const override;
    float x_to_progress(float mouse_x)  const override;

    SDL_Window* get_window() const override { return window_; }

    /// @brief 진행률(0.0~1.0)로 플레이어 탐색 수행 (공통 유틸)
    static void seek_to_progress(MediaPlayer* player, float progress);

protected:
    SDL_Window* window_     = nullptr;
    bool        fullscreen_ = false;
    bool        osd_enabled_= false;
};


// ══════════════════════════════════════════════════════════════════
//  MediaRenderer – SDL3 기본 렌더러 (SDL_Renderer + TTF 자막)
// ══════════════════════════════════════════════════════════════════

/**
 * @class MediaRenderer
 * @brief SDL_Renderer 기반의 렌더러. 자막, OSD, FFT 스펙트럼 시각화를 지원합니다.
 *
 *  render(player) 동작:
 *    get_texture() != nullptr → Letterbox 렌더링
 *    get_texture() == nullptr → FFT 스펙트럼 시각화
 *    get_length()  > 0        → 하단 진행바
 *    get_subtitle_text() != ""→ 자막 오버레이 (진행바 바로 위)
 *    osd_enabled_             → 좌상단 정보 오버레이
 */
class MediaRenderer : public BaseRenderer {
public:
    /**
     * @param title      창 제목
     * @param w,h        창 크기
     * @param x,y        창 위치
     * @param fullscreen 전체화면 여부
     * @param font_path  SDL_ttf 폰트 파일 경로 (비어 있으면 시스템 폰트 자동 탐색)
     * @param font_size  자막 폰트 크기 (pt)
     */
    MediaRenderer(const std::string& title,
                  int w, int h, int x, int y,
                  bool fullscreen = false,
                  const std::string& font_path = "",
                  int font_size = 28,
                  const char * sdl_backend = nullptr);

    ~MediaRenderer() override;

    // IRenderer::render() 구현
    void render(MediaPlayer* player,
                const std::string& filename = {},
                bool bar_dragging = false) override;

    SDL_Renderer* get_renderer() const { return renderer_; }

    std::string get_sdl_backend_name()
    {
        const char* name = SDL_GetRendererName(renderer_);
        if (name)
            return std::string(name);
        return "unknown";
    }

private:
    // ── 초기화 헬퍼 ─────────────────────────────────────────────
    void load_font(const std::string& font_path, int font_size);
    static std::string find_system_font();

    // ── 렌더링 헬퍼 ─────────────────────────────────────────────
    void render_centered_texture(SDL_Texture* tex) const;
    void render_progress_bar(float progress, bool highlighted) const;
    void render_fft(MediaPlayer* player) const;
    void render_subtitle(const std::string& text) const;
    void render_osd(MediaPlayer* player, const std::string& filename) const;

    // ── SDL 렌더러 ───────────────────────────────────────────────
    SDL_Renderer* renderer_  = nullptr;

    // ── 폰트 (자막 + OSD 공용) ───────────────────────────────────
    TTF_Font* font_      = nullptr;
    int       font_size_ = 28;

    // ── 자막 텍스처 캐시 ─────────────────────────────────────────
    mutable SDL_Texture* sub_texture_      = nullptr;
    mutable std::string  sub_text_cached_;  ///< 마지막으로 렌더링한 텍스트
    mutable int          sub_tex_w_        = 0;
    mutable int          sub_tex_h_        = 0;
    mutable int          sub_win_w_        = 0; ///< 창 크기 변경 감지용

    // ── OSD 텍스처 캐시 (1초 단위 갱신) ─────────────────────────
    mutable SDL_Texture* osd_texture_      = nullptr;
    mutable std::string  osd_text_cached_;
    mutable int          osd_tex_w_        = 0;
    mutable int          osd_tex_h_        = 0;
};


// ══════════════════════════════════════════════════════════════════
//  MediaGLRenderer – OpenGL 렌더러 (glad 사용)
// ══════════════════════════════════════════════════════════════════
// glad는 보통 헤더 하나만 포함하면 됩니다.
#include <glad/glad.h>

// OpenGL 수학 라이브러리 (선택)
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

class MediaGLRenderer : public BaseRenderer {
public:
    MediaGLRenderer(const std::string& title,
                    int w, int h, int x, int y,
                    bool fullscreen = false);
    ~MediaGLRenderer() override;

    void render(MediaPlayer* player,
                const std::string& filename = {},
                bool bar_dragging = false) override;

private:
    // OpenGL 관련 멤버
    SDL_GLContext gl_context_ = nullptr;

    // 셰이더 프로그램
    GLuint program_id_      = 0;   // 비디오 YUV→RGB
    GLuint program_ui_id_   = 0;   // UI (자막, OSD, 진행바)
    GLuint vs_id_ = 0, fs_id_ = 0, fs_ui_id_ = 0;

    // Vertex Array / Buffer Objects
    GLuint vao_id_  = 0, vbo_id_  = 0, ebo_id_  = 0;
    GLuint ui_vao_id_ = 0, ui_vbo_id_ = 0;   // (선택적 분리)

    // 텍스처 ID
    GLuint video_tex_id_[3] = {0,0,0};  // Y, U, V
    GLuint sub_tex_id_   = 0;
    GLuint osd_tex_id_   = 0;

    // 캐싱용
    std::string sub_text_cached_;
    std::string osd_text_cached_;
    int sub_win_w_ = 0, osd_win_w_ = 0;
    int sub_tex_w_ = 0, sub_tex_h_ = 0;
    int osd_tex_w_ = 0, osd_tex_h_ = 0;

    // 투영 행렬 (2D ortho)
    glm::mat4 projection_;

    // 내부 헬퍼 함수
    void compile_shaders();
    void check_program_link(GLuint prog);
    void setup_quad();
    void update_projection();

    // 텍스처 업데이트
    void update_video_texture(MediaPlayer* player);
    void update_subtitle_texture(const std::string& text, int win_w, int win_h);
    void update_osd_texture(MediaPlayer* player, const std::string& filename);

    // 렌더링 헬퍼
    void render_progress_bar(float progress, bool highlighted);
};

// ══════════════════════════════════════════════════════════════════
//  MediaGPURenderer – SDL3 GPU API 렌더러 (뼈대)
// ══════════════════════════════════════════════════════════════════

/**
 * @class MediaGPURenderer
 * @brief SDL3 GPU API(SDL_GPUDevice) 기반 렌더러. 향후 구현 예정입니다.
 * @todo  구현 예정
 */
class MediaGPURenderer : public BaseRenderer {
public:
    MediaGPURenderer(const std::string& title,
                     int w, int h, int x, int y,
                     bool fullscreen = false);
    ~MediaGPURenderer() override;

    void render(MediaPlayer* player,
                const std::string& filename = {},
                bool bar_dragging = false) override;
};
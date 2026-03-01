/**
 * @file mediarender.cpp
 * @brief MediaRenderer 구현 (자막 포함)
 *
 *  구현 계층:
 *    BaseRenderer     – SDL 윈도우 생성/소멸, 전체화면, 진행바 유틸리티
 *    MediaRenderer    – SDL_Renderer 렌더링, TTF 자막/OSD, FFT 시각화
 *    MediaGLRenderer  – 뼈대 (향후 구현)
 *    MediaGPURenderer – 뼈대 (향후 구현)
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mediarender.h"
#include "subtitle.h"

// ════════════════════════════════════════════════════════════════════
//  BaseRenderer
// ════════════════════════════════════════════════════════════════════

/**
 * @brief SDL 윈도우를 생성하고 공통 상태를 초기화합니다.
 */
BaseRenderer::BaseRenderer(const std::string& title,
                           int w, int h, int x, int y,
                           bool fullscreen, const char * sdl_backend)
    : fullscreen_(fullscreen)
{
    SDL_WindowFlags flag = SDL_WINDOW_RESIZABLE; 
    if (SDL_strncmp(sdl_backend, "opengl", 6) == 0)
        flag |= SDL_WINDOW_OPENGL;
    if (SDL_strcmp(sdl_backend, "vulkan") == 0)
        flag |= SDL_WINDOW_VULKAN;

    window_ = SDL_CreateWindow(title.c_str(), w, h, flag);
    if (!window_) throw std::runtime_error("SDL_CreateWindow 실패");

    SDL_SetWindowPosition(window_, x, y);
    if (fullscreen_) SDL_SetWindowFullscreen(window_, true);
}

BaseRenderer::~BaseRenderer() {
    if (window_) SDL_DestroyWindow(window_);
}

void BaseRenderer::toggle_fullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_);
}

void BaseRenderer::set_title(const std::string& title) {
    SDL_SetWindowTitle(window_, title.c_str());
}

/**
 * @brief 주어진 Y 좌표가 진행바 영역에 속하는지 검사
 */
bool BaseRenderer::is_over_bar(float mouse_y) const {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    const float top = static_cast<float>(h) - BAR_H - BAR_MARGIN;
    return mouse_y >= top - HIT_MARGIN && mouse_y <= top + BAR_H + HIT_MARGIN;
}

/**
 * @brief 마우스 X 좌표를 진행률(0.0~1.0)로 변환
 */
float BaseRenderer::x_to_progress(float mouse_x) const {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    return SDL_clamp(mouse_x / static_cast<float>(w), 0.0f, 1.0f);
}

/**
 * @brief 진행률(0.0~1.0)에 따라 플레이어 탐색 (정적 유틸)
 */
void BaseRenderer::seek_to_progress(MediaPlayer* player, float progress) {
    if (!player) return;
    const double len = player->get_length();
    if (len > 0.0) player->seek(len * static_cast<double>(progress));
}

// ════════════════════════════════════════════════════════════════════
//  MediaRenderer
// ════════════════════════════════════════════════════════════════════

/**
 * @brief SDL_Renderer 및 TTF 폰트를 초기화합니다.
 *        창 생성은 BaseRenderer 위임.
 */
MediaRenderer::MediaRenderer(const std::string& title,
                             int w, int h, int x, int y,
                             bool fullscreen,
                             const std::string& font_path,
                             int font_size,
                             const char * sdl_backend)
    : BaseRenderer(title, w, h, x, y, fullscreen, sdl_backend)
    , font_size_(font_size)
{
    renderer_ = SDL_CreateRenderer(window_, sdl_backend);
    if (!renderer_) {
        // BaseRenderer::~BaseRenderer() 가 window_ 를 정리함
        throw std::runtime_error("SDL_CreateRenderer 실패");
    }
    // When a renderer is created, vsync defaults to SDL_RENDERER_VSYNC_DISABLED.
    // The vsync parameter can be 1 to synchronize present with every vertical refresh, 
    // 2 to synchronize present with every second vertical refresh, 
    // etc., SDL_RENDERER_VSYNC_ADAPTIVE for late swap tearing (adaptive vsync), 
    // or SDL_RENDERER_VSYNC_DISABLED to disable. 
    // Not every value is supported by every driver, 
    // so you should check the return value to see whether the requested setting is supported.
    SDL_SetRenderVSync(renderer_, 1);

    // TTF_Init() - SDL_ttf 초기화
    // Returns:
    // true on success or false on failure; call SDL_GetError() for more information.
    if (TTF_Init()) {
        load_font(font_path, font_size);
    } else {
        std::cerr << "[자막] TTF_Init 실패: " << SDL_GetError() << "\n";
    }
}

MediaRenderer::~MediaRenderer() {
    if (sub_texture_) SDL_DestroyTexture(sub_texture_);
    if (osd_texture_) SDL_DestroyTexture(osd_texture_);
    if (font_)        TTF_CloseFont(font_);
    TTF_Quit();
    if (renderer_)    SDL_DestroyRenderer(renderer_);
    // window_ 소멸은 BaseRenderer::~BaseRenderer() 에서 처리
}

// ── 폰트 로드 ────────────────────────────────────────────────────

/**
 * @brief 플랫폼별 시스템 폰트 경로 탐색 (한글 폴백 포함)
 */
std::string MediaRenderer::find_system_font() {
    static const char* candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/malgun.ttf",
        "C:/Windows/Fonts/malgunbd.ttf",
        "C:/Windows/Fonts/gulim.ttc",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/NotoSans-Regular.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
#else   // Linux
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
        nullptr
    };

    for (int i = 0; candidates[i]; ++i) {
        if (std::filesystem::exists(candidates[i]))
            return candidates[i];
    }
    return {};
}

/**
 * @brief 폰트 로드 (지정 경로 → 시스템 폴백)
 */
void MediaRenderer::load_font(const std::string& font_path, int size) {
    auto try_open = [&](const std::string& path) -> bool {
        if (path.empty()) return false;
        font_ = TTF_OpenFont(path.c_str(), size);
        if (font_) {
            std::cout << "[자막] 폰트 로드: " << path << "\n";
            return true;
        }
        return false;
    };

    if (try_open(font_path)) return;
    if (!try_open(find_system_font()))
        std::cerr << "[자막] 폰트를 찾을 수 없습니다. 자막이 표시되지 않습니다.\n";
}

// ── 렌더링 헬퍼 ──────────────────────────────────────────────────

/**
 * @brief 텍스처를 Letterbox 방식으로 화면 중앙에 렌더링
 */
void MediaRenderer::render_centered_texture(SDL_Texture* tex) const {
    if (!tex) return;

    float tex_w, tex_h;
    SDL_GetTextureSize(tex, &tex_w, &tex_h);

    int win_w, win_h;
    SDL_GetCurrentRenderOutputSize(renderer_, &win_w, &win_h);

    const float scale  = std::min(static_cast<float>(win_w) / tex_w,
                                  static_cast<float>(win_h) / tex_h);
    const float draw_w = tex_w * scale;
    const float draw_h = tex_h * scale;

    SDL_FRect dst = { (win_w - draw_w) / 2.0f,
                      (win_h - draw_h) / 2.0f,
                      draw_w, draw_h };
    SDL_RenderTexture(renderer_, tex, nullptr, &dst);
}

/**
 * @brief 하단 진행바 렌더링
 * @param progress    0.0~1.0 진행률
 * @param highlighted 마우스 오버/드래그 시 강조
 */
void MediaRenderer::render_progress_bar(float progress, bool highlighted) const {
    progress = SDL_clamp(progress, 0.0f, 1.0f);

    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    const float bar_h = highlighted ? BAR_H + 2.0f : BAR_H;
    const float top_y = static_cast<float>(win_h) - BAR_H - BAR_MARGIN
                      - (highlighted ? 1.0f : 0.0f);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // 배경 트랙 (반투명 검정)
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 150);
    SDL_FRect track = { 0.0f, top_y, static_cast<float>(win_w), bar_h };
    SDL_RenderFillRect(renderer_, &track);

    // 채워진 부분 (밝은 회색)
    SDL_SetRenderDrawColor(renderer_, 210, 210, 210, 220);
    SDL_FRect filled = { 0.0f, top_y, static_cast<float>(win_w) * progress, bar_h };
    SDL_RenderFillRect(renderer_, &filled);

    // 핸들 (흰색 세로 막대)
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
    SDL_FRect handle = { static_cast<float>(win_w) * progress - 4.0f,
                         top_y - 2.0f, 8.0f, bar_h + 4.0f };
    SDL_RenderFillRect(renderer_, &handle);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

/**
 * @brief 오디오 FFT 스펙트럼 시각화 (64개 막대)
 */
void MediaRenderer::render_fft(MediaPlayer* player) const {
    if (!player) return;

    float fft[256] = {};
    if (!player->get_fft(fft, 256)) return;

    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    constexpr int BARS  = 64;
    const float   bar_w = static_cast<float>(win_w) / (BARS + 2);

    for (int i = 0; i < BARS; ++i) {
        float h = fft[i * 2] * (win_h * 0.7f);
        if (h > win_h) h = static_cast<float>(win_h);

        SDL_FRect rect = { bar_w * (i + 1),
                           static_cast<float>(win_h) - h,
                           bar_w * 0.8f, h };
        SDL_SetRenderDrawColor(renderer_,
                               0,
                               static_cast<Uint8>(std::min(180 + i * 2, 255)),
                               255, 255);
        SDL_RenderFillRect(renderer_, &rect);
    }
}

/**
 * @brief 자막 텍스트를 화면 하단 중앙에 렌더링 (캐싱 적용)
 *
 *  - 텍스처 캐싱: 텍스트 내용 또는 창 너비가 바뀔 때만 재생성
 *  - 멀티라인: '\n' 기준으로 분리 후 줄별 렌더링
 *  - 반투명 배경 박스: 가독성 향상
 *  - 진행바 위, 패딩 8px
 */
void MediaRenderer::render_subtitle(const std::string& text) const {
    if (!font_ || text.empty()) return;

    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    // 캐시 유효성 검사
    if (text != sub_text_cached_ || win_w != sub_win_w_) {
        if (sub_texture_) { SDL_DestroyTexture(sub_texture_); sub_texture_ = nullptr; }
        sub_text_cached_ = text;
        sub_win_w_       = win_w;

        // 줄 분리
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char c : text) {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else           cur += c;
            }
            if (!cur.empty()) lines.push_back(cur);
        }
        if (lines.empty()) return;

        // 각 줄을 서피스로 렌더
        SDL_Color white = {255, 255, 255, 255};
        std::vector<SDL_Surface*> surfs;
        int total_h = 0, max_w = 0;

        for (auto& line : lines) {
            const std::string render_line = line.empty() ? " " : line;
            SDL_Surface* s = TTF_RenderText_Blended(font_,
                                                    render_line.c_str(),
                                                    render_line.size(),
                                                    white);
            if (!s) continue;
            surfs.push_back(s);
            total_h += s->h + 2;
            if (s->w > max_w) max_w = s->w;
        }
        if (surfs.empty()) return;

        constexpr int PAD_X = 12, PAD_Y = 8;
        SDL_Surface* combined = SDL_CreateSurface(max_w  + PAD_X * 2,
                                                  total_h + PAD_Y * 2,
                                                  SDL_PIXELFORMAT_RGBA32);
        if (!combined) {
            for (auto* s : surfs) SDL_DestroySurface(s);
            return;
        }

        SDL_FillSurfaceRect(combined, nullptr,
            SDL_MapSurfaceRGBA(combined, 0, 0, 0, 160));

        int y_off = PAD_Y;
        for (auto* s : surfs) {
            SDL_Rect dst = { PAD_X + (max_w - s->w) / 2, y_off, s->w, s->h };
            SDL_BlitSurface(s, nullptr, combined, &dst);
            y_off += s->h + 2;
            SDL_DestroySurface(s);
        }

        sub_texture_ = SDL_CreateTextureFromSurface(renderer_, combined);
        SDL_DestroySurface(combined);
        if (!sub_texture_) return;

        float fw, fh;
        SDL_GetTextureSize(sub_texture_, &fw, &fh);
        sub_tex_w_ = static_cast<int>(fw);
        sub_tex_h_ = static_cast<int>(fh);
    }

    if (!sub_texture_) return;

    constexpr float BOTTOM_MARGIN = BAR_H + BAR_MARGIN + 8.0f;
    SDL_FRect dst = { (win_w - sub_tex_w_) / 2.0f,
                      win_h  - BOTTOM_MARGIN - sub_tex_h_,
                      static_cast<float>(sub_tex_w_),
                      static_cast<float>(sub_tex_h_) };

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderTexture(renderer_, sub_texture_, nullptr, &dst);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

/**
 * @brief OSD(On-Screen Display) – 좌상단에 재생 정보를 표시합니다.
 *
 *  표시 내용 (3줄):
 *    파일명 (확장자 제외, 최대 60자)
 *    시간: MM:SS / MM:SS
 *    볼륨: XX%   일시정지 시 [일시정지] 추가
 */
void MediaRenderer::render_osd(MediaPlayer* player,
                                const std::string& filename) const {
    if (!font_ || !player) return;

    std::string disp_name = filename;
    if (disp_name.size() > 60) { disp_name.resize(57); disp_name += "..."; }

    const double pos = player->get_position();
    const double len = player->get_length();
    const int    vol = static_cast<int>(player->get_volume() * 100.0f + 0.5f);

    std::string time_str = util::sec2str(pos, "%M:%S")
                         + " / "
                         + (len > 0.0 ? util::sec2str(len, "%M:%S") : "--:--");

    std::string vol_str = "볼륨: " + std::to_string(vol) + "%";
    if (player->is_paused()) vol_str += "  [일시정지]";

    const std::string osd_text = disp_name + '\n' + time_str + '\n' + vol_str;

    if (osd_text != osd_text_cached_) {
        if (osd_texture_) { SDL_DestroyTexture(osd_texture_); osd_texture_ = nullptr; }
        osd_text_cached_ = osd_text;

        std::vector<std::string> lines = { disp_name, time_str, vol_str };
        SDL_Color fg = {220, 220, 220, 255};
        std::vector<SDL_Surface*> surfs;
        int total_h = 0, max_w = 0;

        for (auto& line : lines) {
            SDL_Surface* s = TTF_RenderText_Blended(font_,
                                                    line.c_str(),
                                                    line.size(), fg);
            if (!s) continue;
            surfs.push_back(s);
            total_h += s->h + 2;
            if (s->w > max_w) max_w = s->w;
        }
        if (surfs.empty()) return;

        constexpr int PAD_X = 10, PAD_Y = 8;
        SDL_Surface* combined = SDL_CreateSurface(max_w  + PAD_X * 2,
                                                  total_h + PAD_Y * 2,
                                                  SDL_PIXELFORMAT_RGBA32);
        if (!combined) {
            for (auto* s : surfs) SDL_DestroySurface(s);
            return;
        }

        SDL_FillSurfaceRect(combined, nullptr,
            SDL_MapSurfaceRGBA(combined, 0, 0, 0, 180));

        int y_off = PAD_Y;
        for (auto* s : surfs) {
            SDL_Rect dst = { PAD_X, y_off, s->w, s->h };
            SDL_BlitSurface(s, nullptr, combined, &dst);
            y_off += s->h + 2;
            SDL_DestroySurface(s);
        }

        osd_texture_ = SDL_CreateTextureFromSurface(renderer_, combined);
        SDL_DestroySurface(combined);
        if (!osd_texture_) return;

        float fw, fh;
        SDL_GetTextureSize(osd_texture_, &fw, &fh);
        osd_tex_w_ = static_cast<int>(fw);
        osd_tex_h_ = static_cast<int>(fh);
    }

    if (!osd_texture_) return;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_FRect dst = { 8.0f, 8.0f,
                      static_cast<float>(osd_tex_w_),
                      static_cast<float>(osd_tex_h_) };
    SDL_RenderTexture(renderer_, osd_texture_, nullptr, &dst);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

// ── 공개 render() ────────────────────────────────────────────────

/**
 * @brief 최종 렌더링: 배경 클리어 → 미디어 → 자막 → 진행바 → OSD
 */
void MediaRenderer::render(MediaPlayer* player,
                            const std::string& filename,
                            bool bar_dragging) {
    SDL_SetRenderDrawColor(renderer_, 10, 10, 20, 255);
    SDL_RenderClear(renderer_);

    if (!player) {
        SDL_RenderPresent(renderer_);
        return;
    }

    SDL_Texture* tex      = player->get_texture();
    const double len      = player->get_length();
    const float  progress = player->get_progress();

    if (tex) render_centered_texture(tex);
    else     render_fft(player);

    const std::string sub = player->get_subtitle_text();
    if (!sub.empty()) render_subtitle(sub);

    if (len > 0.0) render_progress_bar(progress, bar_dragging);

    if (osd_enabled_) render_osd(player, filename);

    SDL_RenderPresent(renderer_);
}

// ════════════════════════════════════════════════════════════════════
//  MediaGLRenderer – OpenGL 렌더러 (glad + 셰이더)
// ════════════════════════════════════════════════════════════════════


MediaGLRenderer::MediaGLRenderer(const std::string& title,
                                 int w, int h, int x, int y,
                                 bool fullscreen)
    : BaseRenderer(title, w, h, x, y, fullscreen)
{
    // 1. OpenGL 속성 설정
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    // 2. OpenGL 컨텍스트 생성
    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
        throw std::runtime_error("SDL_GL_CreateContext 실패: " + std::string(SDL_GetError()));
    }

    // 3. glad 초기화 (함수 포인터 로딩)
    // if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
    if (!gladLoadGL()) {
        SDL_GL_DestroyContext(gl_context_);
        throw std::runtime_error("gladLoadGL 실패");
    }

    // 4. 기본 셰이더 컴파일
    compile_shaders();

    // 5. 전체 화면을 덮는 단일 사각형(Quad) VAO/VBO 설정
    setup_quad();

    // 6. 자막/OSD 렌더링을 위한 2D 투영 행렬 준비
    update_projection();

    // 7. OpenGL 상태 초기 설정
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.05f, 0.05f, 0.1f, 1.0f);
}

MediaGLRenderer::~MediaGLRenderer() {
    // 텍스처 삭제
    if (video_tex_id_[0]) glDeleteTextures(1, &video_tex_id_[0]);
    if (video_tex_id_[1]) glDeleteTextures(1, &video_tex_id_[1]);
    if (video_tex_id_[2]) glDeleteTextures(1, &video_tex_id_[2]);
    if (sub_tex_id_)   glDeleteTextures(1, &sub_tex_id_);
    if (osd_tex_id_)   glDeleteTextures(1, &osd_tex_id_);

    // 셰이더 프로그램 삭제
    if (program_id_)         glDeleteProgram(program_id_);
    if (program_ui_id_)      glDeleteProgram(program_ui_id_);
    if (vs_id_)              glDeleteShader(vs_id_);
    if (fs_id_)              glDeleteShader(fs_id_);
    if (fs_ui_id_)           glDeleteShader(fs_ui_id_);

    // VAO, VBO 삭제
    if (vao_id_) {
        glDeleteVertexArrays(1, &vao_id_);
        glDeleteBuffers(1, &vbo_id_);
    }
    if (ui_vao_id_) {
        glDeleteVertexArrays(1, &ui_vao_id_);
        glDeleteBuffers(1, &ui_vbo_id_);
    }

    // ✅ SDL3: Destroy OpenGL context
    if (gl_context_) SDL_GL_DestroyContext(gl_context_);
}

// ── 셰이더 컴파일 헬퍼 ──────────────────────────────────────────────
static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "셰이더 컴파일 오류: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

void MediaGLRenderer::compile_shaders() {
    // --- 버텍스 셰이더 (비디오/UI 공통) ---
    const char* vs_src = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 TexCoord;
        uniform mat4 projection;
        uniform mat4 model;
        void main() {
            gl_Position = projection * model * vec4(aPos, 0.0, 1.0);
            TexCoord = aTexCoord;
        }
    )";

    // --- 프래그먼트 셰이더 (비디오) - 간단한 YUV→RGB 변환 (YUV420P 가정) ---
    const char* fs_video_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D y_tex;
        uniform sampler2D u_tex;
        uniform sampler2D v_tex;
        void main() {
            float y = texture(y_tex, TexCoord).r;
            float u = texture(u_tex, TexCoord).r - 0.5;
            float v = texture(v_tex, TexCoord).r - 0.5;
            float r = y + 1.402 * v;
            float g = y - 0.344 * u - 0.714 * v;
            float b = y + 1.772 * u;
            FragColor = vec4(r, g, b, 1.0);
        }
    )";

    // --- 프래그먼트 셰이더 (UI) - 단순 텍스처 알파 블렌딩 ---
    const char* fs_ui_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D ui_tex;
        void main() {
            FragColor = texture(ui_tex, TexCoord);
        }
    )";

    vs_id_ = compile_shader(GL_VERTEX_SHADER, vs_src);
    fs_id_ = compile_shader(GL_FRAGMENT_SHADER, fs_video_src);
    fs_ui_id_ = compile_shader(GL_FRAGMENT_SHADER, fs_ui_src);

    // 비디오 프로그램
    program_id_ = glCreateProgram();
    glAttachShader(program_id_, vs_id_);
    glAttachShader(program_id_, fs_id_);
    glLinkProgram(program_id_);
    check_program_link(program_id_);

    // UI 프로그램
    program_ui_id_ = glCreateProgram();
    glAttachShader(program_ui_id_, vs_id_);
    glAttachShader(program_ui_id_, fs_ui_id_);
    glLinkProgram(program_ui_id_);
    check_program_link(program_ui_id_);
}

void MediaGLRenderer::check_program_link(GLuint prog) {
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "프로그램 링크 오류: " << log << std::endl;
    }
}

// ── 정점 데이터 설정 ─────────────────────────────────────────────────
void MediaGLRenderer::setup_quad() {
    // 화면을 채우는 사각형 (클립 좌표 -1..1 로 바로 매핑할 수도 있지만,
    // 여기서는 투영 행렬과 함께 사용하기 위해 0..1 범위로 정의)
    float vertices[] = {
        // 위치       // 텍스처 좌표
        0.0f, 0.0f,   0.0f, 0.0f,  // 좌하
        1.0f, 0.0f,   1.0f, 0.0f,  // 우하
        1.0f, 1.0f,   1.0f, 1.0f,  // 우상
        0.0f, 1.0f,   0.0f, 1.0f   // 좌상
    };
    unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };

    glGenVertexArrays(1, &vao_id_);
    glGenBuffers(1, &vbo_id_);
    glGenBuffers(1, &ebo_id_);

    glBindVertexArray(vao_id_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_id_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_id_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // 위치 속성
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // 텍스처 좌표 속성
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    // UI 렌더링용 VAO (동일한 정점 구조 사용 가능)
    ui_vao_id_ = vao_id_; // 간단히 공유
    ui_vbo_id_ = vbo_id_;
}

// ── 투영 행렬 업데이트 (창 크기 변경 시 호출) ───────────────────────
void MediaGLRenderer::update_projection() {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    projection_ = glm::ortho(0.0f, (float)w, (float)h, 0.0f, -1.0f, 1.0f);
}

// ── 비디오 텍스처 생성/갱신 ─────────────────────────────────────────
void MediaGLRenderer::update_video_texture(MediaPlayer* player) {
    if (!player) return;
    /*
    // 가상: 플레이어로부터 프레임 크기와 데이터 얻기
    int frame_w, frame_h;
    if (!player->get_video_frame_size(frame_w, frame_h)) return;
    const uint8_t* data = player->get_video_frame_data();
    if (!data) return;

    // YUV420P 가정: data[0] = Y, data[1] = U, data[2] = V
    if (!video_tex_id_) {
        glGenTextures(3, video_tex_id_);  // 세 개의 텍스처 (Y, U, V)
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, video_tex_id_[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame_w, frame_h, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, video_tex_id_[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame_w/2, frame_h/2, 0, GL_RED, GL_UNSIGNED_BYTE, data + frame_w*frame_h);
    glTexParameteri(...);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, video_tex_id_[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame_w/2, frame_h/2, 0, GL_RED, GL_UNSIGNED_BYTE, data + frame_w*frame_h*5/4);
    glTexParameteri(...);
    */
}

// ── 자막 텍스처 생성 (MediaRenderer와 유사, OpenGL 텍스처로) ────────
void MediaGLRenderer::update_subtitle_texture(const std::string& text, int win_w, int win_h) {
    // ... TTF_Surface 생성 후 OpenGL 텍스처로 업로드 ...
    // (MediaRenderer의 로직을 OpenGL 방식으로 변환)
    // 생략 (비슷한 패턴)
}

// ── OSD 텍스처 생성 ────────────────────────────────────────────────
void MediaGLRenderer::update_osd_texture(MediaPlayer* player, const std::string& filename) {
    // ... 유사 ...
}

// ── 진행바 렌더링 (OpenGL 원시 라인/사각형) ────────────────────────
void MediaGLRenderer::render_progress_bar(float progress, bool highlighted) {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);

    const float bar_h = highlighted ? BAR_H + 2.0f : BAR_H;
    const float top_y = h - BAR_H - BAR_MARGIN - (highlighted ? 1.0f : 0.0f);

    // 간단한 방법: glRecti 를 사용하거나, UI VAO로 채워진 사각형 그리기
    // 여기서는 UI 프로그램(program_ui_id_)을 사용하여 단색 사각형을 텍스처 없이 그리는 것으로 가정
    // 실제로는 별도의 단색 렌더링 경로가 필요하지만, 예제 단순화를 위해 생략.
}

// ── 주 render() ────────────────────────────────────────────────────
void MediaGLRenderer::render(MediaPlayer* player,
                              const std::string& filename,
                              bool bar_dragging) {
    // 창 크기 변경 시 투영 행렬 갱신
    static int prev_w = 0, prev_h = 0;
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    if (w != prev_w || h != prev_h) {
        update_projection();
        prev_w = w; prev_h = h;
    }

    glClear(GL_COLOR_BUFFER_BIT);

    if (!player) {
        SDL_GL_SwapWindow(window_);
        return;
    }
    /*
    // 비디오 텍스처 갱신 및 렌더링
    update_video_texture(player);
    if (video_tex_id_) {
        glUseProgram(program_id_);

        // uniform 설정
        GLint proj_loc = glGetUniformLocation(program_id_, "projection");
        GLint model_loc = glGetUniformLocation(program_id_, "model");
        glUniformMatrix4fv(proj_loc, 1, GL_FALSE, glm::value_ptr(projection_));

        // Letterbox 모델 행렬 계산
        int video_w, video_h;
        player->get_video_frame_size(video_w, video_h);
        float win_aspect = (float)w / h;
        float vid_aspect = (float)video_w / video_h;
        float scale_x = 1.0f, scale_y = 1.0f;
        float trans_x = 0.0f, trans_y = 0.0f;
        if (win_aspect > vid_aspect) {
            scale_x = vid_aspect / win_aspect;
            trans_x = (1.0f - scale_x) * 0.5f;
        } else {
            scale_y = win_aspect / vid_aspect;
            trans_y = (1.0f - scale_y) * 0.5f;
        }
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(trans_x * w, trans_y * h, 0.0f))
                        * glm::scale(glm::mat4(1.0f), glm::vec3(scale_x * w, scale_y * h, 1.0f));
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, glm::value_ptr(model));

        // 텍스처 유닛 연결
        glUniform1i(glGetUniformLocation(program_id_, "y_tex"), 0);
        glUniform1i(glGetUniformLocation(program_id_, "u_tex"), 1);
        glUniform1i(glGetUniformLocation(program_id_, "v_tex"), 2);

        glBindVertexArray(vao_id_);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

    // 자막 렌더링 (별도 UI 패스)
    std::string sub = player->get_subtitle_text();
    if (!sub.empty()) {
        update_subtitle_texture(sub, w, h); // 내부적으로 텍스처 갱신
        // UI VAO로 텍스처 렌더링
    }

    // 진행바
    double len = player->get_length();
    if (len > 0.0) {
        float progress = player->get_progress();
        render_progress_bar(progress, bar_dragging);
    }

    // OSD
    if (osd_enabled_) {
        // OSD 텍스처 렌더링
    }

    SDL_GL_SwapWindow(window_);
    */
}


// ════════════════════════════════════════════════════════════════════
//  MediaGPURenderer – 뼈대 (향후 구현)
// ════════════════════════════════════════════════════════════════════

MediaGPURenderer::MediaGPURenderer(const std::string& title,
                                   int w, int h, int x, int y,
                                   bool fullscreen)
    : BaseRenderer(title, w, h, x, y, fullscreen)
{
    // TODO: SDL_CreateGPUDevice, SDL_ClaimWindowForGPUDevice
}

MediaGPURenderer::~MediaGPURenderer() {
    // TODO: SDL_ReleaseWindowFromGPUDevice, SDL_DestroyGPUDevice
}

void MediaGPURenderer::render(MediaPlayer* /*player*/,
                               const std::string& /*filename*/,
                               bool /*bar_dragging*/) {
    // TODO: SDL_AcquireGPUSwapchainTexture → 커맨드 버퍼 → SDL_SubmitGPUCommandBuffer
}
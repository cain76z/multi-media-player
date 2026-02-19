#pragma once

/**
 * @file media.h
 * @brief MP Media Player – 미디어 플레이어 클래스 계층 구조 (자막 지원)
 *
 *  자막 흐름:
 *    VideoPlayer : 생성자에서 외부 .srt/.ass 탐색, 없으면 FFmpeg 내장 스트림 디코딩
 *    AudioPlayer : 생성자에서 외부 .srt/.ass 탐색
 *    MediaRenderer::render() 내에서 get_subtitle_text() 호출 → render_subtitle()
 *
 *  렌더링:
 *    SDL3_ttf 사용. 폰트 경로는 AppConfig::subtitle_font 또는 시스템 폴백.
 *    자막 텍스처는 텍스트 변경 시에만 재생성(캐싱).
 */

// ──────────────────────────────────────────────────────────────────
//  Third-party
// ──────────────────────────────────────────────────────────────────

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

#include "bass3.hpp"
#include "subtitle.h"
#include "util.hpp"

// ──────────────────────────────────────────────────────────────────
//  Standard
// ──────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ──────────────────────────────────────────────────────────────────
//  UI 상수
// ──────────────────────────────────────────────────────────────────

inline constexpr float BAR_H      =  8.0f;
inline constexpr float BAR_MARGIN =  0.0f;
inline constexpr float HIT_MARGIN = 10.0f;

// ──────────────────────────────────────────────────────────────────
//  AppConfig
// ──────────────────────────────────────────────────────────────────

struct AppConfig {
    int   win_x = 100, win_y = 100;
    int   win_w = 1280, win_h = 720;
    bool  fullscreen      = false;
    float volume          = 1.0f;
    float delay_after     = 2.5f;
    float image_display   = 5.0f;
    float short_threshold = 15.0f;

    // 자막 설정
    std::string subtitle_font;      ///< 폰트 파일 경로 (비어 있으면 자동 탐색)
    int         subtitle_size = 28; ///< 폰트 크기 (pt)

    std::unordered_set<std::wstring> image_exts;
    std::unordered_set<std::wstring> audio_exts;
    std::unordered_set<std::wstring> video_exts;
};

// ──────────────────────────────────────────────────────────────────
//  MediaPlayer – 순수 가상 기반 클래스
// ──────────────────────────────────────────────────────────────────

class MediaPlayer {
public:
    virtual ~MediaPlayer() = default;

    virtual void play()             = 0;
    virtual void stop()             = 0;
    virtual bool update()           = 0;  ///< false = 재생 종료
    virtual void toggle_pause()     = 0;
    virtual void seek(double secs)  = 0;
    virtual void set_volume(float v)= 0;

    virtual double get_position()  const = 0;
    virtual double get_length()    const = 0;
    virtual float  get_volume()    const = 0;
    virtual bool   is_playing()    const = 0;
    virtual bool   is_paused()     const = 0;
    virtual bool   is_ended()      const = 0;

    /// 비디오/이미지 텍스처. nullptr → FFT 시각화.
    virtual SDL_Texture* get_texture()  const { return nullptr; }

    /// 오디오 FFT 스펙트럼 (AudioPlayer 전용).
    virtual bool get_fft(float* buf, int n) const { (void)buf; (void)n; return false; }

    /// 현재 재생 위치의 자막 텍스트 (없으면 빈 문자열).
    virtual std::string get_subtitle_text() const { return {}; }

    float get_progress() const {
        double len = get_length();
        return (len > 0.0) ? static_cast<float>(get_position() / len) : 0.0f;
    }
};

// ──────────────────────────────────────────────────────────────────
//  VideoPlayer
// ──────────────────────────────────────────────────────────────────

/**
 * @class VideoPlayer
 *
 *  자막:
 *    - 생성자에서 외부 .srt/.ass 탐색 (있으면 subtitle_track_ 미리 로드)
 *    - 없으면 decode_loop에서 FFmpeg 내장 subtitle_stream_idx_ 디코딩
 *    - get_subtitle_text() : subtitle_mutex_ 로 보호된 접근
 */
class VideoPlayer : public MediaPlayer {
public:
    explicit VideoPlayer(const char* filename, SDL_Renderer* renderer);
    ~VideoPlayer() override { stop(); cleanup(); }

    void play()  override;
    void stop()  override;
    bool update()override;

    void   toggle_pause()      override { paused_ = !paused_; }
    void   seek(double secs)   override { seek_target_ = std::max(0.0, secs); ended_ = false; }
    void   set_volume(float v) override;
    double get_position() const override { return cur_pts_.load(); }
    double get_length()   const override;
    float  get_volume()   const override { return volume_.load(); }
    bool   is_playing()   const override { return !paused_.load() && !ended_.load(); }
    bool   is_paused()    const override { return paused_.load(); }
    bool   is_ended()     const override { return ended_.load();  }

    SDL_Texture* get_texture()      const override { return texture_; }
    std::string  get_subtitle_text()const override;

    bool is_valid() const { return format_ctx_ && video_ctx_ && texture_; }

private:
    void decode_loop();
    void cleanup();

    // FFmpeg 자원
    AVFormatContext* format_ctx_          = nullptr;
    AVCodecContext*  video_ctx_           = nullptr;
    AVCodecContext*  audio_ctx_           = nullptr;
    AVCodecContext*  subtitle_ctx_        = nullptr; ///< 내장 자막 코덱
    AVStream*        video_stream_        = nullptr;
    SwsContext*      sws_ctx_             = nullptr;
    AVFrame*         rgb_frame_           = nullptr;
    uint8_t*         rgb_buffer_          = nullptr;
    int              video_stream_idx_    = -1;
    int              audio_stream_idx_    = -1;
    int              subtitle_stream_idx_ = -1;      ///< FFmpeg 내장 자막 스트림

    // SDL 자원
    SDL_Renderer*    renderer_             = nullptr;
    SDL_Texture*     texture_             = nullptr;
    SDL_AudioStream* audio_stream_device_ = nullptr;

    // 스레드 동기화
    std::thread       decode_thread_;
    std::mutex        frame_mutex_;
    std::atomic<bool> frame_ready_{false};

    // 자막
    SubtitleTrack       subtitle_track_;
    mutable std::mutex  subtitle_mutex_;       ///< subtitle_track_ 보호
    bool                use_embedded_sub_ = false; ///< FFmpeg 내장 자막 사용 여부

    // 재생 상태
    std::atomic<bool>   running_     {false};
    std::atomic<bool>   paused_      {false};
    std::atomic<bool>   ended_       {false};
    std::atomic<double> seek_target_ {-1.0};
    std::atomic<double> cur_pts_     {0.0};
    std::atomic<float>  volume_      {1.0f};
    double              raw_duration_{0.0};
};

// ──────────────────────────────────────────────────────────────────
//  ImagePlayer
// ──────────────────────────────────────────────────────────────────

class ImagePlayer : public MediaPlayer {
public:
    ImagePlayer(const std::string& filepath, SDL_Renderer* renderer,
                float display_sec = 5.0f);
    ~ImagePlayer() override { stop(); cleanup(); }

    void play()  override;
    void stop()  override { ended_ = true; }
    bool update()override;

    void   toggle_pause()      override { paused_ = !paused_; }
    void   seek(double secs)   override;
    void   set_volume(float)   override {}
    double get_position() const override;
    double get_length()   const override { return static_cast<double>(display_sec_); }
    float  get_volume()   const override { return 0.0f; }
    bool   is_playing()   const override { return !paused_ && !ended_; }
    bool   is_paused()    const override { return paused_; }
    bool   is_ended()     const override { return ended_;  }

    SDL_Texture* get_texture() const override;

    bool is_valid()    const { return image_texture_ || !anim_frames_.empty(); }
    bool is_animated() const { return is_animated_; }
    int  frame_count() const { return static_cast<int>(anim_frames_.size()); }
    int  frame_index() const { return anim_frame_idx_; }

    void seek_frames(int delta); ///< 프레임 단위 상대 이동 (음수 가능)

private:
    void cleanup();

    SDL_Renderer*             renderer_       = nullptr;
    SDL_Texture*              image_texture_  = nullptr;
    IMG_Animation*            current_anim_   = nullptr;
    std::vector<SDL_Texture*> anim_frames_;

    bool  is_animated_     = false;
    bool  paused_          = false;
    bool  ended_           = false;
    int   anim_frame_idx_  = 0;
    int   anim_elapsed_ms_ = 0;
    int   anim_total_ms_   = 0;
    float display_sec_     = 5.0f;

    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_frame_time_;
};

// ──────────────────────────────────────────────────────────────────
//  AudioPlayer
// ──────────────────────────────────────────────────────────────────

/**
 * @class AudioPlayer
 *
 *  자막:
 *    - 생성자에서 외부 .srt/.ass 탐색
 *    - get_subtitle_text() : 현재 재생 위치 기준 자막 반환
 */
class AudioPlayer : public MediaPlayer {
public:
    AudioPlayer(const std::wstring& filepath, float volume = 1.0f);
    ~AudioPlayer() override { stop(); }

    void play()  override { song_.play(); }
    void stop()  override { song_.stop(); }
    bool update()override;

    void   toggle_pause()      override;
    void   seek(double secs)   override { song_.seek(secs); }
    void   set_volume(float v) override { song_.set_volume(v); }
    double get_position() const override { return song_.get_position(); }
    double get_length()   const override { return song_.get_length();   }
    float  get_volume()   const override { return song_.get_volume();   }
    bool   is_playing()   const override { return song_.is_playing();   }
    bool   is_paused()    const override { return song_.is_paused();    }
    bool   is_ended()     const override { return ended_.load();        }

    bool get_fft(float* buf, int /*n*/) const override {
        return const_cast<bass::Song&>(song_).get_fft(buf, BASS_DATA_FFT512);
    }
    std::string get_subtitle_text() const override {
        return subtitle_track_.get_active(song_.get_position());
    }

    bool is_valid()  const { return song_.is_valid(); }
    void restart()         { song_.play(true); ended_ = false; was_playing_ = false; }

private:
    bass::Song        song_;
    SubtitleTrack     subtitle_track_;
    std::atomic<bool> ended_      {false};
    bool              was_playing_{false};
};

// ──────────────────────────────────────────────────────────────────
//  MediaRenderer
// ──────────────────────────────────────────────────────────────────

/**
 * @class MediaRenderer
 *
 *  render(player):
 *    get_texture() != nullptr → Letterbox 렌더링
 *    get_texture() == nullptr → FFT 스펙트럼 시각화
 *    get_length()  > 0        → 하단 진행바
 *    get_subtitle_text() != ""→ 자막 오버레이 (진행바 바로 위)
 */
class MediaRenderer {
public:
    /**
     * @param font_path SDL_ttf 폰트 파일 경로 (비어 있으면 시스템 폰트 자동 탐색)
     * @param font_size 자막 폰트 크기 (pt)
     */
    MediaRenderer(const std::string& title,
                  int w, int h, int x, int y,
                  bool fullscreen = false,
                  const std::string& font_path = "",
                  int font_size = 28);
    ~MediaRenderer();

    void render(MediaPlayer* player, bool bar_dragging = false);

    void toggle_fullscreen();
    void set_title(const std::string& title);

    bool  is_over_bar(float mouse_y)    const;
    float x_to_progress(float mouse_x)  const;
    static void seek_to_progress(MediaPlayer* player, float progress);

    SDL_Window*   get_window()    const { return window_;    }
    SDL_Renderer* get_renderer()  const { return renderer_;  }
    bool          is_fullscreen() const { return fullscreen_; }

private:
    void load_font(const std::string& font_path, int font_size);
    static std::string find_system_font(); ///< 플랫폼별 폴백 폰트 탐색

    void render_centered_texture(SDL_Texture* tex) const;
    void render_progress_bar(float progress, bool highlighted) const;
    void render_fft(MediaPlayer* player) const;
    void render_subtitle(const std::string& text) const;

    SDL_Window*   window_     = nullptr;
    SDL_Renderer* renderer_   = nullptr;
    bool          fullscreen_ = false;

    // 자막 렌더링
    TTF_Font* font_         = nullptr;
    int       font_size_    = 28;

    // 자막 텍스처 캐싱 (텍스트/창 너비가 바뀔 때만 재생성)
    mutable SDL_Texture* sub_texture_  = nullptr;
    mutable std::string  sub_text_cached_;
    mutable int          sub_tex_w_    = 0;
    mutable int          sub_tex_h_    = 0;
    mutable int          sub_win_w_    = 0; ///< 텍스처 생성 시점의 창 너비 (wrap 기준)
};
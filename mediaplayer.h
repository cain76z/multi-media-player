#pragma once

/**
 * @file mediaplayer.h
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

#ifndef NOMINMAX
#define NOMINMAX
#endif

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
//  AppConfig
// ──────────────────────────────────────────────────────────────────

/**
 * @struct AppConfig
 * @brief 애플리케이션 설정 (mp.conf + 명령줄 인자 병합)
 */
struct AppConfig {
    int   win_x = 100, win_y = 100;          ///< 창 위치
    int   win_w = 1280, win_h = 720;          ///< 창 크기
    bool  fullscreen      = false;            ///< 전체화면 여부
    float volume          = 1.0f;             ///< 초기 볼륨 (0.0~1.0)
    float delay_after     = 2.5f;             ///< 오디오 종료 후 다음 트랙까지 대기 시간(초)
    float image_display   = 5.0f;             ///< 정적 이미지 표시 시간(초)
    float short_threshold = 15.0f;            ///< 이 길이(초) 미만 오디오는 반복 재생

    // 자막 설정
    std::string subtitle_font;                 ///< 폰트 파일 경로 (비어 있으면 자동 탐색)
    int         subtitle_size = 28;            ///< 폰트 크기 (pt)

    std::unordered_set<std::wstring> image_exts; ///< 이미지 확장자 목록
    std::unordered_set<std::wstring> audio_exts; ///< 오디오 확장자 목록
    std::unordered_set<std::wstring> video_exts; ///< 비디오 확장자 목록
};

// ──────────────────────────────────────────────────────────────────
//  MediaPlayer – 순수 가상 기반 클래스
// ──────────────────────────────────────────────────────────────────

/**
 * @class MediaPlayer
 * @brief 모든 미디어 플레이어의 추상 베이스 클래스
 *
 *  VideoPlayer, AudioPlayer, ImagePlayer가 이 인터페이스를 구현한다.
 *  주요 상태(재생/일시정지/종료)와 위치, 볼륨 등을 쿼리하고 제어한다.
 */
class MediaPlayer {
public:
    virtual ~MediaPlayer() = default;

    /// 재생 시작 (스레드 시작 등)
    virtual void play()             = 0;
    /// 재생 중지 및 리소스 정리
    virtual void stop()             = 0;
    /**
     * @brief 매 프레임 호출되어 내부 상태 갱신
     * @return false를 반환하면 재생이 종료되었음을 의미 (자동 전환 트리거)
     */
    // virtual bool update()           = 0;
    // ✅ 수정: "화면이 갱신되었으면 true" (dirty flag)
    // - VideoPlayer: frame_ready_ 가 true였을 때만 true 반환
    // - ImagePlayer: 애니메이션 프레임이 넘어갔을 때만 true 반환
    //               정적 이미지는 항상 false (텍스처가 변하지 않음)
    // - AudioPlayer: 항상 false (텍스처 없음, FFT는 별도 처리)
    virtual bool update() = 0; // 반환 의미만 변경
    virtual void toggle_pause()     = 0;
    virtual void seek(double secs)  = 0;
    virtual void set_volume(float v)= 0;

    virtual double get_position()  const = 0;
    virtual double get_length()    const = 0;
    virtual float  get_volume()    const = 0;
    virtual bool   is_playing()    const = 0;
    virtual bool   is_paused()     const = 0;
    virtual bool   is_ended()      const = 0;

    /**
     * @brief 현재 프레임의 SDL_Texture 반환 (비디오/이미지)
     * @return 텍스처 포인터, nullptr이면 오디오 전용 또는 FFT 시각화
     */
    virtual SDL_Texture* get_texture()  const { return nullptr; }

    /**
     * @brief 오디오 FFT 스펙트럼 데이터 (AudioPlayer 전용)
     * @param buf 출력 버퍼
     * @param n   버퍼 크기 (샘플 수)
     * @return 데이터 획득 성공 여부
     */
    virtual bool get_fft(float* buf, int n) const { (void)buf; (void)n; return false; }

    /**
     * @brief 현재 재생 시간에 해당하는 자막 텍스트 반환
     * @return UTF-8 자막 문자열 (없으면 빈 문자열)
     */
    virtual std::string get_subtitle_text() const { return {}; }

    /**
     * @brief 0.0 ~ 1.0 사이의 재생 진행률
     */
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
 * @brief FFmpeg 기반 비디오/오디오 재생, 내장/외부 자막 지원
 *
 *  자막:
 *    - 생성자에서 외부 .srt/.ass 탐색 (있으면 subtitle_track_ 미리 로드)
 *    - 없으면 decode_loop에서 FFmpeg 내장 subtitle_stream_idx_ 디코딩
 *    - get_subtitle_text() : subtitle_mutex_ 로 보호된 접근
 */
class VideoPlayer : public MediaPlayer {
public:
    /**
     * @param filename 비디오 파일 경로 (UTF-8)
     * @param renderer SDL_Renderer (텍스처 생성용)
     */
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

    /// @brief 플레이어가 정상 초기화되었는지 확인
    bool is_valid() const { return format_ctx_ && video_ctx_ && texture_; }

private:
    void decode_loop();   ///< 백그라운드 디코딩 스레드 함수
    void cleanup();       ///< 리소스 정리 (소멸자에서 호출)

    // FFmpeg 자원
    AVFormatContext* format_ctx_          = nullptr;
    AVCodecContext*  video_ctx_           = nullptr;
    AVCodecContext*  audio_ctx_           = nullptr;
    AVCodecContext*  subtitle_ctx_        = nullptr; ///< 내장 자막 코덱
    AVStream*        video_stream_        = nullptr;
    SwsContext*      sws_ctx_             = nullptr; ///< 픽셀 포맷 변환 (YUV→RGBA)
    AVFrame*         rgb_frame_           = nullptr; ///< RGBA 변환된 프레임 저장용
    uint8_t*         rgb_buffer_          = nullptr; ///< rgb_frame_ 의 데이터 버퍼
    int              video_stream_idx_    = -1;
    int              audio_stream_idx_    = -1;
    int              subtitle_stream_idx_ = -1;      ///< FFmpeg 내장 자막 스트림 인덱스

    // SDL 자원
    SDL_Renderer*    renderer_             = nullptr;
    SDL_Texture*     texture_             = nullptr; ///< 비디오 출력 텍스처
    SDL_AudioStream* audio_stream_device_ = nullptr; ///< SDL 오디오 출력 스트림

    // 스레드 동기화
    std::thread       decode_thread_;      ///< 디코딩 스레드
    std::mutex        frame_mutex_;        ///< 프레임 데이터 보호 (frame_ready_)
    std::atomic<bool> frame_ready_{false}; ///< 새 프레임이 준비되었는지 여부

    // 자막
    SubtitleTrack       subtitle_track_;   ///< 외부/내장 자막 저장소
    mutable std::mutex  subtitle_mutex_;   ///< subtitle_track_ 보호
    bool                use_embedded_sub_ = false; ///< FFmpeg 내장 자막 사용 여부

    // 재생 상태
    std::atomic<bool>   running_     {false}; ///< 디코딩 스레드 실행 중
    std::atomic<bool>   paused_      {false};
    std::atomic<bool>   ended_       {false};
    std::atomic<double> seek_target_ {-1.0};  ///< 탐색 목표 시간 (<0 이면 없음)
    std::atomic<double> cur_pts_     {0.0};   ///< 현재 비디오 PTS (초)
    std::atomic<float>  volume_      {1.0f};
    double              raw_duration_{0.0};   ///< AVFormatContext 기준 duration (AV_TIME_BASE 단위)
};

// ──────────────────────────────────────────────────────────────────
//  ImagePlayer
// ──────────────────────────────────────────────────────────────────

/**
 * @class ImagePlayer
 * @brief 정적 이미지 및 애니메이션 GIF 재생
 *
 *  SDL_image의 IMG_Animation을 사용하여 애니메이션 프레임을 관리한다.
 *  단일 이미지는 텍스처로, 애니메이션은 프레임 텍스처 벡터로 저장한다.
 */
class ImagePlayer : public MediaPlayer {
public:
    /**
     * @param filepath   이미지 파일 경로 (UTF-8)
     * @param renderer   SDL_Renderer
     * @param display_sec 정적 이미지 표시 시간(초)
     */
    ImagePlayer(const std::string& filepath, SDL_Renderer* renderer,
                float display_sec = 5.0f);
    ~ImagePlayer() override { stop(); cleanup(); }

    void play()  override;
    void stop()  override { ended_ = true; }
    bool update()override;

    void   toggle_pause()      override { paused_ = !paused_; }
    void   seek(double secs)   override;   ///< 애니메이션에서만 의미 있음
    void   set_volume(float)   override {} ///< 이미지에는 볼륨 개념 없음
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

    /**
     * @brief 프레임 단위로 상대 이동 (애니메이션 전용)
     * @param delta 양수/음수 프레임 수
     */
    void seek_frames(int delta);

private:
    void cleanup();

    SDL_Renderer*             renderer_       = nullptr;
    SDL_Texture*              image_texture_  = nullptr; ///< 단일 이미지 텍스처
    IMG_Animation*            current_anim_   = nullptr; ///< SDL_image 애니메이션 객체
    std::vector<SDL_Texture*> anim_frames_;               ///< 애니메이션 프레임 텍스처

    bool  is_animated_     = false;
    bool  paused_          = false;
    bool  ended_           = false;
    int   anim_frame_idx_  = 0;        ///< 현재 프레임 인덱스
    int   anim_elapsed_ms_ = 0;        ///< 애니메이션 시작 이후 누적 시간(ms)
    int   anim_total_ms_   = 0;        ///< 전체 애니메이션 길이(ms)
    float display_sec_     = 5.0f;     ///< 정적 이미지 표시 시간

    std::chrono::steady_clock::time_point start_time_;     ///< 재생 시작 시각
    std::chrono::steady_clock::time_point last_frame_time_;///< 마지막 프레임 전환 시각
};

// ──────────────────────────────────────────────────────────────────
//  AudioPlayer
// ──────────────────────────────────────────────────────────────────

/**
 * @class AudioPlayer
 * @brief BASS 라이브러리 기반 오디오 재생, 외부 자막 지원
 *
 *  자막:
 *    - 생성자에서 외부 .srt/.ass 탐색
 *    - get_subtitle_text() : 현재 재생 위치 기준 자막 반환
 */
class AudioPlayer : public MediaPlayer {
public:
    /**
     * @param filepath 오디오 파일 경로 (와이드 문자열)
     * @param volume   초기 볼륨 (0.0~1.0)
     */
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
    bass::Song        song_;               ///< BASS 노래 객체
    SubtitleTrack     subtitle_track_;     ///< 외부 자막 저장소
    std::atomic<bool> ended_      {false}; ///< 재생 종료 플래그 (update에서 감지)
    bool              was_playing_{false}; ///< 이전 프레임에서 재생 중이었는지 (종료 감지용)
};

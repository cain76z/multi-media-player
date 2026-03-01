/**
 * @file mediaplayer.cpp
 * @brief VideoPlayer / ImagePlayer / AudioPlayer 구현 (자막 포함)
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mediaplayer.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// ════════════════════════════════════════════════════════════════════
//  VideoPlayer
// ════════════════════════════════════════════════════════════════════

/**
 * @brief VideoPlayer 생성자
 * @param filename 비디오 파일 경로 (UTF-8)
 * @param renderer SDL_Renderer
 *
 *  FFmpeg 포맷 열기, 스트림 인덱스 찾기, 코덱 열기,
 *  외부/내장 자막 준비, RGBA 변환기 초기화, SDL 텍스처/오디오 스트림 생성.
 */
VideoPlayer::VideoPlayer(const char* filename, SDL_Renderer* renderer)
    : renderer_(renderer)
{
    // ── 포맷 열기 ─────────────────────────────────────────────────
    if (avformat_open_input(&format_ctx_, filename, nullptr, nullptr) < 0)
        return;

    avformat_find_stream_info(format_ctx_, nullptr);
    raw_duration_ = static_cast<double>(format_ctx_->duration);

    // ── 코덱 초기화 헬퍼 ─────────────────────────────────────────
    auto open_codec = [&](AVMediaType type, AVCodecContext*& ctx,
                          int& idx, int prefer = -1) {
        const AVCodec* codec = nullptr;
        idx = av_find_best_stream(format_ctx_, type, -1, prefer, &codec, 0);
        if (idx < 0) return;
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, format_ctx_->streams[idx]->codecpar);
        avcodec_open2(ctx, codec, nullptr);
    };

    open_codec(AVMEDIA_TYPE_VIDEO, video_ctx_,    video_stream_idx_);
    open_codec(AVMEDIA_TYPE_AUDIO, audio_ctx_,    audio_stream_idx_, video_stream_idx_);

    if (!video_ctx_) return;

    video_stream_ = format_ctx_->streams[video_stream_idx_];

    // ── 자막 스트림 ──────────────────────────────────────────────
    // 외부 파일 우선, 없으면 FFmpeg 내장 자막 스트림 열기
    {
        std::filesystem::path mpath(filename);
        bool external_loaded;
        {
            std::lock_guard<std::mutex> lk(subtitle_mutex_);
            external_loaded = subtitle_track_.load_file(mpath);
        }

        if (external_loaded) {
            std::wcout << L"[자막] 외부 파일 로드: "
                       << mpath.stem().wstring() << L"\n";
            use_embedded_sub_ = false;
        } else {
            // FFmpeg 내장 자막 스트림 탐색
            open_codec(AVMEDIA_TYPE_SUBTITLE, subtitle_ctx_, subtitle_stream_idx_);
            if (subtitle_stream_idx_ >= 0) {
                use_embedded_sub_ = true;
                std::cout << "[자막] 내장 스트림 #"
                          << subtitle_stream_idx_ << " 활성화\n";
            }
        }
    }

    // ── RGBA 변환 버퍼 ────────────────────────────────────────────
    int buf_sz = av_image_get_buffer_size(AV_PIX_FMT_RGBA,
                                          video_ctx_->width,
                                          video_ctx_->height, 1);
    rgb_buffer_ = static_cast<uint8_t*>(av_malloc(buf_sz));
    rgb_frame_  = av_frame_alloc();
    av_image_fill_arrays(rgb_frame_->data, rgb_frame_->linesize,
                         rgb_buffer_, AV_PIX_FMT_RGBA,
                         video_ctx_->width, video_ctx_->height, 1);

    sws_ctx_ = sws_getContext(
        video_ctx_->width, video_ctx_->height, video_ctx_->pix_fmt,
        video_ctx_->width, video_ctx_->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    // SDL_Texture 생성 (생성자 = 메인 스레드이므로 안전)
    texture_ = SDL_CreateTexture(renderer_,
                                 SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 video_ctx_->width, video_ctx_->height);

    // ── SDL 오디오 스트림 ─────────────────────────────────────────
    if (audio_ctx_) {
        SDL_AudioSpec spec{};
        spec.format   = SDL_AUDIO_F32;
        spec.channels = static_cast<Uint8>(audio_ctx_->ch_layout.nb_channels);
        spec.freq     = audio_ctx_->sample_rate;

        audio_stream_device_ = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_stream_device_) {
            SDL_SetAudioStreamGain(audio_stream_device_, volume_.load());
            SDL_ResumeAudioStreamDevice(audio_stream_device_);
        }
    }
}

/**
 * @brief VideoPlayer 리소스 정리 (소멸자 및 재초기화 시 사용)
 */
void VideoPlayer::cleanup() {
    if (sws_ctx_)             { sws_freeContext(sws_ctx_);            sws_ctx_  = nullptr; }
    if (rgb_frame_)           { av_frame_free(&rgb_frame_);                                }
    if (rgb_buffer_)          { av_free(rgb_buffer_);                 rgb_buffer_ = nullptr;}
    if (subtitle_ctx_)        { avcodec_free_context(&subtitle_ctx_);                      }
    if (video_ctx_)           { avcodec_free_context(&video_ctx_);                         }
    if (audio_ctx_)           { avcodec_free_context(&audio_ctx_);                         }
    if (format_ctx_)          { avformat_close_input(&format_ctx_);                        }
    if (audio_stream_device_) { SDL_DestroyAudioStream(audio_stream_device_);
                                audio_stream_device_ = nullptr;                            }
    if (texture_)             { SDL_DestroyTexture(texture_);         texture_  = nullptr; }
}

/**
 * @brief 전체 재생 길이 반환 (초)
 */
double VideoPlayer::get_length() const {
    return raw_duration_ / static_cast<double>(AV_TIME_BASE);
}

/**
 * @brief 볼륨 설정 (0.0~1.0)
 */
void VideoPlayer::set_volume(float v) {
    volume_ = SDL_clamp(v, 0.0f, 1.0f);
    if (audio_stream_device_)
        SDL_SetAudioStreamGain(audio_stream_device_, volume_.load());
}

/**
 * @brief 현재 시간에 해당하는 자막 텍스트 반환
 */
std::string VideoPlayer::get_subtitle_text() const {
    std::lock_guard<std::mutex> lk(subtitle_mutex_);
    return subtitle_track_.get_active(cur_pts_.load());
}

// ── play / stop ───────────────────────────────────────────────────

/**
 * @brief 디코딩 스레드 시작
 */
void VideoPlayer::play() {
    if (running_.load()) return;
    running_ = true;
    ended_   = false;
    decode_thread_ = std::thread(&VideoPlayer::decode_loop, this);
}

/**
 * @brief 디코딩 스레드 중지 및 대기
 */
void VideoPlayer::stop() {
    running_ = false;
    if (decode_thread_.joinable())
        decode_thread_.join();
}

// ── 메인 스레드: update() ────────────────────────────────────────

/**
 * @brief 메인 루프에서 매 프레임 호출, 새 프레임이 있으면 텍스처 업데이트
 * @return false면 재생 종료 (ended_)
 */
bool VideoPlayer::update() {
    if (!frame_ready_.load()) return !ended_.load();

    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_ready_.load()) {
        SDL_UpdateTexture(texture_, nullptr,
                          rgb_frame_->data[0], rgb_frame_->linesize[0]);
        frame_ready_ = false;
    }
    return !ended_.load();
}

// ── 백그라운드 디코딩 스레드 ─────────────────────────────────────

/**
 * @brief 디코딩 스레드 메인 루프
 *
 *  - seek 처리
 *  - 패킷 읽기 및 스트림별 디코딩
 *  - 비디오: sws_scale로 RGBA 변환 후 frame_ready_ 설정, PTS 기반 동기화 대기
 *  - 오디오: SDL 오디오 스트림으로 데이터 푸시
 *  - 내장 자막: AVSubtitle 파싱 → subtitle_track_에 추가
 */
void VideoPlayer::decode_loop() {
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    Uint64    start_ns = SDL_GetTicksNS();

    while (running_.load()) {

        // ── Seek 처리 ────────────────────────────────────────────
        double seek_val = seek_target_.load();
        if (seek_val >= 0.0) {
            seek_target_ = -1.0;

            av_seek_frame(format_ctx_, -1,
                          static_cast<int64_t>(seek_val * AV_TIME_BASE),
                          AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(video_ctx_);
            if (audio_ctx_)           avcodec_flush_buffers(audio_ctx_);
            if (subtitle_ctx_)        avcodec_flush_buffers(subtitle_ctx_);
            if (audio_stream_device_) SDL_ClearAudioStream(audio_stream_device_);

            // seek 후 내장 자막 캐시 비우기 (외부 파일 자막은 그대로 유지)
            if (use_embedded_sub_) {
                std::lock_guard<std::mutex> lk(subtitle_mutex_);
                subtitle_track_.clear();
            }

            start_ns = SDL_GetTicksNS()
                     - static_cast<Uint64>(seek_val * SDL_NS_PER_SECOND);
            cur_pts_ = seek_val;
        }

        // ── 일시정지 ─────────────────────────────────────────────
        if (paused_.load()) {
            SDL_Delay(10);
            start_ns += 10 * SDL_NS_PER_MS;
            continue;
        }

        // ── 패킷 읽기 ────────────────────────────────────────────
        if (av_read_frame(format_ctx_, pkt) < 0) {
            ended_ = true;
            SDL_Delay(100);
            continue;
        }

        // ── 비디오 패킷 ──────────────────────────────────────────
        if (pkt->stream_index == video_stream_idx_) {
            avcodec_send_packet(video_ctx_, pkt);

            while (avcodec_receive_frame(video_ctx_, frame) == 0) {
                // best_effort_timestamp: FFmpeg 5+ 에서 frame 필드로 직접 접근
                int64_t best_pts = frame->best_effort_timestamp;
                if (best_pts == AV_NOPTS_VALUE) {
                    av_frame_unref(frame);
                    continue;
                }
                const double pts = best_pts * av_q2d(video_stream_->time_base);

                // PTS에 맞춰 대기 (너무 빠르게 디코딩되지 않도록)
                // ✅ 수정: 남은 시간을 계산해서 한 번만 sleep
                const Uint64 target_ns = start_ns + static_cast<Uint64>(pts * SDL_NS_PER_SECOND);
                while (running_.load() && seek_target_.load() < 0.0) {
                    Uint64 now_ns = SDL_GetTicksNS();
                    if (now_ns >= target_ns) break;

                    Uint64 remaining_ms = (target_ns - now_ns) / SDL_NS_PER_MS;
                    if (remaining_ms > 1)
                        SDL_Delay(static_cast<Uint32>(remaining_ms - 1)); // 1ms 여유 남기고 sleep
                    else
                        break; // 1ms 이내면 그냥 진행
                }                
                // while (running_.load() && seek_target_.load() < 0.0 &&
                //        SDL_GetTicksNS() < start_ns +
                //            static_cast<Uint64>(pts * SDL_NS_PER_SECOND))
                // {
                //     SDL_Delay(1);
                // }

                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    sws_scale(sws_ctx_,
                              frame->data, frame->linesize,
                              0, video_ctx_->height,
                              rgb_frame_->data, rgb_frame_->linesize);
                    frame_ready_ = true;
                }
                cur_pts_ = pts;
            }
        }
        // ── 오디오 패킷 ──────────────────────────────────────────
        else if (pkt->stream_index == audio_stream_idx_ &&
                 audio_ctx_ && audio_stream_device_)
        {
            avcodec_send_packet(audio_ctx_, pkt);

            while (avcodec_receive_frame(audio_ctx_, frame) == 0) {
                if (av_sample_fmt_is_planar(
                        static_cast<AVSampleFormat>(frame->format)))
                {
                    SDL_PutAudioStreamPlanarData(
                        audio_stream_device_,
                        const_cast<const void* const*>(
                            reinterpret_cast<void* const*>(frame->data)),
                        audio_ctx_->ch_layout.nb_channels,
                        frame->nb_samples);
                } else {
                    int bytes = frame->nb_samples
                              * audio_ctx_->ch_layout.nb_channels
                              * av_get_bytes_per_sample(
                                    static_cast<AVSampleFormat>(frame->format));
                    SDL_PutAudioStreamData(
                        audio_stream_device_, frame->data[0], bytes);
                }
            }
        }
        // ── 내장 자막 패킷 ───────────────────────────────────────
        else if (use_embedded_sub_ &&
                 pkt->stream_index == subtitle_stream_idx_ &&
                 subtitle_ctx_)
        {
            // 자막 디코딩: send/receive 패턴은 subtitle에 미적용 (FFmpeg 미지원)
            // avcodec_decode_subtitle2가 자막 전용 유일한 정상 API
            AVSubtitle sub{};
            int got_sub = 0;
            if (avcodec_decode_subtitle2(subtitle_ctx_, &sub, &got_sub, pkt) >= 0
                && got_sub)
            {
                AVStream* ssm = format_ctx_->streams[subtitle_stream_idx_];
                double base   = av_q2d(ssm->time_base);
                double start  = (pkt->pts != AV_NOPTS_VALUE)
                              ? pkt->pts * base : 0.0;
                double end    = (sub.end_display_time > 0)
                              ? start + sub.end_display_time / 1000.0
                              : start + 3.0;  // 기본 3초 표시

                for (unsigned r = 0; r < sub.num_rects; ++r) {
                    const AVSubtitleRect* rect = sub.rects[r];
                    std::string raw_text;

                    if (rect->type == SUBTITLE_ASS && rect->ass)
                        raw_text = rect->ass;
                    else if (rect->type == SUBTITLE_TEXT && rect->text)
                        raw_text = rect->text;

                    if (!raw_text.empty()) {
                        std::lock_guard<std::mutex> lk(subtitle_mutex_);
                        subtitle_track_.add_ffmpeg_entry(start, end, raw_text);
                    }
                }
                avsubtitle_free(&sub);
            }
        }

        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

// ════════════════════════════════════════════════════════════════════
//  ImagePlayer
// ════════════════════════════════════════════════════════════════════

/**
 * @brief ImagePlayer 생성자
 * @param filepath    이미지 파일 경로
 * @param renderer    SDL_Renderer
 * @param display_sec 정적 이미지 표시 시간(초)
 *
 *  SDL_image의 IMG_LoadAnimation을 사용하여 애니메이션 여부 판별.
 *  애니메이션이면 프레임 텍스처를 생성하고, 단일 이미지면 텍스처 하나를 생성.
 */
ImagePlayer::ImagePlayer(const std::string& filepath,
                         SDL_Renderer* renderer, float display_sec)
    : renderer_(renderer), display_sec_(display_sec)
{
    current_anim_ = IMG_LoadAnimation(filepath.c_str());

    if (current_anim_ && current_anim_->count > 1) {
        is_animated_  = true;
        anim_total_ms_ = 0;

        for (int i = 0; i < current_anim_->count; ++i) {
            int d = (current_anim_->delays[i] > 0) ? current_anim_->delays[i] : 100;
            anim_total_ms_ += d;
            SDL_Texture* t = SDL_CreateTextureFromSurface(
                renderer_, current_anim_->frames[i]);
            if (t) anim_frames_.push_back(t);
        }
    } else {
        if (current_anim_) { IMG_FreeAnimation(current_anim_); current_anim_ = nullptr; }
        image_texture_ = IMG_LoadTexture(renderer_, filepath.c_str());
    }
}

/**
 * @brief ImagePlayer 리소스 정리
 */
void ImagePlayer::cleanup() {
    if (image_texture_) { SDL_DestroyTexture(image_texture_); image_texture_ = nullptr; }
    for (auto* t : anim_frames_) SDL_DestroyTexture(t);
    anim_frames_.clear();
    if (current_anim_) { IMG_FreeAnimation(current_anim_); current_anim_ = nullptr; }
}

/**
 * @brief 재생 시작 (타이머 초기화)
 */
void ImagePlayer::play() {
    ended_           = false;
    paused_          = false;
    anim_frame_idx_  = 0;
    anim_elapsed_ms_ = 0;
    auto now         = std::chrono::steady_clock::now();
    start_time_      = now;
    last_frame_time_ = now;
}

/**
 * @brief 현재 재생 위치 (초)
 */
double ImagePlayer::get_position() const {
    using namespace std::chrono;
    return duration_cast<duration<double>>(
        steady_clock::now() - start_time_).count();
}

/**
 * @brief 현재 표시할 텍스처 반환 (애니메이션 프레임 또는 단일 이미지)
 */
SDL_Texture* ImagePlayer::get_texture() const {
    if (is_animated_ && !anim_frames_.empty())
        return anim_frames_[anim_frame_idx_];
    return image_texture_;
}

/**
 * @brief 매 프레임 호출, 타이머 및 프레임 전환 처리
 * @return false면 표시 시간이 종료되었음 (ended_)
 */
bool ImagePlayer::update() {
    if (ended_) return false;

    using namespace std::chrono;
    double elapsed = duration_cast<duration<double>>(
        steady_clock::now() - start_time_).count();
    if (elapsed >= static_cast<double>(display_sec_)) {
        ended_ = true;
        return false;
    }

    if (paused_) return true;

    if (is_animated_ && !anim_frames_.empty()) {
        auto now        = steady_clock::now();
        int  elapsed_ms = static_cast<int>(
            duration_cast<milliseconds>(now - last_frame_time_).count());

        int cur_delay = (current_anim_ &&
                         current_anim_->delays[anim_frame_idx_] > 0)
                      ? current_anim_->delays[anim_frame_idx_] : 100;

        if (elapsed_ms >= cur_delay) {
            anim_frame_idx_   = (anim_frame_idx_ + 1)
                              % static_cast<int>(anim_frames_.size());
            last_frame_time_  = now;
        }
    }
    return true;
}

/**
 * @brief 애니메이션에서 지정된 시간(초)으로 이동
 * @param secs 목표 시간 (전체 애니메이션 길이 내에서 순환)
 */
void ImagePlayer::seek(double secs) {
    if (!is_animated_ || anim_total_ms_ <= 0) return;

    int target_ms = static_cast<int>(secs * 1000.0) % anim_total_ms_;
    if (target_ms < 0) target_ms = 0;

    int acc = 0;
    for (int i = 0; i < static_cast<int>(anim_frames_.size()); ++i) {
        int d = (current_anim_ && current_anim_->delays[i] > 0)
              ? current_anim_->delays[i] : 100;
        if (acc + d > target_ms || i == static_cast<int>(anim_frames_.size()) - 1) {
            anim_frame_idx_  = i;
            anim_elapsed_ms_ = target_ms;
            last_frame_time_ = std::chrono::steady_clock::now();
            break;
        }
        acc += d;
    }
}

/**
 * @brief 애니메이션 프레임 단위 이동
 * @param delta 이동할 프레임 수 (양수/음수)
 */
void ImagePlayer::seek_frames(int delta) {
    if (!is_animated_ || anim_frames_.empty()) return;

    int count = static_cast<int>(anim_frames_.size());
    anim_frame_idx_ = ((anim_frame_idx_ + delta) % count + count) % count;

    int acc = 0;
    for (int i = 0; i < anim_frame_idx_; ++i) {
        int d = (current_anim_ && current_anim_->delays[i] > 0)
              ? current_anim_->delays[i] : 100;
        acc += d;
    }
    anim_elapsed_ms_ = acc;
    last_frame_time_ = std::chrono::steady_clock::now();
}

// ════════════════════════════════════════════════════════════════════
//  AudioPlayer
// ════════════════════════════════════════════════════════════════════

/**
 * @brief AudioPlayer 생성자
 * @param filepath 오디오 파일 경로 (와이드 문자열)
 * @param volume   초기 볼륨
 *
 *  BASS Song 로드, 외부 자막 탐색.
 */
AudioPlayer::AudioPlayer(const std::wstring& filepath, float volume) {
    song_.load(filepath.c_str(), BASS_SAMPLE_FLOAT);
    if (!song_.is_valid()) return;

    song_.set_volume(volume);

    // 외부 자막 파일 탐색 (.srt / .ass / .ssa)
    std::filesystem::path mpath(filepath);
    if (subtitle_track_.load_file(mpath)) {
        std::wcout << L"[자막] 외부 파일 로드: " << mpath.stem().wstring() << L"\n";
    }
}

/**
 * @brief 일시정지 토글
 */
void AudioPlayer::toggle_pause() {
    if (song_.is_playing())     song_.pause();
    else if (song_.is_paused()) song_.resume();
}

/**
 * @brief 매 프레임 호출, 종료 감지
 * @return false면 ended_가 true가 되어 자동 전환 트리거
 */
bool AudioPlayer::update() {
    if (ended_.load()) return false;

    bool playing = song_.is_playing();
    if (playing) {
        was_playing_ = true;
    } else if (was_playing_ && !song_.is_paused()) {
        ended_ = true;
        return false;
    }
    return true;
}
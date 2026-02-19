/**
 * @file media.cpp
 * @brief VideoPlayer / ImagePlayer / AudioPlayer / MediaRenderer 구현 (자막 포함)
 */

#define NOMINMAX
#include "media.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// ════════════════════════════════════════════════════════════════════
//  VideoPlayer
// ════════════════════════════════════════════════════════════════════

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

double VideoPlayer::get_length() const {
    return raw_duration_ / static_cast<double>(AV_TIME_BASE);
}

void VideoPlayer::set_volume(float v) {
    volume_ = SDL_clamp(v, 0.0f, 1.0f);
    if (audio_stream_device_)
        SDL_SetAudioStreamGain(audio_stream_device_, volume_.load());
}

std::string VideoPlayer::get_subtitle_text() const {
    std::lock_guard<std::mutex> lk(subtitle_mutex_);
    return subtitle_track_.get_active(cur_pts_.load());
}

// ── play / stop ───────────────────────────────────────────────────

void VideoPlayer::play() {
    if (running_.load()) return;
    running_ = true;
    ended_   = false;
    decode_thread_ = std::thread(&VideoPlayer::decode_loop, this);
}

void VideoPlayer::stop() {
    running_ = false;
    if (decode_thread_.joinable())
        decode_thread_.join();
}

// ── 메인 스레드: update() ────────────────────────────────────────

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
                const double pts =
                    frame->pts * av_q2d(video_stream_->time_base);

                while (running_.load() && seek_target_.load() < 0.0 &&
                       SDL_GetTicksNS() < start_ns +
                           static_cast<Uint64>(pts * SDL_NS_PER_SECOND))
                {
                    SDL_Delay(1);
                }

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
            AVSubtitle sub{};
            int got_sub = 0;
            if (avcodec_decode_subtitle2(subtitle_ctx_, &sub, &got_sub, pkt) >= 0
                && got_sub)
            {
                // 타임베이스 변환
                AVStream* ssm = format_ctx_->streams[subtitle_stream_idx_];
                double base   = av_q2d(ssm->time_base);
                double start  = (pkt->pts != AV_NOPTS_VALUE)
                              ? pkt->pts * base : 0.0;
                double end    = (sub.end_display_time > 0)
                              ? start + sub.end_display_time / 1000.0
                              : start + 3.0;  // 기본 3초 표시

                // 텍스트 추출 (ASS/SRT 모두 AVSubtitleRect로 전달됨)
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

void ImagePlayer::cleanup() {
    if (image_texture_) { SDL_DestroyTexture(image_texture_); image_texture_ = nullptr; }
    for (auto* t : anim_frames_) SDL_DestroyTexture(t);
    anim_frames_.clear();
    if (current_anim_) { IMG_FreeAnimation(current_anim_); current_anim_ = nullptr; }
}

void ImagePlayer::play() {
    ended_           = false;
    paused_          = false;
    anim_frame_idx_  = 0;
    anim_elapsed_ms_ = 0;
    auto now         = std::chrono::steady_clock::now();
    start_time_      = now;
    last_frame_time_ = now;
}

double ImagePlayer::get_position() const {
    using namespace std::chrono;
    return duration_cast<duration<double>>(
        steady_clock::now() - start_time_).count();
}

SDL_Texture* ImagePlayer::get_texture() const {
    if (is_animated_ && !anim_frames_.empty())
        return anim_frames_[anim_frame_idx_];
    return image_texture_;
}

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

void AudioPlayer::toggle_pause() {
    if (song_.is_playing())     song_.pause();
    else if (song_.is_paused()) song_.resume();
}

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

// ════════════════════════════════════════════════════════════════════
//  MediaRenderer
// ════════════════════════════════════════════════════════════════════

MediaRenderer::MediaRenderer(const std::string& title,
                             int w, int h, int x, int y,
                             bool fullscreen,
                             const std::string& font_path,
                             int font_size)
    : fullscreen_(fullscreen), font_size_(font_size)
{
    window_ = SDL_CreateWindow(title.c_str(), w, h, SDL_WINDOW_RESIZABLE);
    if (!window_) throw std::runtime_error("SDL_CreateWindow 실패");

    SDL_SetWindowPosition(window_, x, y);
    if (fullscreen_) SDL_SetWindowFullscreen(window_, true);

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        SDL_DestroyWindow(window_);
        throw std::runtime_error("SDL_CreateRenderer 실패");
    }
    SDL_SetRenderVSync(renderer_, 1);

    // SDL_ttf 초기화 및 폰트 로드
    if (!TTF_Init()) {
        std::cerr << "[자막] TTF_Init 실패: " << SDL_GetError() << "\n";
    } else {
        load_font(font_path, font_size);
    }
}

MediaRenderer::~MediaRenderer() {
    if (sub_texture_) SDL_DestroyTexture(sub_texture_);
    if (font_)        TTF_CloseFont(font_);
    TTF_Quit();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_)   SDL_DestroyWindow(window_);
}

// ── 폰트 로드 ────────────────────────────────────────────────────

std::string MediaRenderer::find_system_font() {
    // 플랫폼별 폰트 폴백 목록
    static const char* candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/malgun.ttf",      // 맑은 고딕 (한국어)
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

    // 사용자 지정 폰트 실패 시 시스템 폴백
    std::string fallback = find_system_font();
    if (!try_open(fallback)) {
        std::cerr << "[자막] 폰트를 찾을 수 없습니다. 자막이 표시되지 않습니다.\n";
    }
}

// ── 공개 메서드 ──────────────────────────────────────────────────

void MediaRenderer::toggle_fullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_);
}

void MediaRenderer::set_title(const std::string& title) {
    SDL_SetWindowTitle(window_, title.c_str());
}

bool MediaRenderer::is_over_bar(float mouse_y) const {
    int w, h; SDL_GetWindowSize(window_, &w, &h);
    float top = static_cast<float>(h) - BAR_H - BAR_MARGIN;
    return mouse_y >= top - HIT_MARGIN && mouse_y <= top + BAR_H + HIT_MARGIN;
}

float MediaRenderer::x_to_progress(float mouse_x) const {
    int w, h; SDL_GetWindowSize(window_, &w, &h);
    return SDL_clamp(mouse_x / static_cast<float>(w), 0.0f, 1.0f);
}

void MediaRenderer::seek_to_progress(MediaPlayer* player, float progress) {
    if (!player) return;
    double len = player->get_length();
    if (len > 0.0) player->seek(len * static_cast<double>(progress));
}

// ── 내부 렌더 헬퍼 ───────────────────────────────────────────────

void MediaRenderer::render_centered_texture(SDL_Texture* tex) const {
    if (!tex) return;

    float tex_w, tex_h;
    SDL_GetTextureSize(tex, &tex_w, &tex_h);

    int win_w, win_h;
    SDL_GetCurrentRenderOutputSize(renderer_, &win_w, &win_h);

    float scale  = std::min(static_cast<float>(win_w) / tex_w,
                            static_cast<float>(win_h) / tex_h);
    float draw_w = tex_w * scale;
    float draw_h = tex_h * scale;

    SDL_FRect dst = { (win_w - draw_w) / 2.0f,
                      (win_h - draw_h) / 2.0f,
                      draw_w, draw_h };
    SDL_RenderTexture(renderer_, tex, nullptr, &dst);
}

void MediaRenderer::render_progress_bar(float progress, bool highlighted) const {
    progress = SDL_clamp(progress, 0.0f, 1.0f);

    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    float bar_h = highlighted ? BAR_H + 2.0f : BAR_H;
    float top_y = static_cast<float>(win_h) - BAR_H - BAR_MARGIN
                - (highlighted ? 1.0f : 0.0f);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 150);
    SDL_FRect track = { 0.0f, top_y, static_cast<float>(win_w), bar_h };
    SDL_RenderFillRect(renderer_, &track);

    SDL_SetRenderDrawColor(renderer_, 210, 210, 210, 220);
    SDL_FRect filled = { 0.0f, top_y, static_cast<float>(win_w) * progress, bar_h };
    SDL_RenderFillRect(renderer_, &filled);

    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
    SDL_FRect handle = { static_cast<float>(win_w) * progress - 4.0f,
                         top_y - 2.0f, 8.0f, bar_h + 4.0f };
    SDL_RenderFillRect(renderer_, &handle);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

void MediaRenderer::render_fft(MediaPlayer* player) const {
    if (!player) return;

    float fft[256] = {};
    if (!player->get_fft(fft, 256)) return;

    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    constexpr int BARS  = 64;
    float         bar_w = static_cast<float>(win_w) / (BARS + 2);

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
 * @brief 자막 텍스트를 화면 하단 중앙에 렌더링합니다.
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

    // 캐시 유효성 검사 (텍스트 변경 또는 창 크기 변경)
    if (text != sub_text_cached_ || win_w != sub_win_w_) {
        if (sub_texture_) {
            SDL_DestroyTexture(sub_texture_);
            sub_texture_ = nullptr;
        }
        sub_text_cached_ = text;
        sub_win_w_       = win_w;

        // 줄별 텍스처 생성 후 합성
        // TTF_RenderUTF8_Blended_Wrapped 사용 (자동 줄바꿈 + 멀티라인)
        // '\n'은 직접 처리: 줄별로 렌더링 후 단일 텍스처로 합성

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

        // 각 줄을 SDL_Surface로 렌더
        SDL_Color white = {255, 255, 255, 255};
        std::vector<SDL_Surface*> surfs;
        int total_h = 0, max_w = 0;

        for (auto& line : lines) {
            const std::string render_line = line.empty() ? " " : line;
            SDL_Surface* s = TTF_RenderText_Blended(font_, render_line.c_str(), 0, white); //render_line.length()
            if (!s) continue;
            surfs.push_back(s);
            total_h += s->h + 2; // 줄 간격 2px
            if (s->w > max_w) max_w = s->w;
        }
        if (surfs.empty()) return;

        // 합성 서피스
        constexpr int PAD_X = 12, PAD_Y = 8;
        int surf_w = max_w  + PAD_X * 2;
        int surf_h = total_h + PAD_Y * 2;

        SDL_Surface* combined = SDL_CreateSurface(surf_w, surf_h,
                                                  SDL_PIXELFORMAT_RGBA32);
        if (!combined) {
            for (auto* s : surfs) SDL_DestroySurface(s);
            return;
        }

        // 반투명 검정 배경
        SDL_FillSurfaceRect(combined, nullptr,
            SDL_MapSurfaceRGBA(combined, 0, 0, 0, 160));

        // 줄 합성
        int y_off = PAD_Y;
        for (auto* s : surfs) {
            // 가로 중앙 정렬
            SDL_Rect dst = { PAD_X + (max_w - s->w) / 2, y_off, s->w, s->h };
            SDL_BlitSurface(s, nullptr, combined, &dst);
            y_off += s->h + 2;
            SDL_DestroySurface(s);
        }

        sub_texture_ = SDL_CreateTextureFromSurface(renderer_, combined);
        SDL_DestroySurface(combined);
        if (!sub_texture_) return;

        SDL_GetTextureSize(sub_texture_, nullptr, nullptr);
        float fw, fh;
        SDL_GetTextureSize(sub_texture_, &fw, &fh);
        sub_tex_w_ = static_cast<int>(fw);
        sub_tex_h_ = static_cast<int>(fh);
    }

    if (!sub_texture_) return;

    // 진행바 위에 배치 (BAR_H + 8px 패딩)
    constexpr float BOTTOM_MARGIN = BAR_H + BAR_MARGIN + 8.0f;
    float x = (win_w - sub_tex_w_) / 2.0f;
    float y = win_h - BOTTOM_MARGIN - sub_tex_h_;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_FRect dst = { x, y, static_cast<float>(sub_tex_w_),
                             static_cast<float>(sub_tex_h_) };
    SDL_RenderTexture(renderer_, sub_texture_, nullptr, &dst);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
}

// ── 공개 render() ────────────────────────────────────────────────

void MediaRenderer::render(MediaPlayer* player, bool bar_dragging) {
    SDL_SetRenderDrawColor(renderer_, 10, 10, 20, 255);
    SDL_RenderClear(renderer_);

    if (!player) {
        SDL_RenderPresent(renderer_);
        return;
    }

    SDL_Texture* tex      = player->get_texture();
    double       len      = player->get_length();
    float        progress = player->get_progress();

    if (tex) render_centered_texture(tex);
    else     render_fft(player);

    // 자막 (진행바보다 먼저 그려서 진행바가 위에 오도록)
    std::string sub = player->get_subtitle_text();
    if (!sub.empty()) render_subtitle(sub);

    if (len > 0.0) render_progress_bar(progress, bar_dragging);

    SDL_RenderPresent(renderer_);
}
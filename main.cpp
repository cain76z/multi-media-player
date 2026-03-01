/**
 * @file mp.cpp
 * @brief MP Media Player v3.5 – 스레드 기반 리팩토링
 *
 *  create_player() → player->play() 로 시작
 *  메인 루프: player->update() → mr.render(player)
 *  교체 시:  player.reset() (소멸자가 stop() 호출) → 새 플레이어 생성 → play()
 */

#include "mediaplayer.h"
#include "mediarender.h"
#include "args.hpp"
#include "fnutil.hpp"
#include "util.hpp"

#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <limits.h>
#endif

// ════════════════════════════════════════════════════════════════════
//  유틸리티
// ════════════════════════════════════════════════════════════════════

/**
 * @brief 실행 파일이 위치한 디렉터리 경로 반환 (와이드 문자열)
 */
static std::wstring get_exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t pos = p.find_last_of(L'\\');
    return (pos != std::wstring::npos) ? p.substr(0, pos + 1) : L"";
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string p(buf);
        size_t pos = p.find_last_of('/');
        if (pos != std::string::npos)
            return util::utf8_to_wstring(p.substr(0, pos + 1));
    }
    return L"";
#endif
}

/**
 * @brief 문자열을 숫자로 안전하게 파싱 (실패 시 fallback 반환)
 */
template<typename T>
static T safe_parse(const std::string& s, T fallback) {
    try {
        if constexpr (std::is_same_v<T, int>)   return std::stoi(s);
        if constexpr (std::is_same_v<T, float>)  return std::stof(s);
    } catch (...) {}
    return fallback;
}

/**
 * @brief "WxH" 또는 "W,H" 형태의 문자열을 파싱하여 a,b 에 저장
 * @return 파싱 성공 시 true
 */
static bool parse_pair(const std::wstring& s, int& a, int& b) {
    for (wchar_t sep : { L'x', L',' }) {
        size_t pos = s.find(sep);
        if (pos == std::wstring::npos) continue;
        int ta = safe_parse<int>(util::wstring_to_utf8(s.substr(0, pos)), -1);
        int tb = safe_parse<int>(util::wstring_to_utf8(s.substr(pos + 1)), -1);
        if (ta >= 0 && tb >= 0) { a = ta; b = tb; return true; }
    }
    return false;
}

/**
 * @brief "--geometry WxH+X+Y" 형식 파싱
 */
static void parse_geometry(const std::wstring& s, int& w, int& h, int& x, int& y) {
    size_t plus = s.find(L'+');
    std::wstring wh = (plus != std::wstring::npos) ? s.substr(0, plus) : s;
    parse_pair(wh, w, h);
    if (plus == std::wstring::npos) return;
    size_t plus2 = s.find(L'+', plus + 1);
    if (plus2 == std::wstring::npos) return;
    x = safe_parse<int>(util::wstring_to_utf8(s.substr(plus + 1, plus2 - plus - 1)), x);
    y = safe_parse<int>(util::wstring_to_utf8(s.substr(plus2 + 1)), y);
}

/**
 * @brief 쉼표로 구분된 확장자 목록을 파싱하여 소문자 집합으로 반환
 */
static std::unordered_set<std::wstring> parse_ext_list(const std::wstring& str) {
    std::unordered_set<std::wstring> exts;
    auto push = [&](std::wstring token) {
        token.erase(0, token.find_first_not_of(L" \t\r\n"));
        auto last = token.find_last_not_of(L" \t\r\n");
        if (last == std::wstring::npos) return;
        token.erase(last + 1);
        if (!token.empty() && token[0] == L'.') token.erase(0, 1);
        if (!token.empty()) exts.insert(util::to_lower_ascii(token));
    };
    size_t start = 0, end;
    while ((end = str.find(L',', start)) != std::wstring::npos) {
        push(str.substr(start, end - start));
        start = end + 1;
    }
    push(str.substr(start));
    return exts;
}

// ════════════════════════════════════════════════════════════════════
//  설정 로딩
// ════════════════════════════════════════════════════════════════════

/**
 * @brief mp.conf 파일을 읽어 키-값 쌍으로 반환
 * @param dir 실행 파일 디렉터리
 */
static std::map<std::wstring, std::wstring> load_mp_conf(const std::wstring& dir) {
    std::map<std::wstring, std::wstring> conf;
    std::ifstream ifs(util::wstring_to_utf8(dir + L"mp.conf"));
    if (!ifs.is_open()) return conf;

    std::string line, cur_val;
    std::wstring cur_key;

    auto flush = [&]() {
        if (!cur_key.empty()) {
            conf[cur_key] = util::utf8_to_wstring(cur_val);
            cur_key.clear(); cur_val.clear();
        }
    };

    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        bool cont = !line.empty() && line.back() == '\\';
        size_t eq = line.find('=');

        if (eq != std::string::npos) {
            flush();
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            k.erase(0, k.find_first_not_of(" \t")); k.erase(k.find_last_not_of(" \t") + 1);
            v.erase(0, v.find_first_not_of(" \t")); v.erase(v.find_last_not_of(" \t") + 1);
            if (cont) { v.pop_back(); v.erase(v.find_last_not_of(" \t") + 1); }
            cur_key = util::utf8_to_wstring(k);
            cur_val = v;
        } else {
            std::string v = line;
            if (cont) v.pop_back();
            v.erase(0, v.find_first_not_of(" \t")); v.erase(v.find_last_not_of(" \t") + 1);
            if (!cur_val.empty()) cur_val += ",";
            cur_val += v;
        }
        if (!cont) flush();
    }
    flush();
    return conf;
}

/**
 * @brief mp.conf 와 명령줄 인자를 병합하여 AppConfig 생성
 */
static AppConfig load_config(const Args& args,
                             const std::map<std::wstring, std::wstring>& conf) {
    AppConfig cfg;

    auto is_true = [](const std::wstring& s) {
        auto l = util::to_lower_ascii(s);
        return l == L"true" || l == L"1" || l == L"yes" || l == L"on";
    };
    auto cs = [&](const std::wstring& k) { return util::wstring_to_utf8(conf.at(k)); };

    if (conf.count(L"fullscreen")      && is_true(conf.at(L"fullscreen"))) cfg.fullscreen = true;
    if (conf.count(L"window_x"))        cfg.win_x           = safe_parse<int>  (cs(L"window_x"),        cfg.win_x);
    if (conf.count(L"window_y"))        cfg.win_y           = safe_parse<int>  (cs(L"window_y"),        cfg.win_y);
    if (conf.count(L"window_width"))    cfg.win_w           = safe_parse<int>  (cs(L"window_width"),    cfg.win_w);
    if (conf.count(L"window_height"))   cfg.win_h           = safe_parse<int>  (cs(L"window_height"),   cfg.win_h);
    if (conf.count(L"volume"))          cfg.volume          = safe_parse<float>(cs(L"volume"),          cfg.volume);
    if (conf.count(L"delay_after"))     cfg.delay_after     = safe_parse<float>(cs(L"delay_after"),     cfg.delay_after);
    if (conf.count(L"image_display"))   cfg.image_display   = safe_parse<float>(cs(L"image_display"),   cfg.image_display);
    if (conf.count(L"short_threshold")) cfg.short_threshold = safe_parse<float>(cs(L"short_threshold"), cfg.short_threshold);

    auto load_exts = [&](const std::wstring& key,
                          std::initializer_list<std::wstring> defaults)
        -> std::unordered_set<std::wstring>
    {
        if (conf.count(key)) return parse_ext_list(conf.at(key));
        return std::unordered_set<std::wstring>(defaults);
    };

    cfg.image_exts = load_exts(L"image_exts", {L"jpg",L"jpeg",L"png",L"bmp",L"gif",L"webp",L"tif",L"tiff"});
    cfg.audio_exts = load_exts(L"audio_exts", {L"mp3",L"wav",L"flac",L"ogg",L"aac",L"ape",L"m4a",L"opus"});
    cfg.video_exts = load_exts(L"video_exts", {L"mp4",L"mkv",L"avi",L"mov",L"webm",L"flv",L"mpeg",L"mpg"});

    if (conf.count(L"subtitle_font")) cfg.subtitle_font = cs(L"subtitle_font");
    if (conf.count(L"subtitle_size")) cfg.subtitle_size = safe_parse<int>(cs(L"subtitle_size"), cfg.subtitle_size);

    auto as = [&](const std::wstring& k) { return util::wstring_to_utf8(args.get(k)); };
    if (args.has(L"--volume"))          cfg.volume          = safe_parse<float>(as(L"--volume"),          cfg.volume);
    if (args.has(L"--delay"))           cfg.delay_after     = safe_parse<float>(as(L"--delay"),           cfg.delay_after);
    if (args.has(L"--image-display"))   cfg.image_display   = safe_parse<float>(as(L"--image-display"),   cfg.image_display);
    if (args.has(L"--short-threshold")) cfg.short_threshold = safe_parse<float>(as(L"--short-threshold"), cfg.short_threshold);
    if (args.has(L"--subtitle-font"))   cfg.subtitle_font   = as(L"--subtitle-font");
    if (args.has(L"--subtitle-size"))   cfg.subtitle_size   = safe_parse<int>(as(L"--subtitle-size"),     cfg.subtitle_size);
    if (args.get_bool(L"--fullscreen")) cfg.fullscreen = true;

    if (args.has(L"--geometry")) parse_geometry(args.get(L"--geometry"), cfg.win_w, cfg.win_h, cfg.win_x, cfg.win_y);
    if (args.has(L"-wh"))        parse_pair(args.get(L"-wh"), cfg.win_w, cfg.win_h);
    if (args.has(L"-xy"))        parse_pair(args.get(L"-xy"), cfg.win_x, cfg.win_y);
    if (args.has(L"--x"))        cfg.win_x = safe_parse<int>(as(L"--x"),      cfg.win_x);
    if (args.has(L"--y"))        cfg.win_y = safe_parse<int>(as(L"--y"),      cfg.win_y);
    if (args.has(L"--width"))    cfg.win_w = safe_parse<int>(as(L"--width"),  cfg.win_w);
    if (args.has(L"--height"))   cfg.win_h = safe_parse<int>(as(L"--height"), cfg.win_h);

    return cfg;
}

// ════════════════════════════════════════════════════════════════════
//  플레이어 팩토리
// ════════════════════════════════════════════════════════════════════

/**
 * @brief 확장자를 판단해 적절한 MediaPlayer 인스턴스를 만들고 play()를 호출합니다.
 * @return 유효 플레이어 또는 nullptr (로드 실패)
 */
static std::unique_ptr<MediaPlayer> create_player(
    const std::filesystem::path& path,
    const AppConfig&             cfg,
    SDL_Renderer*                renderer)
{
    const std::wstring ext  = fnutil::get_extension(path.wstring()); // 확장자 추출 (소문자 변환 포함)
    const std::string  utf8 = util::wstring_to_utf8(path.wstring());

    // ── 이미지 ───────────────────────────────────────────────────
    if (cfg.image_exts.count(ext)) {
        auto p = std::make_unique<ImagePlayer>(utf8, renderer, cfg.image_display);
        if (!p->is_valid()) {
            std::wcout << L"[이미지 로드 실패] " << path.wstring() << L"\n";
            return nullptr;
        }
        p->play();
        std::wcout << L"[이미지] " << path.wstring();
        if (p->is_animated())
            std::wcout << L" (" << p->frame_count() << L" 프레임)";
        std::wcout << L"\n";
        return p;
    }

    // ── 비디오 ───────────────────────────────────────────────────
    if (cfg.video_exts.count(ext)) {
        auto p = std::make_unique<VideoPlayer>(utf8.c_str(), renderer);
        if (!p->is_valid()) {
            std::wcout << L"[비디오 로드 실패] " << path.wstring() << L" → 스킵\n";
            return nullptr;
        }
        p->set_volume(cfg.volume);
        p->play();  // 디코딩 스레드 시작
        std::wcout << L"[비디오] " << path.wstring()
                   << L" (" << util::to_wstring(util::sec2str(p->get_length())) << L")\n";
        return p;
    }

    // ── 오디오 (BASS) ─────────────────────────────────────────────
    if (cfg.audio_exts.count(ext)) {
        auto p = std::make_unique<AudioPlayer>(path.wstring(), cfg.volume);
        if (!p->is_valid()) {
            std::wcout << L"[오디오 로드 실패] " << path.wstring() << L"\n";
            return nullptr;
        }
        p->play();
        std::wcout << L"[오디오] " << path.wstring()
                   << L" (" << util::to_wstring(util::sec2str(p->get_length())) << L")\n";
        return p;
    }

    std::wcout << L"[스킵] 지원하지 않는 파일 형식: " << path.wstring() << L"\n";
    return nullptr;
}

// ════════════════════════════════════════════════════════════════════
//  헬퍼
// ════════════════════════════════════════════════════════════════════

/**
 * @brief 창 제목을 "MP - 파일명 (인덱스/전체)" 형식으로 업데이트
 */
static void update_title(MediaRenderer& mr,
                         const std::filesystem::path& path,
                         size_t idx, size_t total)
{

    std::wstring t = L"MP - "
        + fnutil::get_stem(path.wstring())
        + L" (" + std::to_wstring(idx + 1)
        + L"/" + std::to_wstring(total) + L") - "
        + L" [" + util::utf8_to_wstring(mr.get_sdl_backend_name()) + L"]";
    mr.set_title(util::wstring_to_utf8(t));
}

/**
 * @brief 플레이어 교체: 기존 stop() → 새 생성 → play()
 */
static void load_media(std::unique_ptr<MediaPlayer>& player,
                       const std::filesystem::path&  path,
                       const AppConfig&              cfg,
                       MediaRenderer&                mr,
                       size_t idx, size_t total)
{
    player.reset();  // 소멸자에서 stop() + join 자동 호출
    player = create_player(path, cfg, mr.get_renderer());
    update_title(mr, path, idx, total);
}

// ════════════════════════════════════════════════════════════════════
//  이벤트 처리
// ════════════════════════════════════════════════════════════════════

/**
 * @brief SDL 이벤트 처리
 * @param mr           MediaRenderer 참조
 * @param player       현재 플레이어
 * @param cfg          설정 (볼륨 변경 반영)
 * @param advance      출력: 다음/이전 파일 이동 요청 (+1/-1)
 * @param reload       출력: 재로드 요청 (R 키)
 * @param bar_dragging 출력: 진행바 드래그 상태
 * @return false면 종료 요청 (ESC 또는 윈도우 닫기)
 */
static bool handle_events(MediaRenderer&  mr,
                          MediaPlayer*    player,
                          AppConfig&      cfg,
                          int&            advance,
                          bool&           reload,
                          bool&           bar_dragging)
{
    int win_w, win_h;
    SDL_GetWindowSize(mr.get_window(), &win_w, &win_h);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {

        if (ev.type == SDL_EVENT_QUIT) return false;

        // 마우스: 누름
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            ev.button.button == SDL_BUTTON_LEFT)
        {
            if (mr.is_over_bar(ev.button.y)) {
                bar_dragging = true;
                MediaRenderer::seek_to_progress(player, mr.x_to_progress(ev.button.x));
            }
        }
        // 마우스: 드래그
        if (ev.type == SDL_EVENT_MOUSE_MOTION && bar_dragging)
            MediaRenderer::seek_to_progress(player, mr.x_to_progress(ev.motion.x));

        // 마우스: 뗌
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            ev.button.button == SDL_BUTTON_LEFT)
            bar_dragging = false;

        if (ev.type != SDL_EVENT_KEY_DOWN) continue;

        switch (ev.key.key) {
        case SDLK_ESCAPE:
            if (!ev.key.repeat) return false;
            break;
        case SDLK_N:
            if (!ev.key.repeat) advance = +1;
            break;
        case SDLK_P:
            if (!ev.key.repeat) advance = -1;
            break;

        case SDLK_RIGHT:
            if (!ev.key.repeat) {
                auto* ip = dynamic_cast<ImagePlayer*>(player);
                if (ip) {
                    if (ip->is_animated()) ip->seek_frames(+5);  // 애니메이션: +5프레임
                    else                   advance = +1;          // 정적 이미지: 다음 파일
                } else if (player) {
                    player->seek(player->get_position() + 5.0);  // 비디오/오디오: +5초
                }
            }
            break;

        case SDLK_LEFT:
            if (!ev.key.repeat) {
                auto* ip = dynamic_cast<ImagePlayer*>(player);
                if (ip) {
                    if (ip->is_animated()) ip->seek_frames(-5);  // 애니메이션: -5프레임
                    else                   advance = -1;          // 정적 이미지: 이전 파일
                } else if (player) {
                    player->seek(player->get_position() - 5.0);  // 비디오/오디오: -5초
                }
            }
            break;
        case SDLK_R:
            if (!ev.key.repeat) {
                if (player) player->seek(0.0);
                else        reload = true;
            }
            break;
        case SDLK_F11:
            if (!ev.key.repeat) mr.toggle_fullscreen();
            break;
        case SDLK_O:
            if (!ev.key.repeat) mr.toggle_osd();
            break;
        case SDLK_SPACE:
            if (!ev.key.repeat && player) player->toggle_pause();
            break;
        case SDLK_UP:
            cfg.volume = SDL_clamp(cfg.volume + 0.05f, 0.0f, 1.0f);
            if (player) player->set_volume(cfg.volume);
            std::wcout << L"볼륨: " << static_cast<int>(cfg.volume * 100) << L"%\n";
            break;
        case SDLK_DOWN:
            cfg.volume = SDL_clamp(cfg.volume - 0.05f, 0.0f, 1.0f);
            if (player) player->set_volume(cfg.volume);
            std::wcout << L"볼륨: " << static_cast<int>(cfg.volume * 100) << L"%\n";
            break;
        default: break;
        }
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  자동 진행 판단
// ════════════════════════════════════════════════════════════════════

/**
 * @brief 플레이어 종료 또는 짧은 오디오 반복 여부에 따라 자동 전환 필요성을 판단
 * @param player         현재 플레이어
 * @param cfg            설정
 * @param auto_next_tick 다음 자동 전환 예정 시각 (ms) (출력/입력)
 * @return true면 즉시 다음 파일로 전환해야 함
 */
static bool check_auto_advance(MediaPlayer*     player,
                               const AppConfig& cfg,
                               Uint64&          auto_next_tick)
{
    if (!player) return false;

    const Uint64 now = SDL_GetTicks();

    if (auto_next_tick > 0 && now >= auto_next_tick) {
        auto_next_tick = 0;
        return true;
    }

    if (!player->is_ended()) return false;

    // AudioPlayer: 짧은 트랙 반복
    auto* ap = dynamic_cast<AudioPlayer*>(player);
    if (ap) {
        double len = ap->get_length();
        if (len > 0.0 && len < static_cast<double>(cfg.short_threshold)) {
            ap->restart();
            return false;
        }
        if (auto_next_tick == 0)
            auto_next_tick = now + static_cast<Uint64>(cfg.delay_after * 1000.0);
        return false;
    }

    return true;  // 비디오/이미지: 즉시 전진
}

// ════════════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════════════

/**
 * @brief 프로그램 진입점
 */
int main(int argc, char* argv[]) {
    util::set_console_encoding(util::codepage::UTF8);
    std::wcout << L"🎵 MP Media Player v3.5\n\n";

    if (!bass::init(-1, 44100, 0)) {
        std::cerr << "BASS_Init 실패!\n";
        return 1;
    }

    const std::set<std::wstring> value_flags = {
        L"--volume", L"--delay", L"--image-display", L"--short-threshold",
        L"--x", L"--y", L"--width", L"--height",
        L"-xy", L"-wh", L"--geometry",
        L"--subtitle-font", L"--subtitle-size",
    };
    Args arg_parser(argc, argv, {
        .verify_exists      = true,
        .expand_directories = true,
        .value_args         = value_flags,
    });

    auto file_list = arg_parser.files();
    if (file_list.empty()) {
        std::wcout
            << L"사용법: mp [옵션] <파일|디렉터리|*.확장자>\n\n"
            << L"옵션:\n"
            << L"  --geometry WxH+X+Y       창 크기/위치\n"
            << L"  -wh WxH, -xy X,Y         창 크기/위치\n"
            << L"  --width/--height/--x/--y N\n"
            << L"  --volume 0.0~1.0         초기 볼륨\n"
            << L"  --delay N                오디오 종료 후 대기(초)\n"
            << L"  --image-display N        이미지 표시 시간(초)\n"
            << L"  --short-threshold N      반복 재생 임계 길이(초)\n"
            << L"  --subtitle-font <경로>   자막 폰트 파일 (.ttf/.otf)\n"
            << L"  --subtitle-size N        자막 폰트 크기 (기본 28)\n"
            << L"  --fullscreen             전체화면 시작\n\n"
            << L"자막: 미디어 파일과 같은 이름의 .srt/.ass/.ssa 자동 인식\n"
            << L"      내장 자막 스트림(.mkv 등)도 자동 활성화됩니다.\n\n"
            << L"키:  SPACE 일시정지  N/→ 다음  P/← 이전  R 처음\n"
            << L"     ↑/↓ 볼륨  O OSD  F11 전체화면  ESC 종료\n";
        bass::free();
        return 1;
    }

    // 플레이리스트
    std::vector<std::filesystem::path> playlist = file_list;
    fnutil::sort(playlist.begin(), playlist.end(),
                 fnutil::flag::naturalPath | fnutil::flag::ignoreCase);

    auto raw_conf = load_mp_conf(get_exe_dir());
    AppConfig cfg = load_config(arg_parser, raw_conf);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init 실패: " << SDL_GetError() << "\n";
        bass::free();
        return 1;
    }

    MediaRenderer mr("MP Media Player",
                     cfg.win_w, cfg.win_h,
                     cfg.win_x, cfg.win_y,
                     cfg.fullscreen,
                     cfg.subtitle_font,
                     cfg.subtitle_size,
                     "vulkan");

    size_t                       current_idx    = 0;
    bool                         running        = true;
    bool                         bar_dragging   = false;
    Uint64                       auto_next_tick = 0;
    std::unique_ptr<MediaPlayer> player;

    load_media(player, playlist[current_idx], cfg, mr, current_idx, playlist.size());

    // ══════════════════ 메인 루프 ══════════════════
    while (running) {

        int  advance = INT_MIN;
        bool reload  = false;

        // 1. 이벤트
        running = handle_events(mr, player.get(), cfg,
                                advance, reload, bar_dragging);
        if (!running) break;

        // 2. 명시적 트랙 이동
        if (advance != INT_MIN) {
            size_t n    = playlist.size();
            current_idx = static_cast<size_t>(
                (static_cast<long long>(current_idx) + n + advance) % n);
            load_media(player, playlist[current_idx], cfg, mr,
                       current_idx, playlist.size());
            auto_next_tick = 0;
            bar_dragging   = false;
            continue;
        }

        // 3. 재로드 (R 키)
        if (reload) {
            load_media(player, playlist[current_idx], cfg, mr,
                       current_idx, playlist.size());
            auto_next_tick = 0;
            bar_dragging   = false;
            continue;
        }

        // 4. 플레이어 tick – 메인 스레드에서 호출
        //    VideoPlayer : frame_ready_ 확인 → SDL_UpdateTexture
        //    ImagePlayer : 프레임 전환, 타이머
        //    AudioPlayer : 종료 감지
        if (player) player->update();

        // 5. 렌더링
        const std::string cur_filename = playlist.size() > current_idx
            ? util::wstring_to_utf8(
                  playlist[current_idx].filename().wstring())
            : std::string{};

        mr.render(player.get(), cur_filename, bar_dragging);

        // bool dirty = false;
        // if (player) dirty = player->update(); // update()가 "화면이 바뀌었는가" 반환

        // if (dirty || bar_dragging || mr.is_osd_enabled()) {
        //     mr.render(player.get(), cur_filename, bar_dragging);
        // }
        // 6. 자동 진행
        if (playlist.size() > 1) {
            if (check_auto_advance(player.get(), cfg, auto_next_tick)) {
                current_idx = (current_idx + 1) % playlist.size();
                load_media(player, playlist[current_idx], cfg, mr,
                           current_idx, playlist.size());
                auto_next_tick = 0;
                bar_dragging   = false;
            }
        }

        SDL_Delay(8);  // ~120fps 상한, CPU 과점유 방지
        // VSync ON이면 SDL_Delay 불필요. 단, 오디오/일시정지 중엔 이벤트 대기로 전환
        // if (!dirty) {
        //     SDL_WaitEventTimeout(nullptr, 8); // 최대 8ms 대기, 이벤트 오면 즉시 처리
        // }        
    }

    // 정리 – player 소멸자가 stop() + join 자동 처리
    player.reset();
    SDL_Quit();
    bass::free();

    std::wcout << L"✅ MP Media Player 종료\n";
    return 0;
}
/**
 * @file subtitle.cpp
 * @brief SubtitleTrack 구현 – SRT / ASS / SSA / FFmpeg 내장 자막
 */

#define NOMINMAX
#include "subtitle.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

// ════════════════════════════════════════════════════════════════════
//  시간 파싱
// ════════════════════════════════════════════════════════════════════

/// SRT 타임코드: "01:23:45,678" → 초
double SubtitleTrack::parse_srt_time(const std::string& ts) {
    int h = 0, m = 0, s = 0, ms = 0;
    std::sscanf(ts.c_str(), "%d:%d:%d,%d", &h, &m, &s, &ms);
    return h * 3600.0 + m * 60.0 + s + ms / 1000.0;
}

/// ASS 타임코드: "1:23:45.67" → 초 (centiseconds)
double SubtitleTrack::parse_ass_time(const std::string& ts) {
    int h = 0, m = 0, s = 0, cs = 0;
    std::sscanf(ts.c_str(), "%d:%d:%d.%d", &h, &m, &s, &cs);
    return h * 3600.0 + m * 60.0 + s + cs / 100.0;
}

// ════════════════════════════════════════════════════════════════════
//  텍스트 정제
// ════════════════════════════════════════════════════════════════════

/// ASS 오버라이드 코드 제거: {\an8}, {\pos(…)} 등 { } 블록 전체 제거
std::string SubtitleTrack::strip_ass_tags(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    int depth = 0;
    for (char c : s) {
        if      (c == '{') ++depth;
        else if (c == '}') { if (depth > 0) --depth; }
        else if (depth == 0) result += c;
    }
    return result;
}

/// HTML 태그 제거: <i>, <b>, <font …> 등 < > 블록 전체 제거
std::string SubtitleTrack::strip_html_tags(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    int depth = 0;
    for (char c : s) {
        if      (c == '<') ++depth;
        else if (c == '>') { if (depth > 0) --depth; }
        else if (depth == 0) result += c;
    }
    return result;
}

/// 태그 제거 + \N / \n → '\n' + 앞뒤 공백 트리밍
std::string SubtitleTrack::clean_text(const std::string& s) {
    std::string r = strip_ass_tags(strip_html_tags(s));

    // \N, \n 이스케이프 → 실제 개행
    std::string out;
    out.reserve(r.size());
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] == '\\' && i + 1 < r.size() &&
            (r[i + 1] == 'N' || r[i + 1] == 'n'))
        {
            out += '\n';
            ++i;
        } else {
            out += r[i];
        }
    }

    // 앞뒤 공백/개행 트리밍
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last  = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

// ════════════════════════════════════════════════════════════════════
//  SRT 로더
// ════════════════════════════════════════════════════════════════════

bool SubtitleTrack::load_srt(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    entries_.clear();

    auto trim_cr = [](std::string& line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    };
    auto trim_str = [](std::string& s) {
        auto f = s.find_first_not_of(" \t");
        auto l = s.find_last_not_of(" \t");
        s = (f == std::string::npos) ? std::string{} : s.substr(f, l - f + 1);
    };

    std::string  line;
    SubtitleEntry cur{};
    std::string  text_buf;
    bool         in_entry = false;

    auto flush = [&]() {
        if (in_entry && !text_buf.empty()) {
            cur.text = clean_text(text_buf);
            if (!cur.text.empty())
                entries_.push_back(cur);
        }
        text_buf.clear();
        in_entry = false;
    };

    while (std::getline(ifs, line)) {
        // UTF-8 BOM 제거
        if (line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line = line.substr(3);

        trim_cr(line);

        if (line.empty()) { flush(); continue; }

        // 시퀀스 번호(숫자만) → 새 항목 시작
        if (!line.empty() &&
            std::all_of(line.begin(), line.end(), ::isdigit))
        {
            flush();
            in_entry = true;
            cur = {};
            continue;
        }

        // 타임코드 라인: "00:00:01,000 --> 00:00:04,000 ..."
        const auto arrow = line.find("-->");
        if (arrow != std::string::npos) {
            std::string s_str = line.substr(0, arrow);
            std::string e_str = line.substr(arrow + 3);
            // 위치 정보 등 부가 텍스트 제거 (공백 뒤 첫 번째 토큰만)
            auto eol = e_str.find_first_of(" \t\r\n", e_str.find_first_not_of(" \t"));
            if (eol != std::string::npos) e_str = e_str.substr(0, eol);
            trim_str(s_str); trim_str(e_str);
            cur.start = parse_srt_time(s_str);
            cur.end   = parse_srt_time(e_str);
            continue;
        }

        // 자막 텍스트 누적
        if (in_entry) {
            if (!text_buf.empty()) text_buf += '\n';
            text_buf += line;
        }
    }
    flush();

    return !entries_.empty();
}

// ════════════════════════════════════════════════════════════════════
//  ASS / SSA 로더
// ════════════════════════════════════════════════════════════════════

bool SubtitleTrack::load_ass(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    entries_.clear();

    bool in_events    = false;
    int  text_col_idx = 9; // 기본 ASS 포맷에서 Text는 10번째 필드(0-based: 9)
    std::string line;

    auto trim_cr = [](std::string& s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
    };

    while (std::getline(ifs, line)) {
        trim_cr(line);
        if (line.empty() || line[0] == ';' || line[0] == '!') continue;

        // 섹션 헤더
        if (line[0] == '[') {
            in_events = (line.find("[Events]") != std::string::npos);
            continue;
        }
        if (!in_events) continue;

        // Format 라인 → Text 필드 인덱스 확정
        if (line.rfind("Format:", 0) == 0) {
            std::istringstream ss(line.substr(7));
            std::string field;
            int idx = 0;
            text_col_idx = 9; // 기본값 유지
            while (std::getline(ss, field, ',')) {
                auto f = field.find_first_not_of(" \t");
                auto l = field.find_last_not_of(" \t");
                if (f != std::string::npos)
                    field = field.substr(f, l - f + 1);
                if (field == "Text") { text_col_idx = idx; break; }
                ++idx;
            }
            continue;
        }

        // Dialogue 라인
        if (line.rfind("Dialogue:", 0) != 0) continue;

        const std::string data = line.substr(9);

        // text_col_idx+1 개의 필드를 콤마로 분리
        // (Text 필드 이후의 콤마는 텍스트 내용이므로 분리하지 않음)
        std::vector<std::string> fields;
        fields.reserve(text_col_idx + 2);
        size_t pos = 0;
        for (int i = 0; i <= text_col_idx; ++i) {
            if (i == text_col_idx) {
                // 나머지 전부가 Text
                fields.push_back(data.substr(pos));
            } else {
                const auto comma = data.find(',', pos);
                if (comma == std::string::npos) {
                    fields.push_back(data.substr(pos));
                    break;
                }
                fields.push_back(data.substr(pos, comma - pos));
                pos = comma + 1;
            }
        }

        if (static_cast<int>(fields.size()) < text_col_idx + 1) continue;

        // fields[1]=Start, fields[2]=End (ASS 기본 포맷)
        auto trim_f = [](std::string& s) {
            auto f = s.find_first_not_of(" \t");
            auto l = s.find_last_not_of(" \t");
            s = (f == std::string::npos) ? std::string{} : s.substr(f, l - f + 1);
        };
        if (fields.size() < 3) continue;
        trim_f(fields[1]); trim_f(fields[2]);

        double start = parse_ass_time(fields[1]);
        double end   = parse_ass_time(fields[2]);
        std::string text = clean_text(fields[text_col_idx]);

        if (!text.empty())
            entries_.push_back({start, end, text});
    }

    // ASS는 시간 순서가 보장되지 않을 수 있으므로 정렬
    sort_entries();
    return !entries_.empty();
}

// ════════════════════════════════════════════════════════════════════
//  외부 파일 자동 탐색
// ════════════════════════════════════════════════════════════════════

bool SubtitleTrack::load_file(const std::filesystem::path& media_path) {
    const auto base = media_path.parent_path() / media_path.stem();

    // 우선순위: .srt > .ass > .ssa
    for (const char* ext : {".srt", ".ass", ".ssa"}) {
        auto p = base;
        p += ext;
        if (!std::filesystem::exists(p)) continue;

        bool ok = (std::string(ext) == ".srt") ? load_srt(p) : load_ass(p);
        if (ok) return true;
    }
    return false;
}

// ════════════════════════════════════════════════════════════════════
//  FFmpeg 내장 자막 추가
// ════════════════════════════════════════════════════════════════════

void SubtitleTrack::add_ffmpeg_entry(double start, double end,
                                     const std::string& raw_text) {
    std::string text = clean_text(raw_text);
    if (text.empty()) return;

    // 시간순 삽입 (decode_loop는 PTS 순서로 패킷을 처리하므로 거의 정렬 상태)
    SubtitleEntry entry{start, end, std::move(text)};
    auto it = std::lower_bound(entries_.begin(), entries_.end(), entry,
        [](const SubtitleEntry& a, const SubtitleEntry& b) {
            return a.start < b.start;
        });
    entries_.insert(it, std::move(entry));
}

void SubtitleTrack::sort_entries() {
    std::sort(entries_.begin(), entries_.end(),
        [](const SubtitleEntry& a, const SubtitleEntry& b) {
            return a.start < b.start;
        });
}

// ════════════════════════════════════════════════════════════════════
//  현재 자막 조회 (O(log n) 이진 탐색)
// ════════════════════════════════════════════════════════════════════

const std::string& SubtitleTrack::get_active(double time_sec) const {
    if (entries_.empty()) return empty_str_;

    // start <= time_sec 인 마지막 항목 찾기
    auto it = std::upper_bound(
        entries_.begin(), entries_.end(), time_sec,
        [](double t, const SubtitleEntry& e) { return t < e.start; });

    if (it != entries_.begin()) {
        --it;
        if (it->start <= time_sec && time_sec < it->end)
            return it->text;
    }
    return empty_str_;
}
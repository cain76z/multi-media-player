#pragma once

/**
 * @file subtitle.h
 * @brief 자막 트랙 로딩 및 조회
 *
 *  지원 포맷:
 *    - 외부 파일: .srt / .ass / .ssa (미디어 파일과 같은 이름)
 *    - 내장 자막: FFmpeg AVSubtitle (VideoPlayer decode_loop에서 추가)
 *
 *  우선순위: 외부 .srt > 외부 .ass/.ssa > FFmpeg 내장 스트림
 */

#include <filesystem>
#include <string>
#include <vector>

struct SubtitleEntry {
    double      start;  ///< 표시 시작 (초)
    double      end;    ///< 표시 종료 (초)
    std::string text;   ///< 정제된 UTF-8 텍스트 (줄바꿈 = '\n')
};

class SubtitleTrack {
public:
    /// @brief 미디어 경로 기준 .srt/.ass/.ssa 자동 탐색 후 로드
    bool load_file(const std::filesystem::path& media_path);

    bool load_srt(const std::filesystem::path& path);
    bool load_ass(const std::filesystem::path& path); ///< .ass / .ssa 공용

    /// @brief FFmpeg 내장 자막 엔트리 추가 (decode_loop에서 호출)
    void add_ffmpeg_entry(double start, double end, const std::string& raw_text);

    /// @brief add_ffmpeg_entry 후 시간순 정렬 (decode_loop 종료 후 불필요 – 실시간 삽입)
    void sort_entries();

    /// @brief time_sec 에 해당하는 자막 텍스트 반환 (없으면 빈 문자열)
    const std::string& get_active(double time_sec) const;

    bool   is_loaded() const { return !entries_.empty(); }
    size_t size()      const { return entries_.size();   }
    void   clear()           { entries_.clear();         }

    // ── 정적 유틸 (subtitle.cpp 내부 및 VideoPlayer에서 사용) ────
    static std::string strip_ass_tags (const std::string& s); ///< {…} 제거
    static std::string strip_html_tags(const std::string& s); ///< <…> 제거
    static std::string clean_text     (const std::string& s); ///< 태그 제거 + 트리밍
    static double      parse_srt_time (const std::string& ts);///< HH:MM:SS,mmm → 초
    static double      parse_ass_time (const std::string& ts);///< H:MM:SS.cc  → 초

private:
    std::vector<SubtitleEntry> entries_;
    mutable std::string        empty_str_;
};
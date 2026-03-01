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

/**
 * @struct SubtitleEntry
 * @brief 하나의 자막 항목 (시작/종료 시간, 정제된 텍스트)
 */
struct SubtitleEntry {
    double      start;  ///< 표시 시작 시간 (초)
    double      end;    ///< 표시 종료 시간 (초)
    std::string text;   ///< 정제된 UTF-8 텍스트 (줄바꿈 = '\n')
};

/**
 * @class SubtitleTrack
 * @brief 자막 트랙 전체를 관리하고 시간에 따른 텍스트 조회를 제공
 *
 *  외부 SRT/ASS/SSA 파일을 로드하거나 FFmpeg 디코더에서 생성된
 *  내장 자막 항목을 추가할 수 있습니다. get_active()로 현재 시간의
 *  자막을 O(log n)에 찾습니다.
 */
class SubtitleTrack {
public:
    /**
     * @brief 미디어 파일 경로를 기반으로 같은 이름의 .srt/.ass/.ssa 파일을 자동 탐색하여 로드
     * @param media_path 미디어 파일의 전체 경로
     * @return 자막 파일을 성공적으로 로드했으면 true
     */
    bool load_file(const std::filesystem::path& media_path);

    /**
     * @brief SRT 파일을 직접 로드
     * @param path SRT 파일 경로
     * @return 성공 시 true
     */
    bool load_srt(const std::filesystem::path& path);

    /**
     * @brief ASS 또는 SSA 파일을 직접 로드
     * @param path ASS/SSA 파일 경로
     * @return 성공 시 true
     */
    bool load_ass(const std::filesystem::path& path);

    /**
     * @brief FFmpeg 디코딩 스레드에서 내장 자막 항목을 추가 (실시간 삽입)
     * @param start   시작 시간 (초)
     * @param end     종료 시간 (초)
     * @param raw_text AVSubtitle에서 추출한 원시 텍스트 (태그 포함 가능)
     */
    void add_ffmpeg_entry(double start, double end, const std::string& raw_text);

    /**
     * @brief 모든 항목을 시작 시간 기준으로 정렬
     * @note add_ffmpeg_entry는 이미 정렬된 상태로 삽입하므로 일반적으로 불필요
     */
    void sort_entries();

    /**
     * @brief 현재 재생 시간에 해당하는 자막 텍스트를 반환
     * @param time_sec 현재 시간 (초)
     * @return 자막 텍스트 (없으면 빈 문자열)
     */
    const std::string& get_active(double time_sec) const;

    /// @brief 자막이 하나라도 로드되었는지 확인
    bool   is_loaded() const { return !entries_.empty(); }

    /// @brief 전체 항목 수 반환
    size_t size()      const { return entries_.size();   }

    /// @brief 모든 항목 제거 (seek 시 내장 자막 캐시 비우기 등에 사용)
    void   clear()           { entries_.clear();         }

    // ── 정적 유틸리티 함수 (외부에서도 사용 가능) ────────────────────

    /**
     * @brief ASS/SSA 오버라이드 태그 { ... } 를 모두 제거
     * @param s 입력 문자열
     * @return 태그가 제거된 문자열
     */
    static std::string strip_ass_tags(const std::string& s);

    /**
     * @brief HTML 태그 < ... > 를 모두 제거
     * @param s 입력 문자열
     * @return 태그가 제거된 문자열
     */
    static std::string strip_html_tags(const std::string& s);

    /**
     * @brief ASS 태그, HTML 태그를 제거하고, \N / \n 을 실제 줄바꿈으로 변환,
     *        앞뒤 공백을 제거한 최종 텍스트 반환
     * @param s 원시 자막 텍스트
     * @return 화면 표시용 정제된 텍스트
     */
    static std::string clean_text(const std::string& s);

    /**
     * @brief SRT 타임코드 "HH:MM:SS,mmm" 을 초 단위로 변환
     * @param ts 타임코드 문자열
     * @return 초 단위 시간
     */
    static double parse_srt_time(const std::string& ts);

    /**
     * @brief ASS 타임코드 "H:MM:SS.cc" (cc = centiseconds) 를 초 단위로 변환
     * @param ts 타임코드 문자열
     * @return 초 단위 시간
     */
    static double parse_ass_time(const std::string& ts);

private:
    std::vector<SubtitleEntry> entries_;      ///< 시간순으로 정렬된 자막 항목 목록
    mutable std::string        empty_str_;    ///< 빈 문자열 참조 반환용
};
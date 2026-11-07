// Pair raw FFmpeg frames with stats_mux_pre packet timestamps. The two input
// descriptors are borrowed: their owner closes them and manages the child.
#pragma once

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace nx::movie_decoder_pipe {

enum class Result { frame, eof, error };

class Reader {
public:
    // A frame occupies exactly frame_bytes on stdout. Metadata is read from
    // stderr, where ordinary FFmpeg diagnostic lines are ignored.
    Reader(int pixels_fd, int metadata_fd, size_t frame_bytes)
        : pixels_fd_(pixels_fd), metadata_fd_(metadata_fd), frame_bytes_(frame_bytes) {
        if (pixels_fd < 0 || metadata_fd < 0 || pixels_fd == metadata_fd || frame_bytes == 0)
            state_ = Result::error;
    }

    Result next(std::vector<uint8_t>& pixels, int64_t& pts_us) {
        if (state_ != Result::frame) return state_;
        size_t got = 0;
        for (;;) {
            if (got == frame_bytes_ && !timestamps_.empty()) {
                pts_us = timestamps_.front();
                timestamps_.pop_front();
                return Result::frame;
            }
            // Keep at most 64 parsed timestamps plus a single bounded raw
            // metadata chunk. This permits many tiny, already-produced frames
            // without rejecting a whole read merely because it contains >64.
            if (metadata_pos_ != metadata_size_ && timestamps_.size() < kMaxTimestamps) {
                if (!metadata(got)) return fail();
                continue;
            }
            if (metadata_pipe_eof_ && metadata_pos_ == metadata_size_ && !metadata_eof_) {
                metadata_eof_ = true;
                // A final partial diagnostics line is harmless, but a
                // partial stats line cannot describe a complete packet.
                if (!discarding_ && starts_stats(line_)) return fail();
                line_.clear();
            }
            if (pixels_eof_ && got != 0) return fail();
            if (pixels_eof_ && !timestamps_.empty()) return fail();
            if (metadata_eof_ && timestamps_.empty() && got != 0) return fail();
            if (pixels_eof_ && metadata_eof_) {
                state_ = Result::eof;
                return state_;
            }

            std::array<pollfd, 2> fds{};
            nfds_t count = 0;
            int pixel_index = -1, metadata_index = -1;
            if (!pixels_eof_ && got < frame_bytes_) {
                pixel_index = int(count);
                fds[count++] = {pixels_fd_, POLLIN, 0};
            }
            if (!metadata_pipe_eof_) {
                metadata_index = int(count);
                fds[count++] = {metadata_fd_, POLLIN, 0};
            }
            if (!count) return fail();
            int ready;
            do {
                ready = ::poll(fds.data(), count, -1);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) return fail();

            bool pixel_progress = false;
            if (pixel_index >= 0) {
                const short events = fds[size_t(pixel_index)].revents;
                if (events & POLLNVAL) return fail();
                if (events & (POLLIN | POLLHUP | POLLERR)) {
                    if (pixels.size() != frame_bytes_) {
                        try {
                            pixels.resize(frame_bytes_);
                        } catch (const std::exception&) {
                            return fail();
                        }
                    }
                    const size_t want = std::min(frame_bytes_ - got, size_t(65536));
                    const ssize_t n = ::read(pixels_fd_, pixels.data() + got, want);
                    if (n > 0) {
                        got += size_t(n);
                        pixel_progress = true;
                    } else if (n == 0)
                        pixels_eof_ = true;
                    else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                        return fail();
                }
            }
            // Yield a complete pair before examining more metadata.
            if (got == frame_bytes_ && !timestamps_.empty()) continue;
            if (metadata_index >= 0) {
                const short events = fds[size_t(metadata_index)].revents;
                if (events & POLLNVAL) return fail();
                if (events & (POLLIN | POLLHUP | POLLERR)) {
                    if (metadata_pos_ != metadata_size_) {
                        // The bounded chunk and timestamp queue are full. If
                        // the producer also has unread pipe data and pixels
                        // cannot advance, waiting could deadlock both ends.
                        if (events & POLLIN) {
                            if (!pixel_progress) return fail();
                        } else if (events & POLLHUP) {
                            metadata_pipe_eof_ = true;
                        } else {
                            return fail();
                        }
                    } else {
                        const ssize_t n = ::read(metadata_fd_, metadata_bytes_.data(), metadata_bytes_.size());
                        if (n > 0) {
                            metadata_pos_ = 0;
                            metadata_size_ = size_t(n);
                        } else if (n == 0) {
                            metadata_pipe_eof_ = true;
                        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                            return fail();
                        }
                    }
                }
            }
        }
    }

private:
    static constexpr size_t kMaxLine = 4096;
    static constexpr size_t kMaxTimestamps = 64;

    Result fail() {
        state_ = Result::error;
        return state_;
    }

    static bool starts_stats(std::string_view line) {
        return line.size() >= 5 && line.substr(0, 5) == "NXPTS";
    }

    template <typename T>
    static bool number(std::string_view text, T& out) {
        if (text.empty()) return false;
        const char* first = text.data();
        const char* last = first + text.size();
        const auto result = std::from_chars(first, last, out, 10);
        return result.ec == std::errc{} && result.ptr == last;
    }

    // Round to nearest microsecond, ties away from zero. Multiplications are
    // checked before use; if 128 bits overflow, even division by the largest
    // possible 64-bit denominator cannot produce an int64_t result.
    static bool to_microseconds(int64_t pts, uint64_t num, uint64_t den, int64_t& out) {
        if (!num || !den) return false;
        using Wide = unsigned __int128;
        constexpr Wide wide_max = ~Wide(0);
        const bool negative = pts < 0;
        const uint64_t magnitude = negative ? uint64_t(-(pts + 1)) + 1 : uint64_t(pts);
        Wide product = Wide(magnitude) * Wide(num); // both inputs are at most 64 bits
        if (product > wide_max / 1000000u) return false;
        product *= 1000000u;
        Wide q = product / den;
        const Wide rem = product % den;
        if (rem * 2 >= den) ++q;
        const Wide max_positive = uint64_t(std::numeric_limits<int64_t>::max());
        const Wide limit = max_positive + (negative ? 1 : 0);
        if (q > limit) return false;
        if (negative && q == max_positive + 1)
            out = std::numeric_limits<int64_t>::min();
        else
            out = negative ? -int64_t(q) : int64_t(q);
        return true;
    }

    bool parse(std::string_view line) {
        if (line.size() < 6 || line.substr(0, 6) != "NXPTS ") return false;
        line.remove_prefix(6);
        std::array<std::string_view, 4> fields{};
        for (size_t i = 0; i < fields.size(); ++i) {
            const size_t pos = line.find(' ');
            if (i + 1 == fields.size()) {
                if (pos != std::string_view::npos) return false;
                fields[i] = line;
            } else {
                if (pos == std::string_view::npos) return false;
                fields[i] = line.substr(0, pos);
                line.remove_prefix(pos + 1);
            }
        }
        uint64_t ordinal = 0, num = 0, den = 0, size = 0;
        int64_t pts = 0, us = 0;
        const size_t slash = fields[2].find('/');
        if (slash == std::string_view::npos || fields[2].find('/', slash + 1) != std::string_view::npos ||
            !number(fields[0], ordinal) || !number(fields[1], pts) ||
            !number(fields[2].substr(0, slash), num) || !number(fields[2].substr(slash + 1), den) ||
            !number(fields[3], size) || size != frame_bytes_ || sequence_exhausted_ || ordinal != expected_ ||
            !to_microseconds(pts, num, den, us) || timestamps_.size() == kMaxTimestamps)
            return false;
        if (expected_ == std::numeric_limits<uint64_t>::max())
            sequence_exhausted_ = true;
        else
            ++expected_;
        timestamps_.push_back(us);
        return true;
    }

    bool metadata(size_t got) {
        while (metadata_pos_ < metadata_size_ && timestamps_.size() < kMaxTimestamps &&
               !(got == frame_bytes_ && !timestamps_.empty())) {
            const char ch = metadata_bytes_[metadata_pos_++];
            if (ch == '\n') {
                if (!discarding_ && starts_stats(line_) && !parse(line_)) return false;
                line_.clear();
                discarding_ = false;
            } else if (!discarding_) {
                if (line_.size() == kMaxLine) {
                    if (starts_stats(line_)) return false;
                    // Drop arbitrarily long non-stats diagnostics without
                    // storing them or mistaking their suffix for a stats line.
                    line_.clear();
                    discarding_ = true;
                } else {
                    line_.push_back(ch);
                }
            }
        }
        return true;
    }

    int pixels_fd_;
    int metadata_fd_;
    size_t frame_bytes_;
    std::deque<int64_t> timestamps_;
    std::array<char, 4096> metadata_bytes_{};
    size_t metadata_pos_ = 0, metadata_size_ = 0;
    std::string line_;
    uint64_t expected_ = 0;
    bool sequence_exhausted_ = false;
    bool pixels_eof_ = false;
    bool metadata_pipe_eof_ = false;
    bool metadata_eof_ = false;
    bool discarding_ = false;
    // frame doubles as the active state before the first result.
    Result state_ = Result::frame;
};

}  // namespace nx::movie_decoder_pipe

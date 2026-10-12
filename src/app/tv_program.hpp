#pragma once

// The order a TV channel plays its movies, as cVideoBroadcastChannel::
// ChooseNextMovieKey (Mac binary, BV 0xd8e2ac; docs/audio.md "Som da TV"):
// the Folder programs (list +0x38) in order with an index that wraps, then
// one commercial from CommercialFolder (list +0x44) drawn at random, never
// the one just played, and so alternating. CommercialInterval is not read.
// A games broadcast (IsGames) has the fixed File only. Header-only so the
// order has a headless test (tests/test_tv_program.cpp).

#include <cstddef>
#include <random>
#include <vector>

namespace os2 {

template <typename Key>
class TvProgram {
public:
    std::vector<Key> shows, commercials;

    // The next movie's key; `commercial` says which list it came from.
    // Returns false if there is nothing to play.
    template <typename Rng>
    bool next(Rng& rng, Key& out, bool* commercial = nullptr) {
        const bool ad = !commercials.empty() && !inCommercial_ && started_;
        if (ad) {
            // Never the last one again: draw among the others.
            const size_t n = commercials.size(), last = haveLast_ ? indexOf(lastCommercial_) : n;
            size_t i;
            if (last < n && n > 1) {
                i = std::uniform_int_distribution<size_t>(0, n - 2)(rng);
                if (i >= last)
                    ++i;
            } else {
                i = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
            }
            out = lastCommercial_ = commercials[i];
            haveLast_ = true;
        } else if (!shows.empty()) {
            out = shows[nextShow_++ % shows.size()];
        } else {
            return false;
        }
        inCommercial_ = ad;
        started_ = true;
        if (commercial)
            *commercial = ad;
        return true;
    }

private:
    size_t indexOf(const Key& k) const {
        for (size_t i = 0; i < commercials.size(); ++i)
            if (commercials[i] == k)
                return i;
        return commercials.size();
    }
    size_t nextShow_ = 0;
    bool inCommercial_ = false, started_ = false, haveLast_ = false;
    Key lastCommercial_{};
};

} // namespace os2

#pragma once

// Padel match scoring. No hardware dependencies, so it can be tested on the
// host.
//
// Rules: best of 3 sets. Games are 0/15/30/40 with golden point (the point
// at 40-40 wins the game). Sets go to 6 games with a 2-game lead; at 6-6 a
// tiebreak to 7 points (2 clear) decides the set 7-6.
//
// Serve: teams alternate serving each game. In a tiebreak the team due to
// serve takes the first point, then serve changes every two points; the
// game after the tiebreak is served by the team that received first.
//
// Ends: teams change ends after every set (not after the match). `swaps`
// counts those changes.

#include <stdint.h>

struct Score {
    uint8_t points[2] = {}; // game points: 0..3 (= 0/15/30/40), or tiebreak points
    uint8_t games[2] = {};  // in the current set
    uint8_t sets[2] = {};
    uint8_t set_games[3][2] = {}; // games of each finished set, per team
    uint8_t server = 0;     // team serving the next point
    uint8_t tiebreak_first_server = 0;   // team that served the first tiebreak point
    uint8_t swaps = 0;      // changes of ends so far
    bool tiebreak = false;
    int8_t winner = -1;     // team that won the match, -1 while playing

    bool endsSwapped() const { return swaps % 2; } // team 0 is on the right
    bool golden() const { return !tiebreak && points[0] == 3 && points[1] == 3; }
};

class Match {
public:
    const Score &score() const { return score_; }

    // Awards a point to team 0 or 1. Ignored once the match is over.
    void point(int team);

    // Reverts the last point. Returns false if there was nothing to undo.
    bool undo();

    // Only before the first point: sets which team serves first.
    void setFirstServer(int team);

    void reset() { *this = Match(); }

private:
    static constexpr int HISTORY_DEPTH = 256; // oldest points drop off beyond this

    void winGame(int team);

    Score score_;
    Score history_[HISTORY_DEPTH];
    int head_ = 0;  // next slot to write
    int depth_ = 0; // valid entries, up to HISTORY_DEPTH
};

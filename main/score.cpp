#include "score.h"

static constexpr int SETS_TO_WIN = 2;
static constexpr int GAMES_PER_SET = 6;
static constexpr int TIEBREAK_TO = 7;

void Match::point(int team)
{
    if (s_.winner >= 0 || (team != 0 && team != 1)) {
        return;
    }
    history_[head_] = s_;
    head_ = (head_ + 1) % HISTORY;
    if (depth_ < HISTORY) {
        depth_++;
    }

    const int other = 1 - team;

    if (s_.tiebreak) {
        s_.points[team]++;
        if (s_.points[team] >= TIEBREAK_TO && s_.points[team] - s_.points[other] >= 2) {
            winGame(team);
            return;
        }
        // Point n (0-based) is served by tb_first when n is 0, 3, 4, 7, 8...
        int n = s_.points[0] + s_.points[1];
        s_.server = ((n + 1) / 2) % 2 ? 1 - s_.tb_first : s_.tb_first;
        return;
    }

    if (s_.points[team] == 3) { // at 40: wins outright, including golden point at 40-40
        winGame(team);
    } else {
        s_.points[team]++;
    }
}

void Match::winGame(int team)
{
    const int other = 1 - team;

    s_.points[0] = s_.points[1] = 0;
    s_.games[team]++;
    if (s_.tiebreak) {
        s_.server = 1 - s_.tb_first;
        s_.tiebreak = false;
    } else {
        s_.server = 1 - s_.server;
    }

    bool set_won = (s_.games[team] >= GAMES_PER_SET && s_.games[team] - s_.games[other] >= 2) ||
                   s_.games[team] == GAMES_PER_SET + 1; // 7-6 after a tiebreak
    if (set_won) {
        int n = s_.sets[0] + s_.sets[1];
        s_.set_games[n][0] = s_.games[0];
        s_.set_games[n][1] = s_.games[1];
        s_.sets[team]++;
        s_.games[0] = s_.games[1] = 0;
        if (s_.sets[team] == SETS_TO_WIN) {
            s_.winner = team;
        } else {
            s_.swaps++; // change ends after every set but the last
        }
    } else if (s_.games[0] == GAMES_PER_SET && s_.games[1] == GAMES_PER_SET) {
        s_.tiebreak = true;
        s_.tb_first = s_.server;
    }
}

bool Match::undo()
{
    if (depth_ == 0) {
        return false;
    }
    head_ = (head_ + HISTORY - 1) % HISTORY;
    depth_--;
    s_ = history_[head_];
    return true;
}

void Match::setFirstServer(int team)
{
    if (depth_ == 0) {
        s_.server = team;
    }
}

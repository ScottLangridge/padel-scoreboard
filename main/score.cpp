#include "score.h"

static constexpr int SETS_TO_WIN = 2;
static constexpr int GAMES_PER_SET = 6;
static constexpr int TIEBREAK_TO = 7;

void Match::point(int team)
{
    if (score_.winner >= 0 || (team != 0 && team != 1)) {
        return;
    }
    history_[head_] = score_;
    head_ = (head_ + 1) % HISTORY_DEPTH;
    if (depth_ < HISTORY_DEPTH) {
        depth_++;
    }

    const int other = 1 - team;

    if (score_.tiebreak) {
        score_.points[team]++;
        if (score_.points[team] >= TIEBREAK_TO && score_.points[team] - score_.points[other] >= 2) {
            winGame(team);
            return;
        }
        // Point `played` (0-based) is served by tiebreak_first_server when it is 0, 3, 4, 7, 8...
        int played = score_.points[0] + score_.points[1];
        score_.server = ((played + 1) / 2) % 2 ? 1 - score_.tiebreak_first_server : score_.tiebreak_first_server;
        return;
    }

    if (score_.points[team] == 3) { // at 40: wins outright, including golden point at 40-40
        winGame(team);
    } else {
        score_.points[team]++;
    }
}

void Match::winGame(int team)
{
    const int other = 1 - team;

    score_.points[0] = score_.points[1] = 0;
    score_.games[team]++;
    if (score_.tiebreak) {
        score_.server = 1 - score_.tiebreak_first_server;
        score_.tiebreak = false;
    } else {
        score_.server = 1 - score_.server;
    }

    bool set_won = (score_.games[team] >= GAMES_PER_SET && score_.games[team] - score_.games[other] >= 2) ||
                   score_.games[team] == GAMES_PER_SET + 1; // 7-6 after a tiebreak
    if (set_won) {
        int set_index = score_.sets[0] + score_.sets[1];
        score_.set_games[set_index][0] = score_.games[0];
        score_.set_games[set_index][1] = score_.games[1];
        score_.sets[team]++;
        score_.games[0] = score_.games[1] = 0;
        if (score_.sets[team] == SETS_TO_WIN) {
            score_.winner = team;
        } else {
            score_.swaps++; // change ends after every set but the last
        }
    } else if (score_.games[0] == GAMES_PER_SET && score_.games[1] == GAMES_PER_SET) {
        score_.tiebreak = true;
        score_.tiebreak_first_server = score_.server;
    }
}

bool Match::undo()
{
    if (depth_ == 0) {
        return false;
    }
    head_ = (head_ + HISTORY_DEPTH - 1) % HISTORY_DEPTH;
    depth_--;
    score_ = history_[head_];
    return true;
}

void Match::setFirstServer(int team)
{
    if (depth_ == 0) {
        score_.server = team;
    }
}

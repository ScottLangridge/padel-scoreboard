// Host checks of the scoring model's additions for the display: changes of
// ends, golden point, recorded set scores, and undo restoring all of it.

#include "score.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL score_test.cpp:%d %s\n", __LINE__, #cond);                                                    \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void game(Match &match, int team)
{
    for (int i = 0; i < 4; i++) {
        match.point(team);
    }
}

// Team `team` wins a set `won`-`lost`, from 0-0.
static void set(Match &match, int team, int won, int lost)
{
    for (int i = 0; i < lost; i++) {
        game(match, 1 - team);
    }
    for (int i = 0; i < won; i++) {
        game(match, team);
    }
}

int main()
{
    {
        // Ends change after a set, not after games.
        Match match;
        game(match, 0);
        game(match, 1);
        game(match, 0);
        CHECK(match.score().swaps == 0 && !match.score().endsSwapped());
        for (int i = 0; i < 4; i++) {
            game(match, 0); // 6-1
        }
        CHECK(match.score().sets[0] == 1 && match.score().games[0] == 0 && match.score().games[1] == 0);
        CHECK(match.score().swaps == 1 && match.score().endsSwapped());
        CHECK(match.score().set_games[0][0] == 6 && match.score().set_games[0][1] == 1);
        match.undo(); // back to 5-1, 40-0
        CHECK(match.score().swaps == 0 && match.score().games[0] == 5 && match.score().points[0] == 3);
        match.point(0);
        CHECK(match.score().swaps == 1);

        set(match, 1, 6, 4); // team 1 levels: swap again
        CHECK(match.score().sets[1] == 1 && match.score().swaps == 2 && !match.score().endsSwapped());
        CHECK(match.score().set_games[1][0] == 4 && match.score().set_games[1][1] == 6);

        set(match, 0, 6, 2); // team 0 takes the match: nobody changes ends after it
        CHECK(match.score().winner == 0 && match.score().swaps == 2);
        CHECK(match.score().set_games[2][0] == 6 && match.score().set_games[2][1] == 2);
        match.undo();
        CHECK(match.score().winner == -1 && match.score().sets[0] == 1 && match.score().games[0] == 5);
    }
    {
        // Golden point: 40-40 is golden, the next point wins the game.
        Match match;
        for (int i = 0; i < 3; i++) {
            match.point(0);
            match.point(1);
        }
        CHECK(match.score().golden());
        match.point(1);
        CHECK(!match.score().golden() && match.score().games[1] == 1 && match.score().points[0] == 0);
        match.undo();
        CHECK(match.score().golden());
    }
    {
        // Tiebreak at 6-6, set 7-6, serve returns to the receiver of the tiebreak's first point.
        Match match;
        for (int i = 0; i < 6; i++) {
            game(match, 0);
            game(match, 1);
        }
        CHECK(match.score().tiebreak && match.score().games[0] == 6 && match.score().games[1] == 6);
        int tb_first = match.score().server;
        for (int i = 0; i < 7; i++) {
            match.point(1);
        }
        CHECK(!match.score().tiebreak && match.score().sets[1] == 1);
        CHECK(match.score().set_games[0][0] == 6 && match.score().set_games[0][1] == 7);
        CHECK(match.score().server == 1 - tb_first);
        CHECK(match.score().swaps == 1);
    }
    {
        // First server can only be chosen before the first point.
        Match match;
        match.setFirstServer(1);
        CHECK(match.score().server == 1);
        match.point(0);
        match.setFirstServer(0);
        CHECK(match.score().server == 1);
    }
    if (fails == 0) {
        printf("PASS score   model checks\n");
    }
    return fails ? 1 : 0;
}

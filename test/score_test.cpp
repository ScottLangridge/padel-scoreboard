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

static void game(Match &m, int team)
{
    for (int i = 0; i < 4; i++) {
        m.point(team);
    }
}

// Team `team` wins a set `won`-`lost`, from 0-0.
static void set(Match &m, int team, int won, int lost)
{
    for (int i = 0; i < lost; i++) {
        game(m, 1 - team);
    }
    for (int i = 0; i < won; i++) {
        game(m, team);
    }
}

int main()
{
    {
        // Ends change after a set, not after games.
        Match m;
        game(m, 0);
        game(m, 1);
        game(m, 0);
        CHECK(m.score().swaps == 0 && !m.score().endsSwapped());
        for (int i = 0; i < 4; i++) {
            game(m, 0); // 6-1
        }
        CHECK(m.score().sets[0] == 1 && m.score().games[0] == 0 && m.score().games[1] == 0);
        CHECK(m.score().swaps == 1 && m.score().endsSwapped());
        CHECK(m.score().set_games[0][0] == 6 && m.score().set_games[0][1] == 1);
        m.undo(); // back to 5-1, 40-0
        CHECK(m.score().swaps == 0 && m.score().games[0] == 5 && m.score().points[0] == 3);
        m.point(0);
        CHECK(m.score().swaps == 1);

        set(m, 1, 6, 4); // team 1 levels: swap again
        CHECK(m.score().sets[1] == 1 && m.score().swaps == 2 && !m.score().endsSwapped());
        CHECK(m.score().set_games[1][0] == 4 && m.score().set_games[1][1] == 6);

        set(m, 0, 6, 2); // team 0 takes the match: nobody changes ends after it
        CHECK(m.score().winner == 0 && m.score().swaps == 2);
        CHECK(m.score().set_games[2][0] == 6 && m.score().set_games[2][1] == 2);
        m.undo();
        CHECK(m.score().winner == -1 && m.score().sets[0] == 1 && m.score().games[0] == 5);
    }
    {
        // Golden point: 40-40 is golden, the next point wins the game.
        Match m;
        for (int i = 0; i < 3; i++) {
            m.point(0);
            m.point(1);
        }
        CHECK(m.score().golden());
        m.point(1);
        CHECK(!m.score().golden() && m.score().games[1] == 1 && m.score().points[0] == 0);
        m.undo();
        CHECK(m.score().golden());
    }
    {
        // Tiebreak at 6-6, set 7-6, serve returns to the receiver of the tiebreak's first point.
        Match m;
        for (int i = 0; i < 6; i++) {
            game(m, 0);
            game(m, 1);
        }
        CHECK(m.score().tiebreak && m.score().games[0] == 6 && m.score().games[1] == 6);
        int tb_first = m.score().server;
        for (int i = 0; i < 7; i++) {
            m.point(1);
        }
        CHECK(!m.score().tiebreak && m.score().sets[1] == 1);
        CHECK(m.score().set_games[0][0] == 6 && m.score().set_games[0][1] == 7);
        CHECK(m.score().server == 1 - tb_first);
        CHECK(m.score().swaps == 1);
    }
    {
        // First server can only be chosen before the first point.
        Match m;
        m.setFirstServer(1);
        CHECK(m.score().server == 1);
        m.point(0);
        m.setFirstServer(0);
        CHECK(m.score().server == 1);
    }
    if (fails == 0) {
        printf("PASS score   model checks\n");
    }
    return fails ? 1 : 0;
}

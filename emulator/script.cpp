// The emulator's test scripts: drive the firmware with remote presses on the
// virtual clock, wait for it to react, look at the panel and check results.
//
// One step per line; `#` starts a comment. Steps run in order, and the ones
// marked "waits" hold the script until the firmware has caught up. Waits
// time out after 120 s of emulated time.
//
//   Playing
//   start [a|b]              skip the splash, bind remote a as the left team
//                            (A) and b as the right (B), and have a's or b's
//                            team serve first (default a); waits for the board
//   point <a|b|x> [xN]       a press that scores; waits until the board
//                            settles (idle again, or SWAP / ENDS, or MATCH)
//   game <a|b> [xN]          points for that remote's team until it wins a game
//   set <a|b> [xN]           games until it wins a set (stops at the change of
//                            ends or the end of the match)
//   swap                     acknowledges SWAP / ENDS; waits for the board
//   undo [a|b]               a long press (undo); waits for the board
//
//   Low level
//   tap <a|b|x> [xN]         a quick press (0.15 s), not waiting
//   hold <a|b|x> <seconds>   a press held that long, not waiting
//   wait <seconds>           lets time pass
//   wait idle                until the board is idle, waiting for a press
//   wait log "<text>"        until a log line contains text
//   at <seconds>             until that time since power on
//
//   Looking and checking
//   snap                     prints the panel (# lit, . dark)
//   pixel <x> <y>            prints one pixel: 1 lit, 0 dark (x 0..31, y 0..15)
//   expect score "<text>"    the latest score line contains text, e.g.
//                            "sets 1-0 games 6-6 points 0-0 (tiebreak)"
//   expect log "<text>"      a log line since the previous `expect log`
//                            contains text
//   expect pixel <x> <y> on|off
//   expect board             the panel matches the next 16 lines
//   end                      stops here
//
// A failed check or a timeout prints FAIL with the panel and the recent log
// and exits 1. Reaching the end prints PASS and exits 0.
//
// What the script knows about the firmware's state (the screen it is on,
// the score, which remote is which team) it reads from the firmware's log.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "emulator.h"
#include "internal.h"

namespace {

constexpr int64_t WAIT_TIMEOUT_US = 120 * 1000000LL;
constexpr int64_t UNDO_HOLD_US = 1500000;

struct Step {
    int line_number;
    std::string text; // as written, for messages
    std::string command;
    std::vector<std::string> args;
    int repeat = 1;
    std::vector<std::string> board; // expect board: the 16 rows
};

// The screen the firmware is on, as its log tells it.
enum class Screen { Splash, Bind, Serve, Board, Swap, Match };

const char *screen_name(Screen screen)
{
    static const char *const NAMES[] = {"splash", "bind", "select serve", "board", "swap ends", "match over"};
    return NAMES[(int)screen];
}

struct ScoreLine {
    bool seen = false;
    std::string text;
    unsigned sets[2] = {}, games[2] = {}, points[2] = {};
};

// ---------------------------------------------------------------------------
// Parsing

std::vector<std::string> split_words(const std::string &line, int line_number)
{
    std::vector<std::string> words;
    std::string word;
    bool quoted = false, in_word = false;
    for (char ch : line) {
        if (quoted) {
            if (ch == '"') {
                quoted = false;
            } else {
                word += ch;
            }
        } else if (ch == '"') {
            quoted = in_word = true;
        } else if (ch == '#') {
            break;
        } else if (isspace((unsigned char)ch)) {
            if (in_word) {
                words.push_back(word);
                word.clear();
                in_word = false;
            }
        } else {
            word += ch;
            in_word = true;
        }
    }
    if (quoted) {
        fprintf(stderr, "emulator: script line %d: unclosed quote\n", line_number);
        exit(2);
    }
    if (in_word) {
        words.push_back(word);
    }
    return words;
}

std::string trim(const std::string &text)
{
    size_t first = text.find_first_not_of(" \t\r");
    size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

bool is_remote(const std::string &word, const char *allowed)
{
    return word.size() == 1 && strchr(allowed, word[0]);
}

bool is_number(const std::string &word)
{
    char *rest = nullptr;
    strtod(word.c_str(), &rest);
    return !word.empty() && *rest == 0;
}

// Checks a step's arguments; returns a usage message if they are wrong.
const char *check_step(const Step &step)
{
    const std::string &command = step.command;
    const std::vector<std::string> &args = step.args;
    size_t count = args.size();
    if (command == "start") {
        return count == 0 || (count == 1 && is_remote(args[0], "ab")) ? nullptr : "start [a|b]";
    }
    if (command == "point" || command == "tap") {
        return count == 1 && is_remote(args[0], "abx") ? nullptr : "point|tap <a|b|x> [xN]";
    }
    if (command == "game" || command == "set") {
        return count == 1 && is_remote(args[0], "ab") ? nullptr : "game|set <a|b> [xN]";
    }
    if (command == "swap" || command == "snap" || command == "end") {
        return count == 0 ? nullptr : "takes no arguments";
    }
    if (command == "undo") {
        return count == 0 || (count == 1 && is_remote(args[0], "ab")) ? nullptr : "undo [a|b]";
    }
    if (command == "hold") {
        return count == 2 && is_remote(args[0], "abx") && is_number(args[1]) ? nullptr : "hold <a|b|x> <seconds>";
    }
    if (command == "wait") {
        bool ok = (count == 1 && (args[0] == "idle" || is_number(args[0]))) || (count == 2 && args[0] == "log");
        return ok ? nullptr : "wait <seconds> | wait idle | wait log \"<text>\"";
    }
    if (command == "at") {
        return count == 1 && is_number(args[0]) ? nullptr : "at <seconds>";
    }
    if (command == "pixel") {
        return count == 2 && is_number(args[0]) && is_number(args[1]) ? nullptr : "pixel <x> <y>";
    }
    if (command == "expect") {
        if (count == 2 && (args[0] == "score" || args[0] == "log")) {
            return nullptr;
        }
        if (count == 4 && args[0] == "pixel" && is_number(args[1]) && is_number(args[2]) &&
            (args[3] == "on" || args[3] == "off")) {
            return nullptr;
        }
        if (count == 1 && args[0] == "board") {
            return nullptr;
        }
        return "expect score|log \"<text>\" | expect pixel <x> <y> on|off | expect board";
    }
    return "unknown command";
}

// ---------------------------------------------------------------------------
// Running

class Runner {
public:
    void add(Step step) { steps_.push_back(std::move(step)); }
    void set_watching(bool watching) { watching_ = watching; }

    void begin()
    {
        on_log([this](const std::string &line) { saw_log(line); });
        on_idle([this]() { return saw_idle(); });
        schedule(0, [this]() { resume(); });
    }

private:
    enum class Wait { None, Time, Idle, Settle, Log };

    // Runs steps until one has to wait, or the script ends.
    void resume()
    {
        while (index_ < steps_.size()) {
            const Step &step = steps_[index_];
            if (progress_ == 0 && !echoed_) {
                say(("> " + step.text).c_str());
                echoed_ = true;
            }
            if (!run(step)) {
                return; // waiting: resumed when the wait is over
            }
            index_++;
            progress_ = 0;
            echoed_ = false;
            step_state_ = {};
        }
        finish();
    }

    // One step, or the next part of one. Returns true when it is done, false
    // when it is waiting for something.
    bool run(const Step &step)
    {
        const std::string &command = step.command;
        const std::vector<std::string> &args = step.args;
        if (command == "start") {
            return run_start(args.empty() ? 'a' : args[0][0]);
        }
        if (command == "point") {
            if (progress_ == step.repeat) {
                return true;
            }
            require_board("point");
            progress_++;
            score_point(args[0][0]);
            return false;
        }
        if (command == "game" || command == "set") {
            return run_game_or_set(step, command == "set");
        }
        if (command == "swap") {
            return run_swap();
        }
        if (command == "undo") {
            if (progress_ == 1) {
                return true;
            }
            require_board("undo");
            progress_ = 1;
            press_remote(args.empty() ? 'a' : args[0][0], emu_now_us(), UNDO_HOLD_US);
            wait_for(Wait::Idle);
            return false;
        }
        if (command == "tap") {
            for (int i = 0; i < step.repeat; i++) {
                press_remote(args[0][0], emu_now_us() + i * (TAP_US + 700000), TAP_US);
            }
            return true;
        }
        if (command == "hold") {
            press_remote(args[0][0], emu_now_us(), (int64_t)(atof(args[1].c_str()) * 1e6));
            return true;
        }
        if (command == "wait" || command == "at") {
            if (progress_ == 1) {
                return true;
            }
            progress_ = 1;
            if (command == "at") {
                wait_until((int64_t)(atof(args[0].c_str()) * 1e6));
            } else if (args[0] == "idle") {
                wait_for(Wait::Idle);
            } else if (args[0] == "log") {
                wait_for(Wait::Log, args[1]);
            } else {
                wait_until(emu_now_us() + (int64_t)(atof(args[0].c_str()) * 1e6));
            }
            return false;
        }
        if (command == "snap") {
            print_panel();
            return true;
        }
        if (command == "pixel") {
            int x = atoi(args[0].c_str()), y = atoi(args[1].c_str());
            char line[64];
            snprintf(line, sizeof line, "pixel %d %d = %d at %.3f s", x, y, panel_pixel(x, y), emu_now_us() / 1e6);
            say(line);
            return true;
        }
        if (command == "expect") {
            check(step);
            return true;
        }
        if (command == "end") {
            finish();
        }
        fail("unknown command"); // check_step rules this out
    }

    // start: splash skipped, a bound left, b bound right, `server` serves.
    // Each remote's last press must be released before it can press again.
    bool run_start(char server)
    {
        switch (progress_++) {
        case 0:
            press_remote('a', emu_now_us() + 200000, TAP_US); // skips the splash
            wait_for(Wait::Log, "bind: press the left remote");
            return false;
        case 1:
            wait_until(emu_now_us() + 1000000);
            return false;
        case 2:
            press_remote('a', emu_now_us(), TAP_US);
            wait_for(Wait::Log, "press the right remote");
            return false;
        case 3:
            wait_until(emu_now_us() + 700000); // Bind takes the right remote 0.6 s after the left
            return false;
        case 4:
            press_remote('b', emu_now_us(), TAP_US);
            wait_for(Wait::Log, "select serve");
            return false;
        case 5:
            wait_until(emu_now_us() + 1000000);
            return false;
        case 6:
            press_remote(server, emu_now_us(), TAP_US);
            wait_for(Wait::Idle);
            return false;
        default:
            return true;
        }
    }

    bool run_swap()
    {
        switch (progress_++) {
        case 0:
            if (screen_ != Screen::Swap) {
                fail("swap: SWAP / ENDS isn't showing (the screen is %s)", screen_name(screen_));
            }
            wait_until(emu_now_us() + 600000); // the prompt takes presses from 0.5 s in
            return false;
        case 1:
            press_remote('a', emu_now_us(), TAP_US);
            wait_for(Wait::Idle);
            return false;
        default:
            return true;
        }
    }

    // game / set: points for `remote`'s team until it has won `repeat`
    // games or sets.
    bool run_game_or_set(const Step &step, bool whole_set)
    {
        char remote = step.args[0][0];
        int team = team_of(remote);
        if (team < 0) {
            fail("%s: remote %c isn't bound to a team yet", step.command.c_str(), remote);
        }
        StepState &state = step_state_;
        if (!state.started) {
            state = {true, score_.games[team], score_.sets[team], screen_changes_, 0, 0};
        } else if (won_since(state, team, whole_set)) {
            if (++state.won == step.repeat || screen_ != Screen::Board) {
                if (state.won < step.repeat) {
                    fail("%s: stopped at %s after %d of %d", step.command.c_str(), screen_name(screen_), state.won,
                         step.repeat);
                }
                return true;
            }
            state.games = score_.games[team];
            state.sets = score_.sets[team];
            state.screen_changes = screen_changes_;
        }
        if (++state.points > 400) {
            fail("%s: still not won after 400 points", step.command.c_str());
        }
        require_board(step.command.c_str());
        score_point(remote);
        return false;
    }

    struct StepState {
        bool started = false;
        unsigned games = 0, sets = 0;
        int screen_changes = 0;
        int won = 0, points = 0;
    };

    // Whether the team has won the game (or set) the step is playing. A set's
    // last point goes to SWAP / ENDS or MATCH before any new score line.
    bool won_since(const StepState &state, int team, bool whole_set)
    {
        bool left_board = screen_changes_ != state.screen_changes && screen_ != Screen::Board;
        if (whole_set) {
            return left_board || score_.sets[team] > state.sets;
        }
        return left_board || score_.sets[team] > state.sets || score_.games[team] > state.games;
    }

    void score_point(char remote)
    {
        press_remote(remote, emu_now_us(), TAP_US);
        wait_for(Wait::Settle);
    }

    void require_board(const char *what)
    {
        if (screen_ != Screen::Board) {
            fail("%s: the board isn't showing (the screen is %s%s)", what, screen_name(screen_),
                 screen_ == Screen::Swap ? "; acknowledge it with swap" : "");
        }
    }

    // ------------------------------------------------------------------
    // Waiting

    void wait_until(int64_t at_us)
    {
        unsigned id = begin_wait(Wait::Time, "");
        schedule(at_us, [this, id]() { end_wait(id); });
    }

    void wait_for(Wait kind, const std::string &text = "")
    {
        unsigned id = begin_wait(kind, text);
        log_seen_at_wait_ = log_.size();
        screen_changes_at_wait_ = screen_changes_;
        schedule(emu_now_us() + WAIT_TIMEOUT_US, [this, id]() {
            if (wait_ != Wait::None && wait_id_ == id) {
                fail("timed out after %.0f s waiting for %s", WAIT_TIMEOUT_US / 1e6, describe_wait());
            }
        });
    }

    unsigned begin_wait(Wait kind, const std::string &text)
    {
        wait_ = kind;
        wait_text_ = text;
        return ++wait_id_;
    }

    void end_wait(unsigned id)
    {
        if (wait_ != Wait::None && wait_id_ == id) {
            wait_ = Wait::None;
            resume();
        }
    }

    const char *describe_wait()
    {
        static std::string description;
        switch (wait_) {
        case Wait::Idle:
            return "the board to be idle";
        case Wait::Settle:
            return "the board to settle";
        case Wait::Log:
            description = "a log line with \"" + wait_text_ + "\"";
            return description.c_str();
        default:
            return "time to pass";
        }
    }

    bool saw_idle()
    {
        if (wait_ != Wait::Idle && wait_ != Wait::Settle) {
            return false;
        }
        wait_ = Wait::None;
        resume();
        return true;
    }

    void saw_log(const std::string &line)
    {
        log_.push_back(line);
        track_firmware(line);
        bool done = false;
        if (wait_ == Wait::Log) {
            done = line.find(wait_text_) != std::string::npos;
        } else if (wait_ == Wait::Settle) {
            done = screen_changes_ != screen_changes_at_wait_ && (screen_ == Screen::Swap || screen_ == Screen::Match);
        }
        if (done) {
            unsigned id = wait_id_;
            schedule(emu_now_us(), [this, id]() { end_wait(id); }); // not from inside the firmware's logging
        }
    }

    // ------------------------------------------------------------------
    // What the log says about the firmware

    void track_firmware(const std::string &line)
    {
        auto has = [&line](const char *text) { return line.find(text) != std::string::npos; };
        if (has("scoreboard: bind: press the left remote")) {
            set_screen(Screen::Bind);
        } else if (has("scoreboard: select serve")) {
            set_screen(Screen::Serve);
        } else if (has("scoreboard: swap ends")) {
            set_screen(Screen::Swap);
        } else if (has("scoreboard: match: ")) {
            set_screen(Screen::Match);
        } else if (has("scoreboard: sets ")) {
            set_screen(Screen::Board);
            read_score(line);
        }
        // Which remote is which team: the remote's code is in its PRESS line,
        // and the firmware names the code it bound.
        size_t press = line.find(" PRESS (0x");
        if (has("E (") && press != std::string::npos && press >= 1) {
            char remote = line[press - 1];
            unsigned long code = strtoul(line.c_str() + press + 8, nullptr, 16);
            for (auto &known : codes_) {
                if (known.first == remote) {
                    known.second = code;
                }
            }
        }
        unsigned long bound = 0;
        size_t at = line.find("scoreboard: remote 0x");
        if (at != std::string::npos) {
            bound = strtoul(line.c_str() + at + 19, nullptr, 16);
            int team = has("is left (team A)") ? 0 : has("is right (team B)") ? 1 : -1;
            for (auto &known : codes_) {
                if (team >= 0 && known.second == bound) {
                    teams_[known.first - 'a'] = team;
                }
            }
        }
    }

    void set_screen(Screen screen)
    {
        if (screen != screen_) {
            screen_ = screen;
            screen_changes_++;
        }
    }

    void read_score(const std::string &line)
    {
        size_t at = line.find("sets ");
        ScoreLine score;
        if (sscanf(line.c_str() + at, "sets %u-%u games %u-%u points %u-%u", &score.sets[0], &score.sets[1],
                   &score.games[0], &score.games[1], &score.points[0], &score.points[1]) == 6) {
            score.seen = true;
            score.text = line.substr(at);
            score_ = score;
        }
    }

    int team_of(char remote) const
    {
        int index = remote - 'a';
        return index >= 0 && index < 3 ? teams_[index] : -1;
    }

    // ------------------------------------------------------------------
    // Looking and checking

    void check(const Step &step)
    {
        const std::string &what = step.args[0];
        checks_++;
        if (what == "score") {
            if (!score_.seen || score_.text.find(step.args[1]) == std::string::npos) {
                fail("expected the score to have \"%s\", but it is \"%s\"", step.args[1].c_str(),
                     score_.seen ? score_.text.c_str() : "(no score yet)");
            }
        } else if (what == "log") {
            for (size_t i = log_checked_; i < log_.size(); i++) {
                if (log_[i].find(step.args[1]) != std::string::npos) {
                    log_checked_ = i + 1;
                    return;
                }
            }
            fail("expected a log line with \"%s\" since the last `expect log`", step.args[1].c_str());
        } else if (what == "pixel") {
            int x = atoi(step.args[1].c_str()), y = atoi(step.args[2].c_str());
            bool want = step.args[3] == "on";
            if (panel_pixel(x, y) != want) {
                fail("expected pixel %d %d to be %s", x, y, want ? "on" : "off");
            }
        } else {
            for (int y = 0; y < PANEL_H; y++) {
                for (int x = 0; x < PANEL_W; x++) {
                    if (panel_pixel(x, y) != (step.board[y][x] == '#')) {
                        fail("the panel doesn't match: first difference at pixel %d %d (want %s)", x, y,
                             step.board[y][x] == '#' ? "on" : "off");
                    }
                }
            }
        }
    }

    void print_panel()
    {
        char line[64];
        snprintf(line, sizeof line, "snap at %.3f s", emu_now_us() / 1e6);
        say(line);
        for (int y = 0; y < PANEL_H; y++) {
            char row[PANEL_W + 3] = "  ";
            for (int x = 0; x < PANEL_W; x++) {
                row[x + 2] = panel_pixel(x, y) ? '#' : '.';
            }
            say(row);
        }
    }

    void say(const char *line)
    {
        print_output(line);
    }

    [[noreturn]] void fail(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char message[256];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof message, format, args);
        va_end(args);
        terminal_stop();
        set_log_output(LogOutput::Stdout);
        const Step &step = steps_[std::min(index_, steps_.size() - 1)];
        printf("FAIL at %.3f s, script line %d (%s): %s\n", emu_now_us() / 1e6, step.line_number, step.text.c_str(),
               message);
        print_panel();
        printf("last log lines:\n");
        for (size_t i = log_.size() > 15 ? log_.size() - 15 : 0; i < log_.size(); i++) {
            printf("  %s\n", log_[i].c_str());
        }
        fflush(stdout);
        exit(1);
    }

    [[noreturn]] void finish()
    {
        if (watching_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // a moment to see the end
        }
        terminal_stop();
        printf("PASS: %d checks, ended at %.3f s\n", checks_, emu_now_us() / 1e6);
        fflush(stdout);
        exit(0);
    }

private:
    bool watching_ = false;
    std::vector<Step> steps_;
    size_t index_ = 0;
    int progress_ = 0; // how far through the current step
    bool echoed_ = false;
    StepState step_state_;

    Wait wait_ = Wait::None;
    std::string wait_text_;
    unsigned wait_id_ = 0;
    size_t log_seen_at_wait_ = 0;
    int screen_changes_at_wait_ = 0;

    std::vector<std::string> log_;
    size_t log_checked_ = 0;
    Screen screen_ = Screen::Splash;
    int screen_changes_ = 0;
    ScoreLine score_;
    std::pair<char, unsigned long> codes_[3] = {{'a', 0}, {'b', 0}, {'x', 0}};
    int teams_[3] = {-1, -1, -1};
    int checks_ = 0;
};

Runner g_runner;

} // namespace

void load_script(std::istream &in, bool watching)
{
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        int line_number = (int)i + 1;
        std::vector<std::string> words = split_words(lines[i], line_number);
        if (words.empty()) {
            continue;
        }
        Step step;
        step.line_number = line_number;
        step.text = trim(lines[i].substr(0, lines[i].find(" #")));
        step.command = words[0];
        for (size_t w = 1; w < words.size(); w++) {
            const std::string &word = words[w];
            if (word.size() > 1 && word[0] == 'x' && isdigit((unsigned char)word[1]) && w + 1 == words.size()) {
                step.repeat = atoi(word.c_str() + 1);
            } else {
                step.args.push_back(word);
            }
        }
        const char *problem = check_step(step);
        if (problem) {
            fprintf(stderr, "emulator: script line %d: %s\n    %s\n", line_number, problem, lines[i].c_str());
            exit(2);
        }
        if (step.command == "expect" && step.args[0] == "board") {
            for (int row = 0; row < PANEL_H; row++) {
                std::string text = ++i < lines.size() ? trim(lines[i]) : "";
                if (text.size() != PANEL_W || text.find_first_not_of("#.") != std::string::npos) {
                    fprintf(stderr, "emulator: script line %d: expect board needs 16 rows of 32 # or .\n",
                            (int)i + 1);
                    exit(2);
                }
                step.board.push_back(text);
            }
        }
        g_runner.add(std::move(step));
    }
    g_runner.set_watching(watching);
    g_runner.begin();
}

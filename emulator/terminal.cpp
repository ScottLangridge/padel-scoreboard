// The emulator's terminal display: the panel as a grid of LEDs at the top, a
// help line above it, and the log scrolling underneath. Used live and by
// --watch.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <string>

#include "emulator.h"
#include "internal.h"

namespace {

constexpr int PANEL_TOP = 3;                  // first terminal row of the panel
constexpr int LOG_TOP = PANEL_TOP + PANEL_H + 3; // first row of the scrolling log

termios g_saved_termios;
int g_rows = 40;
bool g_active = false;

void on_signal(int)
{
    terminal_stop();
    _exit(130);
}

} // namespace

void terminal_start(const char *help)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "emulator: the display needs a terminal\n");
        exit(2);
    }
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    termios raw = g_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO); // keys arrive one at a time, unechoed; Ctrl-C still works
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    winsize size = {};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    g_rows = size.ws_row > LOG_TOP ? size.ws_row : 40;

    // Alternate screen, cursor hidden, the log scrolling under the panel.
    printf("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[1;1Hpadel-scoreboard emulator   %s", help);
    printf("\x1b[%d;1H\x1b[2m--- log ---\x1b[0m", LOG_TOP - 1);
    printf("\x1b[%d;%dr", LOG_TOP, g_rows);
    fflush(stdout);
    g_active = true;
    atexit(terminal_stop);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    terminal_draw_panel();
}

void terminal_stop()
{
    if (!g_active) {
        return;
    }
    g_active = false;
    printf("\x1b[r\x1b[?25h\x1b[?1049l");
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
}

void terminal_draw_panel()
{
    char line[64];
    snprintf(line, sizeof line, "\x1b[%d;1H", PANEL_TOP);
    std::string out = line;
    for (int y = 0; y < PANEL_H; y++) {
        out += "  ";
        for (int x = 0; x < PANEL_W; x++) {
            out += panel_pixel(x, y) ? "\x1b[38;5;196m\xe2\x97\x8f " : "\x1b[38;5;237m\xc2\xb7 "; // lit ●, dark ·
        }
        out += "\x1b[0m\x1b[K\r\n";
    }
    snprintf(line, sizeof line, "\x1b[2m  t = %.1f s\x1b[0m\x1b[K", emu_now_us() / 1e6);
    out += line;
    fputs(out.c_str(), stdout);
    fflush(stdout);
}

void terminal_log(const char *line)
{
    printf("\x1b[%d;1H\r\n%s", g_rows, line); // at the bottom of the scroll region, so the log scrolls up
    fflush(stdout);
}

// Keys to remote presses. Holding a key down also works (its auto-repeat
// keeps the remote sending), if the terminal's repeat delay is under about
// half a second.
void read_keys(bool remotes)
{
    while (true) {
        char key;
        if (read(STDIN_FILENO, &key, 1) != 1 || key == 'q') {
            terminal_stop();
            _exit(0);
        }
        if (!remotes) {
            continue;
        }
        switch (key) {
        case 'a':
        case 'b':
        case 'x':
            press_remote(key, emu_now_us(), TAP_US);
            break;
        case 'A':
        case 'B':
            press_remote(key - 'A' + 'a', emu_now_us(), 1500000);
            break;
        }
    }
}

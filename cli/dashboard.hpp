// Live TTY dashboard for the hypercore CLI.
//
// Design choice: a simple ANSI redraw loop, NOT ncurses.
// Why: the dashboard is read-only and single-view — it just repaints a table
// every second. ncurses would add an external dependency (undesirable in the
// minimal boot image, where this same binary runs on the local TTY at startup)
// and windowing machinery we don't need. A handful of ANSI escapes (clear +
// home + optional alt-screen) give us flicker-free full-screen redraw with zero
// dependencies. If we later grow scrolling panes or mouse input, revisit.
//
// The dashboard is a pure CONSUMER of the control socket: it issues `list`
// each tick and renders the reply. It duplicates none of the daemon's state
// logic.
#pragma once

namespace hypercore::cli {

class Client;

// Run the dashboard until the user quits (Ctrl-C / 'q'). Returns an exit code.
int run_dashboard(Client& client);

}  // namespace hypercore::cli

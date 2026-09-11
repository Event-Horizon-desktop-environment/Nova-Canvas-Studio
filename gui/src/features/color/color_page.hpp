#pragma once

// Color page assembly (splitplan-style builder module, M0 UX scaffold): owns the
// Color workspace docks + panels and the enter/leave page-swap. All Color-page
// chrome lives here and in its sibling modules (color_widgets / mini_timeline_
// strip / node_graph_canvas); the rest of the app only calls these three free
// functions. See color.md §2.14 / §3.6 for the target layout contract.

namespace canvas::gui {

class MainWindow;

// Builds the Color workspace (bottom grading dock, left Gallery/LUTs dock,
// right Nodes/Effects/Lightbox docks) and wires cross-widget signals. Called
// once from the shell after the center workspace exists.
void build_color_page(MainWindow& main_window);

// Page-bar handoff: switch the window into the Color layout (hides the
// edit/deliver side docks + full timeline, shows the grading workspace).
void enter_color_page(MainWindow& main_window);

// Page-bar handoff: restore the previous layout when leaving the Color page
// (hides color docks, re-shows the timeline + contextual edit tools). Idempotent.
void leave_color_page(MainWindow& main_window);

}  // namespace canvas::gui
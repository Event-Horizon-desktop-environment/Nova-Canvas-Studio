#pragma once

// App-specific chrome: the per-widget stylesheets that layer token variants on
// top of the global flat-controls sheet and HorizonStyle. Each helper reads
// the active tokens at call time (see UX/theme_tokens.hpp) and is applied to
// individual widgets via apply_theme_style.

#include <QString>

namespace canvas::gui {

QString transport_bar_style();
QString timeline_tools_style();
QString page_switcher_style();
QString time_label_style();
QString dock_glow_style();
QString dock_panel_style();
QString left_tab_strip_style();
QString media_pool_style();
QString viewer_frame_style();
QString timeline_frame_style();
QString global_toolbar_style();
QString top_status_bar_style();
QString big_timecode_style();
QString bin_tree_style();
QString inspector_category_header_style();
QString inspector_card_header_open_style();
QString inspector_card_header_closed_style();
QString inspector_card_body_style();
QString inspector_tab_track_style();
QString inspector_tab_style();
QString page_pill_style();
QString flat_tool_style();
QString tool_cluster_style();
QString outline_pill_style();
QString slider_style();
QString transport_play_style();

}  // namespace canvas::gui
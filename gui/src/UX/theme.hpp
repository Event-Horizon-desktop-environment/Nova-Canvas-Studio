#pragma once

// Unifying facade over the theme system. The implementation is split into
// per-area modules so each chunk of the UX theme can be tuned without wading
// through one monolithic translation unit:
//
//   theme_tokens      design tokens (surfaces, ink, accent) + tokens()/css()
//   theme_state       active mode (dark/light), apply_theme, re-apply callbacks
//   theme_clip_colors Resolve-style clip colour swatches
//   theme_icons       SVG icon loading + theme tinting
//   theme_menu        rounded popup/menu cards
//   theme_styles      per-widget chrome stylesheets
//
// Existing callers can keep including "UX/theme.hpp" — it exposes the whole
// theme surface in one include.

#include "UX/theme_tokens.hpp"
#include "UX/theme_state.hpp"
#include "UX/theme_clip_colors.hpp"
#include "UX/theme_icons.hpp"
#include "UX/theme_menu.hpp"
#include "UX/theme_styles.hpp"
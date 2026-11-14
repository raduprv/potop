// How the interface looks (data/ui.ini, [ui] and [colors]): the font, its
// size, the dialogs' bigger text, the scale of everything, how see-through
// the panels are, the colors. The HUD and the editor's windows share it.

#ifndef UI_STYLE_H
#define UI_STYLE_H

#include <stdbool.h>

#include "ui_backend.h"

typedef struct
{
	char font_path[160];
	float font_size;
	float dialog_text_scale;       // the dialogs' text, times font_size
	float scale;
	float panel_opacity;
	struct nk_color colors[NK_COLOR_COUNT];
} ui_style_t;

void ui_style_defaults(ui_style_t *style);
// One key of ui.ini (or of a state file like it): [ui] font, font_size,
// dialog_text_scale, scale, panel_opacity; [colors] button_hover = r, g, b
// (, a). False if it is not one of these
bool ui_style_key(ui_style_t *style, const char *section, const char *key, const char *value);
// data/ui.ini read, the values kept in their ranges
void ui_style_load(ui_style_t *style);
void ui_style_clamp(ui_style_t *style);
// The colors and the shapes into Nuklear's style
void ui_style_apply(const ui_style_t *style, struct nk_context *context);

#endif

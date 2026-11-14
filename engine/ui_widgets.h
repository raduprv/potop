// The small widgets the HUD, the settings and the editor's windows are
// made of (Nuklear): a row of a label, a slider and its value; a checkbox;
// a color; a choice from a list; a tooltip

#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stdbool.h>
#include <stdint.h>

#include "ui_backend.h"

#define UI_ROW_HEIGHT 26.0f
#define UI_SMALL_ROW_HEIGHT 20.0f

// The tooltip for the next widget, while the pointer is over it (NULL or
// empty: none)
void ui_widget_tooltip(struct nk_context *context, const char *tooltip);
// A dimmed line (a heading)
void ui_widget_section(struct nk_context *context, const char *text);
// label | slider | value; true if the value changed
bool ui_widget_slider(struct nk_context *context, const char *label, float *value, float minimum, float maximum, float step, const char *format,
	const char *tooltip);
bool ui_widget_slider_int(struct nk_context *context, const char *label, int *value, int minimum, int maximum, int step, const char *tooltip);
bool ui_widget_checkbox(struct nk_context *context, const char *label, bool *value, const char *tooltip);
// label | a color, picked in its box
bool ui_widget_color(struct nk_context *context, const char *label, float color[3], const char *tooltip);
// label | one of a list: "512|1024|2048", or named: "0:Off|1:On"
bool ui_widget_choice(struct nk_context *context, const char *label, int32_t *value, const char *choices, const char *tooltip);
// A button that stays down while selected; true when clicked
bool ui_widget_toggle_button(struct nk_context *context, const char *label, const char *tooltip, bool selected);

#endif

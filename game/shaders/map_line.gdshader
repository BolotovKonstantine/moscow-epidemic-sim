shader_type canvas_item;

// Ленты линий участка карты (docs/city-package.md#участки-карты). Вершина ленты лежит на осевой
// линии, UV — смещение в полуширинах (стык и квадратные концы), код класса — в COLOR.r * 255.
// Ширина на экране — max(ширина в метрах, минимум в пикселях) плюс обводка для прохода casing.
// Стиль классов задаёт MapTheme через массивы ниже.

const int MAX_CLASSES = 16;

uniform vec4 class_color[MAX_CLASSES];
uniform float width_m[MAX_CLASSES];
uniform float min_width_px[MAX_CLASSES];
uniform float casing_px[MAX_CLASSES];
uniform float meters_per_pixel = 1.0;
uniform bool casing = false;

void vertex() {
	int cls = clamp(int(round(COLOR.r * 255.0)), 0, MAX_CLASSES - 1);
	float width = max(width_m[cls], min_width_px[cls] * meters_per_pixel);
	if (casing) {
		width += 2.0 * casing_px[cls] * meters_per_pixel;
	}
	VERTEX += UV * (0.5 * width);
	COLOR = class_color[cls];
}

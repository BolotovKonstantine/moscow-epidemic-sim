shader_type canvas_item;

// Точки жителей (#46): квадрат из четырёх вершин в положении жителя (меш участка готовит мост
// SimSession). UV.x = угол (±1) + 4 × вид местонахождения, UV.y = угол; цвет — из палитры по виду.
// Радиус постоянен в пикселях.

uniform float radius_px = 1.8;
uniform float meters_per_pixel = 1.0;
uniform vec4 colors[16] : source_color;

varying vec2 corner;
varying vec4 tint;

void vertex() {
	float code = round(UV.x / 4.0);
	corner = vec2(UV.x - 4.0 * code, UV.y);
	tint = colors[clamp(int(code), 0, 15)];
	VERTEX += corner * radius_px * meters_per_pixel;
}

void fragment() {
	if (length(corner) > 1.0) {
		discard;
	}
	COLOR = tint;
}

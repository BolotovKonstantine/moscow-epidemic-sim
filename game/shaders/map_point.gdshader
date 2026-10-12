shader_type canvas_item;

// Точки слоёв карты (#16): квадрат из четырёх вершин в одной точке, UV — угол (±1, ±1).
// Радиус постоянен в пикселях; фрагмент рисует круг цвета вершины с белой обводкой.

uniform float radius_px = 3.0;
uniform float meters_per_pixel = 1.0;
uniform vec4 outline_color : source_color = vec4(1.0);
uniform float outline_px = 1.0;

void vertex() {
	VERTEX += UV * (radius_px + outline_px) * meters_per_pixel;
}

void fragment() {
	float r = length(UV) * (radius_px + outline_px);
	if (r > radius_px + outline_px) {
		discard;
	}
	COLOR = r > radius_px ? outline_color : COLOR;
}

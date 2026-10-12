shader_type canvas_item;

// Заливка зданий участка карты (#16): обычная заливка, функции или ночное население. Данные здания —
// в UV вершины (MapTileView): x = код класса + 16 × признак оценённой функции, y = класс жителей
// (MapTheme.BUILDING_RESIDENTS_BREAKS; 255 — нет данных). Режим и цвета задаёт MapBuildingStyle.

const int MAX_CLASSES = 16;
const int MAX_BUCKETS = 8;

uniform int mode = 0;   // 0 — заливка, 1 — функции, 2 — население
uniform vec4 fill_color : source_color = vec4(0.84, 0.81, 0.78, 1.0);
uniform vec4 class_color[MAX_CLASSES];
uniform float estimated_mix = 0.55;
uniform vec4 population_color[MAX_BUCKETS];
uniform vec4 no_data_color : source_color = vec4(0.79, 0.77, 0.74, 1.0);

void vertex() {
	if (mode == 1) {
		int packed_code = int(round(UV.x));
		int cls = clamp(packed_code % MAX_CLASSES, 0, MAX_CLASSES - 1);
		COLOR = class_color[cls];
		if (packed_code >= MAX_CLASSES) {
			COLOR = mix(COLOR, fill_color, estimated_mix);
		}
	} else if (mode == 2) {
		int bucket = int(round(UV.y));
		COLOR = bucket < MAX_BUCKETS ? population_color[bucket] : no_data_color;
	} else {
		COLOR = fill_color;
	}
}

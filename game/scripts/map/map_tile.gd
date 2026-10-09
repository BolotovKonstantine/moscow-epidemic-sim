class_name MapTile
extends RefCounted

# Участок карты `.mtile`, формат 1 (docs/city-package.md#участки-карты, решение #9).
# Только данные: заголовок и разделы-массивы. Меши строит MapTileView.
# Ошибка формата не роняет игру: open() возвращает участок с непустым `error`.

const MAGIC := "MESMTILE"
const FORMAT := "mesim-map-tile"
const FORMAT_VERSION := 1
const LEVEL_TILE_M := {1: 8000.0, 2: 2000.0}   # размеры участков уровней формата 1
const MAX_DECODED_BYTES := 512 << 20   # всего данных участка после распаковки
const MAX_COORD_M := 1.0e7   # |координата| плоскости карты: регион — сотни км; 1e7 точно помещается во float32
const MAX_SAFE_INTEGER := 9007199254740992.0   # 2^53: целые JSON без потери точности и переполнения int
const MAX_TILE_INDEX := 1 << 20   # |номер участка|: сетка региона — десятки участков; точно влезает в Vector2i
const ORIGIN_TOLERANCE_M := 0.01
const MAX_LABELS := 100000       # подписей в участке; обзор центра — ~3 тыс.
const MAX_LABEL_CHARS := 200     # длиннейшие названия OSM — около сотни символов
const ANGLE_TOLERANCE := 1.0e-6   # π/2 во float32 чуть отличается от float64
const LABEL_TOLERANCE_M := 1.0   # допуск координат участка: считаются в float64, хранятся во float32
const MAX_LEVEL := 2   # уровни 0–2 формата 1: обзор, участки 8 и 2 км
const PREAMBLE := 16   # магия, u32 версия, u32 длина заголовка
const MAX_HEADER_BYTES := 1 << 20
const MAX_TILE_BYTES := 256 << 20      # предел файла участка; участки центра Москвы — до ~3,5 МБ
const MAX_SECTION_BYTES := 256 << 20   # предел раздела после распаковки; участок 2 км центра — ~3 МБ

# Раздел → число столбцов (1 — плоский массив). Индексные разделы проверяются по числу вершин.
const COLUMNS := {
	"area.xy": 2, "area.cls": 1, "area.tri": 3,
	"line.xy": 2, "line.off": 2, "line.cls": 1, "line.tri": 3,
	"building.xy": 2, "building.cls": 1, "building.tri": 3, "building.outline": 2,
	"pick.ring": 3, "pick.bbox": 4,
	"label.xy": 2, "label.angle": 1, "label.span": 1, "label.cls": 1, "label.weight": 1,
}
# Колонки карточки здания в pick.attrs (docs/city-package.md#участки-карты).
const PICK_COLUMNS := ["id", "name", "function", "function_source", "levels", "levels_source", "footprint_m2",
	"residents", "zone_id", "territory"]
const INT_SECTIONS := ["area.tri", "line.tri", "building.tri", "building.outline", "pick.ring"]
const JSON_SECTIONS := ["pick.attrs", "label.text"]
const PER_VERTEX := ["area.cls", "line.off", "line.cls", "building.cls",
	"label.angle", "label.span", "label.cls", "label.weight"]
const LAYERS := {
	"area": ["area.xy", "area.cls", "area.tri"],
	"line": ["line.xy", "line.off", "line.cls", "line.tri"],
	"building": ["building.xy", "building.cls", "building.tri", "building.outline"],
	"label": ["label.xy", "label.angle", "label.span", "label.cls", "label.weight", "label.text"],
}

var error := ""
var path := ""
var header: Dictionary = {}
var level := -1
var tile := Vector2i.ZERO
var origin := Vector2.ZERO   # левый верхний угол в плоскости карты, м
var tile_size_m := 0.0       # 0 — обзор региона (уровень 0)
var bbox := Rect2()          # охват геометрии в плоскости карты (из заголовка под SHA256), пустой — нет геометрии
var classes: Dictionary = {}
var counts: Dictionary = {}
var sections: Dictionary = {}
var _decoded_bytes := 0   # сумма raw_size принятых разделов   # имя → PackedFloat32Array | PackedInt32Array | данные JSON


## Прочитать и проверить участок. expected — запись индекса и пакет:
## {sha256, level, tile: Vector2i, package_id, package_version, classes}; пустые поля не проверяются.
static func open(file_path: String, expected: Dictionary = {}) -> MapTile:
	var result := MapTile.new()
	result.path = file_path
	result.error = result._read(file_path, expected)
	if not result.error.is_empty():
		result.error = "%s: %s" % [file_path.get_file(), result.error]
		result.sections.clear()
	return result


func has_layer(layer: String) -> bool:
	return sections.has(LAYERS[layer][0])


func vertex_count(layer: String) -> int:
	return (sections[LAYERS[layer][0]] as PackedFloat32Array).size() / 2


func _read(file_path: String, expected: Dictionary) -> String:
	# Размер файла проверяется до чтения в память: испорченный каталог карты не должен исчерпать память.
	var file := FileAccess.open(file_path, FileAccess.READ)
	if file == null:
		return "не удалось прочитать файл (%s)" % error_string(FileAccess.get_open_error())
	var length := file.get_length()
	file.close()
	if length > MAX_TILE_BYTES:
		return "файл %d байт, больше предела %d" % [length, MAX_TILE_BYTES]
	var want_size := int(expected.get("size_bytes", 0))
	if want_size > 0 and length != want_size:
		return "размер %d байт, а в индексе %d" % [length, want_size]
	var bytes := FileAccess.get_file_as_bytes(file_path)
	if bytes.is_empty():
		return "не удалось прочитать файл (%s)" % error_string(FileAccess.get_open_error())
	var want_sha: String = expected.get("sha256", "")
	if not want_sha.is_empty():
		var hashing := HashingContext.new()
		hashing.start(HashingContext.HASH_SHA256)
		hashing.update(bytes)
		var got := hashing.finish().hex_encode()
		if got != want_sha:
			return "SHA256 %s не совпадает с индексом (%s): файл повреждён или от другого экспорта" % [got, want_sha]
	if bytes.size() < PREAMBLE or bytes.slice(0, 8).get_string_from_ascii() != MAGIC:
		return "не файл участка карты (нет магии %s)" % MAGIC
	var version := bytes.decode_u32(8)
	if version != FORMAT_VERSION:
		return "версия формата участка %d, игра читает только %d" % [version, FORMAT_VERSION]
	var head_len := bytes.decode_u32(12)
	if head_len > MAX_HEADER_BYTES or PREAMBLE + head_len > bytes.size():
		return "заголовок длиной %d байт не помещается в файл" % head_len
	var parsed: Variant = JSON.parse_string(bytes.slice(PREAMBLE, PREAMBLE + head_len).get_string_from_utf8())
	if not parsed is Dictionary:
		return "заголовок не является объектом JSON"
	header = parsed
	if header.get("format") != FORMAT or not is_integers([header.get("format_version")], 1) \
			or int(header.format_version) != FORMAT_VERSION:
		return "заголовок: формат %s версии %s вместо %s версии %d" % [
			header.get("format"), header.get("format_version"), FORMAT, FORMAT_VERSION]
	var problem := _read_header(expected)
	if not problem.is_empty():
		return problem
	if not header.sections is Array:
		return "sections должен быть списком разделов"
	for item: Variant in header.sections:
		problem = _read_section(bytes, PREAMBLE + head_len, item)
		if not problem.is_empty():
			return problem
	return _check_layers()


func _read_header(expected: Dictionary) -> String:
	for key: String in ["level", "tile", "origin", "classes", "counts", "sections", "attribution"]:
		if not header.has(key):
			return "в заголовке нет поля %s" % key
	if not is_tile_index(header.tile) or not is_coords(header.origin, 2) or not is_integers([header.level], 1):
		return "level — целое, tile — два целых, origin — два конечных числа"
	level = int(header.level)
	if level < 0 or level > MAX_LEVEL or float(header.level) != level:
		return "уровень %s вне 0..%d" % [header.level, MAX_LEVEL]
	tile = Vector2i(int(header.tile[0]), int(header.tile[1]))
	origin = Vector2(float(header.origin[0]), float(header.origin[1]))
	var size: Variant = header.get("tile_size_m")
	if level == 0 and (tile != Vector2i.ZERO or origin != Vector2.ZERO):
		return "обзор региона должен быть участком (0, 0) с началом (0, 0)"
	if level == 0:
		if size != null:
			return "у обзора региона (уровень 0) tile_size_m должен быть null"
		tile_size_m = 0.0
	elif is_numbers([size], 1) and float(size) == LEVEL_TILE_M[level]:
		tile_size_m = float(size)
		# Начало — из номера участка; заголовок должен совпасть с ним с абсолютным допуском
		# (is_equal_approx масштабирует допуск с величиной координат и пропустил бы метры сдвига).
		var grid := Vector2(tile) * tile_size_m
		if absf(origin.x - grid.x) > ORIGIN_TOLERANCE_M or absf(origin.y - grid.y) > ORIGIN_TOLERANCE_M:
			return "начало участка %s не совпадает с его местом в сетке %s × %.0f м" % [origin, tile, tile_size_m]
		origin = grid
	else:
		return "у участка уровня %d tile_size_m %s, а формат задаёт %.0f м" % [level, size, LEVEL_TILE_M[level]]
	var box: Variant = header.get("bbox")
	if not (box == null or is_box(box)):
		return "bbox должен быть упорядоченными minx, miny, maxx, maxy или null"
	if box != null:
		bbox = Rect2(origin + Vector2(box[0], box[1]), Vector2(box[2] - box[0], box[3] - box[1]))
	if level == 0 and not bbox.has_area():
		return "у обзора региона нет охвата bbox: камере не на что опереться"
	var credits: Variant = header.attribution
	if not credits is Array or not credits.all(func(c): return c is String):
		return "attribution должна быть списком строк"
	if expected.has("attribution") and PackedStringArray(credits) != expected.attribution:
		return "атрибуция участка не совпадает с индексом: в индексе не хватает источников или они другие"
	if expected.has("bbox") and expected.bbox is Rect2 and expected.bbox.has_area() \
			and not expected.bbox.is_equal_approx(bbox):
		return "охват участка %s не совпадает с индексом %s" % [bbox, expected.bbox]
	var problem := MapTile.check_classes(header.classes)
	if not problem.is_empty():
		return problem
	classes = header.classes
	if not header.counts is Dictionary:
		return "counts должен быть словарём"
	counts = header.counts
	if expected.has("level") and int(expected.level) != level:
		return "уровень %d, а в индексе %d" % [level, expected.level]
	if expected.has("tile") and expected.tile != tile:
		return "участок %s, а в индексе %s" % [tile, expected.tile]
	for key: String in ["package_id", "package_version"]:
		if expected.has(key) and header.get(key) != expected[key]:
			return "%s «%s», а в индексе «%s»" % [key, header.get(key), expected[key]]
	if expected.has("classes") and expected.classes != classes:
		return "классы объектов не совпадают с индексом"
	return ""


func _read_section(bytes: PackedByteArray, data_start: int, item: Variant) -> String:
	# Описание раздела проверяется целиком до распаковки: размер после распаковки берётся из файла,
	# и без проверки маленький испорченный участок мог бы запросить гигабайты памяти.
	if not item is Dictionary or not item.get("name") is String or not item.get("dtype") is String \
			or not is_integers([item.get("offset"), item.get("size"), item.get("raw_size")], 3):
		return "описание раздела: нужны строки name, dtype и числа offset, size, raw_size"
	var name: String = item.name
	var dtype: String = item.dtype
	var offset := int(item.offset)
	var size := int(item.size)
	var raw_size := int(item.raw_size)
	# Без offset + size: сумма больших целых переполнилась бы; сравниваем с остатком файла.
	if offset < data_start or offset > bytes.size() or size < 0 or raw_size < 0 or offset % 4 != 0 \
			or size > bytes.size() - offset:
		return "раздел %s выходит за пределы файла (смещение %d, размер %d из %d)" % [name, offset, size, bytes.size()]
	if raw_size > MAX_SECTION_BYTES:
		return "раздел %s: %d байт после распаковки, больше предела %d" % [name, raw_size, MAX_SECTION_BYTES]
	_decoded_bytes += raw_size
	if _decoded_bytes > MAX_DECODED_BYTES:
		return "данные участка после распаковки больше предела %d байт" % MAX_DECODED_BYTES
	var required := _required_dtype(name)
	if required.is_empty():
		return "неизвестный раздел %s" % name
	if dtype != required:
		return "раздел %s: тип %s вместо %s" % [name, dtype, required]
	if dtype != "json":
		var count: Variant = item.get("count")
		var shape: Variant = item.get("shape")
		if not is_integers([count], 1) or not shape is Array or not is_integers(shape, shape.size()):
			return "раздел %s: count и shape должны быть целыми" % name
		var columns: int = COLUMNS[name]
		var expected_shape := [int(count)] if columns == 1 else [int(count) / columns, columns]
		if int(count) * 4 != raw_size or int(count) % columns != 0 or shape.map(func(v): return int(v)) != expected_shape:
			return "раздел %s: форма %s и %d значений не согласуются (%d байт)" % [name, shape, int(count), raw_size]
	var raw := bytes.slice(offset, offset + size)
	match item.get("codec"):
		"none":
			pass
		"deflate":
			raw = raw.decompress(raw_size, FileAccess.COMPRESSION_DEFLATE) if raw_size > 0 else PackedByteArray()
		_:
			return "раздел %s: неизвестное сжатие %s" % [name, item.get("codec")]
	if raw.size() != raw_size:
		return "раздел %s: после распаковки %d байт вместо %d" % [name, raw.size(), raw_size]
	if dtype == "json":
		var json := JSON.new()
		if json.parse(raw.get_string_from_utf8()) != OK:
			return "раздел %s: некорректный JSON (%s)" % [name, json.get_error_message()]
		sections[name] = json.data
		return ""
	if dtype == "f32" and not _all_finite(raw):
		return "раздел %s: нечисловые значения (NaN или бесконечность)" % name
	sections[name] = raw.to_float32_array() if dtype == "f32" else raw.to_int32_array()
	return ""


func _check_layers() -> String:
	for layer: String in LAYERS:
		var names: Array = LAYERS[layer]
		var present := names.filter(func(n): return sections.has(n))
		if present.is_empty():
			continue
		if present.size() != names.size():
			return "слой %s неполон: есть только %s" % [layer, present]
		var vertices := vertex_count(layer)
		for name: String in PER_VERTEX.filter(func(n): return n.begins_with(layer + ".")):
			var values: PackedFloat32Array = sections[name]
			if values.size() != vertices * COLUMNS[name]:
				return "раздел %s: %d значений на %d вершин" % [name, values.size(), vertices]
		if layer == "label" and not (sections["label.text"] is Array and sections["label.text"].size() == vertices
				and sections["label.text"].all(func(t): return t is String and not t.strip_edges().is_empty())):
			return "раздел label.text: нужен список непустых строк по одной на подпись label.xy"
		if layer == "label" and (vertices > MAX_LABELS or sections["label.text"].any(func(t): return t.length() > MAX_LABEL_CHARS)):
			return "подписей %d (предел %d) или подпись длиннее %d символов" % [vertices, MAX_LABELS, MAX_LABEL_CHARS]
		if layer == "label":
			var problem := _check_label_anchors()
			if not problem.is_empty():
				return problem
		for name: String in names:
			if sections[name] is PackedInt32Array and not _indices_within(sections[name], vertices):
				return "раздел %s ссылается на вершину вне 0..%d" % [name, vertices - 1]
		var class_list: Array = classes.get(layer, [])
		var cls_name: String = layer + ".cls"
		if not _codes_within(sections[cls_name], class_list.size()):
			return "раздел %s: код класса вне списка из %d классов" % [cls_name, class_list.size()]
	# Площади и линии подробного участка обрезаны по его квадрату; выступать за край могут только здания.
	if level > 0:
		for name: String in ["area.xy", "line.xy"]:
			if sections.has(name) and not _within_range(sections[name], -LABEL_TOLERANCE_M, tile_size_m + LABEL_TOLERANCE_M):
				return "%s: вершины вне квадрата участка %.0f м" % [name, tile_size_m]
	if has_layer("line") and not _within(sections["line.off"], MapLineStyle.MAX_OFFSET):
		return "line.off: смещение ленты больше %.0f полуширин — шейдер вынес бы её за границы отсечения" % MapLineStyle.MAX_OFFSET
	if level == 0 and has_layer("building"):
		return "в обзоре региона не должно быть зданий"
	return _check_pick()


## Подпись принадлежит участку, где лежит её точка (у обзора — внутри охвата), а длина прямого
## участка под текстом не отрицательна: иначе подпись встала бы у соседа или обошла бы проверку длины.
func _check_label_anchors() -> String:
	var span: PackedFloat32Array = sections["label.span"]
	if not span.is_empty() and _min(span) < 0.0:
		return "label.span: длина прямого участка не может быть отрицательной"
	var angle: PackedFloat32Array = sections["label.angle"]
	if not angle.is_empty() and not (_min(angle) > -PI / 2.0 - ANGLE_TOLERANCE and _max(angle) <= PI / 2.0 + ANGLE_TOLERANCE):
		return "label.angle: угол вне (−π/2, π/2] — текст лёг бы вверх ногами"
	var area := Rect2(Vector2.ZERO, Vector2.ONE * tile_size_m) if level > 0 else Rect2(bbox.position - origin, bbox.size)
	area = area.grow(LABEL_TOLERANCE_M)
	var xy: PackedFloat32Array = sections["label.xy"]
	for i in range(0, xy.size(), 2):
		if xy[i] < area.position.x or xy[i] > area.end.x or xy[i + 1] < area.position.y or xy[i + 1] > area.end.y:
			return "label.xy: подпись %d в (%.0f, %.0f) вне своего участка %s" % [i / 2, xy[i], xy[i + 1], area]
	return ""


static func _max(values: PackedFloat32Array) -> float:
	var sorted := values.duplicate()
	sorted.sort()
	return sorted[-1]


static func _min(values: PackedFloat32Array) -> float:
	var sorted := values.duplicate()
	sorted.sort()
	return sorted[0]


## Данные выбора зданий (уровень 2): кольцо ссылается на строку здания и на вершины building.xy,
## прямоугольники и колонки карточки — по строке на здание.
func _check_pick() -> String:
	var names := ["pick.ring", "pick.bbox", "pick.attrs"]
	var present := names.filter(func(n): return sections.has(n))
	if present.is_empty():
		return ""
	if present.size() != names.size() or not has_layer("building"):
		return "данные выбора неполны или без зданий: %s" % [present]
	var buildings := (sections["pick.bbox"] as PackedFloat32Array).size() / 4
	var attrs: Variant = sections["pick.attrs"]
	if not attrs is Dictionary or not attrs.has_all(PICK_COLUMNS):
		return "pick.attrs: нужны колонки %s" % [PICK_COLUMNS]
	if not attrs.values().all(func(c): return c is Array and c.size() == buildings):
		return "pick.attrs: каждая колонка — список из %d значений" % buildings
	var rings: PackedInt32Array = sections["pick.ring"]
	var vertices := vertex_count("building")
	for i in range(0, rings.size(), 3):
		var row := rings[i]
		var start := rings[i + 1]
		var count := rings[i + 2]
		if row < 0 or row >= buildings or start < 0 or count < 3 or start + count > vertices:
			return "pick.ring: кольцо %d ссылается на здание %d или вершины %d..%d вне данных" % [
				i / 3, row, start, start + count - 1]
	return ""


## Все float32 конечны. Проверка по битам: как int32 значения с экспонентой из одних единиц
## (NaN и ±∞) лежат в [0x7F800000, 0x7FFFFFFF] и [-0x800000, -1]. Нативная сортировка целых
## и двоичный поиск быстрее обхода массива в GDScript, а сравнение целых, в отличие от NaN, корректно.
static func _all_finite(raw: PackedByteArray) -> bool:
	var bits := raw.to_int32_array()
	if bits.is_empty():
		return true
	bits.sort()
	if bits[-1] >= 0x7F800000:
		return false
	var first_negative_special := bits.bsearch(-0x800000)
	return first_negative_special >= bits.size() or bits[first_negative_special] >= 0


## Тип раздела по имени: индексы — i32, данные карточки — json, остальное — f32; "" — неизвестный раздел.
static func _required_dtype(name: String) -> String:
	if name in JSON_SECTIONS:
		return "json"
	if name in INT_SECTIONS:
		return "i32"
	return "f32" if COLUMNS.has(name) else ""


## Таблица классов: словарь, где каждый слой — список строк; нужны слои area и line.
static func check_classes(value: Variant) -> String:
	if not value is Dictionary or not value.has_all(["area", "line"]):
		return "classes должен быть словарём со слоями area и line"
	for layer: Variant in value:
		var names: Variant = value[layer]
		if not names is Array or not names.all(func(v): return v is String):
			return "classes.%s должен быть списком строк" % layer
	return ""


## Значение — массив из count целых чисел (JSON отдаёт числа как float).
static func is_integers(value: Variant, count: int) -> bool:
	return is_numbers(value, count) and value.all(func(v): return is_finite(float(v)) and float(v) == floorf(float(v)) \
		and absf(float(v)) <= MAX_SAFE_INTEGER)


## Значение — массив из count конечных чисел.
static func is_finite_numbers(value: Variant, count: int) -> bool:
	return is_numbers(value, count) and value.all(func(v): return is_finite(float(v)))


## Номер участка [ix, iy]: два целых не больше MAX_TILE_INDEX по модулю — без сужения в Vector2i.
static func is_tile_index(value: Variant) -> bool:
	return is_integers(value, 2) and value.all(func(v): return absf(float(v)) <= MAX_TILE_INDEX)


## Прямоугольник [minx, miny, maxx, maxy]: координаты плоскости (is_coords), min не больше max.
static func is_box(value: Variant) -> bool:
	return is_coords(value, 4) and value[0] <= value[2] and value[1] <= value[3]


## Значение — массив из count координат плоскости карты: конечные и по модулю не больше MAX_COORD_M,
## чтобы не переполнить 32-битный Vector2.
static func is_coords(value: Variant, count: int) -> bool:
	return is_finite_numbers(value, count) and value.all(func(v): return absf(float(v)) <= MAX_COORD_M)


## Значение — массив из count чисел (защита от искажённого JSON до обращения по индексу).
static func is_numbers(value: Variant, count: int) -> bool:
	if not value is Array or value.size() != count:
		return false
	return value.all(func(v): return typeof(v) == TYPE_FLOAT or typeof(v) == TYPE_INT)


static func _indices_within(indices: PackedInt32Array, vertices: int) -> bool:
	if indices.is_empty():
		return true
	# Сортировка копии — нативная и быстрее обхода в GDScript; нужен только минимум и максимум.
	var sorted := indices.duplicate()
	sorted.sort()
	return sorted[0] >= 0 and sorted[-1] < vertices


## Все значения в [low, high] (значения уже проверены на конечность).
static func _within_range(values: PackedFloat32Array, low: float, high: float) -> bool:
	if values.is_empty():
		return true
	var sorted := values.duplicate()
	sorted.sort()
	return sorted[0] >= low and sorted[-1] <= high


## Все значения в [-limit, limit] (значения уже проверены на конечность).
static func _within(values: PackedFloat32Array, limit: float) -> bool:
	if values.is_empty():
		return true
	var sorted := values.duplicate()
	sorted.sort()
	return sorted[0] >= -limit and sorted[-1] <= limit


static func _codes_within(codes: PackedFloat32Array, class_count: int) -> bool:
	if codes.is_empty():
		return true
	var sorted := codes.duplicate()
	sorted.sort()
	if not (sorted[0] >= 0.0 and sorted[-1] < class_count):
		return false
	# Коды — позиции в списке классов: только целые. Обходим лишь различные значения (двоичный поиск).
	var i := 0
	while i < sorted.size():
		var value := sorted[i]
		if not is_finite(value) or value != floorf(value):
			return false
		i = sorted.bsearch(value, false)
	return true

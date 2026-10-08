class_name MapTile
extends RefCounted

# Участок карты `.mtile`, формат 1 (docs/city-package.md#участки-карты, решение #9).
# Только данные: заголовок и разделы-массивы. Меши строит MapTileView.
# Ошибка формата не роняет игру: open() возвращает участок с непустым `error`.

const MAGIC := "MESMTILE"
const FORMAT := "mesim-map-tile"
const FORMAT_VERSION := 1
const PREAMBLE := 16   # магия, u32 версия, u32 длина заголовка
const MAX_HEADER_BYTES := 1 << 20

# Раздел → число столбцов (1 — плоский массив). Индексные разделы проверяются по числу вершин.
const COLUMNS := {
	"area.xy": 2, "area.cls": 1, "area.tri": 3,
	"line.xy": 2, "line.off": 2, "line.cls": 1, "line.tri": 3,
	"building.xy": 2, "building.cls": 1, "building.tri": 3, "building.outline": 2,
	"pick.ring": 3, "pick.bbox": 4,
}
const INT_SECTIONS := ["area.tri", "line.tri", "building.tri", "building.outline", "pick.ring"]
const JSON_SECTIONS := ["pick.attrs"]
const PER_VERTEX := ["area.cls", "line.off", "line.cls", "building.cls"]
const LAYERS := {
	"area": ["area.xy", "area.cls", "area.tri"],
	"line": ["line.xy", "line.off", "line.cls", "line.tri"],
	"building": ["building.xy", "building.cls", "building.tri", "building.outline"],
}

var error := ""
var path := ""
var header: Dictionary = {}
var level := -1
var tile := Vector2i.ZERO
var origin := Vector2.ZERO   # левый верхний угол в плоскости карты, м
var tile_size_m := 0.0       # 0 — обзор региона (уровень 0)
var classes: Dictionary = {}
var counts: Dictionary = {}
var sections: Dictionary = {}   # имя → PackedFloat32Array | PackedInt32Array | данные JSON


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
	if header.get("format") != FORMAT or int(header.get("format_version", -1)) != FORMAT_VERSION:
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
	if not is_numbers(header.tile, 2) or not is_numbers(header.origin, 2) or not is_numbers([header.level], 1):
		return "поля level, tile и origin должны быть числами (tile и origin — по два)"
	level = int(header.level)
	tile = Vector2i(int(header.tile[0]), int(header.tile[1]))
	origin = Vector2(float(header.origin[0]), float(header.origin[1]))
	var size: Variant = header.get("tile_size_m")
	if level == 0:
		if size != null:
			return "у обзора региона (уровень 0) tile_size_m должен быть null"
		tile_size_m = 0.0
	elif is_numbers([size], 1) and is_finite(float(size)) and float(size) > 0.0:
		tile_size_m = float(size)
	else:
		return "у участка уровня %d нет положительного tile_size_m" % level
	classes = header.classes
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
	if not item is Dictionary:
		return "описание раздела не является объектом"
	var name: String = item.get("name", "")
	var dtype: String = item.get("dtype", "")
	var offset := int(item.get("offset", -1))
	var size := int(item.get("size", -1))
	var raw_size := int(item.get("raw_size", -1))
	if offset < data_start or size < 0 or raw_size < 0 or offset % 4 != 0 or offset + size > bytes.size():
		return "раздел %s выходит за пределы файла (смещение %d, размер %d из %d)" % [name, offset, size, bytes.size()]
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
	var required := _required_dtype(name)
	if required.is_empty():
		return "неизвестный раздел %s" % name
	if dtype != required:
		return "раздел %s: тип %s вместо %s" % [name, dtype, required]
	if dtype == "json":
		var json := JSON.new()
		if json.parse(raw.get_string_from_utf8()) != OK:
			return "раздел %s: некорректный JSON (%s)" % [name, json.get_error_message()]
		sections[name] = json.data
		return ""
	var count := int(item.get("count", -1))
	var shape: Variant = item.get("shape", [])
	if not shape is Array or not is_numbers(shape, shape.size()):
		return "раздел %s: форма не является списком чисел" % name
	var columns: int = COLUMNS[name]
	var expected_shape := [count] if columns == 1 else [count / columns, columns]
	if count * 4 != raw_size or count % columns != 0 or shape.map(func(v): return int(v)) != expected_shape:
		return "раздел %s: форма %s и %d значений не согласуются (%d байт)" % [name, shape, count, raw_size]
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
		for name: String in names:
			if sections[name] is PackedInt32Array and not _indices_within(sections[name], vertices):
				return "раздел %s ссылается на вершину вне 0..%d" % [name, vertices - 1]
		var class_list: Array = classes.get(layer, [])
		var cls_name: String = layer + ".cls"
		if not _codes_within(sections[cls_name], class_list.size()):
			return "раздел %s: код класса вне списка из %d классов" % [cls_name, class_list.size()]
	if level == 0 and has_layer("building"):
		return "в обзоре региона не должно быть зданий"
	return ""


## Тип раздела по имени: индексы — i32, данные карточки — json, остальное — f32; "" — неизвестный раздел.
static func _required_dtype(name: String) -> String:
	if name in JSON_SECTIONS:
		return "json"
	if name in INT_SECTIONS:
		return "i32"
	return "f32" if COLUMNS.has(name) else ""


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


static func _codes_within(codes: PackedFloat32Array, class_count: int) -> bool:
	if codes.is_empty():
		return true
	var sorted := codes.duplicate()
	sorted.sort()
	return sorted[0] >= 0.0 and sorted[-1] < class_count

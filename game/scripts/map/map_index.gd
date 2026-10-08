class_name MapIndex
extends RefCounted

# Индекс набора участков `index.json` (docs/city-package.md#участки-карты): пакет, классы,
# атрибуция и список участков с SHA256. Участки читаются по требованию через load_tile().

const FORMAT := "mesim-map-index"
const FORMAT_VERSION := 1
const MAX_INDEX_BYTES := 64 << 20   # индекс всего региона — десятки тысяч записей, единицы МБ
const OSM_CREDIT := "© участники OpenStreetMap, ODbL"   # обязательная первая строка атрибуции (ODbL)

var error := ""
var dir := ""
var package_id := ""
var package_version := ""
var attribution := PackedStringArray()
var classes: Dictionary = {}
var tiles: Array[Dictionary] = []   # {level, tile: Vector2i, path, sha256, size_bytes, bbox: Rect2}


static func open(map_dir: String) -> MapIndex:
	var result := MapIndex.new()
	result.dir = map_dir
	result.error = result._read(map_dir.path_join("index.json"))
	if not result.error.is_empty():
		result.error = "Индекс карты %s: %s" % [map_dir.path_join("index.json"), result.error]
		result.tiles.clear()
	return result


func tiles_of_level(level: int) -> Array[Dictionary]:
	return tiles.filter(func(entry): return entry.level == level)


## Загрузить участок из записи индекса с проверкой SHA256, пакета и классов.
func load_tile(entry: Dictionary) -> MapTile:
	return MapTile.open(dir.path_join(entry.path), {
		"sha256": entry.sha256, "level": entry.level, "tile": entry.tile, "bbox": entry.bbox,
		"size_bytes": entry.size_bytes,
		"package_id": package_id, "package_version": package_version, "classes": classes,
	})


func _read(index_path: String) -> String:
	if not FileAccess.file_exists(index_path):
		return "файл не найден"
	var file := FileAccess.open(index_path, FileAccess.READ)
	if file == null:
		return "не удалось прочитать (%s)" % error_string(FileAccess.get_open_error())
	if file.get_length() > MAX_INDEX_BYTES:
		return "файл %d байт, больше предела %d" % [file.get_length(), MAX_INDEX_BYTES]
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if not parsed is Dictionary:
		return "не является объектом JSON"
	var doc: Dictionary = parsed
	if doc.get("format") != FORMAT or not MapTile.is_integers([doc.get("format_version")], 1) \
			or int(doc.format_version) != FORMAT_VERSION:
		return "формат %s версии %s, игра читает только %s версии %d" % [
			doc.get("format"), doc.get("format_version"), FORMAT, FORMAT_VERSION]
	for key: String in ["package_id", "package_version", "attribution", "classes", "tiles"]:
		if not doc.has(key):
			return "нет поля %s" % key
	if not doc.package_id is String or not doc.package_version is String:
		return "package_id и package_version должны быть строками"
	package_id = doc.package_id
	package_version = doc.package_version
	if not doc.attribution is Array or not doc.attribution.all(func(v): return v is String):
		return "attribution должна быть списком строк"
	if not doc.tiles is Array:
		return "tiles должен быть списком участков"
	attribution = PackedStringArray(doc.attribution)
	var problem := MapTile.check_classes(doc.classes)
	if not problem.is_empty():
		return problem
	classes = doc.classes
	if attribution.is_empty() or attribution[0] != OSM_CREDIT:
		return "первая строка атрибуции должна быть «%s» (ODbL)" % OSM_CREDIT
	var seen := {}
	for item: Variant in doc.tiles:
		if not item is Dictionary or not item.has_all(["level", "tile", "path", "sha256"]):
			return "запись участка без level, tile, path или sha256"
		if not item.path is String:
			return "путь участка не строка"
		var rel_path: String = item.path
		# Только относительный путь с «/»: обратная косая черта и «:» на Windows стали бы разделителем и диском.
		if rel_path.is_empty() or rel_path.is_absolute_path() or rel_path.begins_with("/") or "\\" in rel_path \
				or ":" in rel_path or ".." in rel_path.split("/"):
			return "путь участка %s выходит за каталог карты" % rel_path
		var bbox: Variant = item.get("bbox")
		if not MapTile.is_integers([item.level], 1) or int(item.level) < 0 or int(item.level) > MapTile.MAX_LEVEL \
				or not MapTile.is_integers(item.tile, 2) or not (bbox == null or MapTile.is_box(bbox)) \
				or not item.sha256 is String \
				or item.sha256.length() != 64 or not item.sha256.is_valid_hex_number():
			return "запись участка %s: level — целое 0..2, tile — два целых, bbox — упорядоченные minx, miny, maxx, maxy или null, sha256 — 64 шестнадцатеричных символа" % rel_path
		var size_bytes: Variant = item.get("size_bytes", 0)
		if not MapTile.is_integers([size_bytes], 1) or int(size_bytes) < 0:
			return "запись участка %s: size_bytes — целое неотрицательное" % rel_path
		var identity := Vector3i(int(item.level), int(item.tile[0]), int(item.tile[1]))
		if identity in seen:
			return "участок z%d %d_%d указан в индексе дважды" % [identity.x, identity.y, identity.z]
		seen[identity] = true
		tiles.append({
			"level": int(item.level),
			"tile": Vector2i(int(item.tile[0]), int(item.tile[1])),
			"path": rel_path,
			"sha256": String(item.sha256),
			"size_bytes": int(size_bytes),
			"bbox": Rect2() if bbox == null else Rect2(bbox[0], bbox[1], bbox[2] - bbox[0], bbox[3] - bbox[1]),
		})
	var overview := tiles_of_level(0)
	if overview.size() != 1 or overview[0].tile != Vector2i.ZERO:
		return "нужен ровно один обзор региона — участок уровня 0 с номером (0, 0)"
	return ""

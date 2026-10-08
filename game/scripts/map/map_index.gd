class_name MapIndex
extends RefCounted

# Индекс набора участков `index.json` (docs/city-package.md#участки-карты): пакет, классы,
# атрибуция и список участков с SHA256. Участки читаются по требованию через load_tile().

const FORMAT := "mesim-map-index"
const FORMAT_VERSION := 1
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
		"sha256": entry.sha256, "level": entry.level, "tile": entry.tile,
		"package_id": package_id, "package_version": package_version, "classes": classes,
	})


func _read(index_path: String) -> String:
	if not FileAccess.file_exists(index_path):
		return "файл не найден"
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(index_path))
	if not parsed is Dictionary:
		return "не является объектом JSON"
	var doc: Dictionary = parsed
	if doc.get("format") != FORMAT or int(doc.get("format_version", -1)) != FORMAT_VERSION:
		return "формат %s версии %s, игра читает только %s версии %d" % [
			doc.get("format"), doc.get("format_version"), FORMAT, FORMAT_VERSION]
	for key: String in ["package_id", "package_version", "attribution", "classes", "tiles"]:
		if not doc.has(key):
			return "нет поля %s" % key
	package_id = doc.package_id
	package_version = doc.package_version
	if not doc.attribution is Array or not doc.attribution.all(func(v): return v is String):
		return "attribution должна быть списком строк"
	if not doc.tiles is Array:
		return "tiles должен быть списком участков"
	attribution = PackedStringArray(doc.attribution)
	classes = doc.classes
	if attribution.is_empty() or attribution[0] != OSM_CREDIT:
		return "первая строка атрибуции должна быть «%s» (ODbL)" % OSM_CREDIT
	for item: Variant in doc.tiles:
		if not item is Dictionary or not item.has_all(["level", "tile", "path", "sha256"]):
			return "запись участка без level, tile, path или sha256"
		if not item.path is String:
			return "путь участка не строка"
		var rel_path: String = item.path
		if rel_path.is_absolute_path() or rel_path.begins_with("/") or ".." in rel_path.split("/"):
			return "путь участка %s выходит за каталог карты" % rel_path
		var bbox: Variant = item.get("bbox")
		if not MapTile.is_numbers([item.level], 1) or not MapTile.is_numbers(item.tile, 2) \
				or not (bbox == null or MapTile.is_numbers(bbox, 4)) or not item.sha256 is String:
			return "запись участка %s: level — число, tile — два числа, bbox — четыре числа или null, sha256 — строка" % rel_path
		tiles.append({
			"level": int(item.level),
			"tile": Vector2i(int(item.tile[0]), int(item.tile[1])),
			"path": rel_path,
			"sha256": String(item.sha256),
			"size_bytes": int(item.get("size_bytes", 0)),
			"bbox": Rect2() if bbox == null else Rect2(bbox[0], bbox[1], bbox[2] - bbox[0], bbox[3] - bbox[1]),
		})
	if tiles_of_level(0).is_empty():
		return "нет обзора региона (участка уровня 0)"
	return ""

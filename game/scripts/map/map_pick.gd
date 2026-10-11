class_name MapPick
extends RefCounted

# Данные выбора зданий одного участка 2 км (#15): внешние кольца (`pick.ring`), прямоугольники
# (`pick.bbox`) и колонки карточки (`pick.attrs`). Вершины колец — те же, что у меша зданий
# (PackedVector2Array копируется при записи, поэтому общий массив не занимает память дважды).
# Создаётся в рабочем потоке вместе с массивами мешей (MapTileView.prepare).
#
# Правило попадания: точка внутри внешнего кольца здания; внутренний двор считается частью здания
# (в участке хранятся только внешние кольца). Если точка внутри нескольких зданий (наложенные
# контуры OSM), выбирается здание с наименьшей площадью внешних колец, при равенстве — с меньшим ID.

var origin := Vector2.ZERO   # угол участка в плоскости карты
var bounds := Rect2()        # охват всех зданий участка в плоскости карты (здания выступают за край)
var bytes := 0               # оценка памяти: кольца, прямоугольники и колонки карточки
var _xy: PackedVector2Array  # вершины колец в координатах участка
var _rings: PackedInt32Array # (строка здания, первая вершина, число вершин)
var _bbox: PackedFloat32Array
var _attrs: Dictionary
var _rings_of: Dictionary = {}   # строка здания → номера колец в _rings


## Данные выбора участка; null — в участке их нет (уровни 0 и 1). xy — вершины building.xy.
static func from_tile(map_tile: MapTile, xy: PackedVector2Array) -> MapPick:
	if not map_tile.sections.has("pick.attrs"):
		return null
	var pick := MapPick.new()
	pick.origin = map_tile.origin
	pick._xy = xy
	pick._rings = map_tile.sections["pick.ring"]
	pick._bbox = map_tile.sections["pick.bbox"]
	pick._attrs = map_tile.sections["pick.attrs"]
	for i in pick._rings.size() / 3:
		var row := pick._rings[i * 3]
		if not pick._rings_of.has(row):
			pick._rings_of[row] = PackedInt32Array()
		pick._rings_of[row].append(i)
	var box := Rect2()
	for row in pick.count():
		box = pick._row_rect(row) if row == 0 else box.merge(pick._row_rect(row))
	pick.bounds = Rect2(box.position + pick.origin, box.size)
	pick.bytes = pick._rings.size() * 4 + pick._bbox.size() * 4 + section_raw_size(map_tile, "pick.attrs")
	return pick


## Размер раздела после распаковки по заголовку участка (0 — раздела нет).
static func section_raw_size(map_tile: MapTile, name: String) -> int:
	for item: Dictionary in map_tile.header.get("sections", []):
		if item.get("name") == name:
			return int(item.raw_size)
	return 0


func count() -> int:
	return _bbox.size() / 4


## Строка здания, внутри внешнего кольца которого лежит точка плоскости карты; -1 — ни одного.
## При наложении — наименьшая площадь, затем меньший ID (см. правило в начале файла).
func building_at(map_point: Vector2) -> int:
	if not bounds.has_point(map_point):
		return -1
	var local := map_point - origin
	var best := -1
	var best_area := INF
	for row in count():
		if not _row_rect(row).has_point(local) or not _contains(row, local):
			continue
		var area := area_of(row)
		if best < 0 or area < best_area or (area == best_area and building_id(row) < building_id(best)):
			best = row
			best_area = area
	return best


## Площадь внешних колец здания, м² (по вершинам участка; двор не вычитается).
func area_of(row: int) -> float:
	var total := 0.0
	for ring in _rings_of.get(row, PackedInt32Array()):
		total += absf(_signed_area(_ring_points(ring)))
	return total


func building_id(row: int) -> String:
	return str(_attrs.id[row])


## Значения карточки здания: колонки pick.attrs как есть (null — нет данных). Колонки facilities
## в участках пакетов до 0.3.0 нет — тогда её нет и в словаре.
func attributes(row: int) -> Dictionary:
	var result := {}
	for column: String in _attrs:
		result[column] = _attrs[column][row]
	return result


## Внешние кольца здания в плоскости карты (копии: выбор переживает выгрузку участка).
func outlines(row: int) -> Array[PackedVector2Array]:
	var result: Array[PackedVector2Array] = []
	for ring in _rings_of.get(row, PackedInt32Array()):
		var points := _ring_points(ring)
		for i in points.size():
			points[i] += origin
		result.append(points)
	return result


func _contains(row: int, local: Vector2) -> bool:
	for ring in _rings_of.get(row, PackedInt32Array()):
		if Geometry2D.is_point_in_polygon(local, _ring_points(ring)):
			return true
	return false


func _ring_points(ring: int) -> PackedVector2Array:
	var start := _rings[ring * 3 + 1]
	return _xy.slice(start, start + _rings[ring * 3 + 2])


func _row_rect(row: int) -> Rect2:
	var i := row * 4
	return Rect2(_bbox[i], _bbox[i + 1], _bbox[i + 2] - _bbox[i], _bbox[i + 3] - _bbox[i + 1])


static func _signed_area(points: PackedVector2Array) -> float:
	var total := 0.0
	for i in points.size():
		total += points[i].cross(points[(i + 1) % points.size()])
	return total / 2.0

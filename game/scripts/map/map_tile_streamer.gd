class_name MapTileStreamer
extends RefCounted

# Подгрузка подробных участков по камере (#14). Участки уровней 1–2 читаются и готовятся
# (проверка SHA256, распаковка, массивы мешей) в рабочих потоках WorkerThreadPool; узлы создаёт
# основной поток в poll() — не больше бюджета времени за кадр. Загруженные участки — кеш с
# вытеснением давно не нужных (LRU). Память кеша (MapTileView.data_bytes) не превышает лимит:
# участок, которому нет места даже после вытеснения всех ненужных, не подключается.
# Обзор региона (уровень 0) в кеш не входит: он загружен всегда.

const DEFAULT_LIMIT_MB := 256      # игровая настройка; --map-cache-mb=<МБ> в командной строке
const MAX_IN_FLIGHT := 4           # участков в рабочих потоках одновременно
const ATTACH_BUDGET_USEC := 4000   # основной поток подключает участки, пока не потратит столько за кадр
const PREFETCH := 0.25             # запас вокруг окна в долях его размера: участки у края грузятся заранее
const MAX_POLL_SAMPLES := 100000   # замеров poll() хранится не больше
const INITIAL_RATIO := 4.0         # оценка памяти участка до загрузки: размер файла × коэффициент (moscow-2021 — 3,2–3,7)

var limit_bytes := DEFAULT_LIMIT_MB << 20
var used_bytes := 0              # память загруженных участков
var peak_bytes := 0              # наибольшая память кеша с начала работы
var loaded_total := 0            # участков подключено с начала работы
var evicted_total := 0           # участков выгружено
var errors := PackedStringArray()
var max_attach_usec := 0         # самое долгое подключение участка в основном потоке
var max_poll_usec := 0           # самая долгая работа poll() за кадр (подключение и вытеснение)
var poll_usec := PackedInt32Array()   # время каждого poll(), в котором что-то подключалось (для замеров)

var _index: MapIndex
var _palette: PackedColorArray
var _attach: Callable            # (MapTile, prepared: Dictionary) -> MapTileView или текст ошибки
var _detach: Callable            # (key: Vector3i) -> void
var _entries: Dictionary = {}    # Vector3i(уровень, x, y) → запись индекса
var _loaded: Dictionary = {}     # ключ → {bytes, used: номер обращения}
var _jobs: Dictionary = {}       # ключ → Job в рабочем потоке
var _ready: Dictionary = {}      # ключ → Job, готовый к подключению в основном потоке
var _failed: Dictionary = {}     # ключ → текст ошибки; повторно не грузится
var _rejected: Dictionary = {}   # ключ → true: не поместился в лимит при текущем наборе нужных
var _wanted: Array[Vector3i] = []   # нужные участки по убыванию важности
var _wanted_set: Dictionary = {}
var _tick := 0
var _ratio := INITIAL_RATIO


class Job extends RefCounted:
	var entry: Dictionary
	var task := -1
	var tile: MapTile
	var prepared: Dictionary


func _init(index: MapIndex, palette: PackedColorArray, limit: int, attach: Callable, detach: Callable) -> void:
	_index = index
	_palette = palette
	limit_bytes = limit
	_attach = attach
	_detach = detach
	for entry: Dictionary in index.tiles:
		if entry.level > 0:
			_entries[Vector3i(entry.level, entry.tile.x, entry.tile.y)] = entry


## Участки уровня level, задевающие прямоугольник плоскости карты, — от центра к краям. Участков нет
## в индексе — они вне региона.
func tiles_in(rect: Rect2, level: int) -> Array[Vector3i]:
	var result: Array[Vector3i] = []
	if level <= 0:
		return result
	var size: float = MapTile.LEVEL_TILE_M[level]
	var center := rect.get_center()
	var low := Vector2i(floori(rect.position.x / size), floori(rect.position.y / size))
	var high := Vector2i(floori(rect.end.x / size), floori(rect.end.y / size))
	for y in range(low.y, high.y + 1):
		for x in range(low.x, high.x + 1):
			var key := Vector3i(level, x, y)
			if _entries.has(key):
				result.append(key)
	result.sort_custom(func(a: Vector3i, b: Vector3i) -> bool:
		var da := (Vector2(a.y, a.z) + Vector2(0.5, 0.5)) * size - center
		var db := (Vector2(b.y, b.z) + Vector2(0.5, 0.5)) * size - center
		var la := da.length_squared()
		var lb := db.length_squared()
		return la < lb if la != lb else a < b)
	return result


## Задать нужные участки (по убыванию важности). Остальные загруженные остаются в кеше до вытеснения.
func want(keys: Array[Vector3i]) -> void:
	if keys == _wanted:
		return
	_wanted = keys.duplicate()
	_wanted_set.clear()
	for key in _wanted:
		_wanted_set[key] = true
	_rejected.clear()
	_tick += 1
	for key in _wanted:
		if _loaded.has(key):
			_loaded[key].used = _tick


func is_loaded(key: Vector3i) -> bool:
	return _loaded.has(key)


func is_wanted(key: Vector3i) -> bool:
	return _wanted_set.has(key)


func loaded_keys() -> Array:
	return _loaded.keys()


func in_flight() -> int:
	return _jobs.size()


## Нужные участки, которые ещё могут подключиться: грузятся или ждут очереди.
func pending() -> int:
	return _wanted.filter(func(key): return not _loaded.has(key) and not _failed.has(key) and not _rejected.has(key)).size()


## Нужные участки, которые ещё не подключены (грузятся, ждут очереди или не поместились).
func missing() -> Array[Vector3i]:
	return _wanted.filter(func(key): return not _loaded.has(key) and not _failed.has(key))


## Принять готовые участки и запустить новые загрузки. sync — всё сразу в этом потоке (тесты).
## Возвращает true, если набор подключённых участков изменился.
func poll(sync := false) -> bool:
	var changed := false
	var started := Time.get_ticks_usec()
	for key: Vector3i in _jobs.keys():
		var job: Job = _jobs[key]
		if sync or WorkerThreadPool.is_task_completed(job.task):
			WorkerThreadPool.wait_for_task_completion(job.task)
			_jobs.erase(key)
			_ready[key] = job
	# Готовые подключаются по важности, пока не исчерпан бюджет кадра (хотя бы один за кадр);
	# остальные ждут следующего кадра. sync — без бюджета, недостающие читаются здесь же.
	var attached := false
	for key in _wanted:
		var job: Job = _ready.get(key)
		if job == null and sync and not _loaded.has(key) and not _failed.has(key) and not _rejected.has(key):
			job = Job.new()
			job.entry = _entries[key]
			_run(job)
		if job == null:
			continue
		if not sync and attached and Time.get_ticks_usec() - started > ATTACH_BUDGET_USEC:
			break
		_ready.erase(key)
		changed = _accept(key, job) or changed
		attached = true
	for key: Vector3i in _ready.keys():
		if not _wanted_set.has(key):
			_accept(key, _ready[key])   # ушёл из окна, пока грузился: только учесть ошибку
			_ready.erase(key)
	if not sync:
		_start_jobs()
	if attached:
		var spent := Time.get_ticks_usec() - started
		max_poll_usec = maxi(max_poll_usec, spent)
		if poll_usec.size() < MAX_POLL_SAMPLES:
			poll_usec.append(spent)
	return changed


## Дождаться всех загрузок, не подключая их (выход).
func wait_all() -> void:
	for key: Vector3i in _jobs.keys():
		WorkerThreadPool.wait_for_task_completion(_jobs[key].task)
	_jobs.clear()
	_ready.clear()


func _start_jobs() -> void:
	var reserved := 0.0
	for job: Job in _jobs.values() + _ready.values():
		reserved += job.entry.size_bytes * _ratio
	var needed := 0
	for key in _wanted:
		if _loaded.has(key):
			needed += _loaded[key].bytes
	for key in _wanted:
		if _jobs.size() >= MAX_IN_FLIGHT:
			return
		if _loaded.has(key) or _jobs.has(key) or _ready.has(key) or _failed.has(key) or _rejected.has(key):
			continue
		var estimate: float = _entries[key].size_bytes * _ratio
		if reserved > 0.0 and needed + reserved + estimate > limit_bytes:
			return   # по оценке места уже нет: дальние ждут; точно лимит проверяется при подключении
		var job := Job.new()
		job.entry = _entries[key]
		job.task = WorkerThreadPool.add_task(_run.bind(job), false, "Участок карты")
		_jobs[key] = job
		reserved += estimate


## Работа в рабочем потоке: только чтение, проверка и массивы, без узлов сцены.
func _run(job: Job) -> void:
	job.tile = _index.load_tile(job.entry)
	if job.tile.error.is_empty():
		job.prepared = MapTileView.prepare(job.tile, _palette)


func _accept(key: Vector3i, job: Job) -> bool:
	if not job.tile.error.is_empty():
		_fail(key, job.tile.error)
		return false
	if not _wanted_set.has(key):
		return false   # пока грузился, ушёл из окна
	var bytes: int = job.prepared.bytes
	if job.entry.size_bytes > 0:
		_ratio = maxf(_ratio, float(bytes) / job.entry.size_bytes)
	if not _make_room(bytes):
		_rejected[key] = true
		return false
	var attach_started := Time.get_ticks_usec()
	var view: Variant = _attach.call(job.tile, job.prepared)
	max_attach_usec = maxi(max_attach_usec, Time.get_ticks_usec() - attach_started)
	if view is String:
		_fail(key, view)
		return false
	_loaded[key] = {"bytes": bytes, "used": _tick}
	used_bytes += bytes
	peak_bytes = maxi(peak_bytes, used_bytes)
	loaded_total += 1
	return true


func _fail(key: Vector3i, message: String) -> void:
	_failed[key] = message
	errors.append(message)
	push_error(message)


## Освободить место под bytes, вытесняя давно не нужные участки. false — не помещается даже без них.
func _make_room(bytes: int) -> bool:
	if used_bytes + bytes <= limit_bytes:
		return true
	var spare: Array = _loaded.keys().filter(func(key): return not _wanted_set.has(key))
	spare.sort_custom(func(a: Vector3i, b: Vector3i) -> bool:
		return _loaded[a].used < _loaded[b].used if _loaded[a].used != _loaded[b].used else a < b)
	var freeable := 0
	for key: Vector3i in spare:
		freeable += _loaded[key].bytes
	if used_bytes - freeable + bytes > limit_bytes:
		return false
	for key: Vector3i in spare:
		if used_bytes + bytes <= limit_bytes:
			break
		evict(key)
	return true


func evict(key: Vector3i) -> void:
	if not _loaded.has(key):
		return
	used_bytes -= _loaded[key].bytes
	_loaded.erase(key)
	evicted_total += 1
	_detach.call(key)


## Выгрузить всё (смена набора участков, выход).
func clear() -> void:
	wait_all()
	for key: Vector3i in _loaded.keys():
		evict(key)

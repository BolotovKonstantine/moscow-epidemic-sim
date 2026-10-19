extends RefCounted

# Эталон часов и очереди событий (#40): прямое прочтение docs/sim-core.md#время-и-очередь-событий
# без календарных корзин — двоичная куча (время, ID), у сущности одно ожидающее событие,
# устаревший ключ отбрасывается при извлечении. Продвижение — целыми интервалами: команды на
# границе, затем события раньше конца интервала. Игрушечное правило событий сцены (розыгрыши
# потока расписаний, перенос чужих событий, отмены, далёкие события, выходные дни) задано в
# tests/scenarios/README.md; ядро сверяется с выходом этого эталона (native/tests/clock_tests.cpp).

const Philox := preload("res://sim/core/philox.gd")
const MinHeap := preload("res://sim/core/min_heap.gd")

const DAY := 86400
const NONE := -1
const OTHER_BASE := 0x80000000
const STREAM_SCHEDULES := 3

const TOP_KEYS := ["schema_version", "name", "seed", "interval_s", "start_weekday", "duration_s",
		"checkpoints_s", "residents", "others", "rules", "initial", "commands"]
const RULE_KEYS := ["step_s", "steps", "weekend_factor", "far_per_256", "far_days", "poke_per_256",
		"poke_step_s", "poke_steps", "cancel_per_256"]
const COMMAND_KEYS := ["time", "first", "count", "shift_s"]

var scene: Dictionary
var rules: Dictionary
var key: int
var entities: int
var next: Array = []      # по номеру сущности: время ожидающего события или NONE
var heap := MinHeap.new()
var now := 0
var journal: Array = []     # [время, ID] в порядке исполнения


static func entity_id(scene_value: Dictionary, k: int) -> int:
	return k if k < scene_value.residents else OTHER_BASE + k - scene_value.residents


func _index(id: int) -> int:
	return id if id < OTHER_BASE else scene.residents + id - OTHER_BASE


func _init(scene_value: Dictionary, reverse_init := false) -> void:
	scene = scene_value
	rules = scene.rules
	key = Philox.stream_key(scene.seed, STREAM_SCHEDULES)
	entities = scene.residents + scene.others
	next.resize(entities)
	next.fill(NONE)
	var initial: Array = scene.initial.duplicate()
	if reverse_init:
		initial.reverse()
	for item in initial:
		schedule(item[0], item[1])


func schedule(id: int, t: int) -> void:
	var k := _index(id)
	if next[k] == t:
		return
	next[k] = t
	heap.push([t, id])


func cancel(id: int) -> void:
	next[_index(id)] = NONE


func weekday(t: int) -> int:
	return (scene.start_weekday + t / DAY) % 7


func _event(t: int, id: int) -> void:
	journal.append([t, id])
	var n := 0
	var r: Array = Philox.below(key, id, n, t, 0, rules.steps)
	n = r[1]
	var delay: int = rules.step_s * (1 + r[0])
	if weekday(t) >= 5:
		delay *= rules.weekend_factor
	r = Philox.below(key, id, n, t, 0, 256)
	n = r[1]
	if r[0] < rules.far_per_256:
		r = Philox.below(key, id, n, t, 0, rules.far_days)
		n = r[1]
		delay += DAY * (1 + r[0])
	schedule(id, t + delay)
	r = Philox.below(key, id, n, t, 0, 256)
	n = r[1]
	if r[0] < rules.poke_per_256:
		r = Philox.below(key, id, n, t, 0, entities)
		n = r[1]
		var other := entity_id(scene, r[0])
		r = Philox.below(key, id, n, t, 0, rules.poke_steps)
		n = r[1]
		schedule(other, t + rules.poke_step_s * (1 + r[0]))
	r = Philox.below(key, id, n, t, 0, 256)
	n = r[1]
	if r[0] < rules.cancel_per_256:
		r = Philox.below(key, id, n, t, 0, entities)
		cancel(entity_id(scene, r[0]))


func _apply_command(c: Dictionary) -> void:
	for k in range(c.first, c.first + c.count):
		if next[k] != NONE:
			schedule(entity_id(scene, k), next[k] + c.shift_s)


# Продвижение до границы t: каждый интервал — команды на его начале, затем события раньше конца.
func advance_to(t: int) -> void:
	while now < t:
		for c in scene.commands:
			if c.time == now:
				_apply_command(c)
		var end: int = now + scene.interval_s
		while not heap.is_empty() and heap.top()[0] < end:
			var item: Array = heap.pop()
			var k := _index(item[1])
			if next[k] != item[0]:
				continue  # устаревший ключ
			next[k] = NONE
			_event(item[0], item[1])
		now = end


func checkpoint() -> Dictionary:
	var live := 0
	for v in next:
		if v != NONE:
			live += 1
	return {"t": now, "live": live, "next": next.duplicate()}


# chunks — длины порций продвижения в интервалах по кругу (пусто — до контрольного времени сразу).
static func run_scene(scene_value: Dictionary, chunks := [], reverse_init := false) -> Dictionary:
	var sim = load("res://sim/core/event_reference.gd").new(scene_value, reverse_init)
	var checkpoints := []
	var chunk_i := 0
	for t in scene_value.checkpoints_s:
		while sim.now < t:
			var step: int = t - sim.now
			if not chunks.is_empty():
				step = mini(step, chunks[chunk_i % chunks.size()] * scene_value.interval_s)
				chunk_i += 1
			sim.advance_to(sim.now + step)
		checkpoints.append(sim.checkpoint())
	sim.advance_to(scene_value.duration_s)
	return {"checkpoints": checkpoints, "log": sim.journal}


# ---------- Проверка сцены ----------

static func _int(v) -> bool:
	return typeof(v) == TYPE_INT or (typeof(v) == TYPE_FLOAT and v == floorf(v) and absf(v) < 9.0e15)


static func _exact_keys(d, keys: Array, where: String) -> String:
	if typeof(d) != TYPE_DICTIONARY:
		return "%s: нужен объект" % where
	for k in d:
		if not keys.has(k):
			return "%s: неизвестное поле %s" % [where, k]
	for k in keys:
		if not d.has(k):
			return "%s: нет поля %s" % [where, k]
	return ""


static func _ints(d: Dictionary, keys: Array, where: String) -> String:
	for k in keys:
		if not _int(d[k]):
			return "%s.%s: нужно целое" % [where, k]
		d[k] = int(d[k])
	return ""


# Возвращает {scene} или {error}. Целые из JSON (float) приводятся к int.
static func parse(data) -> Dictionary:
	var err := _exact_keys(data, TOP_KEYS, "сцена")
	if err:
		return {"error": err}
	var s: Dictionary = data.duplicate(true)
	err = _ints(s, ["schema_version", "seed", "interval_s", "start_weekday", "duration_s", "residents", "others"], "сцена")
	if err:
		return {"error": err}
	if s.schema_version != 1:
		return {"error": "schema_version: поддерживается только 1"}
	if s.interval_s < 60 or s.interval_s > 3600 or DAY % s.interval_s != 0:
		return {"error": "interval_s: делит сутки, 60–3600"}
	if s.start_weekday < 0 or s.start_weekday > 6:
		return {"error": "start_weekday: 0–6"}
	if s.duration_s <= 0 or s.duration_s % s.interval_s != 0:
		return {"error": "duration_s: положительное, кратное интервалу"}
	if s.residents < 0 or s.others < 0 or s.residents + s.others == 0:
		return {"error": "residents, others: неотрицательные, хотя бы одна сущность"}
	err = _exact_keys(s.rules, RULE_KEYS, "rules")
	if err:
		return {"error": err}
	err = _ints(s.rules, RULE_KEYS, "rules")
	if err:
		return {"error": err}
	for k in ["step_s", "steps", "weekend_factor", "far_days", "poke_step_s", "poke_steps"]:
		if s.rules[k] < 1:
			return {"error": "rules.%s: не меньше 1" % k}
	for k in ["far_per_256", "poke_per_256", "cancel_per_256"]:
		if s.rules[k] < 0 or s.rules[k] > 256:
			return {"error": "rules.%s: 0–256" % k}
	if typeof(s.checkpoints_s) != TYPE_ARRAY:
		return {"error": "checkpoints_s: нужен массив"}
	var prev := -1
	for i in s.checkpoints_s.size():
		var t = s.checkpoints_s[i]
		if not _int(t) or int(t) <= prev or int(t) > s.duration_s or int(t) % s.interval_s != 0:
			return {"error": "checkpoints_s: строго возрастающие границы интервалов в [0, duration_s]"}
		s.checkpoints_s[i] = int(t)
		prev = int(t)
	if typeof(s.initial) != TYPE_ARRAY:
		return {"error": "initial: нужен массив"}
	var seen := {}
	for i in s.initial.size():
		var item = s.initial[i]
		if typeof(item) != TYPE_ARRAY or item.size() != 2 or not _int(item[0]) or not _int(item[1]):
			return {"error": "initial[%d]: нужна пара [ID, время]" % i}
		var id := int(item[0])
		var t := int(item[1])
		var valid_id: bool = (id >= 0 and id < s.residents) or (id >= OTHER_BASE and id < OTHER_BASE + s.others)
		if not valid_id or seen.has(id):
			return {"error": "initial[%d]: ID вне сущностей сцены или повторяется" % i}
		if t < 0 or t >= 0xFFFFFFFF:
			return {"error": "initial[%d]: время вне [0, 2³² − 1)" % i}
		seen[id] = true
		s.initial[i] = [id, t]
	if typeof(s.commands) != TYPE_ARRAY:
		return {"error": "commands: нужен массив"}
	for i in s.commands.size():
		var where := "commands[%d]" % i
		err = _exact_keys(s.commands[i], COMMAND_KEYS, where)
		if err:
			return {"error": err}
		err = _ints(s.commands[i], COMMAND_KEYS, where)
		if err:
			return {"error": err}
		var c: Dictionary = s.commands[i]
		if c.time < 0 or c.time >= s.duration_s or c.time % s.interval_s != 0:
			return {"error": "%s.time: граница интервала в [0, duration_s)" % where}
		if c.first < 0 or c.count < 0 or c.first + c.count > s.residents + s.others or c.shift_s < 1:
			return {"error": "%s: диапазон сущностей и сдвиг не меньше 1 с" % where}
	return {"scene": s}


static func load_file(path: String) -> Dictionary:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		return {"error": "%s: не прочитан" % path}
	var json := JSON.new()
	if json.parse(text) != OK:
		return {"error": "%s: JSON, строка %d: %s" % [path, json.get_error_line(), json.get_error_message()]}
	return parse(json.data)

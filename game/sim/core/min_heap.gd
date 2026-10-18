extends RefCounted

# Двоичная куча эталона: элементы — массивы целых, порядок лексикографический. Используется
# Дейкстрой (стоимость, узел) и очередью событий (время, ID, сутки рейса).

var items: Array = []


func size() -> int:
	return items.size()


func is_empty() -> bool:
	return items.is_empty()


func top() -> Array:
	return items[0]


static func less(a: Array, b: Array) -> bool:
	for i in a.size():
		if a[i] != b[i]:
			return a[i] < b[i]
	return false


func push(item: Array) -> void:
	items.append(item)
	var i := items.size() - 1
	while i > 0:
		var parent := (i - 1) >> 1
		if not less(items[i], items[parent]):
			break
		var tmp: Array = items[i]
		items[i] = items[parent]
		items[parent] = tmp
		i = parent


func pop() -> Array:
	var out: Array = items[0]
	var last: Array = items.pop_back()
	if items.is_empty():
		return out
	items[0] = last
	var i := 0
	var n := items.size()
	while true:
		var l := 2 * i + 1
		var r := l + 1
		var best := i
		if l < n and less(items[l], items[best]):
			best = l
		if r < n and less(items[r], items[best]):
			best = r
		if best == i:
			break
		var tmp: Array = items[i]
		items[i] = items[best]
		items[best] = tmp
		i = best
	return out

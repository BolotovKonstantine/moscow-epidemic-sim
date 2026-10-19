extends RefCounted

# Случайность эталона (docs/sim-core.md#случайность): Philox4x32-10 (Random123), ключ потока
# SplitMix64 и метод Лемира — независимая реализация на GDScript тех же правил, что
# native/include/mesim/rng.h. Целые GDScript — знаковые 64 бита, поэтому произведения считаются
# по 16-битным частям: ни одно промежуточное значение не выходит за 2⁶³.

const MASK32 := 0xFFFFFFFF
const MASK16 := 0xFFFF
const TWO32 := 0x100000000

# Номера потоков ядра закреплены в rng.h (RngStream): 3 — расписания, 4 — модельные рейсы.
const STREAM_POPULATION := 1
const STREAM_PLACES := 2
const STREAM_TRANSIT := 4


# Произведение двух u32: [старшие 32 бита, младшие 32 бита].
static func mul32(a: int, b: int) -> Array:
	var al := a & MASK16
	var ah := a >> 16
	var bl := b & MASK16
	var bh := b >> 16
	var ll := al * bl
	var mid := ah * bl + al * bh  # < 2³³
	var hh := ah * bh
	var lo_full := ll + ((mid & MASK16) << 16)
	var hi := hh + (mid >> 16) + (lo_full >> 32)
	return [hi & MASK32, lo_full & MASK32]


# Произведение по модулю 2⁶⁴ (результат — знаковое представление тех же битов).
static func mul64(a: int, b: int) -> int:
	var a_lo := a & MASK32
	var a_hi := (a >> 32) & MASK32
	var b_lo := b & MASK32
	var b_hi := (b >> 32) & MASK32
	var low: Array = mul32(a_lo, b_lo)
	var cross: int = (mul32(a_lo, b_hi)[1] + mul32(a_hi, b_lo)[1] + low[0]) & MASK32
	return _join(cross, low[1])


# Логический сдвиг вправо 64-битного значения.
static func shr64(z: int, k: int) -> int:
	return (z >> k) & ((1 << (64 - k)) - 1) if k > 0 else z


static func _join(hi: int, lo: int) -> int:
	# hi << 32 переполняет знаковый int при hi ≥ 2³¹ — собираем через знак явно.
	if hi >= 0x80000000:
		return ((hi - TWO32) << 32) | lo
	return (hi << 32) | lo


# Один блок Philox4x32-10: счётчик из 4 u32, ключ из 2 u32 → 4 u32.
static func block(ctr: Array, key: Array) -> Array:
	var c0: int = ctr[0]
	var c1: int = ctr[1]
	var c2: int = ctr[2]
	var c3: int = ctr[3]
	var k0: int = key[0]
	var k1: int = key[1]
	for round in 10:
		if round > 0:
			k0 = (k0 + 0x9E3779B9) & MASK32
			k1 = (k1 + 0xBB67AE85) & MASK32
		var p0: Array = mul32(0xD2511F53, c0)
		var p1: Array = mul32(0xCD9E8D57, c2)
		var n0: int = p1[0] ^ c1 ^ k0
		var n1: int = p1[1]
		var n2: int = p0[0] ^ c3 ^ k1
		var n3: int = p0[1]
		c0 = n0
		c1 = n1
		c2 = n2
		c3 = n3
	return [c0, c1, c2, c3]


# Ключ потока: SplitMix64 от seed и номера потока, как rng_stream_key ядра.
static func stream_key(seed: int, stream: int) -> int:
	var z := seed ^ ((stream << 56) | 0x5EED)
	z = mul64(z ^ shr64(z, 30), -4658895280553007687)  # 0xBF58476D1CE4E5B9
	z = mul64(z ^ shr64(z, 27), -7723592293110705685)  # 0x94D049BB133111EB
	return z ^ shr64(z, 31)


# Розыгрыш номер n сущности: счётчик (id, n, slot, sub), результат — первое слово, r в [0, 2³²).
static func draw(key: int, id: int, n: int, slot: int, sub: int) -> int:
	return block([id, n, slot, sub], [key & MASK32, (key >> 32) & MASK32])[0]


# Целое из [0, bound), bound от 1 до 2³², методом Лемира с отбраковкой; розыгрыши с номера first.
# Возвращает [значение, номер следующего розыгрыша].
static func below(key: int, id: int, first: int, slot: int, sub: int, bound: int) -> Array:
	var reject := (TWO32 - bound) % bound
	var n := first
	while true:
		var r := draw(key, id, n, slot, sub)
		n += 1
		if bound == TWO32:
			return [r, n]
		var m: Array = mul32(r, bound)
		if m[1] >= reject:
			return [m[0], n]
	return [0, n]

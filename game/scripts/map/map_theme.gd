class_name MapTheme
extends RefCounted

# Тема карты: все цвета и толщины в одном месте (решение #10 — светлая узнаваемая карта:
# фон, вода, зелень, иерархия дорог). Это игровые настройки отображения, не данные.
# Ключи — имена классов из заголовка участка, поэтому порядок кодов в файле не важен.

const BACKGROUND := Color("#ece8e1")

const AREA := {
	"farmland": Color("#eae9d2"),
	"residential": Color("#e2ddd6"),
	"commercial": Color("#ecdcd8"),
	"industrial": Color("#e0dae0"),
	"cemetery": Color("#cfdccb"),
	"grass": Color("#d9e7c6"),
	"park": Color("#c8e3b8"),
	"forest": Color("#b5d6a0"),
	"water": Color("#a9cfdc"),
}
const AREA_UNKNOWN := Color("#d8d4cd")

# Линии: цвет, обводка, ширина в метрах на местности, минимальная ширина и обводка в пикселях.
# Ширина на экране — max(ширина в метрах, минимум в пикселях); обводка добавляется с каждой стороны.
const LINE := {
	"stream": [Color("#a9cfdc"), Color("#a9cfdc"), 2.0, 0.6, 0.0],
	"river": [Color("#a9cfdc"), Color("#a9cfdc"), 10.0, 1.5, 0.0],
	"service": [Color("#ffffff"), Color("#cfc8bd"), 4.0, 0.6, 0.5],
	"pedestrian": [Color("#f6f4f0"), Color("#d2cbc0"), 3.0, 0.5, 0.4],
	"residential": [Color("#ffffff"), Color("#c6beb2"), 8.0, 1.0, 0.6],
	"tertiary": [Color("#ffffff"), Color("#bdb4a6"), 10.0, 1.4, 0.7],
	"secondary": [Color("#f7e7a0"), Color("#c9b46a"), 14.0, 1.8, 0.7],
	"primary": [Color("#fbd5a0"), Color("#d39f5e"), 18.0, 2.2, 0.8],
	"trunk": [Color("#f8b59d"), Color("#c9785f"), 22.0, 2.6, 0.8],
	"motorway": [Color("#e98fa0"), Color("#b55a6c"), 28.0, 3.0, 0.9],
	"tram": [Color("#8a8580"), Color("#8a8580"), 1.5, 1.0, 0.0],
	"rail_minor": [Color("#a8a39c"), Color("#a8a39c"), 2.0, 0.8, 0.0],
	"rail": [Color("#77726c"), Color("#77726c"), 3.0, 1.3, 0.0],
	"boundary_moscow": [Color("#9c7fb3"), Color("#9c7fb3"), 0.0, 1.6, 0.0],
	"boundary_region": [Color("#6e5590"), Color("#6e5590"), 0.0, 2.4, 0.0],
}
const LINE_UNKNOWN := [Color("#999999"), Color("#999999"), 2.0, 1.0, 0.0]

const BUILDING_FILL := Color("#d6cfc6")
const BUILDING_OUTLINE := Color("#b3aa9e")
const BUILDING_OUTLINE_MAX_MPP := 1.5   # контуры зданий — только при сильном приближении, иначе шум

# Уровень подробности по масштабу (метров на пиксель): уровень 2 до 3 м/пикс., 1 — до 14.
const LEVEL_MAX_MPP := {2: 3.0, 1: 14.0}
const MIN_MPP := 0.25   # предельное приближение: дома и дворы различимы (пожелание автора, #12)


static func area_color(cls_name: String) -> Color:
	return AREA.get(cls_name, AREA_UNKNOWN)


static func line_style(cls_name: String) -> Array:
	return LINE.get(cls_name, LINE_UNKNOWN)

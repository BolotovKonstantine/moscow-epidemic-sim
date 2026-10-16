"""Итоги переписи 2020 года (проведена в 2021) для генерации жителей (#34, #38).

Читает пять таблиц Росстата (xlsx — zip с XML, без сторонних библиотек) и выбирает строки
«г. Москва» (территория Москвы) и «Московская область» (пояс за Москвой). Числа копируются как
есть (`observed`); доли и распределения по годам считает генератор жителей, а не пайплайн.
Карточки и SHA256 таблиц — в data/manifests/sources.json (`rosstat-vpn2020-*`), разбор подтверждён
прототипом benchmarks/population_gen.
"""

import re
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

from .manifest import ManifestError

NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
TERRITORIES = {"moscow": "г. Москва", "buffer": "Московская область"}
# Роль источника в конфигурации сборки → таблица переписи.
ROLES = {
    "census_age": "age",
    "census_household_size": "household_size",
    "census_household_children": "household_children",
    "census_labour": "labour",
    "census_study": "study",
}
AGE_BINS = [[0, 4], [5, 9], [10, 14], [15, 19], [20, 24], [25, 29], [30, 34], [35, 39], [40, 44], [45, 49],
            [50, 54], [55, 59], [60, 64], [65, 69], [70, 74], [75, 79], [80, 84], [85, 100]]
LABOUR_BINS = [[15, 19], [20, 29], [30, 39], [40, 49], [50, 59], [60, 69], [70, 74], [75, 100]]
STUDY_BINS = [[0, 2], [3, 6], [7, 9], [10, 14], [15, 17], [18, 19], [20, 24], [25, 29], [30, 34], [35, 39],
              [40, 44], [45, 49], [50, 54], [55, 59], [60, 100]]
CHILDREN_SIZES = [2, 3, 4, 5, 6]   # 6 — шесть и более человек


def rows_of(path: Path):
    """Строки первого листа как списки строк; пустые ячейки — ''. Пропуски столбцов заполняются по ссылке ячейки."""
    try:
        with zipfile.ZipFile(path) as archive:
            shared = []
            if "xl/sharedStrings.xml" in archive.namelist():
                for item in ET.fromstring(archive.read("xl/sharedStrings.xml")).iter(NS + "si"):
                    shared.append("".join(t.text or "" for t in item.iter(NS + "t")))
            sheet = ET.fromstring(archive.read("xl/worksheets/sheet1.xml"))
    except (OSError, KeyError, zipfile.BadZipFile, ET.ParseError) as error:
        raise ManifestError(f"{path}: не удалось прочитать таблицу xlsx: {error}") from error
    result = []
    for row in sheet.iter(NS + "row"):
        cells = []
        for cell in row.iter(NS + "c"):
            letters = re.match(r"[A-Z]+", cell.get("r")).group(0)
            column = 0
            for char in letters:
                column = column * 26 + ord(char) - 64
            while len(cells) < column - 1:
                cells.append("")
            value = cell.find(NS + "v")
            if value is None:
                text = "".join(t.text or "" for t in cell.iter(NS + "t"))
            elif cell.get("t") == "s":
                text = shared[int(value.text)]
            else:
                text = value.text
            cells.append(" ".join((text or "").split()))
        result.append(cells)
    return result


def number(text):
    """Целое число ячейки; «-» и пусто — 0 (так Росстат обозначает отсутствие явления)."""
    text = text.strip()
    if text in ("-", ""):
        return 0
    if re.fullmatch(r"\d+", text):
        return int(text)
    value = float(text)
    if value != int(value) or value < 0:
        raise ManifestError(f"ожидалось целое неотрицательное число, получено {text!r}")
    return int(value)


def block(rows, region, length):
    """Строки, начиная с первой строки, которая начинается с названия региона."""
    for index, row in enumerate(rows):
        if row and row[0].startswith(region):
            return rows[index:index + length]
    raise ManifestError(f"нет строки {region!r}")


def labelled(rows, labels, columns):
    """По подписи строки — список чисел из столбцов columns."""
    out = {}
    for label in labels:
        row = next((r for r in rows if r and r[0].replace(" ", "").startswith(label.replace(" ", ""))), None)
        if row is None:
            raise ManifestError(f"нет строки {label!r}")
        out[label] = [number(row[c]) for c in columns]
    return out


def age(rows, region):
    groups = ["0 – 4", "5 – 9", "10 - 14", "15 - 19", "20 - 24", "25 - 29", "30 - 34", "35 - 39", "40 - 44",
              "45 - 49", "50 - 54", "55 - 59", "60 - 64", "65 - 69", "70 - 74", "75 - 79", "80 - 84", "85 и более"]
    part = block(rows, region, 24)
    values = labelled(part, groups, [1])
    return {"bins": AGE_BINS, "persons": [values[g][0] for g in groups], "total": number(part[1][1])}


def household_size(rows, region):
    row = block(rows, region, 1)[0]
    values = [number(x) for x in row[1:10]]   # десятый столбец — средний размер, дробный и не нужен
    return {"households": values[0], "persons": values[1], "by_size_1_to_5": values[2:7],
            "size_6_plus": {"households": values[7], "persons": values[8]}}


def household_children(rows, region):
    """Первый блок региона: домохозяйства из 2+ человек по размеру (2, 3, 4, 5, 6+) и число с 1, 2, 3+ детьми."""
    part = block(rows, region, 8)

    def pick(label):
        row = next((r for r in part if r and r[0].startswith(label)), None)
        if row is None:
            raise ManifestError(f"нет строки {label!r}")
        return row
    columns = [3, 4, 5, 6, 7]
    with_children = {label: [number(pick(label)[c]) for c in columns] for label in ("с 1 ребенком", "с 2 детьми", "с 3 и более детьми")}
    # У 6+ число детей стоит в подстолбце «в них лиц» (8), а не в «число домохозяйств» (7).
    return {"sizes": CHILDREN_SIZES, "households": [number(pick("Все домохозяйства")[c]) for c in columns],
            "with_1_child": with_children["с 1 ребенком"], "with_2_children": with_children["с 2 детьми"],
            "with_3_plus_children": with_children["с 3 и более детьми"],
            "children_in_3_plus": [number(pick("в них детей")[c]) for c in columns[:-1] + [8]]}


def labour(rows, region):
    groups = ["15 - 19", "20 - 29", "30 - 39", "40 - 49", "50 - 59", "60 - 69", "70 - 74", "75 и более"]
    part = block(rows, region, 14)
    values = labelled(part, groups, [1, 2, 4])
    return {"bins": LABOUR_BINS, "total": [values[g][0] for g in groups],
            "status_indicated": [values[g][1] for g in groups], "employed": [values[g][2] for g in groups]}


def study(rows, region):
    groups = ["0 – 2", "3 – 6", "7 – 9", "10 – 14", "15 – 17", "18 – 19", "20 – 24", "25 – 29", "30 – 34", "35 – 39",
              "40 – 44", "45 – 49", "50 – 54", "55 – 59", "60 и более"]
    part = block(rows, region, 20)
    values = labelled(part, groups, list(range(1, 13)))
    # Столбцы: всего; указавшие обучение; только основные: дошкольное, общее, профессиональное;
    # основные и дополнительные: те же три; только дополнительные; программа не указана; не обучаются; не указали.

    def column(i):
        return [values[g][i] for g in groups]
    return {"bins": STUDY_BINS, "total": column(0), "not_indicated": column(11),
            "preschool": [a + b for a, b in zip(column(2), column(5))],
            "general": [a + b for a, b in zip(column(3), column(6))],
            "professional": [a + b for a, b in zip(column(4), column(7))]}


READERS = {"age": age, "household_size": household_size, "household_children": household_children, "labour": labour, "study": study}


def read_census(paths: dict) -> dict:
    """paths — {роль census_*: путь xlsx}. Результат — {территория: {таблица: данные}}."""
    missing = sorted(set(ROLES) - set(paths))
    if missing:
        raise ManifestError(f"нет таблиц переписи для ролей {missing}")
    result = {territory: {} for territory in TERRITORIES}
    for role, key in ROLES.items():
        rows = rows_of(paths[role])
        for territory, region in TERRITORIES.items():
            try:
                result[territory][key] = READERS[key](rows, region)
            except (ManifestError, IndexError, ValueError) as error:
                raise ManifestError(f"{paths[role]}: {region}: {error}") from error
    return result

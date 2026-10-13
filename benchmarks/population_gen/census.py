"""Прототип генерации жителей (#34): итоги переписи 2020 (проведена в 2021) → census.json.

Читает пять таблиц Росстата из data/raw/ (карточки — в data/manifests/sources.json), проверяет
SHA256 по реестру и выбирает строки «г. Москва» и «Московская область». Только стандартная
библиотека: xlsx — это zip с XML. Числа копируются как есть (observed); доли считаются в
generate.py/prepare.py, а не здесь.

    .venv/bin/python benchmarks/population_gen/census.py <вывод census.json>
"""

import hashlib
import json
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
TERRITORIES = {"moscow": "г. Москва", "buffer": "Московская область"}
SOURCES = {
    "age": "rosstat-vpn2020-tom2-tab2",
    "household_size": "rosstat-vpn2020-tom8-tab2",
    "household_children": "rosstat-vpn2020-tom8-tab4",
    "labour": "rosstat-vpn2020-tom10-tab2",
    "study": "rosstat-vpn2020-tom3-tab5",
}


def rows_of(path):
    """Строки первого листа как списки строк; пустые ячейки — ''. Пропуски столбцов заполняются по ссылке ячейки."""
    with zipfile.ZipFile(path) as archive:
        shared = []
        if "xl/sharedStrings.xml" in archive.namelist():
            for item in ET.fromstring(archive.read("xl/sharedStrings.xml")).iter(NS + "si"):
                shared.append("".join(t.text or "" for t in item.iter(NS + "t")))
        sheet = ET.fromstring(archive.read("xl/worksheets/sheet1.xml"))
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
    text = text.strip()
    if text in ("-", ""):
        return 0
    return int(text) if re.fullmatch(r"\d+", text) else float(text)


def block(rows, region, length):
    """Строки после первой строки, начинающейся с названия региона."""
    for index, row in enumerate(rows):
        if row and row[0].startswith(region):
            return rows[index:index + length]
    raise ValueError(f"нет строки {region!r}")


def labelled(rows, labels, columns, after=None):
    """По подписи строки — список чисел из столбцов columns. after — первая строка, с которой искать."""
    out = {}
    start = 0
    if after is not None:
        start = next(i for i, row in enumerate(rows) if row and row[0].startswith(after))
    for label in labels:
        row = next(r for r in rows[start:] if r and r[0].replace(" ", "").startswith(label.replace(" ", "")))
        out[label] = [number(row[c]) for c in columns]
    return out


def age(rows, region):
    groups = ["0 – 4", "5 – 9", "10 - 14", "15 - 19", "20 - 24", "25 - 29", "30 - 34", "35 - 39", "40 - 44",
              "45 - 49", "50 - 54", "55 - 59", "60 - 64", "65 - 69", "70 - 74", "75 - 79", "80 - 84", "85 и более"]
    part = block(rows, region, 24)
    values = labelled(part, groups, [1])
    return {"bins": [[0, 4], [5, 9], [10, 14], [15, 19], [20, 24], [25, 29], [30, 34], [35, 39], [40, 44], [45, 49],
                     [50, 54], [55, 59], [60, 64], [65, 69], [70, 74], [75, 79], [80, 84], [85, 100]],
            "persons": [values[g][0] for g in groups], "total": number(part[1][1])}


def household_size(rows, region):
    row = block(rows, region, 1)[0]
    values = [number(x) for x in row[1:11]]
    return {"households": values[0], "persons": values[1], "by_size_1_to_5": values[2:7],
            "size_6_plus": {"households": values[7], "persons": values[8]}}


def household_children(rows, region):
    """Первый блок региона: все домохозяйства из 2+ человек по размеру (2, 3, 4, 5, 6+) и число с 1, 2, 3+ детьми."""
    part = block(rows, region, 8)
    pick = lambda label: next(r for r in part if r[0].startswith(label))
    columns = [3, 4, 5, 6, 7]
    total = [number(pick("Все домохозяйства")[c]) for c in columns]
    with_children = {label: [number(pick(label)[c]) for c in columns] for label in ("с 1 ребенком", "с 2 детьми", "с 3 и более детьми")}
    children_3_plus = [number(pick("в них детей")[c]) for c in columns]
    return {"sizes": ["2", "3", "4", "5", "6+"], "households": total, "with_1_child": with_children["с 1 ребенком"],
            "with_2_children": with_children["с 2 детьми"], "with_3_plus_children": with_children["с 3 и более детьми"],
            "children_in_3_plus": children_3_plus}


def labour(rows, region):
    groups = ["15 - 19", "20 - 29", "30 - 39", "40 - 49", "50 - 59", "60 - 69", "70 - 74", "75 и более"]
    part = block(rows, region, 14)
    values = labelled(part, groups, [1, 2, 4])
    return {"bins": [[15, 19], [20, 29], [30, 39], [40, 49], [50, 59], [60, 69], [70, 74], [75, 100]],
            "total": [values[g][0] for g in groups], "status_indicated": [values[g][1] for g in groups],
            "employed": [values[g][2] for g in groups]}


def study(rows, region):
    groups = ["0 – 2", "3 – 6", "7 – 9", "10 – 14", "15 – 17", "18 – 19", "20 – 24", "25 – 29", "30 – 34", "35 – 39",
              "40 – 44", "45 – 49", "50 – 54", "55 – 59", "60 и более"]
    part = block(rows, region, 20)
    values = labelled(part, groups, list(range(1, 13)))
    # Столбцы: всего; указавшие обучение; только основные: дошкольное, общее, профессиональное;
    # основные и дополнительные: те же три; только дополнительные; программа не указана; не обучаются; не указали.
    pick = lambda i: [values[g][i] for g in groups]
    return {"bins": [[0, 2], [3, 6], [7, 9], [10, 14], [15, 17], [18, 19], [20, 24], [25, 29], [30, 34], [35, 39],
                     [40, 44], [45, 49], [50, 54], [55, 59], [60, 100]],
            "total": pick(0), "not_indicated": pick(11),
            "preschool": [a + b for a, b in zip(pick(2), pick(5))],
            "general": [a + b for a, b in zip(pick(3), pick(6))],
            "professional": [a + b for a, b in zip(pick(4), pick(7))]}


READERS = {"age": age, "household_size": household_size, "household_children": household_children,
           "labour": labour, "study": study}


def main(out_path: Path):
    registry = {s["source_id"]: s for s in json.loads((ROOT / "data/manifests/sources.json").read_text(encoding="utf-8"))["sources"]}
    result = {"note": "Итоги ВПН-2020 (перепись 2021) для строк «г. Москва» и «Московская область»; числа как в таблицах Росстата",
              "sources": {}, "territories": {t: {} for t in TERRITORIES}}
    for key, source_id in SOURCES.items():
        source = registry[source_id]
        path = ROOT / "data/raw" / source["file"]
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != source["sha256"]:
            sys.exit(f"{source_id}: SHA256 {digest} не совпадает с реестром; скачайте {source['url']} в data/raw/")
        rows = rows_of(path)
        result["sources"][key] = source_id
        for territory, region in TERRITORIES.items():
            result["territories"][territory][key] = READERS[key](rows, region)
    out_path.write_text(json.dumps(result, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    for territory, data in result["territories"].items():
        print(territory, "население", data["age"]["total"], "домохозяйств", data["household_size"]["households"])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))

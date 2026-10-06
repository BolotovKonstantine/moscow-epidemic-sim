"""Детерминированная запись файлов пакета.

gzip без имени и времени в заголовке, строки только с `\\n`, фиксированное
форматирование чисел: повторная сборка из тех же источников даёт те же байты
(в том же окружении библиотек).
"""

import csv
import gzip
import io
import json
from contextlib import contextmanager
from pathlib import Path

import numpy as np
import shapely


@contextmanager
def open_text(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.suffix == ".gz":
        with path.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=6) as compressed:
                with io.TextIOWrapper(compressed, encoding="utf-8", newline="\n") as text:
                    yield text
    else:
        with path.open("w", encoding="utf-8", newline="\n") as text:
            yield text


def fmt(value, digits=3):
    """Строковое представление значения для CSV; None → пустая ячейка, числа — без лишних нулей."""
    if value is None:
        return ""
    if isinstance(value, (bool, np.bool_)):
        return "1" if value else "0"
    if isinstance(value, str):
        return value
    text = f"{float(value):.{digits}f}"
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text == "-0" else text


def write_csv(path: Path, header, rows):
    with open_text(path) as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(header)
        for row in rows:
            writer.writerow(row)


def dumps(document) -> str:
    return json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def write_json(path: Path, document):
    with open_text(path) as stream:
        stream.write(json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False))
        stream.write("\n")


def write_geojsonl(path: Path, geometries, properties):
    """GeoJSON по одному Feature в строке. Геометрии уже в WGS84 и округлены."""
    texts = shapely.to_geojson(geometries)
    with open_text(path) as stream:
        for text, props in zip(texts, properties, strict=True):
            stream.write('{"type":"Feature","properties":')
            stream.write(dumps(props))
            stream.write(',"geometry":')
            stream.write(text)
            stream.write("}\n")


def write_geojson(path: Path, geometries, properties):
    texts = shapely.to_geojson(geometries)
    features = ",\n".join(
        '{"type":"Feature","properties":' + dumps(props) + ',"geometry":' + text + "}"
        for text, props in zip(texts, properties, strict=True)
    )
    with open_text(path) as stream:
        stream.write('{"type":"FeatureCollection","features":[\n' + features + "\n]}\n")

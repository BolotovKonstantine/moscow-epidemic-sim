"""Проверка паспорта без загрузки геоданных в память целиком."""

import hashlib
import json
import math
import re
from datetime import datetime
from pathlib import Path, PurePosixPath

from jsonschema import Draft202012Validator, FormatChecker

SCHEMA_PATH = Path(__file__).parent / "schemas" / "city-package-v1.schema.json"


class ManifestError(ValueError):
    """Некорректный паспорт или пакет."""


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"Повторяющийся ключ JSON: {key}")
        result[key] = value
    return result


def _invalid_constant(value):
    raise ManifestError(f"Недопустимое значение JSON: {value}")


def _finite_float(text):
    """Число с плавающей точкой; переполнение вроде 1e400 (→ inf) отклоняется сразу."""
    value = float(text)
    if not math.isfinite(value):
        raise ManifestError(f"Недопустимое значение JSON: {text}")
    return value


def parse_json_bytes(data: bytes, origin):
    """Разобрать JSON из уже прочитанных байтов с теми же строгими правилами, что read_json."""
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object, parse_constant=_invalid_constant, parse_float=_finite_float)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"Не удалось прочитать {origin}: {error}") from error


def read_json(path: Path):
    try:
        with path.open(encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=_unique_object, parse_constant=_invalid_constant)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"Не удалось прочитать {path}: {error}") from error


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _timestamp(value):
    if not isinstance(value, str):
        return True  # Тип проверяется схемой.
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})", value):
        return False
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return False
    return "T" in value and parsed.tzinfo is not None


def _asset_path(root: Path, relative: str) -> Path:
    parts = relative.split("/")
    if "\\" in relative or ":" in relative or PurePosixPath(relative).is_absolute() or any(part in ("", ".", "..") for part in parts):
        raise ManifestError(f"Некорректный относительный путь файла: {relative}")
    try:
        resolved_root = root.resolve()
        candidate = (resolved_root / relative).resolve()
    except (OSError, RuntimeError) as error:
        raise ManifestError(f"Не удалось разрешить путь {relative}: {error}") from error
    if not candidate.is_relative_to(resolved_root):
        raise ManifestError(f"Файл находится за пределами пакета: {relative}")
    return candidate


def validate_source_card(card: dict, origin) -> None:
    """Проверить одну карточку источника по схеме паспорта (до сборки, а не после неё)."""
    schema = read_json(SCHEMA_PATH)
    formats = FormatChecker()
    formats.checks("date-time")(_timestamp)
    validator = Draft202012Validator({"$ref": "#/$defs/source", "$defs": schema["$defs"]}, format_checker=formats)
    errors = list(validator.iter_errors(card))
    if errors:
        raise ManifestError("\n".join(f"{origin}: {'.'.join(map(str, error.absolute_path)) or '$'}: {error.message}" for error in errors))


def validate_manifest(path: Path, *, check_files: bool = True) -> dict:
    manifest = read_json(path)
    schema = read_json(SCHEMA_PATH)
    Draft202012Validator.check_schema(schema)
    formats = FormatChecker()
    formats.checks("date-time")(_timestamp)
    errors = list(Draft202012Validator(schema, format_checker=formats).iter_errors(manifest))
    if errors:
        messages = []
        for error in errors:
            location = ".".join(str(part) for part in error.absolute_path) or "$"
            messages.append(f"{location}: {error.message}")
        raise ManifestError("\n".join(messages))

    source_ids = [source["source_id"] for source in manifest["sources"]]
    asset_ids = [asset["asset_id"] for asset in manifest["assets"]]
    paths = [asset["path"] for asset in manifest["assets"]]
    for name, values in (("source_id", source_ids), ("asset_id", asset_ids), ("path", paths)):
        if len(values) != len(set(values)):
            raise ManifestError(f"Повторяющийся {name}")

    boundary = next((asset for asset in manifest["assets"] if asset["asset_id"] == manifest["region"]["boundary_asset_id"]), None)
    if boundary is None or boundary["role"] != "boundary":
        raise ManifestError("region.boundary_asset_id должен ссылаться на файл с ролью boundary")

    known_sources = set(source_ids)
    synthetic_sources = {source["source_id"] for source in manifest["sources"] if source["source_type"] == "synthetic"}
    for asset in manifest["assets"]:
        missing = set(asset["source_ids"]) - known_sources
        if missing:
            raise ManifestError(f"{asset['asset_id']}: неизвестные источники {', '.join(sorted(missing))}")
        if asset["data_kind"] == "observed" and set(asset["source_ids"]) & synthetic_sources:
            raise ManifestError(f"{asset['asset_id']}: синтетический источник не является наблюдаемыми данными")
        target = _asset_path(path.parent, asset["path"])
        if not check_files:
            continue
        try:
            if not target.is_file():
                raise ManifestError(f"{asset['asset_id']}: файл отсутствует: {asset['path']}")
            if target.stat().st_size != asset["size_bytes"]:
                raise ManifestError(f"{asset['asset_id']}: размер файла не совпадает с паспортом")
            if sha256_file(target) != asset["sha256"]:
                raise ManifestError(f"{asset['asset_id']}: SHA256 не совпадает с паспортом")
        except OSError as error:
            raise ManifestError(f"{asset['asset_id']}: ошибка чтения файла: {error}") from error
        if asset["role"] == "map":
            validate_map_index(target, manifest, asset["asset_id"])
    return manifest


MAP_INDEX_MAX_BYTES = 8 << 20   # как MapIndex.MAX_INDEX_BYTES в игре
MAP_MAX_TILE_INDEX = 1 << 20    # как MapTile.MAX_TILE_INDEX: |номер участка|
MAP_MAX_COORD_M = 1.0e7         # как MapTile.MAX_COORD_M: |координата| плоскости карты


def validate_map_index(path: Path, manifest: dict, asset_id: str) -> int:
    """Проверить индекс участков карты: формат, пакет и каждый участок (путь внутри каталога карты,
    размер и SHA256). Хеш индекса записан в паспорте, хеши участков — в индексе. Возвращает число участков."""
    from .maptiles import CLASSES, FORMAT_VERSION, INDEX_FORMAT, LEVEL_TILE_M, MAP_CREDIT, MAX_TILE_BYTES, source_credits

    try:
        size = path.stat().st_size
        if size > MAP_INDEX_MAX_BYTES:
            raise ManifestError(f"{asset_id}: индекс карты {size} байт, предел {MAP_INDEX_MAX_BYTES}")
        index = parse_json_bytes(path.read_bytes(), path)
    except OSError as error:
        raise ManifestError(f"{asset_id}: ошибка чтения индекса карты: {error}") from error
    if not isinstance(index, dict) or index.get("format") != INDEX_FORMAT or type(index.get("format_version")) is not int:
        raise ManifestError(f"{asset_id}: не индекс участков карты {INDEX_FORMAT}")
    if index["format_version"] != FORMAT_VERSION:
        raise ManifestError(f"{asset_id}: версия индекса карты {index['format_version']}, поддерживается {FORMAT_VERSION}")
    for key in ("package_id", "package_version"):
        if index.get(key) != manifest[key]:
            raise ManifestError(f"{asset_id}: {key} индекса карты {index.get(key)!r} не совпадает с паспортом {manifest[key]!r}")
    # Те же требования, что у MapIndex в игре: иначе validate пропустил бы пакет, который игра не откроет.
    attribution = index.get("attribution")
    if not isinstance(attribution, list) or not attribution or not all(isinstance(item, str) for item in attribution) \
            or attribution[0] != MAP_CREDIT:
        raise ManifestError(f"{asset_id}: attribution индекса карты — список строк, первая — «{MAP_CREDIT}»")
    if attribution != source_credits(manifest):
        raise ManifestError(f"{asset_id}: attribution индекса карты не перечисляет все внешние источники паспорта: {source_credits(manifest)}")
    if index.get("classes") != {name: list(values) for name, values in CLASSES.items()}:
        raise ManifestError(f"{asset_id}: таблицы классов индекса карты не совпадают с форматом")
    tiles = index.get("tiles")
    if not isinstance(tiles, list) or not tiles:
        raise ManifestError(f"{asset_id}: в индексе карты нет участков")
    seen_paths, seen_tiles = set(), set()
    for tile in tiles:
        if not isinstance(tile, dict) or not isinstance(tile.get("path"), str) \
                or type(tile.get("level")) is not int or tile["level"] not in LEVEL_TILE_M \
                or type(tile.get("size_bytes")) is not int or not 0 < tile["size_bytes"] <= MAX_TILE_BYTES \
                or not isinstance(tile.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", tile["sha256"]) \
                or not isinstance(tile.get("tile"), list) or len(tile["tile"]) != 2 \
                or any(type(v) is not int or abs(v) > MAP_MAX_TILE_INDEX for v in tile["tile"]):
            raise ManifestError(f"{asset_id}: некорректная запись участка {str(tile)[:200]}")
        if not _map_box(tile.get("bbox")):
            raise ManifestError(f"{asset_id}: bbox участка {tile['path']} — упорядоченные minx, miny, maxx, maxy или null")
        key = (tile["level"], *tile["tile"])
        if tile["path"] in seen_paths or key in seen_tiles:
            raise ManifestError(f"{asset_id}: участок {tile['path']} указан дважды")
        seen_paths.add(tile["path"])
        seen_tiles.add(key)
        target = _asset_path(path.parent, tile["path"])
        try:
            if not target.is_file():
                raise ManifestError(f"{asset_id}: участок отсутствует: {tile['path']}")
            if target.stat().st_size != tile["size_bytes"]:
                raise ManifestError(f"{asset_id}: размер участка {tile['path']} не совпадает с индексом")
            data = target.read_bytes()
            if hashlib.sha256(data).hexdigest() != tile["sha256"]:
                raise ManifestError(f"{asset_id}: SHA256 участка {tile['path']} не совпадает с индексом")
            _check_map_tile(data, tile, index, asset_id)
        except OSError as error:
            raise ManifestError(f"{asset_id}: ошибка чтения участка {tile['path']}: {error}") from error
    if [key for key in seen_tiles if key[0] == 0] != [(0, 0, 0)]:
        raise ManifestError(f"{asset_id}: нужен ровно один обзор региона — участок уровня 0 с номером (0, 0)")
    return len(tiles)


MAP_TOLERANCE_M = 0.01   # как MapTile.ORIGIN_TOLERANCE_M: угол и bbox участка против индекса


def _check_map_tile(data: bytes, entry: dict, index: dict, asset_id: str):
    """Разобрать участок и сверить заголовок с записью индекса: магия и версия, распаковка и размеры
    разделов, пакет, уровень, номер, угол по сетке, bbox, классы и атрибуция. Смысл массивов
    (индексы вершин, коды классов, подписи) по-прежнему проверяет загрузчик игры при чтении."""
    import struct
    import zlib

    from .maptiles import LEVEL_TILE_M, MapTileError, decode_tile

    name = f"{asset_id}: участок {entry['path']}"
    try:
        header, _ = decode_tile(data)
    except (MapTileError, ValueError, KeyError, TypeError, IndexError, struct.error, zlib.error) as error:
        raise ManifestError(f"{name} не читается: {error}") from error
    expected = {"package_id": index["package_id"], "package_version": index["package_version"], "level": entry["level"],
                "tile": entry["tile"], "tile_size_m": LEVEL_TILE_M[entry["level"]], "classes": index["classes"],
                "attribution": index["attribution"]}
    for key, value in expected.items():
        if header.get(key) != value:
            raise ManifestError(f"{name}: {key} в заголовке {str(header.get(key))[:100]!r}, в индексе {str(value)[:100]!r}")
    size = LEVEL_TILE_M[entry["level"]]
    origin = header.get("origin")
    grid = [0.0, 0.0] if size is None else [entry["tile"][0] * size, entry["tile"][1] * size]
    if not _map_box((origin or []) * 2) or any(abs(o - g) > MAP_TOLERANCE_M for o, g in zip(origin, grid)):
        raise ManifestError(f"{name}: угол {origin} не совпадает с сеткой {grid}")
    if not isinstance(header.get("counts"), dict):
        raise ManifestError(f"{name}: counts в заголовке — не объект")
    bbox = header.get("bbox")
    if not _map_box(bbox):
        raise ManifestError(f"{name}: некорректный bbox в заголовке")
    if entry["level"] == 0 and (bbox is None or bbox[0] >= bbox[2] or bbox[1] >= bbox[3]):
        raise ManifestError(f"{name}: у обзора нужен bbox с ненулевой площадью — по нему игра ставит начальный вид")
    shifted = None if bbox is None else [bbox[0] + origin[0], bbox[1] + origin[1], bbox[2] + origin[0], bbox[3] + origin[1]]
    if (shifted is None) != (entry.get("bbox") is None) or (
            shifted is not None and any(abs(a - b) > MAP_TOLERANCE_M for a, b in zip(shifted, entry["bbox"]))):
        raise ManifestError(f"{name}: bbox участка не совпадает с индексом")


def _map_box(value):
    """bbox записи индекса: null или четыре числа не больше MAP_MAX_COORD_M по модулю, minx <= maxx, miny <= maxy."""
    if value is None:
        return True
    if not isinstance(value, list) or len(value) != 4 or any(type(v) not in (int, float) or abs(v) > MAP_MAX_COORD_M for v in value):
        return False
    return value[0] <= value[2] and value[1] <= value[3]

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
    return manifest

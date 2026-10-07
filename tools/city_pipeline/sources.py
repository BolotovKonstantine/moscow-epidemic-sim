"""Реестр источников: получение исходных выгрузок и проверка их SHA256.

Реестр (`data/manifests/sources.json`) хранится в Git; сами файлы — в игнорируемом
`data/raw/`. Сборка использует только файлы, чей хеш совпадает с реестром.
"""

import os
import shutil
import tempfile
import urllib.request
from pathlib import Path

from .manifest import ManifestError, read_json, sha256_file

REGISTRY_FIELDS = ("source_id", "url", "file", "owner", "license", "data_date", "acquired_at", "coverage", "format", "sha256")


def load_registry(path: Path) -> dict:
    registry = read_json(path)
    if registry.get("registry_version") != 1:
        raise ManifestError(f"{path}: неподдерживаемая версия реестра")
    result = {}
    for source in registry.get("sources", []):
        missing = [field for field in REGISTRY_FIELDS if field not in source]
        if missing:
            raise ManifestError(f"{source.get('source_id', '?')}: нет полей {', '.join(missing)}")
        if source["source_id"] in result:
            raise ManifestError(f"Повторяющийся source_id {source['source_id']}")
        if "/" in source["file"] or source["file"] in ("", ".", ".."):
            raise ManifestError(f"{source['source_id']}: имя файла должно быть без каталогов")
        result[source["source_id"]] = source
    return result


def verify(source: dict, raw_dir: Path) -> Path:
    path = raw_dir / source["file"]
    if not path.is_file():
        raise ManifestError(f"{source['source_id']}: нет файла {path}; выполните fetch")
    actual = sha256_file(path)
    if actual != source["sha256"]:
        raise ManifestError(f"{source['source_id']}: SHA256 {actual} не совпадает с реестром {source['sha256']}")
    return path


def fetch(source: dict, raw_dir: Path) -> tuple[Path, bool]:
    """Скачать файл, если его нет или хеш не совпадает. Возвращает (путь, был ли загружен)."""
    raw_dir.mkdir(parents=True, exist_ok=True)
    path = raw_dir / source["file"]
    if path.is_file() and sha256_file(path) == source["sha256"]:
        return path, False
    # Уникальный временный файл: одновременные загрузки не пишут в один файл; замена атомарна.
    handle, name = tempfile.mkstemp(prefix=f"{path.name}.", suffix=".part", dir=raw_dir)
    partial = Path(name)
    try:
        request = urllib.request.Request(source["url"], headers={"User-Agent": "moscow-epidemic-sim-city-pipeline"})
        with urllib.request.urlopen(request, timeout=60) as response, os.fdopen(handle, "wb") as stream:
            shutil.copyfileobj(response, stream, length=1024 * 1024)
        actual = sha256_file(partial)
        if actual != source["sha256"]:
            raise ManifestError(f"{source['source_id']}: скачанный файл имеет SHA256 {actual}, ожидался {source['sha256']}")
        partial.chmod(0o644)  # mkstemp создаёт файл только для владельца
        partial.replace(path)
    finally:
        partial.unlink(missing_ok=True)
    return path, True


def manifest_source(source: dict, processing_version: str) -> dict:
    return {
        "source_id": source["source_id"],
        "source_type": "external",
        "url": source["url"],
        "owner": source["owner"],
        "license": source["license"],
        "data_date": source["data_date"],
        "acquired_at": source["acquired_at"],
        "coverage": source["coverage"],
        "format": source["format"],
        "sha256": source["sha256"],
        "processing_version": processing_version,
    }

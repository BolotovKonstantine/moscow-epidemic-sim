"""CLI проверки, получения источников и сборки городских пакетов."""

import argparse
import fcntl
import os
import re
import shutil
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

from jsonschema import Draft202012Validator
from shapely.errors import ShapelyError

from .manifest import ManifestError, read_json, validate_manifest
from .sources import fetch, load_registry, manifest_source, verify

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = ROOT / "data" / "manifests" / "sources.json"


def _created_at() -> str:
    """Время сборки; SOURCE_DATE_EPOCH фиксирует его для побайтового сравнения паспортов."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    moment = datetime.fromtimestamp(int(epoch), UTC) if epoch else datetime.now(UTC).replace(microsecond=0)
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


SAFE_NAME = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
SAFE_VERSION = re.compile(r"^[0-9A-Za-z][0-9A-Za-z.+_-]*$")


def package_dir(root: Path, package_id, package_version) -> Path:
    """Каталог пакета внутри root; ID и версия — только безопасные для имени файла символы."""
    if not isinstance(package_id, str) or not SAFE_NAME.fullmatch(package_id):
        raise ManifestError(f"Недопустимый package_id для каталога: {package_id!r}")
    if not isinstance(package_version, str) or not SAFE_VERSION.fullmatch(package_version) or ".." in package_version:
        raise ManifestError(f"Недопустимая package_version для каталога: {package_version!r}")
    base = root.resolve()
    out = (base / f"{package_id}-{package_version}").resolve()
    if out.parent != base:
        raise ManifestError(f"Каталог пакета {out} вне {base}")
    return out


def work_dir(root: Path, package_id) -> Path:
    """Каталог промежуточных файлов пакета внутри root."""
    if not isinstance(package_id, str) or not SAFE_NAME.fullmatch(package_id):
        raise ManifestError(f"Недопустимый package_id для каталога: {package_id!r}")
    base = root.resolve()
    work = (base / package_id).resolve()
    if work.parent != base:
        raise ManifestError(f"Рабочий каталог {work} вне {base}")
    return work


SUPPORTED_CONFIG_VERSION = 1


CONFIG_SCHEMA = Path(__file__).parent / "schemas" / "build-config-v1.schema.json"


def load_config(path: Path) -> dict:
    """Конфигурация сборки версии 1, проверенная JSON Schema до любой работы.

    Неизвестная версия отклоняется отдельным сообщением; true не считается версией 1.
    """
    config = read_json(path)
    version = config.get("config_version") if isinstance(config, dict) else None
    if type(version) is not int or version != SUPPORTED_CONFIG_VERSION:
        raise ManifestError(f"{path}: неподдерживаемая config_version {version!r}, ожидается {SUPPORTED_CONFIG_VERSION}")
    errors = sorted(Draft202012Validator(read_json(CONFIG_SCHEMA)).iter_errors(config), key=lambda error: list(error.absolute_path))
    if errors:
        raise ManifestError("\n".join(f"{path}: {'.'.join(map(str, error.absolute_path)) or '$'}: {error.message}" for error in errors))
    # Связь полей, которую не выразить схемой: интервалы для каждого вида транспорта и каждого периода.
    headways = config["model_assumptions"]["headway_minutes"]
    periods = set(headways["periods"])
    problems = []
    for mode in config["transit"]["route_modes"]:
        by_period = headways["by_mode"].get(mode)
        if by_period is None:
            problems.append(f"нет интервалов для вида {mode}")
        elif set(by_period) != periods:
            problems.append(f"{mode}: периоды {sorted(set(by_period))} не совпадают с {sorted(periods)}")
    if problems:
        raise ManifestError(f"{path}: model_assumptions.headway_minutes: " + "; ".join(problems))
    return config


def command_validate(args) -> int:
    manifest = validate_manifest(args.manifest, check_files=not args.metadata_only)
    mode = "только паспорт; файлы не проверялись" if args.metadata_only else "паспорт, файлы и SHA256"
    print(f"OK: {manifest['package_id']} · {manifest['package_version']} · {len(manifest['assets'])} файлов · {mode}")
    if manifest["kind"] == "fixture":
        print("Синтетический тестовый пакет: не является картой Москвы.")
    return 0


def _selected_sources(config, registry):
    selected = {}
    for role, source_id in config["sources"].items():
        if source_id not in registry:
            raise ManifestError(f"Источник {source_id} ({role}) отсутствует в реестре")
        selected[role] = registry[source_id]
    return selected


def command_fetch(args) -> int:
    registry = load_registry(args.registry)
    config = load_config(args.config)
    for role, source in _selected_sources(config, registry).items():
        path, downloaded = fetch(source, args.raw)
        print(f"{'загружен' if downloaded else 'уже есть'}: {role} · {source['source_id']} · {path}")
    return 0


def command_build(args) -> int:
    from .build import build_package, clip_region, make_boundary, prefilter_boundary
    from .geo import Projector

    if shutil.which("osmium") is None:
        raise ManifestError("Не найдена утилита osmium (osmium-tool)")
    registry = load_registry(args.registry)
    config = load_config(args.config)
    # ID и версия проверяются до любой записи на диск: и рабочий, и выходной каталог — внутри своих корней.
    out = package_dir(args.out, config.get("package_id"), config.get("package_version"))
    # Отдельный рабочий каталог на версию и конфигурацию: разные сборки не делят вырезку.
    from .build import config_digest
    work = work_dir(args.work, config["package_id"]) / f"{config['package_version']}-{config_digest(config)}"
    selected = _selected_sources(config, registry)
    osm = verify(selected["osm"], args.raw)
    raster_zip = verify(selected["population"], args.raw)
    work.mkdir(parents=True, exist_ok=True)
    # Одинаковые сборки, запущенные одновременно, ждут друг друга, а не пишут одни файлы.
    lock = (work / ".lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    # Любые сборки в один каталог пакета (даже с разной конфигурацией) устанавливают его по очереди.
    out.parent.mkdir(parents=True, exist_ok=True)
    package_lock = (out.parent / f".{out.name}.lock").open("w")
    fcntl.flock(package_lock, fcntl.LOCK_EX)
    projector = Projector(config["metric_crs"])

    print("Граница региона…")
    boundary_osm = work / "boundary-sources.osm.pbf"
    prefilter_boundary(osm, boundary_osm, config["boundary"])
    parts = make_boundary(config, boundary_osm, projector)
    region_wgs = projector.to_wgs84(parts["region"])
    print(f"  регион {parts['region'].area / 1e6:.0f} км², внутри МКАД {parts['mkad_outer'].area / 1e6:.0f} км²")
    print("Вырезка OSM…")
    region_osm = clip_region(osm, region_wgs, work)

    raster_name = selected["population"]["file"].removesuffix(".zip") + ".tif"
    raster = f"zip://{raster_zip}!/{raster_name}"
    processing = f"city_pipeline-{config['package_id']}-{config['package_version']}"
    sources = [manifest_source(selected[role], processing) for role in sorted(selected)]
    manifest, report = build_package(
        config, sources=sources, source_roles={role: source["source_id"] for role, source in selected.items()},
        kind="city_data", region_parts=parts, region_osm=region_osm, raster=raster, out_dir=out, created_at=_created_at(),
        config_path=args.config,
    )
    failed = [check["check"] for check in report["checks"] if check["status"] != "pass"]
    print(f"OK: {manifest['package_id']} · {manifest['package_version']} · {len(manifest['assets'])} файлов · {out}")
    print(f"Жителей {report['population']['allocated']}, зданий {report['buildings']['count']}, зон {report['zones']['count']}")
    if failed:
        print(f"Проверки отчёта не пройдены: {', '.join(failed)} — см. quality_report.md")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Городские пакеты Moscow Epidemic Sim")
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="Проверить паспорт и файлы пакета")
    validate.add_argument("manifest", type=Path, help="Путь к manifest.json")
    validate.add_argument("--metadata-only", action="store_true", help="Проверить только паспорт, без чтения файлов данных")
    validate.set_defaults(handler=command_validate)

    for name, handler, help_text in (("fetch", command_fetch, "Скачать источники сборки и проверить SHA256"),
                                     ("build", command_build, "Собрать городской пакет из зафиксированных источников")):
        sub = commands.add_parser(name, help=help_text)
        sub.add_argument("config", type=Path, help="Конфигурация сборки, например data/manifests/moscow-2021.json")
        sub.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY, help="Реестр источников")
        sub.add_argument("--raw", type=Path, default=ROOT / "data" / "raw", help="Каталог исходных выгрузок")
        if name == "build":
            sub.add_argument("--work", type=Path, default=ROOT / "data" / "processed", help="Каталог промежуточных файлов")
            sub.add_argument("--out", type=Path, default=ROOT / "data" / "packages", help="Каталог готовых пакетов")
        sub.set_defaults(handler=handler)

    args = parser.parse_args()
    try:
        return args.handler(args)
    except (ManifestError, ValueError, OSError, ShapelyError, subprocess.CalledProcessError) as error:
        print(f"Ошибка пакета:\n{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

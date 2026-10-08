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

from jsonschema import Draft202012Validator, validators
from shapely.errors import ShapelyError

from .manifest import ManifestError, parse_json_bytes, read_json, validate_manifest
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

# В схеме конфигурации integer — только целые Python (60, а не 60.0): дальше значения идут в numpy
# (размер выборки, seed), где float падает лишь в конце сборки. bool целым не считается.
StrictIntegerValidator = validators.extend(
    Draft202012Validator,
    type_checker=Draft202012Validator.TYPE_CHECKER.redefine(
        "integer", lambda _, value: isinstance(value, int) and not isinstance(value, bool)),
)


def load_config(path: Path, data: bytes | None = None) -> dict:
    """Конфигурация сборки версии 1, проверенная JSON Schema до любой работы.

    Неизвестная версия отклоняется отдельным сообщением; true не считается версией 1.
    data — уже прочитанные байты файла (один снимок для разбора и для хеша в паспорте).
    """
    if data is None:
        try:
            data = Path(path).read_bytes()
        except OSError as error:
            raise ManifestError(f"Не удалось прочитать {path}: {error}") from error
    config = parse_json_bytes(data, path)
    version = config.get("config_version") if isinstance(config, dict) else None
    if type(version) is not int or version != SUPPORTED_CONFIG_VERSION:
        raise ManifestError(f"{path}: неподдерживаемая config_version {version!r}, ожидается {SUPPORTED_CONFIG_VERSION}")
    errors = sorted(StrictIntegerValidator(read_json(CONFIG_SCHEMA)).iter_errors(config), key=lambda error: list(error.absolute_path))
    if errors:
        raise ManifestError("\n".join(f"{path}: {'.'.join(map(str, error.absolute_path)) or '$'}: {error.message}" for error in errors))
    try:
        from .geo import Projector
        Projector(config["metric_crs"])
    except Exception as error:   # неизвестная CRS (pyproj) или не метрическая (GeoError)
        raise ManifestError(f"{path}: metric_crs {config['metric_crs']!r} не подходит: {error}") from error
    problem = _check_day_partition(config["model_assumptions"]["headway_minutes"]["periods"])
    if problem:
        raise ManifestError(f"{path}: model_assumptions.headway_minutes.periods: {problem}")
    levels = config["buildings"]
    over = sorted(kind for kind, value in levels["default_levels"].items() if value > levels["max_levels"])
    if over:
        raise ManifestError(f"{path}: buildings.default_levels больше max_levels {levels['max_levels']}: {', '.join(over)}")
    shared_roles = sorted(set(config["transit"]["stop_roles"]) & set(config["transit"]["platform_roles"]))
    if shared_roles:
        raise ManifestError(f"{path}: transit: роли {shared_roles} указаны и как остановка, и как платформа")
    low, high = config["boundary"]["mkad_area_km2_range"]
    if low > high:
        raise ManifestError(f"{path}: boundary.mkad_area_km2_range: нижняя граница {low} больше верхней {high}")
    # Значение building относится ровно к одной функции: иначе результат зависел бы от порядка ключей JSON.
    seen = {}
    for function, values in config["buildings"]["tag_functions"].items():
        for value in values:
            if value in seen and seen[value] != function:
                raise ManifestError(f"{path}: buildings.tag_functions: значение {value!r} указано и в {seen[value]}, и в {function}")
            seen[value] = function
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


ROLE_FORMATS = {"osm": "osm-pbf", "population": "geotiff-in-zip"}   # формат карточки реестра для роли


def _selected_sources(config, registry):
    selected = {}
    for role, source_id in config["sources"].items():
        if source_id not in registry:
            raise ManifestError(f"Источник {source_id} ({role}) отсутствует в реестре")
        if registry[source_id]["format"] != ROLE_FORMATS[role]:
            raise ManifestError(f"Источник {source_id} имеет формат {registry[source_id]['format']}, а для роли {role} нужен {ROLE_FORMATS[role]}")
        selected[role] = registry[source_id]
    return selected


def population_raster(archive: Path) -> str:
    """Путь rasterio к единственному GeoTIFF в архиве, проверенный до тяжёлых этапов сборки."""
    import zipfile

    import rasterio

    try:
        with zipfile.ZipFile(archive) as bundle:
            members = sorted(name for name in bundle.namelist() if name.lower().endswith((".tif", ".tiff")))
    except (OSError, zipfile.BadZipFile) as error:
        raise ManifestError(f"{archive}: не удалось открыть архив сетки населения: {error}") from error
    if len(members) != 1:
        raise ManifestError(f"{archive}: в архиве должен быть ровно один GeoTIFF, найдено {members}")
    path = f"zip://{archive}!/{members[0]}"
    try:
        with rasterio.open(path) as dataset:
            transform = dataset.transform
            if transform.b != 0 or transform.d != 0 or transform.a <= 0 or transform.e >= 0:
                raise ManifestError(f"{path}: сетка должна быть без поворота, север вверху (получено {tuple(transform)[:6]})")
            if dataset.crs is None or dataset.crs.to_epsg() != 4326:
                raise ManifestError(f"{path}: сетка населения должна быть в EPSG:4326, получено {dataset.crs}")
    except rasterio.errors.RasterioError as error:
        raise ManifestError(f"{path}: не удалось открыть растр: {error}") from error
    return path


def _minutes(clock):
    hours, minutes = map(int, clock.split(":"))
    return hours * 60 + minutes


def _check_day_partition(periods):
    """Периоды интервалов должны разбивать сутки без перекрытий и пропусков (переход через полночь допустим)."""
    spans = []
    for name, (start, end) in periods.items():
        a, b = _minutes(start), _minutes(end)
        if a == b:
            return f"период {name} пустой ({start}–{end})"
        spans.append((a, (b - a) % 1440, name))
    if sum(length for _, length, _ in spans) != 1440:
        return f"периоды покрывают {sum(length for _, length, _ in spans)} минут вместо 1440 (перекрытие или пропуск)"
    spans.sort()
    for (a, length, name), (next_a, _, next_name) in zip(spans, spans[1:] + spans[:1]):
        if (a + length) % 1440 != next_a:
            return f"после периода {name} следует {next_name} не встык"
    return None


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
    config_bytes = args.config.read_bytes()   # один снимок: из него и разбор, и хеш в паспорте
    config = load_config(args.config, config_bytes)
    # ID и версия проверяются до любой записи на диск: и рабочий, и выходной каталог — внутри своих корней.
    out = package_dir(args.out, config.get("package_id"), config.get("package_version"))
    # Отдельный рабочий каталог на версию и конфигурацию: разные сборки не делят вырезку.
    from .build import config_digest
    work = work_dir(args.work, config["package_id"]) / f"{config['package_version']}-{config_digest(config)}"
    selected = _selected_sources(config, registry)
    osm = verify(selected["osm"], args.raw)
    raster_zip = verify(selected["population"], args.raw)
    raster = population_raster(raster_zip)   # архив и растр проверяются до границы и вырезки
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
    region_wgs = projector.to_wgs84_dense(parts["region"])   # контур вырезки повторяет метрическую границу
    print(f"  регион {parts['region'].area / 1e6:.0f} км², внутри МКАД {parts['mkad_outer'].area / 1e6:.0f} км²")
    print("Вырезка OSM…")
    region_osm = clip_region(osm, region_wgs, work)

    processing = f"city_pipeline-{config['package_id']}-{config['package_version']}"
    sources = [manifest_source(selected[role], processing) for role in sorted(selected)]
    manifest, report = build_package(
        config, sources=sources, source_roles={role: source["source_id"] for role, source in selected.items()},
        kind="city_data", region_parts=parts, region_osm=region_osm, raster=raster, out_dir=out, created_at=_created_at(),
        config_path=args.config, config_bytes=config_bytes,
    )
    failed = [check["check"] for check in report["checks"] if check["status"] != "pass"]
    print(f"OK: {manifest['package_id']} · {manifest['package_version']} · {len(manifest['assets'])} файлов · {out}")
    print(f"Жителей {report['population']['allocated']}, зданий {report['buildings']['count']}, зон {report['zones']['count']}")
    if failed:
        print(f"Проверки отчёта не пройдены: {', '.join(failed)} — см. quality_report.md")
    return 0


def command_map_tile(args) -> int:
    """Тестовый экспорт участков карты (#11) из собранного пакета и его вырезки OSM."""
    from .build import config_digest
    from .mapbuild import export_test_tiles

    config = load_config(args.config)
    package = package_dir(args.packages, config.get("package_id"), config.get("package_version"))
    validate_manifest(package / "manifest.json", check_files=True)
    # Пакет перезаписывается при той же версии, а вырезка OSM лежит по хешу конфигурации:
    # другая конфигурация дала бы чужую вырезку и CRS к этому пакету.
    packaged = load_config(package / "build_config.json")
    if config_digest(packaged) != config_digest(config):
        raise ManifestError(f"{args.config} не совпадает с конфигурацией, из которой собран {package} (build_config.json): пересоберите пакет командой build")
    region_pbf = work_dir(args.work, config["package_id"]) / f"{config['package_version']}-{config_digest(config)}" / "region.osm.pbf"
    if not region_pbf.is_file():
        raise ManifestError(f"Нет вырезки OSM {region_pbf}: сначала выполните build для этой конфигурации")
    try:
        lon, lat = (float(value) for value in args.at.split(","))
    except ValueError as error:
        raise ManifestError(f"--at ожидает «долгота,широта», получено {args.at!r}") from error
    out = args.out.resolve()
    index = export_test_tiles(package, region_pbf, config["metric_crs"], lon, lat, out)
    for tile in index["tiles"]:
        print(f"z{tile['level']} {tile['tile'][0]}_{tile['tile'][1]}: {tile['size_bytes'] / 1e6:.2f} МБ · {tile['counts']}")
    print(f"OK: {out / 'index.json'}")
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

    tile = commands.add_parser("map-tile", help="Тестовый экспорт участков карты вокруг точки (уровни 0–2)")
    tile.add_argument("config", type=Path, help="Конфигурация сборки собранного пакета")
    tile.add_argument("--at", default="37.6175,55.7520", help="Точка «долгота,широта» внутри региона (по умолчанию — центр Москвы)")
    tile.add_argument("--packages", type=Path, default=ROOT / "data" / "packages", help="Каталог готовых пакетов")
    tile.add_argument("--work", type=Path, default=ROOT / "data" / "processed", help="Каталог промежуточных файлов (вырезка OSM)")
    tile.add_argument("--out", type=Path, default=ROOT / "data" / "processed" / "map-test", help="Каталог участков")
    tile.set_defaults(handler=command_map_tile)

    args = parser.parse_args()
    try:
        return args.handler(args)
    except (ManifestError, ValueError, OSError, ShapelyError, subprocess.CalledProcessError) as error:
        print(f"Ошибка пакета:\n{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

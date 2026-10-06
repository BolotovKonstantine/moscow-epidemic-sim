"""CLI проверки, получения источников и сборки городских пакетов."""

import argparse
import os
import shutil
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

from .manifest import ManifestError, read_json, validate_manifest
from .sources import fetch, load_registry, manifest_source, verify

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = ROOT / "data" / "manifests" / "sources.json"


def _created_at() -> str:
    """Время сборки; SOURCE_DATE_EPOCH фиксирует его для побайтового сравнения паспортов."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    moment = datetime.fromtimestamp(int(epoch), UTC) if epoch else datetime.now(UTC).replace(microsecond=0)
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


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
    config = read_json(args.config)
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
    config = read_json(args.config)
    selected = _selected_sources(config, registry)
    osm = verify(selected["osm"], args.raw)
    raster_zip = verify(selected["population"], args.raw)
    work = args.work / config["package_id"]
    work.mkdir(parents=True, exist_ok=True)
    projector = Projector(config["metric_crs"])

    print("Граница региона…")
    boundary_osm = work / "boundary-sources.osm.pbf"
    prefilter_boundary(osm, boundary_osm)
    parts = make_boundary(config, boundary_osm, projector)
    region_wgs = projector.to_wgs84(parts["region"])
    print(f"  регион {parts['region'].area / 1e6:.0f} км², внутри МКАД {parts['mkad_outer'].area / 1e6:.0f} км²")
    print("Вырезка OSM…")
    region_osm = clip_region(osm, region_wgs, work)

    raster_name = selected["population"]["file"].removesuffix(".zip") + ".tif"
    raster = f"zip://{raster_zip}!/{raster_name}"
    processing = f"city_pipeline-{config['package_id']}-{config['package_version']}"
    sources = [manifest_source(selected[role], processing) for role in sorted(selected)]
    out = args.out / f"{config['package_id']}-{config['package_version']}"
    manifest, report = build_package(
        config, sources=sources, source_roles={role: source["source_id"] for role, source in selected.items()},
        kind="city_data", region_parts=parts, region_osm=region_osm, raster=raster, out_dir=out, created_at=_created_at(),
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
    except (ManifestError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Ошибка пакета:\n{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

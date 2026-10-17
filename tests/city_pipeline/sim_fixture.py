"""Расчётные таблицы синтетического мини-города для тестов загрузчика ядра (#37).

Пакет собирается тем же `build_package`, что и настоящий, из синтетического мини-города
(synthetic_city.py), поэтому проверяет совместимость Python-экспорта `sim/` и C++-чтения. Это не
карта Москвы. В tests/fixtures/sim_mini/ лежат только `manifest.json` пакета и каталог `sim/`:
загрузчику ядра нужны паспорт (пакет, версия, хеш индекса) и таблицы, остальные файлы пакета
в фикстуру не входят, поэтому полная проверка паспорта к ней не применяется.
Тест test_sim_fixture проверяет, что закоммиченные файлы совпадают с пересборкой.

Пересобрать: .venv/bin/python -m tests.city_pipeline.sim_fixture
"""

import shutil
import sys
import tempfile
from pathlib import Path

from tests.city_pipeline.synthetic_city import SOURCE_ROLES, build_city, mini_city_config, synthetic_census, synthetic_sources
from tools.city_pipeline.build import build_package, make_boundary
from tools.city_pipeline.geo import Projector

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "fixtures" / "sim_mini"
CREATED_AT = "2026-10-06T00:00:00Z"


def build_fixture(out: Path):
    """Собрать мини-город во временном каталоге и положить в out паспорт и sim/."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        osm, raster = build_city(root)
        config = mini_city_config()
        parts = make_boundary(config, osm, Projector(config["metric_crs"]))
        package = root / "package"
        build_package(config, sources=synthetic_sources(), source_roles=SOURCE_ROLES, kind="fixture", region_parts=parts,
                      region_osm=osm, raster=raster, census=synthetic_census(), out_dir=package, created_at=CREATED_AT,
                      log=lambda *_: None)
        out.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(package / "manifest.json", out / "manifest.json")
        shutil.copytree(package / "sim", out / "sim")


def fixture_files(directory: Path) -> dict:
    return {path.relative_to(directory).as_posix(): path.read_bytes() for path in sorted(directory.rglob("*")) if path.is_file()}


def main():
    if FIXTURE_DIR.exists():
        shutil.rmtree(FIXTURE_DIR)
    build_fixture(FIXTURE_DIR)
    files = fixture_files(FIXTURE_DIR)
    print(f"{FIXTURE_DIR}: {len(files)} файлов, {sum(len(data) for data in files.values())} байт")
    return 0


if __name__ == "__main__":
    sys.exit(main())

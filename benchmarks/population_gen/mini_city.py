"""Собрать синтетический мини-город тестов пайплайна в каталог пакета — как в tests/city_pipeline/test_build.py.

    .venv/bin/python benchmarks/population_gen/mini_city.py <каталог вывода>
"""

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.city_pipeline.synthetic_city import SOURCE_ROLES, build_city, mini_city_config, synthetic_census, synthetic_sources  # noqa: E402
from tools.city_pipeline.build import build_package, make_boundary  # noqa: E402
from tools.city_pipeline.geo import Projector  # noqa: E402


def main(out_dir: Path):
    with tempfile.TemporaryDirectory() as temporary:
        osm, raster = build_city(Path(temporary))
        config = mini_city_config()
        parts = make_boundary(config, osm, Projector(config["metric_crs"]))
        build_package(config, sources=synthetic_sources(), source_roles=SOURCE_ROLES,
                      kind="fixture", region_parts=parts, region_osm=osm, raster=raster, census=synthetic_census(), out_dir=out_dir,
                      created_at="2026-10-06T00:00:00Z", log=lambda *_: None)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))

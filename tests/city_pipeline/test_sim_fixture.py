import tempfile
import unittest
from pathlib import Path

from tests.city_pipeline.sim_fixture import FIXTURE_DIR, build_fixture, fixture_files


class SimFixtureTests(unittest.TestCase):
    def test_committed_fixture_matches_rebuild(self):
        # Фикстура загрузчика ядра (#37) пишется экспортом пакета: формат таблиц и запись не должны расходиться с ней.
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "sim_mini"
            build_fixture(out)
            expected = fixture_files(out)
        actual = fixture_files(FIXTURE_DIR)
        self.assertEqual(sorted(actual), sorted(expected), "состав tests/fixtures/sim_mini устарел: .venv/bin/python -m tests.city_pipeline.sim_fixture")
        for name, data in expected.items():
            self.assertEqual(actual[name], data, f"{name} устарел: .venv/bin/python -m tests.city_pipeline.sim_fixture")


if __name__ == "__main__":
    unittest.main()

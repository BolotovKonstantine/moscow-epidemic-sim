"""CLI проверки городских пакетов."""

import argparse
import sys
from pathlib import Path

from .manifest import ManifestError, validate_manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Проверка городского пакета Moscow Epidemic Sim")
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="Проверить паспорт и файлы пакета")
    validate.add_argument("manifest", type=Path, help="Путь к manifest.json")
    validate.add_argument("--metadata-only", action="store_true", help="Проверить только паспорт, без чтения файлов данных")
    args = parser.parse_args()
    try:
        manifest = validate_manifest(args.manifest, check_files=not args.metadata_only)
    except ManifestError as error:
        print(f"Ошибка пакета:\n{error}", file=sys.stderr)
        return 1
    mode = "только паспорт; файлы не проверялись" if args.metadata_only else "паспорт, файлы и SHA256"
    print(f"OK: {manifest['package_id']} · {manifest['package_version']} · {len(manifest['assets'])} файлов · {mode}")
    if manifest["kind"] == "fixture":
        print("Синтетический тестовый пакет: не является картой Москвы.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

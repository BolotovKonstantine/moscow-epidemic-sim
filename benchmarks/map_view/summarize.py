"""Сводка замера просмотра карты (#17): прогоны run-<n>.json, пиковая память процесса из
/usr/bin/time -l, устройство, коммит и пакет. Пишет summary.json и summary.md в каталог прогонов
и печатает таблицу. Только стандартная библиотека; запускает benchmarks/map_view/run.sh.

    python3.14 -I summarize.py <путь к .app> <каталог прогонов>
"""

import json
import re
import subprocess
import sys
from pathlib import Path

SCENARIO_ROWS = [
    ("Кадр, среднее, мс", lambda s: s["frame_ms"]["mean"], "{:.2f}"),
    ("Кадр, 95 %, мс", lambda s: s["frame_ms"]["p95"], "{:.2f}"),
    ("Кадр, 99 %, мс", lambda s: s["frame_ms"]["p99"], "{:.2f}"),
    ("Кадр, макс., мс", lambda s: s["frame_ms"]["max"], "{:.1f}"),
    ("FPS, среднее", lambda s: s["fps_mean"], "{:.0f}"),
    ("Кадров дольше 16,7 мс", lambda s: f"{s['slow_frames']} из {s['frames']}", "{}"),
    ("Отрисовка (CPU), 95 %, мс", lambda s: s["render_cpu_ms"]["p95"], "{:.2f}"),
    ("Кадров с недогруженными участками", lambda s: s["missing_frames"], "{}"),
    ("Подключено участков", lambda s: s["tiles_attached"], "{}"),
    ("Подключение за кадр, 95 % / макс., мс",
     lambda s: f"{s['attach_poll_ms']['p95']:.1f} / {s['attach_poll_ms']['max']:.1f}" if s["attach_poll_ms"]["count"] else "—",
     "{}"),
    ("Загрузка участка (поток), медиана / 95 % / макс., мс",
     lambda s: (f"{s['tile_load_ms']['p50']:.0f} / {s['tile_load_ms']['p95']:.0f} / {s['tile_load_ms']['max']:.0f}"
                if s["tile_load_ms"]["count"] else "—"),
     "{}"),
    ("Кеш участков, пик, МБ", lambda s: s["cache_peak_mb"], "{:.0f}"),
    ("Видеопамять, пик, МБ", lambda s: s["video_peak_mb"], "{:.0f}"),
]


def run(args: list[str], cwd: Path | None = None) -> str:
    try:
        return subprocess.run(args, cwd=cwd, capture_output=True, text=True, check=False).stdout.strip()
    except OSError:
        return ""


def time_stats(path: Path) -> dict:
    """Пиковая память процесса из вывода /usr/bin/time -l (macOS): байты → МБ."""
    text = path.read_text(errors="replace") if path.exists() else ""
    result = {}
    for key, pattern in (("max_rss_mb", r"(\d+)\s+maximum resident set size"),
                         ("peak_footprint_mb", r"(\d+)\s+peak memory footprint")):
        match = re.search(pattern, text)
        if match:
            result[key] = int(match.group(1)) / 1048576
    match = re.search(r"([\d.]+)\s+real", text)
    if match:
        result["wall_s"] = float(match.group(1))
    return result


def device() -> dict:
    power = run(["pmset", "-g", "ps"]).splitlines()
    return {
        "model": run(["sysctl", "-n", "hw.model"]),
        "cpu": run(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "memory_gb": int(run(["sysctl", "-n", "hw.memsize"]) or 0) / 2**30,
        "os": f"{run(['sw_vers', '-productName'])} {run(['sw_vers', '-productVersion'])} ({run(['sw_vers', '-buildVersion'])})",
        "power": power[0].removeprefix("Now drawing from ").strip("'") if power else "",
    }


def build_commit(app: Path) -> dict:
    """Коммит, из которого собран экспорт (run.sh пишет его при экспорте рядом с .app)."""
    stamp = app.with_name(app.name + ".commit.json")
    return json.loads(stamp.read_text()) if stamp.exists() else {"commit": "unknown", "dirty": None}


def fmt_mb(value) -> str:
    """Нет значения (не измеряется или не разобрано из вывода time) — «нет данных», а не 0."""
    return "нет данных" if value is None else f"{value:.0f}"


def cell(value, fmt: str) -> str:
    return fmt.format(value) if not isinstance(value, str) else value


def markdown(summary: dict) -> str:
    runs = summary["runs"]
    first = runs[0]
    lines = [
        f"# Замер просмотра карты — {summary['time']}",
        "",
        f"- Устройство: {summary['device']['model']}, {summary['device']['cpu']}, "
        f"{summary['device']['memory_gb']:.0f} ГБ; {summary['device']['os']}; питание: {summary['device']['power']}.",
        f"- Сборка: экспорт {first['build']} ({'редактор' if first['editor'] else 'не редактор'}), Godot {first['godot']}, "
        f"{first['renderer']}, {first['video_adapter']}; экспорт из коммита {summary['git']['commit'][:10]}"
        f"{' (с незакоммиченными изменениями)' if summary['git']['dirty'] else ''}.",
        f"- Пакет: {first['package_id']} {first['package_version']}; участки на диске: "
        + ", ".join(f"уровень {level} — {item['tiles']} шт., {item['bytes'] / 1048576:.1f} МБ"
                    for level, item in sorted(first["tiles_on_disk"].items())) + ".",
        f"- Окно: {first['window']} — {first['window_px'][0]}×{first['window_px'][1]} физ. пикс. (по умолчанию у игры "
        f"{first['default_window_px'][0]}×{first['default_window_px'][1]}; масштаб экрана {first['screen_scale']:g}, "
        f"вид {first['viewport'][0]:.0f}×{first['viewport'][1]:.0f}); vsync выключен; кеш участков до {first['cache_limit_mb']} МБ.",
        "",
        "| Прогон | Обзор при старте, мс | Пик кеша, МБ | Статическая память Godot, пик, МБ (только debug) | Процесс: макс. RSS, МБ | Процесс: пик footprint, МБ |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for n, item in enumerate(runs, 1):
        proc = item.get("process", {})
        lines.append(f"| {n} | {item['overview_load_ms']:.0f} | {item['cache_peak_total_mb']:.0f} | "
                     f"{fmt_mb(item['static_memory_peak_mb'])} | {fmt_mb(proc.get('max_rss_mb'))} | "
                     f"{fmt_mb(proc.get('peak_footprint_mb'))} |")
    for scenario in first["scenarios"]:
        name = scenario["name"]
        per_run = [next(s for s in item["scenarios"] if s["name"] == name) for item in runs]
        frames = ", ".join(str(s["frames"]) for s in per_run)
        durations = ", ".join(f"{s['duration_s']:.1f}" for s in per_run)
        lines += ["", f"## {scenario['title']} (`{name}`)", "",
                  f"Кадров: {frames}; длительность, с: {durations}.", "",
                  "| Метрика | " + " | ".join(f"Прогон {n}" for n in range(1, len(runs) + 1)) + " |",
                  "| --- |" + " --- |" * len(runs)]
        for title, getter, fmt in SCENARIO_ROWS:
            lines.append(f"| {title} | " + " | ".join(cell(getter(s), fmt) for s in per_run) + " |")
    return "\n".join(lines) + "\n"


def main() -> int:
    app, out = (Path(arg) for arg in sys.argv[1:3])
    runs = []
    for path in sorted(out.glob("run-*.json"), key=lambda p: int(p.stem.split("-")[1])):
        item = json.loads(path.read_text())
        item["process"] = time_stats(path.with_suffix(".time.txt"))
        if len(item["process"]) < 3:
            print(f"{path.name}: в выводе /usr/bin/time -l нет части метрик процесса — в сводке «нет данных»",
                  file=sys.stderr)
        runs.append(item)
    if not runs:
        print(f"В {out} нет результатов прогонов (см. run-*.log и run-*.time.txt)", file=sys.stderr)
        return 1
    summary = {
        "format": "mesim-map-bench-summary",
        "time": out.name,
        "device": device(),
        "git": build_commit(app),
        "app": str(app),
        "runs": runs,
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    text = markdown(summary)
    (out / "summary.md").write_text(text)
    print(text)
    print(f"Сводка: {out / 'summary.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Сводка замера приёмки этапа 3 (#49): прогоны ядра без интерфейса (core-<n>.json, ряд core-<n>.csv),
прогоны release-экспорта с окном (app-<n>.json) и без (app-headless.json), пиковая память процессов из
/usr/bin/time -l, устройство, коммит, пакет и размеры. Сверяет хеши контрольных точек всех прогонов и
сравнивает с инженерными целями. Пишет summary.json и summary.md в каталог прогонов. Только стандартная
библиотека; запускает benchmarks/full_region/run.sh.

    python3.14 -I summarize.py <каталог пакета> <каталог прогонов>
"""

import json
import re
import subprocess
import sys
from pathlib import Path

FRAME_TARGET_MS = 1000 / 60      # 60 FPS на ПК (docs/architecture.md)
DAY_TARGET_S = (5.0, 15.0)       # игровые сутки за 5–15 с
WEEKDAYS = ["пн", "вт", "ср", "чт", "пт", "сб", "вс"]


def run(args: list[str]) -> str:
    try:
        return subprocess.run(args, capture_output=True, text=True, check=False).stdout.strip()
    except OSError:
        return ""


def time_stats(path: Path) -> dict:
    """Пиковая память и время процесса из вывода /usr/bin/time -l (macOS): байты → МБ."""
    text = path.read_text(errors="replace") if path.exists() else ""
    result = {}
    for key, pattern in (("max_rss_mb", r"(\d+)\s+maximum resident set size"),
                         ("peak_footprint_mb", r"(\d+)\s+peak memory footprint")):
        match = re.search(pattern, text)
        if match:
            result[key] = int(match.group(1)) / 1e6
    match = re.search(r"([\d.]+)\s+real", text)
    if match:
        result["wall_s"] = float(match.group(1))
    return result


def device() -> dict:
    power = run(["pmset", "-g", "ps"]).splitlines()
    return {
        "model": run(["sysctl", "-n", "hw.model"]),
        "cpu": run(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "cores": run(["sysctl", "-n", "hw.ncpu"]),
        "memory_gb": int(run(["sysctl", "-n", "hw.memsize"]) or 0) / 2**30,
        "os": f"{run(['sw_vers', '-productName'])} {run(['sw_vers', '-productVersion'])} ({run(['sw_vers', '-buildVersion'])})",
        "power": power[0].removeprefix("Now drawing from ").strip("'") if power else "",
    }


def dir_mb(path: Path) -> float | None:
    if not path.exists():
        return None
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file()) / 1e6


def package_info(package: Path, store: Path) -> dict:
    manifest = json.loads((package / "manifest.json").read_text())
    return {
        "dir": str(package),
        "package_id": manifest.get("package_id"),
        "package_version": manifest.get("package_version"),
        "index_sha256": run(["shasum", "-a", "256", str(package / "sim" / "index.json")]).split(" ")[0],
        "size_mb": dir_mb(package),
        "sim_mb": dir_mb(package / "sim"),
        "map_mb": dir_mb(package / "map"),
        "store_mb": dir_mb(store),
    }


def load(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        item = json.loads(path.read_text())
    except json.JSONDecodeError as error:
        print(f"{path.name}: не JSON ({error}) — прогон не завершён? См. вывод рядом", file=sys.stderr)
        return None
    item["process"] = time_stats(path.with_suffix(".time.txt"))
    return item


def hashes_of_core(item: dict) -> dict[int, str]:
    return {int(c["time"]): c["hash"] for c in item.get("checkpoints", [])}


def hashes_of_app(item: dict) -> dict[int, str]:
    return {int(s["checkpoint"]["time"]): s["checkpoint"]["hash"] for s in item.get("scenarios", []) if s.get("checkpoint")}


def compare_hashes(core: list[dict], apps: dict[str, dict]) -> dict:
    """Контрольные времена прогонов приложения: хеш каждого прогона и ядра; совпадают ли все."""
    sources = {f"core-{n}": hashes_of_core(item) for n, item in enumerate(core, 1)}
    sources.update({name: hashes_of_app(item) for name, item in apps.items()})
    times = sorted({t for name, h in sources.items() if not name.startswith("core") for t in h})
    if not times:  # без приложения — сверка прогонов ядра между собой
        times = sorted({t for h in sources.values() for t in h})
    rows = []
    for t in times:
        values = {name: h.get(t) for name, h in sources.items() if t in h}
        # Один источник — сверять не с чем (None), а не «не совпадает».
        rows.append({"time": t, "hashes": values, "equal": len(set(values.values())) == 1 if len(values) > 1 else None})
    compared = [r for r in rows if r["equal"] is not None]
    return {"rows": rows, "compared": len(compared), "all_equal": bool(compared) and all(r["equal"] for r in compared)}


def fmt(value, spec: str = "{:.1f}") -> str:
    return "нет данных" if value is None else spec.format(value)


def clock(t: int) -> str:
    day, rest = divmod(t, 86400)
    return f"{WEEKDAYS[day % 7]} {rest // 3600:02d}:{rest % 3600 // 60:02d}" + (f" (сутки {day + 1})" if day else "")


def mil(value: int) -> str:
    return f"{value / 1e6:.2f} млн" if value >= 1e6 else f"{value / 1e3:.0f} тыс."


def markdown(s: dict) -> str:
    dev, pkg = s["device"], s["package"]
    lines = [
        f"# Замер приёмки этапа 3 — {s['time']}",
        "",
        f"- Устройство: {dev['model']}, {dev['cpu']} ({dev['cores']} ядер), {dev['memory_gb']:.0f} ГБ; {dev['os']}; "
        f"питание: {dev['power']}.",
        f"- Память машины перед замером: {' · '.join((s.get('memory_before') or 'нет данных').splitlines())}.",
        f"- Исходники: коммит {s['source']['commit'][:10]}{' с незакоммиченными изменениями' if s['source']['dirty'] else ''}"
        + (f"; экспорт приложения — {s['app_commit']['commit'][:10]}{' (грязный)' if s['app_commit'].get('dirty') else ''}"
           if s.get("app_commit") else "") + ".",
        f"- Пакет: {pkg['package_id']} {pkg['package_version']}, SHA256 sim/index.json {pkg['index_sha256'][:12]}; "
        f"размер {fmt(pkg['size_mb'], '{:.0f}')} МБ (sim/ {fmt(pkg['sim_mb'], '{:.0f}')}, map/ {fmt(pkg['map_mb'], '{:.0f}')}); "
        f"хранилище мест и планов {fmt(pkg['store_mb'], '{:.0f}')} МБ.",
    ]
    core = s["core"]
    if core:
        first = core[0]
        lines += [
            f"- Сценарий: {first['scenario'].split('/')[-1]}, seed {first['seed']}, интервал {first['interval_s']} с; "
            f"жителей {first['persons']:,}".replace(",", " ") + f"; потоков — все ядра.",
            "",
            "## Ядро без интерфейса (release, CMake `-O3`)",
            "",
            "| Прогон | Старт, с | Весь сценарий, с | Сутки в среднем, с | Интервал p50 / p95 / макс., мс | Пик footprint, МБ | Байт на жителя (ядро / footprint) | Численность и нарушения |",
            "| --- | --- | --- | --- | --- | --- | --- | --- |",
        ]
        for n, item in enumerate(core, 1):
            t, b = item["total"], item["bytes_per_person"]
            lines.append(
                f"| {n} | {item['prepare']['s']:.1f} | {t['s']:.0f} | {t['game_day_s']:.0f} | "
                f"{t['interval_ms']['p50']:.0f} / {t['interval_ms']['p95']:.0f} / {t['interval_ms']['max']:.0f} | "
                f"{fmt(item['process'].get('peak_footprint_mb'), '{:.0f}')} | {b['core_end']:.0f} / {b['footprint_end']:.0f} | "
                f"{'сошлась' if item['unconserved_intervals'] == 0 else 'НЕ сошлась'}, нарушений {item['violations']} |")
        lines += ["", "### По суткам (прогон 1)", "",
                  "| Сутки | Счёт, с | из него новые пути, с | Интервал p50 / p95 / макс., мс | События жителей / рейсов | Поездок (транспортом) | Посадок | Групп (рейс, вагон) | Новый путь | Отмен |",
                  "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
        for d in first["days"]:
            lines.append(
                f"| {WEEKDAYS[d['day'] % 7]} | {d['s']:.0f} | {d['reroute_s']:.0f} | "
                f"{d['interval_ms']['p50']:.0f} / {d['interval_ms']['p95']:.0f} / {d['interval_ms']['max']:.0f} | "
                f"{mil(d['resident_events'])} / {mil(d['run_events'])} | {mil(d['trips'])} ({mil(d['transit_trips'])}) | "
                f"{mil(d['boardings'])} | {mil(d['groups'])} | {mil(d['rerouted'])} | {mil(d['cancelled'])} |")
        if len(core) > 1:
            lines += ["", "Время суток по прогонам, с: " + "; ".join(
                f"прогон {n}: " + ", ".join(f"{d['s']:.0f}" for d in item["days"]) for n, item in enumerate(core, 1)) + "."]
        if "morning_peak" in first:
            p = first["morning_peak"]
            lines += ["", f"Утренний пик понедельника 7:00–10:00: {p['s']:.0f} с, {p['intervals']} интервалов, p50 / p95 / макс. "
                      f"{p['interval_ms']['p50']:.0f} / {p['interval_ms']['p95']:.0f} / {p['interval_ms']['max']:.0f} мс; новые пути "
                      f"{p['reroute_s']:.0f} с; событий жителей {mil(p['resident_events'])}, рейсов {mil(p['run_events'])}."]
        m0, m1 = first["prepare"]["memory_mb"], first["memory_end_mb"]
        lines += ["", "### Память ядра по частям, МБ (прогон 1)", "",
                  "| Часть | После старта | В конце |", "| --- | --- | --- |"]
        names = {"population": "Жители (SoA), семьи, здания", "network": "Сеть маршрутов", "car_ch": "CH машины",
                 "places": "Места и коллективы", "visits": "Кандидаты посещений", "trips": "Регулярные поездки (планы)",
                 "residents": "Состояние движения жителей", "event_queue": "Очередь событий",
                 "runs_queues": "Рейсы, очереди, журналы", "path_cache": "Кеш путей",
                 "snapshot_index": "Привязка для снимков", "total": "**Всего**"}
        for key, title in names.items():
            lines.append(f"| {title} | {m0[key]:.0f} | {m1[key]:.0f} |")
        snap = first["snapshot_aggregates_ms"]
        lines += ["", f"Снимок агрегатов (итоги и участки 2 км) на контрольных временах: медиана {snap['p50']:.0f} мс, "
                  f"худший {snap['max']:.0f} мс ({snap['count']} снимков). Наибольшая очередь — {first['max_queue']}, "
                  f"загрузка рейса — {first['max_onboard']}, группа за рейс — {first['max_group']}."]
    apps = s["apps"]
    if apps:
        any_app = next(iter(apps.values()))
        sess = any_app.get("session") or {}
        lines += ["", "## Приложение (release-экспорт)", "",
                  f"Godot {any_app['godot']}, {any_app['build']}, {any_app['renderer']}, {any_app['video_adapter']}; окно "
                  f"{any_app['window']} — {any_app['window_px'][0]}×{any_app['window_px'][1]} физ. пикс. (масштаб экрана "
                  f"{any_app['screen_scale']:g}); vsync выключен. Сессия: {sess.get('package', '')}, жителей {sess.get('persons', 0)}.",
                  "",
                  "| Прогон | Подготовка сессии, с | Пик footprint процесса, МБ | Макс. RSS, МБ | Время прогона, с |",
                  "| --- | --- | --- | --- | --- |"]
        for name, item in apps.items():
            p = item["process"]
            lines.append(f"| {name} | {fmt((item.get('session') or {}).get('prepare_s'))} | "
                         f"{fmt(p.get('peak_footprint_mb'), '{:.0f}')} | {fmt(p.get('max_rss_mb'), '{:.0f}')} | "
                         f"{fmt(p.get('wall_s'), '{:.0f}')} |")
        windowed = [(n, a) for n, a in apps.items() if not a.get("headless")]
        for name, item in windowed:
            lines += ["", f"### {name}: кадр и темп", "",
                      "| Сценарий | Кадров | Кадр среднее / p95 / p99 / макс., мс | FPS | Кадров > 16,7 мс | Интервалов в секунду | Снимков | Заполнение снимка p50 / макс., мс | Перекладка p50 / макс., мс | Точек на экране, макс. |",
                      "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
            for sc in item["scenarios"]:
                f = sc["frame_ms"]
                ips = sc.get("intervals_per_s")
                lines.append(
                    f"| {sc['title']} | {sc['frames']} | {f['mean']:.1f} / {f['p95']:.1f} / {f['p99']:.1f} / {f['max']:.0f} | "
                    f"{sc['fps_mean']:.0f} | {sc['slow_frames']} | {fmt(ips, '{:.2f}') if sc.get('until') else 'пауза'} | "
                    f"{sc.get('snapshots', 0)} | {sc['snapshot_fill_ms']['p50']:.0f} / {sc['snapshot_fill_ms']['max']:.0f} | "
                    f"{sc['snapshot_convert_ms']['p50']:.1f} / {sc['snapshot_convert_ms']['max']:.1f} | "
                    f"{sc['points_shown']['max']:.0f} |")
    h = s["hashes"]
    if h["rows"]:
        names = sorted({n for r in h["rows"] for n in r["hashes"]})
        lines += ["", "## Контрольные состояния", "",
                  "| Время | " + " | ".join(names) + " | Совпадают |", "| --- |" + " --- |" * (len(names) + 1)]
        for r in h["rows"]:
            lines.append(f"| {clock(r['time'])} | " + " | ".join(r["hashes"].get(n, "—")[:8] if r["hashes"].get(n) else "—"
                                                             for n in names) + f" | {'—' if r['equal'] is None else 'да' if r['equal'] else '**нет**'} |")
        if h["compared"]:
            lines += ["", f"Все контрольные хеши совпадают: {'да' if h['all_equal'] else '**нет**'} ({h['compared']} времён "
                      "с двумя источниками и больше)."]
    t = s["targets"]
    lines += ["", "## Сравнение с инженерными целями", ""]
    if "game_day_s" in t:
        weekend = (f", выходной — {t['weekend_day_s'][0]:.0f}–{t['weekend_day_s'][1]:.0f} с" if t["weekend_day_s"] else "")
        lines.append(f"- Игровые сутки за {DAY_TARGET_S[0]:.0f}–{DAY_TARGET_S[1]:.0f} с: будний день — "
                     f"{t['weekday_day_s'][0]:.0f}–{t['weekday_day_s'][1]:.0f} с{weekend}; среднее по сценарию {t['game_day_s']:.0f} с — "
                     f"{'в цели' if t['game_day_s'] <= DAY_TARGET_S[1] else f'в {t["game_day_s"] / DAY_TARGET_S[1]:.0f} раз медленнее верхней границы'}.")
    if "frames" in t:
        lines.append(f"- 60 FPS (кадр ≤ {FRAME_TARGET_MS:.1f} мс): сценариев с p95 кадра в пределе — "
                     f"{t['frames']['p95_ok']} из {t['frames']['total']}; худший p95 — {t['frames']['worst_p95']:.1f} мс "
                     f"({t['frames']['worst_name']}).")
    return "\n".join(lines) + "\n"


def targets(core: list[dict], apps: dict[str, dict]) -> dict:
    out = {}
    if core:
        days = [d for item in core for d in item["days"]]
        weekday = [d["s"] for d in days if d["day"] % 7 < 5]
        weekend = [d["s"] for d in days if d["day"] % 7 >= 5]
        out["game_day_s"] = sum(item["total"]["game_day_s"] for item in core) / len(core)
        out["weekday_day_s"] = [min(weekday), max(weekday)] if weekday else []
        out["weekend_day_s"] = [min(weekend), max(weekend)] if weekend else []
    scenarios = [(name, sc) for name, item in apps.items() if not item.get("headless") for sc in item["scenarios"]]
    if scenarios:
        worst = max(scenarios, key=lambda x: x[1]["frame_ms"]["p95"])
        out["frames"] = {"total": len(scenarios),
                         "p95_ok": sum(sc["frame_ms"]["p95"] <= FRAME_TARGET_MS for _, sc in scenarios),
                         "worst_p95": worst[1]["frame_ms"]["p95"], "worst_name": f"{worst[0]}: {worst[1]['name']}"}
    return out


def main() -> int:
    package, out = (Path(arg) for arg in sys.argv[1:3])
    source = json.loads((out / "source.json").read_text())
    core = [item for path in sorted(out.glob("core-*.json"), key=lambda p: int(p.stem.split("-")[1]))
            if (item := load(path)) is not None]
    apps = {}
    for path in sorted(out.glob("app-*.json")):
        if (item := load(path)) is not None:
            apps[path.stem] = item
    if not core and not apps:
        print(f"В {out} нет результатов прогонов (см. *.log и *.time.txt)", file=sys.stderr)
        return 1
    app_commit = out / "app.commit.json"
    summary = {
        "format": "mesim-full-region-summary",
        "time": out.name,
        "device": device(),
        "source": source,
        "app_commit": json.loads(app_commit.read_text()) if app_commit.exists() else None,
        "package": package_info(package, Path(source["store"])),
        "memory_before": (out / "memory-before.txt").read_text().strip() if (out / "memory-before.txt").exists() else None,
        "core": core,
        "apps": apps,
        "hashes": compare_hashes(core, apps),
        "targets": targets(core, apps),
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    text = markdown(summary)
    (out / "summary.md").write_text(text)
    print(text)
    print(f"Сводка: {out / 'summary.md'}")
    return 0 if summary["hashes"]["all_equal"] or not summary["hashes"]["compared"] else 1


if __name__ == "__main__":
    sys.exit(main())

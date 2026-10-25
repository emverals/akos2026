#!/usr/bin/env python3
"""Проверка инвариантов боя по логу «Танковых боёв» (вариант 24).

Формат лога задаётся JSON-файлом регулярных выражений (по умолчанию
log_format.json рядом со скриптом); логика проверок от разметки не зависит.
Каждая проверка выдаёт PASS / FAIL / UNVERIFIED. UNVERIFIED — формат не даёт
нужных данных (регулярка не задана или ничего не нашла): это не успех, а
пометка «по логу не проверить».

Запуск: python3 -I check_invariants.py [опции] LOG
        python3 -I check_invariants.py --self-test   # проверки краснеют на испорченных логах
Код выхода: 0 — нарушений нет; 1 — есть FAIL; 2 — ошибка вызова/формата;
            3 — нарушений нет, но есть UNVERIFIED и задан --strict.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

PASS, FAIL, UNVERIFIED = "PASS", "FAIL", "UNVERIFIED"

# Ключи формата. Группа tick в любом событии задаёт такт строки; без неё
# берётся такт последнего заголовка снимка (ключ snapshot).
FORMAT_KEYS = {
    "seed": "группа seed (может встречаться в нескольких строках — все обязаны совпасть)",
    "field": "группы rows, cols",
    "obstacle_line": "строка со списком препятствий",
    "obstacle": "группы row, col; ищется только в строках obstacle_line (если задан)",
    "snapshot": "группа tick: заголовок снимка поля ПОСЛЕ такта tick (tick=0 — расстановка)",
    "position": "строка снимка: группы id, x, y; опц. team",
    "shot": "выстрел: группы tick, shooter",
    "hit": "попадание: группы tick, shooter, target",
    "miss": "выстрел, не попавший в цель (при прямой наводке = выстрел без видимой цели)",
    "damage": "повреждение: группы tick, id, dmg, before, after",
    "destroyed": "уничтожение: группы tick, id, опц. team",
    "action": "любое действие танка: группы tick, id",
    "late_action": "действие фаз ПОСЛЕ уничтожения (наведение, перемещение): группы tick, id; "
                   "танк, уничтоженный в такте T, в T их уже не выполняет",
    "violation": "самодиагностика программы о нарушении инварианта",
    "summary_marker": "начало итоговой статистики",
    "summary_alive": "группы team, alive",
    "summary_ticks": "группа ticks",
    "end_reason": "группа reason (значения переводит end_reason_map)",
    "team_size": "группы team, count — исходный размер команды",
}


@dataclass
class Fmt:
    regex: dict
    position_units: str = "chars"   # chars: x,y — левый верхний угол в символах; cells: x=col, y=row
    tank_w: int = 11
    tank_h: int = 5
    cell_w: int = 11
    cell_h: int = 5
    los_blocks_tanks: bool = True   # луч останавливается на первом танке
    end_reason_map: dict = field(default_factory=dict)  # значение -> destroyed|limit|interrupted|other
    confirmed: bool = False

    @staticmethod
    def from_dict(raw: dict) -> "Fmt":
        rx = {}
        for key, pat in (raw.get("regex") or {}).items():
            if key not in FORMAT_KEYS:
                raise SystemExit(f"log_format: неизвестный ключ regex {key!r}")
            if pat:
                try:
                    rx[key] = re.compile(pat)
                except re.error as exc:
                    raise SystemExit(f"log_format: плохая регулярка {key!r}: {exc}")
        opts = {k: raw[k] for k in ("position_units", "tank_w", "tank_h", "cell_w", "cell_h",
                                     "los_blocks_tanks", "end_reason_map", "confirmed") if k in raw}
        return Fmt(regex=rx, **opts)

    @staticmethod
    def load(path: str) -> "Fmt":
        with open(path, encoding="utf-8") as fh:
            return Fmt.from_dict(json.load(fh))


@dataclass
class Tank:
    tid: str
    team: str | None
    x: int
    y: int


@dataclass
class Log:
    seeds: list = field(default_factory=list)
    rows: int | None = None
    cols: int | None = None
    obstacles: set = field(default_factory=set)
    obstacle_line_seen: bool = False
    snapshot_ticks: list = field(default_factory=list)
    snapshots: dict = field(default_factory=dict)   # tick -> {id: Tank}: состояние ПОСЛЕ такта
    shots: list = field(default_factory=list)       # (tick, shooter)
    hits: list = field(default_factory=list)        # (tick, shooter, target)
    misses: list = field(default_factory=list)      # (tick, line)
    damage: list = field(default_factory=list)      # (tick, id, dmg, before, after)
    destroyed: list = field(default_factory=list)   # (tick, id, team)
    actions: list = field(default_factory=list)     # (tick, id)
    late_actions: list = field(default_factory=list)  # (tick, id)
    violations: list = field(default_factory=list)
    summary_seen: bool = False
    summary_alive: dict = field(default_factory=dict)
    summary_ticks: int | None = None
    end_reason: str | None = None
    team_size: dict = field(default_factory=dict)
    teams: dict = field(default_factory=dict)       # id -> team


def _grp(m: re.Match, name: str):
    try:
        return m.group(name)
    except IndexError:
        return None


def parse(text: str, fmt: Fmt) -> Log:
    log = Log()
    rx = fmt.regex
    cur = None          # такт текущего снимка
    in_summary = False

    def tick_of(m):
        t = _grp(m, "tick")
        return int(t) if t is not None else cur

    def note_team(tid, team):
        if team:
            log.teams.setdefault(tid, team.upper())

    for line in text.splitlines():
        if "seed" in rx:
            log.seeds += [int(m.group("seed")) for m in rx["seed"].finditer(line)]
        if "summary_marker" in rx and rx["summary_marker"].search(line):
            log.summary_seen = in_summary = True
            continue
        if in_summary:
            if "summary_alive" in rx:
                for m in rx["summary_alive"].finditer(line):
                    log.summary_alive[m.group("team").upper()] = int(m.group("alive"))
            if "summary_ticks" in rx and log.summary_ticks is None:
                m = rx["summary_ticks"].search(line)
                if m:
                    log.summary_ticks = int(m.group("ticks"))
            if "end_reason" in rx and log.end_reason is None:
                m = rx["end_reason"].search(line)
                if m:
                    log.end_reason = fmt.end_reason_map.get(m.group("reason"), "other")
            continue
        if "field" in rx and log.rows is None:
            m = rx["field"].search(line)
            if m:
                log.rows, log.cols = int(m.group("rows")), int(m.group("cols"))
        if "obstacle" in rx and ("obstacle_line" not in rx or rx["obstacle_line"].search(line)):
            if "obstacle_line" in rx:
                log.obstacle_line_seen = True
            for m in rx["obstacle"].finditer(line):
                log.obstacles.add((int(m.group("row")), int(m.group("col"))))
        if "team_size" in rx:
            m = rx["team_size"].search(line)
            if m:
                log.team_size[m.group("team").upper()] = int(m.group("count"))
        if "violation" in rx and rx["violation"].search(line):
            log.violations.append(line.strip())
        if "snapshot" in rx:
            m = rx["snapshot"].search(line)
            if m:
                cur = int(m.group("tick"))
                log.snapshot_ticks.append(cur)
                log.snapshots.setdefault(cur, {})
                continue
        if "position" in rx and cur is not None:
            m = rx["position"].search(line)
            if m:
                t = Tank(m.group("id"), (_grp(m, "team") or "").upper() or None,
                         int(m.group("x")), int(m.group("y")))
                log.snapshots[cur][t.tid] = t
                note_team(t.tid, t.team)
                continue
        if "destroyed" in rx:
            m = rx["destroyed"].search(line)
            if m:
                log.destroyed.append((tick_of(m), m.group("id"), (_grp(m, "team") or "").upper() or None))
                note_team(m.group("id"), _grp(m, "team"))
        if "shot" in rx:
            m = rx["shot"].search(line)
            if m:
                log.shots.append((tick_of(m), m.group("shooter")))
        if "hit" in rx:
            m = rx["hit"].search(line)
            if m:
                log.hits.append((tick_of(m), m.group("shooter"), m.group("target")))
        if "miss" in rx:
            m = rx["miss"].search(line)
            if m:
                log.misses.append((tick_of(m), line.strip()))
        if "damage" in rx:
            m = rx["damage"].search(line)
            if m:
                log.damage.append((tick_of(m), m.group("id"), int(m.group("dmg")),
                                   int(m.group("before")), int(m.group("after"))))
        if "action" in rx:
            for m in rx["action"].finditer(line):
                log.actions.append((tick_of(m), m.group("id")))
        if "late_action" in rx:
            for m in rx["late_action"].finditer(line):
                log.late_actions.append((tick_of(m), m.group("id")))
    return log


# ---------------------------------------------------------------- геометрия
def rect(t: Tank, fmt: Fmt):
    if fmt.position_units == "cells":
        x, y = t.x * fmt.cell_w, t.y * fmt.cell_h
    else:
        x, y = t.x, t.y
    return x, y, x + fmt.tank_w, y + fmt.tank_h      # [x0,x1) x [y0,y1)


def overlap(a, b) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def cell_rect(cell, fmt: Fmt):
    r, c = cell
    return c * fmt.cell_w, r * fmt.cell_h, (c + 1) * fmt.cell_w, (r + 1) * fmt.cell_h


def visible_by_ray(shooter: Tank, target: str, alive: dict, log: Log, fmt: Fmt) -> bool:
    """Есть ли направление, в котором луч из центра стрелка доходит до target,
    не встретив препятствие (и, если los_blocks_tanks, другой танк раньше)."""
    x0, y0, _, _ = rect(shooter, fmt)
    W, H = log.cols * fmt.cell_w, log.rows * fmt.cell_h
    others = {tid: rect(t, fmt) for tid, t in alive.items() if tid != shooter.tid}
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        cx, cy = x0 + fmt.tank_w // 2, y0 + fmt.tank_h // 2
        while True:
            cx += dx
            cy += dy
            if not (0 <= cx < W and 0 <= cy < H) or (cy // fmt.cell_h, cx // fmt.cell_w) in log.obstacles:
                break
            on = [tid for tid, r in others.items() if r[0] <= cx < r[2] and r[1] <= cy < r[3]]
            if target in on:
                return True
            if on and fmt.los_blocks_tanks:
                break
    return False


# ---------------------------------------------------------------- проверки
class Report:
    def __init__(self):
        self.items = []

    def add(self, status, name, detail):
        self.items.append((status, name, detail))

    def verdict(self, name, viol, ok_detail):
        self.add(FAIL if viol else PASS, name,
                 f"{len(viol)} нарушений, первое: {viol[0]}" if viol else ok_detail)

    def print(self, out=sys.stdout):
        for st, name, detail in self.items:
            out.write(f"  [{st}] {name}: {detail}\n")

    def has(self, status):
        return any(st == status for st, _, _ in self.items)

    def status_of(self, name):
        return [st for st, n, _ in self.items if n == name]


def run_checks(log: Log, fmt: Fmt, args) -> Report:
    rep = Report()
    rx = fmt.regex

    # seed: записан и во всех местах совпадает с заданным
    if args.seed is not None:
        found = sorted(set(log.seeds))
        if not found:
            rep.add(FAIL if "seed" in rx else UNVERIFIED, "seed_logged", f"seed {args.seed} в логе не найден")
        else:
            rep.add(PASS if found == [args.seed] else FAIL, "seed_logged",
                    f"ожидался {args.seed}, найдено {found}")

    # итоговая статистика
    if "summary_marker" not in rx:
        rep.add(UNVERIFIED, "summary_present", "summary_marker не задан")
    else:
        rep.add(PASS if log.summary_seen else FAIL, "summary_present",
                "итоги найдены" if log.summary_seen else "нет итоговой статистики")

    # снимки после каждого такта: 0, 1, 2, ... без пропусков
    if not log.snapshot_ticks:
        rep.add(UNVERIFIED, "ticks_sequential", "снимки поля не найдены")
    else:
        exp = list(range(len(log.snapshot_ticks)))
        bad = [f"позиция {i}: T{a} вместо T{b}" for i, (a, b) in enumerate(zip(log.snapshot_ticks, exp)) if a != b]
        rep.verdict("ticks_sequential", bad, f"снимки T0..T{log.snapshot_ticks[-1]}")
        if log.summary_ticks is not None:
            last = log.snapshot_ticks[-1]
            rep.add(PASS if log.summary_ticks == last else FAIL, "summary_ticks_match",
                    f"итог {log.summary_ticks} тактов, последний снимок T{last}")

    last_tick = log.summary_ticks if log.summary_ticks is not None else (
        log.snapshot_ticks[-1] if log.snapshot_ticks else None)

    if args.max_ticks is not None and args.max_ticks > 0:
        if last_tick is None:
            rep.add(UNVERIFIED, "tick_limit", "число тактов не извлечено")
        else:
            rep.add(PASS if last_tick <= args.max_ticks else FAIL, "tick_limit",
                    f"сыграно {last_tick}, лимит {args.max_ticks}")

    # самодиагностика программы
    if "violation" in rx:
        rep.verdict("no_self_reported_violation", log.violations, "программа нарушений не сообщала")

    # два действующих танка не занимают одну область; танк внутри поля и не на препятствии
    if not any(log.snapshots.values()):
        rep.add(UNVERIFIED, "no_overlap_alive", "позиции танков не найдены")
    else:
        viol = []
        for tick, snap in sorted(log.snapshots.items()):
            ts = list(snap.values())
            for i in range(len(ts)):
                for j in range(i + 1, len(ts)):
                    if overlap(rect(ts[i], fmt), rect(ts[j], fmt)):
                        viol.append(f"T{tick}: {ts[i].tid}({ts[i].x},{ts[i].y}) и {ts[j].tid}({ts[j].x},{ts[j].y})")
        rep.verdict("no_overlap_alive", viol, f"{len(log.snapshots)} снимков без пересечений")
        if log.rows is None or "obstacle" not in rx or ("obstacle_line" in rx and not log.obstacle_line_seen):
            rep.add(UNVERIFIED, "tank_in_field_not_on_obstacle", "нет размеров поля или списка препятствий")
        else:
            viol = []
            W, H = log.cols * fmt.cell_w, log.rows * fmt.cell_h
            for tick, snap in sorted(log.snapshots.items()):
                for t in snap.values():
                    r = rect(t, fmt)
                    if r[0] < 0 or r[1] < 0 or r[2] > W or r[3] > H:
                        viol.append(f"T{tick}: {t.tid} вне поля {r}")
                    viol += [f"T{tick}: {t.tid} на препятствии {c}" for c in log.obstacles
                             if overlap(r, cell_rect(c, fmt))]
            rep.verdict("tank_in_field_not_on_obstacle", viol, "ok")

    # уничтоженный танк в следующих тактах не действует и не появляется на поле
    died = {}
    for tick, tid, _ in log.destroyed:
        died.setdefault(tid, tick)
    if "destroyed" not in rx:
        rep.add(UNVERIFIED, "dead_tank_inactive", "событие уничтожения не описано в формате")
    elif not log.actions and not log.snapshots:
        rep.add(UNVERIFIED, "dead_tank_inactive", "в логе нет действий и снимков")
    else:
        viol = [f"{tid} уничтожен повторно" for tid in died
                if sum(1 for _, t, _ in log.destroyed if t == tid) > 1]
        viol += [f"{tid} уничтожен в T{died[tid]}, действует в T{tick}"
                 for tick, tid in log.actions + log.shots if tid in died and tick is not None and tick > died[tid]]
        viol += [f"{tid} уничтожен в T{died[tid]}, но в том же такте после уничтожения ещё действует (T{tick})"
                 for tick, tid in log.late_actions if tid in died and tick is not None and tick == died[tid]]
        viol += [f"{tid} уничтожен в T{died[tid]}, но стоит на поле в снимке T{tick}"
                 for tick, snap in log.snapshots.items() for tid in snap if tid in died and tick >= died[tid]]
        rep.verdict("dead_tank_inactive", viol, f"уничтожено {len(died)}, посмертных действий нет")

    # прямая наводка: каждый выстрел попадает в первого на луче противника
    if "miss" in rx:
        rep.verdict("shot_has_visible_target", [f"T{t}: {l}" for t, l in log.misses],
                    f"{len(log.shots)} выстрелов, промахов (выстрелов без цели) нет")
    if not log.hits:
        rep.add(UNVERIFIED, "hit_requires_los", "попадания в логе не найдены")
    elif log.rows is None or "obstacle" not in rx:
        rep.add(UNVERIFIED, "hit_requires_los", "нет поля/препятствий — линию видимости не восстановить")
    else:
        viol, checked = [], 0
        for tick, shooter, target in log.hits:
            state = log.snapshots.get(tick - 1)        # решение принималось по состоянию после T-1
            if state is None or shooter not in state:
                viol.append(f"T{tick}: стрелок {shooter} не на поле перед тактом")
                continue
            if target not in state:
                viol.append(f"T{tick}: цель {target} не на поле перед тактом")
                continue
            checked += 1
            if log.teams.get(shooter) == log.teams.get(target):
                viol.append(f"T{tick}: {shooter} попал в союзника {target}")
            elif not visible_by_ray(state[shooter], target, state, log, fmt):
                viol.append(f"T{tick}: {shooter} -> {target} без линии видимости")
        rep.verdict("hit_requires_los", viol, f"{checked} попаданий, все по видимой цели")
    if log.shots and log.hits and "miss" in rx:
        rep.add(PASS if len(log.shots) == len(log.hits) + len(log.misses) else FAIL, "shots_accounted",
                f"выстрелов {len(log.shots)}, попаданий {len(log.hits)}, промахов {len(log.misses)}")

    # урон: арифметика, непрерывность HP, уничтожение ровно при HP 0
    if "damage" not in rx or not log.damage:
        rep.add(UNVERIFIED, "damage_consistent", "события повреждения не найдены")
    else:
        viol, hp = [], {}
        zero_at = {}
        for tick, tid, dmg, before, after in log.damage:
            if dmg <= 0 or after != max(before - dmg, 0):
                viol.append(f"T{tick}: {tid} -{dmg} ({before} -> {after})")
            if tid in hp and hp[tid] != before:
                viol.append(f"T{tick}: {tid} HP до удара {before}, а после прошлого {hp[tid]}")
            if tid in zero_at and zero_at[tid] < tick:
                viol.append(f"T{tick}: по {tid} попали после обнуления HP в T{zero_at[tid]}")
            hp[tid] = after
            if after == 0:
                zero_at.setdefault(tid, tick)
        if "destroyed" in rx:
            viol += [f"{tid}: HP 0 в T{t}, уничтожение {'в T' + str(died[tid]) if tid in died else 'не записано'}"
                     for tid, t in zero_at.items() if died.get(tid) != t]
            viol += [f"{tid} уничтожен в T{t} без обнуления HP" for tid, t in died.items() if tid not in zero_at]
        rep.verdict("damage_consistent", viol, f"{len(log.damage)} повреждений согласованы")

    # итог согласован с уничтожениями
    init = {"A": args.count_a if args.count_a is not None else log.team_size.get("A"),
            "B": args.count_b if args.count_b is not None else log.team_size.get("B")}
    if args.count_a is not None and log.team_size.get("A") not in (None, args.count_a) or \
       args.count_b is not None and log.team_size.get("B") not in (None, args.count_b):
        rep.add(FAIL, "team_size_as_requested", f"запрошено A={args.count_a} B={args.count_b}, в логе {log.team_size}")
    elif args.count_a is not None or args.count_b is not None:
        rep.add(PASS if log.team_size else UNVERIFIED, "team_size_as_requested", f"в логе {log.team_size}")
    if len(log.summary_alive) < 2 or "destroyed" not in rx or None in init.values():
        rep.add(UNVERIFIED, "summary_consistent", "нет живых в итоге, уничтожений или исходных размеров")
    else:
        lost = {"A": 0, "B": 0}
        for tid in died:
            team = log.teams.get(tid)
            if team in lost:
                lost[team] += 1
        bad = [f"{t}: {init[t]}-{lost[t]} != {log.summary_alive.get(t)}" for t in "AB"
               if log.summary_alive.get(t) != init[t] - lost[t]]
        rep.verdict("summary_consistent", bad,
                    f"живы A={log.summary_alive['A']} B={log.summary_alive['B']}")

    # условие завершения
    if args.expect_end != "any":
        a, b = log.summary_alive.get("A"), log.summary_alive.get("B")
        reason = log.end_reason
        if last_tick is None or a is None or b is None:
            rep.add(UNVERIFIED, "end_condition", "нет числа тактов или живых в итоге")
        else:
            side_gone = a == 0 or b == 0
            at_limit = args.max_ticks is not None and args.max_ticks > 0 and last_tick == args.max_ticks
            info = f"такт {last_tick}, A={a} B={b}, причина={reason}"
            if args.expect_end == "interrupted":
                ok = (reason == "interrupted" or "end_reason" not in rx) and not side_gone and \
                     (args.max_ticks in (None, 0) or last_tick < args.max_ticks)
            elif args.expect_end == "limit":
                ok = at_limit and not side_gone and reason in ("limit", None)
            elif args.expect_end == "destroyed":
                ok = side_gone and reason in ("destroyed", None)
            else:   # complete: сторона уничтожена либо исчерпан лимит, причина соответствует факту
                ok = (side_gone and reason in ("destroyed", None)) or \
                     (at_limit and not side_gone and reason in ("limit", None))
            rep.add(PASS if ok else FAIL, "end_condition", info)
            if side_gone and died:
                final_kill = max(died.values())
                rep.add(PASS if final_kill == last_tick else FAIL, "no_ticks_after_side_destroyed",
                        f"последнее уничтожение T{final_kill}, конец боя T{last_tick}")
    return rep


def summary_line(log: Log) -> str:
    return (f"ticks={log.summary_ticks} reason={log.end_reason} A={log.summary_alive.get('A')} "
            f"B={log.summary_alive.get('B')} destroyed={sorted(log.destroyed, key=lambda d: (d[0], d[1]))}")


# ---------------------------------------------------------------- самопроверка
# Синтетический лог в формате tanks_sem (по исходнику): поле 3x2, A1 сверху,
# B1 снизу в столбце 0, препятствий нет. T1 — наведение, T2 — обмен выстрелами,
# T3 — A1 добивает B1.
GOOD_LOG = """# tanks_sem — имитация танкового боя (ИДЗ-2, вариант 24), синхронизация: POSIX-семафоры + мьютексы
# seed=7 rows=3 cols=2 prob=0 max_ticks=10 delay_ms=0 focus=on
# команда A: ручной состав, 1 танк(ов): A1 mouse
# команда B: ручной состав, 1 танк(ов): B1 t62a
# препятствия [строка,столбец]: нет
[T0] начальная расстановка: в строю A 1/1, B 1/1
         0  1
     0 A1  .
     1  .  .
     2 B1  .
    A1 mouse     (  0,  0) вниз         HP 300/300  орудие: готово
    B1 t62a      (  0, 10) вверх        HP  90/210  орудие: готово
[T1] наблюдение: A1 mouse @(0,0) смотрит вниз | вниз: противник B1 t62a HP 90, заряжен (5)
[T1] наведение: A1 mouse орудие вниз -> вниз
[T1] наведение: B1 t62a орудие вверх -> вверх
[T1] состояние поля после такта: в строю A 1/1, B 1/1
    A1 mouse     (  0,  0) вниз         HP 300/300  орудие: готово, прицелился
    B1 t62a      (  0, 10) вверх        HP  90/210  орудие: готово, прицелился
[T2] выстрел: A1 mouse вниз
[T2] попадание: A1 mouse -> B1 t62a (дистанция 5)
[T2] повреждение: B1 t62a -45 HP (90 -> 45)
[T2] выстрел: B1 t62a вверх
[T2] попадание: B1 t62a -> A1 mouse (дистанция 5)
[T2] повреждение: A1 mouse -60 HP (300 -> 240)
[T2] состояние поля после такта: в строю A 1/1, B 1/1
    A1 mouse     (  0,  0) вниз         HP 240/300  орудие: перезарядка 13
    B1 t62a      (  0, 10) вверх        HP  45/210  орудие: перезарядка 5
[T3] выстрел: A1 mouse вниз
[T3] попадание: A1 mouse -> B1 t62a (дистанция 5)
[T3] повреждение: B1 t62a -45 HP (45 -> 0), прочность исчерпана
[T3] уничтожение: B1 t62a (решающий выстрел: A1 mouse); в команде B осталось 0
[T3] состояние поля после такта: в строю A 1/1, B 0/1
    A1 mouse     (  0,  0) вниз         HP 240/300  орудие: перезарядка 13
    B1 t62a      уничтожен

=== ИТОГИ БОЯ ===
Причина завершения: normal — команда B уничтожена
Сыграно тактов: 3 (лимит: 10); seed: 7
Команда A: в строю 1/1; урон нанесён 90
Команда B: в строю 0/1; урон нанесён 60
"""

# check -> (что заменить, на что, доп. аргументы)
MUTANTS = [
    ("seed_logged", "; seed: 7", "; seed: 8", {}),
    ("no_overlap_alive", "    A1 mouse     (  0,  0) вниз         HP 300/300  орудие: готово, прицелился",
     "    A1 mouse     (  0,  8) вниз         HP 300/300  орудие: готово, прицелился", {}),
    ("hit_requires_los", "столбец]: нет", "столбец]: [1,0]", {}),
    ("hit_requires_los", "попадание: A1 mouse -> B1 t62a (дистанция 5)\n[T2] повреждение: B1",
     "попадание: A1 mouse -> A1 mouse (дистанция 5)\n[T2] повреждение: B1", {}),
    ("tank_in_field_not_on_obstacle", "столбец]: нет", "столбец]: [0,0]", {}),
    ("dead_tank_inactive", "[T3] состояние поля после такта",
     "[T4] смена позиции: B1 t62a (0,10) -> (0,9) вверх на 1\n[T3] состояние поля после такта", {}),
    ("dead_tank_inactive", "[T3] уничтожение: B1 t62a (решающий выстрел: A1 mouse); в команде B осталось 0",
     "[T3] уничтожение: B1 t62a (решающий выстрел: A1 mouse); в команде B осталось 0\n"
     "[T3] смена позиции: B1 t62a (0,10) -> (0,9) вверх на 1", {}),
    ("dead_tank_inactive", "    B1 t62a      уничтожен", "    B1 t62a      (  0, 10) вверх        HP   0/210", {}),
    ("summary_consistent", "Команда B: в строю 0/1", "Команда B: в строю 1/1", {}),
    ("ticks_sequential", "[T2] состояние поля", "[T5] состояние поля", {}),
    ("summary_ticks_match", "Сыграно тактов: 3", "Сыграно тактов: 4", {}),
    ("tick_limit", "", "", {"max_ticks": 2}),
    ("end_condition", "Причина завершения: normal", "Причина завершения: limit", {}),
    ("end_condition", "", "", {"expect_end": "interrupted"}),
    ("summary_present", "=== ИТОГИ БОЯ ===", "", {}),
    ("shot_has_visible_target", "[T3] выстрел: A1 mouse вниз",
     "[T3] выстрел: A1 mouse вниз\n[T3] промах: A1 mouse — снаряд остановлен краем поля через 9 симв.", {}),
    ("shots_accounted", "[T3] выстрел: A1 mouse вниз", "[T3] выстрел: A1 mouse вниз\n[T3] выстрел: A1 mouse вниз", {}),
    ("no_self_reported_violation", "[T3] состояние поля",
     "[T3] НАРУШЕНИЕ ИНВАРИАНТА: A1 пересекает край\n[T3] состояние поля", {}),
    ("damage_consistent", "(45 -> 0)", "(45 -> 5)", {}),
    ("damage_consistent", "(90 -> 45)", "(90 -> 50)", {}),
    ("team_size_as_requested", "", "", {"count_a": 2}),
]


def self_test(fmt_path: str) -> int:
    fmt = Fmt.load(fmt_path)
    base = dict(seed=7, max_ticks=10, count_a=None, count_b=None, expect_end="complete")
    rep = run_checks(parse(GOOD_LOG, fmt), fmt, argparse.Namespace(**base))
    failed = 0
    bad = [i for i in rep.items if i[0] != PASS]
    if bad:
        print("  [FAIL] эталонный лог должен проходить все проверки без UNVERIFIED:")
        for st, name, detail in bad:
            print(f"      [{st}] {name}: {detail}")
        failed += 1
    else:
        print(f"  [PASS] эталонный лог: {len(rep.items)} проверок зелёные")
    for check, old, new, extra in MUTANTS:
        if old and old not in GOOD_LOG:
            print(f"  [FAIL] mutant:{check}: якорь мутации не найден в эталоне")
            failed += 1
            continue
        text = GOOD_LOG.replace(old, new, 1) if old else GOOD_LOG
        args = argparse.Namespace(**{**base, **extra})
        r = run_checks(parse(text, fmt), fmt, args)
        caught = FAIL in r.status_of(check)
        print(f"  [{'PASS' if caught else 'FAIL'}] mutant:{check} ({(new or str(extra))[:50]!r}) "
              f"{'пойман' if caught else 'НЕ пойман'}")
        failed += not caught
    print("SELF-TEST", "OK" if not failed else f"FAILED ({failed})")
    return 0 if not failed else 1


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?")
    ap.add_argument("--format", default=os.path.join(here, "log_format.json"))
    ap.add_argument("--seed", type=int)
    ap.add_argument("--max-ticks", type=int)
    ap.add_argument("--count-a", type=int, help="запрошенное число танков A")
    ap.add_argument("--count-b", type=int, help="запрошенное число танков B")
    ap.add_argument("--expect-end", choices=["any", "complete", "destroyed", "limit", "interrupted"], default="any")
    ap.add_argument("--strict", action="store_true", help="UNVERIFIED считать провалом (код 3)")
    ap.add_argument("--summary", action="store_true", help="только нормализованный итог одной строкой")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test(args.format)
    if not args.log:
        ap.error("нужен путь к логу")
    fmt = Fmt.load(args.format)
    try:
        with open(args.log, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        print(f"  [FAIL] log_readable: {exc}")
        return 1
    log = parse(text, fmt)
    if args.summary:
        print(summary_line(log))
        return 0
    rep = run_checks(log, fmt, args)
    if not fmt.confirmed:
        print("  [NOTE] формат лога не подтверждён на реальном логе (confirmed=false)")
    rep.print()
    if rep.has(FAIL):
        return 1
    return 3 if rep.has(UNVERIFIED) and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())

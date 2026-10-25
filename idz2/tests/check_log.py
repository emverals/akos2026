#!/usr/bin/env python3
"""Независимая проверка журнала tanks_sem по инвариантам варианта 24.

Проверяется только то, что видно в журнале, без опоры на код программы:
  * такты идут подряд с 1;
  * два живых танка не пересекаются, танк не заходит на препятствие и за край;
  * за такт танк смещается по одной оси не дальше своей скорости;
  * уничтоженный танк больше не наблюдает, не решает, не стреляет, не движется;
  * каждый выстрел предварён решением «выстрел» того же танка в том же такте;
  * попадание — только по противнику; HP после повреждения = max(HP - урон, 0);
  * в конфликте клеток побеждает танк раньше в очереди и не легче проигравшего;
  * в каждом полном такте каждый живой танк ровно один раз наблюдает и решает;
  * итоги: число тактов совпадает с последним состоянием поля, урон, нанесённый
    одной командой, равен урону, полученному другой.

Использование: check_log.py ЖУРНАЛ [--expect-reason normal|limit|signal]
Код выхода 0 — нарушений нет, 1 — найдены нарушения (печатаются).
"""
import re
import sys

CELL_W, CELL_H = 11, 5
SPEED = {"mouse": 3, "fv215b183": 2, "t62a": 4, "leopard_1": 5}
MAX_HP = {"mouse": 300, "fv215b183": 130, "t62a": 210, "leopard_1": 180}

RE_TICK = re.compile(r"^=== Такт (\d+) ===$")
RE_STATE = re.compile(r"^\[T(\d+)\] (?:состояние поля после такта|начальная расстановка): в строю A (\d+)/(\d+), B (\d+)/(\d+)$")
RE_ALIVE = re.compile(r"^    ([AB]\d) (\S+)\s+\(\s*(\d+),\s*(\d+)\)\s+(\S+)\s+HP\s+(\d+)/(\d+)")
RE_DEAD = re.compile(r"^    ([AB]\d) (\S+)\s+уничтожен$")
RE_EVENT = re.compile(r"^\[T(\d+)\] ([^:]+): (.*)$")
RE_HEAD = re.compile(r"^# seed=(\d+) rows=(\d+) cols=(\d+) ")
RE_WALLS = re.compile(r"\[(\d+),(\d+)\]")


def overlap(a, b):
    return a[0] < b[0] + CELL_W and a[0] + CELL_W > b[0] and a[1] < b[1] + CELL_H and a[1] + CELL_H > b[1]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    expect_reason = None
    if "--expect-reason" in args:
        expect_reason = args[args.index("--expect-reason") + 1]
    errors = []

    def err(n, msg):
        if len(errors) < 30:
            errors.append(f"строка {n}: {msg}")

    rows = cols = None
    walls = set()
    types = {}
    states = {}            # tick -> {tag: (x, y, hp) | None}
    dead_at = {}           # tag -> tick
    decisions = {}         # (tick, tag) -> text
    observed = {}          # (tick, tag) -> count
    decided = {}
    shots = []             # (n, tick, tag, dir)
    ticks = []
    summary = {}
    aborted = False
    team_dealt = {}
    team_taken = {}

    lines = open(path, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        n = i + 1
        m = RE_HEAD.match(line)
        if m:
            rows, cols = int(m.group(2)), int(m.group(3))
        if line.startswith("# препятствия"):
            walls = {(int(r), int(c)) for r, c in RE_WALLS.findall(line)}
        if line.startswith("# команда"):
            for tag, name in re.findall(r"([AB]\d) (\S+)", line.split(":", 1)[1]):
                types[tag] = name
        m = RE_TICK.match(line)
        if m:
            t = int(m.group(1))
            if ticks and t != ticks[-1] + 1:
                err(n, f"такт {t} после {ticks[-1]}")
            if not ticks and t != 1:
                err(n, f"первый такт {t}")
            ticks.append(t)
        m = RE_STATE.match(line)
        if m:
            t = int(m.group(1))
            alive_a, alive_b = int(m.group(2)), int(m.group(4))
            state = {}
            j = i + 1
            while j < len(lines) and not lines[j].startswith("    "):
                j += 1
            while j < len(lines) and lines[j].startswith("    "):
                ma, md = RE_ALIVE.match(lines[j]), RE_DEAD.match(lines[j])
                if ma:
                    state[ma.group(1)] = (int(ma.group(3)), int(ma.group(4)), int(ma.group(6)))
                    if int(ma.group(7)) != MAX_HP[ma.group(2)]:
                        err(j + 1, f"{ma.group(1)}: максимум HP {ma.group(7)}")
                elif md:
                    state[md.group(1)] = None
                j += 1
            states[t] = state
            live = {k: v for k, v in state.items() if v}
            if sum(1 for k in live if k[0] == "A") != alive_a or sum(1 for k in live if k[0] == "B") != alive_b:
                err(n, f"T{t}: счётчики в строю не совпадают со списком")
            for tag, (x, y, hp) in live.items():
                if not (0 < hp <= MAX_HP[types[tag]]):
                    err(n, f"T{t}: {tag} HP {hp}")
                if x < 0 or y < 0 or x + CELL_W > cols * CELL_W or y + CELL_H > rows * CELL_H:
                    err(n, f"T{t}: {tag} за краем поля ({x},{y})")
                for r in range(y // CELL_H, (y + CELL_H - 1) // CELL_H + 1):
                    for c in range(x // CELL_W, (x + CELL_W - 1) // CELL_W + 1):
                        if (r, c) in walls:
                            err(n, f"T{t}: {tag} на препятствии [{r},{c}]")
            tags = sorted(live)
            for a in range(len(tags)):
                for b in range(a + 1, len(tags)):
                    if overlap(live[tags[a]], live[tags[b]]):
                        err(n, f"T{t}: пересекаются {tags[a]} и {tags[b]}")
            prev = states.get(t - 1)
            if prev is not None:
                for tag, cur in state.items():
                    old = prev.get(tag)
                    if old is None and cur is not None:
                        err(n, f"T{t}: {tag} ожил")
                    if old and cur:
                        ddx, ddy = abs(cur[0] - old[0]), abs(cur[1] - old[1])
                        if (ddx and ddy) or ddx + ddy > SPEED[types[tag]]:
                            err(n, f"T{t}: {tag} сместился на ({ddx},{ddy})")
                        if cur[2] > old[2]:
                            err(n, f"T{t}: у {tag} выросло HP")
            i = j
            continue
        m = RE_EVENT.match(line)
        if m:
            t, kind, text = int(m.group(1)), m.group(2), m.group(3)
            actor = text.split()[0] if text else ""
            if kind in ("наблюдение", "решение", "выстрел", "смена позиции", "наведение", "поворот", "позиция"):
                if actor in dead_at and t > dead_at[actor]:
                    err(n, f"уничтоженный на T{dead_at[actor]} {actor} действует: {kind}")
            if kind == "наблюдение":
                observed[(t, actor)] = observed.get((t, actor), 0) + 1
            elif kind == "решение":
                decided[(t, actor)] = decided.get((t, actor), 0) + 1
                decisions[(t, actor)] = text
            elif kind == "выстрел":
                shots.append((n, t, actor, text.split()[-1]))
            elif kind == "попадание":
                mm = re.match(r"(\w\d) \S+ -> (\w\d)", text)
                if mm and mm.group(1)[0] == mm.group(2)[0]:
                    err(n, f"попадание по своему: {text}")
            elif kind == "повреждение":
                mm = re.match(r"(\w\d) \S+ -(\d+) HP \((\d+) -> (\d+)\)", text)
                if mm and int(mm.group(4)) != max(int(mm.group(3)) - int(mm.group(2)), 0):
                    err(n, f"неверный расчёт HP: {text}")
            elif kind == "уничтожение":
                dead_at[actor] = t
            elif kind == "конфликт клеток":
                mm = re.match(r"(\w\d) \S+ \(масса (\d+), очередь #(\d+)\).*занята (\w\d) \S+ \(масса (\d+), очередь #(\d+)\)", text)
                if mm:
                    m_lose, q_lose, m_win, q_win = int(mm.group(2)), int(mm.group(3)), int(mm.group(5)), int(mm.group(6))
                    if not (q_win < q_lose and m_win >= m_lose):
                        err(n, f"конфликт решён не по массе/очереди: {text}")
        if re.match(r"^\[T\d+\] остановка ", line):
            aborted = True
        mm = re.match(r"^Причина завершения: (\w+)", line)
        if mm:
            summary["reason"] = mm.group(1)
        mm = re.match(r"^Сыграно тактов: (\d+)", line)
        if mm:
            summary["ticks"] = int(mm.group(1))
        mm = re.match(r"^Команда ([AB]): .*урон нанесён (\d+), получен (\d+)", line)
        if mm:
            team_dealt[mm.group(1)] = int(mm.group(2))
            team_taken[mm.group(1)] = int(mm.group(3))
        i += 1

    for n, t, tag, d in shots:
        text = decisions.get((t, tag), "")
        if not text.endswith("намерение: выстрел " + d):
            err(n, f"выстрел {tag} {d} на T{t} без решения о выстреле")
    for t in ticks:
        if t not in states:
            continue
        before = states.get(t - 1, {})
        for tag, v in before.items():
            if v and (observed.get((t, tag)) != 1 or decided.get((t, tag)) != 1):
                err(0, f"T{t}: живой {tag} наблюдал {observed.get((t, tag), 0)} раз, решал {decided.get((t, tag), 0)} раз")
    for tag, t in dead_at.items():
        if t in states and states[t].get(tag) is not None:
            err(0, f"{tag} уничтожен на T{t}, но в состоянии поля жив")
    if "reason" not in summary:
        err(0, "нет итогов боя")
    else:
        last = max(states) if states else 0
        if summary.get("ticks") != last:
            err(0, f"в итогах {summary.get('ticks')} тактов, последнее состояние поля T{last}")
        if expect_reason and summary["reason"] != expect_reason:
            err(0, f"причина завершения {summary['reason']}, ожидалась {expect_reason}")
        if summary["reason"] == "signal" and not aborted:
            err(0, "завершение по сигналу без записи об остановке")
        if team_dealt.get("A") != team_taken.get("B") or team_dealt.get("B") != team_taken.get("A"):
            err(0, f"урон команд не сходится: {team_dealt} / {team_taken}")
        if summary["reason"] == "normal":
            final = states[last]
            a = sum(1 for k, v in final.items() if v and k[0] == "A")
            b = sum(1 for k, v in final.items() if v and k[0] == "B")
            if a and b:
                err(0, "normal, но обе команды в строю")
    if errors:
        print(f"{path}: нарушения ({len(errors)}):")
        for e in errors:
            print("  " + e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

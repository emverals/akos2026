#!/usr/bin/env bash
# Тестовый набор для tanks_sem (подходит и для другой реализации с тем же CLI).
#
#   bash tests/battle_tests.sh [--quick] [БИНАРНИК]
#
# БИНАРНИК по умолчанию ./tanks_sem. --quick — меньше повторов для сборок с
# санитайзерами (они в разы медленнее). Отчёт TSan/ASan/UBSan/LSan в stderr
# любого запуска — провал теста. Каждый запуск ограничен timeout: зависание
# (например, поток, который никто не разбудил к join) — тоже провал.
# Результаты: tests/out/<имя бинарника>/. Код выхода 0 — все тесты прошли.
#
# Конкретные seed (ничья на 27 и т. п.) подобраны для rand_r из glibc.
set -u

QUICK=0
if [ "${1:-}" = "--quick" ]; then
    QUICK=1
    shift
fi
BIN=$(realpath "${1:-./tanks_sem}")
HERE=$(cd "$(dirname "$0")" && pwd)
DATA="$HERE/data"
OUT="$HERE/out/$(basename "$BIN")"
[ -x "$BIN" ] || { echo "нет исполняемого файла $BIN"; exit 2; }
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT" || exit 2

if [ $QUICK = 1 ]; then REPEAT=5; PINNED=3; SEEDS=3; else REPEAT=20; PINNED=20; SEEDS=12; fi
LIMIT=120
PASS=0
FAIL=0
REASON=""
B=(--batch --plain --delay 0)

verdict() { # verdict ИМЯ — по коду предыдущей команды
    if [ "$1" = 0 ]; then PASS=$((PASS + 1)); echo "PASS  $2"; else FAIL=$((FAIL + 1)); echo "FAIL  $2 — $REASON"; fi
}

sanitizer_clean() {
    ! grep -qE 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' "$1"
}

# finish ИМЯ RC ОЖИДАЕМЫЙ — общая проверка завершённого запуска
finish() {
    local name=$1 rc=$2 want=$3
    if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then REASON="зависание, убит по timeout (rc=$rc)"; return 1; fi
    if [ -f "$name.err" ] && ! sanitizer_clean "$name.err"; then REASON="отчёт санитайзера: $OUT/$name.err"; return 1; fi
    if [ "$rc" != "$want" ]; then REASON="код выхода $rc, ожидался $want"; return 1; fi
    return 0
}

# run ИМЯ ОЖИДАЕМЫЙ_КОД ВВОД [аргументы...]
run() {
    local name=$1 want=$2 input=$3
    shift 3
    timeout -k 5 $LIMIT "$BIN" "$@" < "$input" > "$name.out" 2> "$name.err"
    finish "$name" $? "$want"
}

# sig ИМЯ ОЖИДАЕМЫЙ_КОД СИГНАЛ ЧЕРЕЗ [аргументы...] — сигнал извне через timeout
sig() {
    local name=$1 want=$2 signal=$3 after=$4
    shift 4
    timeout -k 5 --preserve-status -s "$signal" "$after" "$BIN" "$@" < /dev/null > "$name.out" 2> "$name.err"
    finish "$name" $? "$want"
}

need() { # need ОПИСАНИЕ КОМАНДА... — условие теста с понятной причиной провала
    local what=$1
    shift
    "$@" && return 0
    REASON=$what
    return 1
}

check() { # check ЖУРНАЛ [--expect-reason R] — независимая проверка инвариантов
    local log=$1
    shift
    python3 -I "$HERE/check_log.py" "$log" "$@" > "$log.check" 2>&1 && return 0
    REASON="инварианты: $(head -3 "$log.check" | tr '\n' ' ')"
    return 1
}

played() { sed -n 's/^Сыграно тактов: \([0-9]*\).*/\1/p' "$1" 2> /dev/null; }

echo "== справка и разбор аргументов"
run help 0 /dev/null --help && need "нет справки" grep -q "Использование" help.out
verdict $? help
for spec in "rows=2" "rows=13" "cols=0" "cols=13" "prob=1.5" "prob=-0.1" "prob=abc" "prob=nan" \
            "seed=-1" "seed=4294967296" "seed=12x" "delay=-1" "delay=10001" "max-ticks=-1" \
            "team-a=1 1 1" "team-a=7" "team-a=0" "team-a=3 3 0 0" "team-b=1 -1 0 0" "team-a=1 1 1 1 1" \
            "test-signal=1:bogus" "test-signal=0:shoot" "test-signal=3:move:HUP" "log="; do
    name="badarg_${spec//[^A-Za-z0-9-]/_}"
    run "$name" 1 /dev/null --batch --plain "--${spec%%=*}" "${spec#*=}" && need "нет сообщения в stderr" test -s "$name.err"
    verdict $? "$name"
done
run badarg_unknown 1 /dev/null --frobnicate; verdict $? badarg_unknown
run badarg_missing_value 1 /dev/null --seed; verdict $? badarg_missing_value
run badarg_flag_value 1 /dev/null --plain=1; verdict $? badarg_flag_value
run badarg_positional 1 /dev/null extra; verdict $? badarg_positional

echo "== бои по умолчанию (10x10, 4 на 4): stdout = журнал, инварианты"
for s in $(seq 1 $SEEDS); do
    run "seed_$s" 0 /dev/null "${B[@]}" --seed $s --log "seed_$s.log" &&
        need "stdout в --plain отличается от журнала" cmp -s "seed_$s.out" "seed_$s.log" && check "seed_$s.log"
    verdict $? "seed_$s"
done

echo "== сценарии ДЗ-1"
run min_3x1 0 /dev/null "${B[@]}" --seed 7 --rows 3 --cols 1 --prob 1 --log min_3x1.log &&
    check min_3x1.log --expect-reason limit && need "не 600 тактов" test "$(played min_3x1.log)" = 600
verdict $? "min_3x1 (prob 1, лимит 600)"
run max_12x12 0 /dev/null "${B[@]}" --seed 7 --rows 12 --cols 12 --prob 0 --team-a 6 --team-b 6 --log max_12x12.log &&
    check max_12x12.log && need "есть препятствия" grep -q "^# препятствия .*нет" max_12x12.log &&
    need "не 12 потоков танков" grep -q "потоки: танков 12, координаторов 2, арбитр 1" max_12x12.log
verdict $? "max_12x12 (6 на 6, без препятствий)"
run manual_6x8 0 /dev/null "${B[@]}" --seed 7 --rows 6 --cols 8 --team-a "1 1 1 1" --team-b "1 1 1 1" --log manual_6x8.log &&
    check manual_6x8.log &&
    need "состав не по типам" grep -q "команда A: ручной состав, 4 танк(ов): A1 mouse A2 fv215b183 A3 t62a A4 leopard_1" manual_6x8.log
verdict $? manual_6x8
run max_ticks_1 0 /dev/null "${B[@]}" --seed 7 --max-ticks 1 --log max_ticks_1.log &&
    check max_ticks_1.log --expect-reason limit && need "не 1 такт" test "$(played max_ticks_1.log)" = 1
verdict $? max_ticks_1
run draw_27 0 /dev/null "${B[@]}" --seed 27 --log draw_27.log && check draw_27.log --expect-reason normal &&
    need "не ничья" grep -q "^Результат: ничья" draw_27.log
verdict $? "draw_27 (последние танки обеих команд гибнут в одном такте)"

echo "== детерминизм: один seed — один журнал при любом планировании"
DET=(--seed 1234 --rows 12 --cols 12 --prob 0.3 --team-a 6 --team-b 6)
rc=0
for k in $(seq 1 $REPEAT); do run "det_$k" 0 /dev/null "${B[@]}" "${DET[@]}" --log "det_$k.log" || { rc=1; break; }; done
verdict $rc "det_runs ($REPEAT запусков)"
if command -v taskset > /dev/null; then
    rc=0
    for k in $(seq 1 $PINNED); do
        timeout -k 5 $LIMIT taskset -c 0 "$BIN" "${B[@]}" "${DET[@]}" --log "det_cpu0_$k.log" < /dev/null > /dev/null 2> "det_cpu0_$k.err"
        finish "det_cpu0_$k" $? 0 || { rc=1; break; }
    done
    verdict $rc "det_cpu0 ($PINNED запусков на одном ядре)"
fi
rc=0
pids=()
for k in $(seq 1 8); do
    timeout -k 5 $LIMIT "$BIN" "${B[@]}" "${DET[@]}" --log "det_par_$k.log" < /dev/null > /dev/null 2> "det_par_$k.err" &
    pids+=($!)
done
for k in $(seq 1 8); do
    wait "${pids[$((k - 1))]}"
    finish "det_par_$k" $? 0 || rc=1
done
verdict $rc "det_parallel (8 одновременных экземпляров)"
distinct=$(md5sum det_*.log | awk '{print $1}' | sort -u | wc -l)
total=$(ls det_*.log | wc -l)
REASON="$distinct разных журналов из $total"
test "$distinct" = 1
verdict $? "determinism ($total журналов побайтно совпадают)"
check det_1.log
verdict $? det_invariants
run seed_differs 0 /dev/null "${B[@]}" --seed 1235 --rows 12 --cols 12 --prob 0.3 --team-a 6 --team-b 6 --log seed_differs.log &&
    need "другой seed дал тот же журнал" bash -c '! cmp -s seed_differs.log det_1.log'
verdict $? seed_differs

echo "== координатор: фокус меняет ход боя, --no-focus — выбор цели ДЗ-1"
run nofocus 0 /dev/null "${B[@]}" --seed 27 --no-focus --log nofocus.log && check nofocus.log &&
    need "счётчик фокуса не нулевой" grep -q "изменённых фокусом координатора: A 0, B 0 (фокус отключён)" nofocus.log &&
    need "назначен фокус при --no-focus" bash -c '! grep -q "фокус огня ->" nofocus.log'
verdict $? nofocus
changed=0
for s in $(seq 1 $SEEDS); do
    run "focus_off_$s" 0 /dev/null "${B[@]}" --seed $s --no-focus --log "focus_off_$s.log" || continue
    # Сравнивается только физика боя: позиции и HP машин после каждого такта.
    cmp -s <(grep '^    [AB][0-9]' "seed_$s.log") <(grep '^    [AB][0-9]' "focus_off_$s.log") || changed=$((changed + 1))
done
REASON="фокус не изменил ни одного боя из $SEEDS"
test $changed -gt 0
verdict $? "focus_matters (изменилось $changed боёв из $SEEDS)"

echo "== интерактивный ввод через read(2)"
run inter_manual 0 "$DATA/interactive_manual.txt" --plain --delay 0 --seed 5 --log inter_manual.log &&
    need "размер не 6x8" grep -q "rows=6 cols=8" inter_manual.log && need "не ручной состав" grep -q "команда B: ручной состав" inter_manual.log &&
    check inter_manual.log
verdict $? "inter_manual (пример ввода из отчёта ДЗ-1)"
run inter_defaults 0 "$DATA/interactive_defaults.txt" --plain --delay 0 --seed 5 --log inter_defaults.log &&
    need "не значения по умолчанию" grep -q "rows=10 cols=10 prob=0.4" inter_defaults.log &&
    need "не 4 случайных" grep -q "команда A: случайные типы, 4 танк" inter_defaults.log
verdict $? "inter_defaults (Enter — значение по умолчанию)"
run inter_random 0 "$DATA/interactive_random.txt" --plain --delay 0 --seed 5 --log inter_random.log &&
    need "нет сообщения о размере команды" grep -q "Команда должна содержать от 1 до 4 танков" inter_random.out &&
    need "A не 3 танка" grep -q "команда A: случайные типы, 3 танк" inter_random.log &&
    need "B не 2 танка" grep -q "команда B: случайные типы, 2 танк" inter_random.log
verdict $? inter_random
run inter_invalid 0 "$DATA/interactive_invalid.txt" --plain --delay 0 --seed 5 --log inter_invalid.log &&
    need "нет сообщения о размере" grep -qF "Строки: 3..12, столбцы: 1..12." inter_invalid.out &&
    need "нет сообщения о числе чисел" grep -qF "Введите ровно 2 целых чисел через пробел." inter_invalid.out &&
    need "нет сообщения о вероятности" grep -qF "Введите число от 0 до 1, дробную часть отделяйте точкой." inter_invalid.out &&
    need "нет сообщения о режиме" grep -qF "Введите 1 или 2." inter_invalid.out &&
    need "нет сообщения о составе" grep -qF "Количество каждого типа неотрицательно, сумма — от 1 до 4." inter_invalid.out &&
    need "после исправлений не 6x8" grep -q "rows=6 cols=8 prob=0.4" inter_invalid.log
verdict $? "inter_invalid (ошибки отклонены, затем бой)"
run inter_long 0 "$DATA/interactive_long_line.txt" --plain --delay 0 --seed 5 --log inter_long.log &&
    need "нет сообщения о длинной строке" grep -q "Слишком длинная строка. Повторите ввод." inter_long.out &&
    need "не 3x1" grep -q "rows=3 cols=1" inter_long.log
verdict $? inter_long
run inter_eof 3 /dev/null --plain --delay 0 --log inter_eof.log &&
    need "журнал создан без боя" test ! -e inter_eof.log && need "нет сообщения" grep -q "бой не запускается" inter_eof.out
verdict $? "inter_eof (пустой ввод — выход без боя, код 3)"
run inter_eof_mid 3 "$DATA/interactive_truncated.txt" --plain --delay 0 --log inter_eof_mid.log &&
    need "журнал создан без боя" test ! -e inter_eof_mid.log
verdict $? "inter_eof_mid (ввод оборвался на середине)"
# Ctrl+C во время вопроса: stdin открыт и пуст, процесс ждёт в ppoll.
(sleep 4) | timeout -k 5 --preserve-status -s INT 1 "$BIN" --plain --log inter_sig.log > inter_sig.out 2> inter_sig.err
finish inter_sig $? 130 && need "нет сообщения" grep -q "Настройка прервана сигналом SIGINT" inter_sig.out &&
    need "журнал создан без боя" test ! -e inter_sig.log
verdict $? "inter_sigint (Ctrl+C при настройке, код 130)"

echo "== внешние сигналы во время боя"
sig sigint 130 INT 1.5 "${B[@]}" --delay 50 --seed 3 --max-ticks 0 --log sigint.log &&
    check sigint.log --expect-reason signal && need "нет записи об остановке" grep -q "получен SIGINT: потоки останавливаются штатно" sigint.log
verdict $? "sigint (код 130, журнал и итоги сохранены)"
sig sigterm 143 TERM 1.5 "${B[@]}" --delay 50 --seed 3 --max-ticks 0 --log sigterm.log && check sigterm.log --expect-reason signal
verdict $? "sigterm (код 143)"
sig unlimited 130 INT 2 "${B[@]}" --delay 1 --rows 3 --cols 1 --prob 1 --seed 1 --max-ticks 0 --log unlimited.log &&
    need "лимит не снят" grep -q "лимит: нет" unlimited.log && need "не сыграно больше 600 тактов" test "$(played unlimited.log)" -gt 600
verdict $? "unlimited (--max-ticks 0: $(played unlimited.log) тактов до SIGINT)"
sig ansi_restore 130 INT 1 --batch --ansi --delay 100 --seed 3 --log ansi.log &&
    need "курсор не скрывался" grep -q $'\033\[?25l' ansi_restore.out &&
    need "курсор/цвет не восстановлены" grep -q $'\033\[0m\033\[?25h' ansi_restore.out &&
    need "нет итогов на экране" grep -q "^=== ИТОГИ БОЯ ===" ansi_restore.out
verdict $? "ansi_restore (после Ctrl+C курсор и цвета возвращены)"
# Прерывания в случайный момент: бой длится ~0.2-0.6 с, сигнал — через 0.1-0.9 с.
# Успел бой закончиться — код 0 и причина normal/limit; не успел — 130 и signal.
rc=0
interrupted=0
runs=$((QUICK ? 4 : 15))
for k in $(seq 1 $runs); do
    timeout -k 5 --preserve-status -s INT "0.$((RANDOM % 9 + 1))" "$BIN" "${B[@]}" --seed $k --delay 2 \
        --log "sigrand_$k.log" < /dev/null > /dev/null 2> "sigrand_$k.err"
    r=$?
    if [ $r = 130 ]; then
        interrupted=$((interrupted + 1))
        finish "sigrand_$k" $r 130 && check "sigrand_$k.log" --expect-reason signal || { rc=1; break; }
    else
        finish "sigrand_$k" $r 0 && check "sigrand_$k.log" || { rc=1; break; }
    fi
done
verdict $rc "sigint_random ($interrupted из $runs боёв прервано в случайный момент)"

echo "== самосигнал в каждой фазе такта (--test-signal)"
for ph in observe coordinate decide shoot destroy move render delay; do
    case $ph in observe | coordinate | decide) want=4 ;; *) want=5 ;; esac
    run "phase_$ph" 130 /dev/null "${B[@]}" --seed 3 --test-signal "5:$ph" --log "phase_$ph.log" &&
        check "phase_$ph.log" --expect-reason signal && need "сыграно $(played "phase_$ph.log") вместо $want" test "$(played "phase_$ph.log")" = $want
    verdict $? "phase_$ph (сыграно $want: решения отбрасываются, применение доводится до конца такта)"
done
run phase_term 143 /dev/null "${B[@]}" --seed 3 --test-signal "5:decide:TERM" --log phase_term.log &&
    need "нет SIGTERM в журнале" grep -q "получен SIGTERM" phase_term.log
verdict $? phase_term
run phase_repeat 130 /dev/null "${B[@]}" --seed 3 --test-signal "5:decide" --log phase_repeat.log &&
    need "журнал прерывания не воспроизвёлся" cmp -s phase_repeat.log phase_decide.log
verdict $? "phase_repeat (прерывание воспроизводимо)"
last=$(played seed_1.log)
run phase_last_tick 0 /dev/null "${B[@]}" --seed 1 --test-signal "${last:-1}:destroy" --log phase_last_tick.log &&
    check phase_last_tick.log --expect-reason normal
verdict $? "phase_last_tick (сигнал в такте гибели команды: бой уже окончен, код 0)"

echo "== конфиг-файл"
run cfg 0 /dev/null --config "$DATA/battle.cfg" --log cfg.log &&
    need "параметры не из файла" grep -q "seed=77 rows=6 cols=8 prob=0.25 max_ticks=300" cfg.log &&
    need "состав B не из файла" grep -q "команда B: ручной состав, 4 танк(ов): B1 mouse B2 mouse B3 t62a B4 leopard_1" cfg.log &&
    check cfg.log
verdict $? cfg
run cfg_override 0 /dev/null --config "$DATA/battle.cfg" --seed 78 --log cfg_override.log &&
    need "командная строка не переопределила файл" grep -q "^# seed=78 rows=6" cfg_override.log
verdict $? "cfg_override (командная строка сильнее файла)"
run cfg_bad_key 1 /dev/null --config "$DATA/bad_key.cfg" && need "нет номера строки" grep -q "bad_key.cfg:2" cfg_bad_key.err
verdict $? cfg_bad_key
run cfg_bad_syntax 1 /dev/null --config "$DATA/bad_syntax.cfg" && need "нет сообщения" grep -q "ожидается key=value" cfg_bad_syntax.err
verdict $? cfg_bad_syntax
run cfg_missing 1 /dev/null --config "$DATA/no_such.cfg"
verdict $? cfg_missing

echo "== ошибки вывода"
run log_unwritable 2 /dev/null "${B[@]}" --seed 1 --log /nonexistent_dir/x.log &&
    need "нет сообщения" grep -q "не удалось открыть журнал" log_unwritable.err
verdict $? log_unwritable
timeout -k 5 $LIMIT "$BIN" "${B[@]}" --seed 1 --log epipe.log < /dev/null 2> epipe.err | head -1 > /dev/null
finish epipe "${PIPESTATUS[0]}" 2 && need "нет итогов в журнале" grep -q "^Причина завершения: io" epipe.log
verdict $? "stdout_closed (| head: код 2, итоги в журнале)"

echo
echo "Итого: пройдено $PASS, провалено $FAIL (результаты: $OUT)"
[ $FAIL = 0 ]

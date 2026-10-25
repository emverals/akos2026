#!/usr/bin/env bash
# Тесты «Танковые бои» (ИДЗ-2, вариант 24): tanks_sem и tanks_cond.
#
# Запуск:  tests/run_tests.sh            — полный прогон
# Переменные окружения:
#   SRC_DIR    каталог с исходниками и Makefile (по умолчанию — родитель tests/)
#   BIN_DIR    готовые tanks_sem/tanks_cond; сборка тогда пропускается
#   TEST_GROUPS какие группы гонять (по умолчанию все):
#              selftest build cli normal determinism sem_cond signals interactive
#              errors bounds sanitizers
#   SEEDS      seed'ы обычных прогонов ("1 2 3 42 2026")
#   DET_RUNS   повторов на детерминизм (20)
#   SAN_SEEDS  seed'ы под санитайзерами ("1 2 3")
#   RUN_TIMEOUT / SAN_TIMEOUT / SIG_TIMEOUT — таймауты, с (60 / 300 / 5)
#   KEEP=1     не удалять рабочий каталог (логи всех прогонов)
#
# Сборка идёт в копии исходников во временном каталоге: рабочее дерево и его
# бинарники тесты не трогают.
# Итог: код 0 — все тесты PASS (SKIP допускаются только при отсутствии
# инструмента окружения); 1 — есть FAIL.

set -u
set -o pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC_DIR=${SRC_DIR:-$(cd "$HERE/.." && pwd)}
DATA=$HERE/data
CHECK=(python3 -I "$HERE/check_invariants.py")
SEEDS=${SEEDS:-"1 2 3 42 2026"}
DET_RUNS=${DET_RUNS:-20}
SAN_SEEDS=${SAN_SEEDS:-"1 2 3"}
RUN_TIMEOUT=${RUN_TIMEOUT:-60}
SAN_TIMEOUT=${SAN_TIMEOUT:-300}
SIG_TIMEOUT=${SIG_TIMEOUT:-5}
GROUPS_SEL=${TEST_GROUPS:-"selftest build cli normal determinism sem_cond signals interactive errors bounds sanitizers"}
KEEP=${KEEP:-0}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/tanks_tests.XXXXXX") || exit 1
mkdir -p "$WORK/cwd"
FIFO_FD_OPEN=0
cleanup() {
    jobs -p | xargs -r kill -KILL 2>/dev/null
    if [ "$KEEP" = 1 ]; then echo "Рабочий каталог сохранён: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

PASSED=0 FAILED=0 SKIPPED=0
FAILED_NAMES=()
pass() { PASSED=$((PASSED + 1)); printf 'PASS  %s\n' "$1"; }
skip() { SKIPPED=$((SKIPPED + 1)); printf 'SKIP  %s — %s\n' "$1" "$2"; }
fail() {
    FAILED=$((FAILED + 1)); FAILED_NAMES+=("$1")
    printf 'FAIL  %s — %s\n' "$1" "$2"
    if [ -n "${3:-}" ]; then printf '%s\n' "$3" | head -n 25 | sed 's/^/        | /'; fi
}
group_on() { [[ " $GROUPS_SEL " == *" $1 "* ]]; }
section() { printf '\n=== %s ===\n' "$1"; }

# ---------------------------------------------------------------- запуск
# run ID TIMEOUT STDIN CMD... -> RC, ELAPSED_MS, $WORK/ID.out, $WORK/ID.err
# env --default-signal снимает SIG_IGN, который bash ставит фоновым заданиям.
run() {
    local id=$1 to=$2 in=$3; shift 3
    local t0; t0=$(date +%s%N)
    (cd "$WORK/cwd" && exec timeout -k 3 "$to" env --default-signal "$@" <"$in" >"$WORK/$id.out" 2>"$WORK/$id.err")
    RC=$?
    ELAPSED_MS=$((($(date +%s%N) - t0) / 1000000))
}

# start_bg ID STDIN CMD... -> BG_PID (сам процесс программы, без обёрток)
start_bg() {
    local id=$1 in=$2; shift 2
    (cd "$WORK/cwd" && exec env --default-signal "$@" <"$in" >"$WORK/$id.out" 2>"$WORK/$id.err") &
    BG_PID=$!
}

# wait_pid PID TIMEOUT -> 0 если завершился (RC = код), 1 если завис (убит KILL)
wait_pid() {
    local pid=$1 to_ms=$(($2 * 1000)) waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$to_ms" ]; then
            kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; RC=137
            return 1
        fi
        sleep 0.05; waited=$((waited + 50))
    done
    wait "$pid"; RC=$?
    return 0
}

tail_of() { { echo "--- stdout (хвост)"; tail -n 8 "$WORK/$1.out" 2>/dev/null; echo "--- stderr"; tail -n 8 "$WORK/$1.err" 2>/dev/null; } }

# Нормализация лога для сравнения tanks_sem и tanks_cond: убрать строку с
# названием примитивов синхронизации и имя бинарника.
normalize_impl() { sed -E -e '/^# tanks_(sem|cond)[ _]/d' -e 's/tanks_(sem|cond)[a-z_-]*/tanks_X/g' "$1"; }
# Тело лога без упоминаний seed — чтобы проверить, что seed влияет на бой.
strip_seed() { grep -vE 'seed' "$1"; }

# check_log NAME LOG [опции check_invariants]
check_log() {
    local name=$1 log=$2; shift 2
    local out rc
    out=$("${CHECK[@]}" "$log" "$@" 2>&1); rc=$?
    case $rc in
        0) pass "$name" ;;
        3) fail "$name" "по логу не удалось проверить часть инвариантов (формат не распознан?)" "$out" ;;
        *) fail "$name" "нарушены инварианты/итоги (rc=$rc)" "$out" ;;
    esac
}

# checked_run_log ID LOG [опции проверщика]: после run — код 0 и инварианты лога
checked_run_log() {
    local id=$1 log=$2; shift 2
    if [ "$RC" -eq 0 ]; then check_log "$id" "$log" "$@"; else fail "$id" "rc=$RC (124 — таймаут)" "$(tail_of "$id")"; fi
}

has_ansi() { LC_ALL=C grep -q $'\x1b' "$1"; }

# ---------------------------------------------------------------- 0. самопроверка проверщика
if group_on selftest; then
    section "Самопроверка check_invariants.py (каждая проверка обязана краснеть на испорченном логе)"
    out=$("${CHECK[@]}" --self-test 2>&1)
    if [ $? -eq 0 ]; then pass "checker_self_test_mutants_caught"; else fail "checker_self_test_mutants_caught" "проверщик не ловит испорченные логи" "$out"; fi
fi

# ---------------------------------------------------------------- 1. сборка
SEM="" COND=""
copy_tree() {   # копия исходников без бинарников, отчётов и тестов
    local dst=$1 f
    mkdir -p "$dst"
    for f in "$SRC_DIR"/* "$SRC_DIR"/.[!.]*; do
        [ -e "$f" ] || continue
        case $(basename "$f") in tests|report|.git|.claude|*.pdf|*.o|*.log) continue ;; esac
        if [ -f "$f" ] && [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ]; then continue; fi
        cp -a "$f" "$dst/"
    done
}
find_bin() { find "$1" -type f -name "$2" -perm -u+x | head -n 1; }

if [ -n "${BIN_DIR:-}" ]; then
    SEM=$(find_bin "$BIN_DIR" tanks_sem); COND=$(find_bin "$BIN_DIR" tanks_cond)
    section "Сборка пропущена: BIN_DIR=$BIN_DIR"
else
    section "Сборка (make в копии исходников: $WORK/build)"
    copy_tree "$WORK/build"
    make -C "$WORK/build" >"$WORK/build.log" 2>&1
    brc=$?
    if [ $brc -eq 0 ]; then pass "build_make_default_target"; else fail "build_make_default_target" "make rc=$brc" "$(tail -n 25 "$WORK/build.log")"; fi
    SEM=$(find_bin "$WORK/build" tanks_sem); COND=$(find_bin "$WORK/build" tanks_cond)
    nwarn=$(grep -c 'warning:' "$WORK/build.log")
    echo "INFO  предупреждений компилятора: $nwarn"
fi
if [ -n "$SEM" ]; then pass "build_tanks_sem_exists"; else fail "build_tanks_sem_exists" "бинарник tanks_sem не собран"; fi
if [ -n "$COND" ]; then pass "build_tanks_cond_exists"; else fail "build_tanks_cond_exists" "бинарник tanks_cond не собран"; fi
BINS=()
[ -n "$SEM" ] && BINS+=("$SEM")
[ -n "$COND" ] && BINS+=("$COND")
short() { basename "$1"; }
if [ ${#BINS[@]} -eq 0 ]; then
    echo "Нет ни одного бинарника — дальнейшие тесты бессмысленны."
    printf '\nИТОГ: %d PASS, %d FAIL, %d SKIP\n' "$PASSED" "$FAILED" "$SKIPPED"
    exit 1
fi

# Безопасные общие флаги неинтерактивного боя.
BATCH=(--batch --plain --no-color --delay 0)
# Бой, который не кончается сам: поле 3x1, p=1 — препятствие в средней строке
# разделяет два танка, ни видеть, ни ехать некуда.
ENDLESS=(--rows 3 --cols 1 --prob 1 --team-a "1 0 0 0" --team-b "0 0 0 1")

# ---------------------------------------------------------------- 2. CLI-контракт и --help
SIGINT_CODE=130 SIGTERM_CODE=143 HAS_TEST_SIGNAL=0
if group_on cli || group_on signals || group_on sanitizers; then
    section "CLI: --help"
    for b in "${BINS[@]}"; do
        n=$(short "$b")
        run "help_$n" 10 /dev/null "$b" --help
        h="$WORK/help_$n.out"
        if [ $RC -eq 0 ] && [ -s "$h" ]; then pass "help_${n}_exit0_nonempty"; else fail "help_${n}_exit0_nonempty" "rc=$RC" "$(tail_of "help_$n")"; fi
        missing=""
        for opt in --seed --delay --max-ticks --log --no-color --plain --rows --cols --prob --batch --defaults --team-a --team-b --help; do
            grep -q -- "$opt" "$h" || missing+=" $opt"
        done
        if [ -z "$missing" ]; then pass "help_${n}_documents_contract_options"; else fail "help_${n}_documents_contract_options" "нет в справке:$missing"; fi
        ci=$(grep -oE '[0-9]{2,3}[^0-9]{0,25}SIGINT' "$h" | grep -oE '^[0-9]+' | head -n1)
        ct=$(grep -oE '[0-9]{2,3}[^0-9]{0,25}SIGTERM' "$h" | grep -oE '^[0-9]+' | head -n1)
        if [ -n "$ci" ] && [ -n "$ct" ] && [ "$ci" != 0 ] && [ "$ct" != 0 ]; then
            pass "help_${n}_documents_signal_exit_codes (SIGINT=$ci SIGTERM=$ct)"
            [ "$b" = "$SEM" ] && SIGINT_CODE=$ci && SIGTERM_CODE=$ct
        else
            fail "help_${n}_documents_signal_exit_codes" "коды выхода по SIGINT/SIGTERM не найдены в --help; далее ожидаются 130/143"
        fi
        # Поддержка --test-signal определяется пробным запуском, а не по справке:
        # неизвестный параметр даёт код 1.
        run "probe_testsig_$n" 20 /dev/null "$b" --batch --plain --delay 0 --seed 1 --max-ticks 5 \
            --test-signal 2:observe:TERM --log "$WORK/probe_testsig_$n.log"
        [ $RC -ne 1 ] && HAS_TEST_SIGNAL=1
    done
    if [ -n "$SEM" ] && [ -n "$COND" ]; then
        if diff -q <(grep -oE -- '--[a-z-]+' "$WORK/help_tanks_sem.out" | sort -u) \
                   <(grep -oE -- '--[a-z-]+' "$WORK/help_tanks_cond.out" | sort -u) >/dev/null; then
            pass "help_sem_cond_same_option_set"
        else
            fail "help_sem_cond_same_option_set" "у tanks_sem и tanks_cond разный набор опций"
        fi
    fi
fi

# ---------------------------------------------------------------- 3. обычные прогоны
if group_on normal; then
    section "Обычные прогоны: --plain --delay 0, seed: $SEEDS"
    for b in "${BINS[@]}"; do
        n=$(short "$b")
        for s in $SEEDS; do
            id="normal_${n}_s$s"; L="$WORK/$id.log"
            run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed "$s" --max-ticks 600 --log "$L"
            if [ $RC -ne 0 ]; then fail "${id}_exit0" "rc=$RC за ${ELAPSED_MS} мс (124 — таймаут)" "$(tail_of "$id")"; continue; fi
            pass "${id}_exit0 (${ELAPSED_MS} мс)"
            if [ -s "$L" ]; then pass "${id}_log_written"; else fail "${id}_log_written" "лог пуст или не создан"; continue; fi
            if has_ansi "$WORK/$id.out"; then fail "${id}_plain_no_escape_codes" "в stdout --plain --no-color есть ESC-последовательности"; else pass "${id}_plain_no_escape_codes"; fi
            # Поле по умолчанию 10x10, по 4 танка (требования к вводу ДЗ-1).
            check_log "${id}_invariants" "$L" --seed "$s" --max-ticks 600 --count-a 4 --count-b 4 --expect-end complete --strict
        done
        id="defaults_${n}"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" --defaults --plain --delay 0 --seed 77 --log "$L"
        if [ $RC -eq 0 ]; then check_log "${id}_flag_runs_documented_defaults" "$L" --seed 77 --max-ticks 600 --count-a 4 --count-b 4 --expect-end complete
        else fail "${id}_flag_runs_documented_defaults" "rc=$RC" "$(tail_of "$id")"; fi
        run "${id}_b" "$RUN_TIMEOUT" /dev/null "$b" --batch --plain --delay 0 --seed 77 --log "$L.b"
        if cmp -s "$L" "$L.b"; then pass "${n}_defaults_equals_batch"; else fail "${n}_defaults_equals_batch" "--defaults и --batch с одним seed дали разные логи"; fi
    done
fi

# ---------------------------------------------------------------- 4. детерминизм
det_series() {   # name runs prefix-cmd... -- bin args...
    local name=$1 runs=$2; shift 2
    local ref="" i diffs=0 firstbad="" L
    DET_REF=""
    for ((i = 1; i <= runs; i++)); do
        L="$WORK/${name}_$i.log"
        run "${name}_$i" "$RUN_TIMEOUT" /dev/null "$@" --log "$L"
        if [ $RC -ne 0 ]; then fail "$name" "прогон $i: rc=$RC" "$(tail_of "${name}_$i")"; return 1; fi
        if [ -z "$ref" ]; then ref=$L; continue; fi
        if ! cmp -s "$ref" "$L"; then diffs=$((diffs + 1)); [ -z "$firstbad" ] && firstbad=$L; fi
        cmp -s "$WORK/${name}_1.out" "$WORK/${name}_$i.out" || { diffs=$((diffs + 1)); [ -z "$firstbad" ] && firstbad="$WORK/${name}_$i.out"; }
    done
    if [ $diffs -eq 0 ]; then pass "$name ($runs прогонов, логи и stdout идентичны)"
    else fail "$name" "$diffs расхождений из $runs" "$(diff "$ref" "$firstbad" 2>/dev/null | head -n 20)"; fi
    DET_REF=$ref
}
if group_on determinism; then
    section "Детерминизм: один seed x$DET_RUNS"
    for b in "${BINS[@]}"; do
        n=$(short "$b")
        det_series "det_${n}_seed42_x$DET_RUNS" "$DET_RUNS" "$b" "${BATCH[@]}" --seed 42
        ref_free=${DET_REF:-}
        if command -v taskset >/dev/null; then
            det_series "det_${n}_seed42_taskset_cpu0_x$DET_RUNS" "$DET_RUNS" taskset -c 0 "$b" "${BATCH[@]}" --seed 42
            if [ -n "$ref_free" ] && [ -n "${DET_REF:-}" ]; then
                if cmp -s "$ref_free" "$DET_REF"; then pass "det_${n}_one_cpu_equals_all_cpus"
                else fail "det_${n}_one_cpu_equals_all_cpus" "на одном ядре бой идёт иначе, чем на всех" "$(diff "$ref_free" "$DET_REF" | head -n 20)"; fi
            fi
        else
            skip "det_${n}_taskset" "taskset не установлен"
        fi
        # Максимум потоков (12 танков): гонки проявляются здесь первыми.
        det_series "det_${n}_12x12_6v6_x$DET_RUNS" "$DET_RUNS" "$b" "${BATCH[@]}" --seed 9 --rows 12 --cols 12 --prob 0.2 --team-a 6 --team-b 6
        # seed обязан влиять на бой, иначе детерминизм выше тривиален.
        distinct=$(for s in 1 2 3 4 5; do
            run "det_${n}_seedvar_$s" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed "$s" --log "$WORK/det_${n}_seedvar_$s.log"
            strip_seed "$WORK/det_${n}_seedvar_$s.log" | md5sum
        done | sort -u | wc -l)
        if [ "$distinct" -ge 2 ]; then pass "det_${n}_seed_changes_battle ($distinct разных боёв из 5)"
        else fail "det_${n}_seed_changes_battle" "5 разных seed дали один и тот же бой — seed не используется"; fi
    done
fi

# ---------------------------------------------------------------- 5. tanks_sem == tanks_cond
if group_on sem_cond; then
    section "Совпадение tanks_sem и tanks_cond при одном seed"
    if [ -z "$SEM" ] || [ -z "$COND" ]; then
        fail "sem_cond_same_result" "нет одного из бинарников (tanks_sem='$SEM', tanks_cond='$COND')"
    else
        for cfg in "seed=1" "seed=42" "seed=2026" "seed=9 12x12" "seed=5 6x8"; do
            s=${cfg#seed=}; s=${s%% *}
            extra=()
            case $cfg in
                *12x12) extra=(--rows 12 --cols 12 --prob 0.2 --team-a 6 --team-b 6) ;;
                *6x8) extra=(--rows 6 --cols 8 --prob 0.4 --team-a "1 1 1 1" --team-b "1 1 1 1") ;;
            esac
            tag=$(echo "$cfg" | tr ' =' '__')
            run "sc_sem_$tag" "$RUN_TIMEOUT" /dev/null "$SEM" "${BATCH[@]}" --seed "$s" "${extra[@]}" --log "$WORK/sc_sem_$tag.log"; r1=$RC
            run "sc_cond_$tag" "$RUN_TIMEOUT" /dev/null "$COND" "${BATCH[@]}" --seed "$s" "${extra[@]}" --log "$WORK/sc_cond_$tag.log"; r2=$RC
            if [ $r1 -ne 0 ] || [ $r2 -ne 0 ]; then fail "sem_cond_same_log_$tag" "rc sem=$r1 cond=$r2"; continue; fi
            d=$(diff <(normalize_impl "$WORK/sc_sem_$tag.log") <(normalize_impl "$WORK/sc_cond_$tag.log"))
            if [ -z "$d" ]; then pass "sem_cond_same_log_$tag"
            else
                fail "sem_cond_same_log_$tag" "журналы различаются (после удаления имени бинарника)" "$d"
                s1=$("${CHECK[@]}" --summary "$WORK/sc_sem_$tag.log"); s2=$("${CHECK[@]}" --summary "$WORK/sc_cond_$tag.log")
                [ "$s1" = "$s2" ] && echo "        итоги при этом совпадают: $s1" || printf '        итоги: sem: %s\n               cond: %s\n' "$s1" "$s2"
            fi
        done
    fi
fi

# ---------------------------------------------------------------- 6. сигналы
# signal_mid_battle NAME BIN SIGNALS("INT" | "TERM" | "INT INT") DELAY_S EXPECT_RC ARGS...
signal_mid_battle() {
    local name=$1 b=$2 sigs=$3 after=$4 expect=$5; shift 5
    local L="$WORK/$name.log" sig
    start_bg "$name" /dev/null "$b" --plain --no-color --seed 5 --log "$L" "$@"
    sleep "$after"
    if ! kill -0 "$BG_PID" 2>/dev/null; then
        wait "$BG_PID"; fail "$name" "бой закончился (rc=$?) раньше сигнала — сценарий не проверен" "$(tail_of "$name")"; return
    fi
    for sig in $sigs; do kill -s "$sig" "$BG_PID" 2>/dev/null; sleep 0.03; done
    if ! wait_pid "$BG_PID" "$SIG_TIMEOUT"; then
        fail "$name" "процесс не завершился за ${SIG_TIMEOUT} с после SIG$sigs — зависание" "$(tail_of "$name")"; return
    fi
    if [ "$RC" -ne "$expect" ]; then fail "$name" "код выхода $RC, задокументирован $expect" "$(tail_of "$name")"; return; fi
    pass "${name}_exit_code_$expect"
    if [ -s "$L" ]; then check_log "${name}_log_saved_with_summary" "$L" --seed 5 --expect-end interrupted
    else fail "${name}_log_saved_with_summary" "лог не сохранён"; fi
}
if group_on signals; then
    section "Сигналы посреди боя (лог сохраняется, нет зависания, документированный код выхода)"
    for b in "${BINS[@]}"; do
        n=$(short "$b")
        signal_mid_battle "sig_${n}_int_endless" "$b" INT 1 "$SIGINT_CODE" --delay 50 --max-ticks 0 "${ENDLESS[@]}"
        signal_mid_battle "sig_${n}_term_endless" "$b" TERM 1 "$SIGTERM_CODE" --delay 50 --max-ticks 0 "${ENDLESS[@]}"
        signal_mid_battle "sig_${n}_int_twice" "$b" "INT INT" 1 "$SIGINT_CODE" --delay 50 --max-ticks 0 "${ENDLESS[@]}"
        # Сигнал при 12 живых потоках танков и активной стрельбе.
        signal_mid_battle "sig_${n}_int_12x12" "$b" INT 1 "$SIGINT_CODE" --delay 100 --rows 12 --cols 12 --prob 0.2 --team-a 6 --team-b 6
        signal_mid_battle "sig_${n}_term_delay0" "$b" TERM 0.3 "$SIGTERM_CODE" --delay 0 --max-ticks 0 "${ENDLESS[@]}"
        # Сигнал на этапе интерактивной настройки: stdin открыт, но молчит.
        fifo="$WORK/fifo_$n"; mkfifo "$fifo"; exec 7<>"$fifo"
        L="$WORK/sig_${n}_setup.log"
        start_bg "sig_${n}_setup" "$fifo" "$b" --seed 1 --delay 0 --log "$L"
        sleep 0.5; kill -INT "$BG_PID" 2>/dev/null
        if wait_pid "$BG_PID" "$SIG_TIMEOUT"; then
            if [ "$RC" -eq "$SIGINT_CODE" ] && [ ! -e "$L" ]; then pass "sig_${n}_int_during_setup_exits_no_battle"
            else fail "sig_${n}_int_during_setup_exits_no_battle" "rc=$RC (ожидался $SIGINT_CODE), лог создан: $([ -e "$L" ] && echo да || echo нет)" "$(tail_of "sig_${n}_setup")"; fi
        else
            fail "sig_${n}_int_during_setup_exits_no_battle" "завис в ожидании ввода после SIGINT"
        fi
        exec 7>&-
        # Детерминированная доставка сигнала в каждую фазу такта (если программа это умеет).
        if [ $HAS_TEST_SIGNAL = 1 ]; then
            for ph in observe coordinate decide shoot destroy move render delay; do
                for sg in INT TERM; do
                    id="sigphase_${n}_${ph}_$sg"; L="$WORK/$id.log"
                    exp=$([ $sg = INT ] && echo "$SIGINT_CODE" || echo "$SIGTERM_CODE")
                    run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 3 --test-signal "5:$ph:$sg" --log "$L"
                    if [ $RC -ne "$exp" ]; then fail "$id" "rc=$RC, ожидался $exp" "$(tail_of "$id")"; continue; fi
                    check_log "$id" "$L" --seed 3 --max-ticks 6 --count-a 4 --count-b 4 --expect-end interrupted
                done
            done
        else
            skip "sigphase_${n}" "программа не принимает --test-signal"
        fi
    done
fi

# ---------------------------------------------------------------- 7. интерактивный ввод
# Эти флаги не отключают вопросы (см. --help): задержка, вывод, seed, лог.
IARGS=(--delay 0 --plain --no-color --seed 11)
P_SIZE='Размер поля'
P_PROB='Вероятность препятствия'
P_MODE='Состав: 1'
P_CNT_A='Количество танков команды A'
P_CNT_B='Количество танков команды B'
P_MAN_A='Команда A, mouse'
P_MAN_B='Команда B, mouse'
count_of() { grep -o -- "$2" "$1" | wc -l; }
# expect_prompts NAME OUT "RE=N" ... — сколько раз прозвучал каждый вопрос
expect_prompts() {
    local name=$1 out=$2 bad="" pair re want got; shift 2
    for pair in "$@"; do
        re=${pair%=*}; want=${pair##*=}; got=$(count_of "$out" "$re")
        [ "$got" -eq "$want" ] || bad+="«$re»: $got раз вместо $want; "
    done
    if [ -z "$bad" ]; then pass "$name"; else fail "$name" "$bad"; fi
}
log_has() { grep -qE -- "$2" "$1"; }

if group_on interactive; then
    section "Интерактивная настройка (stdin из tests/data)"
    for b in "${BINS[@]}"; do
        n=$(short "$b")
        # interactive CASE FILE TIMEOUT [доп. флаги] -> запуск, RC, лог $WORK/i_${n}_CASE.log
        interactive() {
            local c=$1 f=$2 to=$3; shift 3
            IID="i_${n}_$c"; IL="$WORK/$IID.log"
            run "$IID" "$to" "$DATA/$f" "$b" "${IARGS[@]}" --log "$IL" "$@"
        }
        interactive default default.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            expect_prompts "${IID}_asked_all_questions_once" "$WORK/$IID.out" "$P_SIZE=1" "$P_PROB=1" "$P_MODE=1" "$P_CNT_A=1" "$P_CNT_B=1"
            log_has "$IL" '^# seed=11 rows=10 cols=10 prob=0.4 ' && pass "${IID}_defaults_10x10_p0.4" || fail "${IID}_defaults_10x10_p0.4" "в заголовке лога не те параметры" "$(head -n 3 "$IL")"
            check_log "${IID}_battle" "$IL" --seed 11 --count-a 4 --count-b 4 --max-ticks 600 --expect-end complete
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive manual_6x8 manual_6x8.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            log_has "$IL" '^# seed=11 rows=6 cols=8 prob=0.4 ' && log_has "$IL" '^# команда A: ручной состав' && log_has "$IL" '^# команда B: ручной состав' \
                && pass "${IID}_manual_composition_applied" || fail "${IID}_manual_composition_applied" "параметры ввода не применены" "$(head -n 6 "$IL")"
            check_log "${IID}_battle" "$IL" --seed 11 --count-a 4 --count-b 4 --expect-end complete
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive min_3x1_p1 min_3x1_p1.in "$RUN_TIMEOUT" --max-ticks 60
        if [ $RC -eq 0 ]; then
            log_has "$IL" '^# seed=11 rows=3 cols=1 prob=1 ' && pass "${IID}_field_3x1_p1" || fail "${IID}_field_3x1_p1" "поле не 3x1 p=1" "$(head -n 3 "$IL")"
            check_log "${IID}_ends_by_tick_limit" "$IL" --seed 11 --count-a 1 --count-b 1 --max-ticks 60 --expect-end limit
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive max_12x12_p0 max_12x12_p0.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            log_has "$IL" '^# seed=11 rows=12 cols=12 prob=0 ' && log_has "$IL" '^# препятствия .*: нет$' \
                && pass "${IID}_no_obstacles" || fail "${IID}_no_obstacles" "при p=0 есть препятствия или поле не 12x12" "$(head -n 6 "$IL")"
            check_log "${IID}_battle" "$IL" --seed 11 --count-a 6 --count-b 6 --expect-end complete
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        # Ошибочные общие параметры: каждый неверный ответ — сообщение и повтор вопроса.
        interactive err_general err_general.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            expect_prompts "${IID}_each_bad_answer_reasked" "$WORK/$IID.out" "$P_SIZE=13" "$P_PROB=10" "$P_MODE=6" "$P_CNT_A=5" "$P_CNT_B=1"
            log_has "$IL" '^# seed=11 rows=6 cols=8 prob=0.3 ' && pass "${IID}_corrected_values_used" || fail "${IID}_corrected_values_used" "применены не исправленные значения" "$(head -n 3 "$IL")"
            check_log "${IID}_battle" "$IL" --seed 11 --count-a 2 --count-b 4 --expect-end complete
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive err_manual err_manual.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            expect_prompts "${IID}_each_bad_composition_reasked" "$WORK/$IID.out" "$P_SIZE=1" "$P_MAN_A=9" "$P_MAN_B=3"
            check_log "${IID}_battle" "$IL" --seed 11 --count-a 4 --count-b 4 --expect-end complete
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive long_line long_line.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            expect_prompts "${IID}_rejected_and_reasked" "$WORK/$IID.out" "$P_SIZE=2"
            log_has "$IL" '^# seed=11 rows=6 cols=8 ' && pass "${IID}_tail_not_parsed_as_answer" || fail "${IID}_tail_not_parsed_as_answer" "хвост длинной строки принят как ответ" "$(head -n 3 "$IL")"
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive garbage garbage.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ]; then
            expect_prompts "${IID}_control_nul_unicode_injection_rejected" "$WORK/$IID.out" "$P_SIZE=6"
            log_has "$IL" '^# seed=11 rows=6 cols=8 ' && pass "${IID}_valid_answer_after_garbage" || fail "${IID}_valid_answer_after_garbage" "поле не 6x8" "$(head -n 3 "$IL")"
        else fail "${IID}_exit0" "rc=$RC" "$(tail_of "$IID")"; fi

        interactive crlf crlf.in "$RUN_TIMEOUT"
        if [ $RC -eq 0 ] || [ $RC -eq 3 ]; then pass "${IID}_no_crash_no_hang (rc=$RC)"; else fail "${IID}_no_crash_no_hang" "rc=$RC" "$(tail_of "$IID")"; fi

        # Конец ввода при настройке: выход без боя, документированный код 3, лог не создаётся.
        for f in empty eof_after_prob eof_after_team_a; do
            interactive "$f" "$f.in" 10
            if [ $RC -eq 3 ] && [ ! -e "$IL" ] && ! grep -q 'ИТОГИ' "$WORK/$IID.out"; then pass "${IID}_eof_no_battle_exit3"
            else fail "${IID}_eof_no_battle_exit3" "rc=$RC (ожидался 3), лог создан: $([ -e "$IL" ] && echo да || echo нет)" "$(tail_of "$IID")"; fi
        done
    done
fi

# ---------------------------------------------------------------- 8. ошибки командной строки
if group_on errors; then
    section "Ошибочные аргументы: код 1, понятное сообщение, бой не начат"
    for b in "${BINS[@]}"; do
        n=$(short "$b"); k=0
        # err_case ОПИСАНИЕ АРГУМЕНТЫ... — аргументы передаются как есть, без eval
        err_case() {
            local desc=$1; shift
            k=$((k + 1)); local id="err_${n}_$k" L="$WORK/err_${n}_$k.log"
            run "$id" 10 /dev/null "$b" --batch --plain --delay 0 --log "$L" "$@"
            if [ $RC -eq 1 ] && [ -s "$WORK/$id.err" ] && [ ! -e "$L" ]; then pass "${id}_rejected: $desc"
            else fail "${id}_rejected: $desc" "rc=$RC (ожидался 1), stderr: $(head -c 120 "$WORK/$id.err"), лог создан: $([ -e "$L" ] && echo да || echo нет)"; fi
        }
        err_case "rows ниже минимума 3" --rows 2
        err_case "rows выше максимума 12" --rows 13
        err_case "cols 0" --cols 0
        err_case "cols выше максимума 12" --cols 13
        err_case "rows не число" --rows abc
        err_case "rows переполнение" --rows 99999999999999999999
        err_case "rows пусто" --rows ""
        err_case "rows отрицательное" --rows -5
        err_case "rows с инъекцией команды" --rows "10; rm -rf /"
        err_case "rows с мусором после числа" --rows 10x
        err_case "delay с единицами" --delay 5ms
        err_case "prob > 1" --prob 1.5
        err_case "prob < 0" --prob -0.1
        err_case "prob nan" --prob nan
        err_case "prob с запятой" --prob 0,5
        err_case "prob с мусором" --prob 0.5x
        err_case "seed не число" --seed abc
        err_case "seed отрицательный" --seed -1
        err_case "seed переполнение" --seed 99999999999999999999
        err_case "seed дробный" --seed 1.5
        err_case "delay отрицательная" --delay -1
        err_case "delay не число" --delay abc
        err_case "max-ticks отрицательный" --max-ticks -1
        err_case "max-ticks не число" --max-ticks abc
        err_case "team-a сверх вместимости (поле 10 -> 5)" --team-a "7 0 0 0"
        err_case "team-a пустой состав" --team-a "0 0 0 0"
        err_case "team-a отрицательное количество" --team-a "-1 1 1 1"
        err_case "team-a три числа" --team-a "1 1 1"
        err_case "team-a пять чисел" --team-a "1 1 1 1 1"
        err_case "team-a сумма 6 > 5" --team-a "3 3 0 0"
        err_case "team-a случайных 6 > 5" --team-a 6
        err_case "team-a случайных 0" --team-a 0
        err_case "team-b пусто" --team-b ""
        err_case "team-a с SQL-инъекцией" --team-a "1 1 1 1'; DROP TABLE tanks;--"
        err_case "cols 1: два танка не помещаются" --cols 1 --team-a "1 1 0 0"
        err_case "cols 3: три случайных не помещаются" --cols 3 --team-b 3
        err_case "неизвестный флаг" --bogus
        err_case "seed без значения" --seed
        err_case "позиционный аргумент" positional
        err_case "log пустой путь" --log ""
        id="err_${n}_log_unwritable"
        run "$id" 10 /dev/null "$b" "${BATCH[@]}" --seed 1 --log /nonexistent_dir_tanks/x.log
        if [ $RC -ne 0 ] && [ $RC -lt 124 ] && [ -s "$WORK/$id.err" ]; then pass "${id}_fails_cleanly (rc=$RC)"
        else fail "$id" "rc=$RC, ожидалась ненулевая ошибка с сообщением" "$(tail_of "$id")"; fi
    done
fi

# ---------------------------------------------------------------- 9. границы
if group_on bounds; then
    section "Границы: максимальное поле, один танк, лимит тактов, задержка"
    for b in "${BINS[@]}"; do
        n=$(short "$b")
        id="bound_${n}_max_12x12_6v6_p0"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 4 --rows 12 --cols 12 --prob 0 --team-a "2 2 1 1" --team-b "1 2 2 1" --log "$L"
        checked_run_log "$id" "$L" --seed 4 --count-a 6 --count-b 6 --max-ticks 600 --expect-end complete --strict

        id="bound_${n}_max_12x12_6v6_p1"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 4 --rows 12 --cols 12 --prob 1 --team-a 6 --team-b 6 --log "$L"
        checked_run_log "$id" "$L" --seed 4 --count-a 6 --count-b 6 --max-ticks 600 --expect-end complete

        id="bound_${n}_min_3x1_p1_limit50"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 1 --max-ticks 50 "${ENDLESS[@]}" --log "$L"
        checked_run_log "$id" "$L" --seed 1 --count-a 1 --count-b 1 --max-ticks 50 --expect-end limit

        id="bound_${n}_one_vs_one_open_field"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 8 --rows 12 --cols 12 --prob 0 --team-a 1 --team-b 1 --log "$L"
        checked_run_log "$id" "$L" --seed 8 --count-a 1 --count-b 1 --max-ticks 600 --expect-end complete

        id="bound_${n}_max_ticks_1"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 2 --max-ticks 1 --log "$L"
        checked_run_log "$id" "$L" --seed 2 --count-a 4 --count-b 4 --max-ticks 1 --expect-end limit

        id="bound_${n}_narrow_cols1_team_limit1"; L="$WORK/$id.log"
        run "$id" "$RUN_TIMEOUT" /dev/null "$b" "${BATCH[@]}" --seed 2 --rows 12 --cols 1 --prob 0 --max-ticks 300 --log "$L"
        checked_run_log "$id" "$L" --seed 2 --count-a 1 --count-b 1 --max-ticks 300 --expect-end complete

        # Регулируемая задержка: 10 тактов по 100 мс — не меньше 9 задержек (0,9 с), без задержки — вдвое быстрее.
        run "bound_${n}_delay100" "$RUN_TIMEOUT" /dev/null "$b" --batch --plain --delay 100 --seed 1 --max-ticks 10 "${ENDLESS[@]}" --log "$WORK/bd1.log"; t100=$ELAPSED_MS; r100=$RC
        run "bound_${n}_delay0" "$RUN_TIMEOUT" /dev/null "$b" --batch --plain --delay 0 --seed 1 --max-ticks 10 "${ENDLESS[@]}" --log "$WORK/bd0.log"; t0=$ELAPSED_MS
        if [ $r100 -eq 0 ] && [ "$t100" -ge 900 ] && [ "$t0" -lt $((t100 / 2)) ]; then pass "bound_${n}_delay_is_honored (100мс x10: ${t100} мс, 0: ${t0} мс)"
        else fail "bound_${n}_delay_is_honored" "--delay 100: ${t100} мс (rc=$r100), --delay 0: ${t0} мс"; fi

        # Закрытый stdout (| head) не должен вешать программу и убивать её SIGPIPE.
        L="$WORK/bound_${n}_epipe.log"
        rr=$(cd "$WORK/cwd" && timeout -k 3 "$RUN_TIMEOUT" env --default-signal "$b" --batch --plain --delay 0 --seed 3 --log "$L" </dev/null 2>/dev/null | head -c 200 >/dev/null; echo "${PIPESTATUS[0]}")
        if [ "$rr" -lt 124 ] && grep -q 'ИТОГИ' "$L" 2>/dev/null; then pass "bound_${n}_closed_stdout_terminates_log_saved (rc=$rr)"
        else fail "bound_${n}_closed_stdout_terminates_log_saved" "rc=$rr (124 — завис, 141 — убит SIGPIPE), итоги в логе: $(grep -c 'ИТОГИ' "$L" 2>/dev/null)"; fi
    done
fi

# ---------------------------------------------------------------- 10. санитайзеры
san_scan() { grep -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|SUMMARY: (Thread|Address|Leak|UndefinedBehavior)Sanitizer' "$1"; }
if group_on sanitizers; then
    for t in tsan asan; do
        section "Санитайзер: make $t"
        dir="$WORK/san_$t"; copy_tree "$dir"
        before=$(find "$dir" -type f -perm -u+x | sort)
        # TESTS=true глушит встроенный в цель make прогон чужого набора (переменная
        # из Makefile); если такой переменной нет, переопределение ни на что не влияет.
        make -C "$dir" "$t" TESTS=true >"$WORK/make_$t.log" 2>&1; mrc=$?
        mapfile -t sbins < <(comm -13 <(echo "$before") <(find "$dir" -type f -perm -u+x | sort) | while read -r f; do
            [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ] && echo "$f"; done)
        if [ ${#sbins[@]} -eq 0 ]; then fail "san_${t}_build" "make $t rc=$mrc, исполняемых файлов не создано" "$(tail -n 20 "$WORK/make_$t.log")"; continue; fi
        [ $mrc -ne 0 ] && echo "INFO  make $t rc=$mrc (бинарник собран, упал шаг после сборки)"
        pass "san_${t}_build ($(for f in "${sbins[@]}"; do basename "$f"; done | tr '\n' ' '))"
        for sb in "${sbins[@]}"; do
            n=$(basename "$sb")
            export TSAN_OPTIONS="halt_on_error=0 exitcode=66 second_deadlock_stack=1"
            export ASAN_OPTIONS="detect_leaks=1:exitcode=67:detect_stack_use_after_return=1"
            export UBSAN_OPTIONS="print_stacktrace=1"
            for s in $SAN_SEEDS; do
                id="san_${n}_s$s"; L="$WORK/$id.log"
                run "$id" "$SAN_TIMEOUT" /dev/null "$sb" "${BATCH[@]}" --seed "$s" --log "$L"
                rep=$(san_scan "$WORK/$id.err")
                if [ $RC -eq 0 ] && [ -z "$rep" ]; then pass "$id"; else fail "$id" "rc=$RC, отчёт санитайзера:" "${rep:-$(tail -n 10 "$WORK/$id.err")}"; fi
                # Инструментированная сборка обязана играть тот же бой.
                ref="$WORK/san_ref_${n}_$s.log"
                case $n in *cond*) rb=$COND ;; *) rb=$SEM ;; esac
                if [ -n "$rb" ] && [ $RC -eq 0 ]; then
                    run "san_ref_${n}_$s" "$RUN_TIMEOUT" /dev/null "$rb" "${BATCH[@]}" --seed "$s" --log "$ref"
                    diff -q <(normalize_impl "$ref") <(normalize_impl "$L") >/dev/null && pass "${id}_same_battle_as_release" || fail "${id}_same_battle_as_release" "бой под $t отличается от обычной сборки"
                fi
            done
            id="san_${n}_12x12_6v6"
            run "$id" "$SAN_TIMEOUT" /dev/null "$sb" "${BATCH[@]}" --seed 9 --rows 12 --cols 12 --prob 0.2 --team-a 6 --team-b 6 --log "$WORK/$id.log"
            rep=$(san_scan "$WORK/$id.err")
            if [ $RC -eq 0 ] && [ -z "$rep" ]; then pass "$id"; else fail "$id" "rc=$RC" "${rep:-$(tail -n 10 "$WORK/$id.err")}"; fi
            # Остановка по сигналу — самый частый источник гонок и утечек.
            name="san_${n}_sigint"
            start_bg "$name" /dev/null "$sb" --plain --delay 50 --max-ticks 0 --seed 5 --log "$WORK/$name.log" "${ENDLESS[@]}"
            sleep 2; kill -INT "$BG_PID" 2>/dev/null
            if wait_pid "$BG_PID" $((SIG_TIMEOUT * 4)); then
                rep=$(san_scan "$WORK/$name.err")
                if [ "$RC" -eq "$SIGINT_CODE" ] && [ -z "$rep" ]; then pass "$name"; else fail "$name" "rc=$RC" "${rep:-$(tail -n 10 "$WORK/$name.err")}"; fi
            else fail "$name" "завис после SIGINT под $t"; fi
            if [ $HAS_TEST_SIGNAL = 1 ]; then
                for ph in observe decide shoot move delay; do
                    id="san_${n}_sigphase_$ph"
                    run "$id" "$SAN_TIMEOUT" /dev/null "$sb" "${BATCH[@]}" --seed 3 --test-signal "5:$ph:TERM" --log "$WORK/$id.log"
                    rep=$(san_scan "$WORK/$id.err")
                    if [ "$RC" -eq "$SIGTERM_CODE" ] && [ -z "$rep" ]; then pass "$id"; else fail "$id" "rc=$RC" "${rep:-$(tail -n 10 "$WORK/$id.err")}"; fi
                done
            fi
            unset TSAN_OPTIONS ASAN_OPTIONS UBSAN_OPTIONS
        done
    done
fi

# ---------------------------------------------------------------- итог
printf '\nИТОГ: %d PASS, %d FAIL, %d SKIP\n' "$PASSED" "$FAILED" "$SKIPPED"
if [ "$FAILED" -gt 0 ]; then
    printf 'Провалены:\n'; printf '  %s\n' "${FAILED_NAMES[@]}"
    exit 1
fi
exit 0

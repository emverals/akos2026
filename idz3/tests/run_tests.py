#!/usr/bin/env python3
"""Проверки четырёх вариантов ИДЗ-3; журналы сохраняются в tests/out."""
import hashlib
import ctypes
import fcntl
import pty
import select
import termios
import os
import pathlib
import re
import shlex
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / 'tests' / 'out'
OUT.mkdir(exist_ok=True)
os.chdir(ROOT)
FAIL = 0
RUNNING = []
SESSIONS = set()


def check(name, fn):
    global FAIL
    try:
        fn()
        print('PASS', name, flush=True)
    except (AssertionError, OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
        FAIL += 1
        print('FAIL', name, str(e), flush=True)
    finally:
        cleanup()
        for session in SESSIONS:
            leftovers = list(pathlib.Path('/dev/shm').glob(f'*tank3_{session}_*'))
            if pathlib.Path('/dev/mqueue').is_dir():
                leftovers += list(pathlib.Path('/dev/mqueue').glob(f'tank3_{session}_*'))
            if leftovers:
                FAIL += 1
                print('FAIL', name, 'остатки IPC:', *leftovers, flush=True)
        SESSIONS.clear()


def cleanup():
    for p in RUNNING:
        # Группа принадлежит тесту; после смерти owner в ней ещё могут быть дети.
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    for p in RUNNING:
        try:
            p.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
    RUNNING.clear()


def start(args, name):
    f = open(OUT / (name + '.out'), 'w')
    p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT,
                         start_new_session=True)
    f.close()
    RUNNING.append(p)
    return p


def wait_for(predicate, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise AssertionError('превышено время ожидания')


def commands(name, owner, expected=10):
    def read():
        text = (OUT / (name + '.out')).read_text(errors='replace')
        lines = [shlex.split(x) for x in text.splitlines() if x.startswith('./tanks_worker ')]
        if owner.poll() is not None:
            raise AssertionError('owner завершился до запуска работников: ' + text[-600:])
        return lines if len(lines) == expected else None
    lines = wait_for(read)
    assert len({tuple(x) for x in lines}) == expected, 'команды работников повторяются'
    sessions = {line[line.index('--session') + 1] for line in lines}
    assert len(sessions) == 1, 'работники относятся к разным сессиям'
    SESSIONS.update(sessions)
    return lines


def workers(lines, prefix):
    return [start(args, f'{prefix}_worker_{i}') for i, args in enumerate(lines)]


def finish(p, seconds=20, expected=0):
    rc = p.wait(timeout=seconds)
    assert rc == expected, f'код {rc}, ожидался {expected}'


def args(mode, seed, name, *extra):
    return [f'./tanks_{mode}', '--batch', '--plain', '--delay', '0', '--seed', str(seed),
            '--log', str(OUT / (name + '.log')), *extra]


def battle(mode, seed, name, *extra, expected=10, reason="normal"):
    owner = start(args(mode, seed, name, *extra), name)
    ws = []
    if mode in ('independent', 'queue'):
        lines = commands(name, owner, expected)
        time.sleep(.1)
        assert owner.poll() is None, 'бой начался без работников'
        ws = workers(lines, name)
    finish(owner)
    for w in ws:
        finish(w)
    assert_no_ipc(owner.pid)
    subprocess.run([sys.executable, '-I', 'tests/check_log.py', str(OUT / (name + '.log')),
                    '--expect-reason', reason], check=True, timeout=10)
    return (OUT / (name + '.log')).read_text()


def assert_no_ipc(pid):
    leftovers = list(pathlib.Path('/dev/shm').glob(f'*tank3_tanks_{pid}_*'))
    leftovers += list(pathlib.Path('/dev/mqueue').glob(f'tank3_tanks_{pid}_*'))
    assert not leftovers, 'остатки IPC собственной сессии: ' + ', '.join(map(str, leftovers))


def normalize(text):
    return '\n'.join(line.replace('поток танка ', 'процесс танка ') for line in text.splitlines()
                     if not line.startswith(('# tanks', '# повтор:', '# потоки:', '# процессы:')))


def normal(mode):
    text = battle(mode, 27, mode + '_27')
    assert re.search(r'^Результат: ничья', text, re.M), 'seed 27: нет ничьей'
    # Отпечаток нормализованного журнала ИДЗ-2 tanks_sem, glibc, seed 27.
    assert hashlib.sha256(normalize(text).encode()).hexdigest() == (
        '6b4a00b05d1d8d505454895e3e45d5274f1b59778a1fd427bd82ee15aee06a75'
    ), 'бой отличается от исходной реализации ИДЗ-2'
    base = OUT / 'reference.log'
    if not base.exists():
        base.write_text(normalize(text))
    else:
        assert normalize(text) == base.read_text(), 'журналы схем различаются'


def seed_changes():
    text = battle('unnamed', 1, 'unnamed_1')
    assert normalize(text) != (OUT / 'reference.log').read_text(), 'seed не меняет бой'


def invalid(mode):
    for option in (['--seed', '-1'], ['--rows', '2'], ['--unknown']):
        r = subprocess.run([f'./tanks_{mode}', '--batch', *option], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        assert r.returncode != 0, f'принят некорректный аргумент {option}'


def children(pid):
    try:
        return [int(x) for x in pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
    except FileNotFoundError:
        return []


def named_session(pid):
    try:
        maps = pathlib.Path(f'/proc/{pid}/maps').read_text()
    except FileNotFoundError:
        return None
    match = re.search(r'/dev/shm/tank3_([^/ ]+?)_meta(?:\s|$)', maps)
    return match.group(1) if match else None


def owner_signal(mode, sig):
    name = f'{mode}_{sig.name}'
    owner = start(args(mode, 1, name, '--delay', '100'), name)
    if mode in ('independent', 'queue'):
        ws = workers(commands(name, owner), name)
        wait_for(lambda: all(w.poll() is None for w in ws))
    else:
        ws = []
        wait_for(lambda: len(children(owner.pid)) >= 10)
        if mode == 'named':
            SESSIONS.add(wait_for(lambda: named_session(owner.pid)))
    os.kill(owner.pid, sig)
    finish(owner, 10, 128 + sig)
    for w in ws:
        w.wait(timeout=10)


def worker_death(mode):
    name = mode + '_worker_death'
    owner = start(args(mode, 1, name, '--delay', '100'), name)
    if mode in ('independent', 'queue'):
        ws = workers(commands(name, owner), name)
        victim = ws[0].pid
    else:
        ws = []
        wait_for(lambda: len(children(owner.pid)) == 10)
        SESSIONS.add(wait_for(lambda: named_session(owner.pid)))
    ids = [w.pid for w in ws] if ws else children(owner.pid)
    victim = ids[0]
    wait_for(lambda: '[T2] состояние поля после такта' in (OUT / (name + '.log')).read_text())
    os.kill(victim, signal.SIGKILL)
    rc = owner.wait(timeout=10)
    assert rc != 0, 'гибель процесса не обнаружена'
    for w in ws:
        w.wait(timeout=5)
    wait_for(lambda: all(exited(pid) for pid in ids), 5)


def duplicate(mode):
    name = mode + '_duplicate'
    owner = start(args(mode, 1, name, '--delay', '100'), name)
    lines = commands(name, owner)
    first = start(lines[0], name + '_first')
    time.sleep(.2)
    other = start(lines[0], name + '_duplicate')
    rc = other.wait(timeout=5)
    assert rc != 0, 'повторная регистрация работника принята'
    assert first.poll() is None, 'первый работник завершился до READY остальных'
    os.kill(owner.pid, signal.SIGTERM)
    finish(owner, 10, 143)
    first.wait(timeout=10)


def boundaries(mode):
    for label, seed, rows, cols, prob, team, count in (
            ('min', 0, 3, 1, 1, '1 0 0 0', 4),
            ('max', 4294967295, 12, 12, 0, '2 2 1 1', 14)):
        text = battle(mode, seed, mode + '_' + label,
                      '--rows', str(rows), '--cols', str(cols), '--prob', str(prob),
                      '--team-a', team, '--team-b', team, '--max-ticks', '3',
                      expected=count, reason='limit')
        assert f'seed={seed} rows={rows} cols={cols} prob={prob}' in text
    for option in (['--rows', '13'], ['--cols', '0'], ['--cols', '13'],
                   ['--prob', '-0.1'], ['--prob', '1.1'], ['--seed', '4294967296'],
                   ['--rows', '3', '--cols', '1', '--team-a', '2'],
                   ['--team-b', '0 0 0 0']):
        r = subprocess.run([f'./tanks_{mode}', '--batch', *option],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        assert r.returncode == 1, f'неверный код ошибки {option}: {r.returncode}'


def config_priority():
    for i, options in enumerate((['--seed', '29', '--config', 'tests/data/priority.conf'],
                                 ['--config', 'tests/data/priority.conf', '--seed', '29'])):
        text = battle('unnamed', 1, 'config_' + str(i), *options,
                      '--max-ticks', '2', reason='limit')
        assert '# seed=29 rows=6 cols=9 prob=0 max_ticks=2 delay_ms=0 focus=off' in text
        assert 'Решений, изменённых фокусом координатора: A 0, B 0 (фокус отключён)' in text
    text = battle('named', 7, 'no_focus', '--no-focus')
    assert 'фокус отключён (--no-focus)' in text
    assert 'Решений, изменённых фокусом координатора: A 0, B 0 (фокус отключён)' in text


def process_model(mode):
    name = mode + '_processes'
    owner = start(args(mode, 1, name, '--max-ticks', '0', '--delay', '100',
                       '--rows', '3', '--cols', '1', '--prob', '1',
                       '--team-a', '1', '--team-b', '1'), name)
    if mode in ('independent', 'queue'):
        ws = workers(commands(name, owner, 4), name)
        ids = [w.pid for w in ws]
        assert not children(owner.pid), 'independent owner создал дочерние процессы'
    else:
        ws = []
        ids = wait_for(lambda: children(owner.pid) if len(children(owner.pid)) == 4 else None)
        SESSIONS.add(wait_for(lambda: named_session(owner.pid)))
    wait_for(lambda: '=== Такт 2 ===' in (OUT / (name + '.log')).read_text())
    for pid in [owner.pid, *ids]:
        assert len(list(pathlib.Path(f'/proc/{pid}/task').iterdir())) == 1, f'{pid}: несколько потоков'
        if mode in ('independent', 'queue'):
            assert not children(pid), 'independent worker создал дочерние процессы'
    os.kill(owner.pid, signal.SIGTERM)
    finish(owner, 10, 143)
    for w in ws:
        finish(w)
    wait_for(lambda: all(exited(pid) for pid in ids), 3)
    subprocess.run([sys.executable, '-I', 'tests/check_log.py', str(OUT / (name + '.log')),
                    '--expect-reason', 'signal'], check=True, timeout=5)


def exited(pid):
    try:
        return pathlib.Path(f'/proc/{pid}/stat').read_text().split(') ')[1][0] == 'Z'
    except FileNotFoundError:
        return True


def worker_signal(mode, sig, startup=False):
    name = f'{mode}_worker_{sig.name}_{"startup" if startup else "battle"}'
    owner = start(args(mode, 1, name, '--delay', '500', '--max-ticks', '0',
                       '--rows', '3', '--cols', '1', '--prob', '1',
                       '--team-a', '1', '--team-b', '1'), name)
    if mode in ('independent', 'queue'):
        lines = commands(name, owner, 4)
        ws = workers(lines[:1] if startup else lines, name)
        victim = ws[0].pid
    else:
        ws = []
        victim = wait_for(lambda: children(owner.pid) if len(children(owner.pid)) == 4 else None)[0]
        SESSIONS.add(wait_for(lambda: named_session(owner.pid)))
    if not startup:
        wait_for(lambda: '[T2] состояние поля после такта' in (OUT / (name + '.log')).read_text())
        os.kill(victim, signal.SIGSTOP)
        wait_for(lambda: '=== Такт 3 ===' in (OUT / (name + '.log')).read_text())
    else:
        time.sleep(.1)  # первый READY, остальные процессы ещё не запущены
    os.kill(victim, sig)
    if not startup:
        os.kill(victim, signal.SIGCONT)
    if startup:
        finish(ws[0], 3, 128 + sig)
        finish(owner, 10, 128 + sig)
        text = (OUT / (name + '.log')).read_text()
        assert 'Причина завершения: signal' in text, 'сигнал при запуске потерян в итогах'
        assert sig.name in text, 'нет имени сигнала при запуске'
    else:
        assert owner.wait(timeout=10) != 0, 'сигнал работнику не обнаружен'
        for w in ws:
            w.wait(timeout=5)
    wait_for(lambda: exited(victim), 3)


def partial_start(mode):
    name = mode + '_partial'
    owner = start(args(mode, 1, name), name)
    ws = workers(commands(name, owner)[:-1], name)
    assert owner.wait(timeout=35) == 2, 'неполный состав принят'
    for w in ws:
        w.wait(timeout=5)
    assert '=== Такт 1 ===' not in (OUT / (name + '.log')).read_text()


def registration_timeout(mode):
    name = mode + '_registration_timeout'
    owner = start(args(mode, 1, name), name)
    line = commands(name, owner)[0]
    session = line[line.index('--session') + 1]
    with open('/dev/shm/tank3_' + session + '_meta', 'r+b') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        worker = start(line, name + '_worker')
        assert worker.wait(timeout=7) == 2, 'блокировка регистрации не ограничена'
    os.kill(owner.pid, signal.SIGTERM)
    finish(owner, 10, 143)


def owner_death(mode):
    name = mode + '_owner_death'
    owner = start(args(mode, 1, name, '--delay', '100'), name)
    if mode in ('independent', 'queue'):
        lines = commands(name, owner)
        session = lines[0][lines[0].index('--session') + 1]
        ws = workers(lines, name)
        ids = [w.pid for w in ws]
    else:
        ws = []
        ids = wait_for(lambda: children(owner.pid) if len(children(owner.pid)) == 10 else None)
        session = wait_for(lambda: named_session(owner.pid))
    time.sleep(.1)
    os.kill(owner.pid, signal.SIGKILL)
    finish(owner, 3, -signal.SIGKILL)
    try:
        for w in ws:
            assert w.wait(timeout=5) != 0
        wait_for(lambda: all(exited(pid) for pid in ids), 5)
    finally:
        # SIGKILL не даёт владельцу исполнить cleanup. Удаляем только IPC
        # созданной этим тестом сессии, никогда не затрагивая остальные имена.
        libc = ctypes.CDLL(None, use_errno=True)
        for path in pathlib.Path('/dev/shm').glob(f'*tank3_{session}_*'):
            if path.name.startswith('sem.'):
                assert libc.sem_unlink(('/' + path.name[4:]).encode()) == 0
            else:
                assert libc.shm_unlink(('/' + path.name).encode()) == 0
        for path in pathlib.Path('/dev/mqueue').glob(f'tank3_{session}_*'):
            assert libc.mq_unlink(('/' + path.name).encode()) == 0


def io_failures(mode):
    for label, extra, output in (
            ('open', ['--log', str(OUT)], subprocess.DEVNULL),
            ('log_full', ['--log', '/dev/full'], subprocess.DEVNULL),
            ('stdout_full', [], None)):
        name = mode + '_' + label
        with open('/dev/full', 'w') as full:
            p = subprocess.Popen(args(mode, 1, name, '--max-ticks', '1', *extra),
                                 stdin=subprocess.DEVNULL,
                                 stdout=full if output is None else output,
                                 stderr=subprocess.PIPE, start_new_session=True)
            RUNNING.append(p)
            try:
                stderr = p.communicate(timeout=3)[1].decode(errors='replace')
            except subprocess.TimeoutExpired:
                for path in pathlib.Path('/dev/shm').glob(f'tank3_tanks_{p.pid}_*_meta'):
                    SESSIONS.add(path.name[len('tank3_'):-len('_meta')])
                raise
        assert p.returncode == 2, f'{label}: ожидалась ошибка I/O, код {p.returncode}'
        assert stderr.strip(), f'{label}: нет диагностики ошибки в stderr'
        if label in ('open', 'log_full'):
            assert 'журнал' in stderr.lower(), 'нет диагностики журнала в stderr'
        assert_no_ipc(p.pid)


def terminal():
    def run(label, input_bytes, expected, ansi=False, interrupt=False):
        name = 'pty_' + label
        log = OUT / (name + '.log')
        log.unlink(missing_ok=True)
        master, slave = pty.openpty()
        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        argv = ['./tanks_unnamed', '--seed', '27', '--delay', '0', '--max-ticks', '1',
                '--log', str(log)]
        if ansi:
            argv += ['--batch', '--ansi', '--no-color']
            if interrupt:
                argv += ['--rows', '3', '--cols', '1', '--prob', '1', '--team-a', '1',
                         '--team-b', '1', '--max-ticks', '0', '--delay', '100']
        p = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                             preexec_fn=controlling_terminal)
        RUNNING.append(p)
        os.close(slave)
        raw = bytearray()
        try:
            end = time.monotonic() + 10
            sent = ansi and not interrupt
            while time.monotonic() < end:
                if select.select([master], [], [], .05)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError as e:
                        if e.errno != 5:
                            raise
                        break
                    if not chunk:
                        break
                    raw.extend(chunk)
                if not sent and ((b'\x1b[?25l' in raw) if interrupt else ('Размер поля'.encode() in raw)):
                    os.write(master, input_bytes)
                    sent = True
            finish(p, 2, expected)
        finally:
            os.close(master)
            (OUT / (name + '.out')).write_bytes(raw)
        if expected == 0 or interrupt:
            subprocess.run([sys.executable, '-I', 'tests/check_log.py', str(log),
                            '--expect-reason', 'signal' if interrupt else 'limit'], check=True, timeout=5)
        else:
            assert not log.exists(), 'бой стартовал после прерывания настройки'
        return raw.decode(errors='replace'), log
    text, log = run('defaults', b'\n\n\n\n\n', 0)
    assert 'rows=10 cols=10 prob=0.4' in log.read_text()
    text, log = run('manual_invalid', b'bad\n2 1\n3 1\n2\n1\n3\n1\n0 0 0 0\n1 0 0 0\n0 1 0 0\n', 0)
    assert 'rows=3 cols=1 prob=1' in log.read_text()
    assert 'Введите 1 или 2' in text and 'сумма — от 1 до 1' in text
    run('eof', b'\x04', 3)
    run('ctrl_c', b'\x03', 130)
    for label, data, rc, interrupt in (('ansi', b'', 0, False),
                                       ('ansi_ctrl_c', b'\x03', 130, True)):
        text, log = run(label, data, rc, ansi=True, interrupt=interrupt)
        assert '\x1b[?25l' in text and '\x1b[?25h' in text, 'курсор не скрыт/восстановлен'
        assert text.rfind('\x1b[?25h') > text.rfind('\x1b[?25l'), 'курсор остался скрытым'


def ipc_contract():
    executable = OUT / 'ipc_tests'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-I.',
                    'tests/ipc_tests.c', 'ipc.c', '-o', str(executable), '-pthread', '-lrt'],
                   check=True, timeout=30)
    subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == '__main__':
    try:
        (OUT / 'reference.log').unlink(missing_ok=True)
        for mode in ('unnamed', 'named', 'independent', 'queue'):
            check(mode + ': normal, ничья, инварианты и совпадение', lambda m=mode: normal(m))
        check('seed влияет на бой', seed_changes)
        for mode in ('unnamed', 'named', 'independent', 'queue'):
            check(mode + ': некорректные аргументы', lambda m=mode: invalid(m))
            for sig in (signal.SIGINT, signal.SIGTERM):
                check(mode + ': ' + sig.name, lambda m=mode, s=sig: owner_signal(m, s))
            check(mode + ': гибель работника', lambda m=mode: worker_death(m))
        for mode in ('independent', 'queue'):
            check(mode + ': повторная регистрация', lambda m=mode: duplicate(m))
        for mode in ('independent', 'queue'):
            check(mode + ': неполный запуск ограничен', lambda m=mode: partial_start(m))
            check(mode + ': блокировка регистрации ограничена', lambda m=mode: registration_timeout(m))
            for sig in (signal.SIGINT, signal.SIGTERM):
                check(mode + ': worker startup ' + sig.name,
                      lambda m=mode, s=sig: worker_signal(m, s, startup=True))
        check('реальный терминал: defaults/manual/invalid/EOF/Ctrl+C/ANSI', terminal)
        check('конфиг, CLI priority и no-focus', config_priority)
        check('контракты IPC и сохранение чужих ресурсов', ipc_contract)
        for mode in ('unnamed', 'named', 'independent', 'queue'):
            check(mode + ': границы поля/составов/seed', lambda m=mode: boundaries(m))
            check(mode + ': процессы, один поток, max-ticks=0', lambda m=mode: process_model(m))
            check(mode + ': ошибки stdout и журнала', lambda m=mode: io_failures(m))
            for sig in (signal.SIGINT, signal.SIGTERM):
                check(mode + ': worker battle ' + sig.name, lambda m=mode, s=sig: worker_signal(m, s))
            check(mode + ': гибель владельца, bounded exit', lambda m=mode: owner_death(m))
    finally:
        cleanup()
    print('Ошибок:', FAIL)
    sys.exit(bool(FAIL))

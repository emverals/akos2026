#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================
 * 1. ПАРАМЕТРЫ МОДЕЛИ
 * ====================================================================== */

#define MAX_N 12
#define MAX_M 12
/* Танки стоят через клетку, поэтому в стартовой строке помещается (m+1)/2. */
#define MAX_TEAM_TANKS ((MAX_M + 1) / 2)
#define MAX_TANKS (2 * MAX_TEAM_TANKS)
#define TANK_TYPES 4
#define CELL_W 11
#define CELL_H 5
#define TURN_PROBABILITY 0.15
#define LOG_LINES 6

#define DEFAULT_ROWS 10
#define DEFAULT_COLS 10
#define DEFAULT_PROB 0.4
#define DEFAULT_TEAM_SIZE 4
#define DEFAULT_MAX_TICKS 600
#define DEFAULT_DELAY_MS 350
#define MAX_DELAY_MS 10000
#define MAX_TICKS_LIMIT 10000000
#define DEFAULT_LOG_PATH "tanks_cond.log"

#define EXIT_OK 0
#define EXIT_BAD_INPUT 1
#define EXIT_SYSTEM 2
#define EXIT_NO_BATTLE 3

/* ======================================================================
 * 2. МОДЕЛЬ ПРЕДМЕТНОЙ ОБЛАСТИ
 * ====================================================================== */

enum Direction { UP, DOWN, LEFT, RIGHT };
enum Move { WAIT, SHOOT, AIM, MOVE };
/* Что остановило луч наблюдения/выстрела. */
enum RayStop { RAY_EDGE, RAY_WALL, RAY_TANK };

typedef struct {
    const char *name;
    int hp, speed, damage, cooldown, weight;
} TankType;

/* Характеристики ДЗ-1; масса задаёт очередь применения перемещений. */
static const TankType TYPES[TANK_TYPES] = {
    {"mouse", 300, 3, 45, 14, 188},
    {"fv215b183", 130, 2, 100, 24, 60},
    {"t62a", 210, 4, 60, 6, 37},
    {"leopard_1", 180, 5, 70, 8, 40},
};

static const char *const DIR_NAME[4] = {"вверх", "вниз", "влево", "вправо"};

/* Четыре рисунка на тип, порядок направлений как в enum Direction. */
static const wchar_t ICONS[TANK_TYPES][4][CELL_H][CELL_W + 1] = {
    {
        {L"   ▄▄▄▄▄   ", L"  █  |  █  ", L"  █ ▄█▄ █  ", L"  █ ███ █  ", L"  █▄▄▄▄▄█  "},
        {L"  █▀▀▀▀▀█  ", L"  █ ███ █  ", L"  █ ▀█▀ █  ", L"  █  |  █  ", L"   ▀▀▀▀▀   "},
        {L"           ", L"▄▀▀▀▀▀▀▀▀▀█", L"█ ----███ █", L"▀▄▄▄▄▄▄▄▄▄█", L"           "},
        {L"           ", L"█▀▀▀▀▀▀▀▀▀▄", L"█ ███---- █", L"█▄▄▄▄▄▄▄▄▄▀", L"           "},
    },
    {
        {L"   _ ▼ _   ", L"  /  |  \\  ", L"  | ███ |  ", L"  █ --- █  ", L"  ▀-▄▄▄-▀  "},
        {L"  ▄-▀▀▀-▄  ", L"  █ --- █  ", L"  | ███ |  ", L"  \\  |  /  ", L"   ‾ ▲ ‾   "},
        {L"           ", L"  /‾‾‾▀▀▀▀|", L"▶------██|█", L"  \\___▄▄▄▄|", L"           "},
        {L"           ", L"|▀▀▀▀‾‾‾\\  ", L"█|██------◀", L"|▄▄▄▄___/  ", L"           "},
    },
    {
        {L"  /‾ | ‾\\  ", L"  █ ▄▄▄ █  ", L"  █ ▀▀▀ █  ", L"  █ === █  ", L"  █▄▄▄▄▄█  "},
        {L"  █▀▀▀▀▀█  ", L"  █ === █  ", L"  █ ▄▄▄ █  ", L"  █ ▀▀▀ █  ", L"  \\_ | _/  "},
        {L"           ", L"/▀▀▀▀▀▀▀▀▀▄", L"----██  ||█", L"\\▄▄▄▄▄▄▄▄▄▀", L"           "},
        {L"           ", L"▄▀▀▀▀▀▀▀▀▀\\", L"█||  ██----", L"▀▄▄▄▄▄▄▄▄▄/", L"           "},
    },
    {
        {L"  /‾▀▀▀‾\\  ", L"  |  |  |  ", L"  | ▄█▄ |  ", L"  | ‾‾‾ |  ", L"   ‾▀▀▀‾   "},
        {L"   _▄▄▄_   ", L"  | ___ |  ", L"  | ▀█▀ |  ", L"  |  |  |  ", L"  \\_▄▄▄_/  "},
        {L"           ", L"/‾‾‾‾‾‾‾‾‾▄", L"█ --|█|  |█", L"\\_________▀", L"           "},
        {L"           ", L"▄‾‾‾‾‾‾‾‾‾\\", L"█|  |█|-- █", L"▀_________/", L"           "},
    },
};

/* Состояние машины. Характеристики типа берутся из TYPES[type]. */
typedef struct {
    int x, y;            /* левый верхний угол области 11x5, в символах */
    int type, number;    /* тип и постоянный номер внутри команды (с 1) */
    int team;            /* 0 — A, 1 — B */
    bool alive;
    int hp;              /* всегда в [0, TYPES[type].hp]; 0 только у уничтоженных */
    enum Direction dir;
    int last_shot;       /* такт последнего выстрела */
    bool aimed;          /* танк прицелился по направлению dir */
} Tank;

/* Поле боя. Пишет только арбитр (и main до запуска потоков). */
typedef struct {
    int rows, cols;
    bool wall[MAX_N][MAX_M];
    int count;
    /* Индекс в массиве = место в очереди применения (масса по убыванию). */
    Tank tanks[MAX_TANKS];
    int team_size[2];
    int alive[2];
    int frame;           /* число завершённых тактов */
} World;

/* Результат наблюдения одного танка: что первым встречено по каждому лучу. */
typedef struct {
    int hit[4];          /* индекс танка или -1 */
    enum RayStop stop[4];
    int dist[4];         /* шагов луча от центра танка */
} Observation;

/* Сводка донесений команды за такт. Все операции слияния коммутативны,
 * поэтому содержимое не зависит от порядка, в котором доложили танки. */
typedef struct {
    int reports;
    int seen_by[MAX_TANKS];          /* сколько танков команды видят противника j */
    unsigned observers[MAX_TANKS];   /* битовая маска наблюдателей противника j */
} Sightings;

/* Разведданные, которые координатор публикует своей команде. */
typedef struct {
    int focus;           /* противник для сосредоточенного огня или -1 */
    int spotted;
} Intel;

/* Решение танка на такт. */
typedef struct {
    enum Move action;
    enum Direction dir;
    int length;          /* запланированная длина пути по снимку поля */
    bool by_focus;       /* выбор цели изменён фокусом координатора */
} Plan;

typedef struct {
    int shots, hits, dealt, taken, kills, moved;
    int death_tick;      /* такт уничтожения (с 1) или 0 */
} TankStats;

typedef struct {
    TankStats tank[MAX_TANKS];
    int conflicts;
    int focus_decisions[2];
    int violations;
} Stats;

enum ScreenKind { SCR_HIT, SCR_MISS, SCR_DEAD };
typedef struct {
    enum ScreenKind kind;
    int actor, target, damage;
} ScreenEvent;

/* Последние LOG_LINES событий стрельбы и уничтожения для экрана (как в ДЗ-1). */
typedef struct {
    ScreenEvent ev[LOG_LINES];
    int count;
} ScreenLog;

typedef struct {
    bool random;
    int count;
    int comp[TANK_TYPES];
    bool set;
} TeamSpec;

enum Phase {
    PH_OBSERVE, PH_COORDINATE, PH_DECIDE, PH_SHOOT, PH_DESTROY, PH_MOVE,
    PH_RENDER, PH_DELAY, PH_COUNT
};
static const char *const PHASE_KEY[PH_COUNT] = {
    "observe", "coordinate", "decide", "shoot", "destroy", "move", "render", "delay"
};
static const char *const PHASE_NAME[PH_COUNT] = {
    "наблюдение", "координация", "решение", "стрельба", "уничтожение",
    "перемещение", "отрисовка", "задержка"
};

typedef struct {
    unsigned seed;
    bool seed_set;
    int delay_ms;
    int max_ticks;               /* 0 — без лимита */
    char *log_path;              /* всегда собственная копия */
    bool color, plain, ansi, batch, focus;
    int rows, cols;
    double prob;
    TeamSpec team[2];
    bool field_given;            /* задан параметр поля/состава -> без вопросов */
    int inject_tick;             /* тестовый самосигнал: 0 — выключен */
    enum Phase inject_phase;
    int inject_sig;
} Settings;

/* ======================================================================
 * 3. БУФЕРЫ И ВВОД-ВЫВОД ЧЕРЕЗ ДЕСКРИПТОРЫ
 * ====================================================================== */

/* Растущий текстовый буфер. failed выставляется при нехватке памяти, после
 * чего добавления игнорируются, а владелец сообщает о потере данных. */
typedef struct {
    char *data;
    size_t len, cap;
    bool failed;
} StrBuf;

static bool sb_reserve(StrBuf *b, size_t extra)
{
    if (b->failed)
        return false;
    if (extra > SIZE_MAX / 2 - b->len) {
        b->failed = true;
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) {
        b->failed = true;
        return false;
    }
    b->data = p;
    b->cap = cap;
    return true;
}

static void sb_put(StrBuf *b, const char *s, size_t n)
{
    if (!sb_reserve(b, n))
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_puts(StrBuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_printf(StrBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(StrBuf *b, const char *fmt, ...)
{
    if (!sb_reserve(b, 128))
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->data[b->len] = '\0';
        b->failed = true;
        return;
    }
    if ((size_t)n >= b->cap - b->len) {
        if (!sb_reserve(b, (size_t)n)) {
            b->data[b->len] = '\0';
            return;
        }
        va_start(ap, fmt);
        vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
    }
    b->len += (size_t)n;
}

/* Терминал получает UTF-8 независимо от локали: кодируем сами. */
static void sb_putwc(StrBuf *b, wchar_t wc)
{
    unsigned long c = (unsigned long)wc;
    char u[4];
    size_t n;
    if (c < 0x80) {
        u[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        u[0] = (char)(0xC0 | (c >> 6));
        u[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        u[0] = (char)(0xE0 | (c >> 12));
        u[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        u[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (c >> 18));
        u[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        u[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    sb_put(b, u, n);
}

static void sb_spaces(StrBuf *b, int n)
{
    for (int i = 0; i < n; i++)
        sb_put(b, " ", 1);
}

/* Ширина UTF-8 строки в символах (кириллица занимает два байта). */
static int utf8_width(const char *s)
{
    int w = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            w++;
    return w;
}

/* Ячейка таблицы фиксированной ширины с выравниванием по символам. */
static void sb_cell(StrBuf *b, const char *s, int width, bool right)
{
    int pad = width - utf8_width(s);
    if (right)
        sb_spaces(b, pad);
    sb_puts(b, s);
    if (!right)
        sb_spaces(b, pad);
}

static void sb_clear(StrBuf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

static void sb_free(StrBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->failed = false;
}

/* write(2) до конца: повтор при EINTR и при частичной записи. */
static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (k == 0) {
            errno = EIO;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* Короткие сообщения форматируются на стеке; длинные (справка) — в буфер
 * точного размера по результату vsnprintf, без обрезки. */
static int fd_vprintf(int fd, const char *fmt, va_list ap)
{
    char small[1024];
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    if (n < 0) {
        va_end(again);
        return -1;
    }
    if ((size_t)n < sizeof small) {
        va_end(again);
        return write_all(fd, small, (size_t)n);
    }
    char *big = malloc((size_t)n + 1);
    if (!big) {
        va_end(again);
        errno = ENOMEM;
        return -1;
    }
    int m = vsnprintf(big, (size_t)n + 1, fmt, again);
    va_end(again);
    int rc = m == n ? write_all(fd, big, (size_t)n) : -1;
    free(big);
    return rc;
}

static int out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int out_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = fd_vprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return r;
}

/* Диагностика в stderr. Если и stderr недоступен, сообщить уже некуда,
 * поэтому результат записи здесь сознательно не проверяется. */
static void err_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)fd_vprintf(STDERR_FILENO, fmt, ap);
    va_end(ap);
}

/* ======================================================================
 * 4. ИГРОВАЯ ЛОГИКА — чистые функции.
 *    Получают состояние параметрами, пишут события в переданный буфер,
 *    случайность — только через переданное состояние rand_r.
 * ====================================================================== */

typedef struct {
    char s[24];
} Label;

/* "A1 mouse" — команда, номер и тип. */
static Label tank_label(const Tank *t)
{
    Label l;
    snprintf(l.s, sizeof l.s, "%c%d %s", t->team ? 'B' : 'A', t->number, TYPES[t->type].name);
    return l;
}

/* "A1" — короткое обозначение. */
static Label tank_tag(const Tank *t)
{
    Label l;
    snprintf(l.s, sizeof l.s, "%c%d", t->team ? 'B' : 'A', t->number);
    return l;
}

static int imin(int a, int b)
{
    return a < b ? a : b;
}

static int dx(enum Direction d)
{
    return d == LEFT ? -1 : d == RIGHT ? 1 : 0;
}

static int dy(enum Direction d)
{
    return d == UP ? -1 : d == DOWN ? 1 : 0;
}

static enum Direction opposite(enum Direction d)
{
    switch (d) {
    case UP: return DOWN;
    case DOWN: return UP;
    case LEFT: return RIGHT;
    case RIGHT: return LEFT;
    }
    return UP;
}

static void side_directions(enum Direction d, enum Direction sides[2])
{
    bool vertical = d == UP || d == DOWN;
    sides[0] = vertical ? LEFT : UP;
    sides[1] = vertical ? RIGHT : DOWN;
}

static bool contains(const Tank *t, int x, int y)
{
    return x >= t->x && x < t->x + CELL_W && y >= t->y && y < t->y + CELL_H;
}

static bool rects_overlap(int ax, int ay, int bx, int by)
{
    return ax < bx + CELL_W && ax + CELL_W > bx && ay < by + CELL_H && ay + CELL_H > by;
}

static bool reloaded(const World *w, const Tank *t)
{
    return w->frame - t->last_shot >= TYPES[t->type].cooldown;
}

static int reload_left(const World *w, const Tank *t)
{
    int left = TYPES[t->type].cooldown - (w->frame - t->last_shot);
    return left > 0 ? left : 0;
}

/* Равномерное число в [0, 1). Делитель RAND_MAX + 1, чтобы вероятность 1
 * срабатывала всегда, а 0 — никогда. */
static double rand01(unsigned *rng)
{
    return (double)rand_r(rng) / ((double)RAND_MAX + 1.0);
}

/* first_hit из ДЗ-1: луч из центра танка по одному символу до края,
 * препятствия или первого живого танка. Возвращает индекс танка или -1 и
 * дополнительно сообщает, что остановило луч и на каком шаге. */
static int cast_ray(const World *w, int self, enum Direction dir, enum RayStop *stop, int *dist)
{
    const Tank *t = &w->tanks[self];
    int x = t->x + CELL_W / 2;
    int y = t->y + CELL_H / 2;
    for (int step = 1;; step++) {
        x += dx(dir);
        y += dy(dir);
        *dist = step;
        if (x < 0 || x >= w->cols * CELL_W || y < 0 || y >= w->rows * CELL_H) {
            *stop = RAY_EDGE;
            return -1;
        }
        if (w->wall[y / CELL_H][x / CELL_W]) {
            *stop = RAY_WALL;
            return -1;
        }
        for (int i = 0; i < w->count; i++) {
            const Tank *o = &w->tanks[i];
            if (i == self || !o->alive)
                continue;
            if (contains(o, x, y)) {
                *stop = RAY_TANK;
                return i;
            }
        }
    }
}

/* Можно ли поставить танк id левым верхним углом в (x, y). */
static bool can_place(const World *w, int id, int x, int y)
{
    if (x < 0 || y < 0 || x + CELL_W > w->cols * CELL_W || y + CELL_H > w->rows * CELL_H)
        return false;
    for (int row = y / CELL_H; row <= (y + CELL_H - 1) / CELL_H; row++)
        for (int col = x / CELL_W; col <= (x + CELL_W - 1) / CELL_W; col++)
            if (w->wall[row][col])
                return false;
    for (int i = 0; i < w->count; i++) {
        if (i == id || !w->tanks[i].alive)
            continue;
        if (rects_overlap(x, y, w->tanks[i].x, w->tanks[i].y))
            return false;
    }
    return true;
}

/* Максимальная доступная длина хода в направлении dir (не больше скорости). */
static int move_length(const World *w, int id, enum Direction dir)
{
    const Tank *t = &w->tanks[id];
    int speed = TYPES[t->type].speed;
    for (int step = 1; step <= speed; step++)
        if (!can_place(w, id, t->x + dx(dir) * step, t->y + dy(dir) * step))
            return step - 1;
    return speed;
}

static int choose_side(const int lengths[2], unsigned *rng)
{
    if (lengths[0] > 0 && lengths[1] > 0)
        return rand_r(rng) % 2;
    return lengths[0] > 0 ? 0 : 1;
}

static int first_alive(const World *w)
{
    for (int i = 0; i < w->count; i++)
        if (w->tanks[i].alive)
            return i;
    return -1;
}

static bool battle_over(const World *w, int max_ticks)
{
    return w->alive[0] == 0 || w->alive[1] == 0 || (max_ticks > 0 && w->frame >= max_ticks);
}

static int team_size_of(const TeamSpec *ts)
{
    if (ts->random)
        return ts->count;
    int sum = 0;
    for (int i = 0; i < TANK_TYPES; i++)
        sum += ts->comp[i];
    return sum;
}

/* Расстановка из seed. Порядок обращений к генератору как в ДЗ-1:
 * типы случайных команд, препятствия, перемешивание, сортировка по массе. */
static void world_setup(World *w, const Settings *cfg)
{
    unsigned rng = cfg->seed;
    memset(w, 0, sizeof *w);
    w->rows = cfg->rows;
    w->cols = cfg->cols;
    w->team_size[0] = team_size_of(&cfg->team[0]);
    w->team_size[1] = team_size_of(&cfg->team[1]);
    w->count = w->team_size[0] + w->team_size[1];
    for (int i = 0; i < w->count; i++) {
        int team = i >= w->team_size[0];
        int slot = team ? i - w->team_size[0] : i;
        const TeamSpec *ts = &cfg->team[team];
        int type = 0;
        if (ts->random) {
            type = rand_r(&rng) % TANK_TYPES;
        } else {
            int position = slot;
            while (position >= ts->comp[type]) {
                position -= ts->comp[type];
                type++;
            }
        }
        Tank *t = &w->tanks[i];
        t->type = type;
        t->team = team;
        t->number = slot + 1;
        t->x = (team ? w->cols - 1 - slot * 2 : slot * 2) * CELL_W;
        t->y = team ? (w->rows - 1) * CELL_H : 0;
        t->dir = team ? UP : DOWN;
        t->alive = true;
        t->hp = TYPES[type].hp;
        t->last_shot = -TYPES[type].cooldown;
        t->aimed = false;
    }
    /* Верхняя и нижняя строки свободны; в столбце не более одного препятствия. */
    for (int c = 0; c < w->cols; c++)
        if (rand01(&rng) < cfg->prob)
            w->wall[rand_r(&rng) % (w->rows - 2) + 1][c] = true;
    for (int i = w->count - 1; i > 0; i--) {
        int j = rand_r(&rng) % (i + 1);
        Tank tmp = w->tanks[i];
        w->tanks[i] = w->tanks[j];
        w->tanks[j] = tmp;
    }
    /* Устойчивая сортировка: при равной массе остаётся порядок жребия. */
    for (int i = 1; i < w->count; i++) {
        Tank t = w->tanks[i];
        int j = i;
        while (j > 0 && TYPES[w->tanks[j - 1].type].weight < TYPES[t.type].weight) {
            w->tanks[j] = w->tanks[j - 1];
            j--;
        }
        w->tanks[j] = t;
    }
    w->alive[0] = w->team_size[0];
    w->alive[1] = w->team_size[1];
    w->frame = 0;
}

/* --- Фаза наблюдения (поток танка) --- */

static void observe(const World *w, int self, Observation *o)
{
    for (int d = UP; d <= RIGHT; d++)
        o->hit[d] = cast_ray(w, self, (enum Direction)d, &o->stop[d], &o->dist[d]);
}

static void describe_observation(StrBuf *ev, const World *w, int self, const Observation *o)
{
    const Tank *t = &w->tanks[self];
    sb_printf(ev, "[T%d] наблюдение: %s @(%d,%d) смотрит %s", w->frame + 1,
              tank_label(t).s, t->x, t->y, DIR_NAME[t->dir]);
    for (int d = UP; d <= RIGHT; d++) {
        sb_printf(ev, " | %s: ", DIR_NAME[d]);
        if (o->stop[d] == RAY_EDGE) {
            sb_printf(ev, "край поля (%d)", o->dist[d]);
        } else if (o->stop[d] == RAY_WALL) {
            sb_printf(ev, "препятствие (%d)", o->dist[d]);
        } else {
            const Tank *v = &w->tanks[o->hit[d]];
            if (v->team == t->team) {
                sb_printf(ev, "союзник %s (%d)", tank_tag(v).s, o->dist[d]);
            } else if (reloaded(w, v)) {
                sb_printf(ev, "противник %s HP %d, заряжен (%d)", tank_label(v).s, v->hp, o->dist[d]);
            } else {
                sb_printf(ev, "противник %s HP %d, перезарядка %d (%d)", tank_label(v).s, v->hp,
                          reload_left(w, v), o->dist[d]);
            }
        }
    }
    sb_puts(ev, "\n");
}

/* Донесение на доску команды: только += и |=, порядок докладов не важен. */
static void sightings_merge(Sightings *s, const World *w, int self, const Observation *o)
{
    s->reports++;
    for (int d = UP; d <= RIGHT; d++) {
        int h = o->hit[d];
        if (h < 0 || w->tanks[h].team == w->tanks[self].team)
            continue;
        s->seen_by[h]++;
        s->observers[h] |= 1u << self;
    }
}

/* --- Фаза координации (поток координатора) ---
 * Фокус — слабейший по HP из обнаруженных противников; при равенстве — тот,
 * кого видит больше танков команды, затем — раньше стоящий в очереди. */
static void coordinate(const World *w, int team, const Sightings *s, bool use_focus,
                       Intel *out, StrBuf *ev)
{
    out->focus = -1;
    out->spotted = 0;
    int best = -1;
    for (int j = 0; j < w->count; j++) {
        if (s->seen_by[j] == 0)
            continue;
        out->spotted++;
        const Tank *c = &w->tanks[j];
        if (best == -1 || c->hp < w->tanks[best].hp ||
            (c->hp == w->tanks[best].hp && s->seen_by[j] > s->seen_by[best]))
            best = j;
    }
    sb_printf(ev, "[T%d] координатор %c: донесений %d; ", w->frame + 1, team ? 'B' : 'A', s->reports);
    if (out->spotted == 0) {
        sb_puts(ev, "противник не обнаружен, фокус огня не назначен\n");
        return;
    }
    sb_printf(ev, "разведданные (%d):", out->spotted);
    for (int j = 0; j < w->count; j++) {
        if (s->seen_by[j] == 0)
            continue;
        sb_printf(ev, " %s HP %d [видят:", tank_label(&w->tanks[j]).s, w->tanks[j].hp);
        for (int k = 0; k < w->count; k++)
            if (s->observers[j] & (1u << k))
                sb_printf(ev, " %s", tank_tag(&w->tanks[k]).s);
        sb_puts(ev, "]");
    }
    if (use_focus) {
        out->focus = best;
        sb_printf(ev, "; фокус огня -> %s\n", tank_label(&w->tanks[best]).s);
    } else {
        sb_puts(ev, "; фокус отключён (--no-focus)\n");
    }
}

/* --- Фаза решения (поток танка) ---
 * Логика ДЗ-1. Добавлено одно правило: если цель фокуса координатора видна
 * этому танку (первая на одном из лучей), она выбирается вместо цели по
 * правилу ДЗ-1. Видимость и запрет стрелять через союзника не нарушаются,
 * потому что фокус берётся только из hit[] — первых объектов на лучах. */
static void decide(const World *w, int self, const Observation *o, const Intel *intel,
                   unsigned *rng, Plan *p, StrBuf *ev)
{
    const Tank *t = &w->tanks[self];
    const TankType *tt = &TYPES[t->type];
    *p = (Plan){.action = WAIT, .dir = t->dir, .length = 0, .by_focus = false};
    if (!t->alive)
        return;
    sb_printf(ev, "[T%d] решение: %s — ", w->frame + 1, tank_label(t).s);

    int ahead = o->hit[t->dir];
    if (t->aimed && reloaded(w, t) && ahead != -1 && w->tanks[ahead].team != t->team) {
        p->action = SHOOT;
        sb_printf(ev, "цель %s (на линии прицела) — намерение: выстрел %s\n",
                  tank_label(&w->tanks[ahead]).s, DIR_NAME[t->dir]);
        return;
    }

    /* Видимый противник; приоритет у заряженного (правило ДЗ-1). */
    int enemy = -1;
    enum Direction enemy_dir = UP;
    for (int d = UP; d <= RIGHT; d++) {
        int hit = o->hit[d];
        if (hit == -1 || w->tanks[hit].team == t->team)
            continue;
        if (reloaded(w, &w->tanks[hit]) || enemy == -1) {
            enemy = hit;
            enemy_dir = (enum Direction)d;
        }
    }
    if (intel->focus != -1 && intel->focus != enemy) {
        for (int d = UP; d <= RIGHT; d++) {
            if (o->hit[d] == intel->focus) {
                enemy = intel->focus;
                enemy_dir = (enum Direction)d;
                p->by_focus = true;
                break;
            }
        }
    }

    if (enemy != -1) {
        const Tank *e = &w->tanks[enemy];
        sb_printf(ev, "цель %s (%s%s) — ", tank_label(e).s, DIR_NAME[enemy_dir],
                  enemy == intel->focus ? ", фокус координатора" : "");
        bool can_aim = reloaded(w, t);
        enum Direction leave_dirs[2];
        side_directions(enemy_dir, leave_dirs);
        int lengths[2] = {move_length(w, self, leave_dirs[0]), move_length(w, self, leave_dirs[1])};
        bool can_leave = lengths[0] > 0 || lengths[1] > 0;
        /* урон врага / своё HP <= свой урон / HP врага, без деления: все HP > 0. */
        bool prefer_aim = TYPES[e->type].damage * e->hp <= tt->damage * t->hp || !reloaded(w, e);
        if (can_aim && (!can_leave || prefer_aim)) {
            p->action = AIM;
            p->dir = enemy_dir;
            sb_printf(ev, "намерение: прицеливание %s\n", DIR_NAME[enemy_dir]);
            return;
        }
        if (can_leave) {
            int side = choose_side(lengths, rng);
            p->action = MOVE;
            p->dir = leave_dirs[side];
            p->length = lengths[side];
            sb_printf(ev, "намерение: уклонение %s на %d\n", DIR_NAME[p->dir], p->length);
            return;
        }
        sb_puts(ev, "прицел недоступен (перезарядка), уклониться некуда — ");
    } else {
        sb_puts(ev, "противник не виден — ");
    }

    p->action = MOVE;
    int forward = move_length(w, self, t->dir);
    enum Direction sides[2];
    side_directions(t->dir, sides);
    int lengths[2] = {move_length(w, self, sides[0]), move_length(w, self, sides[1])};
    bool can_turn = lengths[0] > 0 || lengths[1] > 0;
    if (can_turn && (forward == 0 || rand01(rng) < TURN_PROBABILITY)) {
        int side = choose_side(lengths, rng);
        p->dir = sides[side];
        p->length = lengths[side];
        sb_printf(ev, "намерение: поворот %s на %d (%s)\n", DIR_NAME[p->dir], p->length,
                  forward == 0 ? "вперёд не пройти" : "случайный поворот");
    } else if (forward > 0) {
        p->length = forward;
        sb_printf(ev, "намерение: движение %s на %d\n", DIR_NAME[p->dir], p->length);
    } else {
        p->dir = opposite(t->dir);
        sb_printf(ev, "намерение: разворот %s (вперёд и в стороны пути нет)\n", DIR_NAME[p->dir]);
    }
}

/* --- Фазы применения (поток арбитра) --- */

static void screen_push(ScreenLog *l, ScreenEvent e)
{
    if (l->count == LOG_LINES) {
        memmove(l->ev, l->ev + 1, sizeof l->ev[0] * (LOG_LINES - 1));
        l->count--;
    }
    l->ev[l->count++] = e;
}

/* Все выстрелы такта. Признак alive не меняется до конца фазы, поэтому
 * уничтожаемый в этом такте танк успевает выстрелить (взаимное уничтожение). */
static void apply_shots(World *w, const Plan plans[], int lethal_by[], Stats *st,
                        ScreenLog *scr, StrBuf *ev)
{
    const int tick = w->frame + 1;
    for (int i = 0; i < w->count; i++)
        lethal_by[i] = -1;
    for (int i = 0; i < w->count; i++) {
        Tank *t = &w->tanks[i];
        if (plans[i].action != SHOOT || !t->alive)
            continue;
        enum RayStop stop;
        int dist;
        int target = cast_ray(w, i, t->dir, &stop, &dist);
        st->tank[i].shots++;
        sb_printf(ev, "[T%d] выстрел: %s %s\n", tick, tank_label(t).s, DIR_NAME[t->dir]);
        if (target != -1 && w->tanks[target].team == t->team) {
            /* Невозможно по построению: позиции с момента решения не менялись. */
            st->violations++;
            sb_printf(ev, "[T%d] НАРУШЕНИЕ ИНВАРИАНТА: выстрел %s пришёлся в союзника %s\n",
                      tick, tank_tag(t).s, tank_tag(&w->tanks[target]).s);
        } else if (target != -1) {
            Tank *v = &w->tanks[target];
            int damage = TYPES[t->type].damage;
            int before = v->hp;
            v->hp = before > damage ? before - damage : 0;
            st->tank[i].hits++;
            st->tank[i].dealt += before - v->hp;
            st->tank[target].taken += before - v->hp;
            if (before > 0 && v->hp == 0)
                lethal_by[target] = i;
            sb_printf(ev, "[T%d] попадание: %s -> %s (дистанция %d)\n", tick, tank_label(t).s,
                      tank_label(v).s, dist);
            sb_printf(ev, "[T%d] повреждение: %s -%d HP (%d -> %d)%s\n", tick, tank_label(v).s,
                      damage, before, v->hp, v->hp == 0 ? ", прочность исчерпана" : "");
            screen_push(scr, (ScreenEvent){SCR_HIT, i, target, damage});
        } else {
            sb_printf(ev, "[T%d] промах: %s — снаряд остановлен %s через %d симв.\n", tick,
                      tank_label(t).s, stop == RAY_WALL ? "препятствием" : "краем поля", dist);
            screen_push(scr, (ScreenEvent){SCR_MISS, i, -1, 0});
        }
        t->last_shot = w->frame;
        t->aimed = false;
    }
}

/* Определение уничтоженных после всей фазы стрельбы. died[i] — погиб сейчас. */
static int apply_deaths(World *w, const int lethal_by[], Stats *st, ScreenLog *scr,
                        StrBuf *ev, bool died[])
{
    const int tick = w->frame + 1;
    int n = 0;
    for (int i = 0; i < w->count; i++) {
        Tank *t = &w->tanks[i];
        died[i] = false;
        if (!t->alive || t->hp > 0)
            continue;
        t->alive = false;
        w->alive[t->team]--;
        died[i] = true;
        n++;
        st->tank[i].death_tick = tick;
        int killer = lethal_by[i];
        if (killer >= 0)
            st->tank[killer].kills++;
        sb_printf(ev, "[T%d] уничтожение: %s (решающий выстрел: %s); в команде %c осталось %d\n",
                  tick, tank_label(t).s, killer >= 0 ? tank_label(&w->tanks[killer]).s : "?",
                  t->team ? 'B' : 'A', w->alive[t->team]);
        screen_push(scr, (ScreenEvent){SCR_DEAD, i, -1, 0});
    }
    return n;
}

/* Кто занимает область, в которую танк self не смог продвинуться. */
static int find_blocker(const World *w, int self, int x, int y)
{
    for (int j = 0; j < w->count; j++) {
        if (j == self || !w->tanks[j].alive)
            continue;
        if (rects_overlap(x, y, w->tanks[j].x, w->tanks[j].y))
            return j;
    }
    return -1;
}

/* Прицеливание и перемещение в очереди по массе. Путь каждого танка
 * пересчитывается по уже обновлённому полю: если более ранний в очереди
 * танк занял область, более поздний проходит только до неё — это и есть
 * однозначное разрешение конфликта клеток. */
static void apply_moves(World *w, const Plan plans[], Stats *st, StrBuf *ev)
{
    const int tick = w->frame + 1;
    for (int i = 0; i < w->count; i++) {
        Tank *t = &w->tanks[i];
        const Plan *p = &plans[i];
        if (!t->alive)
            continue;
        if (p->action == AIM) {
            sb_printf(ev, "[T%d] наведение: %s орудие %s -> %s\n", tick, tank_label(t).s,
                      DIR_NAME[t->dir], DIR_NAME[p->dir]);
            t->aimed = true;
            t->dir = p->dir;
        } else if (p->action == MOVE) {
            enum Direction old_dir = t->dir;
            t->aimed = false;
            t->dir = p->dir;
            int len = imin(move_length(w, i, p->dir), p->length);
            if (len < p->length) {
                st->conflicts++;
                int blk = find_blocker(w, i, t->x + dx(p->dir) * (len + 1), t->y + dy(p->dir) * (len + 1));
                if (blk >= 0) {
                    const Tank *b = &w->tanks[blk];
                    bool heavier = TYPES[b->type].weight > TYPES[t->type].weight;
                    sb_printf(ev,
                              "[T%d] конфликт клеток: %s (масса %d, очередь #%d) претендовал на %d %s, "
                              "область занята %s (масса %d, очередь #%d) — приоритет у %s %s; "
                              "%s сдвинулся на %d, %s удерживает область\n",
                              tick, tank_label(t).s, TYPES[t->type].weight, i + 1, p->length,
                              DIR_NAME[p->dir], tank_label(b).s, TYPES[b->type].weight, blk + 1,
                              tank_tag(b).s, heavier ? "как более тяжёлого" : "по жребию при равной массе",
                              tank_tag(t).s, len, tank_tag(b).s);
                } else {
                    sb_printf(ev, "[T%d] конфликт клеток: %s сдвинулся на %d из %d %s\n", tick,
                              tank_label(t).s, len, p->length, DIR_NAME[p->dir]);
                }
            }
            int ox = t->x, oy = t->y;
            t->x += dx(p->dir) * len;
            t->y += dy(p->dir) * len;
            st->tank[i].moved += len;
            if (len > 0)
                sb_printf(ev, "[T%d] смена позиции: %s (%d,%d) -> (%d,%d) %s на %d\n", tick,
                          tank_label(t).s, ox, oy, t->x, t->y, DIR_NAME[p->dir], len);
            else if (old_dir != t->dir)
                sb_printf(ev, "[T%d] поворот: %s %s -> %s на месте\n", tick, tank_label(t).s,
                          DIR_NAME[old_dir], DIR_NAME[t->dir]);
            else
                sb_printf(ev, "[T%d] позиция: %s остаётся на (%d,%d)\n", tick, tank_label(t).s, t->x, t->y);
        }
    }
}

/* Проверка инвариантов после такта; описание нарушения — в why. */
static bool check_invariants(const World *w, StrBuf *why)
{
    int alive[2] = {0, 0};
    for (int i = 0; i < w->count; i++) {
        const Tank *t = &w->tanks[i];
        if (!t->alive) {
            if (t->hp != 0) {
                sb_printf(why, "уничтоженный %s имеет HP %d", tank_tag(t).s, t->hp);
                return false;
            }
            continue;
        }
        alive[t->team]++;
        if (t->hp <= 0 || t->hp > TYPES[t->type].hp) {
            sb_printf(why, "у живого %s HP %d вне диапазона", tank_tag(t).s, t->hp);
            return false;
        }
        if (!can_place(w, i, t->x, t->y)) {
            sb_printf(why, "%s пересекает край, препятствие или другой танк", tank_tag(t).s);
            return false;
        }
    }
    if (alive[0] != w->alive[0] || alive[1] != w->alive[1]) {
        sb_printf(why, "счётчики живых %d/%d не совпадают с фактическими %d/%d",
                  w->alive[0], w->alive[1], alive[0], alive[1]);
        return false;
    }
    return true;
}

/* ======================================================================
 * 5. ОТОБРАЖЕНИЕ И ТЕКСТОВЫЕ ОТЧЁТЫ
 * ====================================================================== */

typedef struct {
    bool color;
    int max_ticks;
    unsigned seed;
} RenderOpts;

enum { PAL_NONE, PAL_WALL, PAL_A, PAL_B, PAL_FIRE };
static const char *const PALETTE[] = {"\033[0m", "\033[90m", "\033[96m", "\033[91m", "\033[93m"};
static const char *const TEAM_COLOR[2] = {"\033[96m", "\033[91m"};

static void sb_color(StrBuf *b, const RenderOpts *o, const char *code)
{
    if (o->color)
        sb_puts(b, code);
}

static void render_status(StrBuf *b, const World *w, const Tank *t, const RenderOpts *o)
{
    if (!t) {
        sb_spaces(b, 48);
        return;
    }
    char reload[24], line[96];
    if (!t->alive)
        snprintf(reload, sizeof reload, "DEAD");
    else if (reload_left(w, t) == 0)
        snprintf(reload, sizeof reload, "READY");
    else
        snprintf(reload, sizeof reload, "CD: %d", reload_left(w, t));
    snprintf(line, sizeof line, "%c%d %-12s HP: %3d/%3d  %s", t->team ? 'B' : 'A', t->number,
             TYPES[t->type].name, t->hp, TYPES[t->type].hp, reload);
    sb_color(b, o, TEAM_COLOR[t->team]);
    sb_printf(b, "%-48s", line);
    sb_color(b, o, "\033[0m");
}

static void render_screen_event(StrBuf *b, const World *w, const ScreenEvent *e, const RenderOpts *o)
{
    const Tank *a = &w->tanks[e->actor];
    sb_color(b, o, TEAM_COLOR[a->team]);
    sb_puts(b, tank_label(a).s);
    sb_color(b, o, "\033[0m");
    if (e->kind == SCR_DEAD) {
        sb_puts(b, " DEAD");
    } else if (e->kind == SCR_MISS) {
        sb_puts(b, " -> MISS");
    } else {
        const Tank *v = &w->tanks[e->target];
        sb_puts(b, " -> ");
        sb_color(b, o, TEAM_COLOR[v->team]);
        sb_puts(b, tank_label(v).s);
        sb_color(b, o, "\033[0m");
        sb_printf(b, " | -%dhp", e->damage);
    }
}

static void render_panels(StrBuf *b, const World *w, const ScreenLog *log, const RenderOpts *o)
{
    sb_color(b, o, "\033[0m");
    sb_puts(b, "LOG\033[K\r\n");
    for (int i = 0; i < LOG_LINES; i++) {
        if (i < log->count)
            render_screen_event(b, w, &log->ev[i], o);
        sb_color(b, o, "\033[0m");
        sb_puts(b, "\033[K\r\n");
    }
    sb_color(b, o, TEAM_COLOR[0]);
    sb_printf(b, "%-48s", "TEAM A");
    sb_color(b, o, TEAM_COLOR[1]);
    sb_printf(b, "%-48s", "TEAM B");
    sb_color(b, o, "\033[0m");
    sb_puts(b, "\033[K\r\n");
    int rows = w->team_size[0] > w->team_size[1] ? w->team_size[0] : w->team_size[1];
    for (int row = 1; row <= rows; row++) {
        for (int team = 0; team < 2; team++) {
            const Tank *found = NULL;
            for (int i = 0; i < w->count; i++)
                if (w->tanks[i].team == team && w->tanks[i].number == row)
                    found = &w->tanks[i];
            render_status(b, w, found, o);
        }
        sb_puts(b, "\033[K\r\n");
    }
    sb_puts(b, "TYPES | S: symbols/tick, D: damage, HP: initial, CD: ticks, M: mass\033[K\r\n");
    for (int row = 0; row < CELL_H; row++) {
        for (int type = 0; type < TANK_TYPES; type++) {
            for (int col = 0; col < CELL_W; col++)
                sb_putwc(b, ICONS[type][UP][row][col]);
            sb_spaces(b, 16);
        }
        sb_puts(b, "\033[K\r\n");
    }
    for (int type = 0; type < TANK_TYPES; type++)
        sb_printf(b, "%-27s", TYPES[type].name);
    sb_puts(b, "\033[K\r\n");
    for (int type = 0; type < TANK_TYPES; type++) {
        char line[64];
        snprintf(line, sizeof line, "S: %d  D: %d", TYPES[type].speed, TYPES[type].damage);
        sb_printf(b, "%-27s", line);
    }
    sb_puts(b, "\033[K\r\n");
    for (int type = 0; type < TANK_TYPES; type++) {
        char line[64];
        snprintf(line, sizeof line, "HP: %d  CD: %d  M: %d", TYPES[type].hp, TYPES[type].cooldown,
                 TYPES[type].weight);
        sb_printf(b, "%-27s", line);
    }
    sb_puts(b, "\033[K\r\n\033[J");
}

/* Кадр ДЗ-1: before — состояние до выстрелов (линии огня), w — после такта
 * (танки и прицелы). Сначала прицелы, затем выстрелы поверх, затем танки. */
static void render_frame(StrBuf *b, const World *w, const World *before, const Plan plans[],
                         const ScreenLog *log, const RenderOpts *o)
{
    wchar_t pic[MAX_N * CELL_H][MAX_M * CELL_W];
    unsigned char col[MAX_N * CELL_H][MAX_M * CELL_W];
    const int height = w->rows * CELL_H, width = w->cols * CELL_W;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            bool wall = w->wall[y / CELL_H][x / CELL_W];
            pic[y][x] = wall ? L'#' : L' ';
            col[y][x] = wall ? PAL_WALL : PAL_NONE;
        }
    }
    for (int fire = 0; fire <= 1; fire++) {
        const World *u = fire ? before : w;
        for (int i = 0; i < u->count; i++) {
            const Tank *t = &u->tanks[i];
            if (!t->alive || (fire ? plans[i].action != SHOOT : !t->aimed))
                continue;
            int x = t->x + CELL_W / 2, y = t->y + CELL_H / 2;
            for (;;) {
                x += dx(t->dir);
                y += dy(t->dir);
                if (x < 0 || x >= width || y < 0 || y >= height || u->wall[y / CELL_H][x / CELL_W])
                    break;
                bool hit = false;
                for (int j = 0; j < u->count && !hit; j++)
                    hit = j != i && u->tanks[j].alive && contains(&u->tanks[j], x, y);
                if (hit)
                    break;
                if (contains(t, x, y))
                    continue;
                if (fire) {
                    pic[y][x] = dx(t->dir) == 0 ? L'|' : L'-';
                    col[y][x] = PAL_FIRE;
                } else {
                    pic[y][x] = dx(t->dir) == 0 ? L'.' : (wchar_t)0x00B7;
                    col[y][x] = t->team == 0 ? PAL_A : PAL_B;
                }
            }
        }
    }
    for (int i = 0; i < w->count; i++) {
        const Tank *t = &w->tanks[i];
        if (!t->alive)
            continue;
        for (int row = 0; row < CELL_H; row++) {
            for (int c = 0; c < CELL_W; c++) {
                pic[t->y + row][t->x + c] = ICONS[t->type][t->dir][row][c];
                col[t->y + row][t->x + c] = t->team == 0 ? PAL_A : PAL_B;
            }
        }
        if (reloaded(w, t))
            col[t->y + CELL_H / 2 + dy(t->dir) * (CELL_H / 2)][t->x + CELL_W / 2 + dx(t->dir) * (CELL_W / 2)] = PAL_FIRE;
    }
    char limit[16];
    if (o->max_ticks > 0)
        snprintf(limit, sizeof limit, "%d", o->max_ticks);
    else
        snprintf(limit, sizeof limit, "inf");
    sb_puts(b, "\033[H");
    sb_color(b, o, "\033[0m");
    sb_printf(b, "Tick: %d/%s | A: %d | B: %d | seed: %u\033[K\r\n", w->frame, limit, w->alive[0],
              w->alive[1], o->seed);
    sb_put(b, "+", 1);
    for (int x = 0; x < width; x++)
        sb_put(b, "-", 1);
    sb_puts(b, "+\r\n");
    for (int y = 0; y < height; y++) {
        sb_color(b, o, "\033[0m");
        sb_put(b, "|", 1);
        int last = -1;
        for (int x = 0; x < width; x++) {
            if (col[y][x] != last) {
                sb_color(b, o, PALETTE[col[y][x]]);
                last = col[y][x];
            }
            sb_putwc(b, pic[y][x]);
        }
        sb_color(b, o, "\033[0m");
        sb_puts(b, "|\r\n");
    }
    sb_put(b, "+", 1);
    for (int x = 0; x < width; x++)
        sb_put(b, "-", 1);
    sb_put(b, "+", 1);
    sb_color(b, o, "\033[0m");
    sb_puts(b, "\r\n");
    render_panels(b, w, log, o);
}

/* Состояние поля после такта для журнала: карта по клеткам (танк отмечен
 * в клетке своего центра; центры двух непересекающихся танков не могут
 * попасть в одну клетку) и строка на каждую машину. */
static void dump_field_state(StrBuf *b, const World *w, const char *title)
{
    char cell[MAX_N][MAX_M][4];
    for (int r = 0; r < w->rows; r++)
        for (int c = 0; c < w->cols; c++)
            snprintf(cell[r][c], sizeof cell[r][c], "%s", w->wall[r][c] ? "##" : " .");
    for (int i = 0; i < w->count; i++) {
        const Tank *t = &w->tanks[i];
        if (t->alive)
            snprintf(cell[(t->y + CELL_H / 2) / CELL_H][(t->x + CELL_W / 2) / CELL_W], 4, "%c%d",
                     t->team ? 'B' : 'A', t->number);
    }
    sb_printf(b, "[T%d] %s: в строю A %d/%d, B %d/%d\n", w->frame, title, w->alive[0],
              w->team_size[0], w->alive[1], w->team_size[1]);
    sb_puts(b, "      ");
    for (int c = 0; c < w->cols; c++)
        sb_printf(b, "%3d", c);
    sb_puts(b, "\n");
    for (int r = 0; r < w->rows; r++) {
        sb_printf(b, "   %3d", r);
        for (int c = 0; c < w->cols; c++)
            sb_printf(b, " %s", cell[r][c]);
        sb_puts(b, "\n");
    }
    for (int team = 0; team < 2; team++) {
        for (int num = 1; num <= w->team_size[team]; num++) {
            for (int i = 0; i < w->count; i++) {
                const Tank *t = &w->tanks[i];
                if (t->team != team || t->number != num)
                    continue;
                if (!t->alive) {
                    sb_printf(b, "    %-12s уничтожен\n", tank_label(t).s);
                } else {
                    char gun[48];
                    if (reload_left(w, t) == 0)
                        snprintf(gun, sizeof gun, "готово");
                    else
                        snprintf(gun, sizeof gun, "перезарядка %d", reload_left(w, t));
                    sb_printf(b, "    %-12s (%3d,%3d) %-12s HP %3d/%3d  орудие: %s%s\n", tank_label(t).s,
                              t->x, t->y, DIR_NAME[t->dir], t->hp, TYPES[t->type].hp, gun,
                              t->aimed ? ", прицелился" : "");
                }
            }
        }
    }
}

/* Кратчайшая запись double, которая читается обратно в то же значение. */
static void fmt_double(char *out, size_t cap, double v)
{
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(out, cap, "%.*g", prec, v);
        if (strtod(out, NULL) == v)
            return;
    }
}

static void fmt_team_spec(char *out, size_t cap, const TeamSpec *ts)
{
    if (ts->random)
        snprintf(out, cap, "%d", ts->count);
    else
        snprintf(out, cap, "%d %d %d %d", ts->comp[0], ts->comp[1], ts->comp[2], ts->comp[3]);
}

static void write_header(StrBuf *b, const Settings *cfg, const World *w)
{
    char prob[32], ta[32], tb[32];
    fmt_double(prob, sizeof prob, cfg->prob);
    fmt_team_spec(ta, sizeof ta, &cfg->team[0]);
    fmt_team_spec(tb, sizeof tb, &cfg->team[1]);
    sb_puts(b, "# tanks_cond — имитация танкового боя (ИДЗ-2, вариант 24), синхронизация: мьютексы + условные переменные + барьер\n");
    sb_printf(b, "# seed=%u rows=%d cols=%d prob=%s max_ticks=%d delay_ms=%d focus=%s\n", cfg->seed,
              cfg->rows, cfg->cols, prob, cfg->max_ticks, cfg->delay_ms, cfg->focus ? "on" : "off");
    sb_printf(b, "# повтор: tanks_cond --batch --seed %u --rows %d --cols %d --prob %s --team-a \"%s\" "
              "--team-b \"%s\" --max-ticks %d%s\n", cfg->seed, cfg->rows, cfg->cols, prob, ta, tb,
              cfg->max_ticks, cfg->focus ? "" : " --no-focus");
    for (int team = 0; team < 2; team++) {
        const TeamSpec *ts = &cfg->team[team];
        sb_printf(b, "# команда %c: %s, %d танк(ов):", team ? 'B' : 'A',
                  ts->random ? "случайные типы" : "ручной состав", w->team_size[team]);
        for (int num = 1; num <= w->team_size[team]; num++)
            for (int i = 0; i < w->count; i++)
                if (w->tanks[i].team == team && w->tanks[i].number == num)
                    sb_printf(b, " %s", tank_label(&w->tanks[i]).s);
        sb_puts(b, "\n");
    }
    sb_puts(b, "# препятствия [строка,столбец]:");
    int walls = 0;
    for (int r = 0; r < w->rows; r++)
        for (int c = 0; c < w->cols; c++)
            if (w->wall[r][c]) {
                sb_printf(b, " [%d,%d]", r, c);
                walls++;
            }
    sb_puts(b, walls ? "\n" : " нет\n");
    sb_puts(b, "# очередь применения перемещений (масса по убыванию, при равной — жребий):");
    for (int i = 0; i < w->count; i++)
        sb_printf(b, " #%d %s(%d)", i + 1, tank_tag(&w->tanks[i]).s, TYPES[w->tanks[i].type].weight);
    sb_printf(b, "\n# потоки: танков %d, координаторов 2, арбитр 1\n", w->count);
}

enum EndReason { END_NONE, END_DESTROYED, END_LIMIT, END_SIGNAL, END_IO, END_INTERNAL };

static const char *signal_name(int sig)
{
    return sig == SIGINT ? "SIGINT" : sig == SIGTERM ? "SIGTERM" : "сигнал";
}

static void write_summary(StrBuf *b, const World *w, const Stats *st, const Settings *cfg,
                          enum EndReason reason, int sig)
{
    static const char *const KEY[] = {"none", "normal", "limit", "signal", "io", "internal"};
    sb_puts(b, "\n=== ИТОГИ БОЯ ===\n");
    sb_printf(b, "Причина завершения: %s — ", KEY[reason]);
    switch (reason) {
    case END_DESTROYED:
        if (w->alive[0] == 0 && w->alive[1] == 0)
            sb_puts(b, "обе команды уничтожены\nРезультат: ничья\n");
        else
            sb_printf(b, "команда %c уничтожена\nРезультат: победа команды %c\n",
                      w->alive[0] == 0 ? 'A' : 'B', w->alive[0] == 0 ? 'B' : 'A');
        break;
    case END_LIMIT: {
        int hp[2] = {0, 0};
        for (int i = 0; i < w->count; i++)
            hp[w->tanks[i].team] += w->tanks[i].hp;
        sb_printf(b, "достигнут лимит тактов (%d)\nРезультат: лимит, победителя нет; в строю A %d, B %d; "
                  "суммарное HP A %d, B %d\n", cfg->max_ticks, w->alive[0], w->alive[1], hp[0], hp[1]);
        break;
    }
    case END_SIGNAL:
        sb_printf(b, "бой прерван сигналом %s\nРезультат: прерван; в строю A %d, B %d\n",
                  signal_name(sig), w->alive[0], w->alive[1]);
        break;
    case END_IO:
        sb_puts(b, "ошибка вывода на экран\nРезультат: бой остановлен\n");
        break;
    case END_INTERNAL:
        sb_puts(b, "нарушен инвариант модели (см. журнал)\nРезультат: бой остановлен\n");
        break;
    case END_NONE:
        sb_puts(b, "бой не проводился\n");
        break;
    }
    char limit[24];
    if (cfg->max_ticks > 0)
        snprintf(limit, sizeof limit, "%d", cfg->max_ticks);
    else
        snprintf(limit, sizeof limit, "нет");
    sb_printf(b, "Сыграно тактов: %d (лимит: %s); seed: %u\n", w->frame, limit, cfg->seed);
    sb_printf(b, "Разрешено конфликтов перемещения: %d\n", st->conflicts);
    sb_printf(b, "Решений, изменённых фокусом координатора: A %d, B %d%s\n", st->focus_decisions[0],
              st->focus_decisions[1], cfg->focus ? "" : " (фокус отключён)");

    static const char *const HEAD[] = {"Танк", "Итог", "HP", "Нанёс", "Получил", "Выстр.",
                                       "Попад.", "Уничт.", "Тактов", "Путь"};
    static const int WIDTH[] = {13, 10, 8, 7, 8, 7, 7, 7, 7, 6};
    for (int k = 0; k < 10; k++)
        sb_cell(b, HEAD[k], WIDTH[k], k >= 2);
    sb_puts(b, "\n");
    for (int team = 0; team < 2; team++) {
        int alive = 0, dealt = 0, taken = 0, shots = 0, hits = 0, kills = 0, moved = 0;
        for (int num = 1; num <= w->team_size[team]; num++) {
            for (int i = 0; i < w->count; i++) {
                const Tank *t = &w->tanks[i];
                const TankStats *s = &st->tank[i];
                if (t->team != team || t->number != num)
                    continue;
                char v[10][32];
                snprintf(v[0], sizeof v[0], "%s", tank_label(t).s);
                snprintf(v[1], sizeof v[1], "%s", t->alive ? "в строю" : "уничтожен");
                snprintf(v[2], sizeof v[2], "%d/%d", t->hp, TYPES[t->type].hp);
                snprintf(v[3], sizeof v[3], "%d", s->dealt);
                snprintf(v[4], sizeof v[4], "%d", s->taken);
                snprintf(v[5], sizeof v[5], "%d", s->shots);
                snprintf(v[6], sizeof v[6], "%d", s->hits);
                snprintf(v[7], sizeof v[7], "%d", s->kills);
                snprintf(v[8], sizeof v[8], "%d", t->alive ? w->frame : s->death_tick);
                snprintf(v[9], sizeof v[9], "%d", s->moved);
                for (int k = 0; k < 10; k++)
                    sb_cell(b, v[k], WIDTH[k], k >= 2);
                sb_puts(b, "\n");
                alive += t->alive;
                dealt += s->dealt;
                taken += s->taken;
                shots += s->shots;
                hits += s->hits;
                kills += s->kills;
                moved += s->moved;
            }
        }
        sb_printf(b, "Команда %c: в строю %d/%d; урон нанесён %d, получен %d; выстрелов %d, попаданий %d (%d%%); "
                  "уничтожено противников %d; пройдено %d симв.\n", team ? 'B' : 'A', alive,
                  w->team_size[team], dealt, taken, shots, hits, shots ? hits * 100 / shots : 0, kills, moved);
    }
}

/* ======================================================================
 * 6. НАСТРОЙКИ: аргументы, конфиг-файл, интерактивный ввод
 * ====================================================================== */

static void settings_init(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->delay_ms = DEFAULT_DELAY_MS;
    s->max_ticks = DEFAULT_MAX_TICKS;
    s->color = true;
    s->focus = true;
    s->rows = DEFAULT_ROWS;
    s->cols = DEFAULT_COLS;
    s->prob = DEFAULT_PROB;
    s->inject_sig = SIGINT;
}

static void settings_free(Settings *s)
{
    free(s->log_path);
    s->log_path = NULL;
}

static bool parse_long(const char *s, long lo, long hi, long *out)
{
    if (!s || !*s)
        return false;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || *end != '\0' || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

static bool parse_seed(const char *s, unsigned *out)
{
    if (!s || !*s)
        return false;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v > UINT_MAX)
        return false;
    *out = (unsigned)v;
    return true;
}

/* Вероятность 0..1, десятичная точка (локаль не устанавливается — всегда "C"). */
static bool parse_prob(const char *s, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || errno == ERANGE || end[strspn(end, " \t\r\n")] != '\0' || !(v >= 0.0 && v <= 1.0))
        return false;
    *out = v;
    return true;
}

/* "N" — N случайных танков; "a b c d" — количества по типам. */
static bool parse_team_spec(const char *s, TeamSpec *ts)
{
    long v[TANK_TYPES];
    int n = 0;
    const char *p = s;
    for (;;) {
        p += strspn(p, " \t,");
        if (!*p)
            break;
        if (n == TANK_TYPES)
            return false;
        char *end;
        errno = 0;
        long x = strtol(p, &end, 10);
        if (end == p || errno == ERANGE || x < 0 || x > MAX_TEAM_TANKS)
            return false;
        if (*end && !strchr(" \t,", *end))
            return false;
        v[n++] = x;
        p = end;
    }
    TeamSpec r = {0};
    if (n == 1) {
        r.random = true;
        r.count = (int)v[0];
    } else if (n == TANK_TYPES) {
        for (int i = 0; i < TANK_TYPES; i++)
            r.comp[i] = (int)v[i];
    } else {
        return false;
    }
    r.set = true;
    *ts = r;
    return true;
}

static bool parse_bool(const char *s, bool *out)
{
    static const char *const YES[] = {"1", "yes", "on", "true"};
    static const char *const NO[] = {"0", "no", "off", "false"};
    for (int i = 0; i < 4; i++) {
        if (strcmp(s, YES[i]) == 0) {
            *out = true;
            return true;
        }
        if (strcmp(s, NO[i]) == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

static bool parse_inject(const char *s, Settings *cfg)
{
    char buf[64];
    if (strlen(s) >= sizeof buf)
        return false;
    snprintf(buf, sizeof buf, "%s", s);
    char *phase = strchr(buf, ':');
    if (!phase)
        return false;
    *phase++ = '\0';
    char *sig = strchr(phase, ':');
    if (sig)
        *sig++ = '\0';
    long tick;
    if (!parse_long(buf, 1, MAX_TICKS_LIMIT, &tick))
        return false;
    int ph = -1;
    for (int i = 0; i < PH_COUNT; i++)
        if (strcmp(phase, PHASE_KEY[i]) == 0)
            ph = i;
    if (ph < 0)
        return false;
    int signo = SIGINT;
    if (sig) {
        if (strcmp(sig, "INT") == 0)
            signo = SIGINT;
        else if (strcmp(sig, "TERM") == 0)
            signo = SIGTERM;
        else
            return false;
    }
    cfg->inject_tick = (int)tick;
    cfg->inject_phase = (enum Phase)ph;
    cfg->inject_sig = signo;
    return true;
}

/* Применение одного параметра (из командной строки или конфиг-файла).
 * name — без "--", через дефис; where — источник для сообщения об ошибке. */
static bool apply_option(Settings *s, const char *name, const char *value, const char *where)
{
    long v;
    bool flag;
    if (strcmp(name, "seed") == 0) {
        if (parse_seed(value, &s->seed)) {
            s->seed_set = true;
            return true;
        }
        err_printf("%s: seed — целое 0..%u, получено '%s'\n", where, UINT_MAX, value);
    } else if (strcmp(name, "delay") == 0) {
        if (parse_long(value, 0, MAX_DELAY_MS, &v)) {
            s->delay_ms = (int)v;
            return true;
        }
        err_printf("%s: delay — целое 0..%d (мс), получено '%s'\n", where, MAX_DELAY_MS, value);
    } else if (strcmp(name, "max-ticks") == 0) {
        if (parse_long(value, 0, MAX_TICKS_LIMIT, &v)) {
            s->max_ticks = (int)v;
            return true;
        }
        err_printf("%s: max-ticks — целое 0..%d (0 — без лимита), получено '%s'\n", where,
                   MAX_TICKS_LIMIT, value);
    } else if (strcmp(name, "log") == 0) {
        if (*value) {
            char *copy = strdup(value);
            if (!copy) {
                err_printf("%s: нехватка памяти\n", where);
                return false;
            }
            free(s->log_path);
            s->log_path = copy;
            return true;
        }
        err_printf("%s: log — нужен путь к файлу\n", where);
    } else if (strcmp(name, "rows") == 0) {
        if (parse_long(value, 3, MAX_N, &v)) {
            s->rows = (int)v;
            s->field_given = true;
            return true;
        }
        err_printf("%s: rows — целое 3..%d, получено '%s'\n", where, MAX_N, value);
    } else if (strcmp(name, "cols") == 0) {
        if (parse_long(value, 1, MAX_M, &v)) {
            s->cols = (int)v;
            s->field_given = true;
            return true;
        }
        err_printf("%s: cols — целое 1..%d, получено '%s'\n", where, MAX_M, value);
    } else if (strcmp(name, "prob") == 0) {
        if (parse_prob(value, &s->prob)) {
            s->field_given = true;
            return true;
        }
        err_printf("%s: prob — число 0..1 с точкой, получено '%s'\n", where, value);
    } else if (strcmp(name, "team-a") == 0 || strcmp(name, "team-b") == 0) {
        int team = name[5] == 'b';
        if (parse_team_spec(value, &s->team[team])) {
            s->field_given = true;
            return true;
        }
        err_printf("%s: %s — \"N\" (случайный состав) или \"a b c d\" (по типам), числа 0..%d, "
                   "получено '%s'\n", where, name, MAX_TEAM_TANKS, value);
    } else if (strcmp(name, "test-signal") == 0) {
        if (parse_inject(value, s))
            return true;
        err_printf("%s: test-signal — ТАКТ:ФАЗА[:INT|TERM], фазы: observe coordinate decide shoot "
                   "destroy move render delay; получено '%s'\n", where, value);
    } else if (strcmp(name, "color") == 0 || strcmp(name, "plain") == 0 || strcmp(name, "ansi") == 0 ||
               strcmp(name, "batch") == 0 || strcmp(name, "focus") == 0) {
        if (parse_bool(value, &flag)) {
            if (name[0] == 'c')
                s->color = flag;
            else if (name[0] == 'p')
                s->plain = flag;
            else if (name[0] == 'a')
                s->ansi = flag;
            else if (name[0] == 'b')
                s->batch = flag;
            else
                s->focus = flag;
            return true;
        }
        err_printf("%s: %s — 1/0, yes/no, on/off, true/false; получено '%s'\n", where, name, value);
    } else {
        err_printf("%s: неизвестный параметр '%s' (см. --help)\n", where, name);
    }
    return false;
}

static char *trim(char *s)
{
    s += strspn(s, " \t\r");
    size_t n = strlen(s);
    while (n > 0 && strchr(" \t\r", s[n - 1]))
        s[--n] = '\0';
    return s;
}

/* Конфиг-файл: строки key=value, '#' — комментарий; ключи как у длинных
 * опций (подчёркивание равно дефису). Читается через open/read/close. */
static bool load_config(const char *path, Settings *s)
{
    enum { CONFIG_MAX = 64 * 1024 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err_printf("tanks_cond: не удалось открыть конфиг '%s': %s\n", path, strerror(errno));
        return false;
    }
    char *buf = malloc(CONFIG_MAX + 1);
    if (!buf) {
        close(fd);
        err_printf("tanks_cond: нехватка памяти для конфига\n");
        return false;
    }
    size_t len = 0;
    bool ok = true;
    for (;;) {
        if (len == CONFIG_MAX) {
            err_printf("tanks_cond: конфиг '%s' больше %d байт\n", path, CONFIG_MAX);
            ok = false;
            break;
        }
        ssize_t n = read(fd, buf + len, CONFIG_MAX - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            err_printf("tanks_cond: ошибка чтения '%s': %s\n", path, strerror(errno));
            ok = false;
            break;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    char *line = buf;
    for (int lineno = 1; ok && line && *line; lineno++) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        char *text = trim(line);
        line = next;
        if (!*text || *text == '#')
            continue;
        char where[PATH_MAX + 32];
        snprintf(where, sizeof where, "%s:%d", path, lineno);
        char *eq = strchr(text, '=');
        if (!eq) {
            err_printf("%s: ожидается key=value\n", where);
            ok = false;
            break;
        }
        *eq = '\0';
        char *key = trim(text), *value = trim(eq + 1);
        size_t vl = strlen(value);
        if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') {
            value[vl - 1] = '\0';
            value++;
        }
        for (char *k = key; *k; k++)
            if (*k == '_')
                *k = '-';
        if (strcmp(key, "config") == 0) {
            err_printf("%s: вложенный config не поддерживается\n", where);
            ok = false;
        } else {
            ok = apply_option(s, key, value, where);
        }
    }
    free(buf);
    return ok;
}

enum ParseResult { PARSE_OK, PARSE_ERROR, PARSE_HELP };

/* Флаги без значения и их эквиваленты key=value. */
static const struct {
    const char *flag, *name, *value;
} FLAGS[] = {
    {"no-color", "color", "0"}, {"plain", "plain", "1"},    {"ansi", "ansi", "1"},
    {"batch", "batch", "1"},    {"defaults", "batch", "1"}, {"no-focus", "focus", "0"},
};

/* Порядок применения: значения по умолчанию < конфиг-файл < командная строка. */
static enum ParseResult parse_cli(int argc, char **argv, Settings *s)
{
    const char *config = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return PARSE_HELP;
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                err_printf("tanks_cond: --config: требуется путь к файлу\n");
                return PARSE_ERROR;
            }
            config = argv[++i];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config = argv[i] + 9;
        }
    }
    if (config && !load_config(config, s))
        return PARSE_ERROR;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) {
            err_printf("tanks_cond: неожиданный аргумент '%s' (см. --help)\n", arg);
            return PARSE_ERROR;
        }
        char name[32];
        const char *eq = strchr(arg + 2, '=');
        size_t nlen = eq ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);
        if (nlen == 0 || nlen >= sizeof name) {
            err_printf("tanks_cond: неизвестный параметр '%s' (см. --help)\n", arg);
            return PARSE_ERROR;
        }
        memcpy(name, arg + 2, nlen);
        name[nlen] = '\0';
        bool handled = false;
        for (size_t f = 0; f < sizeof FLAGS / sizeof FLAGS[0]; f++) {
            if (strcmp(name, FLAGS[f].flag) != 0)
                continue;
            if (eq) {
                err_printf("tanks_cond: --%s не принимает значение\n", name);
                return PARSE_ERROR;
            }
            if (!apply_option(s, FLAGS[f].name, FLAGS[f].value, arg))
                return PARSE_ERROR;
            handled = true;
        }
        if (handled)
            continue;
        const char *value = eq ? eq + 1 : NULL;
        if (!value) {
            if (i + 1 >= argc) {
                err_printf("tanks_cond: --%s: требуется значение\n", name);
                return PARSE_ERROR;
            }
            value = argv[++i];
        }
        if (strcmp(name, "config") == 0)
            continue;
        char where[48];
        snprintf(where, sizeof where, "--%s", name);
        if (!apply_option(s, name, value, where))
            return PARSE_ERROR;
    }
    return PARSE_OK;
}

/* Составы по умолчанию и проверка вместимости стартовой строки. */
static bool finalize_teams(Settings *s)
{
    int limit = (s->cols + 1) / 2;
    for (int team = 0; team < 2; team++) {
        TeamSpec *ts = &s->team[team];
        if (!ts->set) {
            ts->random = true;
            ts->count = imin(DEFAULT_TEAM_SIZE, limit);
        }
        int size = team_size_of(ts);
        if (size < 1 || size > limit) {
            err_printf("tanks_cond: команда %c: нужно от 1 до %d танков при ширине поля %d (задано %d)\n",
                       team ? 'B' : 'A', limit, s->cols, size);
            return false;
        }
    }
    return true;
}

static void print_help(void)
{
    out_printf(
        "Использование: tanks_cond [опции]\n"
        "Многопоточная имитация танкового боя (ИДЗ-2, вариант 24): потоки танков, координаторов\n"
        "команд и арбитра поля; синхронизация — мьютексы, условные переменные и барьер.\n"
        "Без параметров боя — интерактивная настройка, как в ДЗ-1 (Enter — значение по умолчанию).\n\n"
        "Параметры боя (любой из них отключает вопросы, остальное — по умолчанию):\n"
        "  --rows R            строк поля, 3..%d (по умолчанию %d)\n"
        "  --cols C            столбцов поля, 1..%d (%d)\n"
        "  --prob P            вероятность препятствия в столбце, 0..1 (%.1f)\n"
        "  --team-a SPEC       состав A: \"N\" — N танков случайных типов,\n"
        "                      \"a b c d\" — количества mouse fv215b183 t62a leopard_1\n"
        "  --team-b SPEC       состав B (по умолчанию %d случайных, на узком поле меньше)\n"
        "  --config FILE       файл key=value с теми же ключами (rows=6, team_a=1 1 1 1, seed=7, ...)\n"
        "  --batch, --defaults не задавать вопросов\n\n"
        "Ход боя:\n"
        "  --seed N            зерно генератора 0..%u (по умолчанию — текущее время)\n"
        "  --delay MS          задержка между тактами, 0..%d мс (%d)\n"
        "  --max-ticks N       лимит тактов, 0 — без лимита, до уничтожения или Ctrl+C (%d)\n"
        "  --no-focus          координаторы не назначают фокус огня (выбор цели строго как в ДЗ-1)\n\n"
        "Вывод:\n"
        "  --log FILE          полный журнал событий и итоги (%s)\n"
        "  --plain             без ANSI: журнал построчно в stdout (сам включается, если stdout не терминал)\n"
        "  --ansi              ANSI-перерисовка, даже если stdout не терминал\n"
        "  --no-color          ANSI-перерисовка без цветов\n\n"
        "Тестирование:\n"
        "  --test-signal T:PHASE[:INT|TERM]  на такте T в фазе PHASE послать себе сигнал;\n"
        "                      PHASE: observe coordinate decide shoot destroy move render delay\n"
        "  --help              эта справка\n\n"
        "Коды выхода: 0 — бой завершён (уничтожение команды или лимит); 1 — неверные параметры;\n"
        "  2 — системная ошибка (журнал, потоки, вывод); 3 — ввод закончился при настройке, бой не начат;\n"
        "  130 — прерван SIGINT; 143 — прерван SIGTERM.\n",
        MAX_N, DEFAULT_ROWS, MAX_M, DEFAULT_COLS, DEFAULT_PROB, DEFAULT_TEAM_SIZE, UINT_MAX,
        MAX_DELAY_MS, DEFAULT_DELAY_MS, DEFAULT_MAX_TICKS, DEFAULT_LOG_PATH);
}

/* --- Интерактивная настройка через read(2), диалог ДЗ-1 --- */

/* Флаг сигнала (раздел 7). Объявлен здесь: чтение stdin должно прерываться
 * по Ctrl+C, а read(2) без SA_RESTART возвращает EINTR. */
static volatile sig_atomic_t g_signal = 0;

enum InStatus { IN_OK, IN_EOF, IN_SIGNAL, IN_ERROR };
#define LINE_CAP 128

typedef struct {
    char buf[4096];
    size_t pos, len;
    bool eof;
    sigset_t wait_mask;      /* маска на время ожидания ввода: SIGINT/SIGTERM открыты */
} InReader;

/* Строка без '\n'. Строка длиннее LINE_CAP-2 символов отбрасывается целиком
 * (как fgets с буфером 128 в ДЗ-1). Неполная последняя строка перед EOF
 * возвращается как обычная.
 * SIGINT/SIGTERM в main заблокированы и открываются только атомарно внутри
 * ppoll: сигнал не может проскочить между проверкой флага и блокирующим
 * чтением, поэтому Ctrl+C при настройке срабатывает всегда. ppoll, а не
 * POSIX-аналог pselect: ThreadSanitizer не перехватывает pselect, и под ним
 * обработчик откладывался до конца ввода (проверено тестом inter_sigint). */
static enum InStatus read_raw_line(InReader *in, char line[LINE_CAP], bool *too_long)
{
    size_t n = 0;
    bool any = false;
    *too_long = false;
    for (;;) {
        while (in->pos < in->len) {
            char ch = in->buf[in->pos++];
            any = true;
            if (ch == '\n') {
                line[n] = '\0';
                return IN_OK;
            }
            if (n < LINE_CAP - 2)
                line[n++] = ch;
            else
                *too_long = true;
        }
        in->pos = in->len = 0;
        if (in->eof) {
            line[n] = '\0';
            return any ? IN_OK : IN_EOF;
        }
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        if (ppoll(&pfd, 1, NULL, &in->wait_mask) < 0) {
            if (errno != EINTR)
                return IN_ERROR;
            if (g_signal)
                return IN_SIGNAL;
            continue;
        }
        ssize_t r = read(STDIN_FILENO, in->buf, sizeof in->buf);
        if (r > 0)
            in->len = (size_t)r;
        else if (r == 0)
            in->eof = true;
        else if (errno != EINTR && errno != EAGAIN)
            return IN_ERROR;
    }
}

/* Пустая строка (только пробелы) возвращается как "" — значение по умолчанию. */
static enum InStatus read_line(InReader *in, const char *prompt, char line[LINE_CAP])
{
    for (;;) {
        if (out_printf("%s", prompt) < 0)
            return IN_ERROR;
        bool too_long;
        enum InStatus st = read_raw_line(in, line, &too_long);
        if (st != IN_OK)
            return st;
        if (too_long) {
            if (out_printf("Слишком длинная строка. Повторите ввод.\n") < 0)
                return IN_ERROR;
            continue;
        }
        if (line[strspn(line, " \t\r\n")] == '\0')
            line[0] = '\0';
        return IN_OK;
    }
}

static enum InStatus read_ints(InReader *in, const char *prompt, int values[], int count)
{
    char line[LINE_CAP];
    for (;;) {
        enum InStatus st = read_line(in, prompt, line);
        if (st != IN_OK)
            return st;
        if (!line[0])
            return IN_OK;
        int parsed[TANK_TYPES];
        char *cursor = line;
        bool valid = true;
        for (int i = 0; i < count && valid; i++) {
            cursor += strspn(cursor, " \t\r\n");
            char *end;
            errno = 0;
            long value = strtol(cursor, &end, 10);
            if (end == cursor || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
                valid = false;
                break;
            }
            parsed[i] = (int)value;
            cursor = end;
            if (*cursor && !strchr(" \t\r\n", *cursor))
                valid = false;
        }
        if (valid && cursor[strspn(cursor, " \t\r\n")] == '\0') {
            for (int i = 0; i < count; i++)
                values[i] = parsed[i];
            return IN_OK;
        }
        if (out_printf("Введите ровно %d целых чисел через пробел.\n", count) < 0)
            return IN_ERROR;
    }
}

#define IN_TRY(expr)                        \
    do {                                    \
        enum InStatus st_ = (expr);         \
        if (st_ != IN_OK)                   \
            return st_;                     \
    } while (0)
#define OUT_TRY(expr)                       \
    do {                                    \
        if ((expr) < 0)                     \
            return IN_ERROR;                \
    } while (0)

static enum InStatus interactive_setup(InReader *in, Settings *s)
{
    OUT_TRY(out_printf("Enter - оставить значение по умолчанию.\n"));
    int size[2] = {DEFAULT_ROWS, DEFAULT_COLS};
    for (;;) {
        IN_TRY(read_ints(in, "Размер поля, строки столбцы [10 10]: ", size, 2));
        if (size[0] >= 3 && size[0] <= MAX_N && size[1] >= 1 && size[1] <= MAX_M)
            break;
        OUT_TRY(out_printf("Строки: 3..%d, столбцы: 1..%d.\n", MAX_N, MAX_M));
        size[0] = DEFAULT_ROWS;
        size[1] = DEFAULT_COLS;
    }
    s->rows = size[0];
    s->cols = size[1];

    char line[LINE_CAP];
    for (;;) {
        IN_TRY(read_line(in, "Вероятность препятствия в столбце [0.4]: ", line));
        if (!line[0] || parse_prob(line, &s->prob))
            break;
        OUT_TRY(out_printf("Введите число от 0 до 1, дробную часть отделяйте точкой.\n"));
    }

    int mode = 2;
    for (;;) {
        IN_TRY(read_ints(in, "Состав: 1 — вручную, 2 — случайный [2]: ", &mode, 1));
        if (mode == 1 || mode == 2)
            break;
        OUT_TRY(out_printf("Введите 1 или 2.\n"));
        mode = 2;
    }
    int limit = (s->cols + 1) / 2;
    int default_count = imin(DEFAULT_TEAM_SIZE, limit);
    OUT_TRY(out_printf("В каждой команде от 1 до %d танков (расстановка через клетку).\n", limit));
    for (int team = 0; team < 2; team++) {
        TeamSpec *ts = &s->team[team];
        char prompt[160];
        memset(ts, 0, sizeof *ts);
        ts->set = true;
        if (mode == 2) {
            ts->random = true;
            ts->count = default_count;
            snprintf(prompt, sizeof prompt, "Количество танков команды %c [%d]: ", team ? 'B' : 'A',
                     default_count);
            for (;;) {
                IN_TRY(read_ints(in, prompt, &ts->count, 1));
                if (ts->count >= 1 && ts->count <= limit)
                    break;
                OUT_TRY(out_printf("Команда должна содержать от 1 до %d танков.\n", limit));
                ts->count = default_count;
            }
        } else {
            for (int i = 0; i < default_count; i++)
                ts->comp[i] = 1;
            snprintf(prompt, sizeof prompt, "Команда %c, mouse fv215b183 t62a leopard_1 [%d %d %d %d]: ",
                     team ? 'B' : 'A', ts->comp[0], ts->comp[1], ts->comp[2], ts->comp[3]);
            for (;;) {
                int values[TANK_TYPES];
                memcpy(values, ts->comp, sizeof values);
                IN_TRY(read_ints(in, prompt, values, TANK_TYPES));
                int sum = 0;
                bool valid = true;
                for (int i = 0; i < TANK_TYPES; i++) {
                    if (values[i] < 0 || values[i] > limit) {
                        valid = false;
                        break;
                    }
                    sum += values[i];
                }
                if (valid && sum >= 1 && sum <= limit) {
                    memcpy(ts->comp, values, sizeof values);
                    break;
                }
                OUT_TRY(out_printf("Количество каждого типа неотрицательно, сумма — от 1 до %d.\n", limit));
            }
        }
    }
    return IN_OK;
}

/* ======================================================================
 * 7. СЛОЙ СИНХРОНИЗАЦИИ: мьютексы + условные переменные + барьер
 * ====================================================================== */

/* Обработчик SIGINT/SIGTERM только запоминает первый сигнал. Разбудить
 * отсюда арбитра нечем: pthread_cond_signal и pthread_mutex_lock не входят в
 * список async-signal-safe функций (POSIX, 2.4.3) — обработчик, прервавший
 * поток посреди операции с мьютексом, мог бы его повесить. Поэтому досрочный
 * выход из задержки сделан на маске сигналов (interruptible_delay), а в
 * остальных ожиданиях флаг проверяется на границе фазы.
 * Сигналы открыты только в двух местах: в main внутри ppoll при настройке и
 * в потоке арбитра во время боя. Поэтому флаг читает тот же поток, в котором
 * выполнился обработчик, — случай, для которого volatile sig_atomic_t
 * достаточно. Остальные потоки узнают об остановке от арбитра. */
static void on_signal(int sig)
{
    if (g_signal == 0)
        g_signal = sig;
}

/* Ошибка примитива синхронизации означает сломанный протокол (дальше — риск
 * дедлока), продолжать нельзя: сообщение, восстановление терминала, abort. */
static _Noreturn void sync_fatal(const char *what, int err)
{
    err_printf("tanks_cond: внутренняя ошибка синхронизации: %s: %s\n", what, strerror(err));
    if (isatty(STDOUT_FILENO))
        (void)write_all(STDOUT_FILENO, "\033[0m\033[?25h\n", 11);
    abort();
}

static void mutex_lock(pthread_mutex_t *m)
{
    int rc = pthread_mutex_lock(m);
    if (rc != 0)
        sync_fatal("pthread_mutex_lock", rc);
}

static void mutex_unlock(pthread_mutex_t *m)
{
    int rc = pthread_mutex_unlock(m);
    if (rc != 0)
        sync_fatal("pthread_mutex_unlock", rc);
}

/* Ни одна из функций ниже по POSIX не возвращает EINTR: прерванное сигналом
 * ожидание продолжается само, а флаг сигнала арбитр проверяет между фазами. */
static void cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    int rc = pthread_cond_wait(c, m);
    if (rc != 0)
        sync_fatal("pthread_cond_wait", rc);
}

static void cond_signal(pthread_cond_t *c)
{
    int rc = pthread_cond_signal(c);
    if (rc != 0)
        sync_fatal("pthread_cond_signal", rc);
}

static void cond_broadcast(pthread_cond_t *c)
{
    int rc = pthread_cond_broadcast(c);
    if (rc != 0)
        sync_fatal("pthread_cond_broadcast", rc);
}

/* PTHREAD_BARRIER_SERIAL_THREAD получает один из участников раунда — не ошибка. */
static void barrier_wait(pthread_barrier_t *b)
{
    int rc = pthread_barrier_wait(b);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD)
        sync_fatal("pthread_barrier_wait", rc);
}

/* --- Журнал событий ---
 * Потоки танков и координаторов пишут параллельно, поэтому приём записей
 * защищён мьютексом. Порядок приёма зависит от планировщика, поэтому перед
 * выводом записи сортируются по (участник, порядковый номер): участник —
 * место в очереди применения (танки), 100+команда (координаторы), 200
 * (арбитр). Каждый участник сдаёт свои события одной порцией за фазу. */
#define ACTOR_COORD 100
#define ACTOR_ARBITER 200

typedef struct {
    int actor;
    unsigned seq;
    size_t off, len;
} JournalEntry;

typedef struct {
    pthread_mutex_t lock;      /* защищает staged, entries, count, cap, seq, lost */
    StrBuf staged;
    JournalEntry *entries;
    size_t count, cap;
    unsigned seq;
    bool lost;                 /* часть событий потеряна из-за нехватки памяти */
    /* Ниже — только для сбрасывающего потока (арбитр во время боя, main до и после). */
    StrBuf out;
    int fd;
    bool echo;                 /* --plain: дублировать в stdout */
    int log_errno;             /* ошибка записи журнала (0 — нет) */
    bool out_failed;           /* stdout недоступен */
} Journal;

static int entry_cmp(const void *a, const void *b)
{
    const JournalEntry *x = a, *y = b;
    if (x->actor != y->actor)
        return x->actor < y->actor ? -1 : 1;
    if (x->seq != y->seq)
        return x->seq < y->seq ? -1 : 1;
    return 0;
}

static void journal_submit(Journal *j, int actor, const StrBuf *text)
{
    if (text->failed) {
        mutex_lock(&j->lock);
        j->lost = true;
        mutex_unlock(&j->lock);
    }
    if (text->len == 0)
        return;
    mutex_lock(&j->lock);
    if (j->count == j->cap) {
        size_t cap = j->cap ? j->cap * 2 : 64;
        JournalEntry *p = realloc(j->entries, cap * sizeof *p);
        if (!p) {
            j->lost = true;
            mutex_unlock(&j->lock);
            return;
        }
        j->entries = p;
        j->cap = cap;
    }
    size_t off = j->staged.len;
    sb_put(&j->staged, text->data, text->len);
    if (j->staged.failed) {
        /* sb_reserve отказал до копирования — буфер не изменён, его можно использовать дальше. */
        j->staged.failed = false;
        j->lost = true;
    } else {
        j->entries[j->count++] = (JournalEntry){actor, j->seq++, off, text->len};
    }
    mutex_unlock(&j->lock);
}

/* Вызывается только на границе фазы, когда пишущих участников нет. */
static void journal_flush(Journal *j)
{
    mutex_lock(&j->lock);
    qsort(j->entries, j->count, sizeof j->entries[0], entry_cmp);
    sb_clear(&j->out);
    for (size_t i = 0; i < j->count; i++)
        sb_put(&j->out, j->staged.data + j->entries[i].off, j->entries[i].len);
    j->count = 0;
    sb_clear(&j->staged);
    if (j->out.failed) {
        j->out.failed = false;
        j->lost = true;
    }
    mutex_unlock(&j->lock);
    if (j->out.len == 0)
        return;
    if (j->fd >= 0 && j->log_errno == 0 && write_all(j->fd, j->out.data, j->out.len) < 0)
        j->log_errno = errno;
    if (j->echo && !j->out_failed && write_all(STDOUT_FILENO, j->out.data, j->out.len) < 0)
        j->out_failed = true;
}

/* Запись от арбитра/main с немедленным сбросом (вне фаз потоков). */
static void journal_note(Journal *j, StrBuf *text)
{
    journal_submit(j, ACTOR_ARBITER, text);
    journal_flush(j);
    sb_clear(text);
}

/* --- Общее состояние симуляции --- */

/* Команда танку. CMD_NONE — ящик пуст: команды нет, ждать. */
enum Command { CMD_NONE, CMD_OBSERVE, CMD_DECIDE, CMD_QUIT };

/* Решение main о старте координаторов (см. start_threads). */
enum Launch { LAUNCH_WAIT, LAUNCH_GO, LAUNCH_ABORT };

/* Участники фазы координации: арбитр и координаторы A и B. */
#define COORD_PARTIES 3

typedef struct Sim Sim;
typedef struct {
    Sim *sim;
    int id;                   /* индекс танка или номер команды */
} WorkerArg;

/* Доска донесений команды: в фазе наблюдения в неё одновременно пишут все
 * живые танки команды, в фазе координации её читает и очищает координатор. */
typedef struct {
    pthread_mutex_t lock;
    Sightings data;
} TeamBoard;

/* Диспетчер фаз танков — монитор «мьютекс + две условные переменные».
 * Переменных две, а не одна: на go ждут танки, на done — только арбитр,
 * поэтому отчёт последнего танка будит ровно арбитра (signal), а не соседей. */
typedef struct {
    pthread_mutex_t lock;          /* защищает cmd, pending, launch */
    pthread_cond_t go;             /* изменились cmd или launch: ждут танки и координаторы до старта */
    pthread_cond_t done;           /* pending стал 0: ждёт арбитр */
    enum Command cmd[MAX_TANKS];   /* ящик танка i: кладёт арбитр, забирает и обнуляет танк */
    int pending;                   /* сколько танков ещё не отчитались о текущей фазе */
    enum Launch launch;
} Dispatch;

struct Sim {
    const Settings *cfg;
    /* Мир пишет только арбитр в своих фазах; потоки участников читают его в
     * фазах наблюдения/координации/решения, когда арбитр ждёт на done или на
     * барьере. Блокировка мира не нужна: эксклюзивность обеспечивает протокол. */
    World world;
    World before;                       /* снимок до выстрелов — для отрисовки огня (арбитр) */
    Observation obs[MAX_TANKS];         /* obs[i] пишет и читает только поток танка i */
    Plan plans[MAX_TANKS];              /* plans[i] пишет танк i, читает арбитр после done */
    Intel intel[2];                     /* пишет координатор, читают танки команды в фазе решения */
    TeamBoard board[2];
    unsigned rng[MAX_TANKS];            /* собственный генератор танка i: seed + 1 + i */
    Dispatch dispatch;
    pthread_barrier_t coord_barrier;    /* раунды «начало» и «конец» фазы координации */
    /* Пишет арбитр (или main вместо него) до раунда барьера, читают
     * координаторы после него — порядок обеспечивает сам барьер. */
    bool coord_quit;
    /* Координаторы получили LAUNCH_GO и участвуют в раундах барьера.
     * Пишет main до создания арбитра, затем только арбитр. */
    bool coord_launched;
    /* Поток танка запущен и ещё не получил CMD_QUIT. Пишет арбитр (и main до/после боя). */
    bool tank_running[MAX_TANKS];
    pthread_t tank_thr[MAX_TANKS], coord_thr[2], arbiter_thr;
    bool tank_created[MAX_TANKS], coord_created[2], arbiter_created;
    WorkerArg tank_arg[MAX_TANKS], coord_arg[2];
    Journal journal;
    Stats stats;                        /* только арбитр; main читает после join */
    ScreenLog screen;                   /* только арбитр */
    enum EndReason reason;
    int end_signal;
    bool screen_started;                /* ANSI-режим включён: курсор скрыт */
    int mutexes_ready, conds_ready;     /* сколько примитивов создано — для уничтожения */
    bool barrier_ready;
};

/* Тестовый самосигнал: процесс посылает сигнал себе в заданной фазе такта.
 * Из потоков танков/координатора сигнал приходит арбитру, пока тот ждёт конца фазы. */
static void maybe_inject(const Settings *cfg, enum Phase ph, int tick, StrBuf *ev)
{
    if (cfg->inject_tick != tick || cfg->inject_phase != ph)
        return;
    sb_printf(ev, "[T%d] тест: послан %s в фазе «%s»\n", tick, signal_name(cfg->inject_sig), PHASE_NAME[ph]);
    if (kill(getpid(), cfg->inject_sig) != 0)
        sb_printf(ev, "[T%d] тест: kill не удался: %s\n", tick, strerror(errno));
}

/* --- Поток танка --- */
static void *tank_main(void *arg)
{
    WorkerArg *wa = arg;
    Sim *s = wa->sim;
    Dispatch *d = &s->dispatch;
    const int id = wa->id;
    StrBuf ev = {0};
    for (;;) {
        /* Ждём свою команду. while, а не if: go общий для всех танков, и ни
         * broadcast из-за соседа, ни ложное пробуждение не должны запускать
         * фазу. Арбитр кладёт команду под этим же мьютексом до broadcast,
         * поэтому танк, не успевший уснуть, увидит её в предикате. */
        mutex_lock(&d->lock);
        while (d->cmd[id] == CMD_NONE)
            cond_wait(&d->go, &d->lock);
        const enum Command cmd = d->cmd[id];
        d->cmd[id] = CMD_NONE;
        mutex_unlock(&d->lock);
        if (cmd == CMD_QUIT)
            break;
        const World *w = &s->world;
        const int team = w->tanks[id].team;
        const int tick = w->frame + 1;
        const bool designated = first_alive(w) == id;
        sb_clear(&ev);
        if (cmd == CMD_OBSERVE) {
            observe(w, id, &s->obs[id]);
            describe_observation(&ev, w, id, &s->obs[id]);
            mutex_lock(&s->board[team].lock);
            sightings_merge(&s->board[team].data, w, id, &s->obs[id]);
            mutex_unlock(&s->board[team].lock);
            if (designated)
                maybe_inject(s->cfg, PH_OBSERVE, tick, &ev);
        } else if (cmd == CMD_DECIDE) {
            decide(w, id, &s->obs[id], &s->intel[team], &s->rng[id], &s->plans[id], &ev);
            if (designated)
                maybe_inject(s->cfg, PH_DECIDE, tick, &ev);
        }
        journal_submit(&s->journal, id, &ev);
        /* Отчёт. Последний отчитавшийся будит арбитра; если арбитр ещё не
         * уснул, он увидит pending == 0 в своём предикате. */
        mutex_lock(&d->lock);
        if (--d->pending == 0)
            cond_signal(&d->done);
        mutex_unlock(&d->lock);
    }
    sb_free(&ev);
    return NULL;
}

/* --- Поток координатора команды --- */
static void *coord_main(void *arg)
{
    WorkerArg *wa = arg;
    Sim *s = wa->sim;
    Dispatch *d = &s->dispatch;
    const int team = wa->id;

    /* До решения main к барьеру не подходим (см. start_threads). */
    mutex_lock(&d->lock);
    while (d->launch == LAUNCH_WAIT)
        cond_wait(&d->go, &d->lock);
    const bool launched = d->launch == LAUNCH_GO;
    mutex_unlock(&d->lock);
    if (!launched)
        return NULL;

    StrBuf ev = {0};
    for (;;) {
        barrier_wait(&s->coord_barrier);        /* раунд «начало» */
        if (s->coord_quit)
            break;
        const World *w = &s->world;
        Sightings snap;
        mutex_lock(&s->board[team].lock);
        snap = s->board[team].data;
        memset(&s->board[team].data, 0, sizeof s->board[team].data);
        mutex_unlock(&s->board[team].lock);
        sb_clear(&ev);
        coordinate(w, team, &snap, s->cfg->focus, &s->intel[team], &ev);
        if (team == 0)
            maybe_inject(s->cfg, PH_COORDINATE, w->frame + 1, &ev);
        journal_submit(&s->journal, ACTOR_COORD + team, &ev);
        barrier_wait(&s->coord_barrier);        /* раунд «конец» */
    }
    sb_free(&ev);
    return NULL;
}

/* --- Арбитр поля --- */

/* Раздать фазу всем живым танкам и дождаться отчёта каждого. broadcast, а не
 * signal на каждого: в фазе участвует каждый живой танк, а go у них общий. */
static void run_tank_phase(Sim *s, enum Command cmd)
{
    Dispatch *d = &s->dispatch;
    mutex_lock(&d->lock);
    for (int i = 0; i < s->world.count; i++) {
        if (!s->tank_running[i])
            continue;
        d->cmd[i] = cmd;
        d->pending++;
    }
    cond_broadcast(&d->go);
    while (d->pending > 0)
        cond_wait(&d->done, &d->lock);
    mutex_unlock(&d->lock);
}

/* Фаза координации — два раунда барьера: после «начала» координаторы читают
 * доски донесений, до «конца» публикуют intel[]. Сам арбитр между раундами
 * ничего не делает, он участник, без которого раунд не завершится. */
static void run_coord_phase(Sim *s)
{
    barrier_wait(&s->coord_barrier);
    barrier_wait(&s->coord_barrier);
}

/* Остановить поток танка. Он стоит на go с пустым ящиком (отчитался за
 * фазу), поэтому CMD_QUIT гарантированно будет прочитан. broadcast, а не
 * signal: signal мог бы разбудить другой танк, тот снова уснул бы, а адресат
 * так и не узнал бы о команде — join в конце боя завис бы. */
static void retire_tank(Sim *s, int i)
{
    if (!s->tank_running[i])
        return;
    Dispatch *d = &s->dispatch;
    mutex_lock(&d->lock);
    d->cmd[i] = CMD_QUIT;
    cond_broadcast(&d->go);
    mutex_unlock(&d->lock);
    s->tank_running[i] = false;
}

/* Решение о старте координаторов: LAUNCH_GO — оба созданы, LAUNCH_ABORT —
 * создать их (или потоки до них) не удалось, к барьеру подходить нельзя. */
static void launch_coordinators(Sim *s, enum Launch decision)
{
    Dispatch *d = &s->dispatch;
    mutex_lock(&d->lock);
    d->launch = decision;
    cond_broadcast(&d->go);
    mutex_unlock(&d->lock);
    s->coord_launched = decision == LAUNCH_GO;
}

/* Остановка всех участников. Вызывает арбитр в конце боя или main, если
 * арбитр не стартовал; в обоих случаях — вне фазы координации, когда
 * координаторы стоят (или вот-вот встанут) на раунде «начало». Флаг,
 * выставленный до раунда, они прочтут после него и выйдут, не дожидаясь
 * «конца»: этот лишний раунд и есть выход из барьера. */
static void stop_workers(Sim *s)
{
    for (int i = 0; i < s->world.count; i++)
        retire_tank(s, i);
    if (s->coord_launched) {
        s->coord_quit = true;
        barrier_wait(&s->coord_barrier);
        s->coord_launched = false;
    }
}

static bool stop_requested(const Sim *s)
{
    return g_signal != 0 || s->journal.out_failed;
}

static enum EndReason stop_reason(Sim *s)
{
    if (g_signal != 0) {
        s->end_signal = g_signal;
        return END_SIGNAL;
    }
    return END_IO;
}

static void show_frame(Sim *s, StrBuf *frame, const RenderOpts *ro)
{
    if (s->cfg->plain || s->journal.out_failed)
        return;
    sb_clear(frame);
    render_frame(frame, &s->world, &s->before, s->plans, &s->screen, ro);
    if (frame->failed || write_all(STDOUT_FILENO, frame->data, frame->len) < 0)
        s->journal.out_failed = true;
}

/* Задержка отображения с ранним выходом по сигналу.
 * В tanks_sem.c обработчик будил арбитра семафором. Условную переменную из
 * обработчика сигналить нельзя (см. on_signal), поэтому ожидание построено на
 * атомарной смене маски: SIGINT/SIGTERM блокируются, флаг проверяется, и
 * только ppoll открывает их на время сна — одним системным вызовом. Сигнал,
 * пришедший до блокировки, уже выставил флаг; пришедший после ждёт в очереди
 * и прерывает ppoll сразу (EINTR). Окна «проверил флаг — уснул» нет, поэтому
 * сон не нарезается на короткие куски. Длительность паузы меряется по
 * CLOCK_MONOTONIC, остаток пересчитывается после каждого пробуждения. */
static void interruptible_delay(int ms)
{
    if (ms <= 0)
        return;
    sigset_t sigs, saved, wait_mask;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    int rc = pthread_sigmask(SIG_BLOCK, &sigs, &saved);
    if (rc != 0)
        sync_fatal("pthread_sigmask", rc);
    wait_mask = saved;
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGTERM);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long long total = (long long)ms * 1000000LL;
    while (g_signal == 0) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long left = total - ((now.tv_sec - start.tv_sec) * 1000000000LL + (now.tv_nsec - start.tv_nsec));
        if (left <= 0)
            break;
        struct timespec ts = {.tv_sec = (time_t)(left / 1000000000LL), .tv_nsec = (long)(left % 1000000000LL)};
        if (ppoll(NULL, 0, &ts, &wait_mask) < 0 && errno != EINTR)
            sync_fatal("ppoll", errno);
    }

    rc = pthread_sigmask(SIG_SETMASK, &saved, NULL);
    if (rc != 0)
        sync_fatal("pthread_sigmask", rc);
}

static void *arbiter_main(void *arg)
{
    Sim *s = arg;
    const Settings *cfg = s->cfg;
    World *w = &s->world;
    StrBuf ev = {0}, frame = {0};
    int lethal_by[MAX_TANKS];
    bool died[MAX_TANKS];
    const RenderOpts ro = {.color = cfg->color, .max_ticks = cfg->max_ticks, .seed = cfg->seed};

    /* Только арбитр принимает SIGINT/SIGTERM (см. on_signal). Сигнал, пришедший
     * пока все потоки его блокировали, ждёт в очереди процесса и придёт сюда. */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    int rc = pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
    if (rc != 0)
        sync_fatal("pthread_sigmask", rc);

    s->before = *w;
    if (!cfg->plain) {
        if (write_all(STDOUT_FILENO, "\033[2J\033[?25l", 10) < 0)
            s->journal.out_failed = true;
        s->screen_started = true;
        show_frame(s, &frame, &ro);
    }

    enum EndReason reason = END_NONE;
    enum Phase stopped_after = PH_COUNT;
    while (reason == END_NONE) {
        if (battle_over(w, cfg->max_ticks)) {
            reason = (w->alive[0] == 0 || w->alive[1] == 0) ? END_DESTROYED : END_LIMIT;
            break;
        }
        if (stop_requested(s)) {
            reason = stop_reason(s);
            break;
        }
        const int tick = w->frame + 1;
        sb_printf(&ev, "=== Такт %d ===\n", tick);
        journal_note(&s->journal, &ev);
        /* Как actions[i] = WAIT в ДЗ-1: поток погибшего танка фазу решения
         * больше не получает, и его последний план не должен исполниться. */
        for (int i = 0; i < w->count; i++)
            s->plans[i] = (Plan){.action = WAIT, .dir = w->tanks[i].dir, .length = 0, .by_focus = false};

        /* Фазы участников: мир только читается. Прерывание здесь отбрасывает
         * решения такта целиком — результаты ещё не применялись. */
        static const enum Phase worker_phases[] = {PH_OBSERVE, PH_COORDINATE, PH_DECIDE};
        for (size_t k = 0; k < sizeof worker_phases / sizeof worker_phases[0]; k++) {
            if (worker_phases[k] == PH_COORDINATE)
                run_coord_phase(s);
            else
                run_tank_phase(s, worker_phases[k] == PH_OBSERVE ? CMD_OBSERVE : CMD_DECIDE);
            journal_flush(&s->journal);
            if (stop_requested(s)) {
                stopped_after = worker_phases[k];
                reason = stop_reason(s);
                break;
            }
        }
        if (reason != END_NONE)
            break;
        for (int i = 0; i < w->count; i++)
            if (s->plans[i].by_focus)
                s->stats.focus_decisions[w->tanks[i].team]++;

        /* Фазы арбитра: применение результатов доводится до конца такта даже
         * при сигнале, чтобы итог соответствовал целому числу тактов. */
        s->before = *w;
        apply_shots(w, s->plans, lethal_by, &s->stats, &s->screen, &ev);
        maybe_inject(cfg, PH_SHOOT, tick, &ev);
        apply_deaths(w, lethal_by, &s->stats, &s->screen, &ev, died);
        for (int i = 0; i < w->count; i++) {
            if (died[i]) {
                retire_tank(s, i);
                sb_printf(&ev, "[T%d] поток танка %s остановлен\n", tick, tank_tag(&w->tanks[i]).s);
            }
        }
        maybe_inject(cfg, PH_DESTROY, tick, &ev);
        apply_moves(w, s->plans, &s->stats, &ev);
        maybe_inject(cfg, PH_MOVE, tick, &ev);
        w->frame++;
        StrBuf why = {0};
        if (s->stats.violations > 0 || !check_invariants(w, &why)) {
            sb_printf(&ev, "[T%d] НАРУШЕНИЕ ИНВАРИАНТА: %s\n", tick, why.len ? why.data : "см. выше");
            reason = END_INTERNAL;
        }
        sb_free(&why);
        dump_field_state(&ev, w, "состояние поля после такта");
        journal_note(&s->journal, &ev);
        show_frame(s, &frame, &ro);
        maybe_inject(cfg, PH_RENDER, tick, &ev);
        if (reason == END_NONE && !battle_over(w, cfg->max_ticks)) {
            maybe_inject(cfg, PH_DELAY, tick, &ev);
            journal_note(&s->journal, &ev);
            interruptible_delay(cfg->delay_ms);
        }
        journal_note(&s->journal, &ev);
    }

    if (reason == END_SIGNAL || reason == END_IO) {
        const int at = stopped_after != PH_COUNT ? w->frame + 1 : w->frame;
        if (stopped_after != PH_COUNT)
            sb_printf(&ev, "[T%d] остановка после фазы «%s»: решения такта отброшены, результаты не применялись\n",
                      at, PHASE_NAME[stopped_after]);
        else
            sb_printf(&ev, "[T%d] остановка после завершения такта %d\n", at, at);
        if (reason == END_SIGNAL)
            sb_printf(&ev, "[T%d] получен %s: потоки останавливаются штатно\n", at, signal_name(s->end_signal));
        else
            sb_printf(&ev, "[T%d] вывод на экран недоступен: потоки останавливаются штатно\n", at);
        journal_note(&s->journal, &ev);
    }
    s->reason = reason;
    stop_workers(s);
    sb_free(&ev);
    sb_free(&frame);
    return NULL;
}

/* --- Создание и уничтожение примитивов, запуск и остановка потоков --- */

static bool sync_init(Sim *s)
{
    int rc;
    pthread_mutex_t *mtx[] = {&s->board[0].lock, &s->board[1].lock, &s->journal.lock, &s->dispatch.lock};
    const int nmtx = (int)(sizeof mtx / sizeof mtx[0]);
    for (s->mutexes_ready = 0; s->mutexes_ready < nmtx; s->mutexes_ready++) {
        if ((rc = pthread_mutex_init(mtx[s->mutexes_ready], NULL)) != 0) {
            err_printf("tanks_cond: pthread_mutex_init: %s\n", strerror(rc));
            return false;
        }
    }
    pthread_cond_t *cv[] = {&s->dispatch.go, &s->dispatch.done};
    const int ncv = (int)(sizeof cv / sizeof cv[0]);
    for (s->conds_ready = 0; s->conds_ready < ncv; s->conds_ready++) {
        if ((rc = pthread_cond_init(cv[s->conds_ready], NULL)) != 0) {
            err_printf("tanks_cond: pthread_cond_init: %s\n", strerror(rc));
            return false;
        }
    }
    if ((rc = pthread_barrier_init(&s->coord_barrier, NULL, COORD_PARTIES)) != 0) {
        err_printf("tanks_cond: pthread_barrier_init: %s\n", strerror(rc));
        return false;
    }
    s->barrier_ready = true;
    return true;
}

/* Вызывается, когда все потоки боя завершены (join) или не создавались. */
static void sync_destroy(Sim *s)
{
    if (s->barrier_ready)
        pthread_barrier_destroy(&s->coord_barrier);
    pthread_cond_t *cv[] = {&s->dispatch.go, &s->dispatch.done};
    for (int i = 0; i < s->conds_ready; i++)
        pthread_cond_destroy(cv[i]);
    pthread_mutex_t *mtx[] = {&s->board[0].lock, &s->board[1].lock, &s->journal.lock, &s->dispatch.lock};
    for (int i = 0; i < s->mutexes_ready; i++)
        pthread_mutex_destroy(mtx[i]);
    s->barrier_ready = false;
    s->conds_ready = s->mutexes_ready = 0;
}

static bool start_threads(Sim *s)
{
    int rc;
    for (int i = 0; i < s->world.count; i++) {
        s->tank_arg[i] = (WorkerArg){s, i};
        if ((rc = pthread_create(&s->tank_thr[i], NULL, tank_main, &s->tank_arg[i])) != 0) {
            err_printf("tanks_cond: не удалось создать поток танка: %s\n", strerror(rc));
            return false;
        }
        s->tank_created[i] = s->tank_running[i] = true;
    }
    for (int t = 0; t < 2; t++) {
        s->coord_arg[t] = (WorkerArg){s, t};
        if ((rc = pthread_create(&s->coord_thr[t], NULL, coord_main, &s->coord_arg[t])) != 0) {
            err_printf("tanks_cond: не удалось создать поток координатора: %s\n", strerror(rc));
            return false;
        }
        s->coord_created[t] = true;
    }
    /* Оба координатора существуют — барьер на троих укомплектован: третий
     * участник — арбитр, а если его создать не удастся, то main (join_all). */
    launch_coordinators(s, LAUNCH_GO);
    if ((rc = pthread_create(&s->arbiter_thr, NULL, arbiter_main, s)) != 0) {
        err_printf("tanks_cond: не удалось создать поток арбитра: %s\n", strerror(rc));
        return false;
    }
    s->arbiter_created = true;
    return true;
}

static void join_all(Sim *s)
{
    if (s->arbiter_created) {
        pthread_join(s->arbiter_thr, NULL);
    } else {
        /* Арбитр не стартовал: остановку вместо него проводит main. Танки ждут
         * команды на go; координаторы — либо решения о старте, либо, если оба
         * созданы, раунда барьера, третьим участником которого станет main. */
        if (!s->coord_launched)
            launch_coordinators(s, LAUNCH_ABORT);
        stop_workers(s);
    }
    for (int i = 0; i < MAX_TANKS; i++)
        if (s->tank_created[i])
            pthread_join(s->tank_thr[i], NULL);
    for (int t = 0; t < 2; t++)
        if (s->coord_created[t])
            pthread_join(s->coord_thr[t], NULL);
}

static bool install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    /* SA_RESTART не нужен: ppoll после сигнала возвращает EINTR и проверяет
     * флаг, прерванный write повторяется явно, а ожидания на мьютексе,
     * условной переменной и барьере по POSIX продолжаются сами. */
    sa.sa_flags = 0;
    /* Закрытый stdout (например, `| head`) — ошибка EPIPE и штатная остановка
     * с сохранением журнала, а не гибель процесса от SIGPIPE. */
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0 ||
        sigaction(SIGPIPE, &ign, NULL) != 0) {
        err_printf("tanks_cond: sigaction: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static int run_battle(const Settings *cfg)
{
    Sim *s = calloc(1, sizeof *s);
    if (!s) {
        err_printf("tanks_cond: нехватка памяти\n");
        return EXIT_SYSTEM;
    }
    s->cfg = cfg;
    s->journal.fd = -1;
    world_setup(&s->world, cfg);
    for (int i = 0; i < s->world.count; i++)
        s->rng[i] = cfg->seed + 1u + (unsigned)i;
    if (!sync_init(s)) {
        sync_destroy(s);
        free(s);
        return EXIT_SYSTEM;
    }
    const char *path = cfg->log_path ? cfg->log_path : DEFAULT_LOG_PATH;
    s->journal.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (s->journal.fd < 0) {
        err_printf("tanks_cond: не удалось открыть журнал '%s': %s\n", path, strerror(errno));
        sync_destroy(s);
        free(s);
        return EXIT_SYSTEM;
    }
    s->journal.echo = cfg->plain;

    StrBuf text = {0};
    write_header(&text, cfg, &s->world);
    dump_field_state(&text, &s->world, "начальная расстановка");
    journal_note(&s->journal, &text);

    /* main держит SIGINT/SIGTERM заблокированными (см. main), потоки наследуют
     * эту маску; разблокирует их только арбитр. */
    bool started = start_threads(s);
    join_all(s);

    int code = EXIT_OK;
    if (!started) {
        code = EXIT_SYSTEM;
        s->reason = END_NONE;
    }
    if (s->screen_started)
        (void)write_all(STDOUT_FILENO, "\033[0m\033[?25h", 10);
    write_summary(&text, &s->world, &s->stats, cfg, s->reason, s->end_signal);
    if (s->journal.lost)
        sb_puts(&text, "ВНИМАНИЕ: часть событий журнала потеряна из-за нехватки памяти\n");
    journal_submit(&s->journal, ACTOR_ARBITER, &text);
    journal_flush(&s->journal);
    if (!cfg->plain && !s->journal.out_failed) {
        sb_puts(&text, "Журнал: ");
        sb_puts(&text, path);
        sb_puts(&text, "\n");
        if (write_all(STDOUT_FILENO, text.data, text.len) < 0)
            s->journal.out_failed = true;
    }
    sb_free(&text);

    if (close(s->journal.fd) != 0 && s->journal.log_errno == 0)
        s->journal.log_errno = errno;
    if (s->journal.log_errno != 0) {
        err_printf("tanks_cond: журнал '%s' записан не полностью: %s\n", path, strerror(s->journal.log_errno));
        code = EXIT_SYSTEM;
    }
    if (s->reason == END_IO || s->reason == END_INTERNAL || s->journal.out_failed)
        code = EXIT_SYSTEM;
    if (s->reason == END_SIGNAL)
        code = 128 + s->end_signal;

    sync_destroy(s);
    free(s->journal.entries);
    sb_free(&s->journal.staged);
    sb_free(&s->journal.out);
    free(s);
    return code;
}

/* ======================================================================
 * 8. main
 * ====================================================================== */

int main(int argc, char **argv)
{
    Settings cfg;
    settings_init(&cfg);
    enum ParseResult pr = parse_cli(argc, argv, &cfg);
    if (pr != PARSE_OK) {
        settings_free(&cfg);
        if (pr == PARSE_HELP) {
            print_help();
            return EXIT_OK;
        }
        return EXIT_BAD_INPUT;
    }
    if (!install_signal_handlers()) {
        settings_free(&cfg);
        return EXIT_SYSTEM;
    }
    /* С этого момента SIGINT/SIGTERM в main заблокированы: при настройке они
     * открываются только внутри ppoll, во время боя — только в арбитре. */
    sigset_t sigs, wait_mask;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    int rc = pthread_sigmask(SIG_BLOCK, &sigs, &wait_mask);
    if (rc != 0)
        sync_fatal("pthread_sigmask", rc);
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGTERM);

    int code = EXIT_OK;
    if (!cfg.batch && !cfg.field_given) {
        InReader *in = calloc(1, sizeof *in);
        if (in)
            in->wait_mask = wait_mask;
        enum InStatus st = in ? interactive_setup(in, &cfg) : IN_ERROR;
        free(in);
        if (st == IN_EOF) {
            out_printf("\nВвод закончился до начала боя — бой не запускается.\n");
            code = EXIT_NO_BATTLE;
        } else if (st == IN_SIGNAL) {
            out_printf("\nНастройка прервана сигналом %s — бой не запускается.\n", signal_name(g_signal));
            code = 128 + g_signal;
        } else if (st == IN_ERROR) {
            err_printf("tanks_cond: ошибка ввода-вывода при настройке: %s\n", strerror(errno));
            code = EXIT_SYSTEM;
        }
    }
    if (code == EXIT_OK && !finalize_teams(&cfg))
        code = EXIT_BAD_INPUT;
    if (code == EXIT_OK) {
        if (!cfg.seed_set)
            cfg.seed = (unsigned)time(NULL);
        if (!cfg.plain && !cfg.ansi && !isatty(STDOUT_FILENO))
            cfg.plain = true;
        code = run_battle(&cfg);
    }
    settings_free(&cfg);
    return code;
}

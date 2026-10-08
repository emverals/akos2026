#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <locale.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#define MAX_N 12
#define MAX_M 12
#define MAX_TEAM_TANKS ((MAX_M + 1) / 2)
#define MAX_TANKS (2 * MAX_TEAM_TANKS)
#define TANK_TYPES 4
#define CELL_W 11
#define CELL_H 5
#define TURN_PROBABILITY 0.15
#define MAX_FRAMES 600
#define LOG_LINES 6
int min(int a, int b) {
    return a < b ? a : b;
}
enum Direction {
    UP, DOWN, LEFT, RIGHT
};
enum Move {
    WAIT,
    SHOOT,
    AIM,
    MOVE
};
typedef struct {
    int x, y;// Левый верхний угол в символах
    int type, number; // Тип и постоянный номер внутри команды
    int team;// 0 - А, 1 - В
    bool alive;
    int hp, speed, cooldown, weight, damage;
    enum Direction dir;
    int last_shot;// Такт, на котором был произведен последний выстрел
    bool aimed;// Находится ли танк в режиме прицеливания
    wchar_t icon[4][CELL_H][CELL_W + 1];
} Tank;
Tank types[TANK_TYPES] = {
        {.hp = 300, .speed = 3, .cooldown = 14, .weight = 188, .damage = 45},// Maus
        {.hp = 130, .speed = 2, .cooldown = 24, .weight = 60,  .damage = 100},// FV215b183
        {.hp = 210, .speed = 4, .cooldown = 6,  .weight = 37,  .damage = 60},// T62A
        {.hp = 180, .speed = 5, .cooldown = 8,  .weight = 40,  .damage = 70}// Leopard 1
    };
const char *type_names[TANK_TYPES] = {"mouse", "fv215b183", "t62a", "leopard_1"};
const char *team_colors[2] = {"\033[96m", "\033[91m"};
char logs[LOG_LINES][256];
int log_count = 0;

// Храним только последние LOG_LINES событий.
void add_log(const char *line) {
    if (log_count == LOG_LINES) {
        for (int i = 1; i < LOG_LINES; i++)
            strcpy(logs[i - 1], logs[i]);
        log_count--;
    }
    snprintf(logs[log_count++], sizeof logs[0], "%s", line);
}

int frame = 0;
bool map[MAX_N][MAX_M];
Tank tanks[MAX_TANKS];
enum Move actions[MAX_TANKS];// Действие, выбранное танком
int n = 10, m = 10;
double wall_probability = 0.4;
int team_count[2] = {4, 4};
int tank_count;
int composition[2][TANK_TYPES];
bool random_teams = true;
int alive_a, alive_b;
// Пустая строка оставляет значения по умолчанию.
bool read_line(const char *prompt, char line[128]) {
    while (true) {
        fputs(prompt, stdout);
        fflush(stdout);
        if (!fgets(line, 128, stdin))
            return false;
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            puts("Слишком длинная строка. Повторите ввод.");
            continue;
        }
        if (line[strspn(line, " \t\r\n")] == '\0')
            line[0] = '\0';
        return true;
    }
}

bool read_ints(const char *prompt, int values[], int count) {
    char line[128];
    while (read_line(prompt, line)) {
        if (!line[0])
            return true;
        int parsed[TANK_TYPES];
        char *cursor = line;
        bool valid = true;
        for (int i = 0; i < count; i++) {
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
            if (*cursor && !strchr(" \t\r\n", *cursor)) {
                valid = false;
                break;
            }
        }
        if (valid && cursor[strspn(cursor, " \t\r\n")] == '\0') {
            for (int i = 0; i < count; i++)
                values[i] = parsed[i];
            return true;
        }
        printf("Введите ровно %d целых чисел через пробел.\n", count);
    }
    return false;
}

bool read_settings(void) {
    puts("Enter - оставить значение по умолчанию.");
    int size[2] = {10, 10};
    while (true) {
        if (!read_ints("Размер поля, строки столбцы [10 10]: ", size, 2))
            return false;
        if (size[0] >= 3 && size[0] <= MAX_N && size[1] >= 1 && size[1] <= MAX_M)
            break;
        printf("Строки: 3..%d, столбцы: 1..%d.\n", MAX_N, MAX_M);
        size[0] = 10;
        size[1] = 10;
    }
    n = size[0];
    m = size[1];

    char line[128];
    while (true) {
        if (!read_line("Вероятность препятствия в столбце [0.4]: ", line))
            return false;
        if (!line[0])
            break;
        char *end;
        errno = 0;
        double value = strtod(line, &end);
        if (end != line && errno != ERANGE &&
            end[strspn(end, " \t\r\n")] == '\0' && value >= 0 && value <= 1) {
            wall_probability = value;
            break;
        }
        puts("Введите число от 0 до 1, дробную часть отделяйте точкой.");
    }

    int mode = 2;
    while (true) {
        if (!read_ints("Состав: 1 — вручную, 2 — случайный [2]: ", &mode, 1))
            return false;
        if (mode == 1 || mode == 2)
            break;
        puts("Введите 1 или 2.");
        mode = 2;
    }
    random_teams = mode == 2;
    int limit = (m + 1) / 2;
    printf("В каждой команде от 1 до %d танков (расстановка через клетку).\n", limit);
    for (int team = 0; team < 2; team++) {
        char prompt[160];
        int default_count = min(4, limit);
        if (random_teams) {
            team_count[team] = default_count;
            snprintf(prompt, sizeof prompt, "Количество танков команды %c [%d]: ",
                     team ? 'B' : 'A', default_count);
            while (true) {
                if (!read_ints(prompt, &team_count[team], 1))
                    return false;
                if (team_count[team] >= 1 && team_count[team] <= limit)
                    break;
                printf("Команда должна содержать от 1 до %d танков.\n", limit);
                team_count[team] = default_count;
            }
        } else {
            for (int i = 0; i < default_count; i++)
                composition[team][i] = 1;
            snprintf(prompt, sizeof prompt,
                     "Команда %c, mouse fv215b183 t62a leopard_1 [%d %d %d %d]: ",
                     team ? 'B' : 'A', composition[team][0], composition[team][1],
                     composition[team][2], composition[team][3]);
            while (true) {
                int values[TANK_TYPES];
                memcpy(values, composition[team], sizeof values);
                if (!read_ints(prompt, values, TANK_TYPES))
                    return false;
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
                    memcpy(composition[team], values, sizeof values);
                    team_count[team] = sum;
                    break;
                }
                printf("Количество каждого типа неотрицательно, сумма — от 1 до %d.\n", limit);
            }
        }
    }
    tank_count = team_count[0] + team_count[1];
    alive_a = team_count[0];
    alive_b = team_count[1];
    return true;
}

int dx(enum Direction dir) {
    if (dir == LEFT) return -1;
    if (dir == RIGHT) return 1;
    return 0;
}
int dy(enum Direction dir) {
    if (dir == UP) return -1;
    if (dir == DOWN) return 1;
    return 0;
}
enum Direction opposite(enum Direction dir) {
    switch (dir) {
        case UP:    return DOWN;
        case DOWN:  return UP;
        case LEFT:  return RIGHT;
        case RIGHT: return LEFT;
    }
    return UP;
}
bool contains(const Tank *tank, int x, int y) {
    return x >= tank->x && x < tank->x + CELL_W &&
           y >= tank->y && y < tank->y + CELL_H;
}
void side_directions(enum Direction dir, enum Direction sides[2]) {
    bool vertical = dir == UP || dir == DOWN;
    sides[0] = vertical ? LEFT : UP;
    sides[1] = vertical ? RIGHT : DOWN;
}
// Выбирает случайное направление поворота
int choose_side(const int lengths[2]) {
    if (lengths[0] > 0 && lengths[1] > 0)
        return rand() % 2;
    return lengths[0] > 0 ? 0 : 1;
}
// Номер танка, в который попадет выстрел в напревлении dir. -1, если никуда
int first_hit(Tank *tank, enum Direction dir) {
    int x = tank->x + CELL_W / 2;
    int y = tank->y + CELL_H / 2;
    int step_x = dx(dir), step_y = dy(dir);
    while (true) {
        x += step_x;
        y += step_y;
        if (x < 0 || x >= m * CELL_W ||
            y < 0 || y >= n * CELL_H)
            return -1;
        if (map[y / CELL_H][x / CELL_W])
            return -1;
        for (int i = 0; i < tank_count; i++) {
            Tank *other = &tanks[i];
            if (other == tank || !other->alive)
                continue;
            if (contains(other, x, y))
                return i;
        }
    }
}
bool reloaded(const Tank *tank) {
    return frame - tank->last_shot >= tank->cooldown;
}
void shoots(void) {
// Сначала обработка выстрелов, потом обработка hp
    for (int i = 0; i < tank_count; i++) {
        if (actions[i] == SHOOT) {
            int target = first_hit(&tanks[i], tanks[i].dir);
            char line[256];
            if (target != -1) {
                tanks[target].hp -= tanks[i].damage;
                snprintf(line, sizeof line, "%s%s\033[0m -> %s%s\033[0m | -%dhp",
                         team_colors[tanks[i].team], type_names[tanks[i].type],
                         team_colors[tanks[target].team], type_names[tanks[target].type],
                         tanks[i].damage);
            } else {
                snprintf(line, sizeof line, "%s%s\033[0m -> MISS",
                         team_colors[tanks[i].team], type_names[tanks[i].type]);
            }
            add_log(line);
            tanks[i].last_shot = frame;
            tanks[i].aimed = false;
        }
    }
    for (int i = 0; i < tank_count; i++) {
        if (tanks[i].alive && tanks[i].hp <= 0) {
            tanks[i].hp = 0;
            tanks[i].alive = false;
            char line[256];
            snprintf(line, sizeof line, "%s%s\033[0m DEAD",
                     team_colors[tanks[i].team], type_names[tanks[i].type]);
            add_log(line);
            tanks[i].team ? alive_b-- : alive_a--;
        }
    }
}
// Можно ли поставить танк id в координаты x, y.
bool can_place(int id, int x, int y) {
    if (x < 0 || y < 0 ||
        x + CELL_W > m * CELL_W ||
        y + CELL_H > n * CELL_H)
        return false;
    for (int row = y / CELL_H;
        row <= (y + CELL_H - 1) / CELL_H; row++) {
        for (int col = x / CELL_W;
            col <= (x + CELL_W - 1) / CELL_W; col++) {
            if (map[row][col])
                return false;
        }
    }
    for (int i = 0; i < tank_count; i++) {
        if (i == id || !tanks[i].alive)
            continue;
        if (x < tanks[i].x + CELL_W &&
            x + CELL_W > tanks[i].x &&
            y < tanks[i].y + CELL_H &&
            y + CELL_H > tanks[i].y)
            return false;
    }
    return true;
}
// Максимальное доступное движение в направлении dir
int move_length(int id, enum Direction dir) {
    int step_x = dx(dir), step_y = dy(dir);
    for (int step = 1; step <= tanks[id].speed; step++) {
        if (!can_place(id, tanks[id].x + step_x * step, tanks[id].y + step_y * step))
            return step - 1;
    }
    return tanks[id].speed;
}
// Одна строка таблицы для одного танка
void draw_tank_status(const Tank *tank) {
    if (tank == NULL) {
        printf("%-48s", "");
        return;
    }
    char reload[24], line[96];
    int remaining = tank->cooldown - (frame - tank->last_shot);
    if (!tank->alive)
        strcpy(reload, "DEAD");
    else if (remaining <= 0)
        strcpy(reload, "READY");
    else
        snprintf(reload, sizeof reload, "CD: %d", remaining);
    snprintf(line, sizeof line, "%c%d %-12s HP: %3d/%3d  %s",
             tank->team ? 'B' : 'A', tank->number, type_names[tank->type],
             tank->hp, types[tank->type].hp, reload);
    printf("%s%-48s\033[0m", team_colors[tank->team], line);
}

void draw_panels(void) {
    printf("\033[0mLOG\033[K\r\n");
    for (int i = 0; i < LOG_LINES; i++)
        printf("%s\033[0m\033[K\r\n", i < log_count ? logs[i] : "");

    printf("%s%-48s%s%-48s\033[0m\033[K\r\n",
           team_colors[0], "TEAM A", team_colors[1], "TEAM B");
    int rows = team_count[0] > team_count[1] ? team_count[0] : team_count[1];
    for (int row = 1; row <= rows; row++) {
        for (int team = 0; team < 2; team++) {
            const Tank *tank = NULL;
            for (int i = 0; i < tank_count; i++) {
                if (tanks[i].team == team && tanks[i].number == row) {
                    tank = &tanks[i];
                    break;
                }
            }
            draw_tank_status(tank);
        }
        printf("\033[K\r\n");
    }

    printf("TYPES | S: symbols/tick, D: damage, HP: initial, CD: ticks\033[K\r\n");
    for (int row = 0; row < CELL_H; row++) {
        for (int type = 0; type < TANK_TYPES; type++)
            printf("%ls%16s", types[type].icon[UP][row], "");
        printf("\033[K\r\n");
    }
    for (int type = 0; type < TANK_TYPES; type++)
        printf("%-27s", type_names[type]);
    printf("\033[K\r\n");
    for (int type = 0; type < TANK_TYPES; type++) {
        char line[64];
        snprintf(line, sizeof line, "S: %d  D: %d", types[type].speed, types[type].damage);
        printf("%-27s", line);
    }
    printf("\033[K\r\n");
    for (int type = 0; type < TANK_TYPES; type++) {
        char line[64];
        snprintf(line, sizeof line, "HP: %d  CD: %d", types[type].hp, types[type].cooldown);
        printf("%-27s", line);
    }
    printf("\033[K\r\n\033[J");
}

// before - позиции до движения для отрисовки огня
// tanks - позиции после такта для отрисовки танков и прицела
void draw_field(const Tank before[MAX_TANKS]) {
    wchar_t picture[MAX_N * CELL_H][MAX_M * CELL_W];
    int color[MAX_N * CELL_H][MAX_M * CELL_W];
    const char *colors[] = {
        "\033[0m", "\033[90m", "\033[96m", "\033[91m", "\033[93m"
    };
    for (int y = 0; y < n * CELL_H; y++) {
        for (int x = 0; x < m * CELL_W; x++) {
            picture[y][x] = map[y / CELL_H][x / CELL_W] ? '#' : ' ';
            color[y][x] = map[y / CELL_H][x / CELL_W] ? 1 : 0;
        }
    }
// Сначала прицелы, затем выстрелы поверх них.
    for (int fire = 0; fire <= 1; fire++) {
        const Tank *units = fire ? before : tanks;
        for (int i = 0; i < tank_count; i++) {
            const Tank *tank = &units[i];
            if (!tank->alive) continue;
            if (fire ? actions[i] != SHOOT : !tank->aimed) continue;
            int x = tank->x + CELL_W / 2;
            int y = tank->y + CELL_H / 2;
            while (true) {
                x += dx(tank->dir);
                y += dy(tank->dir);
                if (x < 0 || x >= m * CELL_W ||
                    y < 0 || y >= n * CELL_H)
                    break;
                if (map[y / CELL_H][x / CELL_W]) break;
                bool hit = false;
                for (int j = 0; j < tank_count; j++) {
                    if (j == i || !units[j].alive) continue;
                    if (contains(&units[j], x, y)) {
                        hit = true;
                        break;
                    }
                }
                if (hit) break;
// Пропускаем собственный прямоугольник танка.
                if (contains(tank, x, y))
                    continue;
                if (fire) {
                    picture[y][x] = dx(tank->dir) == 0 ? '|' : '-';
                    color[y][x] = 4;
                } else {
// U+00B7 — средняя точка для горизонтального прицела.
                    picture[y][x] = dx(tank->dir) == 0 ? '.' : 0x00B7;
                    color[y][x] = tank->team == 0 ? 2 : 3;
                }
            }
        }
    }
    for (int i = 0; i < tank_count; i++) {
        const Tank *tank = &tanks[i];
        if (!tank->alive) continue;
        for (int row = 0; row < CELL_H; row++) {
            for (int col = 0; col < CELL_W; col++) {
                picture[tank->y + row][tank->x + col] =
                    tank->icon[tank->dir][row][col];
                color[tank->y + row][tank->x + col] = tank->team == 0 ? 2 : 3;
            }
        }
        if (reloaded(tank)) {
            int x = tank->x + CELL_W / 2
                    + dx(tank->dir) * (CELL_W / 2);
            int y = tank->y + CELL_H / 2
                    + dy(tank->dir) * (CELL_H / 2);
            color[y][x] = 4;
        }
    }
    printf("\033[H\033[0mTick: %d/%d | A: %d | B: %d\033[K\r\n",
           frame, MAX_FRAMES, alive_a, alive_b);
    putchar('+');
    for (int x = 0; x < m * CELL_W; x++) putchar('-');
    printf("+\r\n");
    for (int y = 0; y < n * CELL_H; y++) {
        printf("\033[0m|");
        int last_color = -1;
        for (int x = 0; x < m * CELL_W; x++) {
            if (color[y][x] != last_color) {
                fputs(colors[color[y][x]], stdout);
                last_color = color[y][x];
            }
            printf("%lc", (wint_t)picture[y][x]);
        }
        printf("\033[0m|\r\n");
    }
    putchar('+');
    for (int x = 0; x < m * CELL_W; x++) putchar('-');
    printf("+\033[0m\r\n");
    draw_panels();
    fflush(stdout);
}
int main(void) {
    setlocale(LC_CTYPE, "");
    if (!read_settings())
        return 0;
    srand((unsigned)time(NULL));

// Четыре готовых рисунка для каждого типа, порядок направлений как в enum.
    static const wchar_t models[TANK_TYPES][4][CELL_H][CELL_W + 1] = {
        {// Maus
            {// UP
                L"   ▄▄▄▄▄   ",
                L"  █  |  █  ",
                L"  █ ▄█▄ █  ",
                L"  █ ███ █  ",
                L"  █▄▄▄▄▄█  ",
            },
            {// DOWN
                L"  █▀▀▀▀▀█  ",
                L"  █ ███ █  ",
                L"  █ ▀█▀ █  ",
                L"  █  |  █  ",
                L"   ▀▀▀▀▀   ",
            },
            {// LEFT
                L"           ",
                L"▄▀▀▀▀▀▀▀▀▀█",
                L"█ ----███ █",
                L"▀▄▄▄▄▄▄▄▄▄█",
                L"           ",
            },
            {// RIGHT
                L"           ",
                L"█▀▀▀▀▀▀▀▀▀▄",
                L"█ ███---- █",
                L"█▄▄▄▄▄▄▄▄▄▀",
                L"           ",
            },
        },
        {// FV215b183
            {// UP
                L"   _ ▼ _   ",
                L"  /  |  \\  ",
                L"  | ███ |  ",
                L"  █ --- █  ",
                L"  ▀-▄▄▄-▀  ",
            },
            {// DOWN
                L"  ▄-▀▀▀-▄  ",
                L"  █ --- █  ",
                L"  | ███ |  ",
                L"  \\  |  /  ",
                L"   ‾ ▲ ‾   ",
            },
            {// LEFT
                L"           ",
                L"  /‾‾‾▀▀▀▀|",
                L"▶------██|█",
                L"  \\___▄▄▄▄|",
                L"           ",
            },
            {// RIGHT
                L"           ",
                L"|▀▀▀▀‾‾‾\\  ",
                L"█|██------◀",
                L"|▄▄▄▄___/  ",
                L"           ",
            },
        },
        {// T62A
            {// UP
                L"  /‾ | ‾\\  ",
                L"  █ ▄▄▄ █  ",
                L"  █ ▀▀▀ █  ",
                L"  █ === █  ",
                L"  █▄▄▄▄▄█  ",
            },
            {// DOWN
                L"  █▀▀▀▀▀█  ",
                L"  █ === █  ",
                L"  █ ▄▄▄ █  ",
                L"  █ ▀▀▀ █  ",
                L"  \\_ | _/  ",
            },
            {// LEFT
                L"           ",
                L"/▀▀▀▀▀▀▀▀▀▄",
                L"----██  ||█",
                L"\\▄▄▄▄▄▄▄▄▄▀",
                L"           ",
            },
            {// RIGHT
                L"           ",
                L"▄▀▀▀▀▀▀▀▀▀\\",
                L"█||  ██----",
                L"▀▄▄▄▄▄▄▄▄▄/",
                L"           ",
            },
        },
        {// Leopard1
            {// UP
                L"  /‾▀▀▀‾\\  ",
                L"  |  |  |  ",
                L"  | ▄█▄ |  ",
                L"  | ‾‾‾ |  ",
                L"   ‾▀▀▀‾   ",
            },
            {// DOWN
                L"   _▄▄▄_   ",
                L"  | ___ |  ",
                L"  | ▀█▀ |  ",
                L"  |  |  |  ",
                L"  \\_▄▄▄_/  ",
            },
            {// LEFT
                L"           ",
                L"/‾‾‾‾‾‾‾‾‾▄",
                L"█ --|█|  |█",
                L"\\_________▀",
                L"           ",
            },
            {// RIGHT
                L"           ",
                L"▄‾‾‾‾‾‾‾‾‾\\",
                L"█|  |█|-- █",
                L"▀_________/",
                L"           ",
            },
        },
    };
    for (int i = 0; i < TANK_TYPES; i++) {
        memcpy(types[i].icon, models[i], sizeof types[i].icon);
    }
// Команда A сверху, команда B снизу
    for (int i = 0; i < tank_count; i++) {
        Tank *tank = &tanks[i];
        int team = i >= team_count[0];
        int slot = team ? i - team_count[0] : i;
        int type;
        if (random_teams) {
            type = rand() % TANK_TYPES;
        } else {
            int position = slot;
            type = 0;
            while (position >= composition[team][type]) {
                position -= composition[team][type];
                type++;
            }
        }
        *tank = types[type];
        tank->type = type;
        tank->team = team;
        tank->number = slot + 1;
        tank->x = (tank->team ? m - 1 - slot * 2 : slot * 2) * CELL_W;
        tank->y = tank->team ? (n - 1) * CELL_H : 0;
        tank->dir = tank->team ? UP : DOWN;
        tank->alive = true;
        tank->last_shot = -tank->cooldown;
    }
    // Верхняя и нижняя строки свободны от препятствий, в каждом столбце стоит
    // препятствие с вероятностью  wall_probability в случайной клетке
    for (int i = 0; i < m; i++) {
        if ((double)rand() / RAND_MAX < wall_probability)
            map[rand() % (n - 2) + 1][i] = true;
    }
    // При равной массе приоритет определяется случайной расстановкой
    for (int i = tank_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Tank tmp = tanks[i];
        tanks[i] = tanks[j];
        tanks[j] = tmp;
    }
    // Сортировка по убыванию веса, так как тяжелдые танки делают ходы "первыми"
    for (int i = 1; i < tank_count; i++) {
        Tank tank = tanks[i];
        int j = i;
        while (j > 0 && tanks[j - 1].weight < tank.weight) {
            tanks[j] = tanks[j - 1];
            j--;
        }
        tanks[j] = tank;
    }
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 350000000L};
    Tank before[MAX_TANKS];
    memcpy(before, tanks, sizeof before);
    printf("\033[2J");
    draw_field(before);
    while (alive_a > 0 && alive_b > 0 && frame < MAX_FRAMES) {
        enum Direction go_dir[MAX_TANKS];
        int go_length[MAX_TANKS];
        for (int i = 0; i < tank_count; i++) {
            Tank *tank = &tanks[i];
            actions[i] = WAIT;
            go_dir[i] = tank->dir;
            go_length[i] = 0;
            if (!tank->alive)
                continue;
            int target = first_hit(tank, tank->dir);
            if (tank->aimed && reloaded(tank) &&
                target != -1 && tanks[target].team != tank->team) {
                actions[i] = SHOOT;
                continue;
            }
            // Выбираем противника, в которого можем прицелиться, отдавая приоритет заряженному.
            int enemy = -1;
            enum Direction enemy_dir = UP;
            for (int dir = UP; dir <= RIGHT; dir++) {
                int hit = first_hit(tank, dir);
                if (hit == -1 || tanks[hit].team == tank->team)
                    continue;
                if (reloaded(&tanks[hit]) || enemy == -1) {
                    enemy = hit;
                    enemy_dir = (enum Direction)dir;
                }
            }
            if (enemy != -1) {
                bool can_aim = reloaded(tank);
            // Два направления уклонения, перпендикулярных направлению на врага.
                enum Direction leave_dirs[2];
                side_directions(enemy_dir, leave_dirs);
                int lengths[2] = {
                    move_length(i, leave_dirs[0]),
                    move_length(i, leave_dirs[1])
                };
                bool can_leave = lengths[0] > 0 || lengths[1] > 0;
                bool prefer_aim =
                    (double)tanks[enemy].damage / tank->hp <=
                    (double)tank->damage / tanks[enemy].hp
                    || !reloaded(&tanks[enemy]);
                if (can_aim && (!can_leave || prefer_aim)) {// Прицеливание
                    actions[i] = AIM;
                    go_dir[i] = enemy_dir;
                    continue;
                }
                if (can_leave) {// Попытка уклониться
                    int side = choose_side(lengths);
                    actions[i] = MOVE;
                    go_dir[i] = leave_dirs[side];
                    go_length[i] = lengths[side];
                    continue;
                }
            }
        // 3. Обычное движение
            actions[i] = MOVE;
            int forward_length = move_length(i, tank->dir);
            enum Direction side_dirs[2];
            side_directions(tank->dir, side_dirs);
            int lengths[2] = {
                move_length(i, side_dirs[0]),
                move_length(i, side_dirs[1])
            };
            bool can_turn = lengths[0] > 0 || lengths[1] > 0;
            if (can_turn && (forward_length == 0 ||
                (double)rand() / RAND_MAX < TURN_PROBABILITY)) {
                int side = choose_side(lengths);
                go_dir[i] = side_dirs[side];
                go_length[i] = lengths[side];
            } else if (forward_length > 0) {
                go_length[i] = forward_length;
            } else {
                go_dir[i] = opposite(tank->dir);
            }
        }
        // Применение действий
        memcpy(before, tanks, sizeof before);
        shoots();
        for (int i = 0; i < tank_count; i++) {
            if (!tanks[i].alive)
                continue;
            if (actions[i] == AIM) {
                tanks[i].aimed = true;
                tanks[i].dir = go_dir[i];
            } else if (actions[i] == MOVE) {
                tanks[i].aimed = false;
                tanks[i].dir = go_dir[i];
                int can_length = min(move_length(i, go_dir[i]), go_length[i]);
                tanks[i].x += dx(go_dir[i]) * can_length;
                tanks[i].y += dy(go_dir[i]) * can_length;
            }
        }
        frame++;
        draw_field(before);
        nanosleep(&delay, NULL);
    }
    return 0;
}

// PONG - CON MODO CPU VS JUGADOR
// CC3086 - Programación de microprocesadores
// Requiere: ncurses y pthreads
// Compilar: g++ -std=c++17 pong.c -o pong -lncursesw -lpthread -lm

#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
//Para el calculo de tiempos
#include <chrono>
using namespace std::chrono;
static duration<double> time_ball{0};
static duration<double> time_p1{0};
static duration<double> time_p2{0};
static duration<double> time_menu{0};
static duration<double> time_instructions{0};
static duration<double> time_leaderboard{0};
static duration<double> time_render{0};



// CONFIGURACIÓN
#define TARGET_FPS_PLAY 60
#define FRAME_USEC_PLAY (1000000 / TARGET_FPS_PLAY)

#define PADDLE_LEN 5
#define PADDLE_SPEED 1
#define BALL_SPEED_X 0.8f
#define BALL_SPEED_Y 0.4f
#define SCORE_TO_WIN 7

#define PADDLE_DT        (1.0f / TARGET_FPS_PLAY)
#define PADDLE_ACC       500.0f
#define PADDLE_MAX_V     30.0f
#define PADDLE_FRICTION  30.0f

#define NAME_MAXLEN 24
#define LEADERBOARD_FILE "pong_scores.txt"
#define MAX_LEADER_ENTRIES 200

#define BALL_SPEED_MIN 0.45f
#define BALL_SPEED_MAX 1.10f

// Configuración de dificultad CPU
#define CPU_REACTION_DELAY 3  // Frames de delay para la CPU
#define CPU_ERROR_MARGIN 1.5f // Margen de error en la predicción

#define HOLD_FRAMES 4

// ESTADO GLOBAL
typedef struct {
    float x, y;
    float vx, vy;
} Ball;

typedef struct {
    int x;
    float y;
    float vy;
} Paddle;

typedef struct {
    int p1, p2;
} Score;

typedef struct {
    char winner[NAME_MAXLEN+1];
    char loser[NAME_MAXLEN+1];
    int  winScore;
    int  loseScore;
    time_t ts;
} Entry;

// Estados
typedef enum {
    SC_MENU = 0,
    SC_INSTR,
    SC_PLAYING,
    SC_LEADER
} Scene;

// Modos de juego
typedef enum {
    MODE_PVP = 0,      // Jugador vs Jugador
    MODE_PVC,          // Jugador vs Computadora
    MODE_CVC           // Computadora vs Computadora
} GameMode;

static bool g_p1_ai = false;
static bool g_p2_ai = false;
static int g_p1_hold_up = 0, g_p1_hold_down = 0;
static int g_p2_hold_up = 0, g_p2_hold_down = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_threads_should_run = false;
static volatile bool g_paused = false;
static volatile bool g_exit_requested = false;

// Entradas
static volatile int g_p1_up = 0, g_p1_down = 0;
static volatile int g_p2_up = 0, g_p2_down = 0;

// Elementos del juego
static Ball   g_ball;
static Paddle g_pad1, g_pad2;
static Score  g_score;
static int    g_top, g_bottom, g_left, g_right;
static int    g_midX;

static pthread_t th_ball, th_p1, th_p2;

// Nombres
static char g_name1[NAME_MAXLEN+1] = "Jugador 1";
static char g_name2[NAME_MAXLEN+1] = "CPU";

// Modo de juego actual
static GameMode g_game_mode = MODE_PVP;
static int g_cpu1_delay_counter = 0;     //paleta izquierda
static int g_cpu2_delay_counter = 0;     //paleta derecha

// UTILIDADES
static void clamp_float(float* v, float mn, float mx) {
    if (*v < mn) *v = mn;
    if (*v > mx) *v = mx;
}

static float frand_range(float a, float b) {
    return a + (float)rand() / (float)RAND_MAX * (b - a);
}

static void ball_spawn_random(bool to_right) {
    g_ball.x = (float)((g_left + g_right) / 2);
    g_ball.y = (float)((g_top  + g_bottom) / 2);

    float speed = frand_range(BALL_SPEED_MIN, BALL_SPEED_MAX);

    float angle_y = frand_range(-0.8f, 0.8f);
    float vx = speed * (to_right ? +1.0f : -1.0f);
    float vy = speed * 0.6f * angle_y;

    g_ball.vx = vx;
    g_ball.vy = vy;
}

static void ball_scale_speed(float new_speed) {
    float cur = sqrtf(g_ball.vx * g_ball.vx + g_ball.vy * g_ball.vy);
    if (cur < 1e-6f) {
        g_ball.vx = (g_ball.vx >= 0 ? +1.0f : -1.0f) * new_speed;
        g_ball.vy = 0.0f;
        return;
    }
    float k = new_speed / cur;
    g_ball.vx *= k;
    g_ball.vy *= k;
}

static void versus_screen(void) {
    bool was_nodelay = is_notimeout(stdscr) == FALSE ? false : true;

    int H, W; 
    getmaxyx(stdscr, H, W);

    for (int i = 3; i >= 1; --i) {
        clear();

        attron(COLOR_PAIR(5) | A_BOLD);
        mvprintw(2, (W - (int)strlen("PREPARADOS"))/2, "PREPARADOS");
        attroff(COLOR_PAIR(5) | A_BOLD);

        attron(A_BOLD | COLOR_PAIR(4));
        mvprintw(H/2 - 1, (W - (int)(strlen(g_name1) + 4 + strlen(g_name2)))/2,
                 "%s  VS  %s", g_name1, g_name2);
        attroff(A_BOLD | COLOR_PAIR(4));

        attron(A_BOLD);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", i);
        mvprintw(H/2 + 1, (W - (int)strlen(buf))/2, "%s", buf);
        attroff(A_BOLD);

        const char* hint = "Comenzando...";
        mvprintw(H - 3, (W - (int)strlen(hint))/2, "%s", hint);

        refresh();
        napms(1000); // 1 segundo
    }

    clear();
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(H/2, (W - (int)strlen("¡A JUGAR!"))/2, "¡A JUGAR!");
    attroff(A_BOLD | COLOR_PAIR(3));
    refresh();
    napms(2000); 
}



static void reset_world() {
    int H, W;
    getmaxyx(stdscr, H, W);
    g_pad1.vy = 0.0f;
    g_pad2.vy = 0.0f;

    g_top = 2;
    g_bottom = H - 2;
    g_left = 2;
    g_right = W - 3;
    g_midX = W / 2;

    g_pad1.x = g_left + 2;
    g_pad2.x = g_right - 2;
    g_pad1.y = (g_top + g_bottom) / 2;
    g_pad2.y = (g_top + g_bottom) / 2;

    ball_spawn_random(rand() % 2); 

    g_score.p1 = 0; g_score.p2 = 0;
    g_paused = false;
    g_cpu1_delay_counter = 0;
    g_cpu2_delay_counter = 0;

}

static void draw_borders_and_center() {
    mvaddch(g_top, g_left, '+');
    mvaddch(g_top, g_right, '+');
    mvaddch(g_bottom, g_left, '+');
    mvaddch(g_bottom, g_right, '+');
  
    for (int x = g_left + 1; x < g_right; ++x) {
        mvaddch(g_top, x, '-');
        mvaddch(g_bottom, x, '-');
    }

    for (int y = g_top + 1; y < g_bottom; ++y) {
        mvaddch(y, g_left, '|');
        mvaddch(y, g_right, '|');
    }

    for (int y = g_top + 1; y < g_bottom; y += 2) {
        mvaddch(y, g_midX, ':');
    }
}

static void draw_score() {
    int H, W;
    getmaxyx(stdscr, H, W);
    mvprintw(0, 2, "%s: %d", g_name1, g_score.p1);

    char right_buf[64];
    snprintf(right_buf, sizeof(right_buf), "%s: %d", g_name2, g_score.p2);
    mvprintw(0, W - strlen(right_buf) - 2, "%s", right_buf);

    attron(A_BOLD | COLOR_PAIR(5));
    mvprintw(0, (W - strlen("PONG")) / 2, "%s", "PONG");
    attroff(A_BOLD | COLOR_PAIR(5));

    const char* instr = "(P: pausa, Q: menu)";
    mvprintw(1, (W - (int)strlen(instr)) / 2, "%s", instr);
}

static void draw_paddles_and_ball() {
    int y1 = (int)g_pad1.y;
    attron(COLOR_PAIR(2) | A_BOLD);
    for (int k = -PADDLE_LEN/2; k <= PADDLE_LEN/2; ++k) {
        int yy = y1 + k;
        if (yy > g_top && yy < g_bottom) mvaddch(yy, g_pad1.x, '|');
    }
    attroff(COLOR_PAIR(2) | A_BOLD);

    int y2 = (int)g_pad2.y;
    attron(COLOR_PAIR(3) | A_BOLD);
    for (int k = -PADDLE_LEN/2; k <= PADDLE_LEN/2; ++k) {
        int yy = y2 + k;
        if (yy > g_top && yy < g_bottom) mvaddch(yy, g_pad2.x, '|');
    }
    attroff(COLOR_PAIR(3) | A_BOLD);

    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddch((int)g_ball.y, (int)g_ball.x, 'O');
    attroff(COLOR_PAIR(1) | A_BOLD);
}

void draw_menu_item(int y, int W, const char* text, bool selected) {
    int len = strlen(text);
    int box_w = len + 8; 
    int x = (W - box_w) / 2;
    int inner_w = box_w - 2;
    int left_pad = (inner_w - len) / 2;
    int right_pad = inner_w - len - left_pad;

    if (selected) attron(A_REVERSE);
    mvprintw(y, x, "+-%.*s-+", inner_w, "--------------------------------");
    mvprintw(y+1, x, "|%*s%s%*s|", left_pad, "", text, right_pad, "");
    mvprintw(y+2, x, "+-%.*s-+", inner_w, "--------------------------------");

    if (selected) attroff(A_REVERSE);
}

// LEADERBOARD
static void ensure_file_exists() {
    FILE* f = fopen(LEADERBOARD_FILE, "a");
    if (f) fclose(f);
}

static int cmp_entry(const void* a, const void* b) {
    const Entry* ea = (const Entry*)a;
    const Entry* eb = (const Entry*)b;
    if (eb->winScore != ea->winScore) return eb->winScore - ea->winScore;
    if (eb->ts > ea->ts) return 1;
    if (eb->ts < ea->ts) return -1;
    return 0;
}

static int load_entries(Entry* arr, int maxn) {
    ensure_file_exists();
    FILE* f = fopen(LEADERBOARD_FILE, "r");
    if (!f) return 0;
    int n = 0;
    while (n < maxn) {
        Entry e;
        int scanned = fscanf(f, "%24[^,],%24[^,],%d,%d,%ld\n",
                             e.winner, e.loser, &e.winScore, &e.loseScore, &e.ts);
        if (scanned != 5) break;
        arr[n++] = e;
    }
    fclose(f);
    return n;
}

static void append_entry(const Entry* e) {
    ensure_file_exists();
    FILE* f = fopen(LEADERBOARD_FILE, "a");
    if (!f) return;
    fprintf(f, "%s,%s,%d,%d,%ld\n", e->winner, e->loser, e->winScore, e->loseScore, (long)e->ts);
    fclose(f);
}

// INPUT DE TEXTO
static void read_line_ncurses(char* out, int maxlen, int y, int x) {
    int len = 0;
    out[0] = '\0';
    move(y, x);
    curs_set(1);
    while (1) {
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                out[len] = '\0';
                mvaddch(y, x+len, ' ');
                move(y, x+len);
            }
        } else if (ch >= 32 && ch <= 126) {
            if (len < maxlen) {
                out[len++] = (char)ch;
                out[len] = '\0';
                mvaddch(y, x+len-1, ch);
            }
        }
        refresh();
    }
    curs_set(0);
    if (len == 0) strncpy(out, "Jugador", maxlen);
}

// ============ LÓGICA DE IA PARA CPU ============

// Calcula hacia dónde debe moverse la CPU
static int cpu_calculate_direction(Paddle* cpu_paddle, Ball ball) {
    // Solo reaccionar si la pelota viene hacia la CPU
    bool ball_coming = (cpu_paddle->x > g_midX && ball.vx > 0) || 
                       (cpu_paddle->x < g_midX && ball.vx < 0);
    
    if (!ball_coming) {
        // Volver al centro cuando la pelota no viene hacia nosotros
        float center = (g_top + g_bottom) / 2.0f;
        if (cpu_paddle->y < center - 1.0f) return 1;
        if (cpu_paddle->y > center + 1.0f) return -1;
        return 0;
    }
    
    // Agregar margen de error aleatorio para hacer la CPU más humana
    float target_y = ball.y;
    if (rand() % 100 < 30) { // 30% de chance de error
        target_y += ((rand() % 2) ? 1 : -1) * CPU_ERROR_MARGIN;
    }
    
    // Decidir dirección
    float diff = target_y - cpu_paddle->y;
    if (diff < -0.5f) return -1;
    if (diff > 0.5f) return 1;
    return 0;
}

// ============ LÓGICA DE THREADS ============

static void* thread_ball_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
        auto start = high_resolution_clock::now();
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            g_ball.x += g_ball.vx;
            g_ball.y += g_ball.vy;
            
            if (g_ball.y <= g_top + 1) { 
                g_ball.y = g_top + 1; 
                g_ball.vy *= -1.0f; 
            }
            if (g_ball.y >= g_bottom - 1) { 
                g_ball.y = g_bottom - 1; 
                g_ball.vy *= -1.0f; 
            }
            
            // Colisión con paleta 1
            int y1 = (int)g_pad1.y;
            if ((int)g_ball.x == g_pad1.x + 1 && g_ball.vx < 0) {
                if ((int)g_ball.y >= y1 - PADDLE_LEN/2 && (int)g_ball.y <= y1 + PADDLE_LEN/2) {
                    g_ball.vx *= -1.0f;
                    int dy = (int)g_ball.y - y1;
                    g_ball.vy += 0.15f * dy;
                }
            }
            
            // Colisión con paleta 2
            int y2 = (int)g_pad2.y;
            if ((int)g_ball.x == g_pad2.x - 1 && g_ball.vx > 0) {
                if ((int)g_ball.y >= y2 - PADDLE_LEN/2 && (int)g_ball.y <= y2 + PADDLE_LEN/2) {
                    g_ball.vx *= -1.0f;
                    int dy = (int)g_ball.y - y2;
                    g_ball.vy += 0.15f * dy;
                }
            }
            
            // Puntuación
            if ((int)g_ball.x <= g_left) {
                g_score.p2++;
                ball_spawn_random(true);   // sirve hacia la derecha
            } else if ((int)g_ball.x >= g_right) {
                g_score.p1++;
                ball_spawn_random(false);  // sirve hacia la izquierda
            }

            pthread_mutex_unlock(&g_lock);
        }
        auto end = high_resolution_clock::now();
        time_ball += (end - start);
        usleep(FRAME_USEC_PLAY);
    }
    return NULL;
}

static void move_paddle(Paddle* p, int input_dir) {
    p->vy += input_dir * PADDLE_ACC * PADDLE_DT;

    if (input_dir == 0) {
        if (p->vy > 0) {
            p->vy -= PADDLE_FRICTION * PADDLE_DT;
            if (p->vy < 0) p->vy = 0;
        } else if (p->vy < 0) {
            p->vy += PADDLE_FRICTION * PADDLE_DT;
            if (p->vy > 0) p->vy = 0;
        }
    }

    if (p->vy > PADDLE_MAX_V)  p->vy = PADDLE_MAX_V;
    if (p->vy < -PADDLE_MAX_V) p->vy = -PADDLE_MAX_V;

    p->y += p->vy * PADDLE_DT;

    float minY = g_top + 1 + PADDLE_LEN/2;
    float maxY = g_bottom - 1 - PADDLE_LEN/2;
    if (p->y < minY) { p->y = minY; p->vy = 0; }
    if (p->y > maxY) { p->y = maxY; p->vy = 0; }
}

static void* thread_p1_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
         auto start = high_resolution_clock::now();
    
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            int dir = 0;
            if (g_game_mode == MODE_CVC) {
                // CPU controla paleta 1
                g_cpu1_delay_counter++;
                if (g_cpu1_delay_counter >= CPU_REACTION_DELAY) {
                    dir = cpu_calculate_direction(&g_pad1, g_ball);
                    g_cpu1_delay_counter = 0;
                }
            } else {
                if (g_p1_hold_up   > 0 && g_p1_hold_down == 0) dir = -1;
                else if (g_p1_hold_down > 0 && g_p1_hold_up == 0) dir = +1;
                else dir = 0;
            }
            // update_paddle(...) si usas modelo suave; o move_paddle(...) si usas step fijo
            move_paddle(&g_pad1, dir);
            pthread_mutex_unlock(&g_lock);
        }
        auto end = high_resolution_clock::now();
        time_p1 += (end - start);
        usleep(FRAME_USEC_PLAY);
    }
    return NULL;
}

static void* thread_p2_func(void* arg) {
    (void)arg;
    while (g_threads_should_run) {
        auto start = high_resolution_clock::now();
        if (!g_paused) {
            pthread_mutex_lock(&g_lock);
            int dir = 0;
            if (g_game_mode == MODE_PVC || g_game_mode == MODE_CVC) {
                // CPU controla paleta 2
                g_cpu2_delay_counter++;
                if (g_cpu2_delay_counter >= CPU_REACTION_DELAY) {
                    dir = cpu_calculate_direction(&g_pad2, g_ball);
                    g_cpu2_delay_counter = 0;
                }
            } else {
                if (g_p2_hold_up   > 0 && g_p2_hold_down == 0) dir = -1;
                else if (g_p2_hold_down > 0 && g_p2_hold_up == 0) dir = +1;
                else dir = 0;
            }
            move_paddle(&g_pad2, dir);
            pthread_mutex_unlock(&g_lock);
        }
        auto end = high_resolution_clock::now();
        time_p2 += (end - start);
        usleep(FRAME_USEC_PLAY);
    }
    return NULL;
}


// PANTALLAS
static void input_names_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    clear();
    mvprintw(0, 2, "NOMBRE DEL JUGADOR");
    mvprintw(2, 2, "Ingresa tu nombre:");
    mvprintw(3, 4, "> ");
    refresh();
    read_line_ncurses(g_name1, NAME_MAXLEN, 3, 6);
    
    // El nombre de la CPU se mantiene
    strncpy(g_name2, "CPU", NAME_MAXLEN);
}

static int menu_screen() {
    const char* items[] = {
        "JUGAR",
        "INSTRUCCIONES",
        "PUNTAJES DESTACADOS",
        "SALIR"
    };
    const int N = sizeof(items) / sizeof(items[0]);
    int sel = 0;

    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        int H, W; getmaxyx(stdscr, H, W);

        attron(COLOR_PAIR(5) | A_BOLD);
        mvprintw(2, (W-28)/2, "  ____   ____  _   _  ____ ");
        mvprintw(3, (W-28)/2, " |  _ \\ / __ \\| \\ | |/ ___|");
        mvprintw(4, (W-28)/2, " | |_) | |  | |  \\| | |     ");
        mvprintw(5, (W-28)/2, " |  __/| |  | | . ` | | ___");
        mvprintw(6, (W-28)/2, " | |   | |__| | |\\  | |_| |");
        mvprintw(7, (W-28)/2, " |_|    \\____/|_| \\_|\\____|");
        attroff(COLOR_PAIR(5) | A_BOLD);

        mvprintw(9, (W - strlen("Usa Flechas UP/DOWN y ENTER"))/2, "Usa Flechas UP/DOWN y ENTER");

        for (int i = 0; i < N; ++i) {
            draw_menu_item(11 + i*3, W, items[i], i == sel);
        }

        mvprintw(H-2, (W - strlen("Q para salir rapido"))/2, "Q para salir rapido");

        refresh();
        int ch = getch();
        if (ch == KEY_UP) { sel = (sel - 1 + N) % N; }
        else if (ch == KEY_DOWN) { sel = (sel + 1) % N; }
        else if (ch == '\n' || ch == KEY_ENTER) { return sel; }
        else if (ch == 'q' || ch == 'Q') { return 3; }
    }
}

static int mode_screen() {
    const char* items[] = {
        "JUGADOR VS JUGADOR",
        "JUGADOR VS COMPUTADORA",
        "COMPUTADORA VS COMPUTADORA",
    };
    const int N = sizeof(items) / sizeof(items[0]);
    int sel = 0;

    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        int H, W; getmaxyx(stdscr, H, W);
        
        attron(COLOR_PAIR(4) | A_BOLD);
        mvprintw(2, (W - strlen("SELECCIONA UN MODO DE JUEGO")) / 2, "SELECCIONA UN MODO DE JUEGO");
        attroff(COLOR_PAIR(4) | A_BOLD);
        
        mvprintw(4, (W - strlen("Usa Flechas UP/DOWN y ENTER"))/2, "Usa Flechas UP/DOWN y ENTER");
        
        int start_y = H/2 - (N * 2);
        for (int i = 0; i < N; ++i) {
            draw_menu_item(start_y + i*4, W, items[i], i == sel);
        }
        
        mvprintw(H-2, (W - strlen("Q para volver al menu"))/2, "Q para volver al menu");
        refresh();
        
        int ch = getch();
        if (ch == KEY_UP) { sel = (sel - 1 + N) % N; }
        else if (ch == KEY_DOWN) { sel = (sel + 1) % N; }
        else if (ch == '\n' || ch == KEY_ENTER) { return sel; }
        else if (ch == 'q' || ch == 'Q') { return -1; }
    }
}

static void instructions_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        int H, W;
        getmaxyx(stdscr, H, W);
        
        attron(COLOR_PAIR(4) | A_BOLD);
        mvprintw(2, (W - strlen("INSTRUCCIONES")) / 2, "%s", "INSTRUCCIONES");
        attroff(COLOR_PAIR(4) | A_BOLD);
        
        attron(COLOR_PAIR(3) | A_BOLD);
        mvprintw(5, (W - 40) / 2, "%s", "OBJETIVO");
        attroff(COLOR_PAIR(3) | A_BOLD);
        mvprintw(7, (W - 40) / 2, "Evita que la pelota pase tu borde.");
        mvprintw(8, (W - 40) / 2, "Gana quien llegue a %d puntos.", SCORE_TO_WIN);
        
        attron(COLOR_PAIR(3) | A_BOLD);
        mvprintw(11, (W - 40) / 2, "%s", "CONTROLES");
        attroff(COLOR_PAIR(3) | A_BOLD);
        mvprintw(13, (W - 40) / 2, "Jugador 1:  W (arriba), S (abajo)");
        mvprintw(14, (W - 40) / 2, "Jugador 2:  Flecha UP / DOWN");
        mvprintw(15, (W - 40) / 2, "Globales:  P (pausa), Q (menu)");

        mvprintw(H - 3, (W - strlen("Presiona ENTER para volver al menu")) / 2, "%s", "Presiona ENTER para volver al menu");
        refresh();

        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
    }
}

static void leaderboard_screen() {
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    Entry entries[MAX_LEADER_ENTRIES];
    int n = load_entries(entries, MAX_LEADER_ENTRIES);
    qsort(entries, n, sizeof(Entry), cmp_entry);

    int topN = (n < 10) ? n : 10;

    while (1) {
        clear();
        mvprintw(0, 2, "PUNTAJES DESTACADOS (Top %d)  -  ENTER para volver", topN);
        mvprintw(2, 2, "%-3s %-24s %-6s %-24s %-10s", "#", "Ganador", "Marcador", "Perdedor", "Fecha");
        mvhline(3, 2, '-', 70);
        for (int i = 0; i < topN; ++i) {
            char datebuf[20];
            struct tm* tmv = localtime(&entries[i].ts);
            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", tmv);
            mvprintw(4 + i, 2, "%-3d %-24s %2d-%-3d %-24s %-10s",
                     i+1, entries[i].winner, entries[i].winScore, entries[i].loseScore, entries[i].loser, datebuf);
        }
        if (n == 0) mvprintw(5, 2, "Aun no hay partidas registradas. Juega una y se guardara aqui.");
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
    }
}

static void announce_winner_and_wait(const char* who) {
    int H, W; getmaxyx(stdscr, H, W);
    int cx = W/2;
    attron(A_BOLD);
    mvprintw((H/2)-1, cx - (int)strlen(who)/2, "%s", who);
    attroff(A_BOLD);
    const char* msg = "(ENTER) Reiniciar   (Q) Menu";
    mvprintw((H/2)+1, cx - (int)strlen(msg)/2, "%s", msg);
    refresh();
}

static Scene play_screen() {
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    timeout(0);
    reset_world();
    versus_screen();
    g_threads_should_run = true;
    pthread_create(&th_ball, NULL, thread_ball_func, NULL);
    pthread_create(&th_p1, NULL, thread_p1_func, NULL);
    pthread_create(&th_p2, NULL, thread_p2_func, NULL);

    Scene next = SC_MENU;
    while (!g_exit_requested) {
        // reset “flags instantáneas”
        g_p1_up = g_p1_down = g_p2_up = g_p2_down = 0;

        int ch;
        while ((ch = getch()) != ERR) {
            switch (ch) {
                case 'q': case 'Q': next = SC_MENU; goto END_PLAY;
                case 'p': case 'P': g_paused = !g_paused; break;

                // J1
                case 'w': case 'W':
                    g_p1_up = 1;
                    g_p1_hold_up   = HOLD_FRAMES;
                    g_p1_hold_down = 0;
                    break;
                case 's': case 'S':
                    g_p1_down = 1;
                    g_p1_hold_down = HOLD_FRAMES;
                    g_p1_hold_up   = 0;
                    break;

                // J2
                case KEY_UP:
                    g_p2_up = 1;
                    g_p2_hold_up   = HOLD_FRAMES;
                    g_p2_hold_down = 0;
                    break;
                case KEY_DOWN:
                    g_p2_down = 1;
                    g_p2_hold_down = HOLD_FRAMES;
                    g_p2_hold_up   = 0;
                    break;
            }
        }

        // “decay” por frame
        if (g_p1_hold_up   > 0) g_p1_hold_up--;
        if (g_p1_hold_down > 0) g_p1_hold_down--;
        if (g_p2_hold_up   > 0) g_p2_hold_up--;
        if (g_p2_hold_down > 0) g_p2_hold_down--;


        pthread_mutex_lock(&g_lock);
        clear();
        auto start = high_resolution_clock::now();
        draw_score();
        draw_borders_and_center();
        draw_paddles_and_ball();
        auto end = high_resolution_clock::now();
        time_render += (end - start);

        if (g_paused) {
            attron(A_BOLD);
            mvprintw((g_top + g_bottom)/2, g_midX - 2, "PAUSA");
            attroff(A_BOLD);
        }

        if (g_score.p1 >= SCORE_TO_WIN || g_score.p2 >= SCORE_TO_WIN) {
            const bool p1win = g_score.p1 > g_score.p2;
            const char* who = p1win ? "Gana JUGADOR 1" : "Gana JUGADOR 2";
            announce_winner_and_wait(who);
            pthread_mutex_unlock(&g_lock);

            // Guardar en leaderboard
            Entry e = {0};
            strncpy(e.winner, p1win ? g_name1 : g_name2, NAME_MAXLEN);
            strncpy(e.loser,  p1win ? g_name2 : g_name1, NAME_MAXLEN);
            e.winScore = p1win ? g_score.p1 : g_score.p2;
            e.loseScore = p1win ? g_score.p2 : g_score.p1;
            e.ts = time(NULL);
            append_entry(&e);

            nodelay(stdscr, FALSE);
            int c;
            while ((c = getch())) {
                if (c == 'q' || c == 'Q') { next = SC_MENU; goto END_PLAY; }
                if (c == '\n' || c == KEY_ENTER) {
                    nodelay(stdscr, TRUE);
                    reset_world();
                    break;
                }
            }
        } else {
            refresh();
            pthread_mutex_unlock(&g_lock);
        }
        usleep(FRAME_USEC_PLAY);
    }

END_PLAY:
    g_threads_should_run = false;
    usleep(2 * FRAME_USEC_PLAY);
    pthread_join(th_ball, NULL);
    pthread_join(th_p1, NULL);
    pthread_join(th_p2, NULL);
    return next;
}

// MAIN
int main(void) {
    srand((unsigned int)time(NULL));
    initscr();

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN,   -1);
        init_pair(2, COLOR_RED, -1);
        init_pair(3, COLOR_BLUE,  -1);
        init_pair(4, COLOR_YELLOW,-1);
        init_pair(5, COLOR_MAGENTA,-1);
        init_pair(6, COLOR_WHITE, COLOR_BLACK);
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    Scene scene = SC_MENU;
    while (!g_exit_requested) {
        if (scene == SC_MENU) {
            auto start = std::chrono::high_resolution_clock::now();
            int sel = menu_screen();   // << medir menú
            auto end = std::chrono::high_resolution_clock::now();
            time_menu += (end - start);

            if (sel == 0) {
                int mode = mode_screen();
                if (mode == -1) {
                    // Usuario presionó Q, volver al menú
                    scene = SC_MENU;
                } else if (mode == 0) {
                    // Jugador vs Jugador
                    g_game_mode = MODE_PVP;
                    strncpy(g_name1, "Jugador 1", NAME_MAXLEN);
                    strncpy(g_name2, "Jugador 2", NAME_MAXLEN);
                    scene = SC_PLAYING;
                } else if (mode == 1) {
                    // Jugador vs Computadora
                    g_game_mode = MODE_PVC;

                    auto s1 = std::chrono::high_resolution_clock::now();
                    input_names_screen();   // << medir input nombres
                    auto e1 = std::chrono::high_resolution_clock::now();
                    time_menu += (e1 - s1);

                    strncpy(g_name2, "CPU", NAME_MAXLEN);
                    scene = SC_PLAYING;
                } else if (mode == 2) {
                    // Computadora vs Computadora
                    g_game_mode = MODE_CVC;
                    strncpy(g_name1, "CPU 1", NAME_MAXLEN);
                    strncpy(g_name2, "CPU 2", NAME_MAXLEN);
                    scene = SC_PLAYING;
                }
            }
            else if (sel == 1) { 
                auto s = std::chrono::high_resolution_clock::now();
                instructions_screen();   // << medir instrucciones
                auto e = std::chrono::high_resolution_clock::now();
                time_instructions += (e - s);

                scene = SC_MENU; 
            }
            else if (sel == 2) { 
                auto s = std::chrono::high_resolution_clock::now();
                leaderboard_screen();   // << medir leaderboard
                auto e = std::chrono::high_resolution_clock::now();
                time_leaderboard += (e - s);

                scene = SC_MENU; 
            }
            else if (sel == 3) { 
                g_exit_requested = true; 
            }
        } 
        else if (scene == SC_PLAYING) {
            scene = play_screen();  
        } 
        else if (scene == SC_INSTR) {
            auto s = std::chrono::high_resolution_clock::now();
            instructions_screen();
            auto e = std::chrono::high_resolution_clock::now();
            time_instructions += (e - s);

            scene = SC_MENU;
        } 
        else if (scene == SC_LEADER) {
            auto s = std::chrono::high_resolution_clock::now();
            leaderboard_screen();
            auto e = std::chrono::high_resolution_clock::now();
            time_leaderboard += (e - s);
            scene = SC_MENU;
        }
    }

    endwin();
        // ---- PERFIL FINAL ----
    double total = time_ball.count() + time_p1.count() + time_p2.count() + 
                   time_menu.count() + time_instructions.count() +
                   time_leaderboard.count() + time_render.count();

    printf("\n--- PERFIL DE TIEMPOS ---\n");
    printf("Bola: %.4f s (%.1f%%)\n", time_ball.count(), 100 * time_ball.count() / total);
    printf("Paleta 1: %.4f s (%.1f%%)\n", time_p1.count(), 100 * time_p1.count() / total);
    printf("Paleta 2: %.4f s (%.1f%%)\n", time_p2.count(), 100 * time_p2.count() / total);
    printf("Menu: %.4f s (%.1f%%)\n", time_menu.count(), 100 * time_menu.count() / total);
    printf("Instrucciones: %.4f s (%.1f%%)\n", time_instructions.count(), 100 * time_instructions.count() / total);
    printf("Leaderboard: %.4f s (%.1f%%)\n", time_leaderboard.count(), 100 * time_leaderboard.count() / total);
    printf("Renderizado: %.4f s (%.1f%%)\n", time_render.count(), 100 * time_render.count() / total);
    printf("Tiempo total medido: %.4f s\n", total);

    double T_seq = time_menu.count() + time_instructions.count() + time_leaderboard.count() + time_render.count();
    double T_par = time_ball.count() + time_p1.count() + time_p2.count();
    double f_seq = T_seq / total;
    double f_par = T_par / total;

    printf("T_seq: %.4f s\n", T_seq);
    printf("T_par: %.4f s\n", T_par);
    printf("f_seq: %.4f\n", f_seq);
    printf("f_par: %.4f\n", f_par);
    return 0;
}

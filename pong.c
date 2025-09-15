// PONG
// CC3086 - Programación de microprocesadores
// Requiere: ncurses y pthreads
// Compilar: gcc pong_phase2.c -o pong_phase2 -lncurses -lpthread

#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>

// CONFIGURACIÓN 
#define TARGET_FPS_PLAY 60
#define FRAME_USEC_PLAY (1000000 / TARGET_FPS_PLAY)

#define PADDLE_LEN 5
#define PADDLE_SPEED 1         // celdas por frame de render
#define BALL_SPEED_X 0.8f
#define BALL_SPEED_Y 0.4f
#define SCORE_TO_WIN 7

// ESTADO GLOBAL 
typedef struct {
    float x, y;
    float vx, vy;
} Ball;

typedef struct {
    int x;
    float y;
} Paddle;

typedef struct {
    int p1, p2;
} Score;

// Escena
typedef enum {
    SC_MENU = 0,
    SC_INSTR,
    SC_PLAYING
} Scene;

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
static int    g_top, g_bottom, g_left, g_right; // límites del campo útil (sin la línea de score)
static int    g_midX;

static pthread_t th_ball, th_p1, th_p2;

// UTILIDADES 
static void clamp_float(float* v, float mn, float mx) {
    if (*v < mn) *v = mn;
    if (*v > mx) *v = mx;
}
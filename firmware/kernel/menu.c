#include "kernel.h"

/* The Enaga menu (docs/design.md decisions 2 and 3, "Boot and app switching"): the installed apps, then STATUS
 * (Enaga's status page). F1 + POWER opens it over a running app, which is paused meanwhile (no events, update or
 * draw; its audio goes on), and closes it again; CLR closes it too. UP / DOWN or VALUE move, OK chooses: the
 * running app resumes, another replaces it, STATUS shows the status page until OK or CLR.
 *
 * With no app running the menu cannot close: the screen is the menu, or the status page (after a crash, or when
 * the card holds no apps), from which OK opens the menu. */
#define MENU_APPS   24
#define MENU_ROWS   10                        /* list lines on screen, 9 px apart from y = 26 */

int menu_active;                              /* 1: the menu (or its status page) has the screen and the inputs */
int menu_status;                              /* 1: showing the status page */
static char menu_names[MENU_APPS][APPS_FOLDER];
static int menu_count, menu_cursor, menu_top;
static char menu_message[40];

static void copy(char *dst, unsigned size, const char *src)
{
    unsigned i;
    for (i = 0; i + 1 < size && src && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void menu_open(const char *why)
{
    int i;
    menu_count = apps_scan(menu_names, MENU_APPS);
    if (menu_count < 0) menu_count = 0;
    copy(menu_message, sizeof menu_message, why);
    menu_cursor = 0;
    for (i = 0; i < menu_count; i++) if (app_current && same(menu_names[i], app_folder)) menu_cursor = i;
    menu_top = menu_cursor >= MENU_ROWS ? menu_cursor - MENU_ROWS + 1 : 0;
    menu_status = 0;
    menu_active = 1;
}

void menu_close(void)                         /* only over a running app */
{
    if (app_current) menu_active = menu_status = 0;
}

static void move(int by)
{
    menu_cursor += by;
    if (menu_cursor < 0) menu_cursor = 0;
    if (menu_cursor > menu_count) menu_cursor = menu_count;       /* menu_count = the STATUS line */
    if (menu_cursor < menu_top) menu_top = menu_cursor;
    if (menu_cursor >= menu_top + MENU_ROWS) menu_top = menu_cursor - MENU_ROWS + 1;
}

static void choose(void)
{
    if (menu_cursor == menu_count) { menu_status = 1; return; }
    if (app_current && same(menu_names[menu_cursor], app_folder)) { menu_close(); return; }
    if (apps_start(menu_names[menu_cursor]) == 0) { menu_active = menu_status = 0; return; }
    copy(menu_message, sizeof menu_message, app_status);   /* why it did not start; the menu stays */
}

void menu_event(const enaga_event_t *e)
{
    if (e->type == ENAGA_EVENT_ENCODER && e->id == 4) { if (!menu_status) move(e->value); return; }
    if (e->type != ENAGA_EVENT_KEY || !e->value) return;
    if (menu_status) {
        if (e->id == ENAGA_KEY_OK || e->id == ENAGA_KEY_CLR) menu_status = 0;
        return;
    }
    switch (e->id) {
    case ENAGA_KEY_UP: move(-1); break;
    case ENAGA_KEY_DOWN: move(1); break;
    case ENAGA_KEY_OK: choose(); break;
    case ENAGA_KEY_CLR: menu_close(); break;
    default: break;
    }
}

void menu_draw(void)
{
    int i, y;
    gfx_clear(0);
    gfx_text(0, 0, "ENAGA", 15);
    gfx_text(36, 0, ENAGA_VERSION, 6);
    if (menu_message[0]) gfx_text(0, 12, menu_message, 12);
    else if (menu_count == 0) gfx_text(0, 12, "NO APPS", 12);
    for (i = menu_top, y = 26; i <= menu_count && i < menu_top + MENU_ROWS; i++, y += 9) {
        int running = i < menu_count && app_current && same(menu_names[i], app_folder);
        char shown[19];                       /* 18 characters: room for the running mark */
        if (i == menu_cursor) gfx_text(0, y, ">", 15);
        copy(shown, sizeof shown, i < menu_count ? menu_names[i] : "STATUS");
        gfx_text(8, y, shown, i == menu_cursor ? 15 : 8);
        if (running) gfx_text(116, y, "ON", 12);
    }
}

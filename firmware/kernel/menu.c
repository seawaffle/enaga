#include "kernel.h"

/* The Enaga menu (docs/design.md decisions 2 and 3, "Boot and app switching"): the installed apps by their display
 * names (from each package's header; the folder name if it cannot be read), then SETTINGS (settings.c) and STATUS
 * (Enaga's status page). F1 + POWER opens it over a running app, which is paused meanwhile (no events, update or
 * draw; its audio goes on), and closes it again; CLR closes it too. An app can open it itself (open_menu). UP /
 * DOWN or VALUE move, OK chooses: the running app resumes, another replaces it, SETTINGS and STATUS show their
 * pages until OK or CLR. USB DRIVE (below) is the third built-in line. Each line has an icon: the package's (ENAGA_PACKAGE_ICON), else a generic one; SETTINGS
 * and STATUS have their own. Grey 0 in an icon shows the line's background.
 *
 * With no app running the menu cannot close: the screen is the menu, or the status page (after a crash, or when
 * the card holds no apps), from which OK opens the menu.
 *
 * USB DRIVE (docs/design.md "E5 plan", step 3): the SD card as a USB drive. Opening it stops the running app (its
 * stop() and file closing run as for a switch), unmounts the card and shares it over USB (usb.c); the page then
 * shows the computer's progress. It ends when the computer ejects the drive, when the cable is pulled after the
 * computer had it, or on OK / CLR (the screen says to eject first: the computer may still hold writes). Then the card
 * is mounted again and the menu reopens, rescanned, so apps copied over appear. The app is not restarted. */
#define MENU_APPS   24
#define MENU_ROWS   5                         /* list lines on screen, MENU_LINE px apart from y = MENU_TOP */
#define MENU_TOP    24
#define MENU_LINE   20

int menu_active;                              /* 1: the menu (or its status page) has the screen and the inputs */
int menu_status;                              /* 1: showing the status page */
static int menu_settings;                     /* 1: showing the settings page */
int menu_drive;                               /* 1: showing the USB DRIVE page, the card shared */
static int drive_host_seen;                   /* a host had configured the device while the page was open */
static int drive_stopping;                    /* OK pressed: ends once the computer has stopped writing */
static u32 drive_written_seen, drive_write_ms;   /* the write count last seen, and when it last changed */
#define DRIVE_QUIET_MS 2000                   /* macOS and Linux cache writes and send them a moment later */
static char menu_names[MENU_APPS][APPS_FOLDER];
char menu_titles[MENU_APPS][ENAGA_NAME_BYTES];
static u8 menu_icons[MENU_APPS][ENAGA_ICON_BYTES];
static int menu_has_icon[MENU_APPS];
static int menu_count, menu_cursor, menu_top;   /* lines: the apps, then SETTINGS (menu_count), USB DRIVE, STATUS */
#define LINE_SETTINGS (menu_count)
#define LINE_DRIVE    (menu_count + 1)
#define LINE_STATUS   (menu_count + 2)
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
    for (i = 0; i < menu_count; i++)
        if (apps_title(menu_names[i], menu_titles[i], menu_icons[i], &menu_has_icon[i]) != 0 || !menu_titles[i][0])
            copy(menu_titles[i], sizeof menu_titles[i], menu_names[i]);
    copy(menu_message, sizeof menu_message, why);
    menu_cursor = 0;
    for (i = 0; i < menu_count; i++) if (app_current && same(menu_names[i], app_folder)) menu_cursor = i;
    menu_top = menu_cursor >= MENU_ROWS ? menu_cursor - MENU_ROWS + 1 : 0;
    menu_status = menu_settings = menu_drive = 0;
    menu_active = 1;
}

void menu_close(void)                         /* only over a running app */
{
    if (app_current) menu_active = menu_status = menu_settings = 0;
}

static void move(int by)
{
    menu_cursor += by;
    if (menu_cursor < 0) menu_cursor = 0;
    if (menu_cursor > LINE_STATUS) menu_cursor = LINE_STATUS;
    if (menu_cursor < menu_top) menu_top = menu_cursor;
    if (menu_cursor >= menu_top + MENU_ROWS) menu_top = menu_cursor - MENU_ROWS + 1;
}

static void drive_open(void)
{
    if (!sd_present()) { copy(menu_message, sizeof menu_message, "NO CARD"); return; }
    if (app_current) apps_unload();           /* stop() and its files first, as for a switch */
    files_release();
    files_log_card();                         /* the size the computer will be given, beside USB DRIVE ON */
    usb_drive_read_sectors = usb_drive_written_sectors = 0;
    usb_drive_share(1);
    drive_host_seen = drive_stopping = 0;
    drive_written_seen = 0;
    menu_drive = 1;
    log_line("USB DRIVE ON");
}

static void drive_close(const char *why)
{
    usb_drive_share(0);
    log_line(why);
    if (enaga_musb_tx_busy) {                 /* how often a TX interrupt found the FIFO full (dcd_musb.c) */
        char line[24] = "USB TX BUSY ";
        int n = 12, k = 0;
        char digits[10];
        u32 v = enaga_musb_tx_busy;
        do { digits[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
        while (k) line[n++] = digits[--k];
        line[n] = 0;
        log_line(line);
    }
    apps_mount();                             /* the card back, as at start-up */
    menu_open(why);                           /* rescanned: apps copied over are listed */
}

void menu_service(u32 ms)
{
    if (!menu_drive) return;
    if (usb_configured()) drive_host_seen = 1;
    if (usb_drive_written_sectors != drive_written_seen) {
        drive_written_seen = usb_drive_written_sectors;
        drive_write_ms = ms;
    }
    if (usb_drive_ejected()) drive_close("USB DRIVE EJECTED");
    else if (drive_host_seen && !usb_configured()) drive_close("USB UNPLUGGED");
    else if (drive_stopping && (!drive_written_seen || ms - drive_write_ms >= DRIVE_QUIET_MS))
        drive_close("USB DRIVE ENDED");
}

static void choose(void)
{
    if (menu_cursor == LINE_SETTINGS) { menu_settings = 1; settings_open(); return; }
    if (menu_cursor == LINE_DRIVE) { drive_open(); return; }
    if (menu_cursor == LINE_STATUS) { menu_status = 1; status_scroll = 0; return; }
    if (app_current && same(menu_names[menu_cursor], app_folder)) { menu_close(); return; }
    if (apps_start(menu_names[menu_cursor]) == 0) { menu_active = menu_status = 0; return; }
    copy(menu_message, sizeof menu_message, app_status);   /* why it did not start; the menu stays */
}

void menu_event(const enaga_event_t *e)
{
    if (menu_settings) { if (settings_event(e)) menu_settings = 0; return; }
    if (menu_drive) {
        if (e->type == ENAGA_EVENT_KEY && e->value && (e->id == ENAGA_KEY_OK || e->id == ENAGA_KEY_CLR)) {
            if (drive_stopping) drive_close("USB DRIVE ENDED");   /* pressed again: now, writes or not */
            else drive_stopping = 1;
        }
        return;
    }
    if (e->type == ENAGA_EVENT_ENCODER && e->id == 4) {
        if (!menu_status) move(e->value);
        else status_scroll -= e->value;           /* anticlockwise: further back in the log */
        return;
    }
    if (e->type != ENAGA_EVENT_KEY || !e->value) return;
    if (menu_status) {
        if (e->id == ENAGA_KEY_OK || e->id == ENAGA_KEY_CLR) menu_status = 0;
        if (e->id == ENAGA_KEY_UP) status_scroll++;
        if (e->id == ENAGA_KEY_DOWN) status_scroll--;
        if (e->id == ENAGA_KEY_LEFT || e->id == ENAGA_KEY_RIGHT) {   /* USB audio IN's feeding, for the unit tests */
            char line[] = "USB IN MODE 0";
            usb_audio_in_mode = (usb_audio_in_mode + (e->id == ENAGA_KEY_RIGHT ? 1 : 3)) & 3;
            line[12] = (char)('0' + usb_audio_in_mode);
            log_line(line);
        }
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

/* Built-in icons, as icon.txt spells them (tools/build_app.py): a grey per character, "." for 0. */
static const char *const icon_generic[ENAGA_ICON_SIZE] = {
    "................", "..9999....9999..", ".933339..933339.", ".933339..933339.",
    ".933339..933339.", ".933339..933339.", "..9999....9999..", "................",
    "................", "..9999....9999..", ".933339..933339.", ".933339..933339.",
    ".933339..933339.", ".933339..933339.", "..9999....9999..", "................",
};
static const char *const icon_settings[ENAGA_ICON_SIZE] = {
    "................", "...5....5....5..", "...5....5....5..", "...5...FFF...5..",
    "...5...FFF...5..", "...5....5....5..", "...5....5...FFF.", "...5....5...FFF.",
    "...5....5....5..", "..FFF...5....5..", "..FFF...5....5..", "...5....5....5..",
    "...5....5....5..", "...5....5....5..", "...5....5....5..", "................",
};
static const char *const icon_status[ENAGA_ICON_SIZE] = {
    "................", "....99999999....", "...99......99...", "..99........99..",
    ".99....FF....99.", ".9.....FF.....9.", ".9............9.", ".9....FFF.....9.",
    ".9.....FF.....9.", ".9.....FF.....9.", ".9.....FF.....9.", ".99...FFFF...99.",
    "..99........99..", "...99......99...", "....99999999....", "................",
};

static void draw_builtin(int x, int y, const char *const rows[ENAGA_ICON_SIZE])
{
    int r, c;
    for (r = 0; r < ENAGA_ICON_SIZE; r++)
        for (c = 0; c < ENAGA_ICON_SIZE; c++) {
            char ch = rows[r][c];
            u8 grey = (u8)(ch >= 'A' ? ch - 'A' + 10 : ch >= '0' ? ch - '0' : 0);
            if (grey) gfx_pixel(x + c, y + r, grey);
        }
}

static void draw_icon(int x, int y, const u8 *icon)   /* ENAGA_ICON_BYTES: two pixels a byte, the left one high */
{
    int i;
    for (i = 0; i < ENAGA_ICON_SIZE * ENAGA_ICON_SIZE; i++) {
        u8 grey = (u8)(i & 1 ? icon[i / 2] & 0xF : icon[i / 2] >> 4);
        if (grey) gfx_pixel(x + i % ENAGA_ICON_SIZE, y + i / ENAGA_ICON_SIZE, grey);
    }
}

static void fill(int x, int y, int w, int h, u8 grey)
{
    int i, j;
    for (j = y; j < y + h; j++) for (i = x; i < x + w; i++) gfx_pixel(i, j, grey);
}

/* The frame <enaga/ui.hpp> gives apps (status_line, hint_line, List's cursor), so Enaga's pages look like theirs. */
void ui_title(const char *title)
{
    gfx_text(0, 0, title, 15);
    fill(0, 9, CANVAS_W, 1, 3);
}
void ui_hint(const char *text, int row) { gfx_text(0, CANVAS_H - 8 - 10 * row, text, 6); }
void ui_cursor_bar(int y, int h) { fill(0, y, CANVAS_W - 4, h, 3); }

/* On batteries, a battery at the top right with one bar per level (power.c); nothing on USB or adapter power. */
static void draw_battery(int x, int y)
{
    int level = battery_level(), i;
    if (level < 0) return;
    fill(x, y, 14, 1, 10); fill(x, y + 6, 14, 1, 10);           /* the outline, 14 x 7, and its tip */
    fill(x, y, 1, 7, 10); fill(x + 13, y, 1, 7, 10);
    fill(x + 14, y + 2, 2, 3, 10);
    for (i = 0; i < level; i++) fill(x + 2 + 3 * i, y + 2, 2, 3, 12);   /* bars 2 wide, 1 apart */
}

static const char *const icon_drive[ENAGA_ICON_SIZE] = {
    "................", "................", ".....99999999...", ".....9F9F9F99...",
    ".....9F9F9F99...", ".....99999999...", "....9999999999..", "....9333333339..",
    "....9333333339..", "....9333333339..", "....9333333339..", "....9333333339..",
    "....9333333339..", "....9999999999..", "................", "................",
};

static void kib(char *out, const char *label, u32 sectors)   /* "READ 1234 KB" */
{
    char digits[12];
    u32 v = sectors / 2u, n = 0, i = 0;
    do { digits[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (*label) out[i++] = *label++;
    while (n) out[i++] = digits[--n];
    out[i++] = ' '; out[i++] = 'K'; out[i++] = 'B'; out[i] = 0;
}

static void drive_draw(void)
{
    char line[32];
    gfx_clear(0);
    ui_title("USB DRIVE");
    draw_battery(CANVAS_W - 16, 0);
    if (!usb_configured()) gfx_text(0, 20, "CONNECT A USB CABLE", 12);
    else if (!usb_drive_read_sectors) gfx_text(0, 20, "WAITING FOR THE", 12);
    else gfx_text(0, 20, "THE COMPUTER HAS IT", 12);
    if (usb_configured() && !usb_drive_read_sectors) gfx_text(0, 29, "COMPUTER", 12);
    kib(line, "READ ", usb_drive_read_sectors);
    gfx_text(0, 48, line, 10);
    kib(line, "WRITTEN ", usb_drive_written_sectors);
    gfx_text(0, 57, line, 10);
    if (usb_drive_errors) gfx_text(0, 66, "CARD ERRORS", 12);
    if (drive_stopping) {
        gfx_text(0, 84, "WAITING FOR THE", 12);
        gfx_text(0, 93, "WRITES TO FINISH", 12);
        ui_hint("OK AGAIN: STOP NOW", 0);
    } else {
        gfx_text(0, 84, "EJECT IT ON THE", 8);
        gfx_text(0, 93, "COMPUTER TO FINISH", 8);
        ui_hint("OK: STOP", 0);
    }
}

void menu_draw(void)
{
    int i, y;
    if (menu_settings) { settings_draw(); return; }
    if (menu_drive) { drive_draw(); return; }
    gfx_clear(0);
    ui_title("ENAGA");
    gfx_text(36, 0, ENAGA_VERSION, 6);
    draw_battery(CANVAS_W - 16, 0);
    if (menu_message[0]) gfx_text(0, 12, menu_message, 12);
    else if (menu_count == 0) gfx_text(0, 12, "NO APPS", 12);
    for (i = menu_top, y = MENU_TOP; i <= LINE_STATUS && i < menu_top + MENU_ROWS; i++, y += MENU_LINE) {
        int running = i < menu_count && app_current && same(menu_names[i], app_folder);
        char shown[16];                       /* 15 characters: from x = 22 to the running mark */
        if (i == menu_cursor) ui_cursor_bar(y, MENU_LINE - 1);
        if (i == LINE_SETTINGS) draw_builtin(1, y + 1, icon_settings);
        else if (i == LINE_DRIVE) draw_builtin(1, y + 1, icon_drive);
        else if (i == LINE_STATUS) draw_builtin(1, y + 1, icon_status);
        else if (menu_has_icon[i]) draw_icon(1, y + 1, menu_icons[i]);
        else draw_builtin(1, y + 1, icon_generic);
        copy(shown, sizeof shown, i < menu_count ? menu_titles[i] : i == LINE_SETTINGS ? "SETTINGS"
                                  : i == LINE_DRIVE ? "USB DRIVE" : "STATUS");
        gfx_text(22, y + 6, shown, i == menu_cursor ? 15 : 8);
        if (running) gfx_text(112, y + 6, "ON", 12);
    }
    if (menu_top > 0) fill(CANVAS_W - 2, MENU_TOP, 2, 3, 8);                                  /* more above */
    if (menu_top + MENU_ROWS < LINE_STATUS + 1) fill(CANVAS_W - 2, MENU_TOP + MENU_ROWS * MENU_LINE - 4, 2, 3, 8);
}

/*
 * FlipperPet - a Tamagotchi-style companion for the Flipper Zero.
 *
 * Now with:
 *   - An intro: choose one of three pets (Pup / Kit / Byte) and a gender
 *   - Real animations: a hand that pets your pet's head, food that drops and
 *     gets chomped, play bounces, and RF sense rings
 *   - An "Adore" screen where your pet idles and says cute things
 *   - Real Sub-GHz RSSI sensing (the pet reacts to actual RF in the air)
 *   - NFC / RFID hooks (see flipperpet_radio.c)
 *
 * Build with ufbt (see README.md).
 */

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include "flipperpet_radio.h"

#define SAVE_DIR "/ext/apps_data/flipperpet"
#define SAVE_PATH SAVE_DIR "/save.dat"
#define SAVE_MAGIC 0xF1AA
#define UPGRADE_COUNT 5
#define MAIN_COUNT 13

typedef enum {
    ScreenIntroSpecies,
    ScreenIntroGender,
    ScreenMain,
    ScreenStatus,
    ScreenAdore,
    ScreenFeed,
    ScreenPet,
    ScreenPlay,
    ScreenSubghz,
    ScreenNfc,
    ScreenRfid,
    ScreenIr,
    ScreenBadusb,
    ScreenStats,
    ScreenShop,
    ScreenSettings,
} Screen;

typedef enum { AnimNone, AnimFeed, AnimPet, AnimPlay, AnimSense } Anim;
static const uint8_t anim_len[] = {0, 12, 18, 18, 18};

/* A radio scan requested from the UI, run later without the mutex held. */
enum { RadioNone, RadioNfc, RadioRfid };

/* Serialized to SD card. */
typedef struct {
    uint16_t magic;
    uint8_t created;
    uint8_t species; /* 0 Pup, 1 Kit, 2 Byte */
    uint8_t gender; /* 0 male, 1 female */
    uint8_t level;
    uint16_t xp;
    uint16_t xp_next;
    uint16_t streak;
    uint16_t best;
    uint8_t task_done;
    int8_t hp;
    int8_t hunger;
    int8_t happy;
    uint16_t sparks;
    uint8_t owned[UPGRADE_COUNT];
} PetData;

typedef struct {
    PetData d;
    Screen screen;
    Screen stack[8];
    uint8_t depth;
    uint8_t sel;
    uint8_t tmp_species;
    char toast[28];
    uint32_t toast_until;
    uint32_t frame;
    bool blink;
    Anim anim;
    uint8_t anim_t;
    uint8_t adore_hearts;
    uint8_t pending_radio;
    FuriMutex* mutex;
    ViewPort* view_port;
    Gui* gui;
    FuriMessageQueue* queue;
    FuriTimer* timer;
} App;

/* ---- Menu data ---- */

static const char* const main_items[MAIN_COUNT] = {
    "Status", "Adore", "Feed", "Pet", "Play", "Sub-GHz", "NFC",
    "RFID", "Infrared", "BadUSB", "Stats", "Shop", "Settings"};

static const Screen main_targets[MAIN_COUNT] = {
    ScreenStatus, ScreenAdore, ScreenFeed, ScreenPet, ScreenPlay, ScreenSubghz,
    ScreenNfc, ScreenRfid, ScreenIr, ScreenBadusb, ScreenStats, ScreenShop, ScreenSettings};

static const char* const feed_items[4] = {"Apple", "Full meal", "Battery snack", "Energy drink"};
static const char* const pet_items[4] = {"Head pat", "Belly rub", "Boop nose", "Scratch chin"};
static const char* const play_items[4] = {"Fetch signal", "Hide & seek", "Dance off", "Laser chase"};
static const char* const subghz_items[3] = {"Sense 433MHz", "Sense 315MHz", "Sense 868MHz"};
static const char* const nfc_items[3] = {"Detect tag", "Read UID", "Emulate"};
static const char* const rfid_items[3] = {"Detect tag", "Read EM4100", "Pulse field"};
static const char* const ir_items[3] = {"Detect signal", "Universal", "Learn new"};
static const char* const badusb_items[3] = {"Rickroll", "Spin", "Confetti"};
static const char* const settings_items[4] = {"Sound: ON", "Vibro: ON", "Reset pet", "About"};

static const char* const shop_names[UPGRADE_COUNT] = {
    "Sub-GHz Radar", "NFC Feeder", "Mood Engine", "Signal Diary", "BadUSB Tricks"};
static const uint16_t shop_cost[UPGRADE_COUNT] = {200, 250, 350, 150, 500};
static const uint8_t shop_req[UPGRADE_COUNT] = {3, 5, 7, 2, 10};

/* 8x8 menu glyphs; bit 7 (0x80) is the leftmost pixel. */
static const uint8_t glyphs[MAIN_COUNT][8] = {
    {0x00, 0x66, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00}, /* heart   Status   */
    {0x10, 0x10, 0x54, 0x38, 0xFE, 0x38, 0x54, 0x10}, /* sparkle Adore    */
    {0x00, 0x24, 0x42, 0x00, 0xFF, 0x7E, 0x3C, 0x00}, /* bowl    Feed     */
    {0x14, 0x15, 0x15, 0x77, 0x7F, 0x3E, 0x3C, 0x00}, /* hand    Pet      */
    {0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C}, /* face    Play     */
    {0x18, 0x18, 0x18, 0x5A, 0x3C, 0x18, 0x18, 0x3C}, /* antenna Sub-GHz  */
    {0x00, 0xFE, 0x82, 0xBA, 0x82, 0xFE, 0x00, 0x00}, /* card    NFC      */
    {0x02, 0x12, 0x22, 0x42, 0x42, 0x22, 0x12, 0x02}, /* waves   RFID     */
    {0x18, 0x24, 0x24, 0x24, 0x18, 0x18, 0x18, 0x00}, /* bulb    Infrared */
    {0x18, 0x18, 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x3C}, /* plug    BadUSB   */
    {0x00, 0x02, 0x0A, 0x2A, 0xAA, 0xAA, 0xAA, 0x00}, /* bars    Stats    */
    {0x24, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00}, /* bag     Shop     */
    {0x18, 0x5A, 0x3C, 0x66, 0x66, 0x3C, 0x5A, 0x18}, /* gear    Settings */
};

/* Small overlay sprites (<=8px wide), bit 7 leftmost. */
static const uint8_t spr_hand[9] = {0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x18, 0x18, 0x18, 0x18};
static const uint8_t spr_bone[5] = {0xC3, 0xE7, 0x7E, 0xE7, 0xC3};
static const uint8_t spr_ball[6] = {0x78, 0xFC, 0xFC, 0xFC, 0xFC, 0x78};
static const uint8_t spr_heart[5] = {0x68, 0xF8, 0xF8, 0x70, 0x20};

/* ---- Tables ---- */

static const char* rank_name(uint8_t lvl) {
    if(lvl >= 20) return "Null Pointer God";
    if(lvl >= 15) return "Protocol Phantom";
    if(lvl >= 10) return "Ghost Hacker";
    if(lvl >= 7) return "Signal Sniffer";
    if(lvl >= 4) return "RF Rogue";
    return "Rookie Flipper";
}

static const char* species_name(uint8_t s) {
    static const char* n[3] = {"Pup", "Kit", "Byte"};
    return n[s % 3];
}

static const char* pet_name(const PetData* d) {
    static const char* n[3][2] = {{"Rex", "Bella"}, {"Milo", "Luna"}, {"Chip", "Pixel"}};
    return n[d->species % 3][d->gender % 2];
}

/* ---- Helpers ---- */

static int8_t clampi(int v) {
    if(v < 0) return 0;
    if(v > 100) return 100;
    return (int8_t)v;
}

static void set_toast(App* app, const char* msg) {
    strncpy(app->toast, msg, sizeof(app->toast) - 1);
    app->toast[sizeof(app->toast) - 1] = 0;
    app->toast_until = furi_get_tick() + furi_kernel_get_tick_frequency() * 3 / 2;
}

static void gain_xp(App* app, int n) {
    app->d.xp += n;
    app->d.sparks += n;
    while(app->d.xp >= app->d.xp_next) {
        app->d.xp -= app->d.xp_next;
        app->d.level++;
        app->d.xp_next = (uint16_t)((uint32_t)app->d.xp_next * 3 / 2);
        char buf[28];
        snprintf(buf, sizeof(buf), "LEVEL UP! Lv%d", app->d.level);
        set_toast(app, buf);
    }
}

static void care(App* app, int hp, int hung, int hap, int xp, const char* msg) {
    app->d.hp = clampi(app->d.hp + hp);
    app->d.hunger = clampi(app->d.hunger + hung);
    app->d.happy = clampi(app->d.happy + hap);
    gain_xp(app, xp);
    set_toast(app, msg);
}

static void feature_task(App* app, const char* msg, int xp, int bonus) {
    app->d.happy = clampi(app->d.happy + 6);
    app->d.hunger = clampi(app->d.hunger + 4);
    int total = xp + bonus;
    if(!app->d.task_done) {
        app->d.task_done = 1;
        app->d.streak++;
        if(app->d.streak > app->d.best) app->d.best = app->d.streak;
        total += 10;
    }
    gain_xp(app, total);
    set_toast(app, msg);
}

typedef enum { MoodHungry, MoodHype, MoodSad, MoodHappy, MoodNeutral } Mood;

static Mood get_mood(const PetData* d) {
    if(d->hunger < 30) return MoodHungry;
    if(d->happy >= 85 && d->hunger >= 50) return MoodHype;
    if(d->happy < 40) return MoodSad;
    if(d->happy >= 65) return MoodHappy;
    return MoodNeutral;
}

static const char* adore_phrase(App* app) {
    static const char* P[] = {
        "I love you!", "Boop!", "Best friend!", "Hehe~", "RF is yummy", "You rock!"};
    if(app->adore_hearts > 6) return "Yay! <3";
    Mood m = get_mood(&app->d);
    if(m == MoodHungry) return "I'm hungry...";
    if(m == MoodSad) return "Play with me?";
    return P[(app->frame / 20) % (sizeof(P) / sizeof(P[0]))];
}

/* ---- Drawing primitives ---- */

static void draw_bitmap8(Canvas* c, int ox, int oy, const uint8_t* rows, int h) {
    for(int y = 0; y < h; y++)
        for(int x = 0; x < 8; x++)
            if(rows[y] & (0x80 >> x)) canvas_draw_dot(c, ox + x, oy + y);
}

/* 16x14 pet body; row 6 = eyes, row 8 = mouth, both swap by mood. */
static void draw_body(Canvas* c, int ox, int oy, const PetData* d, bool blink) {
    static const uint16_t body[14] = {
        0x0000,
        0b0000011111100000,
        0b0000110000110000,
        0b0001000000001000,
        0b0010000000000100,
        0b0100000000000010,
        0,
        0b0100000000000010,
        0,
        0b0010000000000100,
        0b0001000000001000,
        0b0000111111110000,
        0b0000010000100000,
        0b0001100000011000};

    Mood m = get_mood(d);
    uint16_t eye = blink ? 0b0100000000000010 :
                           (m == MoodHype ? 0b0101110001110010 : 0b0100110000110010);
    uint16_t mouth;
    switch(m) {
    case MoodHappy:
    case MoodHype:
        mouth = 0b0100011111100010;
        break;
    case MoodHungry:
        mouth = 0b0100000110000010;
        break;
    case MoodSad:
        mouth = 0b0100001001000010;
        break;
    default:
        mouth = 0b0100001111000010;
        break;
    }

    for(int y = 0; y < 14; y++) {
        uint16_t row = body[y];
        if(y == 6) row = eye;
        if(y == 8) row = mouth;
        for(int x = 0; x < 16; x++)
            if(row & (1 << (15 - x))) canvas_draw_dot(c, ox + x, oy + y);
    }
}

/* Body + species ears + gender accessory. */
static void draw_creature(Canvas* c, int ox, int oy, const PetData* d, bool blink) {
    draw_body(c, ox, oy, d, blink);

    switch(d->species % 3) {
    case 0: /* Pup: floppy ears */
        canvas_draw_rbox(c, ox - 2, oy + 2, 4, 8, 2);
        canvas_draw_rbox(c, ox + 14, oy + 2, 4, 8, 2);
        break;
    case 1: /* Kit: pointy ears */
        canvas_draw_line(c, ox + 2, oy + 1, ox + 4, oy - 3);
        canvas_draw_line(c, ox + 4, oy - 3, ox + 6, oy + 1);
        canvas_draw_line(c, ox + 10, oy + 1, ox + 12, oy - 3);
        canvas_draw_line(c, ox + 12, oy - 3, ox + 14, oy + 1);
        break;
    default: /* Byte: antenna */
        canvas_draw_line(c, ox + 8, oy, ox + 8, oy - 4);
        canvas_draw_disc(c, ox + 8, oy - 5, 1);
        break;
    }

    if(d->gender % 2 == 1) { /* female: a little bow up top-left */
        canvas_draw_box(c, ox, oy - 1, 2, 3);
        canvas_draw_box(c, ox + 3, oy - 1, 2, 3);
        canvas_draw_dot(c, ox + 2, oy);
    }
}

static void draw_anim(Canvas* c, App* app, int ox, int oy) {
    int t = app->anim_t;
    switch(app->anim) {
    case AnimFeed:
        if(t < 7)
            draw_bitmap8(c, ox + 4, oy - 14 + t * 3, spr_bone, 5);
        else if(t < 12) {
            canvas_draw_dot(c, ox + 4, oy + 13);
            canvas_draw_dot(c, ox + 11, oy + 13);
        }
        break;
    case AnimPet: {
        int hy;
        if(t < 6)
            hy = oy - 12 + t * 2;
        else if(t < 12)
            hy = oy - 2 + (t % 2 ? 1 : 0);
        else
            hy = oy - 2 - (t - 12) * 2;
        draw_bitmap8(c, ox + 4, hy, spr_hand, 9);
        if(t >= 8) {
            draw_bitmap8(c, ox - 4, oy - (t - 8), spr_heart, 5);
            draw_bitmap8(c, ox + 14, oy + 2 - (t - 8), spr_heart, 5);
        }
        break;
    }
    case AnimPlay: {
        int phase = t % 8;
        int bx = ox + (phase < 4 ? phase * 5 : (8 - phase) * 5);
        int by = oy + 10 - (t % 4) * 2;
        draw_bitmap8(c, bx, by, spr_ball, 6);
        break;
    }
    case AnimSense:
        canvas_draw_circle(c, ox + 8, oy + 6, 4 + (t % 6) * 3);
        break;
    default:
        break;
    }
}

static void draw_bar(Canvas* c, int x, int y, int w, int val, const char* label) {
    canvas_draw_str(c, x, y + 7, label);
    int bx = x + 22, bw = w - 22;
    canvas_draw_frame(c, bx, y, bw, 8);
    int fill = (bw - 2) * val / 100;
    if(fill > 0) canvas_draw_box(c, bx + 1, y + 1, fill, 6);
}

static void draw_header(Canvas* c, App* app, const char* title) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 9, title);
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "L%d S%d", app->d.level, app->d.streak);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, hdr);
    canvas_draw_line(c, 0, 12, 127, 12);
}

static void draw_list(Canvas* c, App* app, const char* title, const char* const* items, int count, bool glyph) {
    draw_header(c, app, title);
    int win = 4, start = 0;
    if(count > win) {
        start = app->sel - win / 2;
        if(start < 0) start = 0;
        if(start > count - win) start = count - win;
    }
    for(int i = 0; i < win && start + i < count; i++) {
        int idx = start + i, y = 14 + i * 12;
        if(idx == app->sel) {
            canvas_set_color(c, ColorBlack);
            canvas_draw_box(c, 0, y, 128, 12);
            canvas_set_color(c, ColorWhite);
        }
        int tx = 4;
        if(glyph) {
            draw_bitmap8(c, 3, y + 2, glyphs[idx], 8);
            tx = 15;
        }
        canvas_draw_str(c, tx, y + 10, items[idx]);
        if(idx == app->sel) canvas_set_color(c, ColorBlack);
    }
}

static void draw_status(Canvas* c, App* app) {
    draw_header(c, app, pet_name(&app->d));
    int ox = 8, oy = 20;
    if(app->anim == AnimPlay) {
        static const int bounce[6] = {0, 2, 4, 4, 2, 0};
        oy -= bounce[app->anim_t % 6];
    } else if(app->frame % 4 >= 2) {
        oy -= 1;
    }
    draw_creature(c, ox, oy, &app->d, app->blink);
    if(app->anim != AnimNone) draw_anim(c, app, ox, oy);

    int bx = 46, bw = 80;
    draw_bar(c, bx, 16, bw, app->d.hp, "HP");
    draw_bar(c, bx, 28, bw, app->d.hunger, "FUL");
    draw_bar(c, bx, 40, bw, app->d.happy, "JOY");
    char buf[26];
    snprintf(buf, sizeof(buf), "XP %d/%d", app->d.xp, app->d.xp_next);
    canvas_draw_str(c, 2, 62, buf);
}

static void draw_adore(Canvas* c, App* app) {
    int ox = 56, oy = 30;
    if(app->frame % 4 >= 2) oy -= 1;
    draw_creature(c, ox, oy, &app->d, app->blink);

    const char* ph = adore_phrase(app);
    canvas_set_font(c, FontSecondary);
    int w = canvas_string_width(c, ph) + 8;
    int bx = 64 - w / 2;
    if(bx < 2) bx = 2;
    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, bx, 1, w, 13);
    canvas_set_color(c, ColorBlack);
    canvas_draw_rframe(c, bx, 1, w, 13, 2);
    canvas_draw_str(c, bx + 4, 11, ph);
    canvas_draw_line(c, 60, 14, 56, 19);

    if(app->adore_hearts > 0) {
        int n = app->adore_hearts;
        draw_bitmap8(c, ox - 6, oy + 4 - (14 - n), spr_heart, 5);
        draw_bitmap8(c, ox + 16, oy - (14 - n), spr_heart, 5);
    }
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, pet_name(&app->d));
}

static void draw_stats(Canvas* c, App* app) {
    draw_header(c, app, "Stats");
    char b[30];
    int yy = 22;
    snprintf(b, sizeof(b), "%s the %s (%c)", pet_name(&app->d), species_name(app->d.species),
             app->d.gender ? 'F' : 'M');
    canvas_draw_str(c, 2, yy, b);
    yy += 10;
    snprintf(b, sizeof(b), "Lv%d %s", app->d.level, rank_name(app->d.level));
    canvas_draw_str(c, 2, yy, b);
    yy += 10;
    snprintf(b, sizeof(b), "XP %d/%d  Strk %d", app->d.xp, app->d.xp_next, app->d.streak);
    canvas_draw_str(c, 2, yy, b);
    yy += 10;
    int owned = 0;
    for(int i = 0; i < UPGRADE_COUNT; i++) owned += app->d.owned[i];
    snprintf(b, sizeof(b), "Upg %d/%d  Sparks %d", owned, UPGRADE_COUNT, app->d.sparks);
    canvas_draw_str(c, 2, yy, b);
}

static void draw_shop(Canvas* c, App* app) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 9, "Shop");
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%d sparks", app->d.sparks);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, hdr);
    canvas_draw_line(c, 0, 12, 127, 12);
    int win = 4, start = 0, count = UPGRADE_COUNT;
    if(count > win) {
        start = app->sel - win / 2;
        if(start < 0) start = 0;
        if(start > count - win) start = count - win;
    }
    for(int i = 0; i < win && start + i < count; i++) {
        int idx = start + i, y = 14 + i * 12;
        if(idx == app->sel) {
            canvas_set_color(c, ColorBlack);
            canvas_draw_box(c, 0, y, 128, 12);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, 4, y + 10, shop_names[idx]);
        char hint[10];
        if(app->d.owned[idx])
            snprintf(hint, sizeof(hint), "ON");
        else if(app->d.level < shop_req[idx])
            snprintf(hint, sizeof(hint), "Lv%d", shop_req[idx]);
        else
            snprintf(hint, sizeof(hint), "%d", shop_cost[idx]);
        canvas_draw_str_aligned(c, 124, y + 10, AlignRight, AlignBottom, hint);
        if(idx == app->sel) canvas_set_color(c, ColorBlack);
    }
}

static void draw_intro(Canvas* c, App* app, bool gender) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 6, 10, gender ? "Pick a gender" : "Choose your pet");

    PetData tmp = {0};
    tmp.species = gender ? app->tmp_species : app->sel;
    tmp.gender = gender ? app->sel : 0;
    tmp.hunger = 70;
    tmp.happy = 80;
    draw_creature(c, 56, 28, &tmp, app->blink);

    canvas_set_font(c, FontSecondary);
    char line[24];
    if(gender)
        snprintf(line, sizeof(line), "%s  %s", pet_name(&tmp), app->sel ? "Female" : "Male");
    else
        snprintf(line, sizeof(line), "%s", species_name(app->sel));
    canvas_draw_str_aligned(c, 64, 52, AlignCenter, AlignBottom, line);

    canvas_draw_str(c, 2, 32, "<");
    canvas_draw_str(c, 122, 32, ">");
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, gender ? "OK to start" : "OK to choose");
}

static void draw_callback(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(app->screen) {
    case ScreenIntroSpecies:
        draw_intro(canvas, app, false);
        break;
    case ScreenIntroGender:
        draw_intro(canvas, app, true);
        break;
    case ScreenMain:
        draw_list(canvas, app, "FlipperPet", main_items, MAIN_COUNT, true);
        break;
    case ScreenStatus:
        draw_status(canvas, app);
        break;
    case ScreenAdore:
        draw_adore(canvas, app);
        break;
    case ScreenStats:
        draw_stats(canvas, app);
        break;
    case ScreenFeed:
        draw_list(canvas, app, "Feed", feed_items, 4, false);
        break;
    case ScreenPet:
        draw_list(canvas, app, "Pet", pet_items, 4, false);
        break;
    case ScreenPlay:
        draw_list(canvas, app, "Play", play_items, 4, false);
        break;
    case ScreenSubghz:
        draw_list(canvas, app, "Sub-GHz", subghz_items, 3, false);
        break;
    case ScreenNfc:
        draw_list(canvas, app, "NFC", nfc_items, 3, false);
        break;
    case ScreenRfid:
        draw_list(canvas, app, "RFID", rfid_items, 3, false);
        break;
    case ScreenIr:
        draw_list(canvas, app, "Infrared", ir_items, 3, false);
        break;
    case ScreenBadusb:
        draw_list(canvas, app, "BadUSB", badusb_items, 3, false);
        break;
    case ScreenShop:
        draw_shop(canvas, app);
        break;
    case ScreenSettings:
        draw_list(canvas, app, "Settings", settings_items, 4, false);
        break;
    }

    if(furi_get_tick() < app->toast_until) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 0, 53, 128, 11);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, app->toast);
        canvas_set_color(canvas, ColorBlack);
    }

    furi_mutex_release(app->mutex);
}

/* ---- Logic ---- */

static int list_count(Screen s) {
    switch(s) {
    case ScreenIntroSpecies:
        return 3;
    case ScreenIntroGender:
        return 2;
    case ScreenMain:
        return MAIN_COUNT;
    case ScreenFeed:
    case ScreenPet:
    case ScreenPlay:
        return 4;
    case ScreenSubghz:
    case ScreenNfc:
    case ScreenRfid:
    case ScreenIr:
    case ScreenBadusb:
        return 3;
    case ScreenShop:
        return UPGRADE_COUNT;
    case ScreenSettings:
        return 4;
    default:
        return 0; /* Status, Adore, Stats */
    }
}

static void go(App* app, Screen s) {
    if(app->depth < 7) app->stack[app->depth++] = app->screen;
    app->screen = s;
    app->sel = 0;
}

/* Switch to the Status screen and play an animation; Back returns to main. */
static void start_anim(App* app, Anim a) {
    app->anim = a;
    app->anim_t = 0;
    app->stack[0] = ScreenMain;
    app->depth = 1;
    app->screen = ScreenStatus;
    app->sel = 0;
}

static void do_subghz(App* app) {
    uint32_t f = app->sel == 0 ? 433920000 : (app->sel == 1 ? 315000000 : 868350000);
    int rssi = flipperpet_subghz_sense(f);
    char b[28];
    if(rssi > -65) {
        snprintf(b, sizeof(b), "Strong RF! %ddBm", rssi);
        feature_task(app, b, 20, 10);
    } else if(rssi > -80) {
        snprintf(b, sizeof(b), "Faint RF %ddBm", rssi);
        feature_task(app, b, 16, 0);
    } else {
        snprintf(b, sizeof(b), "Quiet %ddBm", rssi);
        set_toast(app, b);
        gain_xp(app, 6);
    }
    start_anim(app, AnimSense);
}

static void apply_nfc(App* app, int r) {
    if(r == 1) {
        feature_task(app, "NFC tag found!", 18, 14);
        start_anim(app, AnimSense);
    } else if(r == 0) {
        set_toast(app, "No tag nearby");
        gain_xp(app, 4);
    } else {
        set_toast(app, "NFC unavailable");
    }
}

static void apply_rfid(App* app, int r) {
    if(r == 1) {
        feature_task(app, "LF tag read!", 18, 12);
        start_anim(app, AnimSense);
    } else if(r == 0) {
        set_toast(app, "No LF tag");
        gain_xp(app, 4);
    } else {
        set_toast(app, "RFID unavailable");
    }
}

static void handle_select(App* app) {
    switch(app->screen) {
    case ScreenIntroSpecies:
        app->tmp_species = app->sel;
        app->screen = ScreenIntroGender;
        app->sel = 0;
        break;
    case ScreenIntroGender:
        app->d.species = app->tmp_species;
        app->d.gender = app->sel;
        app->d.created = 1;
        app->screen = ScreenMain;
        app->sel = 0;
        app->depth = 0;
        set_toast(app, "Hello!");
        break;
    case ScreenMain:
        go(app, main_targets[app->sel]);
        break;
    case ScreenAdore:
        app->adore_hearts = 14;
        if(app->d.happy < 100) app->d.happy++;
        break;
    case ScreenFeed:
        if(app->sel == 0)
            care(app, 0, 18, 8, 5, "Yum! Apple");
        else if(app->sel == 1)
            care(app, 0, 35, 5, 12, "Nom nom!");
        else if(app->sel == 2)
            care(app, 5, 12, 10, 8, "Crunchy!");
        else
            care(app, 10, 8, 18, 10, "WIRED!");
        start_anim(app, AnimFeed);
        break;
    case ScreenPet:
        if(app->sel == 0)
            care(app, 2, 0, 14, 6, "Happy wiggle");
        else if(app->sel == 1)
            care(app, 0, -4, 20, 9, "So content");
        else if(app->sel == 2)
            care(app, 0, 0, 9, 4, "Boop!");
        else
            care(app, 0, -2, 16, 7, "Purrr...");
        start_anim(app, AnimPet);
        break;
    case ScreenPlay:
        if(app->sel == 0)
            care(app, -6, -10, 24, 18, "Got it!");
        else if(app->sel == 1)
            care(app, -4, -8, 20, 14, "Found you!");
        else if(app->sel == 2)
            care(app, -8, -6, 28, 20, "Dance party!");
        else
            care(app, -10, -12, 30, 22, "Zoom zoom!");
        start_anim(app, AnimPlay);
        break;
    case ScreenSubghz:
        do_subghz(app);
        break;
    case ScreenNfc:
        set_toast(app, "Scanning NFC...");
        app->pending_radio = RadioNfc;
        break;
    case ScreenRfid:
        set_toast(app, "Scanning RFID...");
        app->pending_radio = RadioRfid;
        break;
    case ScreenIr:
        if(app->sel == 0)
            feature_task(app, "IR detected!", 18, 0);
        else if(app->sel == 1)
            feature_task(app, "Sent!", 10, 0);
        else
            feature_task(app, "Learned!", 16, 0);
        break;
    case ScreenBadusb:
        if(!app->d.owned[4]) {
            set_toast(app, app->d.level < 10 ? "Locked: Lv10" : "Buy in Shop");
        } else {
            app->d.happy = clampi(app->d.happy + 12);
            gain_xp(app, 14);
            set_toast(app, app->sel == 0 ? "Rickrolled!" : (app->sel == 1 ? "*spins*" : "Tada!"));
        }
        break;
    case ScreenShop: {
        int i = app->sel;
        if(app->d.owned[i]) {
            set_toast(app, "Already owned");
        } else if(app->d.level < shop_req[i]) {
            char b[28];
            snprintf(b, sizeof(b), "Locked: Lv%d", shop_req[i]);
            set_toast(app, b);
        } else if(app->d.sparks < shop_cost[i]) {
            char b[28];
            snprintf(b, sizeof(b), "Need %d sparks", shop_cost[i] - app->d.sparks);
            set_toast(app, b);
        } else {
            app->d.sparks -= shop_cost[i];
            app->d.owned[i] = 1;
            set_toast(app, "Unlocked!");
        }
        break;
    }
    case ScreenSettings:
        if(app->sel == 3)
            set_toast(app, "FlipperPet v2.3");
        else if(app->sel == 2)
            set_toast(app, "Long-press Back resets");
        else
            set_toast(app, "Toggled");
        break;
    default:
        break;
    }
}

/* Returns false to exit the app. */
static bool handle_input(App* app, InputEvent* e) {
    bool nav = (e->type == InputTypeShort || e->type == InputTypeRepeat);
    bool act = (e->type == InputTypeShort);
    bool lng = (e->type == InputTypeLong);
    int count = list_count(app->screen);

    switch(e->key) {
    case InputKeyUp:
    case InputKeyLeft:
        if(nav && count) app->sel = (app->sel + count - 1) % count;
        break;
    case InputKeyDown:
    case InputKeyRight:
        if(nav && count) app->sel = (app->sel + 1) % count;
        break;
    case InputKeyBack:
        if(lng && app->screen == ScreenSettings && app->sel == 2) {
            app->d = (PetData){
                .magic = SAVE_MAGIC,
                .created = 0,
                .level = 1,
                .xp = 0,
                .xp_next = 100,
                .hp = 80,
                .hunger = 60,
                .happy = 70};
            app->screen = ScreenIntroSpecies;
            app->sel = 0;
            app->depth = 0;
            set_toast(app, "Pet reset!");
        } else if(act) {
            if(app->screen == ScreenIntroGender) {
                app->screen = ScreenIntroSpecies;
                app->sel = app->tmp_species;
            } else if(app->screen == ScreenIntroSpecies) {
                return false;
            } else if(app->depth > 0) {
                app->depth--;
                app->screen = app->stack[app->depth];
                app->sel = 0;
            } else {
                return false;
            }
        }
        break;
    case InputKeyOk:
        if(act) handle_select(app);
        break;
    default:
        break;
    }
    return true;
}

static void input_callback(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

static void timer_callback(void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->frame++;
    app->blink = (app->frame % 12 == 0);
    if(app->adore_hearts > 0) app->adore_hearts--;
    if(app->anim != AnimNone) {
        app->anim_t++;
        if(app->anim_t >= anim_len[app->anim]) app->anim = AnimNone;
    }
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

/* ---- Persistence ---- */

static void save_data(App* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SAVE_DIR);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, SAVE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS))
        storage_file_write(f, &app->d, sizeof(PetData));
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

static void load_data(App* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, SAVE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        PetData tmp;
        if(storage_file_read(f, &tmp, sizeof(PetData)) == sizeof(PetData) && tmp.magic == SAVE_MAGIC) {
            app->d = tmp;
            ok = true;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    if(!ok) {
        app->d = (PetData){
            .magic = SAVE_MAGIC,
            .created = 0,
            .species = 0,
            .gender = 0,
            .level = 1,
            .xp = 0,
            .xp_next = 100,
            .hp = 80,
            .hunger = 60,
            .happy = 70};
    }
}

/* ---- Entry point ---- */

int32_t flipperpet_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    load_data(app);
    app->screen = app->d.created ? ScreenMain : ScreenIntroSpecies;
    app->sel = 0;
    app->depth = 0;
    app->anim = AnimNone;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app->queue);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_kernel_get_tick_frequency() / 10);

    InputEvent event;
    bool run = true;
    while(run) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            run = handle_input(app, &event);
            uint8_t pending = app->pending_radio;
            app->pending_radio = RadioNone;
            furi_mutex_release(app->mutex);
            view_port_update(app->view_port);

            /* Heavy radio scans run here, without the UI mutex, so the
               "Scanning..." toast shows and the pet keeps animating. */
            if(pending == RadioNfc) {
                int r = flipperpet_nfc_detect();
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                apply_nfc(app, r);
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
            } else if(pending == RadioRfid) {
                int r = flipperpet_rfid_detect();
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                apply_rfid(app, r);
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
            }
        }
    }

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    save_data(app);
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}

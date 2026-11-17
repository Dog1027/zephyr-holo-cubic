#include "fg_app.h"
#include "ui.h"
#include "nav.h"
#include "aquarium_sprites.h"
#include <zephyr/random/random.h>
#include <math.h>

/*
 * 水族箱：水母和微生物，敲一下或按 OK 撒飼料，微生物會游過去吃（水母不吃）。
 * 每隻生物是一個 lv_image，圖是 A8 遮罩（aquarium_sprites.c，tools/mkaquarium.py 產生），
 * 顏色用 image_recolor 上。只移動物件，LVGL 只重畫有變動的區域。
 */

#define AQUA_JELLIES        4
#define AQUA_MICROBES       8
#define AQUA_CREATURES      (AQUA_JELLIES + AQUA_MICROBES)
#define AQUA_FOOD_MAX       16
#define AQUA_FOOD_PER_FEED  6
#define AQUA_FOOD_SIZE      4
#define AQUA_FOOD_SINK      14.0f   // px/s
#define AQUA_FOOD_LIFE_MS   8000    // 沉到底之後這麼久才消失
#define AQUA_SMELL_PX       110.0f  // 微生物聞得到飼料的距離
#define AQUA_EAT_PX         7.0f
#define AQUA_STEP_MS        33      // 大約 30 fps
#define AQUA_HINT_MS        4000
#define AQUA_SCREEN         ((float)UI_SCREEN_SIZE)
#define AQUA_TAU            6.2831853f

typedef struct {
    lv_obj_t *img;
    uint8_t species;
    uint8_t frame;          // 目前顯示的那一格，UINT8_MAX 表示要重設
    bool left;
    bool shown_left;
    float x, y;             // 中心
    float heading;          // 微生物：游的方向（弧度）
    int64_t turn_at_ms;     // 微生物：下一次隨機轉向的時間
    float phase;            // 動畫 / 水母收縮的相位，0 到 1
} creature_t;

typedef struct {
    lv_obj_t *obj;
    bool active;
    float x, y;
    float sway;
    int64_t settled_ms;     // 沉到底的時間，0 表示還在沉
} food_t;

typedef struct {
    creature_t creatures[AQUA_CREATURES];
    food_t food[AQUA_FOOD_MAX];
    lv_obj_t *hint;
    int64_t last_ms;
    int64_t start_ms;
    bool pending_feed;
} self_t;

APP_FG_REGISTER(Aquarium, ICON_AQUARIUM, app_aquarium_init, app_aquarium_run, app_aquarium_exit, 17, self_t);

// 每種生物的動作：speed 是 px/s，anim_hz 是動畫一圈每秒幾次，eats 會不會吃飼料
typedef struct {
    float speed;
    float anim_hz;
    bool eats;
} species_info_t;

static const species_info_t species_info[AQUA_SPECIES_COUNT] = {
    [AQUA_MOON_JELLY] = {6.0f, 0.45f, false},
    [AQUA_LONG_JELLY] = {5.0f, 0.35f, false},
    [AQUA_BABY_JELLY] = {8.0f, 0.70f, false},
    [AQUA_AMOEBA] = {3.0f, 0.50f, true},
    [AQUA_PARAMECIUM] = {14.0f, 2.50f, true},
    [AQUA_EUGLENA] = {11.0f, 2.00f, true},
    [AQUA_ROTIFER] = {6.0f, 1.50f, true},
    [AQUA_COPEPOD] = {16.0f, 2.00f, true},
    [AQUA_VOLVOX] = {4.0f, 0.60f, true},
    [AQUA_DIATOM] = {2.0f, 0.00f, false},
    [AQUA_BACTERIA] = {5.0f, 1.00f, true},
    [AQUA_SPIRILLUM] = {12.0f, 3.00f, true},
    [AQUA_WATER_BEAR] = {4.0f, 1.20f, true},
};

static const uint32_t jelly_colors[] = {THEME_CYAN, THEME_PURPLE, THEME_BLUE, THEME_TEXT};
static const uint32_t microbe_colors[] = {THEME_GREEN, THEME_ORANGE, THEME_YELLOW, THEME_RED, THEME_PURPLE, THEME_CYAN};

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(sys_rand32_get() % 10000) / 10000.0f;
}

static bool is_jelly(uint8_t species)
{
    return species <= AQUA_BABY_JELLY;
}

static void creature_draw(creature_t *c)
{
    const aqua_sprite_t *s = &aqua_sprites[c->species];
    uint8_t frame = (uint8_t)(c->phase * AQUA_FRAMES) % AQUA_FRAMES;

    if (frame != c->frame || c->left != c->shown_left) {
        c->frame = frame;
        c->shown_left = c->left;
        lv_image_set_src(c->img, c->left ? s->left[frame] : s->right[frame]);
    }
    lv_obj_set_pos(c->img, (int32_t)(c->x - s->w / 2), (int32_t)(c->y - s->h / 2));
}

static void creature_create(lv_obj_t *scr, creature_t *c, uint8_t species, uint32_t color)
{
    const aqua_sprite_t *s = &aqua_sprites[species];

    c->species = species;
    c->x = frand(s->w / 2, AQUA_SCREEN - s->w / 2);
    c->y = frand(s->h / 2, AQUA_SCREEN - s->h / 2);
    c->heading = frand(0, AQUA_TAU);
    c->left = cosf(c->heading) < 0;
    c->phase = frand(0, 1);
    c->frame = UINT8_MAX;

    c->img = lv_image_create(scr);
    lv_obj_set_style_image_recolor(c->img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(c->img, LV_OPA_COVER, 0);
    creature_draw(c);
}

// 水母：收縮的那一下往上推，其他時間慢慢往下沉，左右緩緩飄；從上面出去就從底下回來
static void jelly_step(creature_t *c, float dt)
{
    const species_info_t *info = &species_info[c->species];
    const aqua_sprite_t *s = &aqua_sprites[c->species];

    c->phase = fmodf(c->phase + info->anim_hz * dt, 1.0f);
    // 圖的第 3 格（phase 0.75 附近）是收縮最緊的時候
    float push = -sinf(AQUA_TAU * c->phase);
    float vy = push > 0 ? -info->speed * 3.0f * push : info->speed * 0.4f;
    c->y += vy * dt;
    c->x += info->speed * 0.4f * cosf(c->heading) * dt;

    if (c->y < -s->h / 2) {
        c->y = AQUA_SCREEN + s->h / 2;
        c->x = frand(s->w / 2, AQUA_SCREEN - s->w / 2);
    }
    else if (c->y > AQUA_SCREEN + s->h / 2) {
        c->y = -s->h / 2;
    }
    if (c->x < -s->w / 2) {
        c->x = AQUA_SCREEN + s->w / 2;
    }
    else if (c->x > AQUA_SCREEN + s->w / 2) {
        c->x = -s->w / 2;
    }
}

static food_t *food_nearest(float x, float y, float *dist)
{
    food_t *best = NULL;

    *dist = AQUA_SMELL_PX;
    for (int i = 0; i < AQUA_FOOD_MAX; i++) {
        food_t *f = &self->food[i];
        if (!f->active) {
            continue;
        }
        float d = hypotf(f->x - x, f->y - y);
        if (d < *dist) {
            *dist = d;
            best = f;
        }
    }
    return best;
}

static void food_remove(food_t *f)
{
    f->active = false;
    lv_obj_set_hidden(f->obj, true);
}

// 角度 a 往 target 轉，最多轉 max
static float turn_toward(float a, float target, float max)
{
    float d = remainderf(target - a, AQUA_TAU);
    return a + CLAMP(d, -max, max);
}

// 微生物：隨機換方向游，碰到邊緣轉回來；聞到飼料就加速游過去吃
static void microbe_step(creature_t *c, float dt, int64_t now_ms)
{
    const species_info_t *info = &species_info[c->species];
    const aqua_sprite_t *s = &aqua_sprites[c->species];
    float speed = info->speed;
    float target = c->heading;
    float dist;
    food_t *food = info->eats ? food_nearest(c->x, c->y, &dist) : NULL;

    c->phase = fmodf(c->phase + info->anim_hz * dt, 1.0f);
    if (food != NULL) {
        if (dist < AQUA_EAT_PX) {
            food_remove(food);
        }
        else {
            target = atan2f(food->y - c->y, food->x - c->x);
            speed = MAX(speed * 1.8f, 8.0f);
        }
    }
    else if (now_ms >= c->turn_at_ms) {
        target = c->heading + frand(-1.2f, 1.2f);
        c->turn_at_ms = now_ms + (int64_t)frand(1500, 5000);
    }

    // 太靠近邊緣就往畫面中間轉
    float margin = 6.0f;
    if (c->x < s->w / 2 + margin || c->x > AQUA_SCREEN - s->w / 2 - margin || c->y < s->h / 2 + margin ||
        c->y > AQUA_SCREEN - s->h / 2 - margin) {
        target = atan2f(AQUA_SCREEN / 2 - c->y, AQUA_SCREEN / 2 - c->x);
    }
    c->heading = turn_toward(c->heading, target, 1.5f * dt);
    c->x += speed * cosf(c->heading) * dt;
    c->y += speed * sinf(c->heading) * dt;

    // 轉向時不要一直左右翻，水平分量明顯才換方向
    float h = cosf(c->heading);
    if (h < -0.2f) {
        c->left = true;
    }
    else if (h > 0.2f) {
        c->left = false;
    }
}

static void food_step(food_t *f, float dt, int64_t now)
{
    float bottom = AQUA_SCREEN - AQUA_FOOD_SIZE;

    if (f->settled_ms == 0) {
        f->sway += dt;
        f->y += AQUA_FOOD_SINK * dt;
        f->x += 4.0f * sinf(f->sway * 2.0f) * dt;
        if (f->y >= bottom) {
            f->y = bottom;
            f->settled_ms = now;
        }
    }
    else if (now - f->settled_ms > AQUA_FOOD_LIFE_MS) {
        food_remove(f);
        return;
    }
    lv_obj_set_pos(f->obj, (int32_t)f->x, (int32_t)f->y);
}

// 從上方某處撒一把，散開一點
static void feed(void)
{
    float cx = frand(50, AQUA_SCREEN - 50);
    int added = 0;

    for (int i = 0; i < AQUA_FOOD_MAX && added < AQUA_FOOD_PER_FEED; i++) {
        food_t *f = &self->food[i];
        if (f->active) {
            continue;
        }
        f->active = true;
        f->x = cx + frand(-22, 22);
        f->y = frand(-24, -4);
        f->sway = frand(0, 6);
        f->settled_ms = 0;
        lv_obj_set_pos(f->obj, (int32_t)f->x, (int32_t)f->y);
        lv_obj_set_hidden(f->obj, false);
        added++;
    }
}

static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd == NAV_OK || cmd == NAV_TAP) {
        self->pending_feed = true;
        return true;
    }
    return false;
}

static void app_aquarium_init(void *scr)
{
    ui_screen_init(scr);
    self->start_ms = k_uptime_get();
    self->last_ms = self->start_ms;

    // 水母在後面；種類：大的各一隻，再多一隻小的
    static const uint8_t jellies[AQUA_JELLIES] = {AQUA_MOON_JELLY, AQUA_LONG_JELLY, AQUA_BABY_JELLY, AQUA_BABY_JELLY};
    for (int i = 0; i < AQUA_JELLIES; i++) {
        creature_t *c = &self->creatures[i];
        creature_create(scr, c, jellies[i], jelly_colors[i % ARRAY_SIZE(jelly_colors)]);
        c->heading = frand(0, 1) < 0.5f ? 0 : (AQUA_TAU / 2);
    }

    // 微生物：每次打開隨機挑種類，不重複
    uint8_t pool[AQUA_SPECIES_COUNT - AQUA_AMOEBA];
    for (int i = 0; i < ARRAY_SIZE(pool); i++) {
        pool[i] = AQUA_AMOEBA + i;
    }
    for (int i = 0; i < AQUA_MICROBES; i++) {
        int pick = i + sys_rand32_get() % (ARRAY_SIZE(pool) - i);
        uint8_t species = pool[pick];
        pool[pick] = pool[i];
        pool[i] = species;
        creature_create(scr, &self->creatures[AQUA_JELLIES + i], species,
                        microbe_colors[sys_rand32_get() % ARRAY_SIZE(microbe_colors)]);
    }

    for (int i = 0; i < AQUA_FOOD_MAX; i++) {
        lv_obj_t *obj = lv_obj_create(scr);
        lv_obj_remove_style_all(obj);
        lv_obj_set_size(obj, AQUA_FOOD_SIZE, AQUA_FOOD_SIZE);
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(THEME_YELLOW), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_hidden(obj, true);
        self->food[i].obj = obj;
    }

    self->hint = ui_label(scr, &lv_font_montserrat_14, THEME_TEXT_SECONDARY);
    lv_label_set_text(self->hint, "Tap or OK: feed");
    lv_obj_align(self->hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    nav_set_handler(nav_cb);
}

static void app_aquarium_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (self->pending_feed) {
        self->pending_feed = false;
        feed();
    }
    if (self->hint != NULL && now - self->start_ms > AQUA_HINT_MS) {
        lv_obj_delete(self->hint);
        self->hint = NULL;
    }
    if (now - self->last_ms < AQUA_STEP_MS) {
        return;
    }
    // 卡住很久（例如切換動畫）就不要一次跳一大步
    float dt = MIN(now - self->last_ms, 200) / 1000.0f;
    self->last_ms = now;

    for (int i = 0; i < AQUA_FOOD_MAX; i++) {
        if (self->food[i].active) {
            food_step(&self->food[i], dt, now);
        }
    }
    for (int i = 0; i < AQUA_CREATURES; i++) {
        creature_t *c = &self->creatures[i];
        if (is_jelly(c->species)) {
            jelly_step(c, dt);
        }
        else {
            microbe_step(c, dt, now);
        }
        creature_draw(c);
    }
}

static void app_aquarium_exit(void *scr)
{
}

#include "fg_app.h"
#include "ui.h"
#include "nav.h"
#include "led.h"
#include "prefs.h"
#include <zephyr/random/random.h>
#include <string.h>

/*
 * 文字雨：每一欄有一滴往下掉，最前面的字是白的，後面拖著尾巴，尾端幾格淡出，尾巴裡的字偶爾會換。
 * 敲一下或按 OK 換顏色，上 / 下調速度（都存在 flash）。
 *
 * 順不順看的是節奏：字一次跳一整格（20 px），間隔忽長忽短就會卡。所以
 *   - 用 LVGL timer 走一步（app thread 會準時醒來；run() 只在 33 ms 的刷新節拍上被呼叫），
 *     走完馬上 lv_refr_now() 畫出來，不等下一個刷新節拍
 *   - 每一滴固定每 period 步往下一格，不用小數速度
 * 一步要在 step_ms 內畫完。實測（ESP32，SPI 30 MHz）的成本：
 *   - 每個 lv_draw_character 約 400 us（每次都要查字型、把 4 bpp 解開）
 *   - 每塊 invalidate 的區域約 2.5 ms，就算裡面什麼都沒畫（render、SPI 交易的固定成本）
 * 所以
 *   - 每一欄起始相位隨機，每一步要走的欄數差不多，不會有特別重的一步
 *   - 打開時把每個字先解成一格大小的 A8 遮罩（masks）
 *   - 整個畫面是一個物件，DRAW_MAIN 把這次 clip 區域裡的尾巴字合成到一張 A8 圖（乘上各自的透明度），
 *     用一個 lv_draw_image 上色畫出來；白色的頭直接畫那個字的遮罩。一塊區域只有一兩個 draw task
 *   - 一欄走一格只 invalidate 一塊（尾巴外一格到頭），換字只換在這次本來就要重畫的欄裡
 */

#define MX_COL_W        12
#define MX_ROW_H        20
#define MX_COLS         (UI_SCREEN_SIZE / MX_COL_W)
#define MX_ROWS         (UI_SCREEN_SIZE / MX_ROW_H)
#define MX_MIN_TRAIL    6       // 尾巴長度（不含頭），要比 MX_FADE 長
#define MX_MAX_TRAIL    12
#define MX_FADE         4       // 尾端淡出的格數
#define MX_MAX_PERIOD   4       // 一滴最慢幾步走一格
#define MX_MAX_DELAY    10      // 掉完之後最多等幾格才重來
#define MX_MUTATE_ONE_IN 3      // 走一格的欄，每幾次換掉一個尾巴裡的字
#define MX_FONT         (&lv_font_montserrat_18)
// LVGL 一次 render 的區域不會比 draw buffer 大（CONFIG_LV_Z_VDB_SIZE % 的畫面）
#define MX_BUF_PX       (UI_SCREEN_SIZE * UI_SCREEN_SIZE * CONFIG_LV_Z_VDB_SIZE / 100)

// 只用英文和數字。Montserrat 是比例字型，太寬的字（M、W ...）會擠到隔壁欄，只用窄的
static const char glyphs[] = "0123456789ABCDEFGHJKLNPRSTUVXYZ";
#define MX_GLYPHS       (sizeof(glyphs) - 1)

// 透過稜鏡暗色幾乎看不見：尾巴用飽和但亮的顏色，靠透明度淡出
static const uint32_t colors[] = {
    0x5cff7a,   // 綠
    0x6ff3ff,   // 青
    0xd9a8ff,   // 紫
    0xffc070,   // 琥珀
};
// 速度：一步幾毫秒，最快的一滴每步走一格。實測一步畫 25 ms 左右、最多 45 ms，
// 40 ms 就有兩成的步會晚，所以最快 50 ms
static const uint32_t step_ms[] = {130, 100, 70, 50};
#define MX_DEFAULT_SPEED 2
#define MX_SPEEDS       ARRAY_SIZE(step_ms)

typedef struct {
    int16_t head;       // 最前面那個字的列，可以在畫面上方（<0，還沒進來）或下方
    uint8_t trail;
    uint8_t period;     // 幾步走一格
    uint8_t count;      // 距離下次走還有幾步
} drop_t;

typedef struct {
    lv_obj_t *canvas;
    lv_timer_t *timer;
    drop_t drops[MX_COLS];
    uint8_t cells[MX_COLS][MX_ROWS];                // glyphs 的索引
    uint8_t masks[MX_GLYPHS][MX_ROW_H][MX_COL_W];   // 每個字在一格裡的樣子（A8）
    lv_image_dsc_t mask_imgs[MX_GLYPHS];            // 同上，畫白色的頭用
    uint8_t buf[MX_BUF_PX];                         // 合成尾巴的 A8 圖，draw task 執行完才會再用
    lv_image_dsc_t buf_img;
    uint8_t color;
    uint8_t speed;
} self_t;

APP_FG_REGISTER(Matrix, NULL, app_matrix_init, app_matrix_run, app_matrix_exit, 18, self_t);

static uint32_t rand_range(uint32_t lo, uint32_t hi)
{
    return lo + sys_rand32_get() % (hi - lo + 1);
}

static uint8_t random_glyph(void)
{
    return sys_rand32_get() % MX_GLYPHS;
}

static void a8_image_init(lv_image_dsc_t *img, const uint8_t *data, int32_t w, int32_t h)
{
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_A8;
    img->header.w = w;
    img->header.h = h;
    img->header.stride = w;
    img->data = data;
    img->data_size = w * h;
}

// 把每個字解成一格大小、水平置中的 A8 遮罩
static void masks_init(void)
{
    const lv_font_t *font = MX_FONT;
    int32_t top = (MX_ROW_H - lv_font_get_line_height(font)) / 2 + font->line_height - font->base_line;
    lv_draw_buf_t *tmp = lv_draw_buf_create(MX_COL_W * 2, MX_ROW_H * 2, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);

    for (int i = 0; i < MX_GLYPHS; i++) {
        lv_font_glyph_dsc_t g;
        lv_font_get_glyph_dsc(font, &g, (uint8_t)glyphs[i], 0);
        a8_image_init(&self->mask_imgs[i], &self->masks[i][0][0], MX_COL_W, MX_ROW_H);
        if (tmp == NULL || g.box_w > MX_COL_W * 2 || g.box_h > MX_ROW_H * 2 ||
            lv_font_get_glyph_bitmap(&g, tmp) == NULL) {
            continue;
        }
        uint32_t stride = lv_draw_buf_width_to_stride(g.box_w, LV_COLOR_FORMAT_A8);
        int32_t x0 = (MX_COL_W - g.adv_w) / 2 + g.ofs_x;
        int32_t y0 = top - g.box_h - g.ofs_y;
        for (int32_t y = 0; y < g.box_h; y++) {
            for (int32_t x = 0; x < g.box_w; x++) {
                int32_t mx = x0 + x;
                int32_t my = y0 + y;
                if (mx >= 0 && mx < MX_COL_W && my >= 0 && my < MX_ROW_H) {
                    self->masks[i][my][mx] = tmp->data[y * stride + x];
                }
            }
        }
    }
    lv_draw_buf_destroy(tmp);
}

static void drop_reset(drop_t *d)
{
    d->head = -(int16_t)rand_range(1, MX_MAX_DELAY);
    d->trail = rand_range(MX_MIN_TRAIL, MX_MAX_TRAIL);
    d->period = rand_range(1, MX_MAX_PERIOD);
    // 起始相位錯開：不然週期 2、4 的欄每 4 步會擠在同一步一起走，那一步就畫不完
    d->count = rand_range(1, d->period);
}

// 距離頭 dist 格的字的透明度，0 表示不畫
static lv_opa_t trail_opa(const drop_t *d, int dist)
{
    if (dist < 0 || dist > d->trail) {
        return 0;
    }
    int left = d->trail + 1 - dist;     // 到尾巴外面還有幾格，尾端是 1
    return left > MX_FADE ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * left / (MX_FADE + 1));
}

// 重畫第 c 欄 row_from..row_to 那幾列
static void invalidate_rows(int c, int row_from, int row_to)
{
    row_from = MAX(row_from, 0);
    row_to = MIN(row_to, MX_ROWS - 1);
    if (row_from > row_to) {
        return;
    }

    lv_area_t a;
    lv_obj_get_coords(self->canvas, &a);
    a.x1 += c * MX_COL_W;
    a.x2 = a.x1 + MX_COL_W - 1;
    a.y2 = a.y1 + (row_to + 1) * MX_ROW_H - 1;
    a.y1 += row_from * MX_ROW_H;
    lv_obj_invalidate_area(self->canvas, &a);
}

// 把一格的遮罩乘上 opa 疊進 buf（buf 是畫布座標 (bx, by) 起、寬 bw 高 bh 的一塊）
static void blend_cell(int c, int r, lv_opa_t opa, int32_t bx, int32_t by, int32_t bw, int32_t bh)
{
    const uint8_t *mask = &self->masks[self->cells[c][r]][0][0];
    int32_t cx = c * MX_COL_W;
    int32_t cy = r * MX_ROW_H;
    int32_t x1 = MAX(cx, bx);
    int32_t x2 = MIN(cx + MX_COL_W, bx + bw);
    int32_t y1 = MAX(cy, by);
    int32_t y2 = MIN(cy + MX_ROW_H, by + bh);

    for (int32_t y = y1; y < y2; y++) {
        const uint8_t *src = mask + (y - cy) * MX_COL_W + (x1 - cx);
        uint8_t *dst = self->buf + (y - by) * bw + (x1 - bx);
        // 尾巴大部分是不透明的，直接複製
        if (opa == LV_OPA_COVER) {
            memcpy(dst, src, x2 - x1);
            continue;
        }
        for (int32_t x = x1; x < x2; x++) {
            *dst++ = (uint8_t)((*src++ * opa + 255) >> 8);
        }
    }
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->canvas, &a);
    const lv_area_t *lc = &layer->_clip_area;
    lv_area_t clip = {MAX(lc->x1, a.x1), MAX(lc->y1, a.y1), MIN(lc->x2, a.x2), MIN(lc->y2, a.y2)};
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) {
        return;
    }

    // 以下都是畫布裡的座標
    int32_t bx = clip.x1 - a.x1;
    int32_t by = clip.y1 - a.y1;
    int32_t bw = lv_area_get_width(&clip);
    int32_t bh = lv_area_get_height(&clip);
    int c0 = bx / MX_COL_W;
    int c1 = (bx + bw - 1) / MX_COL_W;
    int r0 = by / MX_ROW_H;
    int r1 = (by + bh - 1) / MX_ROW_H;
    // 比 draw buffer 還大的區域不會出現；萬一有，尾巴就每格各畫一次
    bool composite = bw * bh <= MX_BUF_PX;
    bool any = false;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.recolor = lv_color_hex(colors[self->color]);
    dsc.recolor_opa = LV_OPA_COVER;

    if (composite) {
        memset(self->buf, 0, bw * bh);
    }
    for (int c = c0; c <= c1; c++) {
        const drop_t *d = &self->drops[c];
        for (int r = r0; r <= r1; r++) {
            int dist = d->head - r;
            lv_opa_t opa = trail_opa(d, dist);
            if (opa == 0 || dist == 0) {
                continue;
            }
            if (composite) {
                blend_cell(c, r, opa, bx, by, bw, bh);
                any = true;
            }
            else {
                lv_area_t cell = {a.x1 + c * MX_COL_W, a.y1 + r * MX_ROW_H};
                cell.x2 = cell.x1 + MX_COL_W - 1;
                cell.y2 = cell.y1 + MX_ROW_H - 1;
                dsc.src = &self->mask_imgs[self->cells[c][r]];
                dsc.opa = opa;
                lv_draw_image(layer, &dsc, &cell);
            }
        }
    }
    if (any) {
        a8_image_init(&self->buf_img, self->buf, bw, bh);
        dsc.src = &self->buf_img;
        dsc.opa = LV_OPA_COVER;
        lv_draw_image(layer, &dsc, &clip);
    }

    // 白色的頭
    dsc.recolor = lv_color_hex(THEME_TEXT);
    dsc.opa = LV_OPA_COVER;
    for (int c = c0; c <= c1; c++) {
        int r = self->drops[c].head;
        if (r >= r0 && r <= r1) {
            lv_area_t cell = {a.x1 + c * MX_COL_W, a.y1 + r * MX_ROW_H};
            cell.x2 = cell.x1 + MX_COL_W - 1;
            cell.y2 = cell.y1 + MX_ROW_H - 1;
            dsc.src = &self->mask_imgs[self->cells[c][r]];
            lv_draw_image(layer, &dsc, &cell);
        }
    }
}

static void step_cb(lv_timer_t *t)
{
    for (int c = 0; c < MX_COLS; c++) {
        drop_t *d = &self->drops[c];
        if (--d->count > 0) {
            continue;
        }
        d->count = d->period;
        d->head++;
        if (d->head >= 0 && d->head < MX_ROWS) {
            self->cells[c][d->head] = random_glyph();
        }
        // 尾巴中段（亮度不變那段）偶爾換一個字，反正這一欄等一下整段都要重畫
        if (sys_rand32_get() % MX_MUTATE_ONE_IN == 0) {
            int r = d->head - (int)rand_range(1, d->trail - MX_FADE);
            if (r >= 0 && r < MX_ROWS) {
                self->cells[c][r] = random_glyph();
            }
        }
        // 一整段一塊：區域的固定成本比多送幾格貴
        invalidate_rows(c, d->head - d->trail - 1, d->head);
        if (d->head - d->trail >= MX_ROWS) {
            drop_reset(d);
        }
    }

    lv_refr_now(NULL);
}

static void apply_color(void)
{
    led_set_hex(colors[self->color]);
}

static bool nav_cb(nav_cmd_t cmd)
{
    switch (cmd) {
        case NAV_OK:
        case NAV_TAP:
            self->color = (self->color + 1) % ARRAY_SIZE(colors);
            prefs_save("matrix/color", &self->color, sizeof(self->color));
            apply_color();
            lv_obj_invalidate(self->canvas);
            return true;
        case NAV_UP:
        case NAV_DOWN:
            if (cmd == NAV_UP && self->speed + 1 < MX_SPEEDS) {
                self->speed++;
            }
            else if (cmd == NAV_DOWN && self->speed > 0) {
                self->speed--;
            }
            lv_timer_set_period(self->timer, step_ms[self->speed]);
            prefs_save("matrix/speed", &self->speed, sizeof(self->speed));
            return true;
        default:
            return false;
    }
}

static void app_matrix_init(void *scr)
{
    ui_screen_init(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BLACK), 0);

    if (prefs_load("matrix/color", &self->color, sizeof(self->color)) != sizeof(self->color) ||
        self->color >= ARRAY_SIZE(colors)) {
        self->color = 0;
    }
    if (prefs_load("matrix/speed", &self->speed, sizeof(self->speed)) != sizeof(self->speed) ||
        self->speed >= MX_SPEEDS) {
        self->speed = MX_DEFAULT_SPEED;
    }
    masks_init();

    for (int c = 0; c < MX_COLS; c++) {
        for (int r = 0; r < MX_ROWS; r++) {
            self->cells[c][r] = random_glyph();
        }
        // 一打開畫面上就有雨：頭散在整個畫面高度和上方一點
        drop_t *d = &self->drops[c];
        drop_reset(d);
        d->head = (int16_t)rand_range(0, MX_ROWS + d->trail) - MX_ROWS / 2;
    }

    self->canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(self->canvas);
    lv_obj_set_size(self->canvas, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_center(self->canvas);
    lv_obj_set_clickable(self->canvas, false);
    lv_obj_add_event_cb(self->canvas, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    apply_color();
    nav_set_handler(nav_cb);
    self->timer = lv_timer_create(step_cb, step_ms[self->speed], NULL);
}

static void app_matrix_run(void *scr)
{
}

static void app_matrix_exit(void *scr)
{
    // timer 會用 self，關掉之後 state 就交給下一個 app 了
    lv_timer_delete(self->timer);
    led_restore();
}

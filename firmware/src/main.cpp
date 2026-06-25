#include <M5Cardputer.h>
#include "sprites/sprites.h"
#include "net.h"   // v2 联网层（WiFi+MQTT+UID）

// 音效 no-op wrapper：v1 踩坑——Cardputer-Adv 上 ES8311 Speaker 的 I2S task 与
// PubSubClient 冲突致卡死/MQTT 掉线。M5Unified 的 tone() 会在 _play_raw()
// 里主动 begin() 启 I2S，所以“不显式 begin”不够（review Codex #1）——
// 必须用真 no-op 包裹，彻底不碰 Speaker。等硬件隔离测试后再考虑恢复。
static inline void loiter_beep(float, uint32_t) { /* no-op — 见上方说明 */ }

// ──────────────────────────────────────────────────────────────────────────────
// Layout — mirrors the React mock at /像素风换装功能/src/app/App.tsx
//
//   240×135 screen split into:
//     • Left "preview" panel  (x = 0..91)   striped bg + character + pixel stage
//     • 2-px black vertical divider (x = 92..93)
//     • Right "tabs+grid" panel (x = 94..239)
//        - 16 px tab bar (5 tabs: SKIN/EYES/SUIT/HAIR/ITEM)
//        - 2 px black horizontal divider
//        - 117 px scrollable grid (4 cols × up-to-3 rows of 34×34 cells)
//     • Color-palette swatches overlay top-left of preview (only for outfit/hair)
// ──────────────────────────────────────────────────────────────────────────────
static const int SCREEN_W = 240;
static const int SCREEN_H = 135;

static const int LEFT_W   = 92;
static const int RIGHT_X  = 94;
static const int RIGHT_W  = SCREEN_W - RIGHT_X;   // 146

static const int TAB_H        = 16;
static const int TAB_BORDER_H = 2;
static const int GRID_Y       = TAB_H + TAB_BORDER_H;   // 18
static const int GRID_H       = SCREEN_H - GRID_Y;      // 117

static const int STAGE_H = 10;
static const int STAGE_Y = SCREEN_H - STAGE_H;          // 125

static const int PREVIEW_SCALE   = 2;
static const int RENDER_Y_START  = 12;
static const int RENDER_Y_END    = 64;                  // exclusive
static const int RENDER_ROWS     = RENDER_Y_END - RENDER_Y_START;  // 52
static const int PREVIEW_W       = SPRITE_W * PREVIEW_SCALE;       // 64
static const int PREVIEW_H       = RENDER_ROWS * PREVIEW_SCALE;    // 104
static const int PREVIEW_X       = (LEFT_W - PREVIEW_W) / 2;       // 14
static const int PREVIEW_Y       = 10;

// Color-palette overlay (top-left of preview panel, only for layers with color)
static const int PAL_X       = 4;
static const int PAL_Y       = 2;
static const int PAL_SWATCH  = 7;
static const int PAL_STRIDE  = 8;     // 7 + 1px gap

// Grid: 34×34 cells, 3 px gap, 4 cols × 3 rows visible (12 max)
static const int GRID_CELL   = 34;
static const int GRID_GAP    = 3;
static const int GRID_COLS   = 4;
static const int GRID_ROWS   = 3;
static const int GRID_PAD_L  = 1;     // leftover after centering 4 × 34 + 3 × 3 = 145 in 146
static const int GRID_PAD_T  = 3;
static const int GRID_X0     = RIGHT_X + GRID_PAD_L;            // 95
static const int GRID_Y0     = GRID_Y + GRID_PAD_T;             // 21
static const int GRID_VISIBLE = GRID_COLS * GRID_ROWS;          // 12

// ──────────────────────────────────────────────────────────────────────────────
// Palette — RGB565 conversions of the colors used in the React mock
// ──────────────────────────────────────────────────────────────────────────────
static const uint16_t COL_FRAME_BG     = 0x18A4;  // #1a1726 outer bg
static const uint16_t COL_PANEL_BG     = 0x2928;  // #2d2640 panel bg
static const uint16_t COL_LEFT_BG_A    = 0x2928;  // #2d2640 — solid preview bg (0x49ED rendered as orange on this panel)
static const uint16_t COL_LEFT_BG_B    = 0x398B;  // #3d3358 stripe B (kept for reference; no longer drawn)
static const uint16_t COL_STAGE_A      = 0xCC5B;  // #c98bd9 pixel-stage stripe A
static const uint16_t COL_STAGE_B      = 0xAB57;  // #a86bb8 pixel-stage stripe B
static const uint16_t COL_GOLD         = 0xFF14;  // #f8e1a0 highlight / active tab
static const uint16_t COL_GOLD_DARK    = 0xCD4B;  // #c9a85a active-tab inset shadow
static const uint16_t COL_LAVENDER     = 0xDE5F;  // #d8c8ff cell bg
static const uint16_t COL_LAVENDER_DK  = 0x8BD5;  // #8878a8 cell inset shadow
static const uint16_t COL_BLACK        = 0x0000;
static const uint16_t COL_WHITE        = 0xFFFF;
static const uint16_t COL_PINK         = 0xFC78;  // #ff8ec0 — particle heart

// ──────────────────────────────────────────────────────────────────────────────
// State
// ──────────────────────────────────────────────────────────────────────────────
// defaults: body/eyes/outfit start at index 0; hair starts at 1 (skip "none");
// accessory starts at 0 ("none") so the character looks clean by default.
static int g_layer = 0;                                     // start on SKIN tab (first body)
static int g_shape[SPR_LAYER_COUNT] = {0, 0, 0, 1, 0};
static int g_color[SPR_LAYER_COUNT] = {0, 0, 0, 0, 0};
static bool g_dirty = true;
static bool g_first_paint = true;

// ──────────────────────────────────────────────────────────────────────────────
// Screen state machine (chunks A + B + C of V4 PRD)
//
// Phase 1 onboarding screens. Phase 2+ added in later chunks.
// Each screen has draw_<id>() and input_<id>() functions wired up in
// dispatch_redraw() / dispatch_input() below.
// ──────────────────────────────────────────────────────────────────────────────
enum Screen {
    P1_01_WELCOME,
    P1_02_USERNAME,
    P1_03_DRESSUP,
    P1_07_ISLAND_REVEAL,
    P1_08_ARRIVAL,
    P2_01_LIVE_MIRROR,
    P2_03_HI_SENT,
    P2_04_HI_RECEIVED,
    P2_05_HI_COMPLETED,
    P2_06_SAY_INPUT,
    P2_07_JUMP,
    P2_08_SIG_INPUT,
    P3_01_WAITING,
    P3_02_REVEAL_TAG,
    P3_03_REVEAL_TEXT,
    P3_04_CLOSING,
};
static Screen g_screen = P1_01_WELCOME;

// Onboarding state
static char    g_username[13] = "";
static uint8_t g_username_len = 0;
static int8_t  g_island = -1;                       // 0-5 服务端 push（island/<uid>）；-1 前
static bool    g_joined = false;                    // true 后 = 已发 join，大屏有可视小人（含未分岛中心位）

// Phase 1 揭晓屏：服务端据 baked profile_id push 的文艺双语 reason（island/<uid>，v3'）
static char     g_island_reason_en[96] = "";        // EN 文艺解读
static char     g_island_reason_cn[80] = "";        // CN 文艺解读
static bool     g_island_ready        = false;      // reason 已就位（真到达或本地 fallback）
static uint32_t g_island_req_ms       = 0;          // 进揭晓屏首发 profile/request 时刻（0=未发）
static uint32_t g_island_last_req_ms  = 0;          // 上次重发 profile/request（重连/丢包兜底）
static bool     g_island_warned       = false;      // 过久拿不到岛 → 揭晓屏显可见错误提示（非静默死状态）

// Phase 2 / 3 state
static int8_t  g_collection[5]   = {-1,-1,-1,-1,-1};  // slot[0] = home island after P1-08; rest from HI
static int8_t  g_sig_action      = -1;                // -1 = not set; 0=GIFT, 1=PHONE, 2=PUSH_CART
static int8_t  g_sig_particle    = -1;                // -1 = not set; 0=SPARKLE, 1=HEART, 2=LEAF, 3=BUTTERFLY（当前展示）
static bool    g_sig_owned[4]    = {false,false,false,false};  // 背包：拥有的粒子（降临 1 个 + 近距复制收集；S 屏只能切 owned 内的）
static uint8_t g_reroll_count    = 0;
static char    g_say_text[31]    = "";
static uint8_t g_say_len         = 0;
static uint8_t g_reading_page    = 0;                 // 0=page 1 (P3-02), 1-3=pages 2-4 (P3-03)

// Phase 3 reading (双语) — 由 reading/<uid> 下发，替换硬编码占位
static char     g_reading_title[32]    = "";          // EN 身份标签
static char     g_reading_title_cn[20] = "";          // CN 副标题 ≤6 汉字
static char     g_reading_core_cn[40]  = "";          // CN 核心句 ≤12 汉字
static char     g_reading_lines[9][64] = {""};        // 9 行英文（P3-03 三页 ×3 行）
static char     g_reading_lines_cn[9][40] = {""};     // 9 行中文短句 ≤12 汉字/行
static bool     g_reading_ready        = false;       // reading 已就位（真到达或 fallback 填充）
static uint32_t g_reading_req_ms       = 0;           // 进 P3-01 发请求的时刻
static uint32_t g_reading_last_req_ms  = 0;           // 上次（重）发 reading/request 的时刻（重连兜底）
static uint32_t g_reading_ready_at     = 0;           // 真到达时刻（flash 100% 停 300ms 用）

// Per-screen "entered at" timestamp (for animations / countdowns)
static uint32_t g_screen_entered_ms = 0;

// Signature Quest sub-state
enum QuestState { QUEST_PROMPT, QUEST_SHAKING, QUEST_REVEAL };
static QuestState g_quest_state = QUEST_PROMPT;
static uint8_t    g_quest_energy = 0;   // 0-100

// Arrival sub-state
enum ArrivalState { ARRIVAL_LOADING, ARRIVAL_COUNTDOWN };
static ArrivalState g_arrival_state = ARRIVAL_LOADING;

// ── P3b: 真实 HI 握手状态（替换 MOCK）──
enum HiOutcome { HI_NONE, HI_MATCHED, HI_RESONANCE, HI_DECLINED, HI_EXPIRED };
static HiOutcome g_hi_outcome       = HI_NONE;
static bool      g_hi_sent          = false;          // P2-03: false=选择中, true=已发等待
static char      g_hi_target_nick[13] = "";           // 我要 HI 的人（选择）
static uint8_t   g_hi_target_len    = 0;
static int8_t    g_hi_roster_sel    = 0;              // 名册滚动选择索引
static char      g_hi_in_requester[40] = "";          // 收到的 HI：发起者 uid（用于 respond）
static char      g_hi_in_nick[13]   = "";             // 发起者昵称
static uint16_t  g_hi_in_color      = 0;              // 发起者岛色
static char      g_hi_partner_nick[13] = "";          // 握手成功对方昵称
static uint16_t  g_hi_partner_color = 0;              // 对方岛色
static int8_t    g_hi_partner_island = -1;            // 对方岛 idx（-1=未知）
static int8_t    g_hi_slot          = -1;             // 服务端权威填入格位（-1=共鸣不加色）
static bool      g_hi_await_ack     = false;          // P2-04 accept 后等 matched ack（防假成功）
static uint32_t  g_hi_await_since   = 0;              // await 起点（本地 5s 超时兜底）

// RGB565 conversion macro for compile-time island colors
#define RGB565(r,g,b) (uint16_t)((((r)>>3)<<11) | (((g)>>2)<<5) | ((b)>>3))

// 6 Pride islands (matches V4 + PRD + Figma)
struct IslandInfo {
    const char* name;     // e.g. "EMBER ISLAND"
    const char* biome;    // e.g. "volcano / rust earth"
    const char* traits;   // e.g. "fierce . alive"
    uint16_t    color;    // RGB565
    char        icon;     // ASCII fallback (Font0 has no emoji)
};
static const IslandInfo ISLANDS[6] = {
    { "EMBER",  "volcano / rust earth",    "fierce . alive",     RGB565(0xE8, 0x4D, 0x3C), '*' },
    { "HEARTH", "warm village / dusk",     "kind . steady",      RGB565(0xFF, 0x9F, 0x43), '@' },
    { "SPARK",  "grassland / golden hour", "curious . restless", RGB565(0xF9, 0xCA, 0x24), '!' },
    { "GROVE",  "forest / moss",           "rooted . gentle",    RGB565(0x6A, 0xB0, 0x4C), '%' },
    { "TIDE",   "coast / lighthouse",      "deep . patient",     RGB565(0x4A, 0x6F, 0xA5), '~' },
    { "MIST",   "snow peak / fog forest",  "quiet . dreaming",   RGB565(0x5F, 0x27, 0xCD), '#' },
};

// ── P3b: hex(#rrggbb) → RGB565 / island idx（HI 换色用，服务端发岛色 hex）──
static uint16_t rgb565_from_hex(const char* hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return RGB565(0x88, 0x88, 0x88);
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    int r = hv(hex[1]) * 16 + hv(hex[2]);
    int g = hv(hex[3]) * 16 + hv(hex[4]);
    int b = hv(hex[5]) * 16 + hv(hex[6]);
    return RGB565(r, g, b);
}
static int8_t island_idx_from_hex(const char* hex) {
    uint16_t c = rgb565_from_hex(hex);
    for (int i = 0; i < 6; ++i) if (ISLANDS[i].color == c) return (int8_t)i;
    return -1;  // 未知色（不应发生，服务端只发 6 岛色）
}


static void goto_screen(Screen s) {
    g_screen = s;
    g_screen_entered_ms = millis();
    g_first_paint = true;  // force dress-up panel to fully redraw if going to P1-03
    g_dirty = true;
}

// ──────────────────────────────────────────────────────────────────────────────
// Compositing
//   g_composite      → RGB565 pixels (zero where no layer covers)
//   g_composite_mask → 1 bit per pixel, set where *some* layer wrote a pixel
// ──────────────────────────────────────────────────────────────────────────────
static uint16_t g_composite[SPRITE_PIXELS];
static uint8_t  g_composite_mask[SPRITE_MASK_BYTES];

static inline bool variant_mask_bit(const uint8_t* mask, int i) {
    return (pgm_read_byte(&mask[i >> 3]) >> (i & 7)) & 1;
}

static int clamp_color(int layer, int shape_idx) {
    const SpriteShape& sh = SPR_LAYERS[layer].shapes[shape_idx];
    if (sh.count == 0) return 0;
    int c = g_color[layer];
    if (c >= (int)sh.count) c = sh.count - 1;
    if (c < 0) c = 0;
    return c;
}

static const SpriteVariant* layer_variant(int layer, int shape_override = -1) {
    int sIdx = (shape_override >= 0) ? shape_override : g_shape[layer];
    const SpriteShape& sh = SPR_LAYERS[layer].shapes[sIdx];
    if (sh.count == 0) return nullptr;
    return &sh.variants[clamp_color(layer, sIdx)];
}

static void compose_character(int override_layer = -1, int override_shape = -1) {
    for (int i = 0; i < SPRITE_PIXELS; ++i) g_composite[i] = 0;
    for (int i = 0; i < SPRITE_MASK_BYTES; ++i) g_composite_mask[i] = 0;

    for (int L = 0; L < (int)SPR_LAYER_COUNT; ++L) {
        int sIdx = (L == override_layer) ? override_shape : g_shape[L];
        if (sIdx < 0 || sIdx >= (int)SPR_LAYERS[L].shape_count) continue;
        const SpriteVariant* v = layer_variant(L, sIdx);
        if (!v || !v->pixels || !v->mask) continue;
        for (int i = 0; i < SPRITE_PIXELS; ++i) {
            if (variant_mask_bit(v->mask, i)) {
                g_composite[i] = pgm_read_word(&v->pixels[i]);
                g_composite_mask[i >> 3] |= (1 << (i & 7));
            }
        }
    }
}

// Blit the current composite into a region, filling transparent pixels with
// `fill_color`.  Used for grid cells (solid bg).
static void blit_composite_solid(int dst_x, int dst_y, int scale,
                                 int src_y_start, int src_y_end,
                                 uint16_t fill_color) {
    static uint16_t rowbuf[SPRITE_W * 4];
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    int out_y = 0;
    for (int sy = src_y_start; sy < src_y_end; ++sy) {
        for (int sx = 0; sx < SPRITE_W; ++sx) {
            int i = sy * SPRITE_W + sx;
            uint16_t c = (g_composite_mask[i >> 3] & (1 << (i & 7)))
                            ? g_composite[i] : fill_color;
            for (int k = 0; k < scale; ++k) rowbuf[sx * scale + k] = c;
        }
        for (int k = 0; k < scale; ++k) {
            M5Cardputer.Display.pushImage(dst_x, dst_y + out_y,
                                          SPRITE_W * scale, 1, rowbuf);
            out_y++;
        }
    }
}

// Blit only the opaque pixels of the composite via fillRect — transparent
// pixels are left untouched so whatever bg was already painted shows through.
// This avoids the visible "rectangle around the character" caused by pushImage
// and fillRect rendering the same color slightly differently on this panel.
static void blit_composite_opaque(int dst_x, int dst_y, int scale,
                                  int src_y_start, int src_y_end) {
    if (scale < 1) scale = 1;
    for (int sy = src_y_start; sy < src_y_end; ++sy) {
        int py = dst_y + (sy - src_y_start) * scale;
        // Run-length within a row: coalesce adjacent opaque pixels of the same
        // color into a single fillRect call — fewer SPI transactions.
        int sx = 0;
        while (sx < SPRITE_W) {
            int i = sy * SPRITE_W + sx;
            bool opaque = (g_composite_mask[i >> 3] & (1 << (i & 7)));
            if (!opaque) { ++sx; continue; }
            uint16_t c = g_composite[i];
            int run_start = sx;
            ++sx;
            while (sx < SPRITE_W) {
                int j = sy * SPRITE_W + sx;
                bool op2 = (g_composite_mask[j >> 3] & (1 << (j & 7)));
                if (!op2 || g_composite[j] != c) break;
                ++sx;
            }
            int run_len = sx - run_start;
            M5Cardputer.Display.fillRect(dst_x + run_start * scale, py,
                                         run_len * scale, scale, c);
        }
    }
}

// Blit composite onto the striped preview bg — fill color follows the 2-px
// horizontal stripe pattern at each output row.
static void blit_composite_striped(int dst_x, int dst_y, int scale,
                                   int src_y_start, int src_y_end) {
    static uint16_t rowbuf[SPRITE_W * 4];
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    int out_y = 0;
    for (int sy = src_y_start; sy < src_y_end; ++sy) {
        for (int k = 0; k < scale; ++k) {
            int screen_y = dst_y + out_y;
            uint16_t bg = ((screen_y >> 1) & 1) ? COL_LEFT_BG_B : COL_LEFT_BG_A;
            for (int sx = 0; sx < SPRITE_W; ++sx) {
                int i = sy * SPRITE_W + sx;
                uint16_t c = (g_composite_mask[i >> 3] & (1 << (i & 7)))
                                ? g_composite[i] : bg;
                for (int kx = 0; kx < scale; ++kx) rowbuf[sx * scale + kx] = c;
            }
            M5Cardputer.Display.pushImage(dst_x, screen_y,
                                          SPRITE_W * scale, 1, rowbuf);
            out_y++;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Drawing
// ──────────────────────────────────────────────────────────────────────────────
static void draw_left_bg() {
    // Match the right-side grid bg for a unified look.
    M5Cardputer.Display.fillRect(0, 0, LEFT_W, SCREEN_H, COL_LEFT_BG_B);
}

static void draw_pixel_stage() {
    // 1-px black border above the stage
    M5Cardputer.Display.drawFastHLine(0, STAGE_Y - 1, LEFT_W, COL_BLACK);
    // Solid bg (replaces the old pink stripes — now used for keyboard hint)
    M5Cardputer.Display.fillRect(0, STAGE_Y, LEFT_W, STAGE_H, COL_PANEL_BG);
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
    M5Cardputer.Display.drawString("ENTER > NEXT", LEFT_W/2, STAGE_Y + STAGE_H/2);
}

// Draw the color-swatch row at top-left of the preview panel.  Each swatch is
// the variant's pre-computed `swatch` color (sampled from the chest/hair/etc
// area in the Python builder).  Only shown for layers that flag a color palette
// (outfit, hair).
static void draw_color_palette() {
    const SpriteLayer& layer = SPR_LAYERS[g_layer];
    bool show = (layer.flags & SPR_FLAG_HAS_COLOR_PALETTE);

    // Always clear the palette strip so stale swatches don't linger after a
    // tab switch.
    int strip_h = PAL_SWATCH + 2;  // include 1px outline above/below
    M5Cardputer.Display.fillRect(0, PAL_Y - 1, LEFT_W, strip_h, COL_LEFT_BG_B);
    if (!show) return;

    const SpriteShape& shape = layer.shapes[g_shape[g_layer]];
    if (shape.count == 0) return;
    int sel = clamp_color(g_layer, g_shape[g_layer]);

    for (int v = 0; v < (int)shape.count; ++v) {
        int sx = PAL_X + v * PAL_STRIDE;
        // 1-px black border (drawn via outer rect)
        M5Cardputer.Display.drawRect(sx, PAL_Y, PAL_SWATCH, PAL_SWATCH, COL_BLACK);
        // colored fill inside the border
        M5Cardputer.Display.fillRect(sx + 1, PAL_Y + 1, PAL_SWATCH - 2, PAL_SWATCH - 2,
                                     shape.variants[v].swatch);
        // selected: gold 1-px outline outside
        if (v == sel) {
            M5Cardputer.Display.drawRect(sx - 1, PAL_Y - 1, PAL_SWATCH + 2, PAL_SWATCH + 2,
                                         COL_GOLD);
        }
    }
}

static void draw_character_preview() {
    compose_character();
    // No bg fill — draw_left_bg() already painted the panel, and using fillRect
    // for the opaque sprite pixels means they sit on the same render path as
    // the bg, so no visible rectangle outlines the character.
    blit_composite_opaque(PREVIEW_X, PREVIEW_Y, PREVIEW_SCALE,
                          RENDER_Y_START, RENDER_Y_END);
}

static void draw_divider() {
    // 2-px black vertical between preview and right panel (x = 92, 93)
    M5Cardputer.Display.fillRect(LEFT_W, 0, 2, SCREEN_H, COL_BLACK);
}

static void draw_tab_bar() {
    int base_x = RIGHT_X;
    int per = RIGHT_W / (int)SPR_LAYER_COUNT;          // 29
    int extra = RIGHT_W - per * (int)SPR_LAYER_COUNT;  // 1 (added to last tab)

    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setFont(&fonts::Font0);

    for (int L = 0; L < (int)SPR_LAYER_COUNT; ++L) {
        int w = per + (L == (int)SPR_LAYER_COUNT - 1 ? extra : 0);
        int x = base_x + L * per;
        bool active = (L == g_layer);

        uint16_t bg     = active ? COL_GOLD     : COL_LEFT_BG_A;
        uint16_t fg     = active ? COL_FRAME_BG : COL_LAVENDER;
        uint16_t shadow = active ? COL_GOLD_DARK : COL_PANEL_BG;

        M5Cardputer.Display.fillRect(x, 0, w, TAB_H, bg);
        // 2-px inset shadow at the bottom of the tab
        M5Cardputer.Display.fillRect(x, TAB_H - 2, w, 2, shadow);
        // 1-px right divider between tabs (skip for the last tab)
        if (L < (int)SPR_LAYER_COUNT - 1) {
            M5Cardputer.Display.drawFastVLine(x + w - 1, 0, TAB_H, COL_BLACK);
        }
        M5Cardputer.Display.setTextColor(fg, bg);
        M5Cardputer.Display.drawString(SPR_LAYERS[L].label, x + w / 2, TAB_H / 2 - 1);
    }
    // 2-px black horizontal divider under the tab bar
    M5Cardputer.Display.fillRect(RIGHT_X, TAB_H, RIGHT_W, TAB_BORDER_H, COL_BLACK);
}

static void draw_grid_cell(int col, int row, int shape_idx) {
    const SpriteLayer& layer = SPR_LAYERS[g_layer];
    int cx = GRID_X0 + col * (GRID_CELL + GRID_GAP);
    int cy = GRID_Y0 + row * (GRID_CELL + GRID_GAP);

    bool selected = (shape_idx == g_shape[g_layer]);
    uint16_t bg     = selected ? COL_GOLD       : COL_LAVENDER;
    uint16_t shadow = selected ? COL_GOLD_DARK  : COL_LAVENDER_DK;

    // Cell base
    M5Cardputer.Display.fillRect(cx, cy, GRID_CELL, GRID_CELL, bg);
    // 2-px inset shadow at bottom
    M5Cardputer.Display.fillRect(cx, cy + GRID_CELL - 2, GRID_CELL, 2, shadow);
    // 1-px black outer border
    M5Cardputer.Display.drawRect(cx, cy, GRID_CELL, GRID_CELL, COL_BLACK);

    if (shape_idx < 0 || shape_idx >= (int)layer.shape_count) return;
    const SpriteShape& sh = layer.shapes[shape_idx];

    if (layer.flags & SPR_FLAG_GRID_IS_SWATCHES) {
        // Body / eyes: solid colored swatch in the middle of the cell.
        if (sh.count == 0) {
            // 'none' for swatch layers (shouldn't happen for body/eyes)
            M5Cardputer.Display.drawLine(cx + 6, cy + 6, cx + GRID_CELL - 7, cy + GRID_CELL - 7, COL_BLACK);
            return;
        }
        uint16_t swatch = sh.variants[0].swatch;
        int s_inset = 5;
        int s = GRID_CELL - 2 * s_inset;
        M5Cardputer.Display.fillRect(cx + s_inset, cy + s_inset, s, s, swatch);
        M5Cardputer.Display.drawRect(cx + s_inset, cy + s_inset, s, s, COL_BLACK);
    } else if (sh.count == 0) {
        // 'none' shape — slash through cell to indicate no item
        M5Cardputer.Display.drawLine(cx + 6, cy + 6,
                                     cx + GRID_CELL - 7, cy + GRID_CELL - 7, COL_BLACK);
        M5Cardputer.Display.drawLine(cx + 6, cy + GRID_CELL - 7,
                                     cx + GRID_CELL - 7, cy + 6, COL_BLACK);
    } else {
        // Hair / outfit / accessory: render full character with this shape
        // overriding the current layer.  Source rows 12..44 = head + shoulders,
        // exactly 32 px tall → fits 32×32 inside the 34×34 cell with 1-px border.
        // Use opaque-only blit so the cell bg (gold when selected) shows through
        // around the character — same render path as the cell fill, so the
        // selection frame looks uniform (no inner rectangle artifact).
        compose_character(g_layer, shape_idx);
        blit_composite_opaque(cx + 1, cy + 1, 1, 12, 44);
    }
}

static void draw_grid() {
    int n = SPR_LAYERS[g_layer].shape_count;
    int total = GRID_VISIBLE;
    // Bg fill behind the grid (purple).
    M5Cardputer.Display.fillRect(RIGHT_X, GRID_Y + TAB_BORDER_H,
                                 RIGHT_W, GRID_H - TAB_BORDER_H, COL_LEFT_BG_B);

    for (int i = 0; i < total; ++i) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        if (i < n) {
            draw_grid_cell(col, row, i);
        } else {
            // Empty slot — draw a dimmed placeholder
            int cx = GRID_X0 + col * (GRID_CELL + GRID_GAP);
            int cy = GRID_Y0 + row * (GRID_CELL + GRID_GAP);
            M5Cardputer.Display.fillRect(cx, cy, GRID_CELL, GRID_CELL, COL_PANEL_BG);
            M5Cardputer.Display.drawRect(cx, cy, GRID_CELL, GRID_CELL, COL_BLACK);
        }
    }
}

static void redraw_all() {
    if (g_first_paint) {
        M5Cardputer.Display.fillScreen(COL_FRAME_BG);
        g_first_paint = false;
    }
    draw_left_bg();
    draw_pixel_stage();
    draw_color_palette();
    draw_character_preview();
    draw_divider();
    draw_tab_bar();
    draw_grid();
}

// ──────────────────────────────────────────────────────────────────────────────
// Input handling
//
//   ; / .   tabs (prev / next layer)
//   , / /   shape (prev / next item in grid)
//   n / m   color (prev / next swatch in palette; no-op for layers w/o palette)
//   r       randomize everything
//   Enter   emit current selection over serial
//
// Edge-triggered: each handler fires once per physical key press.  We track the
// set of keys held last frame and fire only on keys that are newly down.
// ──────────────────────────────────────────────────────────────────────────────
#include <set>

static std::set<char> g_prev_keys;
static bool g_prev_enter = false;

static void rotate_shape(int dir) {
    int n = SPR_LAYERS[g_layer].shape_count;
    g_shape[g_layer] = (g_shape[g_layer] + dir + n) % n;
    g_dirty = true;
}

static void rotate_color(int dir) {
    int sIdx = g_shape[g_layer];
    int n = SPR_LAYERS[g_layer].shapes[sIdx].count;
    if (n <= 1) return;
    g_color[g_layer] = (g_color[g_layer] + dir + n) % n;
    g_dirty = true;
}

static void rotate_layer(int dir) {
    int n = SPR_LAYER_COUNT;
    g_layer = (g_layer + dir + n) % n;
    g_dirty = true;
}

static void handle_key(char c) {
    switch (c) {
        case ';': rotate_layer(-1); break;
        case '.': rotate_layer(+1); break;
        case ',': rotate_shape(-1); break;
        case '/': rotate_shape(+1); break;
        case 'n': case 'N': rotate_color(-1); break;
        case 'm': case 'M': rotate_color(+1); break;
        case 'r': case 'R':
            for (int L = 0; L < (int)SPR_LAYER_COUNT; ++L) {
                g_shape[L] = random(0, (int)SPR_LAYERS[L].shape_count);
                int vc = SPR_LAYERS[L].shapes[g_shape[L]].count;
                g_color[L] = vc > 0 ? random(0, vc) : 0;
            }
            g_dirty = true;
            break;
    }
}

static void emit_selection() {
    Serial.print("{\"type\":\"join\",\"character\":{");
    for (int L = 0; L < (int)SPR_LAYER_COUNT; ++L) {
        Serial.printf("\"%s\":{\"shape\":%d,\"color\":%d}",
                      SPR_LAYERS[L].key, g_shape[L], clamp_color(L, g_shape[L]));
        if (L + 1 < (int)SPR_LAYER_COUNT) Serial.print(",");
    }
    Serial.println("}}");
}

static void publish_profile_state() {
    net_set_profile(g_shape, g_color, g_sig_particle, g_sig_action);
    net_publish_profile();
}

// ──────────────────────────────────────────────────────────────────────────────
// Onboarding screen drawers + input handlers
// (P1-03 dress-up reuses redraw_all() + handle_key() above)
// ──────────────────────────────────────────────────────────────────────────────

// --- P1-01 Welcome ---
static void draw_p1_01_welcome() {
    auto& d = M5Cardputer.Display;

    // Pride flag background: 6 horizontal color stripes (matches 6 islands)
    int band_h = SCREEN_H / 6;  // 22 (with 3 leftover, last band gets remainder)
    for (int i = 0; i < 6; ++i) {
        int y = i * band_h;
        int h = (i == 5) ? SCREEN_H - y : band_h;
        d.fillRect(0, y, SCREEN_W, h, ISLANDS[i].color);
    }

    d.setFont(&fonts::Font0);
    d.setTextDatum(middle_center);

    // Title "ISLANDS OF COLOR" — centered, dark overlay box for legibility on rainbow bg
    const char* title = "ISLANDS OF COLOR";
    int title_chars = (int)strlen(title);
    int title_w = title_chars * 12;        // ~12px/char at size 2
    int title_box_w = title_w + 16;
    int title_box_h = 26;
    int title_box_x = (SCREEN_W - title_box_w) / 2;
    int title_box_y = (SCREEN_H - title_box_h) / 2 - 8;
    d.fillRect(title_box_x, title_box_y, title_box_w, title_box_h, COL_FRAME_BG);
    d.drawRect(title_box_x, title_box_y, title_box_w, title_box_h, COL_BLACK);
    d.setTextSize(2);
    d.setTextColor(COL_GOLD, COL_FRAME_BG);
    d.drawString(title, SCREEN_W/2, title_box_y + title_box_h/2 + 1);

    // Subtitle tagline
    d.setTextSize(1);
    int sub_box_w = 160;
    int sub_box_h = 12;
    int sub_box_x = (SCREEN_W - sub_box_w) / 2;
    int sub_box_y = title_box_y + title_box_h + 4;
    d.fillRect(sub_box_x, sub_box_y, sub_box_w, sub_box_h, COL_FRAME_BG);
    d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
    d.drawString("welcome to your color.", SCREEN_W/2, sub_box_y + sub_box_h/2);

    // Prompt at bottom
    int prompt_box_w = 130;
    int prompt_box_h = 13;
    int prompt_box_x = (SCREEN_W - prompt_box_w) / 2;
    int prompt_box_y = SCREEN_H - prompt_box_h - 4;
    d.fillRect(prompt_box_x, prompt_box_y, prompt_box_w, prompt_box_h, COL_FRAME_BG);
    d.setTextColor(COL_GOLD, COL_FRAME_BG);
    d.drawString("[ PRESS ENTER ]", SCREEN_W/2, prompt_box_y + prompt_box_h/2);
}

static void input_p1_01_welcome(bool enter_pressed) {
    if (enter_pressed) goto_screen(P1_02_USERNAME);
}

// --- P1-02 Username Input ---
static void draw_p1_02_username() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_FRAME_BG);
    d.setFont(&fonts::Font0);
    d.setTextDatum(middle_center);

    d.setTextSize(1);
    d.setTextColor(COL_GOLD);
    d.drawString("WHAT'S YOUR NAME?", SCREEN_W/2, 22);

    // Input box (size 2 font = ~12px/char; 12 chars × 12 + 12px pad = 156px wide)
    int box_w = 168;
    int box_h = 24;
    int box_x = (SCREEN_W - box_w) / 2;
    int box_y = 48;
    d.fillRect(box_x, box_y, box_w, box_h, COL_PANEL_BG);
    d.drawRect(box_x, box_y, box_w, box_h, COL_GOLD);

    // Username + static cursor "_" (no blink → no flicker)
    d.setTextSize(2);
    d.setTextColor(COL_GOLD, COL_PANEL_BG);
    char buf[16];
    snprintf(buf, sizeof(buf), "%s_", g_username);
    d.drawString(buf, SCREEN_W/2, box_y + box_h/2 + 1);

    // Char count
    d.setTextSize(1);
    d.setTextColor(COL_LAVENDER_DK);
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "%d / 12 characters", g_username_len);
    d.drawString(count_buf, SCREEN_W/2, 85);

    // Hint
    d.drawString("A-Z 0-9 | ENTER ok | DEL back", SCREEN_W/2, 118);
}

static void input_p1_02_username(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    for (char c : new_keys) {
        if (g_username_len >= 12) break;
        char up = c;
        if (c >= 'a' && c <= 'z') up = c - 32;
        if (!((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9'))) continue;
        g_username[g_username_len++] = up;
        g_username[g_username_len] = '\0';
        loiter_beep(2200, 20);  // subtle typing blip
        g_dirty = true;
    }
    if (del_pressed) {
        if (g_username_len > 0) {
            g_username[--g_username_len] = '\0';
            loiter_beep(1800, 20);  // slightly lower for delete
            g_dirty = true;
        } else {
            goto_screen(P1_01_WELCOME);  // empty + DEL → back to Welcome
        }
    }
    if (enter_pressed && g_username_len > 0) {
        // v2: 用户名确定 → 上报 join（设备出现在大屏）
        net_set_nick(g_username);
        net_set_profile(g_shape, g_color, g_sig_particle, g_sig_action);
        net_publish_join();
        g_joined = true;   // 标记已上线：心跳从此刻起维持小人（含未分岛中心位）
        goto_screen(P1_03_DRESSUP);
    }
}

// ── 揭晓屏文字换行助手（EN 贪心按词 / CN 按 UTF-8 码点）──
static int draw_wrap_en(const char* s, int x, int y, int max_w, int line_h, int max_lines, uint16_t color) {
    auto& d = M5Cardputer.Display;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(color);
    char line[80]; int ll = 0, lines = 0, si = 0;
    while (s[si] && lines < max_lines) {
        while (s[si] == ' ') si++;
        char word[40]; int wl = 0;
        while (s[si] && s[si] != ' ' && wl < (int)sizeof(word) - 1) word[wl++] = s[si++];
        word[wl] = '\0';
        if (wl == 0) break;
        char trial[80];
        if (ll == 0) snprintf(trial, sizeof(trial), "%s", word);
        else         snprintf(trial, sizeof(trial), "%s %s", line, word);
        if ((int)d.textWidth(trial) <= max_w) {
            strncpy(line, trial, sizeof(line) - 1); line[sizeof(line) - 1] = '\0'; ll = strlen(line);
        } else {
            if (ll > 0) { d.drawString(line, x, y + lines * line_h); lines++; }
            if (lines >= max_lines) break;
            strncpy(line, word, sizeof(line) - 1); line[sizeof(line) - 1] = '\0'; ll = strlen(line);
        }
    }
    if (ll > 0 && lines < max_lines) { d.drawString(line, x, y + lines * line_h); lines++; }
    return lines;
}

static void draw_wrap_cn(const char* s, int x, int y, int cps_per_line, int line_h, int max_lines, uint16_t color) {
    auto& d = M5Cardputer.Display;
    d.setFont(&fonts::efontCN_14);
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(color);
    char line[80]; int lb = 0, cps = 0, lines = 0, i = 0;
    while (s[i] && lines < max_lines) {
        unsigned char c = (unsigned char)s[i];
        int clen = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        for (int k = 0; k < clen && s[i]; k++) { if (lb < (int)sizeof(line) - 1) line[lb++] = s[i]; i++; }
        cps++;
        if (cps >= cps_per_line) { line[lb] = '\0'; d.drawString(line, x, y + lines * line_h); lines++; lb = 0; cps = 0; }
    }
    if (lb > 0 && lines < max_lines) { line[lb] = '\0'; d.drawString(line, x, y + lines * line_h); }
}

// 本地岛屿 reason fallback（双语）—— server 15s 没回 reason 时兜底，调性不崩、不依赖网络
struct IslandReasonFB { const char* en; const char* cn; };
static const IslandReasonFB ISLAND_REASON_FB[6] = {
    { "you burn bright and refuse to fade",      "你燃烧着，从不肯黯淡" },   // EMBER
    { "you make every room feel like home",      "你让每个角落都像家" },     // HEARTH
    { "your curiosity lights the long road",     "好奇心照亮你的长路" },     // SPARK
    { "you grow quiet and deep, like old roots", "你像老树根，沉静而深" },   // GROVE
    { "you hold steady when the tide runs high", "潮水再高，你也稳住" },     // TIDE
    { "you dream in the fog and find the way",   "你在雾里做梦，也找到路" }, // MIST
};

static void fill_island_reason_fallback(int island_idx) {
    const IslandReasonFB& f = ISLAND_REASON_FB[(island_idx >= 0 && island_idx < 6) ? island_idx : 2];
    strncpy(g_island_reason_en, f.en, sizeof(g_island_reason_en) - 1);
    g_island_reason_en[sizeof(g_island_reason_en) - 1] = '\0';
    strncpy(g_island_reason_cn, f.cn, sizeof(g_island_reason_cn) - 1);
    g_island_reason_cn[sizeof(g_island_reason_cn) - 1] = '\0';
    g_island_ready = true;
}

// 揭晓屏在 loop 里的拉取/兜底（不在 draw 里每帧全屏重绘 → 防闪屏；reason 异步到达靠 on_island 置 g_dirty）：
//   首发 profile/request → 未就位每 3s 重发（重连/丢包兜底）→ 15s 岛已知但 reason 没来 → 本地 fallback。
//   12s 连岛都没来（server/网络挂）→ 置 warned 让揭晓屏显可见错误提示（review Codex P2：非静默死状态）。
static void maybe_request_island() {
    if (g_screen != P1_07_ISLAND_REVEAL || g_island_ready) return;
    uint32_t now = millis();
    if (g_island_req_ms == 0) {                       // 首次进屏：拉一次本机岛屿+reason
        net_publish_profile_request();
        g_island_req_ms = now;
        g_island_last_req_ms = now;
        return;
    }
    if (now - g_island_last_req_ms >= 3000) {          // 3s 重发（QoS0 丢包 / MQTT 重连窗口）
        net_publish_profile_request();
        g_island_last_req_ms = now;
    }
    if (g_island < 0) {
        // 连岛都没来：12s 后点亮可见错误提示（仅一次置 dirty 驱动重绘，之后仍静默重试自愈）
        if (!g_island_warned && now - g_island_req_ms >= 12000) {
            g_island_warned = true;
            g_dirty = true;
        }
        return;
    }
    if (now - g_island_req_ms >= 15000) {             // 15s 兜底：岛已知但 reason 始终没来
        fill_island_reason_fallback(g_island);
        g_dirty = true;
    }
}

// --- P1-07 Island Reveal（v3'：显服务端 push 的 island + 文艺双语 reason）---
static void draw_p1_07_island_reveal() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_FRAME_BG);
    d.setFont(&fonts::Font0);

    // island 还没 push 到 → finding 态（maybe_request_island 在 loop 里拉取）
    if (g_island < 0 || g_island >= 6) {
        d.setTextSize(1);
        d.setTextDatum(middle_center);
        d.setTextColor(COL_LAVENDER);
        d.drawString("finding your island...", SCREEN_W/2, SCREEN_H/2 - 6);
        if (g_island_warned) {  // 过久没来 → 可见错误提示（仍后台重试自愈）
            d.setTextColor(COL_LAVENDER_DK);
            d.drawString("check wifi - ask your host", SCREEN_W/2, SCREEN_H/2 + 12);
        }
        return;
    }
    const IslandInfo& isle = ISLANDS[g_island];

    d.setTextSize(1);
    d.setTextDatum(middle_center);
    d.setTextColor(COL_LAVENDER_DK);
    d.drawString("YOU BELONG ON", SCREEN_W/2, 12);

    // 岛名 size2 + 色块（island idx 服务端权威，色取 ISLANDS 表保持与状态栏一致）
    d.setTextSize(2);
    int name_w = (int)strlen(isle.name) * 12;
    int block = 12, gap = 6;
    int total_w = block + gap + name_w;
    int gx = (SCREEN_W - total_w) / 2, gy = 24;
    d.fillRect(gx, gy, block, block, isle.color);
    d.drawRect(gx, gy, block, block, COL_BLACK);
    d.setTextColor(isle.color);
    d.setTextDatum(top_left);
    d.drawString(isle.name, gx + block + gap, gy - 1);

    // reason 区：就位 → EN 贪心换行（≤3 行）+ CN efontCN（≤2 行）；未就位 → loading 态
    if (g_island_ready && g_island_reason_en[0]) {
        draw_wrap_en(g_island_reason_en, 10, 50, SCREEN_W - 20, 11, 3, COL_LAVENDER);
        if (g_island_reason_cn[0])
            draw_wrap_cn(g_island_reason_cn, 10, 88, 16, 18, 2, COL_GOLD);
    } else {
        d.setTextSize(1);
        d.setTextDatum(middle_center);
        d.setTextColor(COL_LAVENDER_DK);
        d.drawString("reading the tides...", SCREEN_W/2, 72);
    }

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(middle_center);
    d.setTextColor(COL_GOLD);
    d.drawString("[ ENTER TO CONTINUE ]", SCREEN_W/2, 128);
}

static void input_p1_07_island_reveal(bool enter_pressed, bool del_pressed) {
    if (enter_pressed && g_island >= 0 && g_island < 6) {
        // 闸门：岛未到不让继续（review Codex P1：防空 profile / server miss 时隐式默认 island 0）。
        // reason 未到仍可继续（只是调性，不困住用户）。
        g_collection[0] = g_island;
        for (int i = 1; i < 5; ++i) g_collection[i] = -1;
        // Randomly assign signature particle (0-3: SPARKLE/HEART/LEAF/BUTTERFLY)
        g_sig_particle = (int8_t)(random(0, 4));
        g_sig_action = 0;  // always CARRY (single action, action field kept for compat)
        for (int i = 0; i < 4; ++i) g_sig_owned[i] = false;
        g_sig_owned[g_sig_particle] = true;   // 背包初始 = 降临这个粒子
        publish_profile_state();  // 上报降临 sig → 服务端锁定为 origin（近距复制取这个）
        g_arrival_state = ARRIVAL_LOADING;
        goto_screen(P1_08_ARRIVAL);
    }
    if (del_pressed) {
        goto_screen(P1_03_DRESSUP);   // v3'：quiz 已删，回换装屏
    }
}

// --- P1-03 Dress-up input wrapper ---
static void dressup_handle_input(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    for (char c : new_keys) handle_key(c);
    if (enter_pressed) {
        emit_selection();  // Serial debug
        publish_profile_state();  // 把换装结果实时同步到大屏
        // v3'：分岛由 server 据 baked profile_id 决定（join 时已 push island/<uid>）。
        // 进揭晓屏前重置拉取计时 → 未就位则 maybe_request_island 发 profile/request。
        g_island_req_ms = 0;
        g_island_last_req_ms = 0;
        g_island_warned = false;
        goto_screen(P1_07_ISLAND_REVEAL);
    }
    if (del_pressed) {
        goto_screen(P1_02_USERNAME);  // back to username (preserved)
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Shared status bar + collection grid (used by Live Mirror onward)
// ──────────────────────────────────────────────────────────────────────────────
static const int STATUS_H = 13;
static const int COLLECT_H = 16;

static void draw_status_bar() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, COL_PANEL_BG);
    d.drawFastHLine(0, STATUS_H, SCREEN_W, COL_BLACK);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(middle_left);
    // Username on left
    d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
    d.drawString(g_username, 4, STATUS_H/2);
    // Island chip on right
    if (g_island >= 0 && g_island < 6) {
        const IslandInfo& isle = ISLANDS[g_island];
        // Color dot + name
        int dot_x = SCREEN_W - 4 - (int)strlen(isle.name) * 6 - 8;
        d.fillRect(dot_x, STATUS_H/2 - 3, 6, 6, isle.color);
        d.setTextColor(isle.color, COL_PANEL_BG);
        d.drawString(isle.name, dot_x + 9, STATUS_H/2);
    }
}

static void draw_collection_grid() {
    auto& d = M5Cardputer.Display;
    int y = SCREEN_H - COLLECT_H;
    d.fillRect(0, y, SCREEN_W, COLLECT_H, COL_PANEL_BG);
    d.drawFastHLine(0, y, SCREEN_W, COL_BLACK);

    // 5 cells of 16x10, centered with 4px gaps
    int cell_w = 24, cell_h = 10, gap = 4;
    int total = 5 * cell_w + 4 * gap;
    int x0 = (SCREEN_W - total) / 2;
    int cy = y + (COLLECT_H - cell_h) / 2;
    for (int i = 0; i < 5; ++i) {
        int cx = x0 + i * (cell_w + gap);
        bool is_home = (i == 0);
        uint16_t border = is_home ? COL_GOLD : COL_BLACK;
        if (g_collection[i] >= 0 && g_collection[i] < 6) {
            d.fillRect(cx, cy, cell_w, cell_h, ISLANDS[g_collection[i]].color);
        } else {
            d.fillRect(cx, cy, cell_w, cell_h, COL_FRAME_BG);
            d.setTextDatum(middle_center);
            d.setFont(&fonts::Font0);
            d.setTextSize(1);
            d.setTextColor(COL_LAVENDER_DK);
            d.drawString("_", cx + cell_w/2, cy + cell_h/2);
        }
        d.drawRect(cx, cy, cell_w, cell_h, border);
    }
}

// Draws current g_shape/g_color character at given screen position + scale
static void draw_character_at(int x, int y, int scale) {
    compose_character();
    blit_composite_opaque(x, y, scale, RENDER_Y_START, RENDER_Y_END);
}

// Pixel ground dots (background pattern for Live Mirror main area)
static void draw_pixel_ground(int x0, int y0, int w, int h) {
    auto& d = M5Cardputer.Display;
    d.fillRect(x0, y0, w, h, COL_FRAME_BG);
    for (int y = y0 + 2; y < y0 + h; y += 6) {
        for (int x = x0 + 2; x < x0 + w; x += 6) {
            d.drawPixel(x, y, COL_LEFT_BG_B);
        }
    }
}

// ��─────────────────────────────────────────────────────────────────────────────
// IMU shake detection (BMI270 on Cardputer ADV)
// Reads accel each call, returns true if |a|-1g exceeds threshold with debounce.
// ──────────────────────────���───────────────────────────────────────────────────
static uint32_t g_last_shake_at = 0;

// Ring buffer of recent accel magnitudes for static-vs-held detection
static const int   ACCEL_BUF_N = 12;
static float       g_accel_buf[ACCEL_BUF_N] = {0};
static uint8_t     g_accel_buf_pos = 0;
static uint32_t    g_last_accel_sample_at = 0;

// Sample current accel magnitude into ring buffer (~10 Hz)
static void sample_accel_mag() {
    uint32_t now = millis();
    if (now - g_last_accel_sample_at < 100) return;
    g_last_accel_sample_at = now;
    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.getAccelData(&ax, &ay, &az)) return;
    g_accel_buf[g_accel_buf_pos] = sqrtf(ax*ax + ay*ay + az*az);
    g_accel_buf_pos = (g_accel_buf_pos + 1) % ACCEL_BUF_N;
}

// Returns true if M5 is being held (has micro-tremor) vs flat-on-table static.
// Looks at peak-to-peak range over recent samples.
static bool is_held() {
    sample_accel_mag();
    float lo = 99, hi = -99;
    for (int i = 0; i < ACCEL_BUF_N; ++i) {
        if (g_accel_buf[i] < lo) lo = g_accel_buf[i];
        if (g_accel_buf[i] > hi) hi = g_accel_buf[i];
    }
    return (hi - lo) > 0.02f;  // 0.02g range = clearly being held
}

static bool detect_shake_event() {
    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.getAccelData(&ax, &ay, &az)) return false;
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    float intensity = fabsf(mag - 1.0f);
    uint32_t now = millis();
    if (intensity > 0.6f && (now - g_last_shake_at) > 180) {
        g_last_shake_at = now;
        loiter_beep(700, 80);  // low 700Hz blip for shake feedback
        return true;
    }
    return false;
}

// Breathing animation offset: -1 ↔ 0 ↔ +1 over 1.2s period when held.
// Returns 0 when on table (static).
static int compute_breathing_offset() {
    if (!is_held()) return 0;
    float phase = (millis() % 1200) / 1200.0f;       // 0..1
    float s = sinf(phase * 6.28318f);                // -1..+1
    return (int)roundf(s);                           // -1, 0, +1
}

// ──────────────────────────────────────────────────────────────────────────────
// Particle drawings (used in HI Completed + entrance animation)
// Each takes anim_t [0..1] and a pixel scale s (1=normal, 2=2×).
// ──────────────────────────────────────────────────────────────────────────────

// Scaled filled pixel helper
static inline void fpx(int x, int y, int s, uint16_t c) {
    M5Cardputer.Display.fillRect(x, y, s, s, c);
}

// Plot a small bitmap pattern; each bit becomes an s×s block centered on (cx,cy)
static void plot_pattern(int cx, int cy, int w, int h, const uint8_t* rows, uint16_t color, int s = 1) {
    for (int y = 0; y < h; ++y) {
        uint8_t row = rows[y];
        for (int x = 0; x < w; ++x) {
            if (row & (1 << (w - 1 - x))) {
                fpx(cx - (w*s)/2 + x*s, cy - (h*s)/2 + y*s, s, color);
            }
        }
    }
}

// SPARKLE: 4-point star that pulses, with orbiting satellite dot
static void draw_particle_sparkle(int cx, int cy, float t, int s = 1) {
    auto& d = M5Cardputer.Display;
    uint16_t gold = COL_GOLD;
    int r = (1 + (int)(2.5f * (1.0f - fabsf(2.0f * t - 1.0f)))) * s;  // arm length
    d.fillRect(cx, cy - r, s, 2*r + s, gold);           // vertical arm
    d.fillRect(cx - r, cy, 2*r + s, s, gold);           // horizontal arm
    fpx(cx - s, cy - s, s, COL_GOLD_DARK);              // diagonal hints
    fpx(cx + s, cy - s, s, COL_GOLD_DARK);
    fpx(cx - s, cy + s, s, COL_GOLD_DARK);
    fpx(cx + s, cy + s, s, COL_GOLD_DARK);
    // Orbiting satellite
    int rad = 8 * s;
    int ox = cx + (int)(rad * cosf(t * 6.28f));
    int oy = cy + (int)(rad * sinf(t * 6.28f));
    fpx(ox,     oy,     s, gold);
    fpx(ox - s, oy,     s, gold);
    fpx(ox + s, oy,     s, gold);
    fpx(ox,     oy - s, s, gold);
    fpx(ox,     oy + s, s, gold);
}

// HEART: pixel heart floating upward
static void draw_particle_heart(int cx, int cy, float t, int s = 1) {
    static const uint8_t HEART[6] = {
        0b0110110,
        0b1111111,
        0b1111111,
        0b0111110,
        0b0011100,
        0b0001000,
    };
    int dy = (int)(-12.0f * t) * s;
    plot_pattern(cx, cy + dy, 7, 6, HEART, COL_PINK, s);
}

// LEAF: pixel leaf drifting down with sway
static void draw_particle_leaf(int cx, int cy, float t, int s = 1) {
    static const uint8_t LEAF[6] = {
        0b00111000,
        0b01111100,
        0b01111110,
        0b00111100,
        0b00011100,
        0b00001100,
    };
    uint16_t green = RGB565(0x6A, 0xB0, 0x4C);
    int dy = (int)(10.0f * t) * s;
    int dx = (int)(3.0f * sinf(t * 6.28f * 2)) * s;
    plot_pattern(cx + dx, cy + dy, 8, 6, LEAF, green, s);
}

// BUTTERFLY: two wings flutter + zigzag
static void draw_particle_butterfly(int cx, int cy, float t, int s = 1) {
    uint16_t purple = RGB565(0x9F, 0x7A, 0xEA);
    auto& d = M5Cardputer.Display;
    int dx = (int)(6.0f * sinf(t * 6.28f * 2)) * s;
    int wing_phase = (int)(t * 8) % 2;
    int by = cy - 2*s;
    int bx = cx + dx;
    // Body
    d.fillRect(bx, by - s, s, 4*s, purple);
    if (wing_phase == 0) {
        d.fillRect(bx - 4*s, by - s, 3*s, 3*s, purple);
        d.fillRect(bx + 2*s, by - s, 3*s, 3*s, purple);
        fpx(bx - 4*s, by + 2*s, s, purple);
        fpx(bx + 4*s, by + 2*s, s, purple);
    } else {
        d.fillRect(bx - 2*s, by - s, 2*s, 4*s, purple);
        d.fillRect(bx + s,   by - s, 2*s, 4*s, purple);
    }
}

// Dispatcher — s=1 for HI Completed overlay, s=2 for entrance
static void draw_particle(int particle_id, int cx, int cy, float t, int s = 1) {
    switch (particle_id) {
        case 0: draw_particle_sparkle  (cx, cy, t, s); break;
        case 1: draw_particle_heart    (cx, cy, t, s); break;
        case 2: draw_particle_leaf     (cx, cy, t, s); break;
        case 3: draw_particle_butterfly(cx, cy, t, s); break;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Action icons (used in Quest REVEAL + HI Completed)
// ──────────────────────────────────────────────────────────────────────────────

// GIFT box (wrapped present, ~10x10)
static void draw_icon_gift(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    uint16_t box = RGB565(0xF8, 0x88, 0x44);   // warm orange box
    uint16_t ribbon = COL_GOLD;
    uint16_t bow = COL_PINK;
    // Box body
    d.fillRect(cx - 5, cy - 2, 10, 7, box);
    // Vertical ribbon
    d.fillRect(cx - 1, cy - 2, 2, 7, ribbon);
    // Horizontal ribbon
    d.fillRect(cx - 5, cy + 0, 10, 2, ribbon);
    // Bow on top
    d.fillRect(cx - 2, cy - 4, 5, 2, bow);
    d.drawPixel(cx - 3, cy - 5, bow);
    d.drawPixel(cx + 3, cy - 5, bow);
    // Outline
    d.drawRect(cx - 5, cy - 2, 10, 7, COL_BLACK);
}

// PHONE (rectangular phone, ~6x12)
static void draw_icon_phone(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    uint16_t body = RGB565(0x88, 0x88, 0xAA);  // grey-purple
    uint16_t screen_c = RGB565(0x88, 0xCC, 0xFF);  // light blue screen
    // Body
    d.fillRect(cx - 3, cy - 6, 6, 12, body);
    // Screen
    d.fillRect(cx - 2, cy - 4, 4, 7, screen_c);
    // Speaker hole at top
    d.drawFastHLine(cx - 1, cy - 5, 2, COL_FRAME_BG);
    // Button at bottom
    d.fillRect(cx - 1, cy + 4, 2, 1, COL_FRAME_BG);
    // Outline
    d.drawRect(cx - 3, cy - 6, 6, 12, COL_BLACK);
}

// PUSH CART (shopping cart with basket + wheels, ~14x10)
static void draw_icon_cart(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    uint16_t cart = RGB565(0xC0, 0xC0, 0xC8);  // light grey
    // Basket (trapezoid via lines)
    d.drawLine(cx - 5, cy - 3, cx + 5, cy - 3, cart);
    d.drawLine(cx - 4, cy + 1, cx + 4, cy + 1, cart);
    d.drawLine(cx - 5, cy - 3, cx - 4, cy + 1, cart);
    d.drawLine(cx + 5, cy - 3, cx + 4, cy + 1, cart);
    // 2 horizontal bars inside basket
    d.drawLine(cx - 4, cy - 1, cx + 4, cy - 1, cart);
    // Handle (going up-back-left)
    d.drawLine(cx - 5, cy - 3, cx - 6, cy - 5, cart);
    d.drawLine(cx - 6, cy - 5, cx - 7, cy - 4, cart);
    // 2 wheels
    d.fillRect(cx - 4, cy + 2, 2, 2, cart);
    d.fillRect(cx + 2, cy + 2, 2, 2, cart);
    d.drawPixel(cx - 3, cy + 4, COL_BLACK);
    d.drawPixel(cx + 3, cy + 4, COL_BLACK);
}

// Dispatcher
static void draw_icon(int action_id, int cx, int cy) {
    switch (action_id) {
        case 0: draw_icon_gift(cx, cy); break;
        case 1: draw_icon_phone(cx, cy); break;
        case 2: draw_icon_cart(cx, cy); break;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Live Mirror jump animation (triggered by shake)
// ──────────────────────────────────────────────────────────────────────────────
static uint32_t g_jump_started_at = 0;
static const uint32_t JUMP_DURATION_MS = 400;

static int compute_jump_offset() {
    if (g_jump_started_at == 0) return 0;
    uint32_t elapsed = millis() - g_jump_started_at;
    if (elapsed >= JUMP_DURATION_MS) {
        g_jump_started_at = 0;
        return 0;
    }
    // Parabola: 0..1..0 over duration. Peak at 16 px.
    float t = (float)elapsed / (float)JUMP_DURATION_MS;
    float h = 4.0f * t * (1.0f - t);  // 0..1..0
    return (int)(16.0f * h);
}

// ──────────────────────────────────────────────────────────────────────────────
// Move mode (M 键进入：倾斜走小人在大屏地图上移动；DEL/← 退出)。
// 移植自 v1 handleIMU 的 tilt 段：倾斜=缓慢改重力方向（≈1g），与 shake=jump（瞬时远离 1g）
// 物理天然区分；再加 opt-in（只有进 move mode 才采 tilt）双保险，平时手抖小人不会飘。
// 服务端 _handle_move 已含 cooldown + 画布 clamp；大屏 case 'move' 累加渲染（链路 P1 就位）。
// ──────────────────────────────────────────────────────────────────────────────
static bool     g_move_mode = false;
static uint32_t g_move_last_ms = 0;
static uint32_t g_move_pause_until = 0;  // shake 后暂停 tilt 到此时刻
static float    g_move_sent_dx = 0;
static float    g_move_sent_dy = 0;
static const float    MOVE_DEAD_ZONE   = 0.18f;  // 死区：小倾斜不动（防误触）
static const uint32_t MOVE_INTERVAL_MS = 500;    // 节流：每 500ms 最多发一次（对齐 v1）

// 采当前倾斜 → 量化 → 只在方向变化（或归零）时发 move。需在 move mode 下每帧调。
static void sample_tilt_and_publish() {
    if (millis() - g_move_last_ms < MOVE_INTERVAL_MS) return;
    if (millis() < g_move_pause_until) return;   // shake 暂停期间不采样
    g_move_last_ms = millis();

    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.getAccelData(&ax, &ay, &az)) return;

    // Cardputer rotation=1（横屏键盘朝自己）：左倾=ax+→屏幕左，前倾=ay-，取反适配大屏坐标（同 v1）。
    float dx = (fabsf(ax) > MOVE_DEAD_ZONE) ? constrain(-ax, -1.0f, 1.0f) : 0.0f;
    float dy = (fabsf(ay) > MOVE_DEAD_ZONE) ? constrain(ay, -1.0f, 1.0f) : 0.0f;
    // 量化到 0.05 步进，减少无意义更新。
    dx = roundf(dx * 20.0f) / 20.0f;
    dy = roundf(dy * 20.0f) / 20.0f;

    // server/web 是**离散积分**（每包 m.x += dx*step）→ 持续倾斜要持续发包才能持续走。
    // 所以非零方向每节流周期都发；只跳过重复的"停"（0,0）防 idle 空发 spam（Codex P1）。
    // 首次归零会发一包 (0,0) 告诉 server "我停了"（顺带满足 🦞 归零包 nit）。
    if (dx == 0.0f && dy == 0.0f && g_move_sent_dx == 0.0f && g_move_sent_dy == 0.0f) return;
    g_move_sent_dx = dx;
    g_move_sent_dy = dy;
    net_publish_move(dx, dy);
}

// ──────────────────────────────────────────────────────────────────────────────
// Compose character with optional closed-eye override (sleep state).
// Eyes layer (L=1): instead of the baked sprite, draw two thin horizontal
// pixel lines at the known eye position to mimic LimeZu's r6 closed-eye frame.
// ──────────────────────────────────────────────────────────────────────────────
static void compose_character_ex(bool closed_eyes) {
    for (int i = 0; i < SPRITE_PIXELS; ++i) g_composite[i] = 0;
    for (int i = 0; i < SPRITE_MASK_BYTES; ++i) g_composite_mask[i] = 0;

    for (int L = 0; L < (int)SPR_LAYER_COUNT; ++L) {
        int sIdx = g_shape[L];
        if (sIdx < 0 || sIdx >= (int)SPR_LAYERS[L].shape_count) continue;
        const SpriteShape& sh = SPR_LAYERS[L].shapes[sIdx];
        if (sh.count == 0) continue;
        int cIdx = clamp_color(L, sIdx);

        if (closed_eyes && L == 1) {
            // Eyes layer is skipped entirely — we'll overwrite the body's
            // eye outline pixels after the full compose loop.
            continue;
        }

        const SpriteVariant* v = &sh.variants[cIdx];
        if (!v || !v->pixels || !v->mask) continue;
        for (int i = 0; i < SPRITE_PIXELS; ++i) {
            if (variant_mask_bit(v->mask, i)) {
                g_composite[i] = pgm_read_word(&v->pixels[i]);
                g_composite_mask[i >> 3] |= (1 << (i & 7));
            }
        }
    }

    // Post-compose: if sleeping, erase body's vertical eye outlines + draw horizontal closed lines
    if (closed_eyes) {
        uint16_t skin = g_composite[38 * SPRITE_W + 12];
        // Body outline pixels: left eye x=10,11 / right eye x=20,21 / y=40..43
        static const int EYE_X[4] = {10, 11, 20, 21};
        for (int ei = 0; ei < 4; ++ei) {
            int ex = EYE_X[ei];
            for (int ey = 40; ey <= 43; ++ey) {
                int idx = ey * SPRITE_W + ex;
                g_composite[idx] = skin;
                g_composite_mask[idx >> 3] |= (1 << (idx & 7));
            }
        }
        // Draw horizontal closed-eye lines: left x=9..12, right x=21..24, y=42 only
        const SpriteShape& eye_sh = SPR_LAYERS[1].shapes[g_shape[1]];
        uint16_t eye_color = (eye_sh.count > 0)
            ? eye_sh.variants[clamp_color(1, g_shape[1])].swatch
            : RGB565(0x3A, 0x2C, 0x1A);
        static const int LEFT_EYE_CLOSED[4]  = {9, 10, 11, 12};
        static const int RIGHT_EYE_CLOSED[4] = {19, 20, 21, 22};
        for (int ei = 0; ei < 4; ++ei) {
            for (int ey = 42; ey <= 42; ++ey) {
                int idx = ey * SPRITE_W + LEFT_EYE_CLOSED[ei];
                g_composite[idx] = eye_color;
                g_composite_mask[idx >> 3] |= (1 << (idx & 7));
                idx = ey * SPRITE_W + RIGHT_EYE_CLOSED[ei];
                g_composite[idx] = eye_color;
                g_composite_mask[idx >> 3] |= (1 << (idx & 7));
            }
        }
    }
}

// Compose carry animation frame: body uses carry sprite, all other layers use
// their idle front frame. Outfit arm columns (0-9, 23-31) in rows 28-55 are
// skipped so carry body arms show through without the idle outfit arm overlapping.
static void compose_character_carry(int frame_idx) {
    for (int i = 0; i < SPRITE_PIXELS; ++i) g_composite[i] = 0;
    for (int i = 0; i < SPRITE_MASK_BYTES; ++i) g_composite_mask[i] = 0;

    // Layer 0: body — carry frame from flash
    int body_idx = g_shape[0];
    if (body_idx >= 0 && body_idx < 9) {
        const SpriteCarryFrame& cf = SPR_CARRY_BODY[body_idx][frame_idx];
        if (cf.pixels && cf.mask) {
            for (int i = 0; i < SPRITE_PIXELS; ++i) {
                if (variant_mask_bit(cf.mask, i)) {
                    g_composite[i] = pgm_read_word(&cf.pixels[i]);
                    g_composite_mask[i >> 3] |= (1 << (i & 7));
                }
            }
        }
    }

    // Layers 1-4: idle front frame
    for (int L = 1; L < (int)SPR_LAYER_COUNT; ++L) {
        int sIdx = g_shape[L];
        if (sIdx < 0 || sIdx >= (int)SPR_LAYERS[L].shape_count) continue;
        const SpriteVariant* v = layer_variant(L);
        if (!v || !v->pixels || !v->mask) continue;
        for (int i = 0; i < SPRITE_PIXELS; ++i) {
            // Outfit (L=2): skip arm columns in arm rows so carry arm shows through
            if (L == 2) {
                int row = i / SPRITE_W;
                int col = i % SPRITE_W;
                if (row >= 28 && row <= 55 && (col < 10 || col >= 23)) continue;
            }
            if (variant_mask_bit(v->mask, i)) {
                g_composite[i] = pgm_read_word(&v->pixels[i]);
                g_composite_mask[i >> 3] |= (1 << (i & 7));
            }
        }
    }
}


static uint32_t g_static_since  = 0;
static const uint32_t SLEEP_DELAY_MS = 3000;  // 3s still → fall asleep

static uint8_t  g_zzz_stage   = 0;            // 0=none, 1=z, 2=zz, 3=zzz
static uint32_t g_zzz_next_at = 0;
static const uint32_t ZZZ_INTERVAL_MS = 1500;

static bool is_sleeping() {
    return g_static_since != 0 &&
           (millis() - g_static_since) >= SLEEP_DELAY_MS;
}

// Call once per loop with current IMU held state.
// Returns true if sleep state CHANGED (need full redraw of character).
static bool update_sleep_state(bool currently_held) {
    bool was_sleeping = is_sleeping();
    if (currently_held) {
        if (g_static_since != 0) {
            g_static_since = 0;
            g_zzz_stage    = 0;
            g_zzz_next_at  = 0;
        }
    } else {
        if (g_static_since == 0) {
            g_static_since = millis();
            g_zzz_next_at  = g_static_since + SLEEP_DELAY_MS;
        }
        // Advance zzz stages once asleep
        if (is_sleeping() && millis() >= g_zzz_next_at) {
            g_zzz_stage   = (g_zzz_stage % 3) + 1;  // 1→2→3→1
            g_zzz_next_at = millis() + ZZZ_INTERVAL_MS;
            return true;  // need zzz redraw
        }
    }
    return (was_sleeping != is_sleeping());  // transition changed
}

// Draw zzz above character head, centered on sprite, staying within stage width.
static void draw_zzz(int sx, int sy) {
    if (g_zzz_stage == 0) return;
    const char* ztext[4] = {"", "z", "zz", "zzz"};
    auto& d = M5Cardputer.Display;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(middle_center);
    uint16_t col = (g_zzz_stage == 3) ? COL_LAVENDER : COL_LAVENDER_DK;
    // Center above the sprite head: sprite is 64px wide (32×2), head is at top ~20px
    int cx = sx + SPRITE_W;       // center-x of sprite (32 * scale/2 = 32)
    int cy = sy - 8;              // 8px above top of sprite
    d.setTextColor(col, COL_FRAME_BG);
    d.drawString(ztext[g_zzz_stage], cx, cy);
}


// ──────────────────────────────────────────────────────────────────────────────
// P1-08: Arrival Transition (Loading → Countdown)
// Uses partial redraw (g_first_paint) to avoid flicker
// ──────────────────────────────────────────────────────────────────────────────
static int g_arrival_last_frame = -1;
static int g_arrival_last_sec = -1;
static ArrivalState g_arrival_last_state = ARRIVAL_LOADING;

static void draw_p1_08_arrival() {
    auto& d = M5Cardputer.Display;
    uint32_t elapsed = millis() - g_screen_entered_ms;

    // Detect state transition
    if (g_arrival_state != g_arrival_last_state) {
        g_first_paint = true;
        g_arrival_last_state = g_arrival_state;
    }

    if (g_first_paint) {
        d.fillScreen(COL_FRAME_BG);
        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        if (g_arrival_state == ARRIVAL_LOADING) {
            d.setTextSize(1);
            d.setTextColor(COL_LAVENDER_DK);
            d.drawString("preparing your arrival...", SCREEN_W/2, 88);
        } else {
            d.setTextSize(2);
            d.setTextColor(COL_GOLD);
            d.drawString("^^^", SCREEN_W/2, 24);
            d.drawString("LOOK AT BIG SCREEN", SCREEN_W/2, 50);
            d.drawString("^^^", SCREEN_W/2, 78);
            d.setTextSize(1);
            d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
            if (g_island >= 0) {
                char buf[40];
                snprintf(buf, sizeof(buf), "landing on %s", ISLANDS[g_island].name);
                d.drawString(buf, SCREEN_W/2, 102);
            }
        }
        g_first_paint = false;
        g_arrival_last_frame = -1;
        g_arrival_last_sec = -1;
    }

    if (g_arrival_state == ARRIVAL_LOADING) {
        // Update spinner frame only
        const char* spinner[4] = {"|", "/", "-", "\\"};
        int frame = (elapsed / 150) % 4;
        if (frame != g_arrival_last_frame) {
            d.setFont(&fonts::Font0);
            d.setTextDatum(middle_center);
            d.setTextSize(3);
            d.fillRect(SCREEN_W/2 - 12, 38, 24, 26, COL_FRAME_BG);
            d.setTextColor(COL_GOLD, COL_FRAME_BG);
            d.drawString(spinner[frame], SCREEN_W/2, 50);
            g_arrival_last_frame = frame;
        }
        if (elapsed >= 800) {
            g_arrival_state = ARRIVAL_COUNTDOWN;
            g_screen_entered_ms = millis();
        }
    } else {
        // Update countdown digit only
        int sec_left = 2 - (int)(elapsed / 1000);
        if (sec_left < 0) sec_left = 0;
        if (sec_left != g_arrival_last_sec) {
            d.setFont(&fonts::Font0);
            d.setTextDatum(middle_center);
            d.setTextSize(2);
            d.fillRect(SCREEN_W/2 - 20, 116, 40, 16, COL_FRAME_BG);
            d.setTextColor(COL_GOLD, COL_FRAME_BG);
            char cbuf[8];
            snprintf(cbuf, sizeof(cbuf), "%d...", sec_left);
            d.drawString(cbuf, SCREEN_W/2, 122);
            g_arrival_last_sec = sec_left;
        }
        if (elapsed >= 2000) {
            goto_screen(P2_01_LIVE_MIRROR);
        }
    }
}

static void input_p1_08_arrival(bool enter_pressed, bool del_pressed) {
    // All keys ignored — strict transition (per PRD)
    (void)enter_pressed; (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-01: Live Mirror — Base
// ──────────────────────────────────────────────────────────────────────────────
// Signature label arrays (declared early so sidebar can show "sig: GIFT" etc.)
static const char* SIG_ACTION_NAMES[3]   = {"GIFT", "PHONE", "PUSH-CART"};
static const char* SIG_ACTION_ICONS[3]   = {"+", "#", "%"};
static const char* SIG_PARTICLE_NAMES[4] = {"SPARKLE", "HEART", "LEAF", "BUTTERFLY"};
static const char* SIG_PARTICLE_ICONS[4] = {"*", "<3", "%%", "~~"};

static int g_lm_last_jump_offset = -999;
static int g_lm_last_breath_offset = -999;
static bool    g_lm_last_sleeping  = false;
static uint8_t g_lm_last_zzz_stage = 99;
static uint8_t  g_sig_cmd_display_len    = 255;  // 保留作为哨兵（旧 /sig overlay 已移除）
// S 屏（signature 切换，彩蛋 口口相传）键入状态
static char     g_sig_input[12]   = "";   // 键入的粒子名
static uint8_t  g_sig_input_len   = 0;
static int8_t   g_sig_input_err   = 0;    // 0=正常 1=不在背包提示

// Entrance signature animation: plays once when Live Mirror first loads.
static const uint32_t SIG_ENTRANCE_MS = 2200;
static bool     g_lm_sig_entrance_done  = false;
static float    g_entrance_last_t       = -1.0f;
static uint32_t g_entrance_last_draw_ms = 0;


// LIVE MIRROR — split-screen layout
//   Left side: pixel ground + character only (full 135px height, no overlay UI)
//   Right side: status + signature + keys + collection grid (all in one column)
static const int LM_SIDEBAR_W = 100;
static const int LM_STAGE_W   = SCREEN_W - LM_SIDEBAR_W;  // 140

// 画/清 move banner（顶部 14px 横幅）。on=true 画金条提示；on=false 清掉 → 还原像素地面。
// 直接 inline 操作顶条，避免走 first_paint 重放 2.2s 入场动画（期间输入被忽略，tilt 会停摆）。
static void draw_move_banner(bool on) {
    auto& d = M5Cardputer.Display;
    int by = SCREEN_H - 14;   // 底部 14px 横幅（防与小人顶部重叠）
    if (on) {
        d.fillRect(0, by, LM_STAGE_W, 14, COL_GOLD);
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextDatum(middle_left);
        d.setTextColor(COL_FRAME_BG, COL_GOLD);
        d.drawString("MOVE  tilt to walk", 4, by + 7);
        d.setTextDatum(middle_right);
        d.drawString("DEL=exit", LM_STAGE_W - 3, by + 7);
        d.setTextDatum(top_left);
    } else {
        d.fillRect(0, by, LM_STAGE_W, 14, COL_FRAME_BG);
        for (int gy = by + 2; gy < by + 14; gy += 6)
            for (int gx = 2; gx < LM_STAGE_W; gx += 6)
                d.drawPixel(gx, gy, COL_LEFT_BG_B);
    }
}

static void draw_lm_sidebar() {
    auto& d = M5Cardputer.Display;
    int x0 = LM_STAGE_W;
    int w  = LM_SIDEBAR_W;

    d.fillRect(x0, 0, w, SCREEN_H, COL_PANEL_BG);
    d.drawFastVLine(x0, 0, SCREEN_H, COL_BLACK);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(top_left);

    int y = 4;

    // 1) Username
    d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
    d.drawString(g_username[0] ? g_username : "(no name)", x0 + 4, y);
    y += 11;

    // 2) Island chip (color dot + name)
    if (g_island >= 0 && g_island < 6) {
        const IslandInfo& isle = ISLANDS[g_island];
        d.fillRect(x0 + 4, y + 1, 6, 6, isle.color);
        d.drawRect(x0 + 4, y + 1, 6, 6, COL_BLACK);
        d.setTextColor(isle.color, COL_PANEL_BG);
        d.drawString(isle.name, x0 + 14, y);
    }
    y += 13;

    // 3) Divider
    d.drawFastHLine(x0 + 4, y, w - 8, COL_LEFT_BG_B);
    y += 4;

    // 4) Signature particle status
    if (g_sig_particle < 0) {
        d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
        d.drawString("sig: --", x0 + 4, y);
    } else {
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        char buf[24];
        snprintf(buf, sizeof(buf), "sig: %s", SIG_PARTICLE_NAMES[g_sig_particle]);
        d.drawString(buf, x0 + 4, y);
    }
    y += 13;

    // 5) Key shortcuts
    auto row = [&](const char* key, const char* label) {
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        d.drawString(key, x0 + 4, y);
        d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
        d.drawString(label, x0 + 16, y);
        y += 10;
    };
    row("H", "Hi");
    row("A", "Anonymous");
    row("J", "Jump");
    row("M", "Move");

    y += 2;
    d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
    d.drawString("shake=jump", x0 + 4, y);

    // 6) Collection grid at bottom — centered in sidebar
    int cell_w = 16, cell_h = 12, gap = 2;
    int total = 5 * cell_w + 4 * gap;
    int gx0 = x0 + (w - total) / 2;
    int gy = SCREEN_H - cell_h - 4;
    for (int i = 0; i < 5; ++i) {
        int cx = gx0 + i * (cell_w + gap);
        bool is_home = (i == 0);
        uint16_t border = is_home ? COL_GOLD : COL_BLACK;
        if (g_collection[i] >= 0 && g_collection[i] < 6) {
            d.fillRect(cx, gy, cell_w, cell_h, ISLANDS[g_collection[i]].color);
        } else {
            d.fillRect(cx, gy, cell_w, cell_h, COL_FRAME_BG);
        }
        d.drawRect(cx, gy, cell_w, cell_h, border);
    }
}

static void draw_p2_01_live_mirror() {
    auto& d = M5Cardputer.Display;

    int stage_y = 0;
    int stage_h = SCREEN_H;
    int sprite_w = SPRITE_W * 2;
    int sprite_h = (64 - 12) * 2;
    int sx = (LM_STAGE_W - sprite_w) / 2;
    int sy_base = stage_y + (stage_h - sprite_h) / 2;

    // ── Entrance signature animation (first SIG_ENTRANCE_MS after entering screen) ──
    uint32_t elapsed_since_enter = millis() - g_screen_entered_ms;

    bool held = is_held();
    bool sleeping = is_sleeping();
    int jump_off   = (sleeping || !g_lm_sig_entrance_done) ? 0 : compute_jump_offset();
    int breath_off = (sleeping || jump_off > 0 || !g_lm_sig_entrance_done) ? 0 : compute_breathing_offset();
    int sy = sy_base - jump_off + breath_off;

    if (g_first_paint) {
        g_lm_sig_entrance_done  = false;
        g_entrance_last_t       = -1.0f;
        g_entrance_last_draw_ms = 0;
        d.fillScreen(COL_FRAME_BG);
        draw_pixel_ground(0, stage_y, LM_STAGE_W, stage_h);
        draw_lm_sidebar();
        compose_character_ex(false);
        blit_composite_opaque(sx, sy_base, 2, RENDER_Y_START, RENDER_Y_END);
        if (g_move_mode) draw_move_banner(true);   // 重进屏仍在移动模式时补 banner（防御）
        g_first_paint = false;
        g_lm_last_jump_offset   = 0;
        g_lm_last_breath_offset = 0;
        g_lm_last_sleeping      = false;
        g_lm_last_zzz_stage     = g_zzz_stage;
        return;
    }

    // ── Entrance: animate particle with minimal dirty-region update ───────────
    if (!g_lm_sig_entrance_done) {
        if (elapsed_since_enter >= SIG_ENTRANCE_MS) {
            // Entrance finished — clean up particle region and mark done
            int pcx = sx + sprite_w / 2;
            int pcy = sy_base + 20;
            int bx = max(0,          pcx - 22);
            int bw = min(LM_STAGE_W, pcx + 22) - bx;
            int by = max(0,          pcy - 30);
            int bh = min(SCREEN_H,   pcy + 27) - by;
            d.fillRect(bx, by, bw, bh, COL_FRAME_BG);
            for (int gy = (by / 6) * 6 + 2; gy < by + bh; gy += 6)
                for (int gx = (bx / 6) * 6 + 2; gx < bx + bw; gx += 6)
                    if (gx >= 0 && gx < LM_STAGE_W && gy >= 0)
                        d.drawPixel(gx, gy, COL_LEFT_BG_B);
            int sp_y0 = RENDER_Y_START + max(0, (by - sy_base) / 2);
            int sp_y1 = min(RENDER_Y_END, sp_y0 + bh / 2 + 2);
            if (sp_y0 < sp_y1)
                blit_composite_opaque(sx, sy_base + (sp_y0 - RENDER_Y_START) * 2,
                                      2, sp_y0, sp_y1);
            g_lm_sig_entrance_done = true;
            g_entrance_last_t      = -1.0f;
            // Fall through to normal rendering below
        } else {
            // Still animating
            if (g_sig_particle >= 0) {
                uint32_t now = millis();
                if (now - g_entrance_last_draw_ms >= 40 || g_entrance_last_t < 0.0f) {
                    float t = (float)(elapsed_since_enter % 1000) / 1000.0f;
                    int pcx = sx + sprite_w / 2;
                    int pcy = sy_base + 20;

                    int bx = max(0,          pcx - 22);
                    int bw = min(LM_STAGE_W, pcx + 22) - bx;
                    int by = max(0,          pcy - 30);
                    int bh = min(SCREEN_H,   pcy + 27) - by;

                    d.fillRect(bx, by, bw, bh, COL_FRAME_BG);
                    for (int gy = (by / 6) * 6 + 2; gy < by + bh; gy += 6)
                        for (int gx = (bx / 6) * 6 + 2; gx < bx + bw; gx += 6)
                            if (gx >= 0 && gx < LM_STAGE_W && gy >= 0)
                                d.drawPixel(gx, gy, COL_LEFT_BG_B);

                    int sp_y0 = RENDER_Y_START + max(0, (by - sy_base) / 2);
                    int sp_y1 = RENDER_Y_START + (by + bh - sy_base + 1) / 2 + 1;
                    if (sp_y0 < RENDER_Y_START) sp_y0 = RENDER_Y_START;
                    if (sp_y1 > RENDER_Y_END)   sp_y1 = RENDER_Y_END;
                    if (sp_y0 < sp_y1) {
                        int dst_y_p = sy_base + (sp_y0 - RENDER_Y_START) * 2;
                        blit_composite_opaque(sx, dst_y_p, 2, sp_y0, sp_y1);
                    }

                    draw_particle(g_sig_particle, pcx, pcy, t, 2);

                    g_entrance_last_t       = t;
                    g_entrance_last_draw_ms = now;
                }
            }
            return;  // skip normal position update while animating
        }
    }

    // ── Normal IMU-driven animation ───────────────────────────────────────────
    bool pos_changed     = (jump_off != g_lm_last_jump_offset || breath_off != g_lm_last_breath_offset);
    bool sleep_changed   = (sleeping != g_lm_last_sleeping);
    bool zzz_changed     = (g_zzz_stage != g_lm_last_zzz_stage);

    if (pos_changed || sleep_changed) {
        bool is_jumping = (jump_off > 0) || (g_lm_last_jump_offset > 0);
        if (is_jumping || sleep_changed) {
            int erase_top = sy_base - 17;
            int erase_h   = sprite_h + 18;
            // Clamp width so we never draw pixel-ground dots into the sidebar
            int erase_x   = sx - 4;
            int erase_w   = sprite_w + 8;
            if (erase_x + erase_w > LM_STAGE_W) erase_w = LM_STAGE_W - erase_x;
            draw_pixel_ground(erase_x, erase_top, erase_w, erase_h);
        } else {
            int prev_sy = sy_base - g_lm_last_jump_offset + g_lm_last_breath_offset;
            int dy = sy - prev_sy;
            if (dy < 0) draw_pixel_ground(sx, prev_sy + sprite_h + dy, sprite_w, -dy);
            else if (dy > 0) draw_pixel_ground(sx, prev_sy, sprite_w, dy);
        }
        // Always restore sidebar border after erasing
        M5Cardputer.Display.drawFastVLine(LM_STAGE_W, 0, SCREEN_H, COL_BLACK);
        compose_character_ex(sleeping);
        blit_composite_opaque(sx, sy, 2, 12, 64);
        if (sleeping) draw_zzz(sx, sy);
        g_lm_last_jump_offset   = jump_off;
        g_lm_last_breath_offset = breath_off;
        g_lm_last_sleeping      = sleeping;
        g_lm_last_zzz_stage     = g_zzz_stage;
    } else if (zzz_changed && sleeping) {
        int cx = sx + SPRITE_W - 10;
        int ew = 28;
        if (cx + ew > LM_STAGE_W) ew = LM_STAGE_W - cx;
        if (ew > 0) M5Cardputer.Display.fillRect(cx, sy - 12, ew, 12, COL_FRAME_BG);
        // Restore sidebar border
        M5Cardputer.Display.drawFastVLine(LM_STAGE_W, 0, SCREEN_H, COL_BLACK);
        draw_zzz(sx, sy);
        g_lm_last_zzz_stage = g_zzz_stage;
    }
}

static void input_p2_01_live_mirror(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    // Update sleep state (IMU-based) — may mark dirty
    bool held = is_held();
    bool state_changed = update_sleep_state(held);
    if (state_changed) g_dirty = true;

    // Shake → wake up + jump (blocked during entrance animation)
    if (!g_lm_sig_entrance_done) return;  // ignore all input during entrance

    // ── Move mode：倾斜走小人。进入后独占输入（tilt→move + DEL 退出），抑制 shake=jump。 ──
    if (g_move_mode) {
        if (detect_shake_event()) {
            net_publish_shake();  // M-mode shake = sig 交换（不是集体 JUMP）
            // shake 时停止移动 1s（v1 同逻辑：晃动 = 站定交互，不是走路）
            g_move_pause_until = millis() + 1000;  // 暂停 tilt 采样 1s（shake = 站定交互）
            // 发一包 (0,0) 告诉 server 停下
            if (g_move_sent_dx != 0.0f || g_move_sent_dy != 0.0f) {
                g_move_sent_dx = g_move_sent_dy = 0;
                net_publish_move(0, 0);
            }
        } else {
            sample_tilt_and_publish();
        }
        if (del_pressed) {            // ← / DEL 退出移动模式
            g_move_mode = false;
            g_move_sent_dx = g_move_sent_dy = 0;
            loiter_beep(660, 30);
            draw_move_banner(false);  // inline 清掉 banner（不走 first_paint 防重放入场）
        }
        return;                       // move mode 下不响应 H/A/J/B 与 shake（防走路误触）
    }

    if (detect_shake_event()) {
        if (g_jump_started_at == 0) {
            g_jump_started_at = millis();
            net_publish_jump();   // shake=jump 也参与大屏集体 JUMP（review Codex #2）
            update_sleep_state(true);
            g_dirty = true;
        }
    }
    for (char c : new_keys) {
        char up = (c >= 'a' && c <= 'z') ? c - 32 : c;

        if (up == 'H') {
            loiter_beep(880, 30);   // A5
            // P3b: 进入 HI 键入屏（重置目标 + 结果状态）
            g_hi_sent = false;
            g_hi_target_nick[0] = '\0';
            g_hi_target_len = 0;
            g_hi_outcome = HI_NONE;
            g_hi_roster_sel = 0;
            goto_screen(P2_03_HI_SENT);
            return;
        } else if (up == 'A') {
            loiter_beep(1047, 30);  // C6
            g_say_text[0] = '\0';
            g_say_len = 0;
            goto_screen(P2_06_SAY_INPUT);
            return;
        } else if (up == 'J') {
            loiter_beep(659, 30);   // E5
            net_publish_jump();     // C→S 集体跳（10s 窗口 ≥5 人 → 大屏 jump_burst）
            goto_screen(P2_07_JUMP);
            return;
        } else if (up == 'B') {
            loiter_beep(1175, 30);  // D6
            goto_screen(P3_01_WAITING);
            return;
        } else if (up == 'M') {
            loiter_beep(784, 30);   // G5
            g_move_mode = true;     // 进移动模式：倾斜走小人，DEL 退出（详见 sample_tilt_and_publish）
            g_move_last_ms = 0;     // 立即允许首次采样
            g_move_sent_dx = g_move_sent_dy = 0;
            draw_move_banner(true); // inline 画 banner（从 LIVE_MIRROR 进不是 first_paint，不 inline 会不显 —— Codex P2）
            return;
        } else if (up == 'S') {
            // 彩蛋（口口相传）：进 signature 切换屏，只能切 owned 背包里的粒子
            loiter_beep(988, 30);   // B5
            g_sig_input[0] = '\0';
            g_sig_input_len = 0;
            g_sig_input_err = 0;
            goto_screen(P2_08_SIG_INPUT);
            return;
        }
    }
    (void)enter_pressed; (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-02: Signature Quest (Prompt → Shaking → Reveal)
// ──────────────────────────────────────────────────────────────────────────────

static QuestState g_quest_last_state = QUEST_PROMPT;
static int        g_quest_last_filled = -1;

static void draw_p2_02_signature_quest() {
    auto& d = M5Cardputer.Display;

    // Detect sub-state transition → force full redraw
    if (g_quest_state != g_quest_last_state) {
        g_first_paint = true;
        g_quest_last_state = g_quest_state;
    }

    // Modal box layout (computed each time, cheap)
    int main_y = STATUS_H + 1;
    int main_h = SCREEN_H - COLLECT_H - main_y;
    int box_w = 200, box_h = 90;
    int box_x = (SCREEN_W - box_w) / 2;
    int box_y = main_y + (main_h - box_h) / 2;
    int bar_y = box_y + 56;
    int cell_w = 12, cell_h = 8, cell_gap = 2;
    int bar_total = 10 * cell_w + 9 * cell_gap;
    int bar_x = box_x + (box_w - bar_total) / 2;

    if (g_first_paint) {
        // Full draw: bg + status + ground + grid + modal box + static text
        d.fillScreen(COL_FRAME_BG);
        draw_status_bar();
        draw_pixel_ground(0, main_y, SCREEN_W, main_h);
        draw_collection_grid();

        d.fillRect(box_x, box_y, box_w, box_h, COL_PANEL_BG);
        d.drawRect(box_x, box_y, box_w, box_h, COL_GOLD);

        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextDatum(middle_center);
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        d.drawString("** QUEST **", box_x + box_w/2, box_y + 8);

        if (g_quest_state == QUEST_PROMPT || g_quest_state == QUEST_SHAKING) {
            d.setTextColor(g_quest_state == QUEST_SHAKING ? COL_GOLD : COL_LAVENDER, COL_PANEL_BG);
            d.drawString(g_quest_state == QUEST_SHAKING ? "keep shaking!" : "shake your M5",
                         box_x + box_w/2, box_y + 30);
            d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
            d.drawString("the way you say HI", box_x + box_w/2, box_y + 42);
            // Initial empty bar (cell borders only)
            for (int i = 0; i < 10; ++i) {
                d.fillRect(bar_x + i*(cell_w+cell_gap), bar_y, cell_w, cell_h, COL_LEFT_BG_B);
                d.drawRect(bar_x + i*(cell_w+cell_gap), bar_y, cell_w, cell_h, COL_BLACK);
            }
            d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
            d.drawString("[SPACE] = shake (dev)", box_x + box_w/2, box_y + box_h - 10);
        } else {
            // REVEAL: full static draw (icon + particle preview area)
            d.setTextColor(COL_GOLD, COL_PANEL_BG);
            d.drawString("YOU FOUND IT!", box_x + box_w/2, box_y + 14);

            // Preview area: icon + particle (animated each frame)
            // Just draw the static frame here; particle is updated below
            // (clear the preview area first so animation doesn't accumulate)
            d.fillRect(box_x + box_w/2 - 16, box_y + 22, 32, 28, COL_PANEL_BG);
            draw_icon(g_sig_action, box_x + box_w/2, box_y + 42);
            // Particle drawn live each frame in the partial-update section below

            // Label
            char buf[40];
            snprintf(buf, sizeof(buf), "%s + %s",
                     SIG_ACTION_NAMES[g_sig_action],
                     SIG_PARTICLE_NAMES[g_sig_particle]);
            d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
            d.drawString(buf, box_x + box_w/2, box_y + 60);

            char rr[20];
            snprintf(rr, sizeof(rr), "R: re-roll %d/3", g_reroll_count);
            d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
            d.drawString(rr, box_x + box_w/2 - 50, box_y + box_h - 10);
            d.setTextColor(COL_GOLD, COL_PANEL_BG);
            d.drawString("ENTER: keep", box_x + box_w/2 + 40, box_y + box_h - 10);
        }
        g_first_paint = false;
        g_quest_last_filled = -1;
    }

    // Dynamic: only the energy bar fills during SHAKING
    if (g_quest_state == QUEST_SHAKING) {
        int filled = g_quest_energy / 10;
        if (filled != g_quest_last_filled) {
            for (int i = 0; i < 10; ++i) {
                uint16_t c = (i < filled) ? COL_GOLD : COL_LEFT_BG_B;
                d.fillRect(bar_x + i*(cell_w+cell_gap), bar_y, cell_w, cell_h, c);
                d.drawRect(bar_x + i*(cell_w+cell_gap), bar_y, cell_w, cell_h, COL_BLACK);
            }
            g_quest_last_filled = filled;
        }
    } else if (g_quest_state == QUEST_REVEAL) {
        // Animated particle above the icon (loops over 1.5s)
        uint32_t elapsed = millis() - g_screen_entered_ms;
        float t = (float)(elapsed % 1500) / 1500.0f;
        // Clear small particle area then redraw
        int px = box_x + box_w/2;
        int py = box_y + 30;
        d.fillRect(px - 14, py - 14, 28, 14, COL_PANEL_BG);
        draw_particle(g_sig_particle, px, py, t);
    }
}

static void input_p2_02_signature_quest(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    if (g_quest_state == QUEST_PROMPT) {
        // Any shake or SPACE → enter SHAKING
        bool shaken = detect_shake_event();
        bool spaced = false;
        for (char c : new_keys) if (c == ' ') spaced = true;
        if (shaken || spaced) {
            g_quest_state = QUEST_SHAKING;
            g_quest_energy = (shaken ? 12 : 25);
            g_screen_entered_ms = millis();
            g_dirty = true;
        }
    } else if (g_quest_state == QUEST_SHAKING) {
        // Real IMU shake: +12 per event. SPACE dev key: +25.
        if (detect_shake_event()) {
            g_quest_energy += 12;
            if (g_quest_energy > 100) g_quest_energy = 100;
            g_dirty = true;
        }
        for (char c : new_keys) {
            if (c == ' ') {
                g_quest_energy += 25;
                if (g_quest_energy > 100) g_quest_energy = 100;
                g_dirty = true;
            }
        }
        // No auto-tick anymore — user must shake (or press SPACE for dev)
        if (g_quest_energy >= 100) {
            g_sig_action = (int8_t)(random(0, 3));
            g_sig_particle = (int8_t)(random(0, 4));
            g_quest_state = QUEST_REVEAL;
            g_screen_entered_ms = millis();
            g_dirty = true;
        }
    } else {  // QUEST_REVEAL
        for (char c : new_keys) {
            char up = (c >= 'a' && c <= 'z') ? c - 32 : c;
            if (up == 'R' && g_reroll_count < 3) {
                g_reroll_count++;
                g_quest_state = QUEST_PROMPT;
                g_quest_energy = 0;
                g_sig_action = -1;
                g_sig_particle = -1;
                g_dirty = true;
            }
        }
        if (enter_pressed) {
            goto_screen(P2_01_LIVE_MIRROR);
        }
    }
    (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-03: HI —— 键入目标 nick（g_hi_sent=false）→ 发起后等待回应（g_hi_sent=true，30s）
//   真实链路：键入 nick → net_publish_hi_request_nick → 服务端 nick→uid → 对方 incoming
//   结果由 on_hi_result 回调驱动（matched→P2-05 / declined/expired→P2-05 提示）
// ──────────────────────────────────────────────────────────────────────────────
static int g_p2_03_last_sec = -1;

static void draw_p2_03_hi_sent() {
    auto& d = M5Cardputer.Display;
    int main_y = STATUS_H + 1;
    int main_h = SCREEN_H - COLLECT_H - main_y;

    // ── 选择态：滚动名册列表 ──
    if (!g_hi_sent) {
        d.fillScreen(COL_FRAME_BG);
        draw_status_bar();
        draw_pixel_ground(0, main_y, SCREEN_W, main_h);
        draw_collection_grid();

        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextDatum(middle_center);
        d.setTextColor(COL_GOLD, COL_FRAME_BG);
        d.drawString("SAY HI TO...", SCREEN_W/2, main_y + 6);

        int n = net_roster_count();
        if (n == 0) {
            d.setTextColor(COL_LAVENDER_DK, COL_FRAME_BG);
            d.drawString("(no one online yet)", SCREEN_W/2, main_y + main_h/2);
        } else {
            // 滚动列表：最多显示 5 行，当前选中高亮
            if (g_hi_roster_sel >= n) g_hi_roster_sel = n - 1;
            if (g_hi_roster_sel < 0) g_hi_roster_sel = 0;
            int visible = 5;
            int start = g_hi_roster_sel - visible/2;
            if (start < 0) start = 0;
            if (start + visible > n) start = n - visible;
            if (start < 0) start = 0;
            int y = main_y + 16;
            d.setTextDatum(top_left);
            for (int i = start; i < start + visible && i < n; i++) {
                const char* nk = net_roster_nick(i);
                bool sel = (i == g_hi_roster_sel);
                if (sel) {
                    d.fillRect(4, y - 1, SCREEN_W - 8, 11, COL_GOLD);
                    d.setTextColor(COL_BLACK, COL_GOLD);
                    char row[32];
                    snprintf(row, sizeof(row), "> %s", nk);
                    d.drawString(row, 8, y);
                } else {
                    d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
                    d.drawString(nk, 12, y);
                }
                y += 12;
            }
            // 滚动提示
            d.setTextDatum(bottom_center);
            d.setTextColor(COL_LAVENDER_DK, COL_FRAME_BG);
            char footer[32];
            snprintf(footer, sizeof(footer), "%d/%d  ;=up .=down ENTER=hi", g_hi_roster_sel + 1, n);
            d.drawString(footer, SCREEN_W/2, main_y + main_h - 2);
        }
        return;
    }

    // ── 等待态：发起后 30s 倒计时（结果靠回调） ──
    int box_w = 180, box_h = 92;
    int box_x = (SCREEN_W - box_w) / 2;
    int box_y = main_y + (main_h - box_h) / 2;

    if (g_first_paint) {
        d.fillScreen(COL_FRAME_BG);
        draw_status_bar();
        draw_pixel_ground(0, main_y, SCREEN_W, main_h);
        draw_collection_grid();

        d.fillRect(box_x, box_y, box_w, box_h, COL_PANEL_BG);
        d.drawRect(box_x, box_y, box_w, box_h, COL_GOLD);

        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        d.setTextSize(1);
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        d.drawString("* HI SENT TO", box_x + box_w/2, box_y + 10);

        d.setTextSize(2);
        d.drawString(g_hi_target_nick, box_x + box_w/2, box_y + 30);

        d.setTextSize(1);
        d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
        d.drawString("waiting reply...", box_x + box_w/2, box_y + 50);

        d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
        d.drawString("DEL: cancel", box_x + box_w/2, box_y + box_h - 9);

        g_first_paint = false;
        g_p2_03_last_sec = -1;
    }

    // Dynamic: countdown digit
    uint32_t elapsed = millis() - g_screen_entered_ms;
    int sec = 35 - (int)(elapsed / 1000);   // 比服务端 30s 略长，等 expired 回调兜底
    if (sec < 0) sec = 0;
    if (sec != g_p2_03_last_sec) {
        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        d.setTextSize(1);
        d.fillRect(box_x + 30, box_y + 60, box_w - 60, 10, COL_PANEL_BG);
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        char buf[20];
        snprintf(buf, sizeof(buf), "%d sec", sec);
        d.drawString(buf, box_x + box_w/2, box_y + 65);
        g_p2_03_last_sec = sec;
    }

    if (sec == 0) {
        goto_screen(P2_01_LIVE_MIRROR);   // 兜底：回调没来也退出
    }
}

static void input_p2_03_hi_sent(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    if (g_hi_sent) {
        // 等待态：DEL 取消 → 通知服务端 cancel pending（防对方 accept 后单边换色 — Codex P3）
        if (del_pressed) {
            net_publish_hi_cancel();
            goto_screen(P2_01_LIVE_MIRROR);
        }
        (void)enter_pressed;
        return;
    }
    // 选择态：上下滚动名册
    int n = net_roster_count();
    for (char c : new_keys) {
        char up = (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (up == '.' || c == ',') {               // . , = 下滚（Cardputer 键盘 . 在 ; 下方）
            if (g_hi_roster_sel < n - 1) { g_hi_roster_sel++; g_dirty = true; }
        } else if (up == ';' || c == '/' || c == '\'') {  // ; / ' = 上滚
            if (g_hi_roster_sel > 0) { g_hi_roster_sel--; g_dirty = true; }
        }
    }
    if (del_pressed) {
        goto_screen(P2_01_LIVE_MIRROR);
    }
    if (enter_pressed && n > 0 && g_hi_roster_sel < n) {
        const char* sel_nick = net_roster_nick(g_hi_roster_sel);
        strncpy(g_hi_target_nick, sel_nick, sizeof(g_hi_target_nick) - 1);
        g_hi_target_nick[sizeof(g_hi_target_nick) - 1] = '\0';
        g_hi_target_len = (uint8_t)strlen(g_hi_target_nick);
        net_publish_hi_request_nick(g_hi_target_nick, "");
        g_hi_sent = true;
        g_screen_entered_ms = millis();
        g_first_paint = true;
        loiter_beep(880, 30);
        g_dirty = true;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-04: HI Request Received (mock)
// ──────────────────────────────────────────────────────────────────────────────
static int g_p2_04_last_sec = -1;

static void draw_p2_04_hi_received() {
    auto& d = M5Cardputer.Display;
    int main_y = STATUS_H + 1;
    int main_h = SCREEN_H - COLLECT_H - main_y;
    int box_w = 200, box_h = 90;
    int box_x = (SCREEN_W - box_w) / 2;
    int box_y = main_y + (main_h - box_h) / 2;

    // ── accept 后等服务端 matched ack（防假成功：ack 丢/超时不该播成功动画）──
    if (g_hi_await_ack) {
        if (g_first_paint) {
            d.fillScreen(COL_FRAME_BG);
            draw_status_bar();
            draw_pixel_ground(0, main_y, SCREEN_W, main_h);
            draw_collection_grid();
            d.setFont(&fonts::Font0);
            d.setTextDatum(middle_center);
            d.setTextSize(1);
            d.setTextColor(COL_GOLD, COL_FRAME_BG);
            d.drawString("connecting...", SCREEN_W/2, main_y + main_h/2);
            g_first_paint = false;
        }
        // 本地 5s 兜底：matched 没来（pending 过期/发失败/server 丢）→ 回 LIVE，不假成功
        if (millis() - g_hi_await_since >= 5000) {
            g_hi_await_ack = false;
            g_hi_outcome = HI_NONE;
            goto_screen(P2_01_LIVE_MIRROR);
        }
        return;
    }

    if (g_first_paint) {
        d.fillScreen(COL_FRAME_BG);
        draw_status_bar();
        draw_pixel_ground(0, main_y, SCREEN_W, main_h);
        draw_collection_grid();

        d.fillRect(box_x, box_y, box_w, box_h, COL_PANEL_BG);
        d.drawRect(box_x, box_y, box_w, box_h, COL_GOLD);

        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        d.setTextSize(1);
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        char title[40];
        snprintf(title, sizeof(title), "%s WANTS TO HI", g_hi_in_nick);
        d.drawString(title, box_x + box_w/2, box_y + 12);

        // 发起者岛色色块（incoming 携带的 color）
        int sq = 16;
        d.fillRect(box_x + box_w/2 - sq/2, box_y + 24, sq, sq, g_hi_in_color);
        d.drawRect(box_x + box_w/2 - sq/2, box_y + 24, sq, sq, COL_BLACK);

        d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
        d.drawString("ENTER: accept", box_x + box_w/2 - 50, box_y + box_h - 16);
        d.drawString("DEL: ignore", box_x + box_w/2 + 45, box_y + box_h - 16);

        g_first_paint = false;
        g_p2_04_last_sec = -1;
    }

    // Dynamic: countdown digit only
    uint32_t elapsed = millis() - g_screen_entered_ms;
    int sec = 30 - (int)(elapsed / 1000);
    if (sec < 0) sec = 0;
    if (sec != g_p2_04_last_sec) {
        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        d.setTextSize(1);
        d.fillRect(box_x + 20, box_y + 42, box_w - 40, 12, COL_PANEL_BG);
        d.setTextColor(COL_GOLD, COL_PANEL_BG);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d sec to reply", sec);
        d.drawString(buf, box_x + box_w/2, box_y + 48);
        g_p2_04_last_sec = sec;
    }

    if (sec == 0) {
        goto_screen(P2_01_LIVE_MIRROR);
    }
}

static void input_p2_04_hi_received(bool enter_pressed, bool del_pressed) {
    if (g_hi_await_ack) return;   // 等 ack 时忽略输入
    if (enter_pressed) {
        // 接受 → C→S respond(accept)，进 await ack 态（不立刻播成功，防假成功 — Codex P1）
        net_publish_hi_respond(g_hi_in_requester, true);
        strncpy(g_hi_partner_nick, g_hi_in_nick, sizeof(g_hi_partner_nick) - 1);
        g_hi_partner_nick[sizeof(g_hi_partner_nick) - 1] = '\0';
        g_hi_partner_color = g_hi_in_color;
        g_hi_outcome = HI_NONE;
        g_hi_await_ack = true;
        g_hi_await_since = millis();
        g_first_paint = true;     // 重绘成 connecting 屏
        g_dirty = true;
    }
    if (del_pressed) {
        net_publish_hi_respond(g_hi_in_requester, false);   // 拒绝 → 发起方收 declined
        goto_screen(P2_01_LIVE_MIRROR);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-05: HI Completed
//   Phase 1 (0–1500ms): character plays carry animation + particle — full screen
//   Phase 2 (1500–2500ms): color exchange info + collection update
// ──────────────────────────────────────────────────────────────────────────────
static const uint32_t P205_CARRY_MS  = 1500;
static const uint32_t P205_TOTAL_MS  = 2500;
static int8_t g_p205_phase = -1;       // -1=uninit, 0=carry, 1=exchange
static int    g_p205_last_frame = -1;
static uint32_t g_p205_last_draw_ms = 0;

static void draw_p2_05_hi_completed() {
    auto& d = M5Cardputer.Display;
    uint32_t elapsed = millis() - g_screen_entered_ms;

    // ── declined / expired：简单提示屏，~2s 后自动回 P2-01 ──
    if (g_hi_outcome == HI_DECLINED || g_hi_outcome == HI_EXPIRED) {
        if (g_first_paint) {
            int main_y = STATUS_H + 1;
            int main_h = SCREEN_H - COLLECT_H - main_y;
            d.fillScreen(COL_FRAME_BG);
            draw_status_bar();
            draw_pixel_ground(0, main_y, SCREEN_W, main_h);
            draw_collection_grid();
            d.setFont(&fonts::Font0);
            d.setTextDatum(middle_center);
            d.setTextSize(1);
            d.setTextColor(COL_LAVENDER_DK, COL_FRAME_BG);
            d.drawString(g_hi_outcome == HI_DECLINED ? "they passed this time"
                                                     : "no reply...",
                         SCREEN_W/2, main_y + main_h/2 - 6);
            d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
            d.drawString("the sea is wide. try again",
                         SCREEN_W/2, main_y + main_h/2 + 8);
            g_first_paint = false;
        }
        if (elapsed >= 2000) { g_hi_outcome = HI_NONE; goto_screen(P2_01_LIVE_MIRROR); }
        return;
    }

    if (g_first_paint) {
        d.fillScreen(COL_FRAME_BG);
        g_p205_phase = -1;
        g_p205_last_frame = -1;
        g_p205_last_draw_ms = 0;
        g_first_paint = false;
    }

    // ── Phase 0: carry animation ─────────────────────────────────────────────
    if (elapsed < P205_CARRY_MS) {
        if (g_p205_phase != 0) {
            g_p205_phase = 0;
            d.fillScreen(COL_FRAME_BG);
            // Static text (drawn once)
            d.setFont(&fonts::Font0);
            d.setTextDatum(middle_center);
            d.setTextSize(1);
            d.setTextColor(COL_LAVENDER_DK, COL_FRAME_BG);
            d.drawString("< HI >", SCREEN_W/2, 12);
            char peer_buf[20];
            snprintf(peer_buf, sizeof(peer_buf), "%s", g_hi_partner_nick);
            d.setTextColor(g_hi_partner_color, COL_FRAME_BG);
            d.drawString(peer_buf, SCREEN_W/2, SCREEN_H - 10);
        }

        uint32_t now = millis();
        int frame = (int)(elapsed * SPR_CARRY_FRAMES / P205_CARRY_MS);
        if (frame >= SPR_CARRY_FRAMES) frame = SPR_CARRY_FRAMES - 1;

        if (frame != g_p205_last_frame || now - g_p205_last_draw_ms >= 80) {
            // Character centred on screen
            int sw = SPRITE_W * 2;
            int sh = (RENDER_Y_END - RENDER_Y_START) * 2;
            int sx = (SCREEN_W - sw) / 2;
            int sy = (SCREEN_H - sh) / 2;

            // Erase character + particle area
            int ex = sx - 2, ey = max(0, sy - 35);
            int ew = sw + 4, eh = sh + 40;
            if (ex + ew > SCREEN_W) ew = SCREEN_W - ex;
            if (ey + eh > SCREEN_H) eh = SCREEN_H - ey;
            d.fillRect(ex, ey, ew, eh, COL_FRAME_BG);

            // Carry frame
            compose_character_carry(frame);
            blit_composite_opaque(sx, sy, 2, RENDER_Y_START, RENDER_Y_END);

            // Particle above head (2× scale)
            if (g_sig_particle >= 0) {
                float t = (float)(elapsed % 1000) / 1000.0f;
                draw_particle(g_sig_particle, sx + sw/2, sy - 18, t, 2);
            }

            g_p205_last_frame = frame;
            g_p205_last_draw_ms = now;
        }
        return;
    }

    // ── Phase 1: exchange info ────────────────────────────────────────────────
    if (g_p205_phase != 1) {
        g_p205_phase = 1;
        d.fillScreen(COL_FRAME_BG);
        draw_status_bar();
        // 收集格由 on_hi_result(matched) 回调按服务端权威 slot 更新，这里只渲染。
        draw_collection_grid();

        const IslandInfo& self = (g_island >= 0) ? ISLANDS[g_island] : ISLANDS[0];
        int cy = SCREEN_H / 2 - 8;
        int sq = 22;

        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        // Self island square
        d.fillRect(SCREEN_W/2 - 44, cy - sq/2, sq, sq, self.color);
        d.drawRect(SCREEN_W/2 - 44, cy - sq/2, sq, sq, COL_BLACK);
        // Arrow
        d.setTextSize(2);
        d.setTextColor(COL_GOLD, COL_FRAME_BG);
        d.drawString("<>", SCREEN_W/2, cy);
        // Peer island square
        d.fillRect(SCREEN_W/2 + 22, cy - sq/2, sq, sq, g_hi_partner_color);
        d.drawRect(SCREEN_W/2 + 22, cy - sq/2, sq, sq, COL_BLACK);

        d.setTextSize(1);
        d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
        char buf[32];
        snprintf(buf, sizeof(buf), "you <-> %s", g_hi_partner_nick);
        d.drawString(buf, SCREEN_W/2, cy + 22);
        // 加色 vs 共鸣（同岛/重复/满 → slot=-1）
        if (g_hi_outcome == HI_RESONANCE || g_hi_slot < 0) {
            d.setTextColor(COL_GOLD, COL_FRAME_BG);
            d.drawString("resonance! (same hue)", SCREEN_W/2, cy + 34);
        } else {
            const char* pname = (g_hi_partner_island >= 0 && g_hi_partner_island < 6)
                                ? ISLANDS[g_hi_partner_island].name : "new";
            char add[32];
            snprintf(add, sizeof(add), "+ %s color", pname);
            d.setTextColor(g_hi_partner_color, COL_FRAME_BG);
            d.drawString(add, SCREEN_W/2, cy + 34);
        }
    }

    if (elapsed >= P205_TOTAL_MS) goto_screen(P2_01_LIVE_MIRROR);
}

static void input_p2_05_hi_completed(bool enter_pressed, bool del_pressed) {
    (void)enter_pressed; (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-06: SAY Input (anonymous public message)
// ──────────────────────────────────────────────────────────────────────────────
static void draw_p2_06_say_input() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_FRAME_BG);
    draw_status_bar();
    int main_y = STATUS_H + 1;
    int main_h = SCREEN_H - COLLECT_H - main_y;
    draw_pixel_ground(0, main_y, SCREEN_W, main_h);
    draw_collection_grid();

    // Bottom-anchored input bar
    int bar_h = 28;
    int bar_y = main_y + main_h - bar_h;
    d.fillRect(0, bar_y, SCREEN_W, 2, COL_BLACK);
    d.fillRect(0, bar_y + 2, SCREEN_W, bar_h - 2, COL_PANEL_BG);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
    char hint[40];
    snprintf(hint, sizeof(hint), "SAY anon (%d/30):", g_say_len);
    d.drawString(hint, 4, bar_y + 4);

    d.setTextColor(COL_GOLD, COL_PANEL_BG);
    char shown[34];
    snprintf(shown, sizeof(shown), "> %s_", g_say_text);
    d.drawString(shown, 4, bar_y + 14);

    d.setTextDatum(middle_center);
    d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
    d.drawString("ENTER send | DEL cancel", SCREEN_W/2, main_y + 8);
}

static void input_p2_06_say_input(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    for (char c : new_keys) {
        if (g_say_len >= 30) break;
        if (c >= 32 && c <= 126) {
            g_say_text[g_say_len++] = c;
            g_say_text[g_say_len] = '\0';
            loiter_beep(2200, 20);
            g_dirty = true;
        }
    }
    if (del_pressed) {
        if (g_say_len > 0) {
            g_say_text[--g_say_len] = '\0';
            loiter_beep(1800, 20);
            g_dirty = true;
        } else {
            goto_screen(P2_01_LIVE_MIRROR);
        }
    }
    if (enter_pressed && g_say_len > 0) {
        net_publish_anon(g_say_text);   // C→S 匿名公屏（服务端剥身份后广播到大屏）
        goto_screen(P2_01_LIVE_MIRROR);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-07: JUMP Triggered (auto-progress after 10s with fake counter)
// ──────────────────────────────────────────────────────────────────────────────
static int g_p2_07_last_count = -1;
// 集体 JUMP 实时人数（由 net_on_jump_progress 更新，服务端滑动窗口权威）
static int g_jump_live_count = 0;
static int g_jump_need = 5;

static void draw_p2_07_jump() {
    auto& d = M5Cardputer.Display;
    int main_y = STATUS_H + 1;
    int main_h = SCREEN_H - COLLECT_H - main_y;
    int bar_total = 160, bar_h = 8;
    int bar_x = (SCREEN_W - bar_total) / 2;
    int bar_y = main_y + 56;

    if (g_first_paint) {
        d.fillScreen(COL_FRAME_BG);
        draw_status_bar();
        draw_pixel_ground(0, main_y, SCREEN_W, main_h);
        draw_collection_grid();

        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        d.setTextSize(2);
        d.setTextColor(COL_GOLD, COL_FRAME_BG);
        d.drawString("JUMPED!", SCREEN_W/2, main_y + 18);

        d.setTextSize(1);
        d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
        d.drawString("waiting for others...", SCREEN_W/2, main_y + 40);

        // Empty bar
        d.fillRect(bar_x, bar_y, bar_total, bar_h, COL_LEFT_BG_B);
        d.drawRect(bar_x, bar_y, bar_total, bar_h, COL_BLACK);

        d.setTextColor(COL_LAVENDER_DK, COL_FRAME_BG);
        d.drawString("hit 5 for rainbow burst!", SCREEN_W/2, main_y + main_h - 12);

        g_first_paint = false;
        g_p2_07_last_count = -1;
    }

    // Dynamic: 服务端权威的实时跳跃人数（P1-3，替换旧的本地假倒计时 elapsed/2000）
    uint32_t elapsed = millis() - g_screen_entered_ms;
    int need = g_jump_need > 0 ? g_jump_need : 5;
    int count = g_jump_live_count;
    if (count > need) count = need;
    if (count < 0) count = 0;
    if (count != g_p2_07_last_count) {
        // 人数可升可降（服务端 10s 滑窗会剪枝）→ 先清整条 bar 内部再填，防残留
        d.fillRect(bar_x + 1, bar_y + 1, bar_total - 2, bar_h - 2, COL_LEFT_BG_B);
        int fill_w = (bar_total - 2) * count / need;
        if (fill_w > 0) d.fillRect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, COL_GOLD);

        // Counter
        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);
        d.setTextSize(1);
        d.fillRect(SCREEN_W/2 - 24, bar_y + 14, 48, 10, COL_FRAME_BG);
        d.setTextColor(COL_GOLD, COL_FRAME_BG);
        char cbuf[12];
        snprintf(cbuf, sizeof(cbuf), "%d / %d", count, need);
        d.drawString(cbuf, SCREEN_W/2, bar_y + 18);

        g_p2_07_last_count = count;
    }

    if (elapsed >= 10000) {
        goto_screen(P2_01_LIVE_MIRROR);
    }
}

static void input_p2_07_jump(bool enter_pressed, bool del_pressed) {
    (void)enter_pressed; (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P2-08 SIGNATURE INPUT（S 屏 · 彩蛋，口口相传触发）
// 黑底 / 标题 / 拥有的粒子列表 / 文本输入 / ENTER=切换 / DEL=退出
// ──────────────────────────────────────────────────────────────────────────────
static const uint16_t SIG_PARTICLE_COLS[4] = {COL_GOLD, COL_PINK, 0x07E0, 0xC61F}; // sparkle=金, heart=粉, leaf=绿, butterfly=紫

static void draw_p2_08_sig_input() {
    auto& d = M5Cardputer.Display;
    if (g_first_paint) {
        d.fillScreen(COL_BLACK);
        // Title
        d.setFont(&fonts::Font0);
        d.setTextDatum(top_center);
        d.setTextSize(1);
        d.setTextColor(COL_GOLD, COL_BLACK);
        d.drawString("SIGNATURE", SCREEN_W / 2, 4);
        // Owned list
        int y = 20;
        d.setTextDatum(top_left);
        d.setTextSize(1);
        for (int i = 0; i < 4; i++) {
            if (g_sig_owned[i]) {
                uint16_t col = SIG_PARTICLE_COLS[i];
                if (i == g_sig_particle) {
                    d.setTextColor(COL_BLACK, col);
                } else {
                    d.setTextColor(col, COL_BLACK);
                }
                d.drawString(SIG_PARTICLE_NAMES[i], 8, y);
                if (i == g_sig_particle) {
                    d.drawString(" <", 8 + (int)strlen(SIG_PARTICLE_NAMES[i]) * 6, y);
                }
                y += 12;
            }
        }
        // Input prompt
        d.setTextColor(COL_LAVENDER, COL_BLACK);
        d.setTextDatum(top_left);
        d.drawString("> ", 8, SCREEN_H - 24);
        g_first_paint = false;
    }
    // Dynamic: input text + error
    int input_y = SCREEN_H - 24;
    d.fillRect(20, input_y, SCREEN_W - 28, 10, COL_BLACK);
    if (g_sig_input_err) {
        d.setFont(&fonts::Font0);
        d.setTextDatum(top_left);
        d.setTextSize(1);
        d.setTextColor(0xF800, COL_BLACK); // red
        d.drawString("NOT OWNED", 20, input_y);
        // Auto-clear after 2s
        if (millis() - g_screen_entered_ms > 2000) {
            g_sig_input_err = 0;
            g_sig_input_len = 0;
            g_sig_input[0] = '\0';
            g_screen_entered_ms = millis();
            g_first_paint = true;
            g_dirty = true;
        }
    } else {
        d.setFont(&fonts::Font0);
        d.setTextDatum(top_left);
        d.setTextSize(1);
        d.setTextColor(COL_WHITE, COL_BLACK);
        d.drawString(g_sig_input, 20, input_y);
        // Cursor blink
        if ((millis() / 500) % 2 == 0) {
            int cx = 20 + g_sig_input_len * 6;
            d.fillRect(cx, input_y, 6, 8, COL_WHITE);
        }
    }
    // Footer
    d.setFont(&fonts::Font0);
    d.setTextDatum(bottom_center);
    d.setTextSize(1);
    d.setTextColor(COL_LAVENDER_DK, COL_BLACK);
    d.drawString("type name | ENTER=switch | DEL=back", SCREEN_W / 2, SCREEN_H - 2);
}

static void input_p2_08_sig_input(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    if (g_sig_input_err) return; // error display, ignore input until cleared
    if (del_pressed) {
        if (g_sig_input_len > 0) {
            g_sig_input_len--;
            g_sig_input[g_sig_input_len] = '\0';
            g_dirty = true;
        } else {
            // Empty + DEL = exit back to LIVE_MIRROR
            goto_screen(P2_01_LIVE_MIRROR);
        }
        return;
    }
    if (enter_pressed && g_sig_input_len > 0) {
        // Match name (case-insensitive)
        int matched = -1;
        for (int i = 0; i < 4; i++) {
            if (strcasecmp(g_sig_input, SIG_PARTICLE_NAMES[i]) == 0) {
                matched = i;
                break;
            }
        }
        if (matched >= 0 && g_sig_owned[matched]) {
            // Success: switch particle
            g_sig_particle = (int8_t)matched;
            net_publish_sig(g_sig_particle, g_sig_action);
            goto_screen(P2_01_LIVE_MIRROR);
        } else {
            // Not owned or invalid name
            g_sig_input_err = 1;
            g_screen_entered_ms = millis(); // start 2s error timer
            g_dirty = true;
        }
        return;
    }
    // Accumulate characters
    for (char c : new_keys) {
        if (g_sig_input_len < 10 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            g_sig_input[g_sig_input_len++] = (char)toupper(c);
            g_sig_input[g_sig_input_len] = '\0';
            g_dirty = true;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Phase 3 reading — 本地双语 fallback（server/CopilotX 30s 不回时用，按岛分组，不依赖网络）
// 对齐 server reading.py 的 6 套 _FALLBACK，调性一致。
// ──────────────────────────────────────────────────────────────────────────────
struct ReadingFallback {
    const char* title;        // EN
    const char* title_cn;     // CN ≤6
    const char* core_cn;      // CN ≤12
    const char* en[9];        // EN 9 行（P3-03 三页 ×3）
    const char* cn[9];        // CN 9 行短句 ≤12/行
};
static const ReadingFallback READING_FALLBACK[6] = {
    { "EMBER HEART", "火心", "你燃得明亮又温柔",
      {"You arrived already burning,", "a small fierce kind of light.", "You crossed to other shores,",
       "and didn't dim to fit in.", "You left warmth where you went,", "ember by quiet ember.",
       "You came back still glowing-", "softer now, but no less bright.", "That is your color today."},
      {"你来时已经燃着", "一束小小的烈火", "你走向别的海岸", "却没为合群变暗", "你把暖意留在身后",
       "一粒粒安静的火星", "归来时仍在发光", "更柔了 却不更暗", "这就是今天的你"} },
    { "STEADY HEARTH", "暖炉", "别人在你这里找到家",
      {"You weren't the loudest one,", "but you held the door open.", "People leaned toward you,",
       "and somehow felt at home.", "You gathered other colors,", "without losing your warm one.",
       "You became a place to rest,", "for everyone passing through.", "Steady is a color too."},
      {"你不是最响的那个", "却为人留着门", "别人朝你靠过来", "莫名觉得是家", "你收下别的颜色",
       "也没丢自己的暖", "你成了一处歇脚", "给每个路过的人", "稳 也是一种颜色"} },
    { "RESTLESS SPARK", "火花", "你追着光 光也追你",
      {"You couldn't sit still today,", "always chasing the bright thing.", "You asked the loud questions,",
       "and ran toward every spark.", "You gathered colors fast,", "curious about each one.",
       "And somewhere in the chase,", "the light turned and chased you.", "You glow when you wonder."},
      {"你今天坐不住", "总追着亮的东西", "你问出大声的问题", "奔向每一簇火花", "你飞快地收集颜色",
       "对每一种都好奇", "追着追着 不知何时", "光也回头追你", "你好奇时就发光"} },
    { "ROOTED GROVE", "林", "安静也是一种颜色",
      {"You didn't rush anywhere,", "you stayed, quiet and sure.", "Others drifted toward you,",
       "the way things grow toward calm.", "You took in other colors,", "slowly, without losing root.",
       "You let people slow down,", "just by standing where you stood.", "Calm is a color too."},
      {"你哪儿都没赶着去", "站得安静又笃定", "别人朝你飘过来", "像万物向着平静", "你收下别的颜色",
       "慢慢的 根却没动", "你让人也慢下来", "只因你站在那里", "安静 也是一种颜色"} },
    { "OPEN TIDE", "潮", "你温柔地碰过每座岛",
      {"You moved the way the sea moves,", "touching every shore in turn.", "You met so many colors,",
       "and held each one gently.", "You didn't keep them all,", "you carried pieces of each home.",
       "You left a little of yourself", "on every island you passed.", "You are made of many tides."},
      {"你像海一样流动", "轮流碰过每片岸", "你遇见好多颜色", "每一种都轻轻握", "你没把它们都留下",
       "只带走每个家的碎片", "你也留下一点自己", "在路过的每座岛", "你由许多潮汐组成"} },
    { "DREAMING MIST", "雾", "你看见别人走过的",
      {"You drifted soft and deep,", "in your own quiet weather.", "You saw what others passed,",
       "the small things in the fog.", "You gathered gentle colors,", "and kept their secrets safe.",
       "You didn't need the spotlight,", "you were the soft light itself.", "The mist remembers you."},
      {"你飘得又轻又深", "在自己安静的天气里", "你看见别人走过的", "雾中那些小东西", "你收下温柔的颜色",
       "替它们藏好秘密", "你不需要聚光灯", "你就是那束柔光", "雾 记得你"} },
};

// 把本地 fallback 填进 g_reading_*（island_idx 越界 → SPARK 兜底）
static void fill_reading_fallback(int island_idx) {
    const ReadingFallback& f = READING_FALLBACK[(island_idx >= 0 && island_idx < 6) ? island_idx : 2];
    strncpy(g_reading_title,    f.title,    sizeof(g_reading_title) - 1);
    g_reading_title[sizeof(g_reading_title) - 1] = '\0';
    strncpy(g_reading_title_cn, f.title_cn, sizeof(g_reading_title_cn) - 1);
    g_reading_title_cn[sizeof(g_reading_title_cn) - 1] = '\0';
    strncpy(g_reading_core_cn,  f.core_cn,  sizeof(g_reading_core_cn) - 1);
    g_reading_core_cn[sizeof(g_reading_core_cn) - 1] = '\0';
    for (int i = 0; i < 9; ++i) {
        strncpy(g_reading_lines[i], f.en[i], sizeof(g_reading_lines[0]) - 1);
        g_reading_lines[i][sizeof(g_reading_lines[0]) - 1] = '\0';
    }
    for (int i = 0; i < 9; ++i) {
        strncpy(g_reading_lines_cn[i], f.cn[i], sizeof(g_reading_lines_cn[0]) - 1);
        g_reading_lines_cn[i][sizeof(g_reading_lines_cn[0]) - 1] = '\0';
    }
    g_reading_ready = true;
}

// 清空 reading 缓存（restart / 重新开始旅程时调，防上一段旅程的 reading 残留）
static void clear_reading_state() {
    g_reading_title[0] = '\0';
    g_reading_title_cn[0] = '\0';
    g_reading_core_cn[0] = '\0';
    for (int i = 0; i < 9; ++i) g_reading_lines[i][0] = '\0';
    for (int i = 0; i < 9; ++i) g_reading_lines_cn[i][0] = '\0';
    g_reading_ready = false;
    g_reading_req_ms = 0;
    g_reading_last_req_ms = 0;
    g_reading_ready_at = 0;
}

// ──────────────────────────────────────────────────────────────────────────────
// P3-01: Waiting (story generating)
// ──────────────────────────────────────────────────────────────────────────────
// Draw a small pixel-art bell at (cx, cy) — used by P3-01 instead of "!"
static void draw_bell(int cx, int cy, uint16_t color) {
    auto& d = M5Cardputer.Display;
    // Top stem (2×2)
    d.fillRect(cx - 1, cy - 10, 2, 2, color);
    // Narrow top (4×2)
    d.fillRect(cx - 2, cy - 8, 4, 2, color);
    // Wider mid (8×4)
    d.fillRect(cx - 4, cy - 6, 8, 4, color);
    // Widest body (12×4)
    d.fillRect(cx - 6, cy - 2, 12, 4, color);
    // Bottom rim (16×2)
    d.fillRect(cx - 8, cy + 2, 16, 2, color);
    // Clapper (2×3)
    d.fillRect(cx - 1, cy + 5, 2, 3, color);
    // Sound waves (small dots flanking)
    d.fillRect(cx - 14, cy - 4, 2, 2, color);
    d.fillRect(cx - 18, cy - 6, 2, 2, color);
    d.fillRect(cx + 12, cy - 4, 2, 2, color);
    d.fillRect(cx + 16, cy - 6, 2, 2, color);
}

static int g_p3_01_last_pct = -1;
static int g_p3_01_last_mi = -1;

// loading 时长：30s 容错（真 reading 一般 3-5s 就到，进度条还在低位就被推满跳转）
static const uint32_t READING_TIMEOUT_MS = 30000;

static void draw_p3_01_waiting() {
    auto& d = M5Cardputer.Display;

    if (g_first_paint) {
        // 进屏 guard：reading 已就位（重进 P3-01 / phase 重广播）→ 不重发请求，直接进 P3-02
        if (g_reading_ready) { g_reading_page = 0; goto_screen(P3_02_REVEAL_TAG); return; }

        d.fillScreen(COL_FRAME_BG);
        d.setFont(&fonts::Font0);
        d.setTextDatum(middle_center);

        // Bell graphic at top
        draw_bell(SCREEN_W/2, 28, COL_GOLD);

        d.setTextSize(1);
        d.setTextColor(COL_GOLD, COL_FRAME_BG);
        d.drawString("THE BELL RANG", SCREEN_W/2, 52);

        d.setTextColor(COL_LAVENDER, COL_FRAME_BG);
        d.drawString("your story is brewing...", SCREEN_W/2, 70);

        // Empty progress bar
        int bar_w = 180, bar_h = 8;
        int bar_x = (SCREEN_W - bar_w) / 2;
        int bar_y = 86;
        d.fillRect(bar_x, bar_y, bar_w, bar_h, COL_PANEL_BG);
        d.drawRect(bar_x, bar_y, bar_w, bar_h, COL_BLACK);

        // C→S 请求生成 reading（只在首帧发一次；失败/丢包靠下方 3s 重发兜底）
        net_request_reading();
        g_reading_req_ms = millis();
        g_reading_last_req_ms = millis();

        g_first_paint = false;
        g_p3_01_last_pct = -1;
        g_p3_01_last_mi = -1;
    }

    uint32_t elapsed = millis() - g_screen_entered_ms;

    // 重连兜底：未就位时每 3s 重发一次 reading/request（server (uid,gen) inflight+cache 保幂等）。
    // 防进 Phase 3 恰逢 MQTT 重连窗口 → 首发 QoS0 丢包 → 永远等到 30s fallback（review 🦞P1/Codex#1）。
    if (!g_reading_ready && millis() - g_reading_last_req_ms >= 3000) {
        net_request_reading();
        g_reading_last_req_ms = millis();
    }

    // 进度：未到达 → 30s 涨到 95% 封顶；到达 → flash 100%
    int pct;
    if (g_reading_ready) {
        pct = 100;
    } else {
        pct = (int)((elapsed * 95) / READING_TIMEOUT_MS);
        if (pct > 95) pct = 95;
    }

    if (pct != g_p3_01_last_pct) {
        int bar_w = 180, bar_h = 8;
        int bar_x = (SCREEN_W - bar_w) / 2;
        int bar_y = 86;
        d.fillRect(bar_x + 1, bar_y + 1, ((bar_w - 2) * pct) / 100, bar_h - 2, COL_GOLD);
        g_p3_01_last_pct = pct;
    }

    const char* msgs[4] = {
        "collecting your spectrum...",
        "looking at who you met...",
        "finding the right words...",
        "almost there...",
    };
    int mi = (elapsed / 2000) % 4;
    if (mi != g_p3_01_last_mi && !g_reading_ready) {
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextDatum(middle_center);
        d.fillRect(0, 105, SCREEN_W, 12, COL_FRAME_BG);
        d.setTextColor(COL_LAVENDER_DK, COL_FRAME_BG);
        d.drawString(msgs[mi], SCREEN_W/2, 110);
        g_p3_01_last_mi = mi;
    }

    // 真 reading 到达 → flash 100% 停 300ms（让用户感知"读条满了"）→ 跳 P3-02
    if (g_reading_ready) {
        if (millis() - g_reading_ready_at >= 300) {
            g_reading_page = 0;
            goto_screen(P3_02_REVEAL_TAG);
        }
        return;
    }

    // 30s 超时仍没到 → 本地双语 fallback → 设 ready_at，下一帧走上面 ready 分支（统一 flash 100% 停 300ms，review Codex#3）
    if (elapsed >= READING_TIMEOUT_MS) {
        fill_reading_fallback(g_island);
        g_reading_ready_at = millis();
        g_reading_page = 0;
        return;
    }
}

static void input_p3_01_waiting(bool enter_pressed, bool del_pressed) {
    (void)enter_pressed; (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P3-02: Reveal Page 1 — Identity Tag（B-lite 双语，读 g_reading_*）
// ──────────────────────────────────────────────────────────────────────────────
static void draw_p3_02_reveal_tag() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_FRAME_BG);
    d.setTextDatum(middle_center);

    // EN hero 标题
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(COL_GOLD);
    d.drawString(g_reading_title[0] ? g_reading_title : "YOUR COLOR", SCREEN_W/2, 20);

    // CN 副标题（efontCN_14）—— 必须 setTextSize(1)，否则继承 hero 的 size 2 把位图中文放大 2×
if (g_reading_title_cn[0]) {
        d.setFont(&fonts::efontCN_14);
        d.setTextSize(1);
        d.setTextColor(COL_LAVENDER);
        d.drawString(g_reading_title_cn, SCREEN_W/2, 40);
    }

    // 5-color spectrum
    int cell = 16, gap = 4;
    int total = 5 * cell + 4 * gap;
    int sx = (SCREEN_W - total) / 2;
    int sy = 54;
    for (int i = 0; i < 5; ++i) {
        int cx = sx + i * (cell + gap);
        uint16_t color = (g_collection[i] >= 0)
            ? ISLANDS[g_collection[i]].color
            : COL_PANEL_BG;
        d.fillRect(cx, sy, cell, cell, color);
        d.drawRect(cx, sy, cell, cell, COL_BLACK);
    }

    // Core 中文句 in a box（efontCN_14）
    int box_w = 220, box_h = 20;
    int box_x = (SCREEN_W - box_w) / 2;
    int box_y = 84;
    d.drawRect(box_x, box_y, box_w, box_h, COL_GOLD);
    d.fillRect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, COL_PANEL_BG);
    if (g_reading_core_cn[0]) {
        d.setFont(&fonts::efontCN_14);
        d.setTextSize(1);
        d.setTextColor(COL_LAVENDER, COL_PANEL_BG);
        d.drawString(g_reading_core_cn, SCREEN_W/2, box_y + box_h/2);
    }

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(COL_GOLD);
    d.drawString("ENTER >> read more", SCREEN_W/2, 120);
}

static void input_p3_02_reveal_tag(bool enter_pressed, bool del_pressed) {
    if (enter_pressed) {
        g_reading_page = 0;
        goto_screen(P3_03_REVEAL_TEXT);
    }
    (void)del_pressed;
}

// ──────────────────────────────────────────────────────────────────────────────
// P3-03: Reveal Pages 2-4 — Bilingual Reading（Designer 原版：每页 3 英 + 3 中，读 g_reading_lines* 动态）
//   3 页，每页 = 顶部 3 行英文 + divider + 底部 3 行中文（efontCN_14 已验证）
// ──────────────────────────────────────────────────────────────────────────────
static void draw_p3_03_reveal_text() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_FRAME_BG);
    d.setTextDatum(top_left);
    d.setTextSize(1);

    int page = g_reading_page;  // 0..2
    int base = page * 3;        // 本页 3 行起点

    // English block (top) — 本页 3 行英文
    d.setFont(&fonts::Font0);
    d.setTextColor(COL_LAVENDER_DK);
    int y = 6;
    for (int i = 0; i < 3; ++i) {
        d.drawString(g_reading_lines[base + i], 6, y);
        y += 12;
    }

    // Thin divider
    y += 4;
    d.drawFastHLine(20, y, SCREEN_W - 40, COL_LEFT_BG_B);
    y += 6;

    // Chinese block (bottom) — 本页 3 行中文，gold-tinted
    d.setFont(&fonts::efontCN_14);
    d.setTextColor(COL_GOLD);
    for (int i = 0; i < 3; ++i) {
        const char* cn = g_reading_lines_cn[base + i];
        if (cn[0]) d.drawString(cn, 6, y);
        y += 18;
    }

    // Footer — back to Font0
    d.setFont(&fonts::Font0);
    int footer_y = SCREEN_H - 14;
    d.fillRect(0, footer_y - 1, SCREEN_W, 14, COL_PANEL_BG);
    d.setTextDatum(middle_left);
    d.setTextColor(COL_LAVENDER_DK, COL_PANEL_BG);
    d.drawString("<< DEL", 4, footer_y + 5);
    d.setTextDatum(middle_center);
    char pg[16];
    snprintf(pg, sizeof(pg), "PAGE %d / 4", page + 2);
    d.drawString(pg, SCREEN_W/2, footer_y + 5);
    d.setTextDatum(middle_right);
    d.setTextColor(COL_GOLD, COL_PANEL_BG);
    d.drawString("ENTER >>", SCREEN_W - 4, footer_y + 5);
}

static void input_p3_03_reveal_text(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    for (char c : new_keys) {
        if (c == '.') {  // down arrow on Cardputer = next page
            if (g_reading_page < 2) { g_reading_page++; g_dirty = true; }
            else goto_screen(P3_04_CLOSING);
        } else if (c == ';') {  // up arrow = prev page
            if (g_reading_page > 0) { g_reading_page--; g_dirty = true; }
            else goto_screen(P3_02_REVEAL_TAG);
        }
    }
    if (enter_pressed) {
        if (g_reading_page < 2) { g_reading_page++; g_dirty = true; }
        else goto_screen(P3_04_CLOSING);
    }
    if (del_pressed) {
        if (g_reading_page > 0) { g_reading_page--; g_dirty = true; }
        else goto_screen(P3_02_REVEAL_TAG);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// P3-04: Closing — look up at big screen
// ──────────────────────────────────────────────────────────────────────────────

// Draw a pixel-art rainbow arc using the 6 island colors.
// (cx, base_y) = center-bottom of the arc.
// outer_r = outermost radius, bw = band width in px (6 bands total).
static void draw_rainbow_arc(int cx, int base_y, int outer_r, int bw) {
    for (int band = 0; band < 6; ++band) {
        uint16_t c = ISLANDS[band].color;
        int ro = outer_r - band * bw;
        int ri = ro - bw;
        if (ro <= 0) break;
        for (int dy = 0; dy <= ro; ++dy) {
            int y = base_y - dy;
            if (y < 0 || y >= SCREEN_H) continue;
            int xo = (int)sqrtf(max(0.0f, (float)(ro*ro - dy*dy)));
            int xi = (ri > 0) ? (int)sqrtf(max(0.0f, (float)(ri*ri - dy*dy))) : 0;
            int w  = xo - xi;
            if (w <= 0) continue;
            // Left arm
            if (cx - xo >= 0)
                M5Cardputer.Display.fillRect(cx - xo, y, w, 1, c);
            // Right arm
            if (cx + xi + w <= SCREEN_W)
                M5Cardputer.Display.fillRect(cx + xi, y, w, 1, c);
        }
    }
}

static void draw_p3_04_closing() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_FRAME_BG);
    d.setFont(&fonts::Font0);
    d.setTextDatum(middle_center);

    d.setTextSize(3);
    d.setTextColor(COL_GOLD);
    d.drawString("^", SCREEN_W/2, 18);

    d.setTextSize(1);
    d.setTextColor(COL_LAVENDER);
    d.drawString("look up at", SCREEN_W/2, 40);
    d.setTextColor(COL_GOLD);
    d.drawString("the big screen", SCREEN_W/2, 52);

    // Rainbow arc: base at y=92, outer radius 30, band width 4 → arc from y=62 to y=92
    draw_rainbow_arc(SCREEN_W/2, 92, 30, 4);

    d.setTextSize(1);
    d.setTextColor(COL_LAVENDER_DK);
    d.drawString("thank you for being here.", SCREEN_W/2, 108);
    d.drawString("<< DEL  re-read your story", SCREEN_W/2, 122);
}

static void input_p3_04_closing(bool enter_pressed, bool del_pressed) {
    if (del_pressed) {
        g_reading_page = 2;
        goto_screen(P3_03_REVEAL_TEXT);
    }
    (void)enter_pressed;
}

// --- Dispatchers ---
static void dispatch_redraw() {
    switch (g_screen) {
        case P1_01_WELCOME:        draw_p1_01_welcome(); break;
        case P1_02_USERNAME:       draw_p1_02_username(); break;
        case P1_03_DRESSUP:        redraw_all(); break;
        case P1_07_ISLAND_REVEAL:  draw_p1_07_island_reveal(); break;
        case P1_08_ARRIVAL:        draw_p1_08_arrival(); break;
        case P2_01_LIVE_MIRROR:    draw_p2_01_live_mirror(); break;
        case P2_03_HI_SENT:        draw_p2_03_hi_sent(); break;
        case P2_04_HI_RECEIVED:    draw_p2_04_hi_received(); break;
        case P2_05_HI_COMPLETED:   draw_p2_05_hi_completed(); break;
        case P2_06_SAY_INPUT:      draw_p2_06_say_input(); break;
        case P2_07_JUMP:           draw_p2_07_jump(); break;
        case P2_08_SIG_INPUT:      draw_p2_08_sig_input(); break;
        case P3_01_WAITING:        draw_p3_01_waiting(); break;
        case P3_02_REVEAL_TAG:     draw_p3_02_reveal_tag(); break;
        case P3_03_REVEAL_TEXT:    draw_p3_03_reveal_text(); break;
        case P3_04_CLOSING:        draw_p3_04_closing(); break;
    }
}

static void dispatch_input(const std::set<char>& new_keys, bool enter_pressed, bool del_pressed) {
    switch (g_screen) {
        case P1_01_WELCOME:        input_p1_01_welcome(enter_pressed); break;
        case P1_02_USERNAME:       input_p1_02_username(new_keys, enter_pressed, del_pressed); break;
        case P1_03_DRESSUP:        dressup_handle_input(new_keys, enter_pressed, del_pressed); break;
        case P1_07_ISLAND_REVEAL:  input_p1_07_island_reveal(enter_pressed, del_pressed); break;
        case P1_08_ARRIVAL:        input_p1_08_arrival(enter_pressed, del_pressed); break;
        case P2_01_LIVE_MIRROR:    input_p2_01_live_mirror(new_keys, enter_pressed, del_pressed); break;
        case P2_03_HI_SENT:        input_p2_03_hi_sent(new_keys, enter_pressed, del_pressed); break;
        case P2_04_HI_RECEIVED:    input_p2_04_hi_received(enter_pressed, del_pressed); break;
        case P2_05_HI_COMPLETED:   input_p2_05_hi_completed(enter_pressed, del_pressed); break;
        case P2_06_SAY_INPUT:      input_p2_06_say_input(new_keys, enter_pressed, del_pressed); break;
        case P2_07_JUMP:           input_p2_07_jump(enter_pressed, del_pressed); break;
        case P2_08_SIG_INPUT:      input_p2_08_sig_input(new_keys, enter_pressed, del_pressed); break;
        case P3_01_WAITING:        input_p3_01_waiting(enter_pressed, del_pressed); break;
        case P3_02_REVEAL_TAG:     input_p3_02_reveal_tag(enter_pressed, del_pressed); break;
        case P3_03_REVEAL_TEXT:    input_p3_03_reveal_text(new_keys, enter_pressed, del_pressed); break;
        case P3_04_CLOSING:        input_p3_04_closing(enter_pressed, del_pressed); break;
    }
}

static bool needs_animation_redraw() {
    switch (g_screen) {
        case P2_03_HI_SENT:
            // 仅等待态需每帧倒计时动画；键入态靠 g_dirty 重绘（否则每帧 fillScreen → 闪屏）
            return g_hi_sent;
        case P1_08_ARRIVAL:
        case P2_01_LIVE_MIRROR:
        case P2_04_HI_RECEIVED:
        case P2_05_HI_COMPLETED:
        case P2_07_JUMP:
        case P2_08_SIG_INPUT:
        case P3_01_WAITING:
            return true;
        default:
            return false;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// v2 网络回调 —— 服务端入站事件驱动 UI 状态机
// ──────────────────────────────────────────────────────────────────────────────
static void net_on_island(int island, const char* name, const char* color,
                          const char* reason_en, const char* reason_cn) {
    // v3'：服务端据 baked profile_id 权威分岛 + 文艺双语 reason（先空后到 → 二次 push 补 reason）
    if (island >= 0 && island < 6) {
        g_island = island;
        if (g_collection[0] >= 0) g_collection[0] = island;  // 收集格首格本色（review Codex #2）
    }
    if (reason_en && reason_en[0]) {  // reason 真到达（首推可能空，notify_reason_ready 二次推填）
        strncpy(g_island_reason_en, reason_en, sizeof(g_island_reason_en) - 1);
        g_island_reason_en[sizeof(g_island_reason_en) - 1] = '\0';
        strncpy(g_island_reason_cn, reason_cn ? reason_cn : "", sizeof(g_island_reason_cn) - 1);
        g_island_reason_cn[sizeof(g_island_reason_cn) - 1] = '\0';
        g_island_ready = true;
    }
    g_dirty = true;
    (void)name; (void)color;
}
static void net_on_hi_incoming(const char* requester, const char* nick, const char* color, const char* msg) {
    // 有人 HI 我 → 存发起者信息 + 仅在 LIVE_MIRROR 闲置时弹 P2-04（忙时丢弃，服务端会 expire）
    strncpy(g_hi_in_requester, requester, sizeof(g_hi_in_requester) - 1);
    g_hi_in_requester[sizeof(g_hi_in_requester) - 1] = '\0';
    strncpy(g_hi_in_nick, nick, sizeof(g_hi_in_nick) - 1);
    g_hi_in_nick[sizeof(g_hi_in_nick) - 1] = '\0';
    g_hi_in_color = rgb565_from_hex(color);
    Serial.printf("[net] HI incoming from %s (%s): %s\n", nick, requester, msg);
    if (g_screen == P2_01_LIVE_MIRROR) goto_screen(P2_04_HI_RECEIVED);
    (void)msg;
}
static void net_on_hi_result(const char* event, const char* partner, const char* color, int slot) {
    // 我发起/我应答的 HI 有结果。matched=双向换色成立；declined/expired=收尾。
    Serial.printf("[net] HI result %s partner=%s color=%s slot=%d\n", event, partner, color, slot);
    g_hi_await_ack = false;   // ack 已到（无论结果），解除 P2-04 connecting 等待
    if (strcmp(event, "matched") == 0) {
        strncpy(g_hi_partner_nick, partner, sizeof(g_hi_partner_nick) - 1);
        g_hi_partner_nick[sizeof(g_hi_partner_nick) - 1] = '\0';
        g_hi_partner_color  = rgb565_from_hex(color);
        g_hi_partner_island = island_idx_from_hex(color);
        g_hi_slot           = slot;
        if (slot >= 0 && slot < 5 && g_hi_partner_island >= 0) {
            g_collection[slot] = g_hi_partner_island;   // 服务端权威格位
            g_hi_outcome = HI_MATCHED;
        } else {
            g_hi_outcome = HI_RESONANCE;                // 同岛/重复/满 → 不加色
        }
        goto_screen(P2_05_HI_COMPLETED);
    } else if (strcmp(event, "declined") == 0) {
        g_hi_outcome = HI_DECLINED;
        goto_screen(P2_05_HI_COMPLETED);
    } else if (strcmp(event, "expired") == 0) {
        g_hi_outcome = HI_EXPIRED;
        goto_screen(P2_05_HI_COMPLETED);
    }
}
static void net_on_phase(int phase) {
    Serial.printf("[net] phase -> %d\n", phase);
    // Phase 3 广播 → 把设备带进 P3-01（进屏首帧发 reading/request）。
    // 只在还没进 P3 时带入：已在 P3 任意屏 → 忽略，防把正在读 reading 的人踹回 loading。
    if (phase == 3 && g_screen < P3_01_WAITING) {
        goto_screen(P3_01_WAITING);
    }
}
static void net_on_reading(const char* title, const char* title_cn, const char* core_cn,
                           const char* lines[9], const char* lines_cn[9]) {
    // phase guard：只在已进入 Phase 3 等待屏（或更后）才接收 reading，杜绝 broker retained /
    // 早到包在 Phase 1/2 误设 ready（数据仍可存，但不标 ready 防 P3-01 进屏 guard 误跳）。
    // ASSUMPTION: Screen enum 按 Phase 顺序排列 P1 < P2 < P3；若重排此 guard 失效（🦞 review 锁定）。
    if (g_screen < P3_01_WAITING) return;
    strncpy(g_reading_title,    title    ? title    : "", sizeof(g_reading_title) - 1);
    g_reading_title[sizeof(g_reading_title) - 1] = '\0';
    strncpy(g_reading_title_cn, title_cn ? title_cn : "", sizeof(g_reading_title_cn) - 1);
    g_reading_title_cn[sizeof(g_reading_title_cn) - 1] = '\0';
    strncpy(g_reading_core_cn,  core_cn  ? core_cn  : "", sizeof(g_reading_core_cn) - 1);
    g_reading_core_cn[sizeof(g_reading_core_cn) - 1] = '\0';
    for (int i = 0; i < 9; ++i) {
        strncpy(g_reading_lines[i], lines[i] ? lines[i] : "", sizeof(g_reading_lines[0]) - 1);
        g_reading_lines[i][sizeof(g_reading_lines[0]) - 1] = '\0';
    }
    for (int i = 0; i < 9; ++i) {
        strncpy(g_reading_lines_cn[i], lines_cn[i] ? lines_cn[i] : "", sizeof(g_reading_lines_cn[0]) - 1);
        g_reading_lines_cn[i][sizeof(g_reading_lines_cn[0]) - 1] = '\0';
    }
    g_reading_ready = true;
    g_reading_ready_at = millis();
    // 推进只发生在 P3-01（draw 里判 ready_at → flash 100% 停 300ms 跳 P3-02）；
    // 其他屏：数据已存，用户翻到 P3-02/03 时自然读到（不强跳，防打断）。
    g_dirty = true;
}
static void net_on_anon(const char* text) {
    Serial.printf("[net] anon: %s\n", text);
}
// 集体 JUMP 实时人数（jump/progress 广播）→ P2-07 跳跃屏显真 N/need（替换本地假倒计时）；
// g_jump_live_count/g_jump_need 在 draw_p2_07_jump 上方声明（使用点之前）
static void net_on_jump_progress(int count, int need) {
    g_jump_live_count = count;
    if (need > 0) g_jump_need = need;
    // P2-07 每帧重绘（在 needs_animation_redraw）→ 下一帧自然拿到新人数，无需强踢 g_dirty
}
static void net_on_sig_recv(int particle, int action, const char* from_nick) {
    // 近距 shake 复制成功 → 对方降临 sig 收进背包 owned + 切 current 展示，replay 入场动画预览
    Serial.printf("[net] sig recv particle=%d action=%d from=%s\n", particle, action, from_nick);
    if (particle >= 0 && particle <= 3) {
        g_sig_owned[particle] = true;          // 永久收进背包（之后 S 屏可切回）
        g_sig_particle = (int8_t)particle;     // 切 current 展示“我复制了对方”
    }
    if (action >= 0 && action <= 2)     g_sig_action   = (int8_t)action;
    if (g_screen == P2_01_LIVE_MIRROR) {
        g_lm_sig_entrance_done = false;   // replay entrance 动画显示新 sig 粒子
        g_screen_entered_ms    = millis();
        draw_lm_sidebar();                // 侧栏 sig: 标签刷新
    }
    g_dirty = true;
}
static void net_redraw() { g_dirty = true; }

// ──────────────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(180);
    M5Cardputer.Display.fillScreen(COL_FRAME_BG);
    Serial.begin(115200);

    // ⚠️ Speaker 彻底不碰：所有音效走 loiter_beep() no-op（见文件顶部说明）。
    // v1 踩坑：ES8311 Speaker 的 I2S task 与 PubSubClient 冲突致卡死/MQTT 掉线。
    // 不 begin 也不够（tone 会自 begin），所以调用点全换成 no-op（review Codex #1/#5）。

    // IMU for shake detection (BMI270 on Cardputer ADV)
    if (!M5.Imu.begin()) {
        Serial.println("[boot] IMU init failed (continuing without shake support)");
    }

    randomSeed(esp_random());
    Serial.println("[boot] Pride workshop firmware — state machine ready");

    // v2 联网层：WiFi（splash+配网门户）+ MQTT + UID
    NetCallbacks cb = {};
    cb.on_island      = net_on_island;
    cb.on_hi_incoming = net_on_hi_incoming;
    cb.on_hi_result   = net_on_hi_result;
    cb.on_phase       = net_on_phase;
    cb.on_reading     = net_on_reading;
    cb.on_anon        = net_on_anon;
    cb.on_jump_progress = net_on_jump_progress;
    cb.on_sig_recv    = net_on_sig_recv;
    cb.redraw         = net_redraw;
    net_begin(cb);

    g_screen = P1_01_WELCOME;
    g_dirty = true;
}

static bool g_prev_del = false;
static uint32_t g_last_anim_redraw = 0;

// Reset all participant state and go to Welcome (dev key R)
static void restart_all() {
    net_publish_leave();   // 通知服务端清除旧旅程（island/spectrum），重新输名 = 全新 member（review Codex P1/P2）
    g_username[0] = '\0';
    g_username_len = 0;
    g_island = -1;
    g_island_ready = false;
    g_island_reason_en[0] = '\0';
    g_island_reason_cn[0] = '\0';
    g_island_req_ms = 0;
    g_island_last_req_ms = 0;
    g_island_warned = false;
    g_joined = false;   // dev reset 回大厅：停心跳，重新输名后再起
    g_shape[0] = 0; g_shape[1] = 0; g_shape[2] = 0; g_shape[3] = 1; g_shape[4] = 0;
    g_color[0] = 0; g_color[1] = 0; g_color[2] = 0; g_color[3] = 0; g_color[4] = 0;
    for (int i = 0; i < 5; ++i) g_collection[i] = -1;
    g_sig_action = -1;
    g_sig_particle = -1;
    for (int i = 0; i < 4; i++) g_sig_owned[i] = false;
    g_reroll_count = 0;
    g_say_text[0] = '\0';
    g_say_len = 0;
    g_reading_page = 0;
    clear_reading_state();   // 清上一段旅程的 reading 缓存（防 dev reset 后 P3-01 ready guard 直跳旧 reading，review Codex#2）
    g_arrival_state = ARRIVAL_LOADING;
    g_quest_state = QUEST_PROMPT;
    g_quest_energy = 0;
    goto_screen(P1_01_WELCOME);
}

// Fill missing state with dev defaults so Phase 2/3 jumps render correctly
static void fill_dev_state() {
    if (g_username_len == 0) { strcpy(g_username, "DEV"); g_username_len = 3; }
    if (g_island < 0) g_island = random(0, 6);
    if (g_collection[0] < 0) {
        g_collection[0] = g_island;
        g_collection[1] = random(0, 6);
        g_collection[2] = random(0, 6);
        g_collection[3] = -1;
        g_collection[4] = -1;
    }
}

// ── 心跳自愈 ──────────────────────────────────────────────────────────
// 服务端是纯内存态：意外重启会清空 room → 设备的 MQTT 连接还活着（不触发重连 →
// 不重发 join）→ 小人从大屏消失且不会自动回来。心跳定期重发 join，让服务端据 baked
// profile_id 确定性地重建该成员（同 profile_id → 同岛 + 重推 island/<uid> 带 reason）。
// 门控 = g_joined（只要已上报 join、大屏有可视小人就维持，含"已输名但未到揭晓"的中心位
// 小人 — review Codex P1：未分岛 join 在大屏会 spawn 到地图中心，也需恢复）。对存活服务端
// 幂等：join 重发仅刷 last_seen（服务端 _handle_join 对重复 join 不再 emit，防大屏每 25s
// 假 arrival）；island/<uid> 重推由 server _apply_profile 幂等保证（已分岛只刷不重置）。
// v3' 不再发 quiz/done —— 分岛权威搬到 profile_id，设备不带分岛逻辑。
static const uint32_t HEARTBEAT_MS = 25000;
static uint32_t g_last_heartbeat = 0;
static void maybe_publish_heartbeat() {
    if (!net_online() || !g_joined) return;
    uint32_t now = millis();
    if (now - g_last_heartbeat < HEARTBEAT_MS) return;
    g_last_heartbeat = now;
    net_set_profile(g_shape, g_color, g_sig_particle, g_sig_action);
    net_publish_join();   // 维持小人；server 据 baked profile_id 重建同一岛 + 重推 island/<uid>
}

void loop() {
    M5Cardputer.update();
    net_loop();   // v2: wifiEnsure + mqttEnsure + mqtt.loop
    maybe_publish_heartbeat();   // 心跳自愈：服务端重启后小人自动回归（详见函数注释）
    maybe_request_island();      // 揭晓屏未就位时拉取 island/<uid>（首发+3s重发+15s fallback）
    auto status = M5Cardputer.Keyboard.keysState();

    // Build sets of keys this frame
    std::set<char> current_keys;
    for (auto c : status.word) current_keys.insert((char)c);

    // Edge-trigger: only keys newly down this frame
    std::set<char> new_keys;
    for (char c : current_keys) {
        if (g_prev_keys.find(c) == g_prev_keys.end()) new_keys.insert(c);
    }
    bool enter_pressed = status.enter && !g_prev_enter;
    bool del_pressed   = status.del   && !g_prev_del;

    g_prev_keys  = current_keys;
    g_prev_enter = status.enter;
    g_prev_del   = status.del;

    // ── Global dev keys（仅 dev 构建 `-e islands-dev` 启用；参与者 `islands` 构建无此键，
    //    防误触 '1' 触发 restart_all+leave 把自己踢下大屏，线下实测 P0 — 2026-06-25）──
#if defined(LOITER_DEV_KEYS)
    bool in_text_input = (g_screen == P1_02_USERNAME) || (g_screen == P2_06_SAY_INPUT);
    if (!in_text_input) {
        if (new_keys.count('1')) { restart_all(); return; }
        if (new_keys.count('2')) { fill_dev_state(); goto_screen(P2_01_LIVE_MIRROR); return; }
        if (new_keys.count('3')) { fill_dev_state(); goto_screen(P3_01_WAITING); return; }
    }
#endif

    // Dispatch input
    dispatch_input(new_keys, enter_pressed, del_pressed);

    // Animated screens: throttle to ~16 fps. Partial-redraw avoids flicker even at higher rates.
    if (needs_animation_redraw()) {
        uint32_t now = millis();
        if (now - g_last_anim_redraw >= 60) {
            g_dirty = true;
            g_last_anim_redraw = now;
        }
    }

    if (g_dirty) {
        // 先清再画：draw 内部若 goto_screen（如 P3-01 ready→P3-02）会重设 g_dirty=true，
        // 需保留到下一帧驱动新屏首绘。若反过来（画完再清）会 clobber 掉这次转屏的 dirty，
        // 非动画屏（P3-02）就永不重绘 → 定格上一屏（reading 卡进度条真因）。
        g_dirty = false;
        dispatch_redraw();
    }

    delay(20);
}

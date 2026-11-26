// The Windows DS core: the app's own melonDS engine (app/src/main/cpp/nds/pk_nds.cpp, the same code the Android build
// runs) behind libretro. What the engine gives the app it gives here instead: EXPAND's wide overworld, the game's own
// functions (PC, FLY, ...), saves written the app's way, and melonDS states.
//
// The picture is the screen the game is on, in DS pixels, and the frontend scales it to the window:
//   - EXPAND on (the companion's ASPECT) and the field on show: the window is the whole canvas, the top screen in the
//     middle and EXPAND's margins around it, as far as the window reaches (pokedaisy_window, the frontend's window, is
//     read every frame, so resizing and fullscreen re-lay it). ZOOM (the companion's row) grows that canvas 1.25x, 1.5x
//     or 2x, and the frontend scales the whole picture down.
//   - a DS LAYOUT (the companion's SETTINGS): the two screens in its arrangement, on a canvas of the window's own shape
//     (or of what the host's status bar leaves), so a desktop window never shows the picture between black bars.
//   - otherwise (ONE SCREEN with EXPAND off) the two screens one above the other, on that same canvas.
// R2 shows the bottom screen (the touch screen) by itself until it's pressed again, on that canvas too.
#include "libretro.h"
#include "pd_ds_overlay.h"
#include "pk_nds_cores.h"
#include "pk_nds_field.h"
#include "pd_env.h"
#include "layout.h"
extern "C" {
#include "pd_fs.h" // pd_save_path: the host's own rule for which .sav is this game's
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

retro_environment_t env_cb;
retro_video_refresh_t video_cb;
retro_audio_sample_batch_t audio_cb;
retro_input_poll_t poll_cb;
retro_input_state_t input_cb;

constexpr int SAMPLE_RATE = 48000;
constexpr size_t STATE_MAX = 32u << 20;   // melonDS's states are ~8 MB: room to read one back
size_t stateSize = 0;                     // the size the first state of this game came out at

// The geometry of this frame (compose_layout): the picture's canvas, and, for a wide picture, the top screen's corner in it.
struct Layout {
    enum Mode { Wide, Arrange } mode = Arrange;
    int w = PK_NDS_W, h = PK_NDS_H * 2;
    int x = 0, y = 0;   // the top screen's corner in a Wide canvas
} layout;

// A chosen DS LAYOUT other than ONE SCREEN: ds_compose's rectangles over a 512 x 384 stage, the same arrangement the
// Linux DS core and the app use (top / bottom at 2x, side by side, stacked, large + small, picture in picture).
DsArrangement arrangement;

// ONE SCREEN's floating touch window. A menu gets it at three quarters of the top screen, centred on it (the app's
// 352 x 264 in its 512 x 384 stage is close to the same proportion). A battle gets 15/32 of the DS screen, a little under
// half, at the left under the enemy's HP bar, so the battlefield, both Pokemon and their bars stay in view.
DsRect touch_panel_rect(bool battle)
{
    if (layout.mode != Layout::Wide) return {};
    if (battle) {
        const int w = PK_NDS_W * 15 / 32, h = PK_NDS_H * 15 / 32;
        return {std::max(4, layout.x - PK_NDS_W * 3 / 8), layout.y + PK_NDS_H / 4 + 4, w, h};
    }
    const int w = PK_NDS_W * 3 / 4, h = PK_NDS_H * 3 / 4;
    return {layout.x + (PK_NDS_W - w) / 2, layout.y + (PK_NDS_H - h) / 2, w, h};
}

int panelW = 512, panelH = 384;
bool touchView = false;
bool padCombo = false;
bool wideEnabled = false;
bool haveFrame = false;
bool hudHost = false;   // the frontend draws the field HUD itself (PD_ENV_SET_HUD), at the window's pixels
int presentedW = PK_NDS_W, presentedH = PK_NDS_H * 2;   // the frame the frontend has (the overlay's size while it shows)
std::vector<uint32_t> canvas;   // the frame as it goes out (XRGB8888)
std::vector<uint32_t> scratch;  // a wide view asked for while the camera may have stopped
std::vector<uint32_t> lastWide; // the last wide frame, kept through a fade
std::vector<uint32_t> panelPixels; // the bottom screen in the host's XRGB8888, not the engine's RGBA words
bool lastWideValid = false;
long frameNo = 0;
std::string saveDir;
std::string savePath;
std::string romPath;
bool quickFf;

bool option(const char* key, std::string& value)
{
    retro_variable var {key, nullptr};
    if (!env_cb || !env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return false;
    value = var.value;
    return true;
}

// The save file the host keeps for a ROM: saves/<the ROM's own file name>.sav (extension kept), or, when only that
// one is there, saves/<the name without its extension>.sav (another emulator's), so the one the host loads into the
// save memory is the one the engine reads and writes.
std::string file_name(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void read_options()
{
    std::string value;
    // The window itself first: the host's PANEL preset names a screen to lay out for (a handheld's), and a canvas laid
    // out for it inside a differently shaped window would sit in bars. Falls back to the panel for a frontend without it.
    const bool haveWindow = option("pokedaisy_window", value) || option("pokedaisy_panel", value);
    if (haveWindow) {
        int w = 0, h = 0;
        if (sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            panelW = w;
            panelH = h;
        }
    }
}

// The window's area for the picture: the renderer's output, less the strip the host's status bar owns while it is on.
void frame_area(int& w, int& h)
{
    w = panelW > 0 ? panelW : 1280;
    h = panelH > 0 ? panelH : 720;
    pd_status_bar bar {};
    if (overlay_status(&bar) && bar.enabled && bar.active) {
        const int strip = pd_status_bar_height(h);
        if (strip > 0 && strip < h / 2) h -= strip;
    }
}

// The picture's canvas: the area's shape, sized around the DS's own 512 x 384 (a wider window widens it, a taller one
// heightens it). A canvas of another shape than the window would be shown between black bars; this one fills it.
void frame_canvas(int& w, int& h)
{
    int areaW, areaH;
    frame_area(areaW, areaH);
    const double aspect = (double) areaW / areaH;
    if (aspect >= 4.0 / 3.0) {
        w = (int) llround(384.0 * aspect);
        h = 384;
        if (w > 2048) {
            w = 2048;
            h = (int) llround(w / aspect);
        }
    } else {
        w = 512;
        h = (int) llround(512.0 / aspect);
        if (h > 2048) {
            h = 2048;
            w = (int) llround(h * aspect);
        }
    }
}

// The app lays an arrangement out on its 512 x 384 stage. The canvas can be another shape (a desktop window), so its
// rectangles are scaled to the largest size that fits and centred: the screens use the window rather than sitting in a
// 4:3 picture inside it, and ds_compose fills the rest with the blurred screen.
void fit_arrangement()
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    auto add = [&](const DsRect& r) {
        if (r.w <= 0 || r.h <= 0) return;
        if (!any) {
            x0 = r.x;
            y0 = r.y;
            x1 = r.x + r.w;
            y1 = r.y + r.h;
            any = true;
            return;
        }
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    };
    add(arrangement.top);
    add(arrangement.touch);
    if (!any) return;
    const double s = std::min((double) layout.w / (x1 - x0), (double) layout.h / (y1 - y0));
    const int cx = (layout.w - (int) llround((x1 - x0) * s)) / 2;
    const int cy = (layout.h - (int) llround((y1 - y0) * s)) / 2;
    auto place = [&](DsRect& r) {
        if (r.w <= 0 || r.h <= 0) return;
        const int left = cx + (int) llround((r.x - x0) * s);
        const int top = cy + (int) llround((r.y - y0) * s);
        const int right = cx + (int) llround((r.x + r.w - x0) * s);
        const int bottom = cy + (int) llround((r.y + r.h - y0) * s);
        r = {left, top, right - left, bottom - top};
    };
    place(arrangement.top);
    place(arrangement.touch);
}

// Where this frame goes: the companion's DS LAYOUT, or, for ONE SCREEN, EXPAND's canvas when it can be wide (at least 3:2
// and the field on show); else the two screens stacked. Every non-EXPAND shape is the shared arrangement table's
// (linux/libretro-ds/layout.cpp), fitted on the picture's canvas; R2 shows the bottom screen alone on it too.
void compose_layout()
{
    read_options();
    layout = Layout {};
    // A pinned arrangement (the player chose this DS LAYOUT), fitted to this frame's canvas. The HUD's box is the top
    // screen's, so the host keeps the HUD over it and off the rest of the picture.
    auto arrange = [&](DsArrangement a) {
        arrangement = a;
        layout.mode = Layout::Arrange;
        frame_canvas(layout.w, layout.h);
        fit_arrangement();
        overlay_hud_box(arrangement.top.x, arrangement.top.y, arrangement.top.w, arrangement.top.h);
    };
    const int choice = overlay_ds_layout();
    if (choice != DS_ONE) {
        DsArrangement a = ds_arrange(choice, DsScenePolicy {}, true);
        // This host draws the HUD over the whole top screen and cannot move its buttons as the Linux core's HUD does, so
        // PICTURE IN PICTURE's touch screen sits above the HUD's bottom-right row rather than over it.
        if (choice == DS_PIP) a.touch = {378, 208, 122, 92};
        arrange(a);
        return;
    }
    if (touchView) {
        arrange(ds_arrange(DS_BOTTOM, DsScenePolicy {}, true));
        return;
    }
    if (overlay_two_screen_app()) {
        // The game's two-screen pages (HeartGold / SoulSilver's summary, the PC, ...): both screens side by side, as
        // the app's ONE SCREEN lays them out. ONE SCREEN's floating touch panel covered the page behind it (the
        // summary's stats, nature and met place).
        arrange(ds_arrange(DS_SIDE, DsScenePolicy {}, true));
        return;
    }
    int areaW, areaH;
    frame_area(areaW, areaH);
    if (overlay_wide() && (long long) panelW * 10 >= (long long) panelH * 15) {
        // EXPAND extends the field sideways, not above and below the screen: at 1x the canvas is exactly the window's
        // shape, the screen's height, so every margin is one the 3D fills. A ZOOM step (the companion's SETTINGS, the
        // app's SCREEN group) grows the canvas by 1.25x, 1.5x or 2x: the field keeps filling it and the frontend shows
        // it at the next whole scale, which is more of the map with the game smaller. As in the app, a field text box steps
        // in only with AUTO ZOOM IN on; everything else (a battle, a menu, a fade, a title) always steps in to 1x. Past
        // the renderer's widest margins the step falls back.
        static const int zooms[4][2] = {{4, 4}, {5, 4}, {6, 4}, {8, 4}};
        int level = overlay_zoom();
        const int scene = pk_ndsm_wide_scene();
        if (scene != 0 && !(scene == 1 && !overlay_auto_zoom())) level = 0;
        const int baseW = std::max(PK_NDS_W, (int) llround((double) PK_NDS_H * areaW / areaH));
        for (; level > 0; --level) {
            const int w = std::max(PK_NDS_W, (int) llround((double) baseW * zooms[level][0] / zooms[level][1]));
            const int h = (int) llround((double) PK_NDS_H * zooms[level][0] / zooms[level][1]);
            if (w <= PK_NDS_W + 2 * 504 && h <= PK_NDS_H + 2 * 96) break; // pk_nds_wide.h's largest margins
        }
        layout.mode = Layout::Wide;
        layout.w = std::max(PK_NDS_W, (int) llround((double) baseW * zooms[level][0] / zooms[level][1]));
        layout.h = std::max(PK_NDS_H, (int) llround((double) PK_NDS_H * zooms[level][0] / zooms[level][1]));
        layout.x = (layout.w - PK_NDS_W) / 2;
        layout.y = (layout.h - PK_NDS_H) / 2;
        // The HUD keeps the 1x picture's place and size on screen: its box is the 1x top screen, scaled with the window.
        overlay_hud_box((baseW - PK_NDS_W) / 2 * zooms[level][0] / zooms[level][1], 0,
                        PK_NDS_W * zooms[level][0] / zooms[level][1], PK_NDS_H * zooms[level][0] / zooms[level][1]);
        return;
    }
    // ONE SCREEN with EXPAND off: the two screens stacked (the app's STACKED arrangement) on the picture's canvas.
    arrange(ds_arrange(DS_STACKED, DsScenePolicy {}, true));
}

inline uint32_t xrgb(uint32_t rgba)
{
    return 0xFF000000u | ((rgba & 0xFFu) << 16) | (rgba & 0xFF00u) | ((rgba >> 16) & 0xFFu);
}

// Whether the wide picture is held through this scene instead of the screen standing alone: a fade, or the field
// stopped for a warp (a door, a leader change) rather than a battle.
bool hold_wide(int scene, bool battle)
{
    if (scene == 5) return true;
    return scene == 4 && !battle;
}

// A full-screen card on the game's screen (the area name, the season): black on nearly every pixel, with its text on
// top. The picture then covers the whole window, as the card does on a DS, instead of sitting in a frame of world.
bool screen_is_card(const uint32_t* screen)
{
    if (!screen) return false;
    int dark = 0, sampled = 0;
    for (int y = 0; y < PK_NDS_H; y += 4)
        for (int x = 0; x < PK_NDS_W; x += 4, sampled++) {
            const uint32_t pixel = screen[(size_t) y * PK_NDS_W + x];
            if (std::max({pixel & 0xFFu, (pixel >> 8) & 0xFFu, (pixel >> 16) & 0xFFu}) < 16) dark++;
        }
    return dark * 100 >= sampled * 90;
}

// Whether the wide view has anything to show (the camera stopped: it is all black then).
bool has_picture(const std::vector<uint32_t>& pixels)
{
    for (const uint32_t pixel : pixels)
        if (pixel & 0x00FFFFFFu) return true;
    return false;
}

// Dims a frame by the game's own fade: k of 16, as the screen fades to black (a door, a warp).
void fade_frame(std::vector<uint32_t>& pixels, int k)
{
    if (k <= 0) return;
    if (k > 16) k = 16;
    const uint32_t f = (uint32_t) (16 - k);
    for (auto& pixel : pixels)
        pixel = 0xFF000000u | ((((pixel >> 16) & 0xFFu) * f / 16) << 16)
                             | ((((pixel >> 8) & 0xFFu) * f / 16) << 8)
                             | (((pixel & 0xFFu) * f) / 16);
}

void blit(uint32_t* dst, int dstW, int dstH, int x, int y, const uint32_t* src, int w, int h)
{
    if (!src) return;
    for (int row = 0; row < h; row++) {
        const int outY = y + row;
        if (outY < 0 || outY >= dstH) continue;
        const int from = std::max(0, -x), to = std::min(w, dstW - x);
        for (int col = from; col < to; col++) dst[(size_t) outY * dstW + x + col] = xrgb(src[(size_t) row * w + col]);
    }
}

void fill_rect(std::vector<uint32_t>& pixels, int w, int h, int x, int y, int rw, int rh, uint32_t color)
{
    const int x0 = std::clamp(x, 0, w), x1 = std::clamp(x + rw, 0, w);
    const int y0 = std::clamp(y, 0, h), y1 = std::clamp(y + rh, 0, h);
    for (int row = y0; row < y1; ++row)
        for (int col = x0; col < x1; ++col) pixels[(size_t) row * w + col] = color;
}

// A DS screen scaled into a panel by point sampling. Averaging the source pixels made the menu's small text soft; the
// panel keeps whole pixels and crisp edges instead, as the frontend's own nearest-neighbour upscale does.
void blit_nearest(uint32_t* dst, int dstW, int x0, int y0, int dw, int dh, const uint32_t* src, int sw, int sh)
{
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int dy = 0; dy < dh; ++dy) {
        const int sy = std::min(sh - 1, (dy * sh + sh / 2) / dh);
        for (int dx = 0; dx < dw; ++dx) {
            const int sx = std::min(sw - 1, (dx * sw + sw / 2) / dw);
            dst[(size_t) (y0 + dy) * dstW + x0 + dx] = xrgb(src[(size_t) sy * sw + sx]);
        }
    }
}

// The app's ONE SCREEN floating window: dim the wide picture, frame the panel, and lay the game's touch screen in it.
// The game keeps running underneath, and the panel takes the pointer only inside its frame. A host with the native-panel
// command draws the 256 x 192 screen itself over the panel's place, so the inside keeps the wide picture until it does.
void draw_touch_panel(std::vector<uint32_t>& pixels, int w, int h, bool battle, bool native_screen)
{
    const DsRect panel = touch_panel_rect(battle);
    if (!panel.w || !panel.h) return;
    const DsRect frame {panel.x - 5, panel.y - 5, panel.w + 10, panel.h + 10};
    // A menu dims the field behind its panel; a battle does not: both Pokemon, the bars and the battlefield stay at full
    // brightness while the command panel floats over them.
    if (!battle) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (panel.contains(x, y)) continue;
                uint32_t& pixel = pixels[(size_t) y * w + x];
                pixel = 0xFF000000u | ((((pixel >> 16) & 0xFFu) * 140 / 255) << 16)
                                     | ((((pixel >> 8) & 0xFFu) * 140 / 255) << 8)
                                     | (((pixel & 0xFFu) * 140) / 255);
            }
    }
    // A host that draws the native 256 x 192 bottom screen supplies its frame too (main.c draws the same 5 px dark,
    // 3 px white ring). A frontend without the command gets the frame and a point-sampled picture here, so the panel
    // still shows everywhere.
    if (native_screen) return;
    fill_rect(pixels, w, h, frame.x, frame.y, frame.w, frame.h, 0xFF303840u);
    fill_rect(pixels, w, h, panel.x - 3, panel.y - 3, panel.w + 6, panel.h + 6, 0xFFF8F8F8u);
    blit_nearest(pixels.data(), w, panel.x, panel.y, panel.w, panel.h, pk_ndsm_bottom(), PK_NDS_W, PK_NDS_H);
}

void send_status_bar()
{
    if (!env_cb) return;
    pd_status_bar st {};
    if (overlay_status(&st))
        env_cb(PD_ENV_SET_STATUS_BAR, &st);
}

// The screen on a canvas of the window's shape, the margins around it the blurred screen (as ds_compose fills an
// arrangement's margins): a picture EXPAND has no wide view for.
void compose_screen_with_margins(std::vector<uint32_t>& pixels, const uint32_t* top)
{
    static std::vector<uint32_t> screenPixels;
    screenPixels.resize(PK_NDS_W * PK_NDS_H);
    for (size_t i = 0; i < screenPixels.size(); ++i) screenPixels[i] = top ? xrgb(top[i]) : 0xFF000000u;
    DsArrangement framed {};
    framed.top = {layout.x, layout.y, PK_NDS_W, PK_NDS_H};
    ds_compose(pixels.data(), framed, screenPixels.data(), nullptr, nullptr, nullptr, nullptr, 0, layout.w, layout.h);
    for (auto& pixel : pixels) pixel |= 0xFF000000u;
}

// Whether EXPAND left the canvas around the screen all black: its 3D layer had nothing to draw there.
bool margins_blank(const std::vector<uint32_t>& pixels)
{
    for (int y = 0; y < layout.h; ++y)
        for (int x = 0; x < layout.w; ++x) {
            if (x >= layout.x && x < layout.x + PK_NDS_W && y >= layout.y && y < layout.y + PK_NDS_H) {
                x = layout.x + PK_NDS_W - 1;
                continue;
            }
            if (pixels[(size_t) y * layout.w + x] & 0x00FFFFFFu) return false;
        }
    return true;
}

void compose()
{
    canvas.assign((size_t) layout.w * layout.h, 0xFF000000u);
    const uint32_t* top = pk_ndsm_top();
    const uint32_t* bottom = pk_ndsm_bottom();
    const bool battle = overlay_battle();
    const bool touch = overlay_touch_screen();
    switch (layout.mode) {
    case Layout::Arrange: {
        // An arrangement: a chosen DS LAYOUT, ONE SCREEN's stacked or two-screen page, or R2's bottom screen. ds_compose
        // scales, frames and fills what the arrangement leaves with the blurred screen, as it does in the Linux core.
        static std::vector<uint32_t> arrangedTop, arrangedBottom;
        arrangedTop.resize(PK_NDS_W * PK_NDS_H);
        arrangedBottom.resize(PK_NDS_W * PK_NDS_H);
        for (size_t i = 0; i < arrangedTop.size(); ++i) {
            arrangedTop[i] = top ? xrgb(top[i]) : 0xFF000000u;
            arrangedBottom[i] = bottom ? xrgb(bottom[i]) : 0xFF000000u;
        }
        ds_compose(canvas.data(), arrangement, arrangedTop.data(), arrangedBottom.data(), nullptr, nullptr, nullptr, 0,
                   layout.w, layout.h);
        for (auto& pixel : canvas) pixel |= 0xFF000000u;   // ds_compose draws opaque 24-bit colours
        break;
    }
    case Layout::Wide: {
        // EXPAND draws the whole canvas (the screen's middle and the margins around it) when it has the field's camera
        // this frame (scene 0-2: the field, a text box over it, the extra overlays) or a battle it can carry into the
        // margins. The game's own battle, bag and party screens still float over it.
        const int scene = pk_ndsm_wide_scene();
        bool wide = false;
        if (scene <= 2) {
            wide = pk_ndsm_wide_draw(canvas.data(), -layout.x, -layout.y, layout.w, layout.h);
        } else if (scene == 4 && battle) {
            scratch.resize(canvas.size());
            if (pk_ndsm_wide_draw(scratch.data(), -layout.x, -layout.y, layout.w, layout.h) && has_picture(scratch)) {
                canvas = scratch;
                wide = true;
            }
        } else if (hold_wide(scene, battle)) {
            // A fade or a warp (the field stops for a door, a leader change): the camera may have stopped, so the last
            // wide frame is kept and faded with the screen rather than falling back to the small screen in a black frame.
            scratch.resize(canvas.size());
            if (pk_ndsm_wide_draw(scratch.data(), -layout.x, -layout.y, layout.w, layout.h) && has_picture(scratch)) {
                canvas = scratch;
                wide = true;
            }
        }
        // PD_DS_SCENE: the scene number and whether EXPAND had the picture, when either changes.
        static const bool sceneLog = std::getenv("PD_DS_SCENE") != nullptr;
        static int lastScene = -1, lastWideScene = -1;
        if (sceneLog && (scene != lastScene || (int) wide != lastWideScene))
            fprintf(stderr, "ds scene: frame %ld scene %d wide %d\n", frameNo, scene, (int) wide);
        lastScene = scene;
        lastWideScene = wide;
        if (wide) {
            for (auto& pixel : canvas) pixel = xrgb(pixel);
            if (hold_wide(scene, battle)) fade_frame(canvas, pk_ndsm_fade() & 0xFF);
            if (screen_is_card(top)) {
                std::fill(canvas.begin(), canvas.end(), 0xFF000000u);
                blit(canvas.data(), layout.w, layout.h, layout.x, layout.y, top, PK_NDS_W, PK_NDS_H);
            } else if (margins_blank(canvas)) {
                // EXPAND's 3D layer has nothing beside the screen here (the intro's 2D scenes, a flat menu): the
                // margins are the blurred screen, as an arrangement's are, rather than black bars.
                compose_screen_with_margins(canvas, top);
            } else {
                lastWide = canvas;
                lastWideValid = true;
            }
        } else if (hold_wide(scene, battle) && lastWideValid && lastWide.size() == canvas.size()) {
            canvas = lastWide;
            fade_frame(canvas, pk_ndsm_fade() & 0xFF);
        } else {
            compose_screen_with_margins(canvas, top);
        }
        break;
    }
    }
    // The game's touch screen is handed to a host that understands PD_ENV_SET_TOUCH_PANEL at its native 256 x 192, so the
    // host scales it once to its place and draws it above the HUD (the app's rule: the bottom screen stays whole, the HUD
    // keeps clear of it). ONE SCREEN's floating panel is the wide picture's: a menu, a battle's commands, bag or party. The
    // layouts that show both screens (large + small, picture in picture, side by side, stacked) hand over the bottom screen
    // in its own rectangle. The panel is opened before the companion / quick menu so those still take the frame.
    bool native_panel = false;
    if (!overlay_menu_open() && bottom) {
        int px = 0, py = 0, pw = 0, ph = 0;
        if (layout.mode == Layout::Wide && touch) {
            const DsRect panel = touch_panel_rect(battle);
            px = panel.x;
            py = panel.y;
            pw = panel.w;
            ph = panel.h;
        } else if (layout.mode == Layout::Arrange && arrangement.touch.w > 0) {
            px = arrangement.touch.x;
            py = arrangement.touch.y;
            pw = arrangement.touch.w;
            ph = arrangement.touch.h;
        }
        if (pw > 0 && ph > 0) {
            // The engine's bottom screen is RGBA words; the host's ARGB texture wants XRGB words, the same conversion the
            // frame's own pixels get. Without this the touch window's reds and blues are swapped.
            panelPixels.resize(PK_NDS_W * PK_NDS_H);
            for (size_t i = 0; i < panelPixels.size(); ++i) panelPixels[i] = xrgb(bottom[i]);
            pd_touch_panel msg { panelPixels.data(), PK_NDS_W, PK_NDS_H, px, py, pw, ph, 1 };
            native_panel = env_cb && env_cb(PD_ENV_SET_TOUCH_PANEL, &msg);
        }
    }
    if (!native_panel && env_cb) {
        pd_touch_panel msg {};
        env_cb(PD_ENV_SET_TOUCH_PANEL, &msg);
    }
    if (layout.mode == Layout::Wide && touch) draw_touch_panel(canvas, layout.w, layout.h, battle, native_panel);
    haveFrame = true;
    // The companion or the quick menu over it: the overlay's canvas at twice the game's size. The HUD goes on the top
    // screen's own pixels first, so the overlay is told where they sit in this picture.
    int outW = layout.w, outH = layout.h;
    if (layout.mode == Layout::Arrange) overlay_view(arrangement.top.x, arrangement.top.y, arrangement.top.w > 0);
    else overlay_view(layout.x, layout.y, true);
    overlay_compose(canvas, outW, outH);
    // The HUD at the window's own pixels, for a host that draws it: the canvas above is scaled to the window, and at a
    // ZOOM step (1.25x, 1.5x) a HUD blended into it would be resampled with the picture.
    if (hudHost && env_cb) {
        pd_hud hud {};
        overlay_hud_native(&hud);
        env_cb(PD_ENV_SET_HUD, &hud);
    }
    send_status_bar();
    presentedW = outW;
    presentedH = outH;
    video_cb(canvas.data(), outW, outH, outW * (int) sizeof(uint32_t));
}

// The pointer as the game's touch screen takes it: where it is on the canvas, in DS pixels, if that is the screen shown.
// [overlayOnly]: the companion has the picture, so the pointer is only the overlay's (the game gets no touch at all).
void update_touch(bool overlayOnly)
{
    // libretro's pointer is normalised over the frame (-0x8000 .. 0x7FFF both ways): its place on the canvas first.
    const int16_t nx = input_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    const int16_t ny = input_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    bool down = input_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
    const int px = (int) ((long) (nx + 0x7FFF) * layout.w / 0xFFFE);
    const int py = (int) ((long) (ny + 0x7FFF) * layout.h / 0xFFFE);
    // The game's touch screen is drawn above the HUD (the panel is the last thing on the picture, and the HUD's buttons may
    // sit in its corner), so a click inside it is the game's, not the HUD's: picture in picture and large + small keep both.
    bool on_panel = false;
    if (!overlay_menu_open()) {
        if (layout.mode == Layout::Wide && overlay_touch_screen())
            on_panel = touch_panel_rect(overlay_battle()).contains(px, py);
        else if (layout.mode == Layout::Arrange && arrangement.touch.w > 0)
            on_panel = arrangement.touch.contains(px, py);
    }
    // The overlay first, where its own picture is what the frontend shows: a click on the HUD or the quick menu.
    if (!on_panel && overlay_pointer((int) ((long) (nx + 0x7FFF) * presentedW / 0xFFFE),
                        (int) ((long) (ny + 0x7FFF) * presentedH / 0xFFFE), down)) {
        pk_ndsm_touch(false, 0, 0);
        return;
    }
    if (overlayOnly) {
        pk_ndsm_touch(false, 0, 0);
        return;
    }
    if (layout.mode == Layout::Arrange) {
        const DsPen pen = ds_pen(arrangement, px, py);
        pk_ndsm_touch(down && pen.valid, pen.x, pen.y);
        return;
    }
    // The floating touch screen takes the pointer only inside its panel; a click on the dimmed picture beside it is not
    // the game's touch screen. The panel's 256 x 192 pixels are scaled to its place in the wide canvas.
    if (layout.mode == Layout::Wide && overlay_touch_screen()) {
        const DsRect panel = touch_panel_rect(overlay_battle());
        const bool inside = panel.contains(px, py);
        const bool pressed = down && inside;
        const int tx = pressed ? std::clamp((px - panel.x) * PK_NDS_W / panel.w, 0, PK_NDS_W - 1) : 0;
        const int ty = pressed ? std::clamp((py - panel.y) * PK_NDS_H / panel.h, 0, PK_NDS_H - 1) : 0;
        pk_ndsm_touch(pressed, tx, ty);
        return;
    }
    // Only the wide picture is left here (an arrangement returned above): its screen rectangle is the game's touch screen.
    const int x = px - layout.x, y = py - layout.y;
    down = down && x >= 0 && x < PK_NDS_W && y >= 0 && y < PK_NDS_H;
    // PD_DS_TOUCH: the taps as the game gets them, for a pointer that can't be felt on the window.
    static const bool touchLog = std::getenv("PD_DS_TOUCH") != nullptr;
    if (touchLog && down) fprintf(stderr, "ds touch: %d,%d\n", x, y);
    pk_ndsm_touch(down, x, y);
}

// Every pad button in libretro's ids: what the overlay works in (the DS mask below only has the console's).
uint32_t read_input_mask()
{
    uint32_t keys = 0;
    for (unsigned id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
        if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) keys |= 1u << id;
    return keys;
}
uint32_t lastInputMask = 0;

// The overlay's keys, from its libretro ids to the engine's DS bits (bit 0 A, 1 B, 2 SELECT, ...).
uint32_t overlay_to_ds(uint32_t libretro)
{
    static const struct { unsigned lr, ds; } map[] = {
        {RETRO_DEVICE_ID_JOYPAD_A, 0}, {RETRO_DEVICE_ID_JOYPAD_B, 1},
        {RETRO_DEVICE_ID_JOYPAD_SELECT, 2}, {RETRO_DEVICE_ID_JOYPAD_START, 3},
        {RETRO_DEVICE_ID_JOYPAD_RIGHT, 4}, {RETRO_DEVICE_ID_JOYPAD_LEFT, 5},
        {RETRO_DEVICE_ID_JOYPAD_UP, 6}, {RETRO_DEVICE_ID_JOYPAD_DOWN, 7},
        {RETRO_DEVICE_ID_JOYPAD_R, 8}, {RETRO_DEVICE_ID_JOYPAD_L, 9},
        {RETRO_DEVICE_ID_JOYPAD_X, 10}, {RETRO_DEVICE_ID_JOYPAD_Y, 11},
    };
    uint32_t keys = 0;
    for (const auto& key : map)
        if (libretro & (1u << key.lr)) keys |= 1u << key.ds;
    return keys;
}

uint32_t read_keys()
{
    uint32_t keys = 0;
    static const struct { unsigned pad; unsigned ds; } map[] = {
        {RETRO_DEVICE_ID_JOYPAD_A, 0}, {RETRO_DEVICE_ID_JOYPAD_B, 1},
        {RETRO_DEVICE_ID_JOYPAD_SELECT, 2}, {RETRO_DEVICE_ID_JOYPAD_START, 3},
        {RETRO_DEVICE_ID_JOYPAD_RIGHT, 4}, {RETRO_DEVICE_ID_JOYPAD_LEFT, 5},
        {RETRO_DEVICE_ID_JOYPAD_UP, 6}, {RETRO_DEVICE_ID_JOYPAD_DOWN, 7},
        {RETRO_DEVICE_ID_JOYPAD_R, 8}, {RETRO_DEVICE_ID_JOYPAD_L, 9},
        {RETRO_DEVICE_ID_JOYPAD_X, 10}, {RETRO_DEVICE_ID_JOYPAD_Y, 11},
    };
    for (const auto& key : map)
        if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, key.pad)) keys |= 1u << key.ds;
    return keys;
}

// The frontend's state slot for this ROM (the host's states/<ROM>.ss), shared by the quick menu and the host hotkey.
std::string quickStateFile()
{
    std::string root = saveDir.empty() ? std::string(".") : saveDir;
    const size_t slash = root.find_last_of("/\\");
    if (slash != std::string::npos) root.erase(slash);
    return root + "/states/" + file_name(romPath) + ".ss";
}

bool state_exists(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t) n : 0);
    const bool ok = n <= 0 || fread(out.data(), 1, (size_t) n, f) == (size_t) n;
    fclose(f);
    return ok;
}

} // namespace

bool overlay_state_exists()
{
    return !romPath.empty() && state_exists(quickStateFile());
}

bool overlay_state_save()
{
    return pk_ndsm_active() && !romPath.empty() && pk_ndsm_save_state(quickStateFile().c_str());
}

bool overlay_state_load()
{
    if (!pk_ndsm_active() || romPath.empty()) return false;
    const std::string path = quickStateFile();
    if (!state_exists(path)) return false;
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) return false;
    overlay_reset();
    return pk_ndsm_load_state_mem_keeping_save(bytes.data(), bytes.size());   // the battery save stays (retro_unserialize)
}

bool overlay_fast_forward() { return quickFf; }

bool overlay_set_fast_forward(bool on)
{
    if (!env_cb || !env_cb(PD_ENV_SET_FAST_FORWARD, &on)) return false;
    quickFf = on;
    return true;
}

RETRO_API void retro_set_environment(retro_environment_t cb) { env_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "PokeDaisy DS";
    info->library_version = "1.0";
    info->valid_extensions = "nds";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
    memset(info, 0, sizeof *info);
    info->geometry.base_width = PK_NDS_W;
    info->geometry.base_height = PK_NDS_H * 2;
    info->geometry.max_width = 2048;
    info->geometry.max_height = 2048;
    info->geometry.aspect_ratio = 0.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_reset(void)
{
    overlay_reset();
    pk_ndsm_deinit();
    wideEnabled = false;
    lastWideValid = false;
    haveFrame = false;
    stateSize = 0;
    if (!romPath.empty() && pk_ndsm_init(romPath.c_str(), savePath.c_str(), SAMPLE_RATE)) {
        overlay_load(romPath.c_str(), saveDir.c_str());
        compose_layout();
    }
}

RETRO_API void retro_run(void)
{
    if (!pk_ndsm_active()) return;
    frameNo++;
    compose_layout();
    if (poll_cb) poll_cb();
    // The overlay first: L2 opens the companion (a tap toggles it, held for a moment it opens the quick menu), and while
    // it shows it takes the pad, so the game doesn't walk away underneath it.
    const uint32_t heldInput = read_input_mask();
    const uint32_t eaten = overlay_keys(heldInput & ~lastInputMask, heldInput);
    lastInputMask = heldInput;
    pk_ndsm_set_keys(read_keys() & ~overlay_to_ds(eaten));
    overlay_tick(pk_ndsm_field_bits());
    const bool showing = overlay_open();
    // R2: the bottom screen on show, at its own size, until pressed again (the touch screen, for a game that asks for it).
    // The host's DS hotkeys need SELECT held (its modifier), so R2 alone is the core's.
    const bool combo = !showing && input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) != 0;
    if (combo && !padCombo) {
        touchView = !touchView;
        compose_layout();
    }
    padCombo = combo;
    // The wheel the frontend reports (the mouse's, in notches): the companion's lists scroll with it.
    const int wheelUp = input_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
    const int wheelDown = input_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);
    if (wheelUp || wheelDown) overlay_wheel(wheelUp - wheelDown);
    update_touch(showing);
    pk_ndsm_set_draw(true, true);
    // EXPAND: the engine's own widescreen, on while the picture is laid out for a wide window. Without this the 3D is
    // only drawn for the screen and the margins would stay black.
    const bool wantWide = layout.mode == Layout::Wide;
    if (wantWide != wideEnabled) {
        pk_ndsm_wide_enable(wantWide);
        wideEnabled = wantWide;
    }
    if (wantWide) pk_ndsm_wide_window(-layout.x, -layout.y, layout.w, layout.h);
    pk_ndsm_run_frame();
    int16_t audio[2048 * 2];
    const int frames = pk_ndsm_read_audio(audio, 2048);
    if (frames > 0 && audio_cb) audio_cb(audio, frames);
    // PD_DS_AUDIO: a line a second of the audio the engine hands out, for a build without a sound card to check it.
    static const bool audioLog = std::getenv("PD_DS_AUDIO") != nullptr;
    static int audioRuns = 0;
    if (audioLog && (++audioRuns % 60) == 0) fprintf(stderr, "ds audio: %d frames\n", frames);
    compose();
}

RETRO_API size_t retro_serialize_size(void)
{
    if (!pk_ndsm_active()) return 0;
    // melonDS's states are variable length (they grow as sections are written), so the size a frontend allocates comes
    // from one taken once, with room to grow: the same game's stays about the same.
    if (!stateSize) {
        const size_t n = pk_ndsm_state_mem(nullptr, 0);
        if (n) stateSize = std::min(STATE_MAX, n + (n >> 4));
    }
    return stateSize ? stateSize : STATE_MAX;
}

// In memory, never through a file: the host's REWIND takes one every other frame.
RETRO_API bool retro_serialize(void* data, size_t size)
{
    if (!pk_ndsm_active() || !data || !size) return false;
    const size_t n = pk_ndsm_state_mem(static_cast<uint8_t*>(data), size);
    if (!n) return false;
    memset(static_cast<uint8_t*>(data) + n, 0, size - n);   // melonDS reads its own length; the padding is never read
    return true;
}

// The frontend owns the battery save (RETRO_MEMORY_SAVE_RAM, its .sav and its backups): a state never puts its own,
// maybe older, copy over it - as the Linux DS core and the GB / GBA cores keep it.
RETRO_API bool retro_unserialize(const void* data, size_t size)
{
    if (!pk_ndsm_active() || !data || size > STATE_MAX) return false;
    overlay_reset();
    return pk_ndsm_load_state_mem_keeping_save(static_cast<const uint8_t*>(data), size);
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM) return const_cast<uint8_t*>(pk_ndsm_save_data());
    if (id == RETRO_MEMORY_SYSTEM_RAM) return const_cast<uint8_t*>(pk_ndsm_mainram());
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM) return pk_ndsm_save_length();
    if (id == RETRO_MEMORY_SYSTEM_RAM) return pk_ndsm_mainram() ? 4u << 20 : 0;
    return 0;
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
    if (!game || !game->path) return false;
    const char* dir = nullptr;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir) saveDir = dir;
    romPath = game->path;
    const std::string saveRoot = saveDir.empty() ? std::string(".") : saveDir;
    // The host's rule (linux/pd_fs.h), so both name the same file: <rom>.sav, or another emulator's <base>.sav when no
    // other ROM beside this one has that base name.
    char sav[4096];
    pd_save_path(saveRoot.c_str(), game->path, sav, sizeof sav);
    savePath = sav;

    retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

    read_options();
    wideEnabled = false;
    touchView = false;
    padCombo = false;
    lastWideValid = false;
    frameNo = 0;
    stateSize = 0;
    if (!pk_ndsm_init(game->path, savePath.c_str(), SAMPLE_RATE)) return false;
    // RetroAchievements' memory map: main RAM at 0x02000000 and the ARM9's DTCM at 0x0E000000. The descriptors point
    // straight at the engine's memory, so rc_client reads the game while it runs.
    if (env_cb && pk_ndsm_mainram() && pk_ndsm_dtcm()) {
        struct retro_memory_descriptor desc[2] = {};
        desc[0].ptr = const_cast<uint8_t*>(pk_ndsm_mainram());
        desc[0].start = 0x02000000;
        desc[0].len = 4u << 20;
        desc[1].ptr = const_cast<uint8_t*>(pk_ndsm_dtcm());
        desc[1].start = 0x0E000000;
        desc[1].len = 0x4000;
        struct retro_memory_map map = {desc, 2};
        env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
    }
    // The host loads the .sav into the save memory it reads here; the engine has read the same file at boot.
    overlay_load(game->path, saveDir.c_str());
    // Whether this frontend draws the HUD itself: it takes the HUD's own 256x192 picture (PD_ENV_SET_HUD) and scales it
    // to the window, so a ZOOM step's smaller canvas doesn't soften it.
    pd_hud probe {};
    hudHost = env_cb && env_cb(PD_ENV_SET_HUD, &probe);
    overlay_hud_host(hudHost);
    quickFf = false;
    if (env_cb) env_cb(PD_ENV_SET_FAST_FORWARD, &quickFf);
    compose_layout();
    return true;
}

RETRO_API void retro_unload_game(void)
{
    overlay_reset();
    if (hudHost && env_cb) { // the HUD's picture goes with the overlay's buffers
        pd_hud off {};
        env_cb(PD_ENV_SET_HUD, &off);
    }
    if (env_cb) {
        pd_status_bar off {};
        env_cb(PD_ENV_SET_STATUS_BAR, &off);
    }
    quickFf = false;
    // A save the game is still writing ("SAVING...") finishes first, as the app does before it stops a DS game: else
    // the newer slot is left half written.
    pk_ndsm_finish_save(600);
    pk_ndsm_deinit();
    haveFrame = false;
    romPath.clear();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

RETRO_API void retro_cheat_reset(void) {}

RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

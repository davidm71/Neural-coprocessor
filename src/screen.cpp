// ---------------------------------------------------------------------------
// screen.cpp - R108. THE IDLE SCREEN, drawn entirely with scissored clears.
// See screen.hpp for why this exists and why it can work when nothing else can.
// ---------------------------------------------------------------------------

#include "screen.hpp"

#include <cstring>
#include <cstdio>

namespace mgpu
{
namespace screen
{
namespace
{

// ---- THE FONT ----
//
// 5 wide by 7 tall, one bit per pixel, row-major from the top: bit (row*5+col).
// 35 bits, so it rides in the low half of a 64-bit word. Generated from an
// ASCII-art source and rendered back out to be read by eye before it was
// allowed to become a table - a mistyped glyph in an error message is a bug
// that only shows up on the day someone needs the message.
struct glyph { char c; unsigned long long bits; };

const glyph FONT[] = {
    { ' ', 0x000000000ULL },
    { '-', 0x0000F8000ULL },
    { '.', 0x18C000000ULL },
    // V2. Added so the error screen can carry a repo path. Bottom-left to
    // top-right, one pixel per row with two doubled rows so the slope reads at
    // small sizes. Drawn out and checked by eye before it went in the table,
    // for the reason the comment above this one gives.
    { '/', 0x44222110ULL },
    { '0', 0x3A33AE62EULL },
    { '1', 0x3884210C4ULL },
    { '2', 0x7C444422EULL },
    { '3', 0x3A308311FULL },
    { '4', 0x211F4A988ULL },
    { '5', 0x3A3083C3FULL },
    { '6', 0x3A317862EULL },
    { '7', 0x08422221FULL },
    { '8', 0x3A317462EULL },
    { '9', 0x3A30F462EULL },
    { ':', 0x00C6018C0ULL },
    { 'A', 0x4631FC62EULL },
    { 'B', 0x3E318BE2FULL },
    { 'C', 0x3A210862EULL },
    { 'D', 0x3E318C62FULL },
    { 'E', 0x7C217843FULL },
    { 'F', 0x04217843FULL },
    { 'G', 0x3A31E862EULL },
    { 'H', 0x4631FC631ULL },
    { 'I', 0x108421084ULL },
    { 'J', 0x3A3184210ULL },
    { 'K', 0x452519531ULL },
    { 'L', 0x7C2108421ULL },
    { 'M', 0x46318D771ULL },
    { 'N', 0x4631CD671ULL },
    { 'O', 0x3A318C62EULL },
    { 'P', 0x04217C62FULL },
    { 'Q', 0x59358C62EULL },
    { 'R', 0x45257C62FULL },
    { 'S', 0x3A308382EULL },
    { 'T', 0x10842109FULL },
    { 'U', 0x3A318C631ULL },
    { 'V', 0x11518C631ULL },
    { 'W', 0x4775AC631ULL },
    { 'X', 0x462A22A31ULL },
    { 'Y', 0x108422A31ULL },
    { 'Z', 0x7C222221FULL },
};
const int FONT_N = (int)(sizeof(FONT) / sizeof(FONT[0]));

unsigned long long glyph_bits(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (int i = 0; i < FONT_N; ++i)
        if (FONT[i].c == c) return FONT[i].bits;
    return 0ull;   // unknown -> blank, never a wrong glyph
}

// ---- THE ONLY PRIMITIVE ----
//
// One filled rectangle. Everything on this screen - the field, the sweep, every
// pixel of every letter - is this call. Clipped here rather than at the call
// sites so no caller can walk off the target.
void fill(ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
          const float rgba[4], int x, int y, int w, int h,
          int maxw, int maxh)
{
    if (w <= 0 || h <= 0) return;
    if (x >= maxw || y >= maxh) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > maxw) w = maxw - x;
    if (y + h > maxh) h = maxh - y;
    if (w <= 0 || h <= 0) return;

    D3D12_RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    cl->ClearRenderTargetView(rtv, rgba, 1, &r);
}

int text_width(const char *s, int px, int gap)
{
    if (s == nullptr) return 0;
    const int n = (int)std::strlen(s);
    if (n == 0) return 0;
    return n * (5 * px + gap) - gap;
}

void draw_text(ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
               const char *s, int x, int y, int px, int gap,
               const float rgba[4], int maxw, int maxh)
{
    if (s == nullptr) return;
    for (int i = 0; s[i] != 0; ++i)
    {
        const unsigned long long b = glyph_bits(s[i]);
        const int gx = x + i * (5 * px + gap);
        if (b == 0ull) continue;
        for (int row = 0; row < 7; ++row)
        {
            // Runs, not single pixels: a row of a glyph is at most three solid
            // spans, so this is a handful of rectangles instead of 35.
            int col = 0;
            while (col < 5)
            {
                if ((b & (1ull << (row * 5 + col))) == 0ull) { ++col; continue; }
                int run = 0;
                while (col + run < 5 &&
                       (b & (1ull << (row * 5 + col + run))) != 0ull) ++run;
                fill(cl, rtv, rgba, gx + col * px, y + row * px,
                     run * px, px, maxw, maxh);
                col += run;
            }
        }
    }
}

float ease(float t)   // 0..1 -> 0..1..0, smooth
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float u = t * 2.0f - 1.0f;
    const float v = 1.0f - u * u;
    return v * v;
}

} // namespace

void draw(ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
          unsigned width, unsigned height, unsigned long long frame,
          int st, const char *line1, const char *line2)
{
    if (cl == nullptr || width == 0 || height == 0) return;

    const int W = (int)width;
    const int H = (int)height;

    // ---- THE FIELD ----
    // 8% grey. Dark enough to rest on an OLED for an hour, light enough that
    // the screen is obviously ON and not a dead signal.
    const float base[4] = { 0.030f, 0.032f, 0.035f, 1.0f };
    fill(cl, rtv, base, 0, 0, W, H, W, H);

    // ---- THE SWEEP ----
    // Period and brightness carry the state; the hue does not move. A slow dim
    // drift means something is missing. Quicker and brighter means armed and
    // waiting. Error is the one time any saturation appears at all.
    const float period = (st == st_waiting) ? 90.0f : 180.0f;
    const float phase  = (float)(frame % (unsigned long long)period) / period;

    float peak[4] = { 0.16f, 0.17f, 0.19f, 1.0f };
    if (st == st_waiting) { peak[0] = 0.22f; peak[1] = 0.24f; peak[2] = 0.27f; }
    if (st == st_error)   { peak[0] = 0.26f; peak[1] = 0.07f; peak[2] = 0.07f; }

    const int bar_h  = (H / 220 > 2) ? H / 220 : 2;
    const int bar_y  = H - (H / 6);
    const int bar_w  = W / 5;
    const int bar_x  = (int)(phase * (float)(W + bar_w)) - bar_w;

    // Four sub-rects with a falling ramp give the bar a soft edge without a
    // gradient, which a clear cannot do.
    for (int k = 0; k < 4; ++k)
    {
        const float a = ease((float)(k + 1) / 5.0f) * 0.9f;
        float c[4];
        for (int i = 0; i < 3; ++i) c[i] = base[i] + (peak[i] - base[i]) * a;
        c[3] = 1.0f;
        const int seg = bar_w / 4;
        fill(cl, rtv, c, bar_x + k * seg, bar_y, seg, bar_h, W, H);
    }

    // ---- THE MESSAGE ----
    // Scaled off the width so it is the same apparent size at 1080p and 4K,
    // and never wider than three quarters of the screen.
    int px = W / 320;
    if (px < 1) px = 1;
    const int gap = px * 2;

    while (px > 1 && text_width(line1, px, gap) > (W * 3) / 4) --px;

    float fg[4] = { 0.62f, 0.64f, 0.68f, 1.0f };
    if (st == st_error) { fg[0] = 0.78f; fg[1] = 0.34f; fg[2] = 0.32f; }

    const int y1 = H / 3;
    if (line1 != nullptr && line1[0] != 0)
        draw_text(cl, rtv, line1, (W - text_width(line1, px, gap)) / 2, y1,
                  px, gap, fg, W, H);

    // Second line is dimmer and smaller: it is the instruction, not the fault.
    if (line2 != nullptr && line2[0] != 0)
    {
        int px2 = (px > 1) ? px - (px / 3) : 1;
        if (px2 < 1) px2 = 1;

        // V2. LINE 2 WAS NEVER CLAMPED. Only line1 was shrunk to fit, so a
        // long second line - an error message with a URL in it, say - ran off
        // both edges of the screen, centred and unreadable. The whole point of
        // this screen is to be legible when nothing else works.
        while (px2 > 1 && text_width(line2, px2, px2 * 2) > (W * 9) / 10) --px2;

        const int gap2 = px2 * 2;
        const float dim[4] = { fg[0] * 0.6f, fg[1] * 0.6f, fg[2] * 0.6f, 1.0f };
        draw_text(cl, rtv, line2, (W - text_width(line2, px2, gap2)) / 2,
                  y1 + 7 * px + 6 * px, px2, gap2, dim, W, H);
    }
}


void draw_fps_hud(ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                  unsigned width, unsigned height, double game_fps, double nr_fps)
{
    if (cl == nullptr || width == 0 || height == 0) return;

    const int W = (int)width;
    const int H = (int)height;
    int px = W / 960;                 // compact at 4K, still readable at 1440p
    if (px < 2) px = 2;
    const int gap = px;
    const int pad = px * 4;
    const int line_h = 7 * px;

    char game[48] = {};
    char nr[48] = {};
    snprintf(game, sizeof game, "GAME %.1f FPS", game_fps);
    snprintf(nr, sizeof nr, "NR   %.1f FPS", nr_fps);

    const int tw = (text_width(game, px, gap) > text_width(nr, px, gap))
                 ? text_width(game, px, gap) : text_width(nr, px, gap);
    const int box_w = tw + pad * 2;
    const int box_h = line_h * 2 + pad * 3;
    const int x = pad;
    const int y = pad;

    // Opaque neutral backing: ClearRenderTargetView cannot alpha-blend, and an
    // opaque patch keeps the numbers legible over every scene without shaders.
    const float bg[4] = { 0.018f, 0.020f, 0.023f, 1.0f };
    const float fg[4] = { 0.88f, 0.90f, 0.94f, 1.0f };
    fill(cl, rtv, bg, x, y, box_w, box_h, W, H);
    draw_text(cl, rtv, game, x + pad, y + pad, px, gap, fg, W, H);
    draw_text(cl, rtv, nr, x + pad, y + pad * 2 + line_h, px, gap, fg, W, H);
}

} // namespace screen
} // namespace mgpu

// tray_draw.inc - TR2: the icon artwork, shared by the tray icon (tray.cpp)
// and the installer window (installer/setup.cpp). Plain C++, no Windows calls.
#pragma once
#include <initializer_list>

// TR2: the tray icon - a blue rounded square, a white graphics card (bracket,
// body with two fans, connector), and a status dot in the corner. Drawn with
// 4x4 supersampling into straight-alpha BGRA (0xAARRGGBB). No Windows calls,
// so the same code renders the preview off-line.
static void tr_render(int n, unsigned char sr, unsigned char sg, unsigned char sb, unsigned *out)
{
    const double S = (double)n;
    struct col { double r, g, b, a; };
    auto in_rrect = [](double x, double y, double x0, double y0, double x1, double y1, double rad) {
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        const double cx = x < x0 + rad ? x0 + rad : (x > x1 - rad ? x1 - rad : x);
        const double cy = y < y0 + rad ? y0 + rad : (y > y1 - rad ? y1 - rad : y);
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad;
    };
    auto in_circ = [](double x, double y, double cx, double cy, double r) {
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    };
    const col blue  = { 30, 111, 217, 255 };
    const col white = { 255, 255, 255, 255 };
    const col dot   = { (double)sr, (double)sg, (double)sb, 255 };
    for (int py = 0; py < n; ++py)
        for (int px = 0; px < n; ++px)
        {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                {
                    const double x = (px + (sx + 0.5) / 4.0) / S;   // 0..1
                    const double y = (py + (sy + 0.5) / 4.0) / S;
                    col c = { 0, 0, 0, 0 };
                    if (in_rrect(x, y, 0.0, 0.0, 1.0, 1.0, 0.20)) c = blue;
                    // the card: bracket, body, connector
                    const bool bracket = in_rrect(x, y, 0.10, 0.20, 0.17, 0.74, 0.02);
                    const bool body    = in_rrect(x, y, 0.20, 0.27, 0.88, 0.62, 0.05);
                    const bool conn    = x >= 0.32 && x <= 0.66 && y >= 0.62 && y <= 0.71;
                    if (bracket || body || conn) c = white;
                    // two fans cut out of the body, each with a white hub
                    for (double fx : { 0.39, 0.69 })
                        if (body && in_circ(x, y, fx, 0.445, 0.125))
                            c = in_circ(x, y, fx, 0.445, 0.04) ? white : blue;
                    // the status dot, with a blue ring so it stands off the card
                    if (in_circ(x, y, 0.80, 0.80, 0.20)) c = blue;
                    if (in_circ(x, y, 0.80, 0.80, 0.155)) c = dot;
                    r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                }
            if (a <= 0.0) { out[py * n + px] = 0; continue; }
            const unsigned R = (unsigned)(r / a + 0.5), G = (unsigned)(g / a + 0.5), B = (unsigned)(b / a + 0.5);
            const unsigned A = (unsigned)(a / 16.0 + 0.5);
            out[py * n + px] = (A << 24) | (R << 16) | (G << 8) | B;
        }
}


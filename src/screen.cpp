// ---------------------------------------------------------------------------
// screen.cpp - R108. THE IDLE SCREEN, drawn entirely with scissored clears.
// See screen.hpp for why this exists and why it can work when nothing else can.
// ---------------------------------------------------------------------------

#include "screen.hpp"

#include <cstring>
#include <cstdio>
#include <d3dcompiler.h>

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


// ---- LIVE HUD TRANSLUCENT BACKGROUND ----
//
// ClearRenderTargetView can only replace pixels, so the original HUD box had
// to be opaque.  The live HUD is different from the idle/error screen: there
// is already a neural frame underneath it, and preserving that frame is useful.
// This tiny pipeline draws exactly two alpha-blended triangles over the HUD
// rectangle.  It is created lazily on first use and has no per-frame allocation,
// readback or synchronization.  If shader/pipeline creation ever fails, the HUD
// falls back to the old opaque clear rather than disappearing.
struct hud_blend_state
{
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *pso = nullptr;
    bool attempted = false;
};

hud_blend_state g_hud_blend;

typedef HRESULT (WINAPI *hud_d3dcompile_fn)(LPCVOID, SIZE_T, LPCSTR,
                                             const D3D_SHADER_MACRO *, ID3DInclude *,
                                             LPCSTR, LPCSTR, UINT, UINT,
                                             ID3DBlob **, ID3DBlob **);

bool ensure_hud_blend_pipeline(ID3D12GraphicsCommandList *cl)
{
    if (g_hud_blend.pso != nullptr && g_hud_blend.rs != nullptr) return true;
    if (g_hud_blend.attempted || cl == nullptr) return false;
    g_hud_blend.attempted = true;

    ID3D12Device *dev = nullptr;
    if (FAILED(cl->GetDevice(IID_PPV_ARGS(&dev))) || dev == nullptr) return false;

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (dc == nullptr) { dev->Release(); return false; }
    hud_d3dcompile_fn compile = reinterpret_cast<hud_d3dcompile_fn>(
        GetProcAddress(dc, "D3DCompile"));
    if (compile == nullptr) { dev->Release(); return false; }

    static const char *hlsl =
        "cbuffer Hud : register(b0) { float4 rect; float4 color; };\n"
        "struct V { float4 p : SV_Position; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "  float2 q[6] = { float2(0,0),float2(1,0),float2(0,1),"
        "float2(0,1),float2(1,0),float2(1,1) };\n"
        "  float2 p = float2(lerp(rect.x,rect.z,q[id].x),"
        "lerp(rect.y,rect.w,q[id].y)); V o; o.p=float4(p,0,1); return o; }\n"
        "float4 ps(V i) : SV_Target { return color; }\n";

    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compile(hlsl, std::strlen(hlsl), "hud", nullptr, nullptr,
                         "vs", "vs_5_0", 0, 0, &vs, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (FAILED(hr) || vs == nullptr) { dev->Release(); return false; }
    hr = compile(hlsl, std::strlen(hlsl), "hud", nullptr, nullptr,
                 "ps", "ps_5_0", 0, 0, &ps, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (FAILED(hr) || ps == nullptr) { vs->Release(); dev->Release(); return false; }

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.RegisterSpace = 0;
    rp.Constants.Num32BitValues = 8;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &sig, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (FAILED(hr) || sig == nullptr)
    {
        vs->Release(); ps->Release(); dev->Release(); return false;
    }
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  IID_PPV_ARGS(&g_hud_blend.rs));
    sig->Release();
    if (FAILED(hr) || g_hud_blend.rs == nullptr)
    {
        vs->Release(); ps->Release(); dev->Release(); return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_hud_blend.rs;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.AlphaToCoverageEnable = FALSE;
    pd.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.LogicOpEnable = FALSE;
    b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pd.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pd.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.RasterizerState.MultisampleEnable = FALSE;
    pd.RasterizerState.AntialiasedLineEnable = FALSE;
    pd.RasterizerState.ForcedSampleCount = 0;
    pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.InputLayout = { nullptr, 0 };
    pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;

    hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_hud_blend.pso));
    vs->Release();
    ps->Release();
    dev->Release();
    if (FAILED(hr) || g_hud_blend.pso == nullptr)
    {
        if (g_hud_blend.rs != nullptr) { g_hud_blend.rs->Release(); g_hud_blend.rs = nullptr; }
        return false;
    }
    return true;
}

bool translucent_hud_box(ID3D12GraphicsCommandList *cl,
                         D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                         int x, int y, int w, int h, int W, int H)
{
    if (!ensure_hud_blend_pipeline(cl)) return false;

    // NDC rectangle: left, top, right, bottom.  Alpha 0.58 keeps the neural
    // frame visible while giving the bitmap text enough contrast to read.
    const float c[8] = {
        2.0f * (float)x / (float)W - 1.0f,
        1.0f - 2.0f * (float)y / (float)H,
        2.0f * (float)(x + w) / (float)W - 1.0f,
        1.0f - 2.0f * (float)(y + h) / (float)H,
        0.018f, 0.020f, 0.023f, 0.58f
    };

    D3D12_VIEWPORT vp{};
    vp.Width = (float)W; vp.Height = (float)H;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    D3D12_RECT sc{ 0, 0, W, H };

    cl->SetPipelineState(g_hud_blend.pso);
    cl->SetGraphicsRootSignature(g_hud_blend.rs);
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->SetGraphicsRoot32BitConstants(0, 8, c, 0);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(6, 1, 0, 0);
    return true;
}

void draw_fps_hud(ID3D12GraphicsCommandList *cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                  unsigned width, unsigned height, int mode,
                  double input_fps, double consumed_fps, double nr_eval_fps,
                  double nr_skip_fps, double present_fps, double nr_gpu_ms,
                  double gen_fps, const char (*status)[48], int n_status)
{
    if (cl == nullptr || width == 0 || height == 0 || mode == 0) return;

    const int W = (int)width;
    const int H = (int)height;
    int px = W / 960;
    if (px < 2) px = 2;
    const int gap = px;
    const int pad = px * 4;
    const int line_h = 7 * px;

    // Up to 7 FPS lines (FG1) and 6 status lines (ST1).
    static const int MAXL = 13;
    char text[MAXL][48] = {};
    bool is_status[MAXL] = {};
    int lines = 0;
    if (mode == 1)
    {
        // BASIC shows the final successful MGPU output-present rate.
        snprintf(text[lines++], 48, "FPS %.1f", present_fps);
    }
    else
    {
        // DETAIL exposes both sides of the MGPU pipeline without implying that
        // the producer counter is an independent measurement of game Present.
        snprintf(text[lines++], 48, "MGPU IN  %.1f FPS", input_fps);
        snprintf(text[lines++], 48, "CONSUMED %.1f FPS", consumed_fps);
        snprintf(text[lines++], 48, "NR EVAL  %.1f FPS", nr_eval_fps);
        snprintf(text[lines++], 48, "NR SKIP  %.1f FPS", nr_skip_fps);
        snprintf(text[lines++], 48, "PRESENT  %.1f FPS", present_fps);
        snprintf(text[lines++], 48, "NR GPU   %.2f MS", nr_gpu_ms);
        if (gen_fps >= 0.0)
            snprintf(text[lines++], 48, "GENERATED %.1f FPS", gen_fps);
    }
    for (int i = 0; status != nullptr && i < n_status && lines < MAXL; ++i)
    {
        memcpy(text[lines], status[i], 48);
        text[lines][47] = '\0';
        is_status[lines++] = true;
    }

    int tw = 0;
    for (int i = 0; i < lines; ++i)
    {
        const int w = text_width(text[i], px, gap);
        if (w > tw) tw = w;
    }
    const int box_w = tw + pad * 2;
    const int box_h = line_h * lines + pad * (lines + 1);
    const int x = pad;
    const int y = pad;

    const float bg[4] = { 0.018f, 0.020f, 0.023f, 1.0f };
    const float fg[4] = { 0.88f, 0.90f, 0.94f, 1.0f };
    const float sf[4] = { 0.55f, 0.85f, 1.00f, 1.0f };   // ST1: status lines, tinted
    if (!translucent_hud_box(cl, rtv, x, y, box_w, box_h, W, H))
        fill(cl, rtv, bg, x, y, box_w, box_h, W, H);
    for (int i = 0; i < lines; ++i)
        draw_text(cl, rtv, text[i], x + pad, y + pad * (i + 1) + line_h * i, px, gap,
                  is_status[i] ? sf : fg, W, H);
}

} // namespace screen
} // namespace mgpu

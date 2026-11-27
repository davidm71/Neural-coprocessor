// ---------------------------------------------------------------------------
// live_resize.hpp - LR1 (LR97): the rules for following a resolution or
// format change of the game's motion vectors and depth while the game runs.
// Portable, no Windows: tests/live_resize_test.cpp runs every rule here.
//
// WHAT CHANGES IN PLAY (Cyberpunk 2077, 2026-11-25):
//  - DLSS quality: the game's render size moves (1280x720 -> 853x480). The
//    motion vectors and depth move with it; the picture stays 2560x1440.
//  - DLSS model (Transformer / Transformer 2 / CNN): the motion-vector FORMAT
//    can change at the same size (10 = RGBA16F -> 34 = RG16F).
// Until IN96 both lanes were sized once, at arm, and any other size or format
// was refused for the rest of the session (RC1: restart the game).
//
// HOW IT IS FOLLOWED (the add-on code does the D3D12 work; these are its rules):
//  1. At arm each lane's region in the ring slot gets ROOM: enough bytes for
//     the lane at the picture's size (and, for vectors, at 8 bytes per pixel,
//     the largest format seen in practice). Only the bytes in use are ever
//     copied, so the room costs memory, not bus time.
//  2. The copy keeps refusing a shape it was not set up for, as before. Each
//     refusal is followed: the SAME new shape refused on `need` frames in a
//     row, with no frame of the old shape between, is a real change - a menu,
//     a load screen or an engine that alternates two buffers never gets there.
//  3. A real change is checked: a format GPU 1 can take (LR98, format_ok), the
//     picture's shape (a render size is the picture scaled on both axes - this
//     keeps a shadow map or a UI buffer out), and it must fit the room. Then it
//     is posted to the bridge.
//  4. The bridge applies it at the one safe moment it has (no GPU 1 work in
//     flight), rebuilds what was sized from the lane, and frames sealed after
//     that moment carry the new shape. Older frames lose that lane once.
// A shape that cannot be followed (too big, wrong shape, the rebuild failed) is
// HELD: not tried again, and the panel says to restart, as RC1 always did.
// ---------------------------------------------------------------------------
#pragma once

namespace mgpu
{
namespace live_resize
{

// Why a shape is not followed. Kept as ints in the stream state (atomics).
enum hold_why : int
{
    held_none     = 0,
    held_too_big  = 1,   // more bytes than the region's room
    held_shape    = 2,   // not the picture's shape (aspect differs)
    held_failed   = 3,   // the bridge could not rebuild for it
    held_off      = 4,   // LiveResize=0
    held_format   = 5,   // LR98: a format GPU 1 cannot take in this lane (see format_ok)
};

// A lane's shape: size and DXGI format, as the game's resource reports them.
struct shape
{
    unsigned w = 0, h = 0, fmt = 0;
};
inline bool same(const shape &a, const shape &b) { return a.w == b.w && a.h == b.h && a.fmt == b.fmt; }
inline bool empty(const shape &a) { return a.w == 0u || a.h == 0u; }

// How many distinct frames in a row a refused shape must be seen before it is
// a change rather than a passing thing. 20 frames is a third of a second at
// 60 fps - well past a load-screen flicker, well inside what a player waits for.
const unsigned NEED_FRAMES = 20u;

// The picture's shape rule, in thousandths: |w/h - pw/ph| <= tol of pw/ph.
// DLSS rounds its render sizes (2560x1440 / 3 = 853x480: 0.04% off), dynamic
// resolution and 8-pixel padding stay well under 1%. 4% keeps a 1:1 shadow map,
// a 2:1 octahedral buffer or a 4:3 UI target out on a 16:9 or 21:9 picture.
const unsigned ASPECT_TOL_PERMILLE = 40u;

inline bool same_aspect(unsigned w, unsigned h, unsigned pw, unsigned ph,
                        unsigned tol_permille = ASPECT_TOL_PERMILLE)
{
    if (w == 0u || h == 0u || pw == 0u || ph == 0u) return false;
    const unsigned long long a = (unsigned long long)w * ph;   // w/h compared with pw/ph,
    const unsigned long long b = (unsigned long long)h * pw;   // both sides times h*ph
    const unsigned long long d = (a > b) ? (a - b) : (b - a);
    return d * 1000ull <= b * (unsigned long long)tol_permille;
}

// The room's size at arm: the larger of what the game hands over now and the
// picture's own size, per axis.
inline void room_dims(unsigned w, unsigned h, unsigned pw, unsigned ph, unsigned &rw, unsigned &rh)
{
    rw = (w > pw) ? w : pw;
    rh = (h > ph) ? h : ph;
}

// Per lane, on the thread that makes the copy (the game's render thread).
struct follower
{
    shape cand;                        // the shape the copy refused last
    unsigned frames = 0;               // distinct frames in a row it was refused on
    unsigned long long last = ~0ull;   // the frame it was last counted on
    shape held;                        // a shape that is not followed (see hold_why)
    int held_why = held_none;
};

// The copy refused `got` on frame `frame` (the frame being built - the same
// number may come more than once if the engine binds the buffer twice).
// Returns the distinct frames in a row `got` has been refused on.
inline unsigned refused(follower &f, const shape &got, unsigned long long frame)
{
    if (!same(got, f.cand)) { f.cand = got; f.frames = 0u; f.last = ~0ull; }
    if (frame != f.last) { f.last = frame; ++f.frames; }
    return f.frames;
}

// The copy took the lane's current shape: whatever was being followed was a
// passing thing (a menu, a second buffer), so the count starts over.
inline void accepted(follower &f) { f.cand = shape{}; f.frames = 0u; f.last = ~0ull; }

inline bool is_held(const follower &f, const shape &s) { return f.held_why != held_none && same(f.held, s); }
inline void hold(follower &f, const shape &s, int why) { f.held = s; f.held_why = why; }

// ---- LR98: which FORMAT changes are followed ----
//
// Cyberpunk, 2026-11-26: applying a settings change recreated ReShade's runtime,
// and for a few dozen frames the depth lane was handed the game's own depth-
// stencil buffer (fmt 19, R32G8X24_TYPELESS - two planes) instead of the depth
// tap's R32_FLOAT (fmt 41). Same size, so LR97 followed it; GPU 1 made a fmt 19
// texture and copied plane 0's footprint (R32_TYPELESS) into it - an invalid
// copy, and the neural card's device was removed (DXGI_ERROR_INVALID_CALL).
//  - DEPTH follows SIZE only. Its format is fixed by the route the arm took
//    (the tap writes R32_FLOAT); another format is another resource.
//  - VECTORS follow a format change only between the plain float formats
//    motion vectors come in (DLSS model switches: RGBA16F <-> RG16F).
//  - BOTH: the copy's footprint must be in the resource's own format. A planar
//    or multi-plane format reports a different one (plane 0's), and GPU 1 could
//    not take it as one texture.
// DXGI values, kept here so the rule stays portable and testable.
const unsigned FMT_R32G32B32A32_FLOAT = 2u;
const unsigned FMT_R16G16B16A16_FLOAT = 10u;
const unsigned FMT_R32G32_FLOAT       = 16u;
const unsigned FMT_R16G16_FLOAT       = 34u;
inline bool plain_vector_format(unsigned f)
{
    return f == FMT_R32G32B32A32_FLOAT || f == FMT_R16G16B16A16_FLOAT ||
           f == FMT_R32G32_FLOAT || f == FMT_R16G16_FLOAT;
}
// `lane_fmt` is the format the lane has now, `got` the refused one, and
// `fp_fmt` the format GetCopyableFootprints reported for `got`'s subresource 0.
inline bool format_ok(bool depth, unsigned lane_fmt, unsigned got, unsigned fp_fmt)
{
    if (fp_fmt != got) return false;           // planar / multi-plane: never one texture
    if (got == lane_fmt) return true;          // a size-only change
    if (depth) return false;                   // depth: another format is another resource
    return plain_vector_format(lane_fmt) && plain_vector_format(got);
}

// What to do with a shape refused `frames` frames in a row.
enum class step
{
    wait,          // not yet a change
    post,          // a change that can be followed: hand it to the bridge
    refuse_big,    // a change, but its bytes exceed the room
    refuse_shape,  // a change, but not the picture's shape
    refuse_format, // LR98: a change, but to a format this lane cannot take
};
inline step decide(unsigned frames, bool aspect_ok, unsigned long long bytes, unsigned long long room,
                   bool fmt_ok = true, unsigned need = NEED_FRAMES)
{
    if (frames < need) return step::wait;
    if (!fmt_ok) return step::refuse_format;
    if (!aspect_ok) return step::refuse_shape;
    if (bytes == 0ull || bytes > room) return step::refuse_big;
    return step::post;
}

// The regions of one ring slot the GPU 0 staging copy moves (StageCopy=1).
// Before LR1 the whole slot was copied; with room in the slot that would move
// unused bytes every frame, so only what GPU 1 reads is copied: the seal and
// the colour (`head`, from 0), then depth, vectors and the HUD-less copy at
// their offsets with the bytes in use. Ranges that touch are merged. Returns
// the count (at most 4). A zero length leaves a region out.
struct range { unsigned long long off = 0, len = 0; };
inline unsigned slot_ranges(unsigned long long head,
                            unsigned long long d_off, unsigned long long d_len,
                            unsigned long long m_off, unsigned long long m_len,
                            unsigned long long h_off, unsigned long long h_len,
                            range out[4])
{
    range in[4];
    unsigned n_in = 0;
    if (head  != 0ull) in[n_in++] = range{ 0ull, head };
    if (d_len != 0ull) in[n_in++] = range{ d_off, d_len };
    if (m_len != 0ull) in[n_in++] = range{ m_off, m_len };
    if (h_len != 0ull) in[n_in++] = range{ h_off, h_len };
    // The slot layout is ascending (seal+colour, depth, vectors, HUD-less), but
    // sort anyway: four elements, and a wrong order would be a silent overlap.
    for (unsigned i = 1; i < n_in; ++i)
        for (unsigned j = i; j > 0 && in[j].off < in[j - 1].off; --j)
        { const range t = in[j]; in[j] = in[j - 1]; in[j - 1] = t; }
    unsigned n = 0;
    for (unsigned i = 0; i < n_in; ++i)
    {
        if (n != 0u && in[i].off <= out[n - 1].off + out[n - 1].len)
        {
            const unsigned long long end = in[i].off + in[i].len;
            if (end > out[n - 1].off + out[n - 1].len) out[n - 1].len = end - out[n - 1].off;
        }
        else out[n++] = in[i];
    }
    return n;
}

// The seal checks compare a frame against the lane's CURRENT geometry. A frame
// sealed before the bridge switched geometry (index below `from`) was built for
// the old one: it is not a contract mismatch, and its lane was invalidated.
inline bool sealed_before_switch(unsigned long long f, unsigned long long from) { return f < from; }

}   // namespace live_resize
}   // namespace mgpu

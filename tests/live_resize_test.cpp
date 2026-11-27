// live_resize_test.cpp - LR1 (LR97): following a motion-vector / depth size or
// format change while the game runs. g++ -std=c++17 -I src tests/live_resize_test.cpp
#include <cstdio>
#include "live_resize.hpp"

using namespace mgpu::live_resize;
static int g_n = 0, g_fail = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static shape S(unsigned w, unsigned h, unsigned f) { shape s; s.w = w; s.h = h; s.fmt = f; return s; }

int main()
{
    // ---- the picture's shape: Cyberpunk's DLSS render sizes at 2560x1440 pass
    CHECK(same_aspect(1280, 720, 2560, 1440));    // Performance
    CHECK(same_aspect(853, 480, 2560, 1440));     // Ultra Performance (rounded)
    CHECK(same_aspect(1708, 960, 2560, 1440));    // Quality (RE Requiem's vectors)
    CHECK(same_aspect(1708, 961, 2560, 1440));    // Dawnwalker's subrect
    CHECK(same_aspect(1485, 835, 2560, 1440));    // the 58% extent in the R30 notes
    CHECK(same_aspect(2560, 1440, 2560, 1440));   // DLAA
    CHECK(same_aspect(2293, 960, 3440, 1440));    // 21:9
    CHECK(same_aspect(1712, 968, 2560, 1440));    // 8-pixel padded
    // ...and what is not a render size stays out
    CHECK(!same_aspect(2048, 2048, 2560, 1440));  // a shadow map
    CHECK(!same_aspect(1024, 512, 2560, 1440));   // 2:1
    CHECK(!same_aspect(1280, 960, 2560, 1440));   // 4:3
    CHECK(!same_aspect(1920, 1080, 3440, 1440));  // 16:9 on a 21:9 picture
    CHECK(!same_aspect(0, 480, 2560, 1440));
    CHECK(!same_aspect(853, 480, 0, 1440));

    // ---- room: the larger per axis
    {
        unsigned rw = 0, rh = 0;
        room_dims(1280, 720, 2560, 1440, rw, rh); CHECK(rw == 2560 && rh == 1440);
        room_dims(2560, 1440, 2560, 1440, rw, rh); CHECK(rw == 2560 && rh == 1440);
        room_dims(2600, 1460, 2560, 1440, rw, rh); CHECK(rw == 2600 && rh == 1460);   // padded beyond the picture
        room_dims(3000, 1000, 2560, 1440, rw, rh); CHECK(rw == 3000 && rh == 1440);
    }

    // ---- following: the same shape on NEED_FRAMES distinct frames
    {
        follower f;
        const shape n = S(853, 480, 10);
        unsigned k = 0;
        for (unsigned long long fr = 100; fr < 100 + NEED_FRAMES - 1; ++fr) k = refused(f, n, fr);
        CHECK(k == NEED_FRAMES - 1);
        CHECK(decide(k, true, 100, 200) == step::wait);
        k = refused(f, n, 100 + NEED_FRAMES - 1);
        CHECK(k == NEED_FRAMES);
        CHECK(decide(k, true, 100, 200) == step::post);
    }
    // the engine binding the buffer twice in a frame counts once
    {
        follower f;
        const shape n = S(853, 480, 10);
        unsigned k = 0;
        for (unsigned long long fr = 1; fr <= 10; ++fr) { k = refused(f, n, fr); k = refused(f, n, fr); }
        CHECK(k == 10);
    }
    // a frame of the lane's own shape between: start over (an engine with two buffers)
    {
        follower f;
        const shape n = S(853, 480, 10);
        for (unsigned long long fr = 1; fr <= 15; ++fr) (void)refused(f, n, fr);
        accepted(f);
        unsigned k = 0;
        for (unsigned long long fr = 16; fr <= 30; ++fr) k = refused(f, n, fr);
        CHECK(k == 15);
        CHECK(decide(k, true, 1, 2) == step::wait);
    }
    // a different new shape between: start over on it (the menu flicked through two sizes)
    {
        follower f;
        for (unsigned long long fr = 1; fr <= 15; ++fr) (void)refused(f, S(853, 480, 10), fr);
        unsigned k = refused(f, S(853, 480, 34), 16);   // format changed: a new shape
        CHECK(k == 1);
        for (unsigned long long fr = 17; fr <= 16 + NEED_FRAMES - 1; ++fr) k = refused(f, S(853, 480, 34), fr);
        CHECK(k == NEED_FRAMES);
    }
    // a format-only change at the same size is a change (the DLSS model switch)
    {
        follower f;
        unsigned k = 0;
        for (unsigned long long fr = 1; fr <= NEED_FRAMES; ++fr) k = refused(f, S(1280, 720, 34), fr);
        CHECK(decide(k, same_aspect(1280, 720, 2560, 1440), 1280ull * 720 * 4, 2560ull * 1440 * 8) == step::post);
    }
    // the decision's refusals
    CHECK(decide(NEED_FRAMES, false, 10, 20) == step::refuse_shape);
    CHECK(decide(NEED_FRAMES, true, 21, 20) == step::refuse_big);
    CHECK(decide(NEED_FRAMES, true, 20, 20) == step::post);          // exactly the room
    CHECK(decide(NEED_FRAMES, true, 0, 20) == step::refuse_big);     // no footprint: never posted
    CHECK(decide(NEED_FRAMES + 50, true, 5, 20) == step::post);
    // ResizeRoom=0 (room = the arm's bytes): smaller follows, larger is refused
    {
        const unsigned long long arm = 1280ull * 720 * 8;   // RGBA16F at 720p
        CHECK(decide(NEED_FRAMES, true, 853ull * 480 * 8, arm) == step::post);
        CHECK(decide(NEED_FRAMES, true, 1280ull * 720 * 4, arm) == step::post);    // RG16F, same size
        CHECK(decide(NEED_FRAMES, true, 1708ull * 960 * 8, arm) == step::refuse_big);
    }
    // ---- LR98: formats. The Cyberpunk failure: depth lane R32_FLOAT (41) handed the
    // game's depth-stencil R32G8X24_TYPELESS (19), whose plane-0 footprint is R32_TYPELESS (39).
    CHECK(!format_ok(true, 41, 19, 39));               // the device-removal case: refused
    CHECK(!format_ok(true, 41, 19, 19));               // even if the footprint agreed: depth keeps its format
    CHECK(format_ok(true, 41, 41, 41));                // depth size-only change: followed
    CHECK(!format_ok(true, 41, 40, 40));               // D32_FLOAT is still another resource
    CHECK(format_ok(false, FMT_R16G16B16A16_FLOAT, FMT_R16G16_FLOAT, FMT_R16G16_FLOAT));   // the DLSS model switch
    CHECK(format_ok(false, FMT_R16G16_FLOAT, FMT_R16G16B16A16_FLOAT, FMT_R16G16B16A16_FLOAT));
    CHECK(format_ok(false, FMT_R16G16_FLOAT, FMT_R32G32_FLOAT, FMT_R32G32_FLOAT));
    CHECK(!format_ok(false, FMT_R16G16_FLOAT, 24, 24));            // R10G10B10A2: a colour target, not vectors
    CHECK(!format_ok(false, FMT_R16G16_FLOAT, 28, 28));            // RGBA8
    CHECK(format_ok(false, 33, 33, 33));                           // R16G16_TYPELESS lane, size-only: fine
    CHECK(!format_ok(false, 33, FMT_R16G16_FLOAT, FMT_R16G16_FLOAT));   // from a typeless lane: not followed
    CHECK(!format_ok(false, FMT_R16G16_FLOAT, FMT_R16G16_FLOAT, 0));    // footprint disagrees: never
    CHECK(decide(NEED_FRAMES, true, 10, 20, false) == step::refuse_format);
    CHECK(decide(NEED_FRAMES - 1, true, 10, 20, false) == step::wait);  // still only after the frames
    CHECK(decide(NEED_FRAMES, false, 10, 20, false) == step::refuse_format);   // format first
    CHECK(decide(NEED_FRAMES, true, 10, 20, true) == step::post);

    // held shapes
    {
        follower f;
        CHECK(!is_held(f, S(853, 480, 10)));
        hold(f, S(853, 480, 10), held_too_big);
        CHECK(is_held(f, S(853, 480, 10)));
        CHECK(!is_held(f, S(853, 480, 34)));
        CHECK(!is_held(f, S(854, 480, 10)));
        hold(f, S(1, 1, 1), held_none);
        CHECK(!is_held(f, S(1, 1, 1)));
    }

    // ---- the staging ranges: what GPU 1 reads, nothing else
    {
        // A 1440p RGBA16F colour slot with room: seal+colour, depth 720p of a 1440p room,
        // vectors 720p of a 1440p x 8 room, no HUD-less region.
        const unsigned long long SEAL = 512, pay = 2560ull * 1440 * 8;
        const unsigned long long head = SEAL + pay;
        const unsigned long long d_off = ((head + 511) / 512) * 512, d_room = 2560ull * 1440 * 4, d_len = 1280ull * 720 * 4;
        const unsigned long long m_off = ((d_off + d_room + 511) / 512) * 512, m_len = 1280ull * 720 * 8;
        range r[4];
        unsigned n = slot_ranges(head, d_off, d_len, m_off, m_len, 0, 0, r);
        CHECK(n == 2 || n == 3);   // head and depth touch when head is 512-aligned
        unsigned long long tot = 0;
        for (unsigned i = 0; i < n; ++i) tot += r[i].len;
        CHECK(tot == head + d_len + m_len);
        CHECK(r[0].off == 0);
        CHECK(r[n - 1].off == m_off && r[n - 1].len == m_len);
        for (unsigned i = 1; i < n; ++i) CHECK(r[i].off >= r[i - 1].off + r[i - 1].len);   // no overlap
    }
    {
        // touching regions merge; a gap keeps them apart
        range r[4];
        unsigned n = slot_ranges(1024, 1024, 512, 2048, 256, 4096, 1024, r);
        CHECK(n == 3);
        CHECK(r[0].off == 0 && r[0].len == 1536);
        CHECK(r[1].off == 2048 && r[1].len == 256);
        CHECK(r[2].off == 4096 && r[2].len == 1024);
        n = slot_ranges(1024, 0, 0, 0, 0, 0, 0, r);   // colour only
        CHECK(n == 1 && r[0].off == 0 && r[0].len == 1024);
        n = slot_ranges(1024, 1024, 1024, 2048, 1024, 3072, 1024, r);   // all packed: one copy, like IN96
        CHECK(n == 1 && r[0].len == 4096);
        n = slot_ranges(1024, 2048, 512, 1536, 256, 0, 0, r);   // out of order in, sorted out
        CHECK(n == 3 && r[1].off == 1536 && r[2].off == 2048);
    }

    // ---- the seal checks skip frames built for the old geometry
    CHECK(sealed_before_switch(99, 100));
    CHECK(!sealed_before_switch(100, 100));
    CHECK(!sealed_before_switch(5, 0));   // never switched

    std::printf("live_resize_test: %d/%d passed\n", g_n - g_fail, g_n);
    return g_fail == 0 ? 0 : 1;
}

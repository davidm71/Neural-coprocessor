// MGPU Bridge - the private D3D12 device on the selected adapter (T3;
// the device-removal poll accessor arrives with T4)
//
// Bridge thread only: every GPU 1 object is created, used and destroyed
// on the bridge thread. T3 does exactly one thing - create the device
// against the T2 selection, re-verify the binding LUID, log. The window
// and swapchain arrive with T4/T5.
#pragma once

#include <windows.h>

#include "adapter.hpp"

namespace mgpu::gpu1
{
    // Bridge thread only. Returns false when no device was created
    // (invalid selection, creation failure, or LUID mismatch). Every
    // failure is already logged with its inputs; there is never a
    // fallback to the game's adapter.
    bool create_device(const adapter::selection_result &sel);

    // Bridge thread only. Releases the device if one exists.
    //
    // T5 (extension, not replacement): first releases the present chain
    // if one exists - waiting for the GPU to idle (one final fence signal
    // + wait), then releasing the chain objects in reverse creation
    // order - and only then the device, exactly as before. The chain goes
    // first because the swapchain holds a reference to the window it was
    // created against and must not outlive DestroyWindow (worker.cpp
    // reorders its teardown accordingly). Still a no-op for the chain
    // when none was created (the no-window path is unaffected).
    // V32. Release the NGX features and everything under them. MUST be called
    // before shutdown(), from the bridge thread's ordered teardown.
    //
    // Nothing did this before V32. stream_release() only ran on a failed arm
    // or on a stream that reached its frame bound, so an ordinary game exit
    // abandoned two NR feature handles, the SR feature handle, both parameter
    // blocks and the driver snippet DLL - and then shutdown() released the
    // device they were created against. Safe to call when nothing was ever
    // armed; it returns silently.
    // EXPERIMENTAL, default off. NoActivate=1 creates the bridge window
    // WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW so a click on it cannot take
    // foreground from the game - which is what stops input, pad included.
    bool window_no_activate();

    // ---- V49 - V53: the ghost mode. All four are inert when DcompOverlay=0.
    // mgpu.ini DcompOverlay=1. Read at present-chain creation and at window
    // creation, nowhere else.
    bool dcomp_overlay_mode();
    bool dcomp_overlay_mode_settled(unsigned timeout_ms);   // R273b: bridge thread - wait for it, bounded, then decide
    // R242: DcompForceWindowed (absent = 1). See dllmain's create_swapchain / set_fullscreen_state.
    bool dcomp_force_windowed();
    // V55. Pushed from worker.cpp at T4: does the BRIDGE adapter drive any
    // display? Also starts the hint clock.
    void note_bridge_headless(bool headless);
    // V55. One active path, bridge adapter headless, and the mode NOT already
    // on - i.e. exactly the people who should be told about it. Advice only.
    bool single_display_hint();
    // V55. True while AutoArm should wait for the hint to be read. Ten
    // seconds, wall clock, not frames.
    bool autoarm_hint_holding();
    // The GAME's HWND, pushed from dllmain once the swapchain is confirmed to
    // be the game's by LUID. Only called when dcomp_overlay_mode() is true.
    void set_game_hwnd(void *hwnd);
    // V53. Roots the composition visual on the first frame carrying neural
    // output, so the idle screen never blacks out the game. Bridge thread.
    void dcomp_root_on_first_neural_frame();
    // V52. CTRL+ALT+F6. Unroots the visual so the GAME's own ReShade overlay -
    // which is behind it by construction - can be reached, and roots it again.
    // Returns whether the bridge is on screen AFTER the call. Bridge thread.
    // V65. Root or unroot the composition visual explicitly. Called when the
    // game's ReShade overlay opens and closes, so the bridge steps aside for
    // an overlay that IS interactive instead of covering it with one that
    // cannot be. Bridge thread, or any thread the overlay callback runs on.
    void dcomp_set_visible(bool on);

    // ---- R158: THE ONE CONFIGURATION THE GLOBAL OVERLAY KEY APPLIES TO ----
    //
    // True only when mgpu.ini says DcompOverlay 0 EXPLICITLY and there is one
    // active display path. Both halves are deliberate:
    //
    //   EXPLICITLY 0, not "resolved to off". An absent key and an `auto` that
    //   resolved off both leave the bridge as a window too, but the reported
    //   defect was measured with the key written as 0 and that is the blast
    //   radius asked for. A behaviour that fires on configurations nobody
    //   tested is how R156 got shipped and reverted.
    //
    //   ONE display. With a second panel the bridge overlay is on its own
    //   screen, does not sit over the game, and mirroring the key would open a
    //   panel the person cannot see.
    //
    // Latched on first call, like dcomp_overlay_mode() and for the same
    // reason: it is read from an input event and must give one answer per run.
    bool dcomp_explicit_off_single_display();
    bool dcomp_peek_toggle();

    // Final-output FPS HUD. CTRL+ALT+H cycles BASIC -> DETAIL -> OFF.
    int fps_hud_toggle();
    int fps_hud_mode();

    // REV: lock_timeout_ms > 0 (DllMain): give up instead of blocking when the
    // bridge thread holds the stream lock - it may be inside an NGX call that
    // needs the loader lock this caller holds.
    void stream_shutdown(unsigned lock_timeout_ms = 0);

    void shutdown();

    bool has_device();

    // T4 (brief section 00, exception 4): poll the device's removal reason
    // without exposing the ID3D12Device. The raw pointer never leaves this
    // translation unit - handing it out would put it outside the mutex that
    // guards it, and shutdown() could release it between the caller's read
    // and its use. Returns false when no device exists (nothing to poll;
    // `out` is set to S_OK). Returns true and sets `out` to
    // ID3D12Device::GetDeviceRemovedReason() (S_OK when healthy, otherwise
    // the removal reason) when a device exists. Takes this file's lock, so
    // the call is made with the device pointer still guarded. The value is
    // sticky once removed (brief section 09), so the caller logs only on the
    // transition away from S_OK.
    bool device_removed_reason(HRESULT &out);

    // ---- T5: the present chain (brief section 06) ----
    //
    // All D3D12/DXGI objects live behind this door: the raw pointers never
    // leave this translation unit - the same guarantee as the device, which
    // is why they sit behind the same mutex. T6 will need the native device
    // / queue / swapchain pointers for create_effect_runtime; the accessor
    // for those is written with T6, not now.

    // Bridge thread only. Creates, on the T3 device: the command queue
    // (D3D12_COMMAND_LIST_TYPE_DIRECT), the DXGI swapchain against
    // `hwnd` (FLIP_DISCARD, R8G8B8A8_UNORM, 2 buffers, windowed), the
    // RTV heap, the command allocator, the command list, and the fence
    // + event. Queries the window's client rect itself (the T5 window is
    // non-resizable, so the size is fixed for the chain's lifetime). The
    // swapchain lands on the queue's adapter - the T3 device's adapter -
    // which is the entire mechanism that makes this a GPU 1 swapchain;
    // there is no adapter parameter to get wrong. Returns false with
    // everything released on any failure; every failure is logged with
    // the failing call and its HRESULT.
    bool create_present_chain(HWND hwnd);

    // Bridge thread only. One frame: PRESENT -> RENDER_TARGET barrier,
    // ClearRenderTargetView on the current backbuffer's RTV, the barrier
    // back to PRESENT, Close, ExecuteCommandLists, Present(1, 0) (vsync -
    // the loop's pacing), Signal, and a wait on the fence before
    // returning (the command allocator is single, so the next frame may
    // not Reset it until the GPU has finished this one). Returns false on
    // the first failure of any step; that failure is logged once (the
    // step, its HRESULT, and the removal reason) and later failures are
    // silent - the caller stops presenting after the first.
    bool present_frame(float r, float g, float b);

    // Any thread (takes this file's lock). True when the present chain
    // exists.
    bool has_present_chain();

    // ---- P1.0: the NGX probe ----
    //
    // Bridge thread only. Called exactly once, after create_present_chain()
    // succeeds and before the present loop starts - nothing is presenting
    // yet, so CreateFeature's ~1.16 s (measured on the reference run) stalls
    // nothing.
    //
    // Answers one question: does NGX initialise and create a feature on a
    // headless, non-game adapter? Everything downstream in this milestone
    // assumes it does, and nothing had tested it.
    //
    // Resolves the entry points by hand - no static library, no new link
    // library - from the driver's own _nvngx.dll (NGX Core) and, for names
    // the core does not export, from nvngx_dlssnr.dll (the DLSS-NR feature
    // snippet, which sits beside dxgi.dll). It then initialises NGX against
    // the GPU 1 device, takes the capability parameter map, creates
    // NVSDK_NGX_Feature_Reserved18 at the given size on a private command
    // list, and tears all of it down again. The device pointer stays inside
    // gpu1_context.cpp exactly as it does everywhere else - there is no
    // accessor, and the probe runs where the pointer already is.
    //
    // Every NGX call's numeric result is logged before the next is
    // attempted; every failure path releases what it created and returns
    // false. A false return never stops the bridge: the window, the present
    // loop and the teardown behave exactly as P0 shipped them. This is a
    // probe, not a dependency.
    // P3.0: an externally supplied colour frame for ngx_probe.
    //
    // The pixels are CPU-side, tightly packed at `row_pitch` bytes per row, and
    // must already be in the format ngx_probe builds its colour texture with
    // (R8G8B8A8_UNORM today). `dxgi_format` is the ORIGINAL format the frame was
    // captured in and is carried for the log only - it is what says whether a
    // conversion happened on the way here, which is a real per-frame cost in any
    // production version of this path and must not become invisible.
    struct ngx_input_frame
    {
        const unsigned char *pixels = nullptr;
        UINT row_pitch = 0;
        unsigned dxgi_format = 0;

        // P3.1. false: `pixels` have already been converted to R8G8B8A8 and
        // ngx_probe builds its textures in that format - the P3.0 behaviour.
        // true: `pixels` are the frame's ORIGINAL bytes and ngx_probe builds
        // its colour and output textures in `dxgi_format` instead, asking the
        // question P3.0 deliberately left open - can DLSS-NR consume the
        // game's buffer as it is rendered, with no conversion stage at all?
        //
        // A yes deletes a full-resolution CPU pass from every frame of any
        // production version of this path. A no is worth having in writing
        // too, because it makes the conversion a permanent structural cost to
        // be budgeted on the GPU rather than wished away.
        bool native_format = false;
    };

    // ext == nullptr is the P1 behaviour, unchanged: NR runs on a generated
    // pattern. ext != nullptr is P3.0: NR runs on the frame supplied, which is
    // the game's own, and the P1.4 transit block is skipped because its control
    // belongs to the synthetic path.
    bool ngx_probe(UINT width, UINT height, const ngx_input_frame *ext = nullptr);

// P1.3: does a buffer cross between the two adapters intact? Creates its
// OWN device on the game's adapter - the game's device is never touched -
// tries a shared cross-adapter buffer first and a host-pinned heap second,
// and verifies the payload byte for byte. Bridge thread only; safe to
// ignore the return value, the probe logs its own verdict.
// P1.3g: `tag` names the run in every log line it produces. "startup" is the
// automatic run that fires while shaders are still compiling; a manual run
// triggered by the hotkey passes its own label. The same launch can therefore
// contain a contaminated sample and a settled one, and the DIFFERENCE between
// them is the measurement of the contamination itself.
bool transit_probe(const char *tag = "startup");

// ---- P1.5: the host's real frame ----
//
// Everything before this transported a pattern we generated. P1.5 transports
// the game's finished colour buffer: 2560x1440 R10G10B10A2_UNORM on this rig,
// a resource we do not own, in a state we did not set.
//
// Three calls, in this order, and each is a no-op until the one before it has
// succeeded:
//
//   capture_on_finish_effects  the ReShade event. Called on the GAME's thread
//                              with the GAME's command list. Filters by adapter
//                              LUID - the bridge's own runtime raises this
//                              event too, and acting on it would capture our
//                              own window. First call allocates and arms;
//                              the second records the copies. One shot.
//   capture_poll               bridge thread, once per present. Does nothing
//                              until the capture has been recorded and the
//                              handoff is known to have completed, then reads,
//                              compares and reports.
//
// P2.0 CLOSES THE SYNCHRONISATION GAP. P1.5 inferred "the copy has completed"
// from frames elapsed, because we do not own the game's queue. P2.0 creates a
// fence with SHARED | SHARED_CROSS_ADAPTER on the game's device, opens the same
// fence on the bridge's device, and signals it on the game's own queue on the
// frame AFTER the copies were recorded - queue order then guarantees the signal
// lands behind them. capture_poll waits on that fence instead of counting.
// The frame counter is kept as a labelled fallback for the case where the
// shared fence cannot be created, and the sentinel fill stays in either mode:
// it is what turns "read too early" into a named diagnosis rather than a
// plausible wrong answer.
// Bridge thread. Until this is called the capture path is inert: the event
// handler returns immediately and the game's command list is never touched.
// Without it P1.5 would fire on the first two frames of the process and
// capture a loading screen, spending its one shot on a black frame.
void capture_request();
// P2.0 adds cmd_queue: the game's immediate command queue, as a native
// ID3D12CommandQueue*. It is the one object we need that the P1.5 signature
// did not carry - without it the shared fence can be created and opened but
// never signalled, and the wait would hang instead of measuring. It is passed
// as void* for the same reason as the others: this header names no ReShade and
// no D3D12 types.
void capture_on_finish_effects(void *runtime, void *cmd_list, void *cmd_queue,
                               unsigned long long rtv_handle);
void capture_poll();

// ---- P4.0: the stream ----
//
// The first stage that RUNS rather than probes: every game frame, sealed and
// transited into a ring of slots, until a self-imposed bound.
//
// A stream is where the QUIET failures in P1_INSTRUMENT section 00 live - torn,
// stale, dropped, duplicated, reordered, slot-aliased. None of them is
// reachable by a one-shot probe and none is visible to a person watching the
// window; a stream that consistently delivers frame N-4 looks perfect on static
// content. The 64-byte seal carried in each slot is what makes them nameable,
// and section 06 committed to shipping it with the first task that transits a
// stream.
//
// Scope, so the log is not over-read: the seal proves IDENTITY, ORDER and AGE.
// It does NOT verify pixels per frame (P1.5 established the payload crosses
// byte-exact, and re-proving it per frame would measure the instrument), and
// the barcode field is written as 0 and left UNCHECKED because the shader that
// would make it an independent check does not exist yet. The neural stage is
// deliberately not attached: if both landed in one commit, a failure would not
// say which half.

// P5.2. Any thread. True only when mgpu.ini says Probes=1.
//
// DEFECT C: the one-shot probe chain (P1.3 transit, P1.5 capture and the P3.x
// ngx_probe it leads into) and the P4.1 stream both armed from the same hotkey,
// which put TWO independent NGX consumers on ONE shared parameter block -
// GetCapabilityParameters returns the core's block, not a per-caller one. The
// probe's teardown then destroyed it under the running stream. The visible
// symptom was a neural image with the colours wrong while every transport
// counter stayed clean, which is precisely the failure shape the seal cannot
// see: the bytes arrived, the consumer was broken.
//
// The destroy is now suppressed while the stream holds the block, and the
// probes themselves are opt-in and default OFF - they are answered questions,
// and re-running them under a live stream can only cost. Set Probes=1 in
// mgpu.ini to run the old chain again, with the stream deliberately not armed.
bool probes_enabled();

// ---- P7.10: two settings worker.cpp needs, read through this file's reader ----
//
// Both come out of the same mgpu.ini the P7.2 line names and the P4.0 arm line
// reports. They live here rather than in worker.cpp because the ini reader and
// its buffer are in this file's anonymous namespace, and a second parser in a
// second file is how two files start disagreeing about the settings.

// Monitor= auto | <index>. -1 = auto, meaning the bridge adapter's first
// output attached to the desktop. An index selects among THAT ADAPTER's
// outputs - not Windows' display numbering - because the point of the setting
// is to keep scan-out on the card that did the neural work.
int monitor_index();

// AutoArm= 0 | 1 | <frames>. 0 = off (the default, and what every published
// measurement ran under). 1 = on at the built-in delay; a larger value is that
// delay in presented bridge frames. The delay is load-bearing: the stream is
// armed once against the game's swapchain as it exists at that moment, so
// arming while the game is still building it - or while the user is still in
// the graphics menu - leaves the consumer bound to an arrangement that is about
// to be replaced.
unsigned autoarm_frames();

// R246. THE STARTUP NGX PROBE IS DEFERRED UNTIL THE GAME HAS RENDERED.
//
// Skyrim SE (SK-6..SK-14): the add-on's first NGX Init - P1.0c, on the bridge
// thread ~1 s into the process, right after the present chain and before the
// present loop - returns FAIL_OutOfDate on some launches and Success on others
// with every input identical (R245 trace: same nvapi sequence, same DRS
// answers, same statuses up to the fork). Every launch that passed had the
// game further along at that moment (Options first, launcher from the folder,
// alt-tab at launch); every launch that failed had the game still in its own
// startup. A failed first Init decides for the process: P4.1 at arm, and a
// manual arm minutes later (AutoArm=0), fail the same way. Requiem shows the
// same shape on the same module. So the first Init must not happen in that
// window. The probe now runs from the present loop once the game has delivered
// its first finish_effects frame - the game's own progress, not a constant -
// bounded by NGX_PROBE_DEFER_MAX_MS so a title that never reaches that event
// still gets its probe. NgxProbeAtStart=1 in mgpu.ini restores the old place.
unsigned long long game_effects_frames();   // finish_effects events seen, armed or not
void set_mvec_eligible_count(unsigned n);   // R278: probe's eligible velocity candidates, via dllmain
void set_mvec_extract_active(bool on);     // R280c: mvec_extract's virtual target exists (units uv by construction; no verdict keys on that path)
int  mvec_extract_edge_request();         // R280e: which write edge of the source the module should hold (0 = last of the frame, 1 = first) - the judge decides
void set_mvec_extract_edges(unsigned n);  // R280e: the module reports the last frame's write-edge count
void set_mvec_extract_deciding(bool on); // R280j: armed, no source yet, candidates being written - the arm's hold does not count these frames (bounded by the module's give-up)
// R280k: a contract title = the game's NGX Init was seen OR the calibrator holds the game's
// own NGX table (DW-R1: Dawnwalker's Init never reaches the hook, its table does). Lock-free.
bool game_contract_seen();
// R280o. dllmain brackets the R277 tap with this (game render thread): a
// motion-vector copy made inside the bracket, on that thread, is a TAP copy and
// is counted apart, so R118's "the barrier route produced zero copies" is not
// defeated by the tap's own copies (SF-R1). Thread-local: barrier-route copies
// on the engine's recording threads and evaluate-route copies are never tagged.
void tap_copy_scope(bool on);
int mvec_tap_live();   // R278b: tap mode the first-launch learning turned on (0 = none)
void set_mvec_neighbours(int cur, int next_same, int next_class);   // R278f: from probe, via dllmain
int mvec_candidate_request();   // R278f: the learning's live switch (-1 = none)
bool stream_producing();   // R278f-r: the stream has sealed a frame (lock-free)
bool ngx_probe_at_start();                  // NgxProbeAtStart=1 (default 0: deferred)
void note_game_d3d12();                     // R282: dllmain, the LUID-proved D3D12 game chain
bool ngx_probe_scene_hold_decide(char *why, size_t n);   // R282: once; true = hold the probe for the game's scene
bool ngx_probe_scene_seen();                // R282: the game's first DLSS evaluate has been read (lock-free)
void depth_fallback_request();              // R290: the depth hold reached DepthHoldMax with evidence (game thread)
bool depth_fallback_requested();            // R290: read by the worker's probe block
void note_ngx_probe_ran();                  // R290: the startup or a manual probe ran
bool ngx_probe_ran();                       // R290: the fallback arm follows the probe
unsigned long long own_route_copies();      // R289: transport copies not made by the R277 tap, counted directly (lock-free)
int  own_route_delivered();                 // R289: 0 none, 1 evaluate route (R280o), 2 the game's own route on a D3D12 contract title

// R247. RELOAD-AND-RETRY AFTER A REFUSED FIRST INIT, ON A NO-CONTRACT TITLE.
// A first Init that returns FAIL_OutOfDate decides for the process: a second
// Init on another device (P4.1), or minutes later by hand, answers the same
// (SK-6..SK-15). On a title with no NGX contract the core is ours (R240 loaded
// it) and the snippet is the private instance, so the refused state can be
// dropped with them: FreeLibrary both, let the locator load them again, Init
// again. worker.cpp drives it: up to NGX_RETRY_MAX tries, spaced by the game's
// own frames. ngx_reload_for_retry reports what it freed and whether the
// modules actually left the process. A core the game owns is never freed.
bool ngx_probe_init_was_outofdate();
bool ngx_reload_for_retry(char *why, size_t why_n);

// R248. How many OTHER processes with this executable's name are alive (a
// previous session still tearing down), and the first one's pid. Marcelo's
// reliable repro for the refused Init ends "close, then Steam Play": the new
// process makes its first NGX Init while the old one is still exiting. The
// worker logs this at every Init and, when one is present, waits (bounded)
// for it to leave before the Init.
unsigned other_instances_of_this_exe(unsigned long *first_pid);

// ---- P6.4: what the overlay panel reads and writes ----
//
// Plain scalars on purpose. This header names no ReShade type and no ImGui
// type, and the panel that drives these lives in dllmain.cpp where those
// headers already are - the same separation that has kept gpu1_context free of
// ReShade since T3.
//
// ANY THREAD: the overlay callback runs on whichever thread presents the
// runtime it belongs to, not the bridge thread, so these take the stream's lock
// internally. They are tiny and never block - a UI callback that can stall is a
// UI callback that can stall a present.
struct ui_state
{
    bool armed = false, summarised = false, neural = false, nr_ok = false;
    bool profile = false;
    // P7.4: 0 = neural output, 1 = the frame handed TO the model, 2 = split
    // (left half input, right half output, the same frame).
    int present_mode = 0;
    // P7.4: 0 = manual, 1 = front-loaded, 2 = back-loaded. See ui_set_preset.
    int preset = 0;
    // P7.5: split seam position, 0.0 (all output) .. 1.0 (all input).
    float split_pos = 0.5f;
    // P7.7: 2, down from 6. gpu1_context.cpp owns the real bound (MAX_PASSES)
    // and the reasoning; this is the copy the panel reads. A static_assert in
    // ui_read() fails the build if the two ever disagree, because this header
    // cannot see the constant and a silently smaller array would truncate the
    // copy rather than error.
    unsigned passes = 1, max_passes = 2;
    float intensity[2] = {};
    // P7.9: the guide's tuning parameters. tuning_on false = none of them are
    // set, which is what every published measurement ran under.
    bool  tuning_on = false, auto_mask = false;
    float tone_strength = 1.0f, structure_strength = 1.0f, skin_strength = 1.0f,
          style = 0.0f;
    // P8.0: 0 = off (no DLSSNR.MVec bound - the control arm and what every
    // published measurement ran under), 1 = a synthetic constant field.
    int   mvec_mode = 0;
    float mvec_dx = 0.0f, mvec_dy = 0.0f;
    float mvec_scale_x = 1.0f, mvec_scale_y = 1.0f;
    unsigned long long consumed = 0, produced = 0, dropped = 0, overrun = 0, skipped = 0;
    unsigned long long reordered = 0, bad_magic = 0, contract = 0;

    // ---- LIVE TELEMETRY (the panel's second box) ----
    //
    // Every figure here is the SAME NUMBER the end-of-run log line prints, over
    // the same divisor. That is deliberate and it is the only rule this block
    // has: a panel that computes a mean its own way is a second instrument
    // quietly disagreeing with the first.
    //
    // Rates are rolling over the panel's own draw cadence (about 0.25 s), not
    // averages since arm - an average since arm stops responding after a minute
    // and is useless for watching a change land.
    double fps_produced = 0.0;   // the GAME's frame rate, from the seal counter
    double fps_consumed = 0.0;   // what GPU 1 is actually finishing

    // GPU 1's own timestamps on GPU 1's own queue. gpu1_eval_ms SPANS EVERY
    // PASS and, when SR is up, it also contains the reduce and the upscale -
    // it is not the neural cost on its own.
    bool   gpu1_ts_ok = false;
    double gpu1_copy_ms = 0.0, gpu1_unpack_ms = 0.0,
           gpu1_eval_ms = 0.0, gpu1_out_ms = 0.0;

    // lat_ready_ms is L2: from the first poll that could have known the frame
    // existed, to the seal read. lat_submit_ms is the SEAL figure, which starts
    // a frame earlier at copy-record time. Neither includes the game's render
    // before them or the bridge's present after them, so NEITHER IS THE LATENCY
    // A PLAYER FEELS end to end - they bound the bridge's own share of it.
    double lat_ready_ms = 0.0, lat_submit_ms = 0.0;

    // L1. GPU 0's queue depth at copy-record time. 1.0 is the STRUCTURAL FLOOR,
    // not an error: the signal for frame N is issued on frame N+1's event.
    // Around 1.0 means a working low-latency mode; 2.5+ means the game is
    // queueing deep and that is most of the latency.
    double             backlog_mean = 0.0;
    unsigned long long backlog_max = 0;

    unsigned           ring_depth = 0;    // slots allocated
    unsigned           ring_window = 0;   // 0 = the window is off (the default)
    unsigned long long ring_skipped = 0;  // skipped BY the window - NOT drops

    // ---- DLSS SUPER RESOLUTION, the third box ----
    //
    // Read-only on purpose. Quality, preset and R are all baked into an NGX
    // feature handle at arm time; changing one means releasing and rebuilding
    // that handle mid-stream, which is a 200-450 ms stall on the bridge thread
    // and a teardown path that has crashed before. Showing the state costs
    // nothing and is most of the value; editing it is its own change.
    bool sr_on = false;         // a handle exists and SR is in the chain
    bool sr_requested = false;  // SRUpscale=1 but it may have declined to arm
    unsigned sr_w = 0, sr_h = 0;        // R, what NR and SR actually run at
    unsigned out_w = 0, out_h = 0;      // D, the display extent
    int      sr_quality = 1;
    int      sr_preset = 0;             // 0 = the title's default
    unsigned sr_scale_pct = 0;          // 0 = R inherited from the game
    unsigned sr_mv_mode = 0;            // 0 off, 1 on+corrected, 2 derive
    bool     sr_mv_lowres = false;      // the flag as actually passed
    float    sr_mv_fix_x = 1.0f, sr_mv_fix_y = 1.0f;
    // V27. A rebuild staged by the panel and committed by stream_poll.
    // sr_rebuild_last_ok: -1 none yet, 0 the last one failed, 1 it worked.
    // ---- V28: the inner loop ("Auto") ----
    // auto_last_mean is the measured evaluate mean over the last window, and
    // auto_budget_ms is 1000/target. The controller compares those two and
    // nothing else - there is no cost model in it, deliberately (ledger 6g).
    bool     auto_on = false;
    unsigned auto_target_fps = 60;
    unsigned auto_rung = 0, auto_rungs = 0, auto_changes = 0;
    double   auto_last_mean = 0.0, auto_budget_ms = 0.0;

    bool     sr_rebuild_pending = false;
    unsigned sr_rebuild_count = 0;
    int      sr_rebuild_last_ok = -1;
    bool     sr_snippet_requested = false;
    bool     sr_snippet_driver = false; // which one ANSWERED

    // ---- REFLEX ----
    //
    // reflex_now is the driver's REPORTED state and it has been measured
    // disagreeing with reality: a SetSleepMode that returned 0 and visibly
    // collapsed the queue still read back OFF. Show it, label it, and never
    // draw a verdict from it - backlog_mean is the verdict.
    int  reflex_was = -1, reflex_now = -1;   // -1 unknown, 0 off, 1 on
    bool reflex_applied = false;             // SetSleepMode returned 0

    // OPT2: late-latch, live. ll_ready is false when there is no copy queue
    // this arm (CopyQueue=0, or it failed to come up), which disables it.
    bool     ll_on = false, ll_ready = false;
    unsigned ll_age = 2;
    unsigned long long ll_issued = 0, ll_hits = 0, ll_stale = 0;
    float ll_recent_pct = -1.0f;   // last ~1200 frames; -1 not yet known
};
void ui_read(ui_state &out);

// OPT2: late-latch, live from the next frame. Also written to mgpu.ini by the
// panel so the next launch starts the same way. Age is clamped to 0..4.
void ui_set_late_latch(bool on);
// FG1/FG2: frame generation prototype. Mode 0 off, 1 on, 2 motion view.
// FG2: the bridge thread hands each finished NR frame (and its in-between
// frame) to a present thread, which paces them; true means handed over and
// the caller must not present it. `failed` reports a present failure there.
bool fg_handoff(float r, float g, float b, bool &failed);
void fg_present_stop();
double hud_gen_fps();
int  ui_frame_gen();
void ui_set_frame_gen(int mode);   // 0 off, 1 ours, 2 motion view, 3 AMD FSR 3
int  ui_frame_gen_last();          // FSR1: the mode CTRL+ALT+G turns back on
int  ui_fsr_state();               // FSR1: 0 not tried, 1 up, -1 failed
int  ui_dlssg_state();             // DLSSG1: 0 not tried, 1 up, -1 failed
int  ui_fg_mult();                 // DLSSG2: 2..6, what FGMultiplier asks for
int  ui_fg_mult_running();         // MFG6: what NVIDIA FG is producing now, 0 = not generating
bool ui_fg_extras_failed();        // MFG7: the 5x/6x buffers could not be made (out of video memory)
void ui_set_fg_mult(int m);
float ui_color_strength();          // CS1: 0..1, 1 = neural output untouched
void  ui_set_color_strength(float k);
bool ui_status_hud();               // ST1: StatusHud=1 adds the status lines to the FPS HUD
void ui_set_status_hud(bool on);
// CK1: clock and stopwatch box, top-right of the picture. The clock's
// visibility is saved (Clock in mgpu.ini); the stopwatch restarts from 0
// every time it is shown and is not saved.
bool ui_clock();       void ui_set_clock(bool on);
// NM1: NR network: 0 = the DLL's default, 1 = A (original), 2 = B (fast).
// Read when NR is created, so a change applies from the next launch.
int  ui_nr_model();    void ui_set_nr_model(int m);
// FL1: FG Lite (lighter NVIDIA frame generation) and FG DLL source
// (0 = driver model store first, 1 = mgpu\nvngx_dlssg.dll first). Next launch.
int  ui_fg_lite();     void ui_set_fg_lite(int on);
// RS1: reduced-resolution reconstruction (0 DLSS SR, 1 residual) and filters
// (down: 0 area 1 bilinear 2 nearest; up: 0 bilinear 1 bicubic 2 nearest).
// UC1: NR UI correction (0 auto, 1 off, 2 on). All live.
int  ui_rs_recon();    void ui_set_rs_recon(int v);
int  ui_rs_down();     void ui_set_rs_down(int v);
int  ui_rs_up();       void ui_set_rs_up(int v);
int  ui_fov_mode();    void ui_set_fov_mode(int m);       // FV1/FV2: 0 off, 1 centre, 2 lower band
int  ui_fov_band();    void ui_set_fov_band(int v);
int  ui_fov_bpos();    void ui_set_fov_bpos(int v);       // FV3: 0 top, 50 middle, 100 bottom
bool ui_fov_fullw();   void ui_set_fov_fullw(bool on);
int  ui_fov_pct();     void ui_set_fov_pct(int v);
int  ui_fov_feather(); void ui_set_fov_feather(int v);
int  ui_nr_uic();      void ui_set_nr_uic(int v);
// SC1: enlarge a below-monitor game frame with 0 off / 1 bilinear / 2 FSR 1 /
// 3 NIS, sharpness 0-100. active = the window was sized to the monitor for it.
int  ui_scaler();        void ui_set_scaler(int m);
int  ui_scaler_sharp();  void ui_set_scaler_sharp(int pct);
bool ui_scaler_active();
// PP1: PanelOverPicture support (upstream 0.4.0) and the scaled-picture case.
bool dcomp_visual_rooted();
void *dcomp_game_hwnd();
void sc_set_panel_live(bool up);
bool sc_cover_live_rect(RECT *r);   // the 4K picture's screen rect while it is shown
// PP2: the pointer from the cover window's own mouse messages (worker.cpp).
bool sc_panel_live();
void sc_cover_mouse(int x, int y, unsigned btn, int wheel);
bool sc_cover_mouse_get(float *u, float *v, unsigned *btn, int *wheel);   // false: no message yet / panel not up
void ui_driver_notice(char *out, unsigned n);   // DRV1: empty when the driver is fine
unsigned ui_driver_version();                    // DRV1: 61742 = 617.42, 0 unknown
void sc_unzoom_now();   // SC9: undo the console zoom (unload path)
// PF1: detailed log (repeating diagnostic lines) on/off. Live.
bool ui_detailed_log();  void ui_set_detailed_log(bool on);
bool ui_perf_log();      void ui_set_perf_log(bool on);      // PF1: the 10 s performance summary
// DV1: one line naming the NVIDIA DLL versions loaded this session.
void ui_dll_versions(char *out, unsigned n);
int  ui_fg_source();   void ui_set_fg_source(int v);
bool ui_stopwatch();   void ui_set_stopwatch(bool on);
void set_hook_present_active(bool on);   // HP1: dllmain says the present-time feed is running
int  ui_dlssg_max_mult();          // DLSSG2: driver's MultiFrameCountMax+1, 0 unknown yet
bool ui_fg_hudfix();             // FG3
void ui_set_fg_hudfix(bool on);
void ui_set_late_latch_age(unsigned age);

// HARDEN2: release the late-latch timer. BRIDGE THREAD ONLY, after its present
// loop has exited, so nothing can be waiting on the handle.
void late_latch_shutdown();

// Fork panel: PresentVsync live (the present path reads it every present).
bool ui_present_vsync();
void ui_set_present_vsync(bool on);
// Fork panel: DcompOverlay as the FILE has it: -2 absent, -1 auto, 0 off, 1 on.
int  ui_ini_read_dcomp();
// Fork panel: write a text value (DcompOverlay=auto).
bool ui_ini_write_text(const char *key, const char *value);

// Pass count, live. Every NGX feature handle is created at arm time, so this is
// only a count change - nothing is created or destroyed, and it takes effect on
// the next consumed frame. Clamped to 1..max_passes.
void ui_set_passes(unsigned n);

// pass_1based == 0 sets every pass; otherwise that one. Clamped 0.0..2.0.
void ui_set_intensity(unsigned pass_1based, float v);

// P7.9. The DLSS-NR tuning parameters, names and types taken from the feature's
// programming guide rather than guessed:
//
//   DLSSNR.LocalToneStrength       float  - local contrast, reads as AO-like shading
//   DLSSNR.LocalStructureStrength  float  - detail synthesis
//   DLSSNR.SkinStructureStrength   float
//   DLSSNR.Style                   float
//   DLSSNR.UseAutoMask             uint   - 0/1, a different Set overload
//
// Until P7.9 this add-on set only DLSSNR.Intensity and left all of these at the
// feature's defaults, while the reference implementation measured against in
// RESULTS.md sets them. That is a recorded unmatchable difference between the
// two arms and the first thing to reach for when output looks wrong at strength.
//
// DEFAULT OFF, and that matters: with tuning_on false nothing here is set and
// the evaluate path is identical to every published run. Turning it on makes the
// run a tuning run, and the log says so.
void ui_set_tuning(bool on);

// which: 0 tone, 1 structure, 2 skin, 3 style, 4 auto-mask (0.0/1.0).
void ui_set_tuning_value(int which, float v);

// P8.0. The motion vector field handed to DLSS-NR.
//
// The model keeps temporal history and reprojects it with motion vectors.
// Every evaluate this project has run bound none, so that history is
// misaligned on every frame the camera moves and the model re-synthesises
// detail instead of reusing it.
//
// mode 0 = off, byte-identical to every published run. mode 1 binds a CONSTANT
// field: a control in the P4.2 depth sense, two evaluates differing only in
// whether a non-zero MVec is bound. Same output means NR does not read it here;
// different output means it does, and only then is a flow estimator worth
// writing.
void ui_set_mvec_mode(int mode);

// P8.1. Which passes keep DLSS-NR's temporal history.
//   0 all passes keep it (the shipped behaviour)
//   1 passes 2..N reset every frame - the cascade of two misaligned histories
//     is broken and later passes become stateless refinement
//   2 every pass resets every frame - no temporal history anywhere. If the
//     artifact largely goes at 2, it IS the model's own misaligned history.
void ui_set_pass_reset(int mode);

// which: 0 dx, 1 dy, 2 scaleX, 3 scaleY. Sign and units are NVIDIA's
// convention, cannot be read back, and are live for exactly that reason.
void ui_set_mvec_value(int which, float v);

// Turn the neural stage off without tearing it down - the handles stay alive so
// it can come back without a 400 ms CreateFeature stall.
void ui_set_neural(bool on);

// P7.4. The present mode, live: 0 neural output, 1 the frame handed TO the
// model, 2 SPLIT - both halves of the SAME frame side by side, input left,
// output right, with a white seam between them.
//
// Split is the only way to compare input against output in a game: two runs
// never contain the same frame, and an exterior changes underneath you, so a
// difference between two captures can never be attributed cleanly. It changes
// nothing about the neural stage or its timing - only the copy into the
// bridge's backbuffer.
void ui_set_present_mode(int mode);

// V27. LIVE DLSS QUALITY AND PRESET, STAGED. Pass -1 to leave a field alone.
//
// NOT A PLAIN SETTER, AND THE DIFFERENCE MATTERS. Both values are baked into
// the NGX feature handle when it is created, so changing either means
// releasing that handle and building a new one. This records the request; the
// commit happens at the top of the next stream_poll, which is the only moment
// on the bridge thread when GPU 1 has nothing outstanding. Calling it from the
// overlay is therefore safe - the overlay never touches the feature itself.
//
// Costs one rebuild of roughly 30-40 ms, seen as a single long frame. R does
// NOT change with either value, so nothing R-sized is rebuilt: not the
// transport, not the ring, not the neural stage.
//
// quality: the NGX PerfQualityValue - 0 max perf, 1 balanced, 2 max quality,
//          3 ultra perf, 4 ultra quality, 5 DLAA.
// preset:  the raw NGX render-preset enum - 0 leaves the title default,
//          11 is K, 13 is M. A DLL that does not carry the preset asked for
//          silently uses its own; its log says which one it HONOURED, and that
//          log is the only honest answer.
//
// IF THE REBUILD FAILS, SR IS OFF FOR THE REST OF THE RUN and the stream
// continues without it - the same behaviour a failed create at arm has always
// had. Figures from before and after a rebuild are different pipelines and
// must not be pooled.
// scale: R as a percentage of the display extent, or -1 to leave it. The
// quality modes are nearly inert without it - in ordinary DLSS the mode IS how
// the render resolution is picked, and here R came from the game instead.
void ui_set_sr_request(int quality, int preset, int scale);

// V41. The panel arms the stream, and holds AutoArm while it is open.
//
// Settings are read from mgpu.ini AT ARM TIME, so anything the panel writes
// before arming takes effect in THIS session - no restart. That is what makes
// a settings menu worth having, and it is why AutoArm must not fire while
// somebody is still reading it.
//
// ui_panel_drawn() is called by the overlay every time it draws; "open" means
// drawn within the last half second, because ReShade gives no closed event.
// ui_request_arm() stages; the bridge loop takes it with ui_take_arm_request()
// and calls stream_request() itself, because that is BRIDGE THREAD ONLY.
void ui_panel_drawn();
bool ui_panel_is_open();
void ui_request_arm();
bool ui_take_arm_request();

// V28. Write a key into mgpu.ini and report whether it landed. THE RUNNING
// SESSION IS UNCHANGED - every key is read at arm, so this takes effect on the
// next launch. That is the compromise for settings that cannot be live:
// Reflex is a driver mode engaged once on the game's first frame, and the DLAA
// lever moves R before the feature exists. The panel edits the file and says
// so, rather than offering a toggle that silently does nothing.
//
// Line-anchored like the reader: a commented-out key is a comment, not a key.
// One line changes and the rest of the file - which is mostly documentation -
// is preserved byte for byte. Written to a temp and renamed, because a
// half-written mgpu.ini launches with every key at its default and no way to
// tell (P7.2).
bool ui_ini_write(const char *key, int value);

// The FILE's current value for a key, cached after the first read so the panel
// can call it while drawing. NOT the running session's value: after a write
// the two disagree until the next launch, and that difference is the whole
// point of showing it.
int ui_ini_read(const char *key, int dflt);

// ---- V46: THE INSTALL LAYOUT, AND IT IS NOT A COSMETIC WARNING ----
//
// nvngx_dlssnr.dll sitting beside the game executable is THE CAUSE of the
// crash that this project spent a night on, proven 2026-09-13. The title's
// own Streamline scans the executable's folder for nvngx_*.dll and
// initialises what it finds, which binds NGX's cubin layer to the GAME'S
// adapter before this add-on's thread exists - and CreateFeature on the
// second adapter then faults inside NVIDIA's allocator and takes the process
// with it. Cyberpunk has no use for that DLL. It loaded it because 0.1.0's
// install instructions put it there.
//
// So this ranks with the GitHub line in the panel: one is where to send a log
// when something breaks, and this is the single thing most likely to be
// broken. It returns:
//
//   0  correct - a private copy under a subfolder, none beside the exe
//   1  WRONG   - a copy sits beside the exe; move it into the mgpu folder
//   2  MISSING - nvngx_dlssnr.dll is nowhere findable; neural cannot start
//
// Filesystem only. Safe to call while drawing, before anything arms, and in a
// recovery launch where NGX is deliberately untouched.
int ui_install_layout();

// P7.4. The intensity SHAPE, held as a mode rather than written once:
//   0 manual  - per-pass values as they are, nothing rewritten
//   1 front   - pass 1 at 2.00, every other pass at 0.10
//   2 back    - the LAST active pass at 2.00, every other pass at 0.10
//
// The shape FOLLOWS the pass count. Raising the count moves the peak with it,
// which is what keeps front and back a single clean variable while the count
// is changing live on camera. Any manual slider or hotkey step returns the
// mode to manual, so a hand-edited run is never labelled as a preset.
void ui_set_preset(int mode);

// P7.5. Move the split seam one step. dir is -1 or +1; coarse takes a tenth of
// the frame instead of a fortieth.
//
// A hotkey rather than only a slider, on purpose: the panel is the ReShade
// overlay, and an open overlay is the one thing that cannot be on screen while
// the seam is dragged across a face for the camera. This has to work with
// nothing visible but the game.
void ui_split_move(int dir, bool coarse);

// P7.5. Absolute seam position for the panel slider, 0.0 .. 1.0.
void ui_set_split_pos(float v);

// P6.3. BRIDGE THREAD ONLY - both of these are called from the hotkey handler
// in the message pump, which runs on the bridge thread, and they touch state
// that only the bridge thread reads. Do not call them from anywhere else.
//
// Intensity used to be read from mgpu.ini once at arm time, so finding a value
// cost one game launch per value. NGX parameters are live per evaluate (P1.2)
// and the pass loop sets them every frame, so a change here takes effect on the
// next frame with no re-arm and no relaunch.
//
// intensity_cycle_target picks what the steps act on: all passes, or one of
// them. intensity_step moves it by one increment, clamped, and logs the whole
// per-pass ladder each time so the log says what was on screen when.
//
// A run whose intensity was edited mid-stream says so in its own summary: the
// frames it covers were not all produced at the same strength, and its timings
// must not be quoted as a figure for any single value.
void intensity_cycle_target();
void intensity_step(int dir);

// ---- R142: THE GAME RUNTIME'S DEPTH-TAP STATE, FOR THE IDLE SCREEN ----
//
// Pushed in from dllmain, which is the only file that can see a ReShade
// runtime. A plain scalar for the same reason set_mvec_hook is a function
// pointer: this header names no ReShade type and dllmain names no stream
// type, and that boundary is older than this milestone.
//
//   -2  not determined yet - the enumeration has not settled
//   -1  mgpu_depth_tap.fx was NOT enumerated on the GAME runtime at all
//    0  enumerated, technique off
//    1  enumerated and on - the state Depth=1 needs
//
// ONLY THE GAME RUNTIME IS EVER REPORTED HERE. The bridge runtime has no
// depth to tap, so its own tap state is not a fault and must never reach this
// screen. Any thread; a relaxed atomic store and nothing else.
void ui_set_tap_state(int state);

// ---- R143: THE GAME RUNTIME NEVER RAN AN EFFECT PASS ----
//
// ui_set_tap_state ABOVE CANNOT REPORT THE CASE IT WAS WRITTEN FOR, and this
// exists because that was measured rather than reasoned about. The tap state
// is pushed from log_preset_once, which runs from reshade_finish_effects -
// and R138 already established, on the issue 15 reporter's machine, that the
// event only fires when ReShade actually runs an effect pass. Remove the tap
// from a GAME runtime that has no other effects and the event never arrives:
// the enumeration never runs, the tap state stays at -2, and the screen that
// was supposed to say ERROR 204 sits on "ARMING" forever. Reproduced on
// Cyberpunk 2077 with mgpu_depth_tap.fx deliberately deleted.
//
// So this is pushed from the PRESENT path instead, which arrives whatever the
// effect runtime is doing - the same correction R140 made to the log, applied
// to the screen. True means: the game's swapchain has presented past AutoArm's
// own threshold and reshade_finish_effects has never once fired on it.
//
// It is a latch. It is set once and never cleared, because the condition it
// reports cannot un-happen within a process: the early return in on_present
// stops the counter the moment an effect pass is seen, so the threshold can
// only ever be crossed by a runtime that has already stayed silent past it.
void ui_set_game_fx_absent(bool absent);

// Bridge thread. Arms the stream; inert until called, one stream per process.
// Reads Fault= from mgpu.ini beside the add-on - absent means no fault, so the
// shipped default is a clean run and a missing file is never an error.
void stream_request();

// R227 (DX11 producer). Real vectors (MVec=3) need the bind hook or the NGX
// tap, both D3D12 in this build. The producer says so once; stream_request
// then treats MVec=3 as the synthetic field (MVec=1) and logs it, instead of
// holding the arm forever for a lane that cannot deliver.
void stream_note_no_real_vectors(const char *why);
// R232: the producer saw real vectors (D3D11 DLSS evaluate tapped) - clears the above.
void stream_note_real_vectors();

// GAME thread, every frame, with the game's command list open. Filters by
// adapter LUID; signals the previous frame's fence value before recording the
// current one, because ReShade executes our list after this returns.
// R63: depth_handle is THIS FRAME's depth source, or 0 when ReShade has none.
// Zero is not an error - it is the seal's depth_valid, and it is what stops a
// menu's flat plane of zeros from reaching the model. The CALLER issues the
// transition around this call, because ReShade owns that resource and R56
// proved ReShade's own mapping is the correct one for it.
// R78: mvec_handle is the VELOCITY BUFFER the probe published, or 0 when it
// has not published one yet. It is used ONLY at arm time, to size the slot's
// MVec region from the resource; the per-frame copy does not come through
// here, because by the time this event fires the buffer is a render target
// again. See stream_mvec_copy.
// L3. The produced-count signal, moved off the effects event.
//
// stream_on_finish_effects signals gfence for the frame BEFORE the one it is
// handling, because ReShade executes the list we record into AFTER that handler
// returns - so a signal issued there would sit ahead of our own copy. That
// costs a full frame: the fence cannot report frame N until frame N+1's effects
// event fires, and L1 measured the consequence as a backlog pinned at 3.
//
// By the time ReShade raises PRESENT for frame N, that list has necessarily
// been submitted - which is the same guarantee the next-frame delay was buying,
// one event earlier. Signalling here removes the structural frame.
//
// Off unless SignalAt=1, so every run before 2026-09-12 stays reproducible.
// If ReShade turns out to submit after this event rather than before, the
// consumer reads a slot whose copy has not landed and the SEAL checker says so
// on the first frame - bad_magic or a contract mismatch, loudly, not silently.
void stream_on_present(void *cmd_queue);

void stream_on_finish_effects(void *cmd_list, void *cmd_queue,
                              unsigned long long rtv_handle,
                              unsigned long long depth_handle,
                              unsigned long long mvec_handle);

// GAME thread, MID-FRAME, from the render-target bind event - and that is the
// whole reason it is a separate entry point rather than another argument
// above.
//
// The velocity buffer is a transient. The engine writes it while drawing the
// scene and has bound it as a render target again by the time effects run, so
// finish_effects is too late to read it: the only moment it holds this
// frame's motion is the moment the bind event names it. This records a copy
// into the slot the current frame is about to seal.
//
// THE CALLER ISSUES THE BARRIER - render_target -> copy_source and back -
// because that transition belongs to a ReShade type and this file holds none.
// Same boundary the depth copy uses, for the same reason.
//
// Inert unless MVec=3 and the stream is armed, so a colour-only or synthetic
// run costs one mutex and two loads per frame.
void stream_mvec_copy(void *cmd_list, unsigned long long mvec_handle);

// HL1. The HUD-less scene texture, copied mid-frame into the slot's 4th
// region. Same caller contract as stream_mvec_copy: the probe has put the
// resource in COPY_SOURCE around the call. Inert unless HudLess=1 and armed.
void stream_hud_copy(void *cmd_list, unsigned long long handle);

// Bridge thread, once per present. Consumes whatever the fence says has
// arrived, checks each seal, and prints the summary once the producer has
// stopped and drained.
void stream_poll();

// P5.1. Bridge thread, called immediately before present_frame. Returns true
// when the bridge should put a frame on screen.
//
// While the stream is running with an on-screen output, that is once per NEW
// neural frame rather than once per vsync - which removes three quarters of the
// full-frame backbuffer copies and three quarters of the DWM cross-adapter
// copies of the bridge window, both of which were competing with the payload
// for the same link. When there is nothing new it blocks on the shared fence
// (outside the stream's lock) for up to `timeout_ms`, so the consumer wakes on
// a frame landing rather than on a vblank, and its cadence stops depending on
// which display GPU 1 is attached to.
//
// Returns true unconditionally when the stream is idle or in profile mode, so
// the cycling clear colour - T5's liveness proof - keeps running as it always
// has.
bool stream_present_gate(unsigned long timeout_ms);
}
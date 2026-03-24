// main.cpp — BYT3/YUY0 V1.22
//
// Cambios V1.22:
//   - Core1 construye ControlFrame una vez por frame y lo pasa a process()
//   - CV IN se lee desde el frame (no desde g_adc directamente post-process)
//   - Flash save gesture usa frame.is_pressed() en vez de g_pads
//   - LED update usa LedUpdateParams struct (firma legible, sin 22 args posicionales)
//   - now_ms del loop se reutiliza para g_ui.update() en vez de duplicar timestamp
//   - Doble comentario "MIDI clock out: comparar tick..." eliminado
//   - #include de AudioOutputPWM mantenido (fallback bring-up)

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "audio/audio_engine.h"
#include "audio/audio_output_i2s.h"
#include "audio/audio_output_pwm.h"
#include "io/cap_pad_handler.h"
#include "io/adc_handler.h"
#include "io/control_frame.h"        // V1.22: ControlFrame + build_control_frame
#include "io/input_router.h"
#include "state/state_manager.h"
#include "state/flash_store.h"
#include "sequencer/sequencer.h"
#include "sequencer/clock_in.h"
#include "sequencer/clock_out.h"
#include "midi/uart_midi.h"
#include "midi/midi_router.h"
#include "utils/ring_buffer.h"
#include "utils/debug_log.h"
#include "led/led_controller.h"
#include "led/led_update_params.h"   // V1.22: LedUpdateParams
#include "ui/oled_display.h"
#include "ui/ui_renderer.h"
#include "sequencer/event_types.h"

#ifdef NO_PAD_HARDWARE
#define HEARTBEAT_PIN PIN_ONBOARD_LED
#endif

// ── Compartido Core0/Core1 ────────────────────────────────────────
static RingBuffer<SequencerEvent, 128> g_event_queue;
static StateManager                    g_state;

// ── Core0 ─────────────────────────────────────────────────────────
static AudioOutputI2S g_audio_out;
// static AudioOutputPWM g_audio_out;  // fallback PWM
static AudioEngine    g_audio_engine;

// ── Core1 ─────────────────────────────────────────────────────────
static CapPadHandler  g_pads;
static AdcHandler     g_adc;
static InputRouter    g_router;
static Sequencer      g_seq;
static ClockIn        g_clock_in;
static ClockOut       g_clock_out;
static float          g_int_bpm = 120.0f;
static UartMidi       g_midi;
static MidiRouter     g_midi_router;
static LedController  g_leds;
static OledDisplay    g_oled;
static UiRenderer     g_ui;

// ── Output mode switch ────────────────────────────────────────────
static constexpr uint    OUTPUT_MODE_PIN           = 18;
static bool              g_split_output_stable     = false;
static uint8_t           g_split_output_integrator = 0;
static constexpr uint8_t OUTPUT_SWITCH_DEBOUNCE_MAX = 8u;

static bool update_output_mode_switch_debounced() {
    const bool raw_split = (gpio_get(OUTPUT_MODE_PIN) == 0);
    if (raw_split) {
        if (g_split_output_integrator < OUTPUT_SWITCH_DEBOUNCE_MAX) ++g_split_output_integrator;
    } else {
        if (g_split_output_integrator > 0) --g_split_output_integrator;
    }
    const bool prev = g_split_output_stable;
    if (g_split_output_integrator == 0u) {
        g_split_output_stable = false;
    } else if (g_split_output_integrator >= OUTPUT_SWITCH_DEBOUNCE_MAX) {
        g_split_output_stable = true;
        g_split_output_integrator = OUTPUT_SWITCH_DEBOUNCE_MAX;
    }
    return prev != g_split_output_stable;
}

// ── Scan timers ───────────────────────────────────────────────────
static repeating_timer_t g_pad_timer;
static repeating_timer_t g_adc_timer;

static bool pad_timer_cb(repeating_timer_t*) { g_pads.scan(); return true; }
static bool adc_timer_cb(repeating_timer_t*) { g_adc.poll();  return true; }

// ── Flash save: SHIFT + REC + PLAY ───────────────────────────────
static bool     save_combo_prev_     = false;
static bool     save_combo_fired_    = false;
static uint32_t save_combo_start_ms_ = 0;
static constexpr uint32_t SAVE_GUARD_MS = 700u;

// V1.22: recibe now_ms del frame en vez de llamar to_ms_since_boot() internamente
static void check_flash_save(bool shift, bool rec, bool play, uint32_t now_ms) {
    const bool save_combo = shift && rec && play;
    if (save_combo && !save_combo_prev_) {
        save_combo_start_ms_ = now_ms;
        save_combo_fired_    = false;
        g_leds.on_action(ActionFeedback::SAVE_ARM);
    }
    if (save_combo && !save_combo_fired_ && (now_ms - save_combo_start_ms_) >= SAVE_GUARD_MS) {
        bool ok = g_state.flash_save();
        LOG("MAIN: flash save (SHIFT+REC+PLAY hold) %s", ok ? "OK" : "FAIL");
        if (ok) g_leds.on_flash_save();
        save_combo_fired_ = true;
    }
    if (!save_combo) save_combo_fired_ = false;
    save_combo_prev_ = save_combo;
}

// ── Core1 entry point ─────────────────────────────────────────────
void core1_main() {
    g_pads.init(CapPadHandler::Preset::DRY());
    g_pads.calibrate();  // ~1s — no tocar pads

#ifdef NO_PAD_HARDWARE
    gpio_init(HEARTBEAT_PIN);
    gpio_set_dir(HEARTBEAT_PIN, GPIO_OUT);
    gpio_put(HEARTBEAT_PIN, 0);
#endif

    g_adc.init();
    g_clock_in.init();
    g_clock_out.init();
    g_leds.init();
    g_oled.init();
    g_ui.init(&g_oled);

    gpio_init(OUTPUT_MODE_PIN);
    gpio_set_dir(OUTPUT_MODE_PIN, GPIO_IN);
    gpio_pull_up(OUTPUT_MODE_PIN);
    sleep_us(50);
    g_split_output_stable     = (gpio_get(OUTPUT_MODE_PIN) == 0);
    g_split_output_integrator = g_split_output_stable ? OUTPUT_SWITCH_DEBOUNCE_MAX : 0u;

    g_seq.set_clock_out(&g_clock_out);
    g_seq.set_state_manager(&g_state);
    g_seq.init();

    g_router.aftertouch_mode = AftertouchMode::STUTTER_DEPTH;
    g_router.midi            = &g_midi;
    g_router.led_ctrl        = &g_leds;
    g_router.ui_renderer     = &g_ui;

    add_repeating_timer_ms(-5, pad_timer_cb, nullptr, &g_pad_timer);
    add_repeating_timer_ms(-2, adc_timer_cb, nullptr, &g_adc_timer);

    // MIDI config
    MidiConfig midi_cfg = {};
    midi_cfg.rx_channel       = 1;
    midi_cfg.tx_channel       = 1;
    midi_cfg.cc_map[0]        = 74;  // Macro
    midi_cfg.cc_map[1]        = 71;  // Tonal
    midi_cfg.cc_map[2]        = 72;  // Drive
    midi_cfg.cc_map[3]        = 73;  // Env attack
    midi_cfg.cc_map[4]        = 75;  // Env release
    midi_cfg.cc_map[5]        = 76;  // Glide
    midi_cfg.cc_map[6]        = 77;  // Delay div
    for (uint8_t i = 0; i < 8; i++)
        midi_cfg.note_map[i]  = 36 + i;  // C2-G2 → snapshots A-H
    midi_cfg.cc_out_enable    = true;
    for (uint8_t i = 0; i < 7; i++)
        midi_cfg.cc_out_map[i] = midi_cfg.cc_map[i];
    midi_cfg.clock_out_enable = true;
    midi_cfg.clock_in_enable  = true;
    g_midi.init(midi_cfg);
    g_midi_router.init(&g_midi, midi_cfg);

    ClockSource last_src      = ClockSource::INT;
    uint32_t    last_seq_tick = 0;

    while (true) {
        const uint64_t loop_start_us = time_us_64();

        // ── Clock In — auto-detect EXT/INT ───────────────────
        g_clock_in.update();
        const ClockSource desired = g_clock_in.is_ext_sync()
                                    ? ClockSource::EXT : ClockSource::INT;
        if (desired != last_src) {
            g_seq.set_clock_source(desired);
            last_src = desired;
            if (desired == ClockSource::INT) {
                g_seq.set_bpm(g_int_bpm);
                LOG("MAIN: clock -> INT | BPM restored=%.1f", g_int_bpm);
            } else {
                LOG("MAIN: clock -> EXT");
            }
        }
        if (desired == ClockSource::INT) {
            const float seq_bpm = g_seq.get_bpm();
            if (seq_bpm != g_int_bpm) g_int_bpm = seq_bpm;
        }
        if (desired == ClockSource::EXT) {
            while (g_clock_in.consume_tick()) {
                g_seq.set_bpm(g_clock_in.get_bpm());
                g_seq.on_ext_tick();
            }
        }

        const uint64_t now_us = time_us_64();
        g_seq.update_int(now_us);
        g_seq.tick(g_event_queue);
        g_clock_out.update();

        // MIDI clock out: emitir en cada tick del sequencer (24 PPQN)
        const uint32_t cur_tick = g_seq.current_tick();
        if (cur_tick != last_seq_tick) {
            g_midi_router.on_clock_tick(g_midi);
            last_seq_tick = cur_tick;
        }

        // ── V1.22: construir ControlFrame una vez por frame ──
        const ControlFrame frame = build_control_frame(g_pads, g_adc);
        const uint32_t now_ms   = frame.now_ms;

        // Input routing — todo el control pasa por el frame
        g_router.process(frame, g_seq, g_state, g_event_queue);

        // ── CV IN — desde el frame, no desde g_adc ───────────
        // Modula Macro del snapshot activo de forma aditiva.
        if (frame.cv_active) {
            const float pot      = frame.pot(0);  // P0 = MACRO capa normal
            const float combined = pot + frame.cv * (1.0f - pot);
            g_state.set_patch_param(PARAM_MACRO,
                combined > 1.0f ? 1.0f : combined);
        }

        // ── Flash save — usa frame.is_pressed() ──────────────
        check_flash_save(frame.is_pressed(PAD_SHIFT),
                         frame.is_pressed(PAD_REC),
                         frame.is_pressed(PAD_PLAY),
                         now_ms);

        // ── Output mode switch ────────────────────────────────
        const bool output_mode_changed = update_output_mode_switch_debounced();
        const bool split_output        = g_split_output_stable;
        g_audio_engine.set_output_mode(split_output ? AudioEngine::OUTPUT_SPLIT
                                                    : AudioEngine::OUTPUT_MASTER);
        if (output_mode_changed)
            g_ui.show_action_message(split_output ? "OUT SPLIT" : "OUT MASTER");

        // ── LED update — LedUpdateParams (V1.22) ─────────────
        {
            const PlayState ps       = g_seq.play_state();
            const bool      playing  = (ps == PlayState::PLAYING || ps == PlayState::RECORDING);
            const bool      is_rec   = (ps == PlayState::RECORDING);
            const bool      seq_view = g_seq.has_sequence() || g_seq.is_step_write_mode();

            const uint8_t page_base = g_seq.visible_page_base();
            uint8_t page_snap_mask   = 0;
            uint8_t page_note_mask   = 0;
            uint8_t page_drum_mask   = 0;
            uint8_t page_motion_mask = 0;
            for (uint8_t i = 0; i < 8; ++i) {
                const uint8_t step = (uint8_t)(page_base + i);
                if (g_seq.step_has_snapshot(step)) page_snap_mask   |= (1u << i);
                if (g_seq.step_has_note(step))     page_note_mask   |= (1u << i);
                if (g_seq.step_has_drum(step))     page_drum_mask   |= (1u << i);
                if (g_seq.step_has_motion(step))   page_motion_mask |= (1u << i);
            }

            uint8_t         snapshot_valid_mask = 0u;
            uint8_t         snapshot_mute_mask  = 0u;
            const Snapshot* snaps               = g_state.get_snapshots();
            for (uint8_t i = 0; i < StateManager::NUM_SNAPSHOTS; ++i) {
                if (snaps[i].valid)           snapshot_valid_mask |= (uint8_t)(1u << i);
                if (g_state.get_mute_snap(i)) snapshot_mute_mask  |= (uint8_t)(1u << i);
            }

            // V1.22: LedUpdateParams — un campo por argumento, sin posicionales
            LedUpdateParams lp;
            lp.tick_ppqn           = g_seq.current_tick();
            lp.active_slot         = g_state.get_active_slot();
            lp.is_playing          = playing;
            lp.is_rec              = is_rec;
            lp.shift_held          = frame.is_pressed(PAD_SHIFT);
            lp.shift_rec_held      = g_router.is_shift_rec_active();
            lp.note_mode_active    = g_state.is_note_mode();
            lp.env_loop_active     = g_state.get_env_loop();
            lp.snapshot_valid_mask = snapshot_valid_mask;
            lp.snapshot_mute_mask  = snapshot_mute_mask;
            lp.sequence_view_active= seq_view;
            lp.sequence_len        = g_seq.sequence_length();
            lp.sequence_page_base  = page_base;
            lp.playhead_step       = g_seq.is_step_write_mode()
                                        ? g_seq.current_write_step_index()
                                        : g_seq.last_emitted_step_index();
            lp.write_step          = g_seq.current_write_step_index();
            lp.page_snap_mask      = page_snap_mask;
            lp.page_note_mask      = page_note_mask;
            lp.page_drum_mask      = page_drum_mask;
            lp.page_motion_mask    = page_motion_mask;
            lp.manual_step_write   = g_seq.is_step_write_mode();
            lp.armed_record        = g_seq.is_armed_record();
            lp.preroll_steps_left  = g_seq.preroll_steps_left();
            g_leds.update(lp);

            g_ui.set_status(g_state.get_active_slot(),
                            g_seq.get_bpm(),
                            playing,
                            is_rec,
                            lp.shift_held,
                            g_router.is_shift_rec_active(),
                            g_state.is_note_mode(),
                            g_state.is_snapshot_morph_active(),
                            split_output,
                            g_state.get_encoder_state(),
                            g_state.note_active(),
                            g_state.note_degree(),
                            g_state.note_midi(),
                            g_state.note_voice_source());
            g_ui.update(now_ms);
        }

        // ── MIDI RX ───────────────────────────────────────────
        g_midi.poll_rx();
        g_midi_router.process_in(g_midi, g_state, g_seq, g_event_queue);

#ifdef NO_PAD_HARDWARE
        if ((now_ms % 1000) < 500)
            gpio_put(HEARTBEAT_PIN, 1);
        else
            gpio_put(HEARTBEAT_PIN, 0);
#endif

        // Sleep adaptativo: mantener slot de 500µs
        {
            const uint64_t elapsed = time_us_64() - loop_start_us;
            if (elapsed < 500u) sleep_us(500u - elapsed);
        }
    }
}

// ── Core0 entry point ─────────────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(500);
    LOG("=== BYT3/YUY0 V1.22 ===");

    g_state.init();
    LOG("MAIN: state init OK");

    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_event_queue);

    multicore_lockout_victim_init();
    multicore_launch_core1(core1_main);

    LOG("Core0: audio @ %u Hz, BLOCK_SIZE=%u",
        AudioEngine::SAMPLE_RATE, AudioEngine::BLOCK_SIZE);
    g_audio_engine.run();  // nunca retorna
    return 0;
}

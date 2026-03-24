#pragma once
// led/led_update_params.h — BYT3/YUY0 V1.22
//
// LedUpdateParams: agrupa los 22 argumentos de LedController::update()
// en un struct con miembros nombrados y valores default seguros.
//
// Agregar en led_controller.h la sobrecarga:
//   void update(const LedUpdateParams& p);
// (ver snippet al final de este archivo)

#include <cstdint>

struct LedUpdateParams {
    uint32_t tick_ppqn            = 0;
    uint8_t  active_slot          = 0;
    bool     is_playing           = false;
    bool     is_rec               = false;
    bool     shift_held           = false;
    bool     shift_rec_held       = false;
    bool     note_mode_active     = false;
    bool     env_loop_active      = false;
    uint8_t  snapshot_valid_mask  = 0;
    uint8_t  snapshot_mute_mask   = 0;
    bool     sequence_view_active = false;
    uint8_t  sequence_len         = 0;
    uint8_t  sequence_page_base   = 0;
    uint8_t  playhead_step        = 0;
    uint8_t  write_step           = 0;
    uint8_t  page_snap_mask       = 0;
    uint8_t  page_note_mask       = 0;
    uint8_t  page_drum_mask       = 0;
    uint8_t  page_motion_mask     = 0;
    bool     manual_step_write    = false;
    bool     armed_record         = false;
    uint8_t  preroll_steps_left   = 0;
};

// ── Snippet para agregar en led_controller.h ─────────────────────
// Dentro de la clase LedController, después de la firma larga:
//
//   #include "led_update_params.h"
//
//   void update(const LedUpdateParams& p) {
//       update(p.tick_ppqn, p.active_slot, p.is_playing, p.is_rec,
//              p.shift_held, p.shift_rec_held, p.note_mode_active,
//              p.env_loop_active, p.snapshot_valid_mask, p.snapshot_mute_mask,
//              p.sequence_view_active, p.sequence_len, p.sequence_page_base,
//              p.playhead_step, p.write_step, p.page_snap_mask,
//              p.page_note_mask, p.page_drum_mask, p.page_motion_mask,
//              p.manual_step_write, p.armed_record, p.preroll_steps_left);
//   }

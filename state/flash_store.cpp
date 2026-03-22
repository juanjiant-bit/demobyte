// flash_store.cpp — Bytebeat Machine V1.21
// V1.10: pack/unpack de env_release_u8, env_attack_u8, env_loop (V3)
// V1.18: pack/unpack de delay_div_u8, delay_wet_u8 (V4)
//        Migración V3→V4: delay_div=0.40 (1/4 negra), delay_wet=0.0 (off)
// V1.22: payload del motor bytebeat paramétrico (V5)
// Persistencia de snapshots en Flash. Formato V2 incluye scale_id, root,
// drum_color, drum_decay.
#include "flash_store.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "../utils/debug_log.h"
#include <cstring>

#ifndef XIP_BASE
static constexpr uint32_t XIP_BASE = 0x10000000u;
#endif


namespace {
inline uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
}

// ── Pack / Unpack ─────────────────────────────────────────────
void FlashStore::pack(const Snapshot& src, PackedSnapshot& dst) {
    memset(&dst, 0, sizeof(dst));
    dst.snapshot_version = src.snapshot_version;
    dst.seed          = src.seed;
    dst.zone          = src.zone;
    dst.macro         = src.macro;
    dst.glide_time    = src.glide_time;
    dst.time_div      = src.time_div;
    dst.tonal         = src.tonal;
    dst.spread        = src.spread;
    dst.filter_cutoff = src.filter_cutoff;
    dst.fx_amount     = src.fx_amount;
    dst.drive         = src.drive;
    dst.reverb_room   = src.reverb_room;
    dst.reverb_wet    = src.reverb_wet;
    dst.valid         = src.valid ? 1u : 0u;
    // V2 fields
    dst.scale_id      = src.scale_id;
    dst.root          = src.root;
    dst.drum_color    = src.drum_color;
    dst.drum_decay    = src.drum_decay;
    // V3 fields (V1.10)
    dst.env_release_u8 = (uint8_t)(src.env_release * 255.0f + 0.5f);
    dst.env_attack_u8  = (uint8_t)(src.env_attack  * 255.0f + 0.5f);
    dst.env_loop       = src.env_loop ? 1u : 0u;
    // V4 fields (V1.18)
    dst.delay_div_u8   = (uint8_t)(src.delay_div * 255.0f + 0.5f);
    dst.delay_wet_u8   = (uint8_t)(src.delay_wet * 255.0f + 0.5f);
    dst.formula_a      = src.formula_a;
    dst.formula_b      = src.formula_b;
    dst.morph          = src.morph;
    dst.rate           = src.rate;
    dst.shift          = src.shift;
    dst.mask           = src.mask;
    dst.feedback       = src.feedback;
    dst.jitter         = src.jitter;
    dst.phase          = src.phase;
    dst.xor_fold       = src.xor_fold;
    dst.bb_seed        = src.bb_seed;
    dst.filter_macro   = src.filter_macro;
    dst.resonance      = src.resonance;
    dst.env_macro      = src.env_macro;
}

void FlashStore::unpack(const PackedSnapshot& src, Snapshot& dst) {
    dst.snapshot_version = src.snapshot_version;
    dst.seed          = src.seed;
    dst.zone          = src.zone;
    dst.macro         = src.macro;
    dst.glide_time    = src.glide_time;
    dst.time_div      = src.time_div;
    dst.tonal         = src.tonal;
    dst.spread        = src.spread;
    dst.filter_cutoff = src.filter_cutoff;
    dst.fx_amount     = src.fx_amount;
    dst.drive         = src.drive;
    dst.reverb_room   = src.reverb_room;
    dst.reverb_wet    = src.reverb_wet;
    dst.valid         = (src.valid != 0);
    // V2 fields (con defaults seguros si se carga formato V1 antiguo)
    dst.scale_id      = (src.scale_id < 8) ? src.scale_id : 1u;  // default MAJOR
    dst.root          = (src.root < 12)    ? src.root      : 0u;  // default C
    dst.drum_color    = (src.drum_color >= 0.0f && src.drum_color <= 1.0f)
                        ? src.drum_color : 0.0f;
    dst.drum_decay    = (src.drum_decay >= 0.0f && src.drum_decay <= 1.0f)
                        ? src.drum_decay : 0.5f;
    // V3 fields — presentes si se guardó con V1.10+.
    // Si ambos campos son 0 asumimos migración V2→V3 y usamos defaults
    // musicales (evita click audible en release con valor 0).
    const bool env_fields_empty = (src.env_release_u8 == 0 && src.env_attack_u8 == 0
                                   && src.env_loop == 0);
    dst.env_release   = env_fields_empty ? 0.15f : (src.env_release_u8 / 255.0f);
    dst.env_attack    = env_fields_empty ? 0.01f : (src.env_attack_u8  / 255.0f);
    dst.env_loop      = (src.env_loop != 0);
    // V4 fields — siempre decodificar directamente.
    // La detección de migración V3→V4 la hace load() usando block.version.
    dst.delay_div = src.delay_div_u8 / 255.0f;
    dst.delay_wet = src.delay_wet_u8 / 255.0f;
    dst.formula_a = src.formula_a % BytebeatGraph::FORMULA_COUNT;
    dst.formula_b = src.formula_b % BytebeatGraph::FORMULA_COUNT;
    dst.morph = src.morph;
    dst.rate = src.rate;
    dst.shift = clamp_u8(src.shift, 0u, 7u);
    dst.mask = src.mask;
    dst.feedback = src.feedback;
    dst.jitter = src.jitter;
    dst.phase = src.phase;
    dst.xor_fold = src.xor_fold;
    dst.bb_seed = src.bb_seed;
    dst.filter_macro = src.filter_macro;
    dst.resonance = src.resonance;
    dst.env_macro = src.env_macro;
}

// ── Load ──────────────────────────────────────────────────────
bool FlashStore::load(Snapshot out_snapshots[8]) {
    const uint8_t* flash_ptr =
        reinterpret_cast<const uint8_t*>(XIP_BASE + FLASH_TARGET_OFFSET);

    FlashBlock block;
    memcpy(&block, flash_ptr, sizeof(FlashBlock));

    if (block.magic != FLASH_MAGIC) {
#ifdef DEBUG_AUDIO
        printf("[FLASH] magic inválido 0x%08lX — usando defaults\n",
               (unsigned long)block.magic);
#endif
        return false;
    }

    // Política de versiones:
    //   V4 (actual)  → carga completa.
    //   V3 (1.10)    → carga con delay_div/wet = defaults (0.40/0.0).
    //   V2 (1.6.1)   → carga con env fields + delay = defaults musicales.
    //   V1 o unknown → rechazar: estructura pre-V2 no tiene campos de validación
    //                  confiables, los floats legacy podrían ser NaN/Inf.
    if (block.version < 2 || block.version > FLASH_VERSION) {
#ifdef DEBUG_AUDIO
        printf("[FLASH] versión incompatible %u (soportadas: 2-%u) — usando defaults\n",
               block.version, FLASH_VERSION);
#endif
        return false;
    }
    // Validar CRC sobre el array de snapshots
    uint32_t crc_computed = crc32_compute(
        reinterpret_cast<const uint8_t*>(block.snapshots),
        sizeof(block.snapshots)
    );
    if (crc_computed != block.crc32) {
#ifdef DEBUG_AUDIO
        printf("[FLASH] CRC mismatch 0x%08lX vs 0x%08lX\n",
               (unsigned long)crc_computed, (unsigned long)block.crc32);
#endif
        return false;
    }

    uint8_t n = block.num_snapshots;
    if (n > 8) n = 8;
    const bool is_pre_v4 = (block.version < 4);
    const bool is_pre_v5 = (block.version < 5);
    const bool is_pre_v6 = (block.version < 6);
    for (uint8_t i = 0; i < n; i++) {
        unpack(block.snapshots[i], out_snapshots[i]);
        // Migración V3→V4: delay fields eran _pad3=0 (no inicializado).
        // Aplicar defaults musicales: div=0.40 (1/4 negra), wet=0.0 (off).
        if (is_pre_v4 && out_snapshots[i].valid) {
            out_snapshots[i].delay_div = 0.40f;
            out_snapshots[i].delay_wet = 0.0f;
        }
        if (is_pre_v5 && out_snapshots[i].valid) {
            out_snapshots[i].snapshot_version = 0;
            out_snapshots[i].formula_a = 0;
            out_snapshots[i].formula_b = 1;
            out_snapshots[i].morph = 96;
            out_snapshots[i].rate = 128;
            out_snapshots[i].shift = 2;
            out_snapshots[i].mask = 160;
            out_snapshots[i].feedback = 24;
            out_snapshots[i].jitter = 12;
            out_snapshots[i].phase = 32;
            out_snapshots[i].xor_fold = 0;
            out_snapshots[i].bb_seed = 0;
            out_snapshots[i].filter_macro = 128;
            out_snapshots[i].resonance = 24;
            out_snapshots[i].env_macro = 128;
        }
        if (is_pre_v6 || out_snapshots[i].snapshot_version == 0) {
            out_snapshots[i].snapshot_version = SNAPSHOT_ENGINE_VERSION;
        }
    }
    for (uint8_t i = n; i < 8; i++) {
        out_snapshots[i] = Snapshot{};
        out_snapshots[i].snapshot_version = SNAPSHOT_ENGINE_VERSION;
        out_snapshots[i].valid = false;
    }
#ifdef DEBUG_AUDIO
    printf("[FLASH] load OK — %u snapshots\n", n);
#endif
    return true;
}

// ── Save ──────────────────────────────────────────────────────
// Protocolo multicore-safe:
//   1. multicore_lockout_start_blocking() — pausa Core0
//   2. save_irq = save_and_disable_interrupts()
//   3. flash_range_erase()
//   4. flash_range_program()
//   5. restore_interrupts(save_irq)
//   6. multicore_lockout_end_blocking() — reactiva Core0
bool FlashStore::save(const Snapshot in_snapshots[8]) {
    FlashBlock block;
    memset(&block, 0xFF, sizeof(block));

    block.magic         = FLASH_MAGIC;
    block.version       = FLASH_VERSION;
    block.num_snapshots = 8;
    block._pad          = 0;

    for (uint8_t i = 0; i < 8; i++) {
        pack(in_snapshots[i], block.snapshots[i]);
    }
    block.crc32 = crc32_compute(
        reinterpret_cast<const uint8_t*>(block.snapshots),
        sizeof(block.snapshots)
    );

    // Preparar buffer alineado a 256 bytes (requisito flash_range_program)
    static uint8_t buf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &block, sizeof(FlashBlock));

    // Pausa Core0 y deshabilita IRQ para escritura Flash segura
    multicore_lockout_start_blocking();
    uint32_t save_irq = save_and_disable_interrupts();

    flash_range_erase  (FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buf, FLASH_SECTOR_SIZE);

    restore_interrupts(save_irq);
    multicore_lockout_end_blocking();

#ifdef DEBUG_AUDIO
    printf("[FLASH] save OK — CRC=0x%08lX\n", (unsigned long)block.crc32);
#endif
    return true;
}

// ── Erase (factory reset) ─────────────────────────────────────
void FlashStore::erase_sector() {
    multicore_lockout_start_blocking();
    uint32_t save_irq = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(save_irq);
    multicore_lockout_end_blocking();
}

#pragma once
// flash_store.h — Bytebeat Machine V1.21
// V1.10: PackedSnapshot agrega env_release_u8, env_attack_u8, env_loop_u8 (3 bytes)
//        Tamaño 64 → 68 bytes. FLASH_VERSION: 2 → 3.
// V1.18: PackedSnapshot agrega delay_div_u8, delay_wet_u8 (2 bytes + padding)
//        Tamaño 68 → 72 bytes. FLASH_VERSION: 3 → 4.
//        Migración V3→V4: delay_div/wet → defaults (0.40 / 0.0).
// Persistencia de snapshots en el último sector Flash del RP2040 (4KB).
//
// LAYOUT (offset 0x1FF000, 4096 bytes):
//   [0..3]   magic  0xBB161100  (bytebeat 1.6.1)
//   [4..7]   CRC32 del bloque de snapshots
//   [8..9]   version = 2
//   [10..11] num_snapshots = 8
//   [12..N]  8 × PackedSnapshot V6
//   resto    0xFF
//
// REGLAS:
//   Llamar SOLO desde Core1 con Core0 pausado (multicore_lockout).
//   flash_store.save() maneja todo el protocolo de pausa.
//
// VERSIÓN 2 añade: scale_id, root, drum_color, drum_decay.
//
#include <cstdint>
#include "state_manager.h"

static constexpr uint32_t FLASH_TARGET_OFFSET = 0x1FF000u;  // 2MB - 4KB
static constexpr uint32_t FLASH_SECTOR_SIZE   = 4096u;
static constexpr uint32_t FLASH_MAGIC         = 0xBB161100u;  // V1.6.1
static constexpr uint16_t FLASH_VERSION       = 6;            // Formato V6: agrega versionado por snapshot y migración robusta

// ── PackedSnapshot V2 ─────────────────────────────────────────
// 64 bytes — todos los campos de Snapshot que el usuario puede guardar.
// El BytebeatGraph se regenera desde seed en init().
struct PackedSnapshot {
    uint8_t  snapshot_version;
    uint8_t  _padv[3];
    // V1 fields (offset 4)
    uint32_t seed;
    uint8_t  zone;
    uint8_t  _pad0[3];
    float    macro;
    float    glide_time;
    float    time_div;
    float    tonal;
    float    spread;
    float    filter_cutoff;
    float    fx_amount;
    float    drive;
    float    reverb_room;
    float    reverb_wet;
    uint8_t  valid;
    uint8_t  _pad1[3];
    // V2 fields (offset 52) — añadidos en V1.6.1
    uint8_t  scale_id;
    uint8_t  root;
    uint8_t  _pad2[2];
    float    drum_color;
    float    drum_decay;
    // V1.10 — envelope (empaquetado como uint8_t para mínimo impacto en layout)
    uint8_t  env_release_u8;  // 0-255 mapeado de float 0.0-1.0
    uint8_t  env_attack_u8;   // 0-255 mapeado de float 0.0-1.0
    uint8_t  env_loop;        // 0=off, 1=on
    // V1.18 — delay (div+wet en snapshot; fb queda como bus global)
    uint8_t  delay_div_u8;    // 0-255 → float 0.0-1.0 (reemplaza _pad3)
    uint8_t  delay_wet_u8;    // 0-255 → float 0.0-1.0

    // V5 — bytebeat engine snapshot payload
    uint8_t  formula_a;
    uint8_t  formula_b;
    uint8_t  morph;
    uint8_t  rate;
    uint8_t  shift;
    uint8_t  mask;
    uint8_t  feedback;
    uint8_t  jitter;
    uint8_t  phase;
    uint8_t  xor_fold;
    uint8_t  bb_seed;
    uint8_t  filter_macro;
    uint8_t  resonance;
    uint8_t  env_macro;
    uint8_t  _pad4[1];        // alineación → sizeof = 92
} __attribute__((packed));

static_assert(sizeof(PackedSnapshot) == 92, "PackedSnapshot V6 size mismatch");

struct FlashBlock {
    uint32_t       magic;
    uint32_t       crc32;
    uint16_t       version;
    uint8_t        num_snapshots;
    uint8_t        _pad;
    PackedSnapshot snapshots[8];
} __attribute__((packed));  // 8 × 92 = 736 bytes de snapshots

// ── CRC32 IEEE 802.3 (sin tabla, sin dependencias) ────────────
inline uint32_t crc32_compute(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (crc & 1u ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

// ── API ───────────────────────────────────────────────────────
class FlashStore {
public:
    // Cargar snapshots. Retorna true si magic+CRC+version OK.
    // Llamar desde Core1 antes de lanzar Core0.
    static bool load(Snapshot out_snapshots[8]);

    // Guardar snapshots. Pausa Core0, erase, write, reactiva.
    // Solo desde Core1.
    static bool save(const Snapshot in_snapshots[8]);

    // Erase explícito (útil para factory reset)
    static void erase_sector();

private:
    static void pack  (const Snapshot& src, PackedSnapshot& dst);
    static void unpack(const PackedSnapshot& src, Snapshot& dst);
};

#pragma once
// snapshot_event.h — canal Core1 → Core0
// Solo TRIGGER activo. SAVE fue eliminado en V1.21: request_save() llama
// do_save() directamente con spin_lock, sin pasar por la cola de eventos.
#include <cstdint>

enum class SnapshotEventType : uint8_t { NONE, TRIGGER };

struct SnapshotEvent {
    SnapshotEventType type = SnapshotEventType::NONE;
    uint8_t           slot = 0;
};

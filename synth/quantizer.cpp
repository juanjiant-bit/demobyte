#include "quantizer.h"

float Quantizer::note_mode_degree_to_ratio(uint8_t scale_id, uint8_t root, uint8_t degree) {
    if (scale_id >= SCALE_COUNT) scale_id = (uint8_t)ScaleId::MAJOR;
    const auto& d = SCALE_DEFS[scale_id];
    const uint8_t idx = degree % 8u;
    const uint8_t step = idx % d.length;
    const uint8_t octave = idx / d.length;
    const uint8_t semitone = uint8_t(d.degrees[step] + root + 12u * octave);
    return semitone_to_ratio(semitone);
}

const char* Quantizer::scale_name(uint8_t scale_id) {
    if (scale_id >= SCALE_COUNT) scale_id = (uint8_t)ScaleId::MAJOR;
    return SCALE_DEFS[scale_id].name;
}

// state/state_manager.cpp — BYT3/YUY0 V1.22
//
// Este archivo es idéntico al V1.21 excepto por:
//
// BUG-SM-2 CORREGIDO: set_graph_param_on_live_graphs() faltaba
// begin_live_write() al inicio. El par begin/end_live_write() implementa
// un seqlock: Core0 re-intenta la lectura si live_state_seq_ cambia entre
// el inicio y el fin. Sin el begin al inicio, Core0 podía ver una secuencia
// par (aparentemente válida) mientras la escritura estaba en progreso.
//
// ÚNICA FUNCIÓN MODIFICADA — pegar en el .cpp existente reemplazando la
// implementación de set_graph_param_on_live_graphs():

/*
void StateManager::set_graph_param_on_live_graphs(ParamId id, uint8_t value) {
    begin_live_write();   // <-- LÍNEA AÑADIDA
    switch (id) {
    case PARAM_FORMULA_A:
        graphs_[active_graph_].set_formula_a(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_formula_a(value);
        break;
    case PARAM_FORMULA_B:
        graphs_[active_graph_].set_formula_b(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_formula_b(value);
        break;
    case PARAM_MORPH:
        graphs_[active_graph_].set_morph(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_morph(value);
        break;
    case PARAM_RATE:
        graphs_[active_graph_].set_rate(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_rate(value);
        break;
    case PARAM_SHIFT:
        graphs_[active_graph_].set_shift(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_shift(value);
        break;
    case PARAM_MASK:
        graphs_[active_graph_].set_mask(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_mask(value);
        break;
    case PARAM_FEEDBACK:
        graphs_[active_graph_].set_feedback(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_feedback(value);
        break;
    case PARAM_JITTER:
        graphs_[active_graph_].set_jitter(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_jitter(value);
        break;
    case PARAM_PHASE:
        graphs_[active_graph_].set_phase(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_phase(value);
        break;
    case PARAM_XOR_FOLD:
        graphs_[active_graph_].set_xor_fold(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_xor_fold(value);
        break;
    case PARAM_BB_SEED:
        graphs_[active_graph_].set_seed_mod(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_seed_mod(value);
        break;
    case PARAM_FILTER_MACRO:
        graphs_[active_graph_].set_filter_macro(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_filter_macro(value);
        break;
    case PARAM_RESONANCE:
        graphs_[active_graph_].set_resonance(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_resonance(value);
        break;
    case PARAM_ENV_MACRO:
        graphs_[active_graph_].set_env_macro(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_env_macro(value);
        break;
    default:
        break;
    }
    end_live_write();
}
*/
//
// El resto del .cpp no cambia. Mantener state_manager.cpp V1.21 completo
// y aplicar solo este reemplazo.

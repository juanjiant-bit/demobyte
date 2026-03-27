// Dem0!Byt3 V6.2 — base V6 preserved, pads hardened, louder master
//
// Hardware baseline preserved:
// ROW = GP5
// COL = GP8, GP9, GP13, GP14
// POT = GP26
// BCLK = GP10
// DIN = GP12
//
// Design goals:
// - preserve the clear V6 bytebeat path
// - make pad triggers much more stable
// - add hold / pseudo-aftertouch state
// - raise output level without turning the whole thing into harsh noise

#include <cstdint>
#include <cmath>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "pcm5102_i2s.pio.h"
#include "audio/drums/drum_engine.h"

static PIO  g_pio = pio0;
static uint g_sm  = 0;

static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

constexpr uint PIN_ROW  = 5;
constexpr uint PIN_COL[] = {8, 9, 13, 14};
constexpr uint PIN_LED  = 25;
constexpr uint PIN_POT  = 26;
constexpr uint PIN_BCLK = 10;
constexpr uint PIN_DIN  = 12;
constexpr uint SR       = 44100;

static volatile float   g_pot = 0.5f;
static volatile uint8_t g_pad_event = 0;
static volatile bool    g_ready = false;
static volatile float   g_pad_pressure[4] = {0.f, 0.f, 0.f, 0.f};

static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next(){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static inline float rng_f(){ return float(rng_next() >> 8) * (1.f / 16777215.f); }
static inline float fw(float x){ x -= (int)x; return x < 0 ? x + 1.f : x; }
static inline float fs(float p){ float x = fw(p) * 6.28318f - 3.14159f, y = x * (1.2732f - 0.4053f * fabsf(x)); return y * (0.225f * (fabsf(y) - 1.f) + 1.f); }
static inline float ft(float p){ float x = fw(p); return 4.f * fabsf(x - .5f) - 1.f; }
static inline float fsq(float p, float pw = .5f){ return fw(p) < pw ? 1.f : -1.f; }
static inline float fclip(float x){ return x / (1.f + fabsf(x)); }
static inline float ffold(float x){ x = x * .5f + .5f; x -= (int)x; if(x < 0) x += 1.f; return x * 2.f - 1.f; }
static inline float lerp(float a, float b, float t){ return a + (b - a) * t; }
static inline float clamp01(float x){ return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

// ── Bytebeat ──────────────────────────────────────────────────────
static inline uint8_t bbf(uint8_t id, uint32_t t, uint8_t s){
    switch(id % 17u){
    case 0:  return (uint8_t)(t * (((((t >> 10) & 42u) & 0xFFu) ? (((t >> 10) & 42u) & 0xFFu) : 1u)));
    case 1:  return (uint8_t)(t * (((((t >> 9) ^ (t >> 11)) & 28u) + 4u)));
    case 2:  return (uint8_t)(t * (((((t >> 8) & 15u) ^ ((t >> 11) & 7u)) + 3u)));
    case 3:  return (uint8_t)(t * (((((t >> 10) & 5u) | ((t >> 13) & 2u)) + 2u)));
    case 4:  return (uint8_t)(t & (t >> 8));
    case 5:  return (uint8_t)(((t * 5u) & (t >> 7)) | ((t * 3u) & (t >> 10)));
    case 6:  return (uint8_t)(((t >> 6) | (t * 3u)) & ((t >> 9) | (t * 5u)));
    case 7:  return (uint8_t)(((t >> 5) & (t >> 8)) | ((t >> 3) & (t * 2u)));
    case 8:  return (uint8_t)(((t >> 4) & (t >> 7)) * ((255u - (t >> 6)) & 255u));
    case 9:  return (uint8_t)(((t * (9u + (s & 1u))) & (t >> 4)) ^ ((t * (5u + ((s >> 1) & 1u))) & (t >> 7)));
    case 10: return (uint8_t)((t >> 2) ^ (t >> 5) ^ (t >> 7));
    case 11: return (uint8_t)((t * ((t >> 9) & 3u)) & (t >> 5));
    case 12: return (uint8_t)(t ^ (t >> 3) ^ (t >> 6));
    case 13: return (uint8_t)((t * (t >> 9)) ^ (t >> 7) ^ (t >> 13));
    case 14: return (uint8_t)(((t * 7u) & (t >> 9)) ^ ((t * 11u) & (t >> 11)));
    case 15: return (uint8_t)((t * ((((t >> 10) & 21u) + 3u))) | ((t >> 7) & (t >> 9)));
    case 16: return (uint8_t)(((t * ((((t >> 11) & 13u) + 2u))) ^ ((t * 5u) & (t >> 8))));
    default: return (uint8_t)t;
    }
}

// ── Floatbeat ─────────────────────────────────────────────────────
static constexpr float PENTA[8] = {1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]  = {.5f,.749f,1.f,.749f};

struct FbSt{
    float t = 0.f, env = 0.f, sph = 0.f, drift = 0.f;
    uint32_t lf = 0xACE1u;
};

static float fbalgo(FbSt& st, float dt, float hz, float body, uint8_t algo){
    st.t += dt;
    if(st.t > 4096.f) st.t -= 4096.f;

    st.drift += dt * 0.17f;
    if(st.drift > 6.28318f) st.drift -= 6.28318f;

    float hz_live = hz * (1.f + fs(st.drift * 0.15f) * 0.08f);
    const float t = st.t;
    hz_live = hz_live < 20 ? 20 : (hz_live > 2200 ? 2200 : hz_live);
    body = clamp01(body);

    switch(algo % 12u){
    case 0:{ float x = fs(t * hz_live) * (.5f + .2f * body) + fs(t * hz_live * .5f) * (.25f + .2f * body) + fs(t * hz_live * 1.5f) * .08f; return fclip(x * .85f); }
    case 1:{ float x = fs(t * hz_live * .5f) * .55f + fs(t * hz_live) * .30f + ft(t * hz_live * 2.f) * .08f * (1.f + body); return fclip(x * .9f); }
    case 2:{ float a = fs(t * hz_live), b = fs(t * (hz_live * 1.007f)); return fclip(a * b * (.7f + .2f * body) + fs(t * hz_live * .5f) * .2f); }
    case 3:{ st.env += dt * 4.f; if(st.env > 1.f) st.env = 0.f; float e = 1.f - st.env; return fclip(fs(t * hz_live + fs(t * hz_live * 2.f) * .3f * e) * e * (.8f + .15f * body)); }
    case 4:{ float pw = .1f + .4f * (fs(t * hz_live * .125f) * .5f + .5f); return fclip((fsq(t * hz_live, pw) * (.5f + .3f * body) + fs(t * hz_live * .5f) * .2f) * .75f); }
    case 5:{ float gate = fsq(t * hz_live * .0625f, .35f) * .5f + .5f; return fclip((fs(t * hz_live) + ft(t * hz_live * 2.01f) * .3f) * gate * (.7f + .2f * body)); }
    case 6:{ st.sph += dt * hz_live * .03125f; uint8_t n = (uint8_t)(st.sph) % 8u; return fclip((fs(t * hz_live * PENTA[n]) * (.65f + .2f * body) + fs(t * hz_live * PENTA[n] * .5f) * .2f) * .85f); }
    case 7:{ float ps = st.sph; st.sph += dt * hz_live * .03125f; if((uint8_t)st.sph != (uint8_t)ps){ st.lf ^= st.lf << 7; st.lf ^= st.lf >> 9; } return fclip(fs(t * hz_live * PENTA[st.lf % 8u]) * (.75f + .15f * body)); }
    case 8:{ st.sph += dt * hz_live * .015625f; uint8_t n = (uint8_t)(st.sph) % 4u; return fclip((fs(t * hz_live * BASS[n]) * (.6f + .25f * body) + fs(t * hz_live * BASS[n] * .5f) * .25f)); }
    case 9:{ float idx = 2.f + body * 5.f, mod = fs(t * (hz_live * 1.41f)) * idx; return fclip((fs(t * hz_live + mod) * .6f + fs(t * hz_live * .5f) * .2f) * .8f); }
    case 10:{ float x = ft(t * hz_live) * (1.5f + body * 2.5f); return fclip(ffold(x) * (.65f + .2f * body)); }
    case 11:{ st.lf ^= st.lf << 13; st.lf ^= st.lf >> 17; st.lf ^= st.lf << 5; float n = float((int32_t)st.lf) * (1.f / 2147483648.f); return fclip(fs(t * hz_live) * (.3f + .4f * body) + n * (.15f - .1f * body)); }
    default: return 0.f;
    }
}

// ── Voice / Synth ────────────────────────────────────────────────
struct Voice{
    uint8_t  bfa = 2, bfb = 10, bmorph = 128, bseed = 0;
    uint16_t brate = 1;
    FbSt     fst;
    uint8_t  falgo = 0;
    float    fhz = 110.f, fbody = .5f;
    float    lp = 0.f;
    int32_t  dcx = 0, dcy = 0;
    float    fdcx = 0.f, fdcy = 0.f;

    void randomize(){
        bfa = (uint8_t)(rng_next() % 17u);
        bfb = (uint8_t)(rng_next() % 17u);
        bmorph = (uint8_t)(rng_next() >> 24);
        brate = (uint16_t)(1u + rng_next() % 3u);
        bseed = (uint8_t)(rng_next() >> 24);
        falgo = (uint8_t)(rng_next() % 12u);
        fhz = 55.f * powf(2.f, rng_f() * 4.f);
        fbody = .15f + rng_f() * .70f;
        fst = FbSt{};
    }

    float bb_sample(uint32_t t){
        const uint32_t ts = t / (brate ? brate : 1u);
        uint8_t va = bbf(bfa, ts, bseed);
        uint8_t vb = bbf(bfb, ts ^ (uint32_t)(bseed * 0x55u), bseed ^ 0xA5u);
        uint8_t raw = (uint8_t)(((uint16_t)va * (255u - bmorph) + (uint16_t)vb * bmorph) >> 8);
        float bb = float((int8_t)(raw ^ 0x80u)) * (1.f / 128.f);

        lp += 0.05f * (bb - lp);
        bb = lp * .65f + bb * .35f;

        int32_t s32 = (int32_t)(bb * 32767.f);
        int32_t y = s32 - dcx + ((dcy * 252) >> 8);
        dcx = s32;
        dcy = y;
        return float(y) * (1.f / 32767.f);
    }

    float fb_sample(float dt){
        float fb = fbalgo(fst, dt, fhz, fbody, falgo);
        float y = fb - fdcx + fdcy * (252.f / 256.f);
        fdcx = fb;
        fdcy = y;
        return y;
    }
};

struct Synth{
    Voice va, vb;
    float xfade = 1.f;
    int   active_new = 0;

    void randomize(){
        if(active_new == 0){ vb.randomize(); active_new = 1; }
        else{ va.randomize(); active_new = 0; }
        xfade = 0.f;
    }

    int16_t next(uint32_t t, float pot){
        constexpr float DT = 1.f / 44100.f;

        if(xfade < 1.f) xfade += DT * 20.f;
        if(xfade > 1.f) xfade = 1.f;

        Voice& v_old = (active_new == 0) ? vb : va;
        Voice& v_new = (active_new == 0) ? va : vb;

        float out;
        const float sm = xfade * xfade * (3.f - 2.f * xfade);

        if(pot < 0.3f){
            float bb = v_new.bb_sample(t);
            float coeff = 0.02f + pot * (0.10f / 0.3f);
            v_new.lp += coeff * (bb - v_new.lp);
            out = v_new.lp;
        } else if(pot < 0.7f){
            float m = (pot - 0.3f) / 0.4f;

            const uint32_t ta = t / (v_new.brate ? v_new.brate : 1u);
            const uint32_t tb = t / (v_old.brate ? v_old.brate : 1u);

            uint8_t ra = (uint8_t)(((uint16_t)bbf(v_new.bfa, ta, v_new.bseed) * (255u - v_new.bmorph)
                                  + (uint16_t)bbf(v_new.bfb, ta ^ (uint32_t)(v_new.bseed * 0x55u), v_new.bseed ^ 0xA5u) * v_new.bmorph) >> 8);
            uint8_t rb = (uint8_t)(((uint16_t)bbf(v_old.bfa, tb, v_old.bseed) * (255u - v_old.bmorph)
                                  + (uint16_t)bbf(v_old.bfb, tb ^ (uint32_t)(v_old.bseed * 0x55u), v_old.bseed ^ 0xA5u) * v_old.bmorph) >> 8);

            uint8_t rmix = (uint8_t)(ra * (1.f - m) + rb * m);
            float bb = float((int8_t)(rmix ^ 0x80u)) * (1.f / 128.f);

            v_new.lp += 0.05f * (bb - v_new.lp);
            bb = v_new.lp * .65f + bb * .35f;

            int32_t s32 = (int32_t)(bb * 32767.f);
            int32_t y = s32 - v_new.dcx + ((v_new.dcy * 252) >> 8);
            v_new.dcx = s32;
            v_new.dcy = y;
            float bb_out = float(y) * (1.f / 32767.f);

            float fb_amt = m > 0.5f ? (m - 0.5f) * 2.f : 0.f;
            float fb_out = v_new.fb_sample(DT);
            out = lerp(bb_out, fb_out, fb_amt * fb_amt);
        } else {
            out = v_new.fb_sample(DT);
        }

        if(sm < 0.999f){
            float old_out;
            if(pot < 0.3f)      old_out = v_old.bb_sample(t);
            else if(pot < 0.7f) old_out = v_old.fb_sample(DT);
            else                old_out = v_old.fb_sample(DT);
            out = lerp(old_out, out, sm);
        }

        // preserve V6 clarity, slightly hotter than before
        out *= 0.92f;
        if(out > .95f) out = .95f;
        if(out < -.95f) out = -.95f;
        return (int16_t)(out * 32767.f);
    }
};

// ── Pads hardened ────────────────────────────────────────────────
static uint32_t pad_dis_us = 5000;
static uint32_t pad_max_us = 30000;

static float    pad_base[4]      = {};
static float    pad_pressure[4]  = {};
static bool     pad_on[4]        = {};
static bool     pad_prev[4]      = {};
static uint8_t  pad_conf_on[4]   = {};
static uint8_t  pad_conf_off[4]  = {};
static uint16_t pad_cooldown[4]  = {};

static uint32_t measure_pad(uint8_t c){
    for(uint8_t i = 0; i < 4; ++i){
        if(i != c){
            gpio_set_dir(PIN_COL[i], GPIO_OUT);
            gpio_put(PIN_COL[i], 0);
        }
    }

    gpio_set_dir(PIN_COL[c], GPIO_IN);
    gpio_disable_pulls(PIN_COL[c]);

    gpio_put(PIN_ROW, 0);
    sleep_us(pad_dis_us);

    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);

    while(!gpio_get(PIN_COL[c])){
        if((time_us_32() - t0) >= pad_max_us){
            gpio_put(PIN_ROW, 0);
            for(uint8_t i = 0; i < 4; ++i){
                gpio_set_dir(PIN_COL[i], GPIO_IN);
                gpio_disable_pulls(PIN_COL[i]);
            }
            return pad_max_us;
        }
    }

    const uint32_t dt = time_us_32() - t0;
    gpio_put(PIN_ROW, 0);

    for(uint8_t i = 0; i < 4; ++i){
        gpio_set_dir(PIN_COL[i], GPIO_IN);
        gpio_disable_pulls(PIN_COL[i]);
    }

    return dt;
}

static void calibrate_pads(){
    static const uint32_t dis_steps[] = {500,1000,2000,5000,10000,20000,50000};

    for(int s = 0; s < 7; ++s){
        pad_dis_us = dis_steps[s];
        pad_max_us = dis_steps[s] * 15;
        bool ok = true;

        for(uint8_t c = 0; c < 4; ++c){
            uint32_t v = 0;
            for(int i = 0; i < 6; ++i) v += measure_pad(c);
            v /= 6;
            if(v >= pad_max_us * 85 / 100){ ok = false; break; }
        }

        if(ok) break;
    }

    gpio_put(PIN_ROW, 0);
    sleep_ms(120);

    for(uint8_t c = 0; c < 4; ++c){
        uint32_t vals[24];
        for(int i = 0; i < 24; ++i) vals[i] = measure_pad(c);

        // simple trimmed mean: drop 4 highest and 4 lowest
        for(int i = 0; i < 24; ++i){
            for(int j = i + 1; j < 24; ++j){
                if(vals[j] < vals[i]){
                    uint32_t tmp = vals[i];
                    vals[i] = vals[j];
                    vals[j] = tmp;
                }
            }
        }

        uint64_t sum = 0;
        for(int i = 4; i < 20; ++i) sum += vals[i];
        pad_base[c] = float(sum / 16);
        pad_pressure[c] = 0.f;
    }
}

static void scan_pads(){
    bool any = pad_on[0] || pad_on[1] || pad_on[2] || pad_on[3];

    for(uint8_t c = 0; c < 4; ++c){
        pad_prev[c] = pad_on[c];

        if(pad_cooldown[c] > 0) pad_cooldown[c]--;

        const uint32_t raw = measure_pad(c);
        const float delta = float(raw) - pad_base[c];

        // harder thresholds than V6 to reduce ghost triggers
        const float on_th   = pad_base[c] * 0.34f + 30.f;
        const float hold_th = pad_base[c] * 0.22f + 16.f;
        const float off_th  = pad_base[c] * 0.12f + 8.f;

        if(!pad_on[c]){
            if(delta >= on_th && pad_cooldown[c] == 0){
                if(pad_conf_on[c] < 255) pad_conf_on[c]++;
                if(pad_conf_on[c] >= 3){
                    pad_on[c] = true;
                    pad_conf_on[c] = 0;
                    pad_conf_off[c] = 0;
                    pad_cooldown[c] = 3;
                }
            } else {
                pad_conf_on[c] = 0;
                if(!any){
                    // adapt baseline only when no pad is active globally
                    pad_base[c] += 0.0006f * (float(raw) - pad_base[c]);
                }
            }
        } else {
            if(delta < off_th){
                if(pad_conf_off[c] < 255) pad_conf_off[c]++;
                if(pad_conf_off[c] >= 3){
                    pad_on[c] = false;
                    pad_conf_off[c] = 0;
                    pad_conf_on[c] = 0;
                    pad_pressure[c] = 0.f;
                }
            } else {
                pad_conf_off[c] = 0;
            }
        }

        // pseudo-aftertouch / hold pressure
        if(pad_on[c]){
            float p = (delta - hold_th) / (on_th * 1.6f);
            p = clamp01(p);
            pad_pressure[c] += 0.18f * (p - pad_pressure[c]);
        } else {
            pad_pressure[c] *= 0.85f;
            if(pad_pressure[c] < 0.001f) pad_pressure[c] = 0.f;
        }

        g_pad_pressure[c] = pad_pressure[c];
    }
}

static void core1_main(){
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    gpio_init(PIN_ROW);
    gpio_set_dir(PIN_ROW, GPIO_OUT);
    gpio_put(PIN_ROW, 0);

    for(uint8_t c = 0; c < 4; ++c){
        gpio_init(PIN_COL[c]);
        gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    adc_init();
    adc_gpio_init(PIN_POT);
    adc_select_input(0);

    gpio_put(PIN_LED, 1);
    calibrate_pads();
    gpio_put(PIN_LED, 0);
    sleep_ms(80);
    gpio_put(PIN_LED, 1);
    sleep_ms(80);
    gpio_put(PIN_LED, 0);

    g_ready = true;

    float pot_s = float(adc_read()) / 4095.f;
    while(true){
        pot_s += 0.06f * (float(adc_read()) / 4095.f - pot_s);
        g_pot = pot_s;

        scan_pads();

        if(pad_on[0] && !pad_prev[0]){
            g_pad_event |= 1u;
            gpio_put(PIN_LED, 1);
        }
        if(!pad_on[0]) gpio_put(PIN_LED, 0);

        if(pad_on[1] && !pad_prev[1]) g_pad_event |= 2u;
        if(pad_on[2] && !pad_prev[2]) g_pad_event |= 4u;
        if(pad_on[3] && !pad_prev[3]) g_pad_event |= 8u;

        sleep_us(400);
    }
}

// ── Master ───────────────────────────────────────────────────────
static float g_master_env = 0.f;

static inline int16_t master_process(float x){
    float a = fabsf(x);
    g_master_env += 0.0025f * (a - g_master_env);

    float gain = 1.0f;
    if(g_master_env > 0.42f) gain = 0.42f / g_master_env;

    // makeup gain after light compression
    x *= gain;
    x *= 1.55f;

    x = fclip(x);

    if(x > 0.98f) x = 0.98f;
    if(x < -0.98f) x = -0.98f;

    return (int16_t)(x * 32767.f);
}

int main(){
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, SR);

    for(int i = 0; i < 16; ++i) i2s_write(0, 0);

    static DrumEngine drums;
    drums.init();
    drums.set_params(0.3f, 0.5f, 1.0f);

    static Synth synth;
    synth.va.randomize();
    synth.vb.randomize();

    multicore_launch_core1(core1_main);
    while(!g_ready) sleep_ms(10);

    uint32_t t = 0, cr = 0;

    while(true){
        if(++cr >= 32u){
            cr = 0;

            const uint8_t ev = g_pad_event;
            if(ev){
                g_pad_event = 0;
                if(ev & 1u) synth.randomize();
                if(ev & 2u) drums.trigger(DRUM_KICK);
                if(ev & 4u) drums.trigger(DRUM_SNARE);
                if(ev & 8u) drums.trigger(DRUM_HAT);
            }

            const float pot = g_pot;

            // Keep V6 behavior, but let pad pressure subtly affect drum decay/brightness.
            const float p2 = g_pad_pressure[1];
            const float p3 = g_pad_pressure[2];
            const float p4 = g_pad_pressure[3];

            const float drum_color = clamp01(pot * .6f + .1f + p4 * 0.08f);
            const float drum_decay = clamp01(.3f + pot * .5f + p2 * 0.12f + p3 * 0.08f);

            drums.set_params(drum_color, drum_decay, -1.f);
        }

        const int16_t ss = synth.next(t, g_pot);

        int16_t dl = 0, dr = 0;
        int32_t sc = 32767;
        drums.process(dl, dr, sc);

        const int32_t sd = ((int32_t)ss * sc) >> 15;

        // slightly hotter than V6, but still conservative before master comp
        int32_t ol = ((sd * 21627) >> 15) + (((int32_t)dl * 30146) >> 15); // ~0.66 synth + ~0.92 drums
        int32_t or_ = ((sd * 21627) >> 15) + (((int32_t)dr * 30146) >> 15);

        float xl = float(ol) / 32768.f;
        float xr = float(or_) / 32768.f;

        i2s_write(master_process(xl), master_process(xr));
        ++t;
    }
}

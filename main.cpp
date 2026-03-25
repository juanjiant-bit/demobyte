// Dem0!Byt3 V4 — synth depth + pad fix
//
// CAMBIOS vs V3:
//   SÍNTESIS: pot dividido en 3 zonas claras (BB / morph / FB)
//             floatbeat amplía paleta: arpegios, percusivo, secuencial, estático
//             EQ: LP más agresivo en BB, ganancia equilibrada, sin clip excesivo
//   PADS:     baseline congelado mientras hay toque activo en CUALQUIER pad
//             discharge aumentado a 30ms, threshold más alto y confirm=3
//             baseline_alpha = 0 durante actividad → sin deriva espontánea

// ── PARÁMETROS DE PAD ─────────────────────────────────────────────
#define PAD_DISCHARGE_US   30000   // 30ms
#define PAD_MAX_US         80000   // 80ms timeout
#define PAD_THRESHOLD_ON   0.28f   // bajar si no responde, subir si falsos triggers
#define PAD_THRESHOLD_OFF  0.14f
#define PAD_CONFIRM_NEEDED 3
#define PAD_BASELINE_ALPHA 0.001f  // muy lento — solo deriva cuando ningún pad activo

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

constexpr uint PIN_ROW     = 5;
constexpr uint PIN_COL[4]  = {8, 9, 13, 14};
constexpr uint PIN_LED     = 25;
constexpr uint PIN_POT     = 26;
constexpr uint PIN_BCLK    = 10;
constexpr uint PIN_DIN     = 12;
constexpr uint SAMPLE_RATE = 44100;

static volatile float   g_pot       = 0.5f;
static volatile uint8_t g_pad_event = 0;
static volatile bool    g_ready     = false;

static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng<<13; g_rng ^= g_rng>>17; g_rng ^= g_rng<<5; return g_rng;
}
static inline float rng_f() { return float(rng_next()>>8)*(1.0f/16777215.0f); }

// ── Osciladores float ─────────────────────────────────────────────
static inline float f_wrap(float x) { x-=(int)x; return x<0?x+1.0f:x; }
static inline float f_sine(float p) {
    float x=f_wrap(p)*6.28318f-3.14159f;
    float y=x*(1.2732f-0.4053f*fabsf(x));
    return y*(0.225f*(fabsf(y)-1.0f)+1.0f);
}
static inline float f_tri(float p)  { float x=f_wrap(p); return 4.0f*fabsf(x-0.5f)-1.0f; }
static inline float f_saw(float p)  { return f_wrap(p)*2.0f-1.0f; }
static inline float f_sqr(float p, float pw=0.5f) { return f_wrap(p)<pw?1.0f:-1.0f; }
static inline float f_clip(float x) { return x/(1.0f+fabsf(x)); }
static inline float f_fold(float x) {  // wavefold suave
    x = x*0.5f+0.5f;
    x = x-(int)x; if(x<0) x+=1.0f;
    x = x*2.0f-1.0f;
    return x;
}

// ── Bytebeat — 17 fórmulas ────────────────────────────────────────
static inline uint8_t bb_formula(uint8_t id, uint32_t t, uint8_t s) {
    switch (id%17u) {
    case 0: return(uint8_t)(t*((((t>>10)&42u)&0xFFu)?(((t>>10)&42u)&0xFFu):1u));
    case 1: return(uint8_t)(t*((((t>>9)^(t>>11))&28u)+4u));
    case 2: return(uint8_t)(t*((((t>>8)&15u)^((t>>11)&7u))+3u));
    case 3: return(uint8_t)(t*((((t>>10)&5u)|((t>>13)&2u))+2u));
    case 4: return(uint8_t)(t&(t>>8));
    case 5: return(uint8_t)(((t*5u)&(t>>7))|((t*3u)&(t>>10)));
    case 6: return(uint8_t)(((t>>6)|(t*3u))&((t>>9)|(t*5u)));
    case 7: return(uint8_t)(((t>>5)&(t>>8))|((t>>3)&(t*2u)));
    case 8: return(uint8_t)(((t>>4)&(t>>7))*((255u-(t>>6))&255u));
    case 9: return(uint8_t)(((t*(9u+(s&1u)))&(t>>4))^((t*(5u+((s>>1)&1u)))&(t>>7)));
    case 10:return(uint8_t)((t>>2)^(t>>5)^(t>>7));
    case 11:return(uint8_t)((t*((t>>9)&3u))&(t>>5));
    case 12:return(uint8_t)(t^(t>>3)^(t>>6));
    case 13:return(uint8_t)((t*(t>>9))^(t>>7)^(t>>13));
    case 14:return(uint8_t)(((t*7u)&(t>>9))^((t*11u)&(t>>11)));
    case 15:return(uint8_t)((t*((((t>>10)&21u)+3u)))|((t>>7)&(t>>9)));
    case 16:return(uint8_t)(((t*((((t>>11)&13u)+2u)))^((t*5u)&(t>>8))));
    default:return(uint8_t)t;
    }
}

// ── Floatbeat — 12 algos con mayor variedad de comportamiento ─────
// Categorías:
//   0-2: tónicos estables (buena base)
//   3-5: rítmicos / percusivos (envelopes internos)
//   6-8: secuenciales / melódicos (pitch cuantizado)
//   9-11: texturales / ruidosos

struct FbState {
    float t=0, t2=0, env=0, seq_ph=0;
    uint32_t lfsr=0xACE1u;
};

// Notas de una escala menor pentatónica expresadas como ratios
static constexpr float PENTA[8] = {1.0f,1.189f,1.335f,1.498f,1.782f,2.0f,2.378f,2.670f};

static float fb_algo(FbState& st, float dt, float hz, float body, uint8_t algo) {
    st.t += dt;
    if (st.t > 4096.0f) st.t -= 4096.0f;
    st.t2 += dt * 1.618f;  // fase secundaria en razón áurea
    if (st.t2 > 4096.0f) st.t2 -= 4096.0f;

    const float t = st.t;
    hz = hz<20?20:(hz>2000?2000:hz);
    body = body<0?0:(body>1?1:body);

    switch (algo % 12u) {

    // ── TÓNICOS ───────────────────────────────────────────────────
    case 0: {  // sine stack — cuerpo lleno, estable
        float x = f_sine(t*hz)*(0.50f+0.20f*body)
                + f_sine(t*hz*0.5f)*(0.25f+0.20f*body)
                + f_sine(t*hz*1.5f)*0.08f;
        return f_clip(x*0.85f);
    }
    case 1: {  // sub organ — fundamental + 2do armónico suave
        float x = f_sine(t*hz*0.5f)*0.55f
                + f_sine(t*hz)*0.30f
                + f_tri(t*hz*2.0f)*0.08f*(1.0f+body);
        return f_clip(x*0.90f);
    }
    case 2: {  // metallic pair — ring mod entre dos osciladores cercanos
        float a = f_sine(t*hz);
        float b = f_sine(t*(hz*1.007f));  // casi unísono → beating lento
        float x = a*b*(0.70f+0.20f*body) + f_sine(t*hz*0.5f)*0.20f;
        return f_clip(x);
    }

    // ── RÍTMICOS / PERCUSIVOS ─────────────────────────────────────
    case 3: {  // pluck envelope — decay rápido con retroalimentación
        st.env += dt * 4.0f;
        if (st.env > 1.0f) st.env = 0.0f;  // reset periódico
        float env = 1.0f - st.env;          // decae cada 0.25s a 110Hz
        float x = f_sine(t*hz + f_sine(t*hz*2.0f)*0.3f*env) * env;
        return f_clip(x*(0.80f+0.15f*body));
    }
    case 4: {  // stutter — wave cuadrada con duty cycle modulado
        float pw = 0.1f + 0.4f * (f_sine(t*hz*0.125f)*0.5f+0.5f);
        float x = f_sqr(t*hz, pw)*(0.5f+0.3f*body)
                + f_sine(t*hz*0.5f)*0.20f;
        return f_clip(x*0.75f);
    }
    case 5: {  // gate burst — tono con gate rítmico a subdivisión
        float gate_rate = hz * 0.0625f;  // 1/16 del tono
        float gate = f_sqr(t*gate_rate, 0.35f)*0.5f+0.5f;
        float x = (f_sine(t*hz)+f_tri(t*hz*2.01f)*0.3f) * gate;
        return f_clip(x*(0.70f+0.20f*body));
    }

    // ── SECUENCIALES / MELÓDICOS ──────────────────────────────────
    case 6: {  // arpeggio up — sube por la pentatónica
        st.seq_ph += dt * hz * 0.03125f;  // avanza 1 nota cada 32 periodos
        uint8_t note = (uint8_t)(st.seq_ph) % 8u;
        float f = hz * PENTA[note];
        float x = f_sine(t*f)*(0.65f+0.20f*body)
                + f_sine(t*f*0.5f)*0.20f;
        return f_clip(x*0.85f);
    }
    case 7: {  // arpeggio random — salta entre notas de la escala
        st.seq_ph += dt * hz * 0.03125f;
        uint8_t step = (uint8_t)(st.seq_ph);
        if (step != (uint8_t)(st.seq_ph - dt*hz*0.03125f)) {
            // nueva nota: usa LFSR para elegir dentro de la escala
            st.lfsr ^= st.lfsr<<7; st.lfsr ^= st.lfsr>>9;
        }
        uint8_t note = st.lfsr % 8u;
        float f = hz * PENTA[note];
        return f_clip(f_sine(t*f)*(0.75f+0.15f*body));
    }
    case 8: {  // bass seq — 3 notas que se repiten (root, 5th, octave)
        st.seq_ph += dt * hz * 0.015625f;  // más lento
        static const float BASS[4] = {0.5f, 0.749f, 1.0f, 0.749f};
        uint8_t n = (uint8_t)(st.seq_ph) % 4u;
        float f = hz * BASS[n];
        float x = f_sine(t*f)*(0.60f+0.25f*body)
                + f_sine(t*f*0.5f)*0.25f;
        return f_clip(x);
    }

    // ── TEXTURALES ────────────────────────────────────────────────
    case 9: {  // FM caótico — índice de mod alto
        float idx = 2.0f + body*5.0f;
        float mod = f_sine(t*(hz*1.41f)) * idx;
        float x = f_sine(t*hz + mod)*0.60f
                + f_sine(t*hz*0.5f)*0.20f;
        return f_clip(x*0.80f);
    }
    case 10: {  // wavefold sweep — tri con fold controlado por body
        float x = f_tri(t*hz) * (1.5f + body*2.5f);
        x = f_fold(x);
        return f_clip(x*(0.65f+0.20f*body));
    }
    case 11: {  // noise filtered — ruido con LP resonante simulado
        st.lfsr ^= st.lfsr<<13; st.lfsr ^= st.lfsr>>17; st.lfsr ^= st.lfsr<<5;
        float noise = float((int32_t)st.lfsr) * (1.0f/2147483648.0f);
        // LP simple: el feedback del seno actúa como resonancia
        float filt = f_sine(t*hz) * (0.30f+0.40f*body) + noise*(0.15f-0.10f*body);
        return f_clip(filt);
    }
    default: return 0.0f;
    }
}

// ── Motor híbrido ─────────────────────────────────────────────────
struct HybridSynth {
    uint8_t  bb_fa=2, bb_fb=10, bb_morph=128, bb_seed=0;
    uint16_t bb_rate=1;
    FbState  fb_st;
    uint8_t  fb_algo_id=0;
    float    fb_hz=110.0f, fb_body=0.5f;
    float    xfade=1.0f, prev_out=0.0f;
    float    bb_lp=0.0f;       // LP del bytebeat — elimina agudos excesivos
    float    fb_dcx=0, fb_dcy=0;
    int32_t  bb_dcx=0, bb_dcy=0;

    void randomize(float pot) {
        bb_fa    = (uint8_t)(rng_next()%17u);
        bb_fb    = (uint8_t)(rng_next()%17u);
        bb_morph = (uint8_t)(rng_next()>>24);
        bb_rate  = (uint16_t)(1u+rng_next()%3u);
        bb_seed  = (uint8_t)(rng_next()>>24);
        fb_algo_id = (uint8_t)(rng_next()%12u);
        // hz fija según pot en el momento del random, NO sigue el pot
        // Esto evita que todos los fb suenen igual
        fb_hz   = 55.0f * powf(2.0f, pot*3.5f);  // 55..622 Hz
        fb_body = 0.15f + rng_f()*0.70f;
        // reset estado para evitar artefactos
        fb_st = FbState{};
        xfade = 0.0f;
    }

    int16_t next(uint32_t t, float pot) {
        constexpr float DT = 1.0f/44100.0f;

        // ── Bytebeat ──────────────────────────────────────────────
        const uint32_t ts = t/(bb_rate?bb_rate:1u);
        uint8_t va = bb_formula(bb_fa, ts, bb_seed);
        uint8_t vb = bb_formula(bb_fb, ts^(uint32_t)(bb_seed*0x55u), bb_seed^0xA5u);
        uint8_t bb_raw = (uint8_t)(((uint16_t)va*(255u-bb_morph)+(uint16_t)vb*bb_morph)>>8);
        float bb_f = float((int8_t)(bb_raw^0x80u))*(1.0f/128.0f);

        // LP agresivo para bajar agudos del bytebeat (el principal culpable del clipping)
        // coeff=0.06 → fc≈420Hz — elimina alias pero conserva carácter
        bb_lp += 0.06f*(bb_f - bb_lp);
        bb_f = bb_lp * 0.75f + bb_f * 0.25f;  // mezcla crudo+filtrado

        // DC blocker
        int32_t bb_s32=(int32_t)(bb_f*32767.0f);
        int32_t bb_y=bb_s32-bb_dcx+((bb_dcy*252)>>8);
        bb_dcx=bb_s32; bb_dcy=bb_y;
        bb_f=float(bb_y)*(1.0f/32767.0f);

        // ── Floatbeat ─────────────────────────────────────────────
        float fb_f = fb_algo(fb_st, DT, fb_hz, fb_body, fb_algo_id);

        // DC blocker float
        float fby=fb_f-fb_dcx+fb_dcy*(252.0f/256.0f);
        fb_dcx=fb_f; fb_dcy=fby; fb_f=fby;

        // ── Pot divide en 3 zonas ─────────────────────────────────
        // 0.0..0.4 → bytebeat puro con LP variable (pot controla cutoff)
        // 0.4..0.6 → morph progresivo BB→FB
        // 0.6..1.0 → floatbeat puro, pot controla fb_hz en tiempo real
        float domain;
        float bb_vol=1.0f, fb_vol=1.0f;
        if (pot < 0.4f) {
            domain = 0.0f;
            // en zona BB: pot controla el LP del bytebeat (abre el filtro)
            float lp_coeff = 0.02f + pot*(0.25f/0.4f);
            bb_lp += lp_coeff*(bb_f - bb_lp);  // segundo LP
            bb_f = bb_lp;
        } else if (pot < 0.6f) {
            domain = (pot-0.4f)/0.2f;  // 0..1 en la zona morph
        } else {
            domain = 1.0f;
            // en zona FB: pot actualiza hz en tiempo real
            float target = 55.0f*powf(2.0f,(pot-0.6f)*(3.5f/0.4f));
            fb_hz += 0.0005f*(target-fb_hz);
        }
        float out = bb_f*(1.0f-domain)*bb_vol + fb_f*domain*fb_vol;

        // Ganancia suave — sin clip duro
        out *= 0.80f;
        if (out >  0.92f) out =  0.92f;
        if (out < -0.92f) out = -0.92f;

        // Crossfade al randomizar
        if (xfade < 1.0f) {
            out = prev_out*(1.0f-xfade) + out*xfade;
            xfade += 1.0f/512.0f;  // ~11ms
        }
        prev_out = out;
        return (int16_t)(out*32767.0f);
    }
};

// ── Pads ──────────────────────────────────────────────────────────
static float   pad_base[4]    = {};
static bool    pad_on[4]      = {};
static bool    pad_prev[4]    = {};
static uint8_t pad_confirm[4] = {};

static uint32_t measure_pad(uint8_t c) {
    gpio_put(PIN_ROW,0); sleep_us(PAD_DISCHARGE_US);
    const uint32_t t0=time_us_32();
    gpio_put(PIN_ROW,1);
    while(!gpio_get(PIN_COL[c]))
        if((time_us_32()-t0)>=(uint32_t)PAD_MAX_US){gpio_put(PIN_ROW,0);return PAD_MAX_US;}
    const uint32_t dt=time_us_32()-t0; gpio_put(PIN_ROW,0); return dt;
}

static void calibrate_pads() {
    gpio_put(PIN_ROW,0); sleep_ms(100);
    for(uint8_t c=0;c<4;++c){
        uint64_t sum=0;
        for(int s=0;s<16;++s) sum+=measure_pad(c);
        pad_base[c]=float(sum/16);
    }
}

static void scan_pads() {
    // Si cualquier pad está activo, congela el baseline de TODOS
    // Esto evita que el baseline suba durante un toque y "cancele" la detección
    bool any_active = pad_on[0]||pad_on[1]||pad_on[2]||pad_on[3];

    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        const uint32_t raw=measure_pad(c);
        const float d=float(raw)-pad_base[c];
        const float on_th =pad_base[c]*PAD_THRESHOLD_ON;
        const float off_th=pad_base[c]*PAD_THRESHOLD_OFF;

        if(!pad_on[c]){
            if(d>=on_th){
                if(++pad_confirm[c]>=PAD_CONFIRM_NEEDED) pad_on[c]=true;
            } else {
                pad_confirm[c]=0;
                // solo actualiza baseline si NINGÚN pad está activo
                if(!any_active)
                    pad_base[c]+=PAD_BASELINE_ALPHA*(float(raw)-pad_base[c]);
            }
        } else {
            if(d<off_th){ pad_on[c]=false; pad_confirm[c]=0; }
        }
    }
}

static void core1_main() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED,GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW,GPIO_OUT); gpio_put(PIN_ROW,0);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c],GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }
    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);

    gpio_put(PIN_LED,1);
    calibrate_pads();
    gpio_put(PIN_LED,0);
    g_ready=true;

    float pot_s=float(adc_read())/4095.0f;
    while(true){
        pot_s+=0.06f*(float(adc_read())/4095.0f-pot_s);
        g_pot=pot_s;
        scan_pads();
        if(pad_on[0]&&!pad_prev[0]){g_pad_event|=1u;gpio_put(PIN_LED,1);}
        if(!pad_on[0])              gpio_put(PIN_LED,0);
        if(pad_on[1]&&!pad_prev[1]) g_pad_event|=2u;
        if(pad_on[2]&&!pad_prev[2]) g_pad_event|=4u;
        if(pad_on[3]&&!pad_prev[3]) g_pad_event|=8u;
    }
}

int main() {
    const uint off=pio_add_program(g_pio,&pcm5102_i2s_program);
    g_sm=pio_claim_unused_sm(g_pio,true);
    pcm5102_i2s_program_init(g_pio,g_sm,off,PIN_DIN,PIN_BCLK,SAMPLE_RATE);
    for(int i=0;i<16;++i) i2s_write(0,0);

    static DrumEngine drums;
    drums.init();
    drums.set_params(0.3f,0.5f,1.0f);

    static HybridSynth synth;

    multicore_launch_core1(core1_main);
    while(!g_ready) sleep_ms(10);

    uint32_t t=0, cr=0;
    while(true){
        if(++cr>=32u){
            cr=0;
            const uint8_t ev=g_pad_event;
            if(ev){
                g_pad_event=0;
                if(ev&1u) synth.randomize(g_pot);
                if(ev&2u) drums.trigger(DRUM_KICK);
                if(ev&4u) drums.trigger(DRUM_SNARE);
                if(ev&8u) drums.trigger(DRUM_HAT);
            }
            const float pot=g_pot;
            drums.set_params(pot*0.8f, 0.2f+pot*0.6f, -1.0f);
        }

        const int16_t ss=synth.next(t,g_pot);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd=((int32_t)ss*sc)>>15;
        int32_t ol=sd+(int32_t)dl, or_=sd+(int32_t)dr;
        if(ol> 32767)ol= 32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;
        i2s_write((int16_t)ol,(int16_t)or_);
        ++t;
    }
}

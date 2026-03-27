// Dem0!Byt3 V15 — motor mínimo verificado matemáticamente
//
// BUGS CORREGIDOS en iteraciones anteriores:
//   - DC blocker coeff=252/256 → fc=110Hz → cortaba fundamental del BB (34Hz)
//   - BRATE 256-2048 → t_bb avanza 10Hz → DC blocker = silencio
//   - Doble adc_init() → ADC corrupto
//
// MOTOR:
//   BB: t/brate (brate=4-8) → formula → LP 400-2000Hz → DC blocker fc=6Hz
//   FB: oscilador sinusoidal puro con pitch y body simples
//   MORPH: lerp(bb, fb, pot)
//   Sin crossfade durante síntesis — crossfade solo en randomize
//
// POTS:
//   GP26 MORPH  — 0=BB puro, 1=FB puro
//   GP27 MACRO1 — PITCH del FB (55-880Hz logarítmico)
//   GP28 MACRO2 — BODY/TIMBRE (0=simple, 1=complejo) + LP del BB
//                 Lectura directa, sin slew

#include <cstdint>
#include <cmath>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "pcm5102_i2s.pio.h"
#include "audio/drums/drum_engine.h"

static PIO  g_pio=pio0; static uint g_sm=0;
static inline void i2s_write(int16_t l,int16_t r){
    pio_sm_put_blocking(g_pio,g_sm,(uint32_t)(uint16_t)l<<16);
    pio_sm_put_blocking(g_pio,g_sm,(uint32_t)(uint16_t)r<<16);
}

constexpr uint PIN_PAD[4]={8,9,13,14};
constexpr uint PIN_LED=25, PIN_BCLK=10, PIN_DIN=12, SR=44100;

static volatile float   g_morph=0.5f;
static volatile float   g_macro1=0.5f;
static volatile float   g_macro2=0.5f;
static volatile uint8_t g_pad_event=0;
static volatile bool    g_ready=false;

static uint32_t g_rng=0xDEADBEEFu;
static inline uint32_t rng_next(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
static inline float rng_f(){return float(rng_next()>>8)*(1.f/16777215.f);}
static inline float clampf(float x,float lo,float hi){return x<lo?lo:(x>hi?hi:x);}

// ── Oscilador sinusoidal rápido ───────────────────────────────────
static inline float fw(float x){x-=(int)x; return x<0.f?x+1.f:x;}
static inline float fast_sin(float phase){
    // Bhaskara I approximation — error < 0.2%
    float x=fw(phase)*6.28318f-3.14159f;
    float y=x*(1.2732f-0.4053f*fabsf(x));
    return y*(0.225f*(fabsf(y)-1.f)+1.f);
}

// ── Bytebeat 17 fórmulas ─────────────────────────────────────────
static inline uint8_t bbf(uint8_t id, uint32_t t, uint8_t s){
    switch(id%17u){
    case  0: return(uint8_t)(t*((((t>>10)&42u)&0xFFu)?(((t>>10)&42u)&0xFFu):1u));
    case  1: return(uint8_t)(t*((((t>>9)^(t>>11))&28u)+4u));
    case  2: return(uint8_t)(t*((((t>>8)&15u)^((t>>11)&7u))+3u));
    case  3: return(uint8_t)(t*((((t>>10)&5u)|((t>>13)&2u))+2u));
    case  4: return(uint8_t)(t&(t>>8));
    case  5: return(uint8_t)(((t*5u)&(t>>7))|((t*3u)&(t>>10)));
    case  6: return(uint8_t)(((t>>6)|(t*3u))&((t>>9)|(t*5u)));
    case  7: return(uint8_t)(((t>>5)&(t>>8))|((t>>3)&(t*2u)));
    case  8: return(uint8_t)(((t>>4)&(t>>7))*((255u-(t>>6))&255u));
    case  9: return(uint8_t)(((t*(9u+(s&1u)))&(t>>4))^((t*(5u+((s>>1)&1u)))&(t>>7)));
    case 10: return(uint8_t)((t>>2)^(t>>5)^(t>>7));
    case 11: return(uint8_t)((t*((t>>9)&3u))&(t>>5));
    case 12: return(uint8_t)(t^(t>>3)^(t>>6));
    case 13: return(uint8_t)((t*(t>>9))^(t>>7)^(t>>13));
    case 14: return(uint8_t)(((t*7u)&(t>>9))^((t*11u)&(t>>11)));
    case 15: return(uint8_t)((t*((((t>>10)&21u)+3u)))|((t>>7)&(t>>9)));
    case 16: return(uint8_t)(((t*((((t>>11)&13u)+2u)))^((t*5u)&(t>>8))));
    default: return(uint8_t)t;
    }
}

// ── Floatbeat — osciladores simples con body ─────────────────────
struct FbState { float phase=0.f, phase2=0.f, phase3=0.f; };

static float floatbeat(FbState& s, float dt, float hz, float body, uint8_t algo){
    // Avanzar fases
    s.phase  = fw(s.phase  + dt*hz);
    s.phase2 = fw(s.phase2 + dt*hz*2.0f);
    s.phase3 = fw(s.phase3 + dt*hz*0.5f);

    float p  = fast_sin(s.phase);
    float p2 = fast_sin(s.phase2);
    float p3 = fast_sin(s.phase3);

    float out;
    switch(algo % 8u){
    case 0: // Sine puro + sub
        out = p*(0.6f+0.2f*body) + p3*(0.3f+0.2f*body);
        break;
    case 1: // Sine + 2do armónico
        out = p*0.65f + p2*(0.25f+0.25f*body);
        break;
    case 2: // Ring mod suave (beating)
        out = p * fast_sin(fw(s.phase + dt*hz*0.05f));  // ~5% detuning
        out = out*(0.7f+0.2f*body) + p3*0.2f;
        break;
    case 3: // Tri suave (approx con sines)
        out = p*0.7f - p2*(0.1f+0.1f*body) + fast_sin(fw(s.phase3*3.f))*0.05f*body;
        break;
    case 4: // Arpeggio sobre pentatónica (cambia nota cada ciclo)
        {
        static constexpr float penta[5]={1.f,1.189f,1.335f,1.498f,1.782f};
        uint8_t note=(uint8_t)(s.phase3*5.f)%5u;
        float note_phase=fw(s.phase*penta[note]);
        out=fast_sin(note_phase)*(0.7f+0.2f*body)+p3*0.2f;
        }
        break;
    case 5: // FM simple
        {
        float mod=fast_sin(s.phase2)*body*2.f;
        out=fast_sin(fw(s.phase+mod*0.15f))*0.75f + p3*0.2f;
        }
        break;
    case 6: // Sine + sub octave
        out = p*0.55f + fast_sin(fw(s.phase*0.5f))*(0.3f+0.2f*body);
        break;
    case 7: // Chorus sutil (dos sines ligeramente detuned)
        out = p*0.5f + fast_sin(fw(s.phase*1.003f))*0.5f;
        out = out*(0.65f+0.2f*body) + p3*0.15f;
        break;
    default: out = p;
    }
    // Softclip suave — nunca clipea duro
    return out / (1.f + fabsf(out)*0.5f);
}

// ── Voice ─────────────────────────────────────────────────────────
struct Voice {
    // BB
    uint8_t  bfa=2, bfb=10, bmorph=128, bseed=0;
    uint16_t brate=5;       // 4-8: tick rate ~5500-11000Hz (musical)
    float    bb_lp=0.f;     // LP simple
    float    bb_dc_x=0.f, bb_dc_y=0.f;  // DC blocker con fc=6Hz
    // FB
    FbState  fb;
    uint8_t  fb_algo=0;
    float    fb_hz=220.f;

    void randomize(){
        bfa   = (uint8_t)(rng_next()%17u);
        bfb   = (uint8_t)(rng_next()%17u);
        bmorph= (uint8_t)(rng_next()>>24);
        bseed = (uint8_t)(rng_next()>>24);
        static const uint16_t BR[]={4,5,6,7,8};
        brate = BR[rng_next()%5u];
        fb_algo = (uint8_t)(rng_next()%8u);
        fb_hz = 55.f*powf(2.f, rng_f()*4.f);  // 55-880Hz
        fb = FbState{};
        bb_lp=0.f; bb_dc_x=0.f; bb_dc_y=0.f;
    }

    // morph: 0=BB, 1=FB
    // macro1: pitch del FB (55-880Hz logarítmico)
    // macro2: body del FB + LP del BB
    float sample(uint32_t t, float morph, float macro1, float macro2){
        constexpr float DT = 1.f/44100.f;

        // ── BYTEBEAT ─────────────────────────────────────────────
        uint32_t ts = t / brate;
        uint8_t va = bbf(bfa, ts, bseed);
        uint8_t vb = bbf(bfb, ts^(uint32_t)(bseed*0x55u), bseed^0xA5u);
        uint8_t raw= (uint8_t)(((uint16_t)va*(255u-bmorph)+(uint16_t)vb*bmorph)>>8);

        // Convertir a float centrado en 0
        float bb = float((int8_t)(raw ^ 0x80u)) * (1.f/128.f);

        // LP — macro2 controla cutoff: 0→fc=400Hz, 1→fc=2000Hz
        // coeff = 2π×fc/SR
        float lp_c = 0.057f + macro2*0.228f;
        bb_lp += lp_c*(bb - bb_lp);
        bb = bb_lp;

        // DC blocker con fc=6Hz (coeff=0.9991)
        // Solo quita DC real, no toca el audio útil
        float bb_y = bb - bb_dc_x + 0.9991f*bb_dc_y;
        bb_dc_x = bb; bb_dc_y = bb_y;
        bb = bb_y;

        // ── FLOATBEAT ────────────────────────────────────────────
        // macro1 → pitch logarítmico 55-880Hz
        float hz = 55.f * powf(16.f, macro1);  // 55, 110, 220, 440, 880
        float body = macro2;  // body directo, sin slew

        float fb_out = floatbeat(fb, DT, hz, body, fb_algo);

        // ── MORPH ────────────────────────────────────────────────
        return bb*(1.f-morph) + fb_out*morph;
    }
};

// ── Synth: una voz activa, crossfade de 100ms al randomizar ──────
struct Synth {
    Voice cur, prv;
    float xfade = 1.f;

    void init(){ cur.randomize(); prv=cur; xfade=1.f; }

    void randomize(){ prv=cur; cur.randomize(); xfade=0.f; }

    int16_t next(uint32_t t, float morph, float m1, float m2){
        float s = cur.sample(t, morph, m1, m2);
        if(xfade < 1.f){
            float sp = prv.sample(t, morph, m1, m2);
            float f  = xfade*xfade*(3.f-2.f*xfade);  // smoothstep
            s = sp*(1.f-f) + s*f;
            xfade += 1.f/4410.f;  // 100ms
            if(xfade>1.f) xfade=1.f;
        }
        // Ganancia limpia
        s *= 0.85f;
        if(s > 0.95f)  s =  0.95f;
        if(s < -0.95f) s = -0.95f;
        return (int16_t)(s*32767.f);
    }
};

// ── Master bus: +6dB boost + softclip suave ──────────────────────
static inline int16_t master_bus(int32_t x){
    float s = float(x) * (2.f/32768.f);      // +6dB
    float sc = s / (1.f + fabsf(s)*0.5f);     // softclip suave
    sc = clampf(sc, -0.97f, 0.97f);
    return (int16_t)(sc*32767.f);
}

// ── Pads ─────────────────────────────────────────────────────────
static float    pad_baseline[4]={9300,9300,9300,9300};
static bool     pad_on[4]={},pad_prev[4]={};
static uint8_t  pad_conf_on[4]={}, pad_conf_off[4]={};

static uint32_t measure_pad_us(uint8_t c){
    gpio_set_dir(PIN_PAD[c],GPIO_OUT);
    gpio_put(PIN_PAD[c],1); sleep_us(300);
    gpio_put(PIN_PAD[c],0); sleep_us(800);
    gpio_set_dir(PIN_PAD[c],GPIO_IN);
    gpio_disable_pulls(PIN_PAD[c]);
    const uint32_t t0=time_us_32();
    while(!gpio_get(PIN_PAD[c]))
        if((time_us_32()-t0)>=12000u) return 12000u;
    return time_us_32()-t0;
}

static void calibrate_pads(){
    sleep_ms(300);
    for(uint8_t c=0;c<4;++c){
        uint32_t sum=0;
        for(int i=0;i<20;++i) sum+=measure_pad_us(c);
        pad_baseline[c]=float(sum)/20.f;
    }
}

static void scan_pads(){
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        float raw=float(measure_pad_us(c));
        bool touched=(raw > pad_baseline[c]*1.30f);
        if(!touched && !pad_on[c])
            pad_baseline[c]+=0.0005f*(raw-pad_baseline[c]);
        if(!pad_on[c]){
            if(touched){ if(++pad_conf_on[c]>=3) pad_on[c]=true; }
            else        pad_conf_on[c]=0;
        } else {
            if(!touched){ if(++pad_conf_off[c]>=2) pad_on[c]=false; }
            else          pad_conf_off[c]=0;
        }
    }
}

// ── Core1: control ───────────────────────────────────────────────
static float adc_read_ch(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]); gpio_set_dir(PIN_PAD[c],GPIO_IN);
        gpio_disable_pulls(PIN_PAD[c]);
    }

    adc_init();
    adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);

    // Entropía del ADC flotante
    uint32_t entropy=0;
    adc_select_input(0);
    for(int i=0;i<64;++i)
        entropy=entropy*1664525u+adc_read()+1013904223u;
    g_rng^=entropy;

    gpio_put(PIN_LED,1);
    calibrate_pads();
    for(int i=0;i<3;++i){
        gpio_put(PIN_LED,0);sleep_ms(100);
        gpio_put(PIN_LED,1);sleep_ms(100);
    }
    gpio_put(PIN_LED,0);
    g_ready=true;

    float sm=0.5f, sm1=0.5f;
    while(true){
        sm  += 0.15f*(adc_read_ch(0)-sm);  g_morph  = sm;
        sm1 += 0.15f*(adc_read_ch(1)-sm1); g_macro1 = sm1;
        g_macro2 = adc_read_ch(2);          // directo, sin slew

        scan_pads();
        if(pad_on[0]&&!pad_prev[0]){g_pad_event|=1u;gpio_put(PIN_LED,1);}
        if(!pad_on[0])              gpio_put(PIN_LED,0);
        if(pad_on[1]&&!pad_prev[1]) g_pad_event|=2u;
        if(pad_on[2]&&!pad_prev[2]) g_pad_event|=4u;
        if(pad_on[3]&&!pad_prev[3]) g_pad_event|=8u;
    }
}

// ── Core0: audio ─────────────────────────────────────────────────
int main(){
    const uint off=pio_add_program(g_pio,&pcm5102_i2s_program);
    g_sm=pio_claim_unused_sm(g_pio,true);
    pcm5102_i2s_program_init(g_pio,g_sm,off,PIN_DIN,PIN_BCLK,SR);
    for(int i=0;i<16;++i)i2s_write(0,0);

    static DrumEngine drums;
    drums.init(); drums.set_params(0.3f,0.5f,1.0f);

    static Synth synth;

    multicore_launch_core1(core1_main);
    while(!g_ready) sleep_ms(10);

    synth.init();  // después de que Core1 seede el RNG

    uint32_t t=0, cr=0;
    while(true){
        if(++cr>=32u){
            cr=0;
            const uint8_t ev=g_pad_event;
            if(ev){
                g_pad_event=0;
                if(ev&1u) synth.randomize();
                if(ev&2u) drums.trigger(DRUM_KICK);
                if(ev&4u) drums.trigger(DRUM_SNARE);
                if(ev&8u) drums.trigger(DRUM_HAT);
            }
            drums.set_params(g_macro2*0.6f+0.1f, 0.3f+g_macro2*0.5f, -1.f);
        }

        const int16_t ss = synth.next(t, g_morph, g_macro1, g_macro2);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd = ((int32_t)ss*sc)>>15;
        int32_t ol = ((sd*19661)>>15) + ((int32_t)dl*29491>>15);
        int32_t or_= ((sd*19661)>>15) + ((int32_t)dr*29491>>15);

        i2s_write(master_bus(ol), master_bus(or_));
        ++t;
    }
}

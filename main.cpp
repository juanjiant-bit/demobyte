// Dem0!Byt3 V11 — pads 100kΩ/100nF corregidos + motor random + pots extremos
//
// PADS: 3V3→100kΩ→GPIO, 100nF entre GPIO y GND
// Técnica correcta para este circuito:
//   1. OUTPUT LOW 500µs → descarga cap a 0V
//   2. INPUT (sin pull interno) → cap carga a través del 100kΩ externo
//   3. Medir tiempo hasta HIGH
//   Sin dedo: sube a HIGH en ~9.3ms (τ=10ms, 100kΩ×100nF)
//   Con dedo: V_ss = 3.3×R_skin/(100k+R_skin)
//     Si R_skin < 150kΩ: nunca llega a HIGH → TIMEOUT → TOQUE
//     Si R_skin > 150kΩ: llega a HIGH pero más lento → TOQUE
//   Threshold: 10.3ms (9.3ms baseline + 1ms margen)
//   Detecta hasta R_skin = 2MΩ (manos muy secas)
//
// POTS: sin slew, rango extremo
//   MORPH  (GP26): 0=voz A pura, 1=voz B pura
//   MACRO1 (GP27): transpone pitch en ±1.5 octavas sobre hz base de cada voz
//   MACRO2 (GP28): timbre — 0=seco/fórmula A, 1=complejo/fórmula B
//
// RANDOM: entropía del ADC en cada encendido

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

static PIO  g_pio=pio0; static uint g_sm=0;
static inline void i2s_write(int16_t l,int16_t r){
    pio_sm_put_blocking(g_pio,g_sm,(uint32_t)(uint16_t)l<<16);
    pio_sm_put_blocking(g_pio,g_sm,(uint32_t)(uint16_t)r<<16);
}

constexpr uint PIN_PAD[4]={8,9,13,14};
constexpr uint PIN_LED=25,PIN_BCLK=10,PIN_DIN=12,SR=44100;

static volatile float   g_morph=0.5f;   // GP26 — mezcla voz A↔B
static volatile float   g_filter=0.5f;  // GP27 — LP cutoff global
static volatile float   g_timbre=0.5f;  // GP28 — body/complejidad
static volatile uint8_t g_pad_event=0;
static volatile uint8_t g_pad_state=0;   // bitmask pads activos ahora (para feedback audio)
static volatile bool    g_ready=false;

static uint32_t g_rng=0xDEADBEEFu;
static inline uint32_t rng_next(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
static inline float rng_f(){return float(rng_next()>>8)*(1.f/16777215.f);}
static inline float clamp01(float x){return x<0?0:(x>1?1:x);}

// ── Osciladores ───────────────────────────────────────────────────
static inline float fw(float x){x-=(int)x;return x<0?x+1.f:x;}
static inline float fs(float p){
    float x=fw(p)*6.28318f-3.14159f,y=x*(1.2732f-0.4053f*fabsf(x));
    return y*(0.225f*(fabsf(y)-1.f)+1.f);
}
static inline float ft(float p){float x=fw(p);return 4.f*fabsf(x-.5f)-1.f;}
static inline float fsq(float p,float pw=.5f){return fw(p)<pw?1.f:-1.f;}
static inline float fclip(float x){return x/(1.f+fabsf(x));}
static inline float ffold(float x){x=x*.5f+.5f;x-=(int)x;if(x<0)x+=1.f;return x*2.f-1.f;}

// ── Bytebeat — 17 fórmulas clásicas ──────────────────────────────
static inline uint8_t bbf(uint8_t id,uint32_t t,uint8_t s){
    switch(id%17u){
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

// ── Floatbeat — 12 algos con rangos de pitch y timbre variados ───
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};
struct FbSt{float t=0,env=0,sph=0;uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    const float t=st.t;
    hz=hz<15?15:(hz>3000?3000:hz);
    body=clamp01(body);
    switch(algo%12u){
    case 0:{float x=fs(t*hz)*(.5f+.2f*body)+fs(t*hz*.5f)*(.28f+.18f*body)+fs(t*hz*1.5f)*.07f;return fclip(x*.85f);}
    case 1:{float x=fs(t*hz*.5f)*.55f+fs(t*hz)*.30f+ft(t*hz*2.f)*.09f*(1.f+body);return fclip(x*.9f);}
    case 2:{float a=fs(t*hz),b=fs(t*(hz*1.007f));return fclip(a*b*(.7f+.2f*body)+fs(t*hz*.5f)*.2f);}
    case 3:{st.env+=dt*4.f;if(st.env>1.f)st.env=0.f;float e=1.f-st.env;
            return fclip(fs(t*hz+fs(t*hz*2.f)*.3f*e)*e*(.8f+.15f*body));}
    case 4:{float pw=.1f+.4f*(fs(t*hz*.125f)*.5f+.5f);
            return fclip((fsq(t*hz,pw)*(.5f+.3f*body)+fs(t*hz*.5f)*.2f)*.75f);}
    case 5:{float gate=fsq(t*hz*.0625f,.35f)*.5f+.5f;
            return fclip((fs(t*hz)+ft(t*hz*2.01f)*.3f)*gate*(.7f+.2f*body));}
    case 6:{st.sph+=dt*hz*.03125f;uint8_t n=(uint8_t)(st.sph)%8u;
            return fclip((fs(t*hz*PENTA[n])*(.65f+.2f*body)+fs(t*hz*PENTA[n]*.5f)*.2f)*.85f);}
    case 7:{float ps=st.sph;st.sph+=dt*hz*.025f;
            if((uint8_t)st.sph!=(uint8_t)ps){st.lf^=st.lf<<7;st.lf^=st.lf>>9;}
            return fclip(fs(t*hz*PENTA[st.lf%8u])*(.75f+.15f*body));}
    case 8:{st.sph+=dt*hz*.015625f;uint8_t n=(uint8_t)(st.sph)%4u;
            return fclip((fs(t*hz*BASS[n])*(.62f+.22f*body)+fs(t*hz*BASS[n]*.5f)*.24f));}
    case 9:{float idx=2.f+body*6.f,mod=fs(t*(hz*1.618f))*idx;
            return fclip((fs(t*hz+mod)*.62f+fs(t*hz*.5f)*.18f)*.82f);}
    case 10:{float x=ft(t*hz)*(1.5f+body*2.5f);return fclip(ffold(x)*(.65f+.2f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5;
             float n=float((int32_t)st.lf)*(1.f/2147483648.f);
             return fclip(fs(t*hz)*(.3f+.4f*body)+n*(.18f-.1f*body));}
    default:return 0.f;
    }
}

// ── Voice — BB o FB, completamente aleatoria ──────────────────────
struct Voice {
    bool    is_fb=false;
    // BB
    uint8_t bfa=2,bfb=5,bmorph=128,bseed=0;
    uint16_t brate=1;
    // FB — fhz_base es la "nota" de esta voz, invariante al randomize
    FbSt    fst;
    uint8_t falgo=0;
    float   fhz_base=110.f;  // pitch base de esta voz — único por voz
    float   fbody_base=0.5f;
    // DSP
    float   lp=0.f; int32_t dcx=0,dcy=0; float fdcx=0,fdcy=0;

    void randomize(){
        is_fb = (rng_next()&1u)!=0;

        // BB completamente aleatorio
        bfa   = (uint8_t)(rng_next()%17u);
        bfb   = (uint8_t)(rng_next()%17u);
        bmorph= (uint8_t)(rng_next()>>24);
        // brate controla velocidad del bytebeat — valores musicales
        // 8=rápido(~5.5Hz patrón), 32=lento(~22Hz), 64=muy lento(~43Hz)
        static const uint16_t BRATES[]={4,6,8,12,16,24,32,48,64};
        brate = BRATES[rng_next()%9u];
        bseed = (uint8_t)(rng_next()>>24);

        // FB completamente aleatorio
        falgo      = (uint8_t)(rng_next()%12u);
        // fhz_base: nota aleatoria entre 40Hz y 800Hz
        // Escala logarítmica para distribución musical uniforme
        fhz_base   = 40.f * powf(20.f, rng_f());  // 40Hz → 800Hz
        fbody_base = rng_f();
        fst        = FbSt{};

        // Resetear DSP
        lp=0.f; dcx=0; dcy=0; fdcx=0; fdcy=0;
    }

    // macro1: transpone ±1.5 octavas sobre fhz_base (0=baja, 0.5=original, 1=alta)
    // macro2: timbre — 0=formula A/cuerpo simple, 1=formula B/cuerpo complejo
    float sample(uint32_t t, float domain, float timbre){
        constexpr float DT = 1.f/44100.f;

        // ── BB ────────────────────────────────────────────────────
        // macro1 afecta la velocidad del bytebeat (brate: lento↔rápido)
        // macro2 hace morph entre fórmula A y B
        const uint32_t ts = t / (brate ? brate : 1u);
        uint8_t va = bbf(bfa, ts, bseed);
        uint8_t vb = bbf(bfb, ts^(uint32_t)(bseed*0x55u), bseed^0xA5u);
        uint8_t m  = (uint8_t)(timbre*255.f);
        uint8_t raw = (uint8_t)(((uint16_t)va*(255u-m)+(uint16_t)vb*m)>>8);
        float bb = float((int8_t)(raw^0x80u))*(1.f/128.f);

        float lp_c = 0.08f;  // LP del BB fijo — el filter global controla el tono
        lp += lp_c*(bb-lp);
        bb = lp*0.60f + bb*0.40f;

        // DC blocker
        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
        bb=float(y)*(1.f/32767.f);

        // ── FB ────────────────────────────────────────────────────
        // macro1 transpone ±1.5 octavas: 0=fhz/2.83, 0.5=fhz, 1=fhz×2.83
        float hz = fhz_base;

        // macro2 controla body (0=simple, 1=complejo)
        float body = timbre;

        float fb = fbalgo(fst, DT, hz, body, falgo);
        float fy = fb-fdcx+fdcy*(252.f/256.f); fdcx=fb;fdcy=fy; fb=fy;

        // ── Morph: 0=BB puro, 1=FB puro ───────────────────────────
        return bb*(1.f-domain) + fb*domain;
    }
};

// ── Synth con doble voz + crossfade ──────────────────────────────
struct Synth {
    Voice va, vb;
    Voice old_a, old_b;
    float xfade=1.f;

    void randomize(){
        old_a=va; old_b=vb;
        va.randomize();
        vb.randomize();
        // Garantizar que las dos voces tengan pitches distintos
        // Si ambas FB tienen hz muy similares, transponer la voz B
        if(va.is_fb && vb.is_fb){
            float ratio = va.fhz_base / vb.fhz_base;
            if(ratio < 1.33f && ratio > 0.75f){
                // Muy similares — alejar por al menos una 4ª justa
                vb.fhz_base *= (rng_f()>0.5f) ? 1.5f : 0.667f;
            }
        }
        xfade=0.f;
    }

    int16_t next(uint32_t t, float morph){
        float sa = va.sample(t, morph, g_timbre);
        float sb = vb.sample(t, morph, g_timbre);
        float out = sa*(1.f-morph) + sb*morph;

        if(xfade<1.f){
            float f=xfade*xfade*(3.f-2.f*xfade);
            float sao=old_a.sample(t,morph,g_timbre);
            float sbo=old_b.sample(t,morph,g_timbre);
            float out_old=sao*(1.f-morph)+sbo*morph;
            out=out_old*(1.f-f)+out*f;
            xfade+=1.f/2205.f;  // 50ms
            if(xfade>1.f)xfade=1.f;
        }

        out*=0.82f;
        if(out>.92f)out=.92f; if(out<-.92f)out=-.92f;
        return(int16_t)(out*32767.f);
    }
};

// ── Pads resistivos — 100kΩ pullup a 3V3, 100nF a GND ───────────
// TÉCNICA: medir tiempo REAL de carga vs baseline calibrado
// Sin dedo: cap sube a HIGH en ~9.3ms (100kΩ×100nF)
// Con dedo: sube más lento o nunca llega → tiempo > baseline
// Threshold: 5% más lento que baseline = toque detectado
// Detecta desde R_skin=700kΩ (manos muy secas) hasta timeout

constexpr uint32_t PAD_CHARGE_US  = 300;   // carga inicial
constexpr uint32_t PAD_DISCH_US   = 800;   // descarga antes de medir
constexpr uint32_t PAD_TIMEOUT_US = 25000; // 25ms timeout máximo

static float   pad_baseline[4] = {9300,9300,9300,9300}; // µs, calibrado
static bool    pad_on[4]={},pad_prev[4]={};
static uint8_t pad_conf[4]={};

// Retorna tiempo de carga en µs (PAD_TIMEOUT_US si no llega a HIGH)
static uint32_t measure_pad_us(uint8_t c){
    // Cargar cap
    gpio_set_dir(PIN_PAD[c], GPIO_OUT);
    gpio_put(PIN_PAD[c], 1);
    sleep_us(PAD_CHARGE_US);
    // Descargar cap
    gpio_put(PIN_PAD[c], 0);
    sleep_us(PAD_DISCH_US);
    // Soltar — 100kΩ externo carga el cap
    gpio_set_dir(PIN_PAD[c], GPIO_IN);
    gpio_disable_pulls(PIN_PAD[c]);
    // Medir tiempo hasta HIGH
    const uint32_t t0 = time_us_32();
    while(!gpio_get(PIN_PAD[c])){
        if((time_us_32()-t0) >= PAD_TIMEOUT_US)
            return PAD_TIMEOUT_US;
    }
    return time_us_32()-t0;
}

static void calibrate_pads(){
    // Medir baseline sin dedo: promedio de 20 lecturas
    sleep_ms(200);
    for(uint8_t c=0;c<4;++c){
        uint32_t sum=0;
        for(int i=0;i<20;++i) sum+=measure_pad_us(c);
        pad_baseline[c] = float(sum)/20.f;
    }
}

static void scan_pads(){
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        uint32_t t = measure_pad_us(c);
        // Toque si: tiempo > baseline×1.05 (5% más lento)
        // O si timeout (nunca llegó a HIGH)
        bool touched = (float(t) > pad_baseline[c]*1.05f);
        // Actualizar baseline lentamente cuando no hay toque
        if(!touched && !pad_on[c])
            pad_baseline[c] += 0.005f*(float(t)-pad_baseline[c]);
        if(!pad_on[c]){
            if(touched){if(++pad_conf[c]>=2)pad_on[c]=true;}
            else pad_conf[c]=0;
        } else {
            if(!touched){pad_on[c]=false;pad_conf[c]=0;}
        }
    }
}

static void scan_pads(){
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        bool touched=measure_pad_touched(c);
        if(!pad_on[c]){
            if(touched){if(++pad_conf[c]>=2)pad_on[c]=true;}
            else pad_conf[c]=0;
        } else {
            if(!touched){pad_on[c]=false;pad_conf[c]=0;}
        }
    }
}

static float adc_direct(uint ch){
    adc_select_input(ch);
    return float(adc_read())/4095.f;
}

static void core1_main(){
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]);
        gpio_set_dir(PIN_PAD[c],GPIO_IN);
        gpio_disable_pulls(PIN_PAD[c]);
    }
    adc_init();
    adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);

    // Triple parpadeo = listo
    for(int i=0;i<3;++i){
        gpio_put(PIN_LED,1);sleep_ms(120);
        gpio_put(PIN_LED,0);sleep_ms(120);
    }
    g_ready=true;

    while(true){
        // Sin slew — lectura directa, respuesta inmediata
        g_morph  = adc_direct(0);  // GP26 MORPH
        g_filter = adc_direct(1);  // GP27 FILTER
        g_timbre = adc_direct(2);  // GP28 TIMBRE

        scan_pads();

        // Actualizar estado de pads para feedback de audio
        uint8_t pstate=0;
        for(uint8_t i=0;i<4;++i) if(pad_on[i]) pstate|=(1u<<i);
        g_pad_state=pstate;

        if(pad_on[0]&&!pad_prev[0]){g_pad_event|=1u;gpio_put(PIN_LED,1);}
        if(!pad_on[0])              gpio_put(PIN_LED,0);
        if(pad_on[1]&&!pad_prev[1]) g_pad_event|=2u;
        if(pad_on[2]&&!pad_prev[2]) g_pad_event|=4u;
        if(pad_on[3]&&!pad_prev[3]) g_pad_event|=8u;
    }
}

int main(){
    const uint off=pio_add_program(g_pio,&pcm5102_i2s_program);
    g_sm=pio_claim_unused_sm(g_pio,true);
    pcm5102_i2s_program_init(g_pio,g_sm,off,PIN_DIN,PIN_BCLK,SR);
    for(int i=0;i<16;++i)i2s_write(0,0);

    static DrumEngine drums;
    drums.init(); drums.set_params(0.3f,0.5f,1.0f);

    // Entropía real: ruido térmico del ADC (bits bajos son ruido)
    adc_init(); adc_gpio_init(26);
    uint32_t entropy=0;
    for(int i=0;i<64;++i){
        adc_select_input(0);
        entropy=entropy*1664525u+adc_read()+1013904223u;
    }
    g_rng^=entropy;

    static Synth synth;
    static float lp_l=0.f, lp_r=0.f;  // LP global (filter pot)
    synth.va.randomize();
    synth.vb.randomize();
    // Garantizar pitches distintos en FB
    if(synth.va.is_fb && synth.vb.is_fb){
        float r=synth.va.fhz_base/synth.vb.fhz_base;
        if(r<1.33f&&r>0.75f) synth.vb.fhz_base*=1.5f;
    }
    synth.old_a=synth.va; synth.old_b=synth.vb;
    synth.xfade=1.f;

    multicore_launch_core1(core1_main);
    while(!g_ready)sleep_ms(10);

    uint32_t t=0,cr=0;
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
            drums.set_params(
                clamp01(g_timbre*.8f+.1f),   // drum color
                clamp01(g_timbre*.6f+.2f),   // drum decay
                -1.f);
        }

        const int16_t ss=synth.next(t,g_morph);
        // Feedback de pad: mezcla un tono suave cuando hay toque activo
        // PAD0=pad1=440Hz, PAD1=kick confirm, etc — audible sobre cualquier synth
        int16_t pad_fb=0;
        if(g_pad_state){
            // Pulso de confirmación: tono de 880Hz mezclado al 15%
            pad_fb=(int16_t)(((t%50)<25)?3000:-3000);
        }
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd=((int32_t)(ss+pad_fb)*sc)>>15;
        int32_t ol=sd+(int32_t)dl;
        int32_t or_=sd+(int32_t)dr;
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;
        // FILTER: LP global — coeff cuadrático para respuesta natural
        // filter=0.0 → fc≈20Hz (muy oscuro), filter=1.0 → sin filtro
        {
            float fc=g_filter*g_filter*0.45f+0.001f;
            lp_l+=fc*(float(ol)-lp_l); ol=(int32_t)lp_l;
            lp_r+=fc*(float(or_)-lp_r); or_=(int32_t)lp_r;
        }
        i2s_write((int16_t)ol,(int16_t)or_);
        ++t;
    }
}

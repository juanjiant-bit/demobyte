// Dem0!Byt3 V11 — pots directos + macros absolutos + pad fix circuito

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

// Sin slew — lectura directa del ADC, sin filtro
static volatile float   g_morph=0.5f,g_macro1=0.5f,g_macro2=0.5f;
static volatile uint8_t g_pad_event=0;
static volatile bool    g_ready=false;

static uint32_t g_rng=0xDEADBEEFu;
static inline uint32_t rng_next(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
static inline float rng_f(){return float(rng_next()>>8)*(1.f/16777215.f);}
static inline float clamp01(float x){return x<0?0:(x>1?1:x);}

// ── Osciladores ───────────────────────────────────────────────────
static inline float fw(float x){x-=(int)x;return x<0?x+1.f:x;}
static inline float fs(float p){float x=fw(p)*6.28318f-3.14159f,y=x*(1.2732f-0.4053f*fabsf(x));return y*(0.225f*(fabsf(y)-1.f)+1.f);}
static inline float ft(float p){float x=fw(p);return 4.f*fabsf(x-.5f)-1.f;}
static inline float fsq(float p,float pw=.5f){return fw(p)<pw?1.f:-1.f;}
static inline float fclip(float x){return x/(1.f+fabsf(x));}
static inline float ffold(float x){x=x*.5f+.5f;x-=(int)x;if(x<0)x+=1.f;return x*2.f-1.f;}

// ── Bytebeat 17 fórmulas ─────────────────────────────────────────
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

// ── Floatbeat 12 algos ────────────────────────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};
struct FbSt{float t=0,env=0,sph=0;uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    const float t=st.t;
    hz=hz<20?20:(hz>3000?3000:hz);
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

// ── Voice — BB o FB, elegida aleatoriamente en cada randomize ────
// is_fb determina el tipo — completamente aleatorio
struct Voice {
    bool    is_fb=false;
    // BB
    uint8_t bfa=2,bfb=10,bmorph=128,bseed=0;
    uint16_t brate=1;
    // FB
    FbSt    fst;
    uint8_t falgo=0;
    float   fhz_base=110.f, fbody_base=.5f;
    // DSP
    float   lp=0.f; int32_t dcx=0,dcy=0; float fdcx=0,fdcy=0;

    void randomize(){
        is_fb=(rng_next()&1u)!=0;      // 50% BB, 50% FB — completamente aleatorio
        bfa  =(uint8_t)(rng_next()%17u);
        bfb  =(uint8_t)(rng_next()%17u);
        bmorph=(uint8_t)(rng_next()>>24);
        brate=(uint16_t)(1u+rng_next()%4u);  // 1-4
        bseed=(uint8_t)(rng_next()>>24);
        falgo=(uint8_t)(rng_next()%12u);
        fhz_base=55.f*powf(2.f,rng_f()*4.f);  // 55-880Hz
        fbody_base=rng_f();
        fst=FbSt{};
        lp=0.f; dcx=0; dcy=0; fdcx=0; fdcy=0;
    }

    // macro1: 0=subgrave(27Hz) → 1=agudo(2000Hz) — RANGO ABSOLUTO
    // macro2: 0=seco/simple    → 1=lleno/complejo
    float sample(uint32_t t, float domain, float macro1, float macro2){
        constexpr float DT=1.f/44100.f;

        // ── Macro1 → hz ABSOLUTA (no relativa) ───────────────────
        // 27Hz a 2000Hz en escala logarítmica
        float hz = 27.f * powf(74.f, macro1);  // 27Hz → 2000Hz

        // ── Macro2 → body/complejidad ABSOLUTO ────────────────────
        float body = macro2;

        // ── BB ────────────────────────────────────────────────────
        const uint32_t ts = t / (brate ? brate : 1u);
        uint8_t va = bbf(bfa, ts, bseed);
        uint8_t vb = bbf(bfb, ts^(uint32_t)(bseed*0x55u), bseed^0xA5u);
        // macro2 mueve el morph BB: izq=fórmula A, der=fórmula B
        uint8_t m = (uint8_t)(macro2*255.f);
        uint8_t raw = (uint8_t)(((uint16_t)va*(255u-m)+(uint16_t)vb*m)>>8);
        float bb = float((int8_t)(raw^0x80u))*(1.f/128.f);

        // LP del BB: macro1 controla cutoff (grave=cerrado, agudo=abierto)
        float lp_c = 0.02f + macro1*0.14f;
        lp += lp_c*(bb-lp);
        bb = lp*.65f + bb*.35f;

        // DC blocker BB
        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
        bb=float(y)*(1.f/32767.f);

        // ── FB ────────────────────────────────────────────────────
        float fb = fbalgo(fst, DT, hz, body, falgo);
        float fy = fb-fdcx+fdcy*(252.f/256.f); fdcx=fb;fdcy=fy; fb=fy;

        // ── Morph: 0=BB, 1=FB ─────────────────────────────────────
        return bb*(1.f-domain) + fb*domain;
    }
};

// ── Synth — doble voz con crossfade ──────────────────────────────
struct Synth {
    Voice va, vb;
    Voice old_a, old_b;
    float xfade=1.f;

    void randomize(){
        old_a=va; old_b=vb;
        va.randomize(); vb.randomize();
        xfade=0.f;
    }

    int16_t next(uint32_t t, float morph, float macro1, float macro2){
        float sa = va.sample(t, morph, macro1, macro2);
        float sb = vb.sample(t, morph, macro1, macro2);
        float out = (sa+sb)*0.5f;

        if(xfade<1.f){
            float f=xfade*xfade*(3.f-2.f*xfade);
            float sao=old_a.sample(t,morph,macro1,macro2);
            float sbo=old_b.sample(t,morph,macro1,macro2);
            float out_old=(sao+sbo)*0.5f;
            out=out_old*(1.f-f)+out*f;
            xfade+=1.f/2205.f;
            if(xfade>1.f)xfade=1.f;
        }

        out*=0.82f;
        if(out>.92f)out=.92f; if(out<-.92f)out=-.92f;
        return(int16_t)(out*32767.f);
    }
};

// ── Pads resistivos ───────────────────────────────────────────────
// Circuito: 3V3→10kΩ→GPIO, 100nF entre GPIO y GND
// El dedo conecta GPIO a GND a través de R_piel
//
// TÉCNICA CORREGIDA:
// 1. OUTPUT HIGH: carga el cap a 3V3 (5ms = 5×RC para 10kΩ×100nF)
// 2. OUTPUT LOW: el cap descarga a través del pullup externo 10kΩ
//    (no INPUT — así el pullup no compite con la descarga)
//    Con dedo: cap descarga TAMBIÉN a través de R_piel → más rápido
// 3. Cuando el cap llega a ~0.8V (threshold LOW), el GPIO detecta LOW
//    Esto solo pasa cuando hay dedo (R_piel en paralelo con el pullup)
//
// NOTA: sin dedo el cap descarga solo a través del pullup externo
// que está conectado a 3V3 — así que el cap NO descarga, queda HIGH
// Con dedo R_piel tira a GND y el cap descarga
//
// TÉCNICA FINAL CORRECTA para este circuito:
// Sin dedo: 10kΩ a 3V3, cap se mantiene HIGH → gpio_get()=HIGH siempre
// Con dedo: R_piel a GND en paralelo → divisor de tensión
//   V_pin = 3.3 × R_piel / (10kΩ + R_piel)
//   R_piel=10kΩ → V=1.65V → puede ser HIGH o LOW (zona gris)
//   R_piel=1kΩ → V=0.3V → LOW ✓
//   R_piel=100Ω → V=0.03V → LOW ✓
// → El GPIO leerá LOW cuando R_piel < ~5kΩ (toque firme)
// → LECTURA DIGITAL SIMPLE: gpio_get()==LOW → pad tocado

static bool    pad_on[4]={},pad_prev[4]={};
static uint8_t pad_conf[4]={};

static bool read_pad(uint8_t c){
    // Con el circuito 10kΩ a 3V3 + 100nF a GND:
    // Sin dedo: pin HIGH (pullup domina)
    // Con dedo fuerte: pin LOW (R_piel shuntea a GND)
    // El cap filtra ruido pero no afecta la lectura estática
    gpio_set_dir(PIN_PAD[c], GPIO_IN);
    gpio_disable_pulls(PIN_PAD[c]);  // usar solo el pullup externo
    sleep_us(100);  // breve settle
    return !gpio_get(PIN_PAD[c]);   // LOW = tocado → retorna true
}

static void scan_pads(){
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        bool touched=read_pad(c);
        if(!pad_on[c]){
            if(touched){if(++pad_conf[c]>=3)pad_on[c]=true;}
            else pad_conf[c]=0;
        } else {
            if(!touched){pad_on[c]=false;pad_conf[c]=0;}
        }
    }
}

static float adc_read_direct(uint ch){
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

    // Parpadeo de inicio
    for(int i=0;i<3;++i){
        gpio_put(PIN_LED,1);sleep_ms(100);
        gpio_put(PIN_LED,0);sleep_ms(100);
    }
    g_ready=true;

    while(true){
        // Sin slew — lectura directa
        g_morph  = adc_read_direct(0);
        g_macro1 = adc_read_direct(1);
        g_macro2 = adc_read_direct(2);

        scan_pads();
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

    // Entropía del ADC flotante antes de init completo
    adc_init(); adc_gpio_init(26);
    uint32_t entropy=0;
    for(int i=0;i<64;++i){
        adc_select_input(0);
        entropy=entropy*1664525u+adc_read()+1013904223u;
    }
    g_rng^=entropy^0xDEAD1234u;

    static Synth synth;
    synth.va.randomize(); synth.vb.randomize();
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
                g_macro1*.7f+.1f,
                g_macro2*.7f+.1f,
                -1.f
            );
        }

        const int16_t ss=synth.next(t,g_morph,g_macro1,g_macro2);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd=((int32_t)ss*sc)>>15;
        int32_t ol=sd+(int32_t)dl;
        int32_t or_=sd+(int32_t)dr;
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;
        i2s_write((int16_t)ol,(int16_t)or_);
        ++t;
    }
}

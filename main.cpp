// Dem0!Byt3 V15 — arquitectura limpia, una voz, controles directos
//
// DIAGNÓSTICO DE ITERACIONES ANTERIORES:
//   V14 bug: BRATE 256-2048 → t_bb avanza 10 Hz → DC blocker = silencio
//   V13 bug: mix sin escalar → hard clip → ruido
//   V12 bug: doble adc_init() → ADC corrupto → chirrido
//   SOLUCIÓN: brate 4-8 (audio rate correcto), LP 800Hz, sin slew en pots
//
// ARQUITECTURA: 3 capas separadas
//   CONTROL:  core1 — ADC + pads → variables volátiles
//   SYNTH:    core0 — una voz, MORPH/MACRO1/MACRO2 directos, sin slew
//   OUTPUT:   core0 — mix, boost, softclip
//
// POTS:
//   GP26 MORPH  — blend BB↔FB (izq=bytebeat, der=floatbeat)
//   GP27 MACRO1 — PITCH floatbeat (55-880Hz) + bmorph bytebeat (timbre)
//   GP28 MACRO2 — BODY floatbeat (0=simple, 1=complejo) + LP BB (grave↔brillante)
//                 SIN SLEW — respuesta directa
//
// PADS: threshold calibrado, sin hold mínimo (permite retriggers rápidos)

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

static volatile float   g_morph=0.5f;   // GP26
static volatile float   g_macro1=0.5f;  // GP27 — pitch + bmorph
static volatile float   g_macro2=0.5f;  // GP28 — body + LP (SIN SLEW)
static volatile uint8_t g_pad_event=0;
static volatile bool    g_ready=false;

static uint32_t g_rng=0xDEADBEEFu;
static inline uint32_t rng_next(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
static inline float rng_f(){return float(rng_next()>>8)*(1.f/16777215.f);}
static inline float clampf(float x,float lo,float hi){return x<lo?lo:(x>hi?hi:x);}

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

// ── Bytebeat 17 fórmulas ─────────────────────────────────────────
static inline uint8_t bbf(uint8_t id,uint32_t t,uint8_t s){
    switch(id%17u){
    case 0:return(uint8_t)(t*((((t>>10)&42u)&0xFFu)?(((t>>10)&42u)&0xFFu):1u));
    case 1:return(uint8_t)(t*((((t>>9)^(t>>11))&28u)+4u));
    case 2:return(uint8_t)(t*((((t>>8)&15u)^((t>>11)&7u))+3u));
    case 3:return(uint8_t)(t*((((t>>10)&5u)|((t>>13)&2u))+2u));
    case 4:return(uint8_t)(t&(t>>8));
    case 5:return(uint8_t)(((t*5u)&(t>>7))|((t*3u)&(t>>10)));
    case 6:return(uint8_t)(((t>>6)|(t*3u))&((t>>9)|(t*5u)));
    case 7:return(uint8_t)(((t>>5)&(t>>8))|((t>>3)&(t*2u)));
    case 8:return(uint8_t)(((t>>4)&(t>>7))*((255u-(t>>6))&255u));
    case 9:return(uint8_t)(((t*(9u+(s&1u)))&(t>>4))^((t*(5u+((s>>1)&1u)))&(t>>7)));
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

// ── Floatbeat 12 algos ───────────────────────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};
struct FbSt{float t=0,env=0,sph=0,drift=0; uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    // Drift lento ±6% — da vida sin vibrato obvio
    st.drift+=dt*0.13f; if(st.drift>6.28318f)st.drift-=6.28318f;
    float hzl=hz*(1.f+fs(st.drift*0.15f)*0.06f);
    hzl=clampf(hzl,15.f,2200.f); body=clampf(body,0.f,1.f);
    const float t=st.t;
    switch(algo%12u){
    case 0:{float x=fs(t*hzl)*(.5f+.2f*body)+fs(t*hzl*.5f)*(.28f+.18f*body)+fs(t*hzl*1.5f)*.07f;return fclip(x*.85f);}
    case 1:{float x=fs(t*hzl*.5f)*.55f+fs(t*hzl)*.28f+ft(t*hzl*2.f)*.09f*(1.f+body);return fclip(x*.9f);}
    case 2:{float a=fs(t*hzl),b=fs(t*(hzl*1.007f));return fclip(a*b*(.7f+.2f*body)+fs(t*hzl*.5f)*.2f);}
    case 3:{st.env+=dt*4.f;if(st.env>1.f)st.env=0.f;float e=1.f-st.env;
            return fclip(fs(t*hzl+fs(t*hzl*2.f)*.3f*e)*e*(.8f+.15f*body));}
    case 4:{float pw=.1f+.4f*(fs(t*hzl*.125f)*.5f+.5f);
            return fclip((fsq(t*hzl,pw)*(.5f+.3f*body)+fs(t*hzl*.5f)*.2f)*.75f);}
    case 5:{float gate=fsq(t*hzl*.0625f,.35f)*.5f+.5f;
            return fclip((fs(t*hzl)+ft(t*hzl*2.01f)*.3f)*gate*(.7f+.2f*body));}
    case 6:{st.sph+=dt*hzl*.03125f;uint8_t n=(uint8_t)(st.sph)%8u;
            return fclip((fs(t*hzl*PENTA[n])*(.65f+.2f*body)+fs(t*hzl*PENTA[n]*.5f)*.2f)*.85f);}
    case 7:{float ps=st.sph;st.sph+=dt*hzl*.025f;
            if((uint8_t)st.sph!=(uint8_t)ps){st.lf^=st.lf<<7;st.lf^=st.lf>>9;}
            return fclip(fs(t*hzl*PENTA[st.lf%8u])*(.75f+.15f*body));}
    case 8:{st.sph+=dt*hzl*.015625f;uint8_t n=(uint8_t)(st.sph)%4u;
            return fclip((fs(t*hzl*BASS[n])*(.62f+.22f*body)+fs(t*hzl*BASS[n]*.5f)*.24f));}
    case 9:{float idx=1.5f+body*4.f,mod=fs(t*(hzl*1.618f))*idx;
            return fclip((fs(t*hzl+mod)*.65f+fs(t*hzl*.5f)*.18f)*.82f);}
    case 10:{float x=ft(t*hzl)*(1.2f+body*2.f);return fclip(ffold(x)*(.7f+.18f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5;
             float n=float((int32_t)st.lf)*(1.f/2147483648.f)*.3f;
             return fclip(fs(t*hzl)*(.4f+.3f*body)+n*(1.f-body*.5f));}
    default:return 0.f;
    }
}

// ── Voice ─────────────────────────────────────────────────────────
struct Voice{
    uint8_t  bfa=2, bfb=7, bmorph=128, bseed=0;
    uint16_t brate=5;    // 4-8: audio rate correcto (≈8000-11000 Hz tick rate)
    FbSt     fst;
    uint8_t  falgo=0;
    float    fhz=110.f, fbody=.5f;
    float    lp=0.f;     // LP simple para BB
    int32_t  dcx=0,dcy=0;
    float    fdcx=0,fdcy=0;

    void randomize(){
        bfa   =(uint8_t)(rng_next()%17u);
        bfb   =(uint8_t)(rng_next()%17u);
        bmorph=(uint8_t)(rng_next()>>24);
        // brate 4-8: rango que suena musical con las fórmulas clásicas
        static const uint16_t BR[]={4,5,6,7,8};
        brate =BR[rng_next()%5u];
        bseed =(uint8_t)(rng_next()>>24);
        falgo =(uint8_t)(rng_next()%12u);
        fhz   =55.f*powf(2.f,rng_f()*4.f);
        fbody =.2f+rng_f()*.6f;
        fst   =FbSt{};
        lp=0.f; dcx=0;dcy=0;fdcx=0;fdcy=0;
    }

    float sample(uint32_t t, float morph, float macro1, float macro2){
        constexpr float DT=1.f/44100.f;

        // ── BYTEBEAT ─────────────────────────────────────────────
        // MACRO1 afecta bmorph: timbre interno de las dos fórmulas BB
        uint8_t eff_bmorph=(uint8_t)(bmorph*(1.f-macro1*0.9f) + macro1*0.9f*255.f);
        const uint32_t ts=t/(brate?brate:1u);
        uint8_t va=bbf(bfa,ts,bseed);
        uint8_t vb=bbf(bfb,ts^(uint32_t)(bseed*0x55u),bseed^0xA5u);
        uint8_t raw=(uint8_t)(((uint16_t)va*(255u-eff_bmorph)+(uint16_t)vb*eff_bmorph)>>8);
        float bb=float((int8_t)(raw^0x80u))*(1.f/128.f);

        // LP con cutoff controlado por MACRO2 (0=400Hz, 1=2000Hz)
        // Esto tame el aliasing sin matar el sonido
        float fc=0.057f+macro2*0.228f;  // 400Hz→2000Hz
        lp+=fc*(bb-lp);
        bb=lp;

        // DC blocker
        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
        bb=float(y)*(1.f/32767.f);

        // ── FLOATBEAT ────────────────────────────────────────────
        // MACRO1 transpone pitch: 0=55Hz, 0.5=fhz propio, 1=880Hz
        // Mapeo logarítmico para respuesta musical uniforme
        float hz_use=55.f*powf(16.f,macro1);  // 55→880Hz en toda la escala
        hz_use=clampf(hz_use,15.f,3000.f);

        // MACRO2 controla body directo — SIN SLEW
        float body_use=macro2;

        float fb=fbalgo(fst,DT,hz_use,body_use,falgo);
        float fy=fb-fdcx+fdcy*(252.f/256.f); fdcx=fb;fdcy=fy; fb=fy;

        // MORPH: blend directo BB↔FB
        return bb*(1.f-morph)+fb*morph;
    }
};

// ── Synth: una voz activa + crossfade al randomizar ──────────────
struct Synth{
    Voice cur, prv;
    float xfade=1.f;

    void init(){cur.randomize(); prv=cur; xfade=1.f;}

    void randomize(){prv=cur; cur.randomize(); xfade=0.f;}

    int16_t next(uint32_t t, float morph, float m1, float m2){
        float s_cur=cur.sample(t,morph,m1,m2);
        float out;
        if(xfade<1.f){
            float f=xfade*xfade*(3.f-2.f*xfade);
            out=prv.sample(t,morph,m1,m2)*(1.f-f)+s_cur*f;
            xfade+=1.f/4410.f;  // 100ms crossfade
            if(xfade>1.f)xfade=1.f;
        } else {
            out=s_cur;
        }
        // Ganancia conservadora — master bus hace el boost
        out*=0.80f;
        if(out> .92f)out= .92f;
        if(out<-.92f)out=-.92f;
        return(int16_t)(out*32767.f);
    }
};

// ── Master bus ────────────────────────────────────────────────────
// +6dB boost + softclip suave — señal fuerte sin distorsión digital
static inline int16_t master_bus(int32_t x){
    float s=float(x)*(2.0f/32768.f);         // ×2 (+6dB)
    float sc=s/(1.f+fabsf(s)*0.55f);          // softclip
    sc=clampf(sc,-0.97f,0.97f);
    return(int16_t)(sc*32767.f);
}

// ── Pads ─────────────────────────────────────────────────────────
static float    pad_baseline[4]={9300,9300,9300,9300};
static bool     pad_on[4]={},pad_prev[4]={};
static uint8_t  pad_conf_on[4]={},pad_conf_off[4]={};

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
        uint32_t raw=measure_pad_us(c);
        bool touched=(float(raw)>pad_baseline[c]*1.30f);
        // Baseline drift muy lento solo cuando está libre
        if(!touched && !pad_on[c])
            pad_baseline[c]+=0.0005f*(float(raw)-pad_baseline[c]);
        if(!pad_on[c]){
            // ON: necesita 3 lecturas consecutivas (filtra spikes)
            if(touched){if(++pad_conf_on[c]>=3)  pad_on[c]=true;}
            else{pad_conf_on[c]=0;}
        } else {
            // OFF: necesita 2 lecturas consecutivas sin toque
            // SIN hold mínimo — permite retriggers rápidos
            if(!touched){if(++pad_conf_off[c]>=2) pad_on[c]=false;}
            else{pad_conf_off[c]=0;}
        }
    }
}

// ── Core1: control layer ─────────────────────────────────────────
static float adc_read_ch(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]);gpio_set_dir(PIN_PAD[c],GPIO_IN);
        gpio_disable_pulls(PIN_PAD[c]);
    }

    // ADC — una sola vez, aquí
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

    // Smoothing mínimo — solo para evitar zipper noise del ADC
    // MACRO2 sin slew (directo) — el usuario lo pidió
    float sm=0.5f, sm1=0.5f;
    while(true){
        sm  +=0.15f*(adc_read_ch(0)-sm);   g_morph  = sm;
        sm1 +=0.15f*(adc_read_ch(1)-sm1);  g_macro1 = sm1;
        g_macro2 = adc_read_ch(2);          // DIRECTO, sin slew

        scan_pads();
        if(pad_on[0]&&!pad_prev[0]){g_pad_event|=1u;gpio_put(PIN_LED,1);}
        if(!pad_on[0])              gpio_put(PIN_LED,0);
        if(pad_on[1]&&!pad_prev[1]) g_pad_event|=2u;
        if(pad_on[2]&&!pad_prev[2]) g_pad_event|=4u;
        if(pad_on[3]&&!pad_prev[3]) g_pad_event|=8u;
    }
}

// ── Core0: audio layer ───────────────────────────────────────────
int main(){
    const uint off=pio_add_program(g_pio,&pcm5102_i2s_program);
    g_sm=pio_claim_unused_sm(g_pio,true);
    pcm5102_i2s_program_init(g_pio,g_sm,off,PIN_DIN,PIN_BCLK,SR);
    for(int i=0;i<16;++i)i2s_write(0,0);

    static DrumEngine drums;
    drums.init(); drums.set_params(0.3f,0.5f,1.0f);

    static Synth synth;

    multicore_launch_core1(core1_main);
    while(!g_ready)sleep_ms(10);

    // Randomizar DESPUÉS de que Core1 seede el RNG
    synth.init();

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
            // Drums: macro2 controla color y decay
            drums.set_params(
                g_macro2*0.6f+0.1f,
                0.3f+g_macro2*0.5f,
                -1.f);
        }

        const int16_t ss=synth.next(t, g_morph, g_macro1, g_macro2);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        // Sidechain + mix V6 (synth×60% + drums×90%)
        const int32_t sd=((int32_t)ss*sc)>>15;
        int32_t ol=((sd*19661)>>15)+((int32_t)dl*29491>>15);
        int32_t or_=((sd*19661)>>15)+((int32_t)dr*29491>>15);

        // Master bus: +6dB boost + softclip
        i2s_write(master_bus(ol), master_bus(or_));
        ++t;
    }
}

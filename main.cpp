// Dem0!Byt3 V14 — motor controlable: BB a tasa reducida + antialiasing real
//
// PROBLEMA RESUELTO:
//   BB con brate=1..3 corre a 44100Hz → armónicos hasta Nyquist → ruido digital
//   SOLUCIÓN: BRATE 256..2048 (tasa reducida) + LP doble cascada como antialiasing
//
// POTS:
//   GP26 MORPH  — 0=BB puro, 0.5=híbrido, 1=FB puro
//   GP27 MACRO1 — PITCH: transpone fhz ÷4..×4 en tiempo real (4 octavas)
//   GP28 MACRO2 — RATE+DENSITY: BRATE lento↔rápido + LP abierto↔cerrado
//
// PADS: 100kΩ pullup + 100nF a GND, threshold 35%, hold 300ms

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

// ── Floatbeat 12 algos con drift ─────────────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};
struct FbSt{float t=0,env=0,sph=0,drift=0; uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    st.drift+=dt*0.13f; if(st.drift>6.28318f)st.drift-=6.28318f;
    float hzl=hz*(1.f+fs(st.drift*0.15f)*0.06f);
    hzl=hzl<15?15:(hzl>2200?2200:hzl);
    body=clampf(body,0,1);
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
    uint32_t brate=512;   // BRATE alto → tasa reducida → secuencias perceptibles
    FbSt     fst;
    uint8_t  falgo=0;
    float    fhz=110.f, fbody=.5f;
    // LP doble para BB — antialiasing real
    float    lp1=0.f, lp2=0.f;
    // DC blocker
    int32_t  dcx=0, dcy=0;
    float    fdcx=0, fdcy=0;
    // fhz suavizada para evitar clicks al cambiar macro1
    float    fhz_s=110.f;

    void randomize(){
        bfa   =(uint8_t)(rng_next()%17u);
        bfb   =(uint8_t)(rng_next()%17u);
        bmorph=(uint8_t)(rng_next()>>24);
        // BRATE tabla: tasa reducida para secuencias musicales
        // 256=rápido(~170 ticks/s), 512=medio, 1024=lento, 2048=muy lento
        static const uint32_t BR[]={256,384,512,768,1024,1536,2048};
        brate =BR[rng_next()%7u];
        bseed =(uint8_t)(rng_next()>>24);
        falgo =(uint8_t)(rng_next()%12u);
        fhz   =55.f*powf(2.f,rng_f()*4.f);
        fhz_s =fhz;
        fbody =.2f+rng_f()*.6f;
        fst   =FbSt{};
        lp1=0.f; lp2=0.f;
        dcx=0; dcy=0; fdcx=0; fdcy=0;
    }

    // macro1: pitch ÷4..×4  macro2: rate slow↔fast + LP open↔closed
    float sample(uint32_t master_t, float morph, float macro1, float macro2){
        constexpr float DT=1.f/44100.f;

        // ── BYTEBEAT ─────────────────────────────────────────────
        // MACRO2 escala el BRATE: 0=×4 (más lento), 1=÷4 (más rápido)
        float rate_scale = 4.f - macro2*3.75f;  // 4.0→0.25
        uint32_t eff_rate = (uint32_t)(float(brate)*rate_scale);
        if(eff_rate<64) eff_rate=64;
        const uint32_t t_bb = master_t / eff_rate;
        uint8_t va=bbf(bfa, t_bb, bseed);
        uint8_t vb=bbf(bfb, t_bb^(uint32_t)(bseed*0x55u), bseed^0xA5u);
        uint8_t raw=(uint8_t)(((uint16_t)va*(255u-bmorph)+(uint16_t)vb*bmorph)>>8);
        float bb=float((int8_t)(raw^0x80u))*(1.f/128.f);

        // LP doble cascada — MACRO2 controla apertura (0=200Hz, 1=1600Hz)
        float fc = 0.029f + macro2*0.200f;  // coeff 200Hz→1600Hz
        lp1 += fc*(bb-lp1);
        lp2 += fc*(lp1-lp2);  // segunda etapa — -12dB/oct extra
        bb = lp2;

        // DC blocker
        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
        bb=float(y)*(1.f/32767.f);

        // ── FLOATBEAT ────────────────────────────────────────────
        // MACRO1 transpone el pitch ÷4..×4 (suavizado para evitar clicks)
        float hz_target = fhz * (0.25f + macro1*3.75f);
        hz_target=clampf(hz_target,15.f,3000.f);
        fhz_s += 0.001f*(hz_target-fhz_s);  // slew lento solo para fb
        float fb=fbalgo(fst,DT,fhz_s,fbody,falgo);
        float fy=fb-fdcx+fdcy*(252.f/256.f); fdcx=fb;fdcy=fy; fb=fy;

        // ── MORPH ────────────────────────────────────────────────
        return bb*(1.f-morph) + fb*morph;
    }
};

// ── Synth: crossfade suave entre una voz y la siguiente ──────────
struct Synth{
    Voice cur, prv;
    float xfade=1.f;

    void init(){
        cur.randomize(); prv=cur;
    }

    void randomize(){
        prv=cur;
        cur.randomize();
        xfade=0.f;
    }

    int16_t next(uint32_t t, float morph, float m1, float m2){
        float s_cur=cur.sample(t,morph,m1,m2);
        float out;
        if(xfade<1.f){
            float s_prv=prv.sample(t,morph,m1,m2);
            float f=xfade*xfade*(3.f-2.f*xfade);  // smoothstep
            out=s_prv*(1.f-f)+s_cur*f;
            xfade+=1.f/(44100.f*0.2f);  // 200ms crossfade
            if(xfade>1.f)xfade=1.f;
        } else {
            out=s_cur;
        }
        // Ganancia conservadora — el master bus hace el boost
        out*=0.75f;
        if(out> .92f)out= .92f;
        if(out<-.92f)out=-.92f;
        return(int16_t)(out*32767.f);
    }
};

// ── Master bus: boost suave + softclip ───────────────────────────
static inline int16_t master_bus(int32_t x){
    float s=float(x)*(1.8f/32768.f);       // boost ×1.8 (+5dB)
    float sc=s/(1.f+fabsf(s)*0.6f);         // softclip suave
    if(sc> .97f)sc= .97f;
    if(sc<-.97f)sc=-.97f;
    return(int16_t)(sc*32767.f);
}

// ── Pads resistivos ───────────────────────────────────────────────
static float    pad_baseline[4]={9300,9300,9300,9300};
static bool     pad_on[4]={},pad_prev[4]={};
static uint8_t  pad_conf[4]={};
static uint32_t pad_hold_ms[4]={};

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
    uint32_t now=to_ms_since_boot(get_absolute_time());
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        uint32_t raw=measure_pad_us(c);
        bool touched=(float(raw)>pad_baseline[c]*1.35f);
        if(!touched && !pad_on[c])
            pad_baseline[c]+=0.0005f*(float(raw)-pad_baseline[c]);
        if(!pad_on[c]){
            if(touched){if(++pad_conf[c]>=4){pad_on[c]=true;pad_hold_ms[c]=now;}}
            else pad_conf[c]=0;
        } else {
            if(!touched && (now-pad_hold_ms[c])>300u) pad_on[c]=false;
        }
    }
}

static float adc_read_ch(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]);gpio_set_dir(PIN_PAD[c],GPIO_IN);
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
        gpio_put(PIN_LED,0);sleep_ms(120);
        gpio_put(PIN_LED,1);sleep_ms(120);
    }
    gpio_put(PIN_LED,0);
    g_ready=true;

    float sm=0.5f,sm1=0.5f,sm2=0.5f;
    while(true){
        sm  +=0.12f*(adc_read_ch(0)-sm);  g_morph =sm;
        sm1 +=0.12f*(adc_read_ch(1)-sm1); g_macro1=sm1;
        sm2 +=0.12f*(adc_read_ch(2)-sm2); g_macro2=sm2;
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

    static Synth synth;

    multicore_launch_core1(core1_main);
    while(!g_ready)sleep_ms(10);

    synth.init();  // randomiza después de que Core1 seede el RNG

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
            drums.set_params(g_macro2*0.6f+0.1f, 0.3f+g_macro2*0.5f, -1.f);
        }

        const int16_t ss=synth.next(t, g_morph, g_macro1, g_macro2);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd=((int32_t)ss*sc)>>15;
        // Mix V6: synth×60% + drums×90%
        int32_t ol=((sd*19661)>>15)+((int32_t)dl*29491>>15);
        int32_t or_=((sd*19661)>>15)+((int32_t)dr*29491>>15);

        i2s_write(master_bus(ol), master_bus(or_));
        ++t;
    }
}

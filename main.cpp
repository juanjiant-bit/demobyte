// Dem0!Byt3 V13 — motor V6 exacto + pads resistivos + 3 pots + master bus
//
// MOTOR: idéntico al V6 que sonaba bien
//   - FbSt con drift LFO ±8% (da vida a tonos fijos)
//   - brate 1-3 (rango original del bytebeat)
//   - morph 3 zonas: BB puro → BB híbrido → FB puro
//   - mix: synth×60% + drums×90% (sin overflow)
//
// POTS:
//   GP26 MORPH  — mezcla BB↔FB (igual que siempre)
//   GP27 MACRO1 — pitch: escala fhz de ambas voces (×0.25 a ×4, 4 octavas)
//                 + velocidad: brate multiplier (lento↔rápido)
//   GP28 MACRO2 — complejidad: fbody de ambas voces (0=seco, 1=lleno)
//                 + temperatura: modifica cuánto drift tienen los tonos
//
// PADS resistivos: 100kΩ pullup a 3V3, 100nF a GND
//   Threshold 35% sobre baseline, confirm 4 lecturas, hold 300ms
//
// MASTER BUS: boost×2 → softclip suave (tanh) → output limpio y fuerte

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

// GP26=ADC0=MORPH  GP27=ADC1=MACRO1(pitch+speed)  GP28=ADC2=MACRO2(body+drift)
static volatile float   g_pot=0.5f;     // MORPH — mantiene nombre para compatibilidad con motor V6
static volatile float   g_macro1=0.5f;  // MACRO1 — pitch×speed
static volatile float   g_macro2=0.5f;  // MACRO2 — body×drift
static volatile uint8_t g_pad_event=0;
static volatile bool    g_ready=false;

static uint32_t g_rng=0xDEADBEEFu;
static inline uint32_t rng_next(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return g_rng;}
static inline float rng_f(){return float(rng_next()>>8)*(1.f/16777215.f);}

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
static inline float lerp(float a,float b,float t){return a+(b-a)*t;}

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

// ── Floatbeat — igual que V6, con drift ──────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};

// FbSt idéntico al V6 — incluye drift
struct FbSt{float t=0,env=0,sph=0,drift=0; uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo,float drift_amt=0.08f){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    // Drift lento — da vida a tonos. MACRO2 controla la cantidad (0=sin drift, 1=máximo)
    st.drift+=dt*0.17f; if(st.drift>6.28318f)st.drift-=6.28318f;
    float hz_live=hz*(1.f+fs(st.drift*0.15f)*drift_amt);
    const float t=st.t;
    hz_live=hz_live<20?20:(hz_live>2200?2200:hz_live);
    body=body<0?0:(body>1?1:body);
    switch(algo%12u){
    case 0:{float x=fs(t*hz_live)*(.5f+.2f*body)+fs(t*hz_live*.5f)*(.25f+.2f*body)+fs(t*hz_live*1.5f)*.08f;return fclip(x*.85f);}
    case 1:{float x=fs(t*hz_live*.5f)*.55f+fs(t*hz_live)*.30f+ft(t*hz_live*2.f)*.08f*(1.f+body);return fclip(x*.9f);}
    case 2:{float a=fs(t*hz_live),b=fs(t*(hz_live*1.007f));return fclip(a*b*(.7f+.2f*body)+fs(t*hz_live*.5f)*.2f);}
    case 3:{st.env+=dt*4.f;if(st.env>1.f)st.env=0.f;float e=1.f-st.env;
            return fclip(fs(t*hz_live+fs(t*hz_live*2.f)*.3f*e)*e*(.8f+.15f*body));}
    case 4:{float pw=.1f+.4f*(fs(t*hz_live*.125f)*.5f+.5f);
            return fclip((fsq(t*hz_live,pw)*(.5f+.3f*body)+fs(t*hz_live*.5f)*.2f)*.75f);}
    case 5:{float gate=fsq(t*hz_live*.0625f,.35f)*.5f+.5f;
            return fclip((fs(t*hz_live)+ft(t*hz_live*2.01f)*.3f)*gate*(.7f+.2f*body));}
    case 6:{st.sph+=dt*hz_live*.03125f;uint8_t n=(uint8_t)(st.sph)%8u;
            return fclip((fs(t*hz_live*PENTA[n])*(.65f+.2f*body)+fs(t*hz_live*PENTA[n]*.5f)*.2f)*.85f);}
    case 7:{float ps=st.sph;st.sph+=dt*hz_live*.03125f;
            if((uint8_t)st.sph!=(uint8_t)ps){st.lf^=st.lf<<7;st.lf^=st.lf>>9;}
            return fclip(fs(t*hz_live*PENTA[st.lf%8u])*(.75f+.15f*body));}
    case 8:{st.sph+=dt*hz_live*.015625f;uint8_t n=(uint8_t)(st.sph)%4u;
            return fclip((fs(t*hz_live*BASS[n])*(.6f+.25f*body)+fs(t*hz_live*BASS[n]*.5f)*.25f));}
    case 9:{float idx=2.f+body*5.f,mod=fs(t*(hz_live*1.41f))*idx;
            return fclip((fs(t*hz_live+mod)*.6f+fs(t*hz_live*.5f)*.2f)*.8f);}
    case 10:{float x=ft(t*hz_live)*(1.5f+body*2.5f);return fclip(ffold(x)*(.65f+.2f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5;
             float n=float((int32_t)st.lf)*(1.f/2147483648.f);
             return fclip(fs(t*hz_live)*(.3f+.4f*body)+n*(.15f-.1f*body));}
    default:return 0.f;
    }
}

// ── Voice — idéntica al V6 ────────────────────────────────────────
struct Voice{
    uint8_t  bfa=2,bfb=10,bmorph=128,bseed=0;
    uint16_t brate=1;
    FbSt     fst;
    uint8_t  falgo=0;
    float    fhz=110.f, fbody=.5f;
    float    lp=0.f;
    int32_t  dcx=0,dcy=0;
    float    fdcx=0,fdcy=0;

    void randomize(){
        bfa   =(uint8_t)(rng_next()%17u);
        bfb   =(uint8_t)(rng_next()%17u);
        bmorph=(uint8_t)(rng_next()>>24);
        brate =(uint16_t)(1u+rng_next()%3u);  // 1,2,3 — igual que V6
        bseed =(uint8_t)(rng_next()>>24);
        falgo =(uint8_t)(rng_next()%12u);
        fhz   =55.f*powf(2.f,rng_f()*4.f);
        fbody =.15f+rng_f()*.70f;
        fst   =FbSt{};
        lp=0.f; dcx=0; dcy=0; fdcx=0; fdcy=0;
    }

    // MACRO1 escala el pitch: 0=×0.25 (2 octavas abajo), 0.5=×1, 1=×4 (2 octavas arriba)
    // MACRO1 también escala brate: 0=más lento (×2), 1=más rápido (÷2)
    float bb_sample(uint32_t t, float macro1){
        // Escalar brate con macro1: 0→brate×2, 0.5→brate×1, 1→brate÷2
        float rate_scale = 2.f - macro1*1.5f;  // 2.0→0.5
        uint16_t eff_rate = (uint16_t)fmaxf(1.f, float(brate)*rate_scale);
        const uint32_t ts=t/(eff_rate?eff_rate:1u);
        uint8_t va=bbf(bfa,ts,bseed);
        uint8_t vb=bbf(bfb,ts^(uint32_t)(bseed*0x55u),bseed^0xA5u);
        uint8_t raw=(uint8_t)(((uint16_t)va*(255u-bmorph)+(uint16_t)vb*bmorph)>>8);
        float bb=float((int8_t)(raw^0x80u))*(1.f/128.f);
        lp+=0.05f*(bb-lp); bb=lp*.65f+bb*.35f;
        int32_t s32=(int32_t)(bb*32767.f),y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
        return float(y)*(1.f/32767.f);
    }

    // MACRO1 escala fhz: ×0.25 a ×4
    // MACRO2 controla body: 0=fbody propio, 1=lleno
    //         y drift amount: 0=sin drift, 1=máximo ±12%
    float fb_sample(float dt, float macro1, float macro2){
        float pitch_scale = 0.25f*powf(16.f, macro1);  // 0.25→4×
        float hz_use  = fhz * pitch_scale;
        hz_use = hz_use<15.f?15.f:(hz_use>3000.f?3000.f:hz_use);
        float body_use  = fbody*(1.f-macro2) + macro2;  // blend propio→1.0
        float drift_amt = macro2*0.12f + 0.02f;          // 0.02→0.14
        float fb=fbalgo(fst,dt,hz_use,body_use,falgo,drift_amt);
        float y=fb-fdcx+fdcy*(252.f/256.f); fdcx=fb;fdcy=y; return y;
    }
};

// ── Synth — igual que V6, con macros pasados a las voces ─────────
struct Synth{
    Voice va,vb;
    float xfade=1.f;
    int   active_new=0;

    void randomize(){
        if(active_new==0){vb.randomize();active_new=1;}
        else{va.randomize();active_new=0;}
        xfade=0.f;
    }

    int16_t next(uint32_t t, float pot, float macro1, float macro2){
        constexpr float DT=1.f/44100.f;
        if(xfade<1.f) xfade+=DT*20.f;
        if(xfade>1.f) xfade=1.f;
        Voice& v_old=(active_new==0)?vb:va;
        Voice& v_new=(active_new==0)?va:vb;
        const float sm=xfade*xfade*(3.f-2.f*xfade);
        float out;

        if(pot<0.3f){
            float bb=v_new.bb_sample(t,macro1);
            float coeff=0.02f+pot*(0.10f/0.3f);
            v_new.lp+=coeff*(bb-v_new.lp);
            out=v_new.lp;
        }
        else if(pot<0.7f){
            float m=(pot-0.3f)/0.4f;
            const uint32_t ta=t/(v_new.brate?v_new.brate:1u);
            const uint32_t tb=t/(v_old.brate?v_old.brate:1u);
            uint8_t ra=(uint8_t)(((uint16_t)bbf(v_new.bfa,ta,v_new.bseed)*(255u-v_new.bmorph)
                                 +(uint16_t)bbf(v_new.bfb,ta^(uint32_t)(v_new.bseed*0x55u),v_new.bseed^0xA5u)*v_new.bmorph)>>8);
            uint8_t rb=(uint8_t)(((uint16_t)bbf(v_old.bfa,tb,v_old.bseed)*(255u-v_old.bmorph)
                                 +(uint16_t)bbf(v_old.bfb,tb^(uint32_t)(v_old.bseed*0x55u),v_old.bseed^0xA5u)*v_old.bmorph)>>8);
            uint8_t rmix=(uint8_t)(ra*(1.f-m)+rb*m);
            float bb=float((int8_t)(rmix^0x80u))*(1.f/128.f);
            v_new.lp+=0.05f*(bb-v_new.lp); bb=v_new.lp*.65f+bb*.35f;
            int32_t s32=(int32_t)(bb*32767.f),y=s32-v_new.dcx+((v_new.dcy*252)>>8);
            v_new.dcx=s32;v_new.dcy=y;
            float bb_out=float(y)*(1.f/32767.f);
            float fb_amt=m>0.5f?(m-0.5f)*2.f:0.f;
            float fb_out=v_new.fb_sample(DT,macro1,macro2);
            out=lerp(bb_out,fb_out,fb_amt*fb_amt);
        }
        else{
            out=v_new.fb_sample(DT,macro1,macro2);
        }

        if(sm<0.999f){
            float old_out;
            if(pot<0.3f)      old_out=v_old.bb_sample(t,macro1);
            else if(pot<0.7f) old_out=v_old.fb_sample(DT,macro1,macro2);
            else              old_out=v_old.fb_sample(DT,macro1,macro2);
            out=lerp(old_out,out,sm);
        }

        out*=0.80f;
        if(out>.92f)out=.92f; if(out<-.92f)out=-.92f;
        return(int16_t)(out*32767.f);
    }
};

// ── Master bus: boost + softclip suave ───────────────────────────
// Boost de +6dB (×2) antes del softclip para que la señal llene el rango
// Softclip: tanh aproximado via polinomio — sin aliasing, sin harsh
static inline int16_t master_bus(int32_t x){
    // Boost ×2
    float s = float(x) * (2.f/32768.f);  // a float normalizado ×2
    // Softclip suave: tanh(s*0.9)/tanh(0.9) — aproximado con fclip
    // fclip(x) = x/(1+|x|) — más suave que hard clip
    float sc = s / (1.f + fabsf(s) * 0.7f);
    // Clamp final
    if(sc >  0.98f) sc =  0.98f;
    if(sc < -0.98f) sc = -0.98f;
    return (int16_t)(sc * 32767.f);
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
        // Threshold 35% — robusto contra ruido eléctrico
        bool touched=(float(raw)>pad_baseline[c]*1.35f);
        if(!touched && !pad_on[c])
            pad_baseline[c]+=0.0005f*(float(raw)-pad_baseline[c]);
        if(!pad_on[c]){
            if(touched){if(++pad_conf[c]>=4){pad_on[c]=true;pad_hold_ms[c]=now;}}
            else pad_conf[c]=0;
        } else {
            // 300ms mínimo para filtrar rebote al soltar
            if(!touched && (now-pad_hold_ms[c])>300u) pad_on[c]=false;
        }
    }
}

// ── Core1: pads + pots ────────────────────────────────────────────
static float adc_read_ch(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]);gpio_set_dir(PIN_PAD[c],GPIO_IN);
        gpio_disable_pulls(PIN_PAD[c]);
    }
    // ADC init una sola vez aquí
    adc_init();
    adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);

    // Entropía del ADC flotante antes de calibrar
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

    // Smoothing corto: elimina zipper sin lag perceptible
    float sm=0.5f, sm1=0.5f, sm2=0.5f;
    while(true){
        sm  += 0.15f*(adc_read_ch(0)-sm);  g_pot    = sm;
        sm1 += 0.15f*(adc_read_ch(1)-sm1); g_macro1 = sm1;
        sm2 += 0.15f*(adc_read_ch(2)-sm2); g_macro2 = sm2;
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
    while(!g_ready)sleep_ms(10);  // espera entropía + calibración de pads

    // Randomizar DESPUÉS de que Core1 seede el RNG con entropía real
    synth.va.randomize();
    synth.vb.randomize();

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
            // Drums: macro2 controla color/decay
            drums.set_params(g_macro2*0.6f+0.1f, 0.3f+g_macro2*0.5f, -1.f);
        }

        const int16_t ss=synth.next(t, g_pot, g_macro1, g_macro2);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd=((int32_t)ss*sc)>>15;
        // Mix V6 exacto: synth×60% + drums×90%
        int32_t ol=((sd*19661)>>15)+((int32_t)dl*29491>>15);
        int32_t or_=((sd*19661)>>15)+((int32_t)dr*29491>>15);

        // Master bus: boost + softclip
        i2s_write(master_bus(ol), master_bus(or_));
        ++t;
    }
}

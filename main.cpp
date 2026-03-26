// Dem0!Byt3 V10 — tipo libre + BPM lock + stereo (fix overflow + stereo real)
//
// CONEXIONES:
//   GP8/9/13/14 = pads resistivos (10kΩ pullup a 3V3, 100nF a GND, pad a GND)
//   GP26 (ADC0) = POT MORPH   (0=voz A, 1=voz B)
//   GP27 (ADC1) = POT MACRO1  (pitch + velocidad)
//   GP28 (ADC2) = POT MACRO2  (cuerpo + timbre)
//   GP10=BCLK, GP11=LRCK, GP12=DIN, XSMT→3V3

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

static volatile float    g_morph=0.5f, g_macro1=0.5f, g_macro2=0.5f;
static volatile uint8_t  g_pad_event=0;
static volatile bool     g_ready=false;
static volatile uint32_t g_tick=0;  // BPM master tick

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
static inline float ffold(float x){
    x=x*.5f+.5f;x-=(int)x;if(x<0)x+=1.f;return x*2.f-1.f;
}

// ── BPM MASTER — 120 BPM, 48 ticks/beat ─────────────────────────
// tick_period = 44100*60/(120*48) = 459 samples
constexpr uint32_t TICK_PERIOD = SR*60u/(120u*48u);  // 459
static uint32_t tick_accum=0;

// Voz A: tick directo. Voz B: tick*2/3 (poliritmia 3:2, sync cada 3 beats)
static inline uint32_t ta(){ return g_tick; }
static inline uint32_t tb(){ return (g_tick*2u)/3u; }

// ── BYTEBEAT 12 fórmulas (usa tick BPM, no sample counter) ───────
static inline uint8_t bbf(uint8_t id, uint32_t t, uint8_t seed, uint8_t morph){
    // morph mezcla la fórmula id con una variante desplazada
    uint8_t va, vb;
    switch(id%12u){
    case 0: va=(uint8_t)(t*(t>>11&t>>8&57u)); break;
    case 1: va=(uint8_t)((t*(t>>9|t>>13))|t>>6); break;
    case 2: va=(uint8_t)(t*((t>>8|t>>9)&63u)); break;
    case 3: va=(uint8_t)((t>>7|t|t>>6)*10u+4u*(t&t>>13|t>>6)); break;
    case 4: va=(uint8_t)((t&t>>8)*(t>>4|t>>8)); break;
    case 5: va=(uint8_t)(((t*5u)&(t>>7))|((t*3u)&(t>>10))); break;
    case 6: va=(uint8_t)(t*(((t>>11)&3u)+1u)&(t>>5)); break;
    case 7: va=(uint8_t)(((t*7u)&(t>>6))|(t>>4)); break;
    case 8: va=(uint8_t)(t^(t>>3)^(t>>6)); break;
    case 9: va=(uint8_t)((t>>2)^(t>>5)^(t>>7)); break;
    case 10:va=(uint8_t)((t*(t>>9))^(t>>7)^(t>>13)); break;
    case 11:va=(uint8_t)(((t*11u)&(t>>9))^((t*5u)&(t>>11))); break;
    default:va=(uint8_t)t;
    }
    // Variante: misma fórmula con t+seed offset → morph de fase
    uint32_t t2=t^((uint32_t)seed<<3);
    switch(id%12u){
    case 0: vb=(uint8_t)(t2*(t2>>11&t2>>8&57u)); break;
    case 1: vb=(uint8_t)((t2*(t2>>9|t2>>13))|t2>>6); break;
    case 2: vb=(uint8_t)(t2*((t2>>8|t2>>9)&63u)); break;
    case 3: vb=(uint8_t)((t2>>7|t2|t2>>6)*10u+4u*(t2&t2>>13|t2>>6)); break;
    case 4: vb=(uint8_t)((t2&t2>>8)*(t2>>4|t2>>8)); break;
    case 5: vb=(uint8_t)(((t2*5u)&(t2>>7))|((t2*3u)&(t2>>10))); break;
    case 6: vb=(uint8_t)(t2*(((t2>>11)&3u)+1u)&(t2>>5)); break;
    case 7: vb=(uint8_t)(((t2*7u)&(t2>>6))|(t2>>4)); break;
    case 8: vb=(uint8_t)(t2^(t2>>3)^(t2>>6)); break;
    case 9: vb=(uint8_t)((t2>>2)^(t2>>5)^(t2>>7)); break;
    case 10:vb=(uint8_t)((t2*(t2>>9))^(t2>>7)^(t2>>13)); break;
    case 11:vb=(uint8_t)(((t2*11u)&(t2>>9))^((t2*5u)&(t2>>11))); break;
    default:vb=(uint8_t)t2;
    }
    return (uint8_t)(((uint16_t)va*(255u-morph)+(uint16_t)vb*morph)>>8);
}

// ── FLOATBEAT 12 algos ────────────────────────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};
struct FbSt{
    float t=0,env=0,sph=0,drift=0;
    uint32_t lf=0xACE1u;
};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    st.drift+=dt*0.17f; if(st.drift>6.28318f)st.drift-=6.28318f;
    float hzl=hz*(1.f+fs(st.drift*0.13f)*0.04f);
    const float t=st.t;
    hzl=hzl<15?15:(hzl>2400?2400:hzl);
    body=clamp01(body);
    switch(algo%12u){
    case 0:{float x=fs(t*hzl)*(.50f+.20f*body)+fs(t*hzl*.5f)*(.28f+.18f*body)+fs(t*hzl*2.f)*.06f;return fclip(x*.85f);}
    case 1:{float x=fs(t*hzl*.5f)*.58f+fs(t*hzl)*.28f+ft(t*hzl*2.f)*.09f*(1.f+body);return fclip(x*.90f);}
    case 2:{float a=fs(t*hzl),b=fs(t*(hzl*1.0047f));return fclip(a*b*(.65f+.20f*body)+fs(t*hzl*.5f)*.22f);}
    case 3:{float dr=hzl*dt*2.5f;st.env+=dr;if(st.env>1.f)st.env=0.f;
            float e=1.f-st.env*st.env;
            return fclip(fs(t*hzl+fs(t*hzl*2.f)*(.3f+.4f*body)*e)*e*.90f);}
    case 4:{float pw=.08f+.44f*(fs(t*hzl*.08f)*.5f+.5f);
            return fclip((fsq(t*hzl,pw)*(.55f+.25f*body)+fs(t*hzl*.5f)*.20f)*.75f);}
    case 5:{float gate=fsq(t*hzl*.0625f,.3f)*.5f+.5f;
            return fclip((fs(t*hzl)+ft(t*hzl*2.f)*.28f)*gate*(.72f+.20f*body));}
    case 6:{st.sph+=dt*hzl*.03125f;uint8_t n=(uint8_t)(st.sph)%8u;
            float f=hzl*PENTA[n];
            return fclip((fs(t*f)*(.60f+.20f*body)+fs(t*f*.5f)*.22f)*.85f);}
    case 7:{float ps=st.sph;st.sph+=dt*hzl*.025f;
            if((uint8_t)st.sph!=(uint8_t)ps){st.lf^=st.lf<<7;st.lf^=st.lf>>9;}
            return fclip(fs(t*hzl*PENTA[st.lf%8u])*(.72f+.18f*body));}
    case 8:{st.sph+=dt*hzl*.015625f;uint8_t n=(uint8_t)(st.sph)%4u;
            float f=hzl*BASS[n];
            return fclip((fs(t*f)*(.62f+.22f*body)+fs(t*f*.5f)*.26f));}
    case 9:{float idx=1.5f+body*6.f,mod=fs(t*(hzl*1.618f))*idx;
            return fclip((fs(t*hzl+mod)*(.62f+.15f*body)+fs(t*hzl*.5f)*.18f)*.82f);}
    case 10:{float fa=1.2f+body*2.8f+fs(t*hzl*.02f)*.8f;
             return fclip(ffold(ft(t*hzl)*fa)*(.65f+.20f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5;
             float n=float((int32_t)st.lf)*(1.f/2147483648.f)*.25f;
             return fclip(fs(t*hzl)*(.35f+.35f*body)+n*(1.f-body*.6f));}
    default:return 0.f;
    }
}

// ── VOICE — BB o FB, elegido en randomize ─────────────────────────
struct Voice {
    bool    is_fb=false;
    uint8_t bb_id=2, bb_seed=0, bb_morph=128;
    FbSt    fst_l, fst_r;   // FIX: estado SEPARADO para L y R
    uint8_t fb_id=0;
    float   fhz=110.f, fbody=.5f;
    float   bb_lp=0.f;
    int32_t bb_dcx=0, bb_dcy=0;
    float   fdcx_l=0,fdcy_l=0, fdcx_r=0,fdcy_r=0;

    void randomize(){
        is_fb   =(rng_next()&1u)!=0;
        bb_id   =(uint8_t)(rng_next()%12u);
        bb_seed =(uint8_t)(rng_next()>>24);
        bb_morph=(uint8_t)(rng_next()>>24);
        fb_id   =(uint8_t)(rng_next()%12u);
        fhz     =55.f*powf(2.f,rng_f()*4.f);
        fbody   =.15f+rng_f()*.70f;
        fst_l=FbSt{}; fst_r=FbSt{};
        // El canal R arranca con offset de fase de 7ms para efecto Haas
        fst_r.t=7.f/1000.f;  // 7ms offset
        bb_lp=0.f; bb_dcx=0; bb_dcy=0;
        fdcx_l=0;fdcy_l=0;fdcx_r=0;fdcy_r=0;
    }

    void sample_lr(uint32_t tick, float macro1, float macro2,
                   float& out_l, float& out_r){
        constexpr float DT=1.f/44100.f;
        float hz_use  =55.f*powf(2.f,macro1*4.f);
        float body_use=clamp01(macro2);
        uint8_t m=(uint8_t)(macro2*255.f);

        if(!is_fb){
            // BB: L y R usan ticks levemente distintos para micro-diferencia
            float bbl=float((int8_t)(bbf(bb_id,tick,   bb_seed,m)^0x80u))*(1.f/128.f);
            float bbr=float((int8_t)(bbf(bb_id,tick+1u,bb_seed,m)^0x80u))*(1.f/128.f);
            float lc=0.04f+body_use*0.08f;
            bb_lp+=lc*(bbl-bb_lp);
            bbl=bb_lp*.58f+bbl*.42f;
            bbr=bb_lp*.58f+bbr*.42f;
            // DC blocker L
            int32_t s32=(int32_t)(bbl*32767.f);
            int32_t y=s32-bb_dcx+((bb_dcy*252)>>8); bb_dcx=s32;bb_dcy=y;
            out_l=float(y)*(1.f/32767.f);
            // DC blocker R (mismos coefs, aproximación OK para BB)
            s32=(int32_t)(bbr*32767.f);
            y=s32-bb_dcx+((bb_dcy*252)>>8);
            out_r=float(y)*(1.f/32767.f);
        } else {
            // FB: L y R tienen su propio FbSt y hz con 3 cents de detune
            float fl=fbalgo(fst_l,DT,hz_use,       body_use,fb_id);
            float fr=fbalgo(fst_r,DT,hz_use*1.00174f,body_use,fb_id);
            // DC blocker L
            float yl=fl-fdcx_l+fdcy_l*(252.f/256.f); fdcx_l=fl;fdcy_l=yl;
            // DC blocker R
            float yr=fr-fdcx_r+fdcy_r*(252.f/256.f); fdcx_r=fr;fdcy_r=yr;
            out_l=yl; out_r=yr;
        }
    }
};

// ── SYNTH ─────────────────────────────────────────────────────────
struct Synth {
    Voice va,vb, old_a,old_b;
    float xfade=1.f;
    // Pan LFO suave — movimiento lento independiente por voz
    float pan_ph_a=0.f, pan_ph_b=0.5f;  // fases opuestas

    void randomize(){
        old_a=va; old_b=vb;
        va.randomize(); vb.randomize();
        xfade=0.f;
    }

    void next(float morph, float macro1, float macro2,
              int16_t& out_l, int16_t& out_r){
        constexpr float DT=1.f/44100.f;

        // Pan LFO: 0.23Hz y 0.17Hz, rango ±25%
        pan_ph_a+=DT*0.23f; if(pan_ph_a>1.f)pan_ph_a-=1.f;
        pan_ph_b+=DT*0.17f; if(pan_ph_b>1.f)pan_ph_b-=1.f;
        float pan_a=0.5f+fs(pan_ph_a)*0.25f;  // 0.25..0.75
        float pan_b=0.5f+fs(pan_ph_b)*0.25f;

        float al,ar,bl,br;
        va.sample_lr(ta(),macro1,macro2,al,ar);
        vb.sample_lr(tb(),macro1,macro2,bl,br);

        // Crossfade de randomize (100ms)
        if(xfade<1.f){
            float f=xfade*xfade*(3.f-2.f*xfade);
            float oal,oar,obl,obr;
            old_a.sample_lr(ta(),macro1,macro2,oal,oar);
            old_b.sample_lr(tb(),macro1,macro2,obl,obr);
            al=oal*(1.f-f)+al*f; ar=oar*(1.f-f)+ar*f;
            bl=obl*(1.f-f)+bl*f; br=obr*(1.f-f)+br*f;
            xfade+=DT*10.f; if(xfade>1.f)xfade=1.f;
        }

        // MORPH: 0=voz A, 0.5=50/50, 1=voz B
        float ml=al*(1.f-morph)+bl*morph;
        float mr=ar*(1.f-morph)+br*morph;

        // Paneo independiente por voz — da movimiento stereo orgánico
        // Voz A: más a la izquierda cuando pan_a<0.5
        // Voz B: mueve al revés (pan_b tiene fase opuesta)
        float al_pan=al*(1.f-pan_a)*1.41f;
        float ar_pan=ar*pan_a*1.41f;
        float bl_pan=bl*(1.f-pan_b)*1.41f;
        float br_pan=br*pan_b*1.41f;

        // Mezcla: blend entre morph simple y versión paneada
        float fl=ml*0.5f+(al_pan+bl_pan)*0.25f*(1.f-fabsf(morph-0.5f)*2.f);
        float fr=mr*0.5f+(ar_pan+br_pan)*0.25f*(1.f-fabsf(morph-0.5f)*2.f);

        fl*=0.80f; fr*=0.80f;
        if(fl>.92f)fl=.92f; if(fl<-.92f)fl=-.92f;
        if(fr>.92f)fr=.92f; if(fr<-.92f)fr=-.92f;
        out_l=(int16_t)(fl*32767.f);
        out_r=(int16_t)(fr*32767.f);
    }
};

// ── PADS RESISTIVOS ───────────────────────────────────────────────
constexpr uint32_t PAD_CHARGE_US=300, PAD_MAX_US=12000;
static float   pad_base_t=10000.f;
static bool    pad_on[4]={},pad_prev[4]={};
static uint8_t pad_conf[4]={};

static uint32_t measure_pad(uint8_t c){
    gpio_set_dir(PIN_PAD[c],GPIO_OUT);
    gpio_put(PIN_PAD[c],1);
    sleep_us(PAD_CHARGE_US);
    gpio_set_dir(PIN_PAD[c],GPIO_IN);
    gpio_disable_pulls(PIN_PAD[c]);
    const uint32_t t0=time_us_32();
    while(gpio_get(PIN_PAD[c]))
        if((time_us_32()-t0)>=PAD_MAX_US) return PAD_MAX_US;
    return time_us_32()-t0;
}

static void calibrate_pads(){
    sleep_ms(300);
    float sum=0; int n=0;
    for(int i=0;i<10;++i)
        for(uint8_t c=0;c<4;++c){sum+=float(measure_pad(c));++n;}
    pad_base_t=sum/float(n);
}

static void scan_pads(){
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        float raw=float(measure_pad(c));
        float p=clamp01(1.f-raw/pad_base_t);
        if(!pad_on[c]){
            if(p>0.28f){if(++pad_conf[c]>=2)pad_on[c]=true;}
            else pad_conf[c]=0;
        } else {
            if(p<0.12f){pad_on[c]=false;pad_conf[c]=0;}
        }
    }
}

static float adc_ch(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]);
        gpio_set_dir(PIN_PAD[c],GPIO_IN);
        gpio_disable_pulls(PIN_PAD[c]);
    }
    adc_init();
    adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);

    gpio_put(PIN_LED,1);
    calibrate_pads();
    for(int i=0;i<3;++i){
        gpio_put(PIN_LED,0);sleep_ms(120);
        gpio_put(PIN_LED,1);sleep_ms(120);
    }
    gpio_put(PIN_LED,0);
    g_ready=true;

    float ms=adc_ch(0),m1=adc_ch(1),m2=adc_ch(2);
    while(true){
        ms+=0.08f*(adc_ch(0)-ms); g_morph =ms;
        m1+=0.08f*(adc_ch(1)-m1); g_macro1=m1;
        m2+=0.08f*(adc_ch(2)-m2); g_macro2=m2;
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
    synth.randomize();

    multicore_launch_core1(core1_main);
    while(!g_ready)sleep_ms(10);

    uint32_t cr=0;
    while(true){
        // BPM tick
        if(++tick_accum>=TICK_PERIOD){
            tick_accum=0;
            g_tick++;
        }

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
            drums.set_params(g_macro1*.6f+.1f, g_macro2*.6f+.2f, -1.f);
        }

        int16_t sl=0,sr=0;
        synth.next(g_morph,g_macro1,g_macro2,sl,sr);

        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        // FIX: orden correcto de operaciones para evitar overflow int32
        // sc es Q15 (0..32767), sl es int16 → multiplicar primero, luego escalar
        int32_t synth_l=((int32_t)sl*sc)>>15;   // ducked synth L
        int32_t synth_r=((int32_t)sr*sc)>>15;   // ducked synth R
        // Mezcla: synth 65% + drums 90%
        int32_t ol=(synth_l*21299>>15)+(int32_t)dl*29491>>15;
        int32_t or_=(synth_r*21299>>15)+(int32_t)dr*29491>>15;
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;
        i2s_write((int16_t)ol,(int16_t)or_);
    }
}

// Dem0!Byt3 V11 — motor V5 exacto + 3 pots + pads resistivos 100kΩ/100nF

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

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_PAD[4]={8,9,13,14};
constexpr uint PIN_LED=25, PIN_BCLK=10, PIN_DIN=12, SR=44100;
// GP26=ADC0=MORPH  GP27=ADC1=FILTER  GP28=ADC2=TIMBRE

// ── Estado compartido Core0↔Core1 ────────────────────────────────
static volatile float   g_morph=0.5f;  // mezcla BB↔FB dentro de cada voz
static volatile float   g_filter=1.0f; // LP cutoff global (0=oscuro, 1=abierto)
static volatile float   g_timbre=0.5f; // body/complejidad del floatbeat
static volatile uint8_t g_pad_event=0;
static volatile bool    g_ready=false;

// ── RNG ───────────────────────────────────────────────────────────
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
struct FbSt{float t=0,t2=0,env=0,sph=0; uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    const float t=st.t;
    hz=hz<20?20:(hz>2000?2000:hz);
    body=body<0?0:(body>1?1:body);
    switch(algo%12u){
    case 0:{float x=fs(t*hz)*(.5f+.2f*body)+fs(t*hz*.5f)*(.25f+.2f*body)+fs(t*hz*1.5f)*.08f;return fclip(x*.85f);}
    case 1:{float x=fs(t*hz*.5f)*.55f+fs(t*hz)*.30f+ft(t*hz*2.f)*.08f*(1.f+body);return fclip(x*.9f);}
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
            return fclip((fs(t*hz*BASS[n])*(.6f+.25f*body)+fs(t*hz*BASS[n]*.5f)*.25f));}
    case 9:{float idx=2.f+body*5.f,mod=fs(t*(hz*1.41f))*idx;
            return fclip((fs(t*hz+mod)*.6f+fs(t*hz*.5f)*.2f)*.8f);}
    case 10:{float x=ft(t*hz)*(1.5f+body*2.5f);return fclip(ffold(x)*(.65f+.2f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5;
             float n=float((int32_t)st.lf)*(1.f/2147483648.f);
             return fclip(fs(t*hz)*(.3f+.4f*body)+n*(.15f-.1f*body));}
    default:return 0.f;
    }
}

// ── Voice — IDÉNTICA al V5, más tabla de rates y timbre pot ──────
struct Voice {
    uint8_t  bfa=2,bfb=10,bmorph=128,bseed=0;
    uint16_t brate=8;
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
        // Rates musicales: 4→64 (t/4 a t/64)
        static const uint16_t RT[]={4,6,8,12,16,24,32,48,64};
        brate=RT[rng_next()%9u];
        bseed=(uint8_t)(rng_next()>>24);
        falgo=(uint8_t)(rng_next()%12u);
        fhz  =55.f*powf(2.f,rng_f()*4.f);
        fbody=.15f+rng_f()*.70f;
        fst  =FbSt{};
        lp=0.f; dcx=0; dcy=0; fdcx=0; fdcy=0;
    }

    // domain: 0=BB puro, 1=FB puro  (controlado por MORPH pot)
    // g_timbre: body del floatbeat  (controlado por TIMBRE pot)
    float sample(uint32_t t, float domain){
        constexpr float DT=1.f/44100.f;

        // ── BB ────────────────────────────────────────────────────
        const uint32_t ts=t/(brate?brate:1u);
        uint8_t va=bbf(bfa,ts,bseed);
        uint8_t vb=bbf(bfb,ts^(uint32_t)(bseed*0x55u),bseed^0xA5u);
        uint8_t raw=(uint8_t)(((uint16_t)va*(255u-bmorph)+(uint16_t)vb*bmorph)>>8);
        float bb=float((int8_t)(raw^0x80u))*(1.f/128.f);
        // LP — más agresivo en zona BB, se abre hacia FB
        float lp_c=0.04f+domain*0.12f;
        lp+=lp_c*(bb-lp);
        bb=lp*.70f+bb*.30f;
        // DC blocker
        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
        bb=float(y)*(1.f/32767.f);

        // ── FB ────────────────────────────────────────────────────
        // g_timbre blend con fbody propio: 50% carácter aleatorio + 50% pot
        float body_use=fbody*0.5f+g_timbre*0.5f;
        float fb=fbalgo(fst,DT,fhz,body_use,falgo);
        float fy=fb-fdcx+fdcy*(252.f/256.f); fdcx=fb;fdcy=fy; fb=fy;

        return bb*(1.f-domain)+fb*domain;
    }
};

// ── Synth — idéntico al V5 ────────────────────────────────────────
struct Synth {
    Voice voices[2];
    int   active=0;
    float xfade=1.f;

    void randomize(){
        int nxt=(active+1)%2;
        voices[nxt].randomize();
        active=nxt;
        xfade=0.f;
    }

    int16_t next(uint32_t t){
        float domain=g_morph;
        float s_new=voices[active].sample(t,domain);
        float out;
        if(xfade<1.f){
            int prev=(active+1)%2;
            float s_old=voices[prev].sample(t,domain);
            float f=xfade*xfade*(3.f-2.f*xfade);
            out=s_old*(1.f-f)+s_new*f;
            xfade+=1.f/2205.f;
            if(xfade>1.f)xfade=1.f;
        } else {
            out=s_new;
        }
        out*=0.82f;
        if(out>.92f)out=.92f; if(out<-.92f)out=-.92f;
        return(int16_t)(out*32767.f);
    }
};

// ── Pads resistivos — 100kΩ a 3V3, 100nF a GND ──────────────────
// Técnica: descarga (OUT LOW), mide tiempo hasta HIGH (INPUT)
// Sin dedo: HIGH en ~9.3ms  Con dedo: más lento o timeout (12ms)
static float    pad_baseline[4]={9300,9300,9300,9300};
static bool     pad_on[4]={},pad_prev[4]={};
static uint8_t  pad_conf[4]={};
static uint32_t pad_hold_ms[4]={};

static uint32_t measure_pad_us(uint8_t c){
    gpio_set_dir(PIN_PAD[c],GPIO_OUT);
    gpio_put(PIN_PAD[c],1); sleep_us(300);   // carga cap
    gpio_put(PIN_PAD[c],0); sleep_us(800);   // descarga cap
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
        bool touched=(float(raw)>pad_baseline[c]*1.20f);
        if(!touched && !pad_on[c])
            pad_baseline[c]+=0.0005f*(float(raw)-pad_baseline[c]);
        if(!pad_on[c]){
            if(touched){if(++pad_conf[c]>=4){pad_on[c]=true;pad_hold_ms[c]=now;}}
            else pad_conf[c]=0;
        } else {
            if(!touched && (now-pad_hold_ms[c])>200u) pad_on[c]=false;
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

    // ADC init SOLO aquí — una sola vez en todo el firmware
    adc_init();
    adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);

    // Entropía: leer ADC flotante antes de calibrar pads
    // (ADC ya inicializado, hacemos esto aquí para evitar doble init)
    uint32_t entropy=0;
    adc_select_input(0);
    for(int i=0;i<64;++i)
        entropy=entropy*1664525u+adc_read()+1013904223u;
    g_rng^=entropy;

    gpio_put(PIN_LED,1);
    calibrate_pads();
    // Triple parpadeo = listo
    for(int i=0;i<3;++i){
        gpio_put(PIN_LED,0);sleep_ms(120);
        gpio_put(PIN_LED,1);sleep_ms(120);
    }
    gpio_put(PIN_LED,0);
    g_ready=true;

    while(true){
        g_morph  = adc_read_ch(0);  // sin slew — respuesta inmediata
        g_filter = adc_read_ch(1);
        g_timbre = adc_read_ch(2);
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
    // Las voces se randomizan después de que Core1 inicialice el RNG con entropía
    // Core1 modifica g_rng con entropy antes de setear g_ready=true

    multicore_launch_core1(core1_main);
    while(!g_ready)sleep_ms(10);  // esperar entropía + calibración

    // Randomizar DESPUÉS de que Core1 seede el RNG
    synth.voices[0].randomize();
    synth.voices[1].randomize();

    // Estado del filtro LP global — inicializar en 0
    float lp_l=0.f, lp_r=0.f;

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
            drums.set_params(g_timbre*0.7f+0.1f, g_timbre*0.6f+0.2f, -1.f);
        }

        const int16_t ss=synth.next(t);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        const int32_t sd=((int32_t)ss*sc)>>15;
        int32_t ol=sd+(int32_t)dl;
        int32_t or_=sd+(int32_t)dr;
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;

        // FILTER LP global — coeff cuadrático, range 0.001..0.451
        // g_filter=1 (pot der) → fc≈3kHz abierto
        // g_filter=0 (pot izq) → fc≈14Hz muy oscuro
        float fc=g_filter*g_filter*0.45f+0.001f;
        lp_l+=fc*(float(ol)-lp_l);
        lp_r+=fc*(float(or_)-lp_r);
        ol=(int32_t)lp_l; or_=(int32_t)lp_r;
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;

        i2s_write((int16_t)ol,(int16_t)or_);
        ++t;
    }
}

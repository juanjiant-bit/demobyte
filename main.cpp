// Dem0!Byt3 V5 — pad autodischarge + real crossfade
//
// PADS: calibración auto-discharge — encuentra el tiempo correcto
//       para ESTOS pads en PARTICULAR, sin asumir nada
//       Baseline threshold absoluto basado en rango medido
//
// CROSSFADE: dos instancias del synth corriendo en paralelo
//            el fade mezcla las salidas de audio reales, no volúmenes
//
// POT: mapeo directo sin zona muerta artificial
//      izq=bytebeat, der=floatbeat, centro=morph

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

constexpr uint PIN_ROW    = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_POT    = 26;
constexpr uint PIN_BCLK   = 10;
constexpr uint PIN_DIN    = 12;
constexpr uint SR         = 44100;

static volatile float   g_pot       = 0.5f;
static volatile uint8_t g_pad_event = 0;
static volatile bool    g_ready     = false;

static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng^=g_rng<<13; g_rng^=g_rng>>17; g_rng^=g_rng<<5; return g_rng;
}
static inline float rng_f() { return float(rng_next()>>8)*(1.f/16777215.f); }

// ── Osciladores ───────────────────────────────────────────────────
static inline float fw(float x){x-=(int)x;return x<0?x+1.f:x;}
static inline float fs(float p){
    float x=fw(p)*6.28318f-3.14159f;
    float y=x*(1.2732f-0.4053f*fabsf(x));
    return y*(0.225f*(fabsf(y)-1.f)+1.f);
}
static inline float ft(float p){float x=fw(p);return 4.f*fabsf(x-.5f)-1.f;}
static inline float fsaw(float p){return fw(p)*2.f-1.f;}
static inline float fsq(float p,float pw=.5f){return fw(p)<pw?1.f:-1.f;}
static inline float fclip(float x){return x/(1.f+fabsf(x));}
static inline float ffold(float x){
    x=x*.5f+.5f; x-=(int)x; if(x<0)x+=1.f; return x*2.f-1.f;
}

// ── Bytebeat ──────────────────────────────────────────────────────
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

// ── Floatbeat — 12 algos ─────────────────────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4] ={.5f,.749f,1.f,.749f};

struct FbSt{float t=0,t2=0,env=0,sph=0; uint32_t lf=0xACE1u;};

static float fbalgo(FbSt&st,float dt,float hz,float body,uint8_t algo){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    const float t=st.t;
    hz=hz<20?20:(hz>2000?2000:hz);
    body=body<0?0:(body>1?1:body);
    switch(algo%12u){
    case 0:{float x=fs(t*hz)*(.5f+.2f*body)+fs(t*hz*.5f)*(.25f+.2f*body)+fs(t*hz*1.5f)*.08f; return fclip(x*.85f);}
    case 1:{float x=fs(t*hz*.5f)*.55f+fs(t*hz)*.30f+ft(t*hz*2.f)*.08f*(1.f+body); return fclip(x*.9f);}
    case 2:{float a=fs(t*hz),b=fs(t*(hz*1.007f)); return fclip(a*b*(.7f+.2f*body)+fs(t*hz*.5f)*.2f);}
    case 3:{st.env+=dt*4.f;if(st.env>1.f)st.env=0.f; float e=1.f-st.env;
            return fclip(fs(t*hz+fs(t*hz*2.f)*.3f*e)*e*(.8f+.15f*body));}
    case 4:{float pw=.1f+.4f*(fs(t*hz*.125f)*.5f+.5f);
            return fclip((fsq(t*hz,pw)*(.5f+.3f*body)+fs(t*hz*.5f)*.2f)*.75f);}
    case 5:{float gate=fsq(t*hz*.0625f,.35f)*.5f+.5f;
            return fclip((fs(t*hz)+ft(t*hz*2.01f)*.3f)*gate*(.7f+.2f*body));}
    case 6:{st.sph+=dt*hz*.03125f; uint8_t n=(uint8_t)(st.sph)%8u;
            return fclip((fs(t*hz*PENTA[n])*(.65f+.2f*body)+fs(t*hz*PENTA[n]*.5f)*.2f)*.85f);}
    case 7:{float prev_sph=st.sph; st.sph+=dt*hz*.03125f;
            if((uint8_t)st.sph!=(uint8_t)prev_sph){st.lf^=st.lf<<7;st.lf^=st.lf>>9;}
            return fclip(fs(t*hz*PENTA[st.lf%8u])*(.75f+.15f*body));}
    case 8:{st.sph+=dt*hz*.015625f; uint8_t n=(uint8_t)(st.sph)%4u;
            return fclip((fs(t*hz*BASS[n])*(.6f+.25f*body)+fs(t*hz*BASS[n]*.5f)*.25f));}
    case 9:{float idx=2.f+body*5.f,mod=fs(t*(hz*1.41f))*idx;
            return fclip((fs(t*hz+mod)*.6f+fs(t*hz*.5f)*.2f)*.8f);}
    case 10:{float x=ft(t*hz)*(1.5f+body*2.5f); return fclip(ffold(x)*(.65f+.2f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5;
             float n=float((int32_t)st.lf)*(1.f/2147483648.f);
             return fclip(fs(t*hz)*(.3f+.4f*body)+n*(.15f-.1f*body));}
    default: return 0.f;
    }
}

// ── Voice: una voz completa bytebeat+floatbeat ────────────────────
struct Voice {
    uint8_t  bfa=2,bfb=10,bmorph=128,bseed=0; uint16_t brate=1;
    FbSt     fst;
    uint8_t  falgo=0;
    float    fhz=110.f,fbody=.5f;
    float    lp=0.f;
    int32_t  dcx=0,dcy=0;
    float    fdcx=0,fdcy=0;

    void randomize(float pot){
        bfa=(uint8_t)(rng_next()%17u); bfb=(uint8_t)(rng_next()%17u);
        bmorph=(uint8_t)(rng_next()>>24); brate=(uint16_t)(1u+rng_next()%3u);
        bseed=(uint8_t)(rng_next()>>24);
        falgo=(uint8_t)(rng_next()%12u);
        fhz=55.f*powf(2.f,rng_f()*4.f);  // hz aleatoria, independiente del pot
        fbody=.15f+rng_f()*.70f;
        fst=FbSt{};
    }

    // Produce un sample dado domain (0=BB puro, 1=FB puro)
    float sample(uint32_t t, float domain){
        constexpr float DT=1.f/44100.f;

        // Bytebeat
        const uint32_t ts=t/(brate?brate:1u);
        uint8_t va=bbf(bfa,ts,bseed);
        uint8_t vb=bbf(bfb,ts^(uint32_t)(bseed*0x55u),bseed^0xA5u);
        uint8_t raw=(uint8_t)(((uint16_t)va*(255u-bmorph)+(uint16_t)vb*bmorph)>>8);
        float bb=float((int8_t)(raw^0x80u))*(1.f/128.f);

        // LP para suavizar aliasing del BB — más agresivo cuando domain<0.5
        float lp_c=0.04f+domain*0.12f;  // más abierto en zona FB
        lp+=lp_c*(bb-lp);
        bb=lp*.70f+bb*.30f;

        // DC blocker BB
        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32; dcy=y;
        bb=float(y)*(1.f/32767.f);

        // Floatbeat
        float fb=fbalgo(fst,DT,fhz,fbody,falgo);
        float fy=fb-fdcx+fdcy*(252.f/256.f); fdcx=fb; fdcy=fy; fb=fy;

        // Mezcla directa según domain
        return bb*(1.f-domain)+fb*domain;
    }
};

// ── Synth con doble voz para crossfade real ───────────────────────
struct Synth {
    Voice voices[2];
    int   active=0;      // voz activa
    float xfade=1.f;     // 1=voz activa completa, <1=fade entrando

    void randomize(float pot){
        int next=(active+1)%2;
        voices[next].randomize(pot);
        active=next;
        xfade=0.f;
    }

    int16_t next(uint32_t t, float pot){
        // pot → domain directo, sin zonas artificiales
        // 0=BB puro, 1=FB puro
        float domain=pot;

        float s_new=voices[active].sample(t,domain);
        float out;

        if(xfade<1.f){
            int prev=(active+1)%2;
            float s_old=voices[prev].sample(t,domain);
            // fade suave con curva cuadrática (más natural que lineal)
            float f=xfade*xfade*(3.f-2.f*xfade);  // smoothstep
            out=s_old*(1.f-f)+s_new*f;
            xfade+=1.f/2205.f;  // 50ms de crossfade (~2205 samples)
            if(xfade>1.f)xfade=1.f;
        } else {
            out=s_new;
        }

        out*=0.82f;
        if(out> .92f)out= .92f;
        if(out<-.92f)out=-.92f;
        return (int16_t)(out*32767.f);
    }
};

// ── Pads con auto-discharge ───────────────────────────────────────
static uint32_t pad_dis_us  = 5000;   // se ajusta en calibración
static uint32_t pad_max_us  = 30000;
static float    pad_base[4] = {};
static bool     pad_on[4]   = {};
static bool     pad_prev[4] = {};
static uint8_t  pad_conf[4] = {};

static uint32_t measure_pad(uint8_t c){
    gpio_put(PIN_ROW,0); sleep_us(pad_dis_us);
    const uint32_t t0=time_us_32();
    gpio_put(PIN_ROW,1);
    while(!gpio_get(PIN_COL[c]))
        if((time_us_32()-t0)>=pad_max_us){gpio_put(PIN_ROW,0);return pad_max_us;}
    const uint32_t dt=time_us_32()-t0; gpio_put(PIN_ROW,0); return dt;
}

static void calibrate_pads(){
    // Paso 1: auto-discharge — encontrar discharge donde las lecturas
    // no sean timeout y tengan un rango estable
    static const uint32_t dis_steps[]={500,1000,2000,5000,10000,20000,50000};
    for(int s=0;s<7;++s){
        pad_dis_us=dis_steps[s];
        pad_max_us=dis_steps[s]*15;  // max=15× discharge
        bool all_ok=true;
        for(uint8_t c=0;c<4;++c){
            uint32_t v=0;
            for(int i=0;i<5;++i) v+=measure_pad(c);
            v/=5;
            if(v>=pad_max_us*85/100){all_ok=false;break;}
        }
        if(all_ok) break;
    }

    // Paso 2: medir baseline con el discharge correcto
    gpio_put(PIN_ROW,0); sleep_ms(50);
    for(uint8_t c=0;c<4;++c){
        uint64_t sum=0;
        for(int s=0;s<20;++s) sum+=measure_pad(c);
        pad_base[c]=float(sum/20);
    }
}

static void scan_pads(){
    bool any=pad_on[0]||pad_on[1]||pad_on[2]||pad_on[3];
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        const uint32_t raw=measure_pad(c);
        const float d=float(raw)-pad_base[c];
        // Thresholds: 20% ON, 10% OFF — ajustados para rango real
        const float on_th =pad_base[c]*0.20f;
        const float off_th=pad_base[c]*0.10f;
        if(!pad_on[c]){
            if(d>=on_th){if(++pad_conf[c]>=2)pad_on[c]=true;}
            else{pad_conf[c]=0; if(!any)pad_base[c]+=0.001f*(float(raw)-pad_base[c]);}
        } else {
            if(d<off_th){pad_on[c]=false;pad_conf[c]=0;}
        }
    }
}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    gpio_init(PIN_ROW);gpio_set_dir(PIN_ROW,GPIO_OUT);gpio_put(PIN_ROW,0);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_COL[c]);gpio_set_dir(PIN_COL[c],GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }
    adc_init();adc_gpio_init(PIN_POT);adc_select_input(0);

    // LED parpadea durante calibración para que sepas que está trabajando
    gpio_put(PIN_LED,1);
    calibrate_pads();
    gpio_put(PIN_LED,0); sleep_ms(100); gpio_put(PIN_LED,1);
    sleep_ms(100); gpio_put(PIN_LED,0);

    g_ready=true;

    float pot_s=float(adc_read())/4095.f;
    while(true){
        pot_s+=0.06f*(float(adc_read())/4095.f-pot_s);
        g_pot=pot_s;
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
    for(int i=0;i<16;++i) i2s_write(0,0);

    static DrumEngine drums;
    drums.init(); drums.set_params(0.3f,0.5f,1.0f);

    static Synth synth;
    // Randomizar las dos voces al inicio para tener algo que escuchar
    synth.voices[0].randomize(0.3f);
    synth.voices[1].randomize(0.7f);

    multicore_launch_core1(core1_main);
    while(!g_ready) sleep_ms(10);

    uint32_t t=0,cr=0;
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
            drums.set_params(pot*.8f,.2f+pot*.6f,-1.f);
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

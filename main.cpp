#include <cstdint>
#include <cmath>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "pcm5102_i2s.pio.h"
#include "audio/drums/drum_engine.h"

static PIO  g_pio = pio0;
static uint g_sm  = 0;
static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

constexpr uint PIN_PAD[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_BCLK   = 10;
constexpr uint PIN_DIN    = 12;
constexpr uint SR         = 44100;

static volatile float   g_morph  = 0.5f;  // GP26 = voice A ↔ voice B
static volatile float   g_filter = 1.0f;  // GP27 = LP global
static volatile float   g_timbre = 0.5f;  // GP28 = internal complexity
static volatile uint8_t g_pad_event = 0;
static volatile bool    g_ready     = false;

static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static inline float rng_f() { return float(rng_next() >> 8) * (1.f / 16777215.f); }
static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float softclip(float x) { return x / (1.f + fabsf(x)); }

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
    case 0:{float x=fs(t*hz)*(.5f+.2f*body)+fs(t*hz*.5f)*(.25f+.2f*body)+fs(t*hz*1.5f)*.08f; return softclip(x*.85f);}    
    case 1:{float x=fs(t*hz*.5f)*.55f+fs(t*hz)*.30f+ft(t*hz*2.f)*.08f*(1.f+body); return softclip(x*.9f);}    
    case 2:{float a=fs(t*hz),b=fs(t*(hz*1.007f)); return softclip(a*b*(.7f+.2f*body)+fs(t*hz*.5f)*.2f);}    
    case 3:{st.env+=dt*4.f;if(st.env>1.f)st.env=0.f; float e=1.f-st.env; return softclip(fs(t*hz+fs(t*hz*2.f)*.3f*e)*e*(.8f+.15f*body));}
    case 4:{float pw=.1f+.4f*(fs(t*hz*.125f)*.5f+.5f); return softclip((fsq(t*hz,pw)*(.5f+.3f*body)+fs(t*hz*.5f)*.2f)*.75f);}    
    case 5:{float gate=fsq(t*hz*.0625f,.35f)*.5f+.5f; return softclip((fs(t*hz)+ft(t*hz*2.01f)*.3f)*gate*(.7f+.2f*body));}
    case 6:{st.sph+=dt*hz*.03125f; uint8_t n=(uint8_t)(st.sph)%8u; return softclip((fs(t*hz*PENTA[n])*(.65f+.2f*body)+fs(t*hz*PENTA[n]*.5f)*.2f)*.85f);}    
    case 7:{float prev_sph=st.sph; st.sph+=dt*hz*.03125f; if((uint8_t)st.sph!=(uint8_t)prev_sph){st.lf^=st.lf<<7;st.lf^=st.lf>>9;} return softclip(fs(t*hz*PENTA[st.lf%8u])*(.75f+.15f*body));}
    case 8:{st.sph+=dt*hz*.015625f; uint8_t n=(uint8_t)(st.sph)%4u; return softclip((fs(t*hz*BASS[n])*(.6f+.25f*body)+fs(t*hz*BASS[n]*.5f)*.25f));}
    case 9:{float idx=2.f+body*5.f,mod=fs(t*(hz*1.41f))*idx; return softclip((fs(t*hz+mod)*.6f+fs(t*hz*.5f)*.2f)*.8f);}    
    case 10:{float x=ft(t*hz)*(1.5f+body*2.5f); return softclip(ffold(x)*(.65f+.2f*body));}
    case 11:{st.lf^=st.lf<<13;st.lf^=st.lf>>17;st.lf^=st.lf<<5; float n=float((int32_t)st.lf)*(1.f/2147483648.f); return softclip(fs(t*hz)*(.3f+.4f*body)+n*(.15f-.1f*body));}
    default: return 0.f;
    }
}

// ── Voice A/B: morph real entre dos voces completas ─────────────────
struct Voice {
    uint8_t  bfa=2,bfb=10,bmorph=128,bseed=0;
    uint16_t brate=8;
    FbSt     fst;
    uint8_t  falgo=0;
    float    fhz=110.f,fbody=.5f;
    float    bb_bias=0.5f;     // preferencia interna hacia BB
    float    fb_bias=0.5f;     // preferencia interna hacia FB
    float    level=0.65f;
    float    bb_lp=0.f;
    int32_t  bbdcx=0,bbdcy=0;
    float    fbdcx=0, fbdcy=0;
    float    tone_lp=0.f;

    void randomize(uint8_t family){
        static const uint16_t RT_SLOW[]={8,12,16,24,32,48,64};
        static const uint16_t RT_MID[] ={6,8,12,16,24,32};
        static const uint16_t RT_BRT[] ={4,6,8,12,16};
        static const uint8_t  BB_SAFE[] ={0,1,2,3,5,6,9,10,12,13,14,15,16};
        static const uint8_t  FB_SAFE[] ={0,1,2,3,4,5,6,8,9,10};

        const uint16_t* rt = RT_MID;
        uint8_t rt_n = 6;
        switch(family){
            default:
            case 0: rt = RT_SLOW; rt_n = 7; fhz = 55.f * powf(2.f, rng_f()*1.5f); fbody = 0.35f + rng_f()*0.45f; bb_bias = 0.60f + rng_f()*0.20f; fb_bias = 0.25f + rng_f()*0.30f; break;
            case 1: rt = RT_MID;  rt_n = 6; fhz = 70.f * powf(2.f, rng_f()*2.2f); fbody = 0.25f + rng_f()*0.50f; bb_bias = 0.35f + rng_f()*0.35f; fb_bias = 0.35f + rng_f()*0.35f; break;
            case 2: rt = RT_BRT;  rt_n = 5; fhz = 110.f* powf(2.f, rng_f()*2.7f); fbody = 0.15f + rng_f()*0.35f; bb_bias = 0.20f + rng_f()*0.25f; fb_bias = 0.55f + rng_f()*0.30f; break;
        }
        bfa    = BB_SAFE[rng_next()% (sizeof(BB_SAFE)/sizeof(BB_SAFE[0]))];
        bfb    = BB_SAFE[rng_next()% (sizeof(BB_SAFE)/sizeof(BB_SAFE[0]))];
        bmorph = (uint8_t)(64 + (rng_next()%128));
        brate  = rt[rng_next()%rt_n];
        bseed  = (uint8_t)(rng_next()>>24);
        falgo  = FB_SAFE[rng_next()% (sizeof(FB_SAFE)/sizeof(FB_SAFE[0]))];
        fst    = FbSt{};
        bb_lp = tone_lp = 0.f;
        bbdcx = bbdcy = 0;
        fbdcx = fbdcy = 0.f;
        level = 0.52f + rng_f()*0.18f;
    }

    float sample(uint32_t t, float timbre){
        constexpr float DT=1.f/44100.f;
        const float tm = clamp01(timbre);

        const uint32_t ts=t/(brate?brate:1u);
        uint8_t va=bbf(bfa,ts,bseed);
        uint8_t vb=bbf(bfb,ts^(uint32_t)(bseed*0x55u),bseed^0xA5u);
        uint8_t raw=(uint8_t)(((uint16_t)va*(255u-bmorph)+(uint16_t)vb*bmorph)>>8);
        float bb=float((int8_t)(raw^0x80u))*(1.f/128.f);

        // suavizado interno de BB: menos aliasing y menos aspereza
        float bb_fc = 0.025f + 0.090f * (0.25f + 0.75f*tm);
        bb_lp += bb_fc * (bb - bb_lp);
        bb = bb_lp*0.82f + bb*0.18f;

        int32_t s32=(int32_t)(bb*32767.f);
        int32_t y=s32-bbdcx+((bbdcy*250)>>8); bbdcx=s32; bbdcy=y;
        bb=float(y)*(1.f/32767.f);

        float body_use = clamp01(fbody*0.55f + tm*0.45f);
        float fb=fbalgo(fst,DT,fhz,body_use,falgo);
        float fy=fb-fbdcx+fbdcy*(248.f/256.f); fbdcx=fb; fbdcy=fy; fb=fy;

        // TIMBRE ahora controla complejidad interna, no el morph A/B
        float fb_amt = clamp01(fb_bias*0.55f + tm*0.45f);
        float bb_amt = clamp01(bb_bias*0.65f + (1.f-tm)*0.25f);
        float sum = bb_amt + fb_amt;
        if(sum < 0.001f) sum = 1.f;
        bb_amt /= sum;
        fb_amt /= sum;

        float mix = bb*bb_amt + fb*fb_amt;

        // un poco de lowpass interno musical antes del master
        float tone_fc = 0.020f + 0.120f * tm;
        tone_lp += tone_fc * (mix - tone_lp);
        mix = tone_lp*0.7f + mix*0.3f;

        mix = softclip(mix * (0.9f + 0.2f*tm));
        return mix * level;
    }
};

struct Synth {
    Voice voices[2];

    void init(){
        voices[0].randomize(0);
        voices[1].randomize(1);
    }

    void randomize_hidden(float morph){
        // randomiza la voz menos dominante para preservar continuidad
        uint8_t slot = (morph < 0.5f) ? 1u : 0u;
        uint8_t fam  = (uint8_t)(rng_next()%3u);
        voices[slot].randomize(fam);
    }

    int16_t next(uint32_t t){
        float a=voices[0].sample(t, g_timbre);
        float b=voices[1].sample(t, g_timbre);
        float m=clamp01(g_morph);
        float out = a*(1.f-m) + b*m;
        out *= 0.78f;
        out = softclip(out * 1.15f);
        if(out> .95f)out= .95f;
        if(out<-.95f)out=-.95f;
        return (int16_t)(out*32767.f);
    }
};

// ── Pads resistivos ────────────────────────────────────────────────
static float   pad_baseline[4]={9300,9300,9300,9300};
static bool    pad_on[4]={},pad_prev[4]={};
static uint8_t pad_conf_on[4]={};
static uint32_t pad_hold_ms[4]={};

static uint32_t measure_pad_us(uint8_t c){
    gpio_set_dir(PIN_PAD[c],GPIO_OUT);
    gpio_put(PIN_PAD[c],1); sleep_us(300);
    gpio_put(PIN_PAD[c],0); sleep_us(800);
    gpio_set_dir(PIN_PAD[c],GPIO_IN);
    gpio_disable_pulls(PIN_PAD[c]);
    const uint32_t t0=time_us_32();
    while(!gpio_get(PIN_PAD[c])) {
        if((time_us_32()-t0)>=12000u) return 12000u;
    }
    return time_us_32()-t0;
}

static void calibrate_pads(){
    sleep_ms(200);
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
        uint32_t t=measure_pad_us(c);
        bool touched=(float(t)>pad_baseline[c]*1.20f);
        if(!touched && !pad_on[c])
            pad_baseline[c]+=0.0005f*(float(t)-pad_baseline[c]);
        if(!pad_on[c]){
            if(touched){
                if(++pad_conf_on[c]>=4){pad_on[c]=true;pad_hold_ms[c]=now;}
            } else {
                pad_conf_on[c]=0;
            }
        } else {
            if(!touched && (now-pad_hold_ms[c])>200u) pad_on[c]=false;
        }
    }
}

static float adc_direct(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){
        gpio_init(PIN_PAD[c]);gpio_set_dir(PIN_PAD[c],GPIO_IN);
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

    float morph_s = 0.5f;
    float filter_s = 1.0f;
    float timbre_s = 0.5f;

    while(true){
        // smoothing corto: limpia zipper sin meter lag grande
        morph_s  += 0.18f * (adc_direct(0) - morph_s);
        filter_s += 0.12f * (adc_direct(1) - filter_s);
        timbre_s += 0.15f * (adc_direct(2) - timbre_s);
        g_morph  = morph_s;
        g_filter = filter_s;
        g_timbre = timbre_s;

        scan_pads();

        uint32_t irq = save_and_disable_interrupts();
        if(pad_on[0]&&!pad_prev[0]){g_pad_event|=1u;gpio_put(PIN_LED,1);}        
        if(pad_on[1]&&!pad_prev[1]) g_pad_event|=2u;
        if(pad_on[2]&&!pad_prev[2]) g_pad_event|=4u;
        if(pad_on[3]&&!pad_prev[3]) g_pad_event|=8u;
        restore_interrupts(irq);

        if(!pad_on[0]) gpio_put(PIN_LED,0);
    }
}

int main(){
    const uint off=pio_add_program(g_pio,&pcm5102_i2s_program);
    g_sm=pio_claim_unused_sm(g_pio,true);
    pcm5102_i2s_program_init(g_pio,g_sm,off,PIN_DIN,PIN_BCLK,SR);
    for(int i=0;i<16;++i)i2s_write(0,0);

    static DrumEngine drums;
    drums.init();
    drums.set_params(0.28f,0.45f,0.85f);

    adc_init(); adc_gpio_init(26);
    uint32_t entropy=0;
    for(int i=0;i<64;++i){
        adc_select_input(0);
        entropy=entropy*1664525u+adc_read()+1013904223u;
    }
    g_rng^=entropy;

    static Synth synth;
    synth.init();

    multicore_launch_core1(core1_main);
    while(!g_ready)sleep_ms(10);

    static float lp_l=0.f, lp_r=0.f;
    static float out_hpx_l=0.f, out_hpy_l=0.f;
    static float out_hpx_r=0.f, out_hpy_r=0.f;

    uint32_t t=0,cr=0;
    while(true){
        if(++cr>=32u){
            cr=0;
            uint8_t ev;
            uint32_t irq = save_and_disable_interrupts();
            ev = g_pad_event;
            g_pad_event = 0;
            restore_interrupts(irq);

            if(ev){
                if(ev&1u) synth.randomize_hidden(g_morph);
                if(ev&2u) drums.trigger(DRUM_KICK);
                if(ev&4u) drums.trigger(DRUM_SNARE);
                if(ev&8u) drums.trigger(DRUM_HAT);
            }

            // TIMBRE mueve drums, pero con rango más musical y menos extremo
            drums.set_params(
                0.10f + g_timbre*0.55f,
                0.25f + g_timbre*0.45f,
                -1.f);
        }

        const int16_t ss=synth.next(t);
        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        // sidechain más moderado para que no se hunda todo el synth
        int32_t sc_soft = 24576 + ((sc * 8191) >> 15); // aprox 0.75..1.0
        const int32_t sd=((int32_t)ss*sc_soft)>>15;

        int32_t ol=sd + ((int32_t)dl * 3 >> 2);
        int32_t or_=sd + ((int32_t)dr * 3 >> 2);

        // LP global musical
        float fc=g_filter*g_filter*0.30f + 0.0025f;
        lp_l+=fc*(float(ol)-lp_l);
        lp_r+=fc*(float(or_)-lp_r);

        // HP muy leve final para evitar low-end embarrado/DC
        float hp_l = lp_l - out_hpx_l + out_hpy_l * 0.995f;
        out_hpx_l = lp_l; out_hpy_l = hp_l;
        float hp_r = lp_r - out_hpx_r + out_hpy_r * 0.995f;
        out_hpx_r = lp_r; out_hpy_r = hp_r;

        float ml = softclip(hp_l * 0.78f);
        float mr = softclip(hp_r * 0.78f);

        ol=(int32_t)(ml * 32767.f);
        or_=(int32_t)(mr * 32767.f);
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;

        i2s_write((int16_t)ol,(int16_t)or_);
        ++t;
    }
}

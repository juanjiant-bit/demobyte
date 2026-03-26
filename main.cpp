// Dem0!Byt3 V10 — tipo de fórmula libre + BPM lock + stereo
//
// FÓRMULAS: A y B son independientes, cada una puede ser BB o FB
//   BB: 12 fórmulas (melódicas, rítmicas, drones)
//   FB: 12 algos (armónicos, rítmicos, secuenciales, texturales)
//   randomize() elige tipo y fórmula independientemente → BB+BB, FB+FB, BB+FB
//
// MORPH: 0=voz A pura, 0.5=blend 50/50, 1=voz B pura
//   En el centro las dos naturalezas se mezclan directamente
//
// BPM MASTER: un tick counter compartido garantiza sincronía cada 4 beats
//   Voz A corre en tick/1, voz B en tick*2/3 (poliritmia 3:2)
//   Cada 4 beats las fases se alinean → frases de 4 compases
//
// STEREO:
//   Haas delay en voz B canal R (7ms) → imagen ancha instantánea
//   LFO de paneo independiente en A y B → movimiento orgánico lento
//   Detune sutil de 3 cents entre L y R en FB → chorus muy suave

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

static volatile float   g_morph=0.5f,g_macro1=0.5f,g_macro2=0.5f;
static volatile uint8_t g_pad_event=0;
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
static inline float fsaw(float p){return fw(p)*2.f-1.f;}
static inline float fclip(float x){return x/(1.f+fabsf(x));}
static inline float ffold(float x){
    x=x*.5f+.5f; x-=(int)x; if(x<0)x+=1.f; return x*2.f-1.f;
}

// ── BPM MASTER ────────────────────────────────────────────────────
// 120 BPM, 48 ticks por beat
// Tick period = 44100*60/(120*48) = ~459 samples por tick
constexpr uint32_t BPM         = 120;
constexpr uint32_t TICKS_BEAT  = 48;
constexpr uint32_t TICK_SAMP   = SR*60u/(BPM*TICKS_BEAT); // 459 samples/tick
constexpr uint32_t SYNC_TICKS  = 4*TICKS_BEAT;            // sincronía cada 4 beats

static uint32_t g_tick_accum = 0;
static uint32_t g_tick       = 0;  // contador global de ticks

// Voz A usa g_tick directamente
// Voz B usa g_tick * 2 / 3 (poliritmia 3:2, se sincroniza cada 3 beats de A = 2 de B)
static inline uint32_t tick_a(){ return g_tick; }
static inline uint32_t tick_b(){ return (g_tick * 2u) / 3u; }

// ── BYTEBEAT — 12 fórmulas ────────────────────────────────────────
// El 't' aquí es el tick del BPM master, no el sample counter
// Esto garantiza que los períodos sean múltiplos de divisiones musicales
static inline uint8_t bbf(uint8_t id, uint32_t t, uint8_t s){
    switch(id%12u){
    // ── Melódicas ────────────────────────────────────────────────
    case 0: return(uint8_t)(t*(t>>11&t>>8&57u));              // clásica Viznut
    case 1: return(uint8_t)((t*(t>>9|t>>13))|t>>6);           // arpeggio lento
    case 2: return(uint8_t)(t*((t>>8|t>>9)&63u));             // melodía corta
    case 3: return(uint8_t)((t>>7|t|t>>6)*10u+4u*(t&t>>13|t>>6)); // compleja
    // ── Rítmicas/percusivas ──────────────────────────────────────
    case 4: return(uint8_t)((t&t>>8)*(t>>4|t>>8));            // pulsante
    case 5: return(uint8_t)(((t*5u)&(t>>7))|((t*3u)&(t>>10)));// groovy
    case 6: return(uint8_t)(t*(((t>>11)&3u)+1u)&(t>>5));      // stutter
    case 7: return(uint8_t)(((t*7u)&(t>>6))|(t>>4));          // agresivo
    // ── Drones/ambiente ──────────────────────────────────────────
    case 8: return(uint8_t)(t^(t>>3)^(t>>6));                 // ruido suave
    case 9: return(uint8_t)((t>>2)^(t>>5)^(t>>7));            // textura fina
    case 10:return(uint8_t)((t*(t>>9))^(t>>7)^(t>>13));       // drone cambiante
    case 11:return(uint8_t)(((t*11u)&(t>>9))^((t*5u)&(t>>11)));// metallic
    default:return(uint8_t)t;
    }
}

// ── FLOATBEAT — 12 algos ─────────────────────────────────────────
static constexpr float PENTA[8]={1.f,1.189f,1.335f,1.498f,1.782f,2.f,2.378f,2.670f};
static constexpr float BASS[4]={.5f,.749f,1.f,.749f};

struct FbSt{
    float t=0,env=0,sph=0,drift=0;
    uint32_t lf=0xACE1u;
    // Para arpegio: step actual y fase dentro del step
    uint32_t arp_step=0;
    float    arp_ph=0;
};

// hz_detune: multiplicador sutil para el canal R (stereo chorus)
static float fbalgo(FbSt&st, float dt, float hz, float body, uint8_t algo,
                    float hz_detune=1.f){
    st.t+=dt; if(st.t>4096.f)st.t-=4096.f;
    // Drift muy lento — ±4% en ~6s — da vida sin vibrato obvio
    st.drift+=dt*0.17f; if(st.drift>6.28318f)st.drift-=6.28318f;
    float hzl=hz*hz_detune*(1.f+fs(st.drift*0.13f)*0.04f);
    const float t=st.t;
    hzl=hzl<15?15:(hzl>2400?2400:hzl);
    body=clamp01(body);

    switch(algo%12u){
    // ── Armónicas/tónicas ────────────────────────────────────────
    case 0:{  // sine stack — warm pad
        float x=fs(t*hzl)*(.50f+.20f*body)
               +fs(t*hzl*.5f)*(.28f+.18f*body)
               +fs(t*hzl*2.f)*.06f
               +fs(t*hzl*3.f)*.03f*(body);
        return fclip(x*.85f);
    }
    case 1:{  // sub organ — fundamental énfasis
        float x=fs(t*hzl*.5f)*.58f
               +fs(t*hzl)*.28f
               +ft(t*hzl*2.f)*.09f*(1.f+body);
        return fclip(x*.90f);
    }
    case 2:{  // ring + beating — metallic shimmer
        float a=fs(t*hzl), b=fs(t*(hzl*1.0047f));  // ~8 cents → beating
        float x=a*b*(.65f+.20f*body)+fs(t*hzl*.5f)*.22f;
        return fclip(x);
    }
    // ── Rítmicas con envelope interno ────────────────────────────
    case 3:{  // pluck — decay proporcional al hz
        float decay_rate=hzl*dt*2.5f;
        st.env+=decay_rate; if(st.env>1.f)st.env=0.f;
        float e=1.f-st.env*st.env;
        float fm=fs(t*hzl*2.f)*(.3f+.4f*body)*e;
        return fclip(fs(t*hzl+fm)*e*.90f);
    }
    case 4:{  // stutter — duty cycle variable
        float pw=.08f+.44f*(fs(t*hzl*.08f)*.5f+.5f);
        float x=fsq(t*hzl,pw)*(.55f+.25f*body)
               +fs(t*hzl*.5f)*.20f;
        return fclip(x*.75f);
    }
    case 5:{  // gate burst — patrón rítmico con subdivisión
        float gate=fsq(t*hzl*.0625f,.3f)*.5f+.5f;
        float x=(fs(t*hzl)+ft(t*hzl*2.f)*.28f)*gate;
        return fclip(x*(.72f+.20f*body));
    }
    // ── Secuenciales/melódicas ────────────────────────────────────
    case 6:{  // arpeggio pentatónica ascendente
        st.arp_ph+=dt*hzl*.03125f;
        uint8_t n=(uint8_t)(st.arp_ph)%8u;
        float f=hzl*PENTA[n];
        float x=fs(t*f)*(.60f+.20f*body)+fs(t*f*.5f)*.22f;
        return fclip(x*.85f);
    }
    case 7:{  // arpeggio pentatónica random — salta entre notas
        float prev=st.arp_ph; st.arp_ph+=dt*hzl*.025f;
        if((uint8_t)st.arp_ph!=(uint8_t)prev){
            st.lf^=st.lf<<7; st.lf^=st.lf>>9; st.lf^=st.lf<<8;
        }
        float f=hzl*PENTA[st.lf%8u];
        return fclip(fs(t*f)*(.72f+.18f*body));
    }
    case 8:{  // bass sequence — 4 notas que se repiten
        st.arp_ph+=dt*hzl*.015625f;
        uint8_t n=(uint8_t)(st.arp_ph)%4u;
        float f=hzl*BASS[n];
        float x=fs(t*f)*(.62f+.22f*body)+fs(t*f*.5f)*.26f;
        return fclip(x);
    }
    // ── Texturales/experimentales ─────────────────────────────────
    case 9:{  // FM caótico — índice crece con body
        float idx=1.5f+body*6.f;
        float mod=fs(t*(hzl*1.618f))*idx;  // razón áurea como ratio
        float x=fs(t*hzl+mod)*(.62f+.15f*body)
               +fs(t*hzl*.5f)*.18f;
        return fclip(x*.82f);
    }
    case 10:{ // wavefold con sweep lento
        float fold_amt=1.2f+body*2.8f+fs(t*hzl*.02f)*.8f;
        float x=ft(t*hzl)*fold_amt;
        return fclip(ffold(x)*(.65f+.20f*body));
    }
    case 11:{ // noise resonante — LP con retroalimentación del seno
        st.lf^=st.lf<<13; st.lf^=st.lf>>17; st.lf^=st.lf<<5;
        float noise=float((int32_t)st.lf)*(1.f/2147483648.f)*.25f;
        float res=fs(t*hzl)*(.35f+.35f*body);
        return fclip(res+noise*(1.f-body*.6f));
    }
    default:return 0.f;
    }
}

// ── VOICE: puede ser BB o FB, tipo elegido en randomize ───────────
struct Voice {
    bool     is_fb  = false;   // false=BB, true=FB
    // BB params
    uint8_t  bb_id=2, bb_seed=0, bb_morph=128;
    // FB params
    FbSt     fst;
    uint8_t  fb_id=0;
    float    fhz=110.f, fbody=.5f;
    // DSP
    float    lp=0.f;
    int32_t  dcx=0,dcy=0;
    float    fdcx=0,fdcy=0;

    void randomize(){
        is_fb  = (rng_next()&1u) != 0;  // 50% chance BB o FB
        bb_id  = (uint8_t)(rng_next()%12u);
        bb_seed= (uint8_t)(rng_next()>>24);
        bb_morph=(uint8_t)(rng_next()>>24);
        fb_id  = (uint8_t)(rng_next()%12u);
        fhz    = 55.f*powf(2.f, rng_f()*4.f);
        fbody  = .15f+rng_f()*.70f;
        fst    = FbSt{};
        lp=0.f; dcx=0; dcy=0; fdcx=0; fdcy=0;
    }

    // Genera L y R separados para imagen stereo
    // macro1 → hz (55..880Hz),  macro2 → fbody+bb_morph
    void sample_lr(uint32_t t_tick, float macro1, float macro2,
                   float& out_l, float& out_r){
        constexpr float DT=1.f/44100.f;

        // Aplicar macros
        float hz_use   = 55.f*powf(2.f, macro1*4.f);
        float body_use = clamp01(macro2);
        uint8_t morph_use=(uint8_t)(macro2*255.f);

        if(!is_fb){
            // ── BYTEBEAT ──────────────────────────────────────────
            // Usar t_tick del BPM master — sincronizado
            uint8_t raw_l = bbf(bb_id, t_tick, bb_seed);
            // Canal R: t_tick+1 para micro-offset → tiny stereo difference
            uint8_t raw_r = bbf(bb_id, t_tick+1u, bb_seed);

            // Mezcla bb_morph con una variante
            uint8_t alt_l = bbf((bb_id+3u)%12u, t_tick, bb_seed^0x55u);
            uint8_t alt_r = bbf((bb_id+3u)%12u, t_tick+1u, bb_seed^0x55u);
            raw_l=(uint8_t)(((uint16_t)raw_l*(255u-morph_use)+(uint16_t)alt_l*morph_use)>>8);
            raw_r=(uint8_t)(((uint16_t)raw_r*(255u-morph_use)+(uint16_t)alt_r*morph_use)>>8);

            float bbl=float((int8_t)(raw_l^0x80u))*(1.f/128.f);
            float bbr=float((int8_t)(raw_r^0x80u))*(1.f/128.f);

            // LP — cutoff controlado por macro2 (más cuerpo = más abierto)
            float lc=0.03f+body_use*0.10f;
            lp+=lc*(bbl-lp); bbl=lp*.58f+bbl*.42f;
            // R tiene su propio LP implícito (compartimos lp por simpleza)
            bbr=lp*.58f+bbr*.42f;

            // DC blocker (L)
            int32_t s32=(int32_t)(bbl*32767.f);
            int32_t y=s32-dcx+((dcy*252)>>8); dcx=s32;dcy=y;
            out_l=float(y)*(1.f/32767.f);
            // R aproximado
            s32=(int32_t)(bbr*32767.f);
            y=s32-dcx+((dcy*252)>>8);
            out_r=float(y)*(1.f/32767.f);

        } else {
            // ── FLOATBEAT ─────────────────────────────────────────
            // L: hz normal, R: hz+3 cents detune → chorus sutil
            float hz_r=hz_use*1.00174f;  // 3 cents = 2^(3/1200)

            // Necesitamos dos FbSt para L y R — usamos fst para L
            // y un offset de fase para R (simula segundo oscilador)
            FbSt fst_r=fst;
            fst_r.t+=0.007f;  // 7ms de offset de fase → efecto Haas en FB

            float fl=fbalgo(fst,  DT, hz_use, body_use, fb_id, 1.f);
            float fr=fbalgo(fst_r,DT, hz_r,   body_use, fb_id, 1.f);

            // DC blocker L
            float fly=fl-fdcx+fdcy*(252.f/256.f); fdcx=fl;fdcy=fly;
            out_l=fly;

            // DC blocker R (usa mismos coefs — ok para aproximación)
            float fry=fr-fdcx+fdcy*(252.f/256.f);
            out_r=fry;
        }
    }
};

// ── STEREO PAN LFO ────────────────────────────────────────────────
// Dos LFOs lentos e independientes para paneo suave de cada voz
struct PanLFO {
    float ph=0.f;
    float rate;     // Hz
    float depth;    // 0..1 (rango de paneo)

    PanLFO(float r, float d, float init_ph=0.f):rate(r),depth(d),ph(init_ph){}

    // Retorna (pan_l, pan_r) donde pan_l+pan_r puede ser >1 (no es potencia constante)
    // Usamos ley de paneo simple: L=cos(θ), R=sin(θ) con θ en 0..π/2
    void get(float dt, float& pan_l, float& pan_r){
        ph+=dt*rate; if(ph>1.f)ph-=1.f;
        // centro=0.5, mueve ±depth/2 alrededor del centro
        float pos=0.5f+fs(ph)*depth*0.5f;  // 0=izq, 1=der
        // Ley de paneo con potencia constante (aproximación)
        float angle=pos*1.5708f;  // 0..π/2
        pan_l=cosf(angle)*1.41421f;
        pan_r=sinf(angle)*1.41421f;
    }
};

// ── HAAS DELAY BUFFER ─────────────────────────────────────────────
// Delay de 7ms en canal R de voz B → imagen stereo ancha
constexpr uint32_t HAAS_SAMPLES = (SR*7)/1000;  // 309 samples @ 44100
static int16_t haas_buf[HAAS_SAMPLES] = {};
static uint32_t haas_wr = 0;

static int16_t haas_write_read(int16_t in){
    haas_buf[haas_wr]=in;
    haas_wr=(haas_wr+1)%HAAS_SAMPLES;
    return haas_buf[haas_wr];  // read con delay de HAAS_SAMPLES
}

// ── SYNTH ─────────────────────────────────────────────────────────
struct Synth {
    Voice    va, vb;
    Voice    old_a, old_b;
    float    xfade=1.f;
    PanLFO   pan_a{0.27f, 0.35f, 0.0f};   // 0.27Hz, ±35%, fase 0
    PanLFO   pan_b{0.19f, 0.35f, 0.5f};   // 0.19Hz, ±35%, fase opuesta

    void randomize(){
        old_a=va; old_b=vb;
        va.randomize(); vb.randomize();
        xfade=0.f;
    }

    void next(uint32_t master_sample, float morph, float macro1, float macro2,
              int16_t& out_l, int16_t& out_r){
        constexpr float DT=1.f/44100.f;

        float al,ar,bl,br;
        va.sample_lr(tick_a(), macro1, macro2, al, ar);
        vb.sample_lr(tick_b(), macro1, macro2, bl, br);

        // Crossfade de randomize (100ms)
        if(xfade<1.f){
            float f=xfade*xfade*(3.f-2.f*xfade);
            float oal,oar,obl,obr;
            old_a.sample_lr(tick_a(), macro1, macro2, oal, oar);
            old_b.sample_lr(tick_b(), macro1, macro2, obl, obr);
            al=oal*(1.f-f)+al*f; ar=oar*(1.f-f)+ar*f;
            bl=obl*(1.f-f)+bl*f; br=obr*(1.f-f)+br*f;
            xfade+=DT*10.f; if(xfade>1.f)xfade=1.f;
        }

        // MORPH: 0=A pura, 0.5=50/50, 1=B pura
        float ml=al*(1.f-morph)+bl*morph;
        float mr=ar*(1.f-morph)+br*morph;

        // Haas delay en canal R de voz B — ensancha la imagen
        float bl_haas=(float)haas_write_read((int16_t)(bl*32767.f))*(1.f/32767.f);
        // Aplicar el delay solo en la parte B del mix R
        mr=ar*(1.f-morph)+bl_haas*morph;

        // PAN LFO independiente por voz
        float pal,par,pbl,pbr;
        pan_a.get(DT,pal,par);
        pan_b.get(DT,pbl,pbr);

        // Mezcla con paneo: A paneada, B paneada al contrario
        float final_l=(al*pal+bl*pbl)*0.5f*(1.f-morph*0.3f);
        float final_r=(ar*par+br*pbr)*0.5f*(1.f-morph*0.3f);
        // Blend con el morph simple (para que morph=0.5 sea claramente híbrido)
        final_l=final_l*.6f+ml*.4f;
        final_r=final_r*.6f+mr*.4f;

        final_l*=0.82f; final_r*=0.82f;
        if(final_l> .92f)final_l= .92f; if(final_l<-.92f)final_l=-.92f;
        if(final_r> .92f)final_r= .92f; if(final_r<-.92f)final_r=-.92f;
        out_l=(int16_t)(final_l*32767.f);
        out_r=(int16_t)(final_r*32767.f);
    }
};

// ── PADS RESISTIVOS ───────────────────────────────────────────────
// Circuito: 3V3 → 10kΩ → GPIO, 100nF entre GPIO y GND
// Con dedo: el pad conecta GPIO a GND a través de R_piel → descarga rápida
constexpr uint32_t PAD_CHARGE_US=300, PAD_MAX_US=12000;
static float   pad_base_t=10000.f;
static bool    pad_on[4]={},pad_prev[4]={};
static uint8_t pad_conf[4]={};

static uint32_t measure_pad(uint8_t c){
    gpio_set_dir(PIN_PAD[c],GPIO_OUT); gpio_put(PIN_PAD[c],1);
    sleep_us(PAD_CHARGE_US);
    gpio_set_dir(PIN_PAD[c],GPIO_IN); gpio_disable_pulls(PIN_PAD[c]);
    const uint32_t t0=time_us_32();
    while(gpio_get(PIN_PAD[c]))
        if((time_us_32()-t0)>=PAD_MAX_US) return PAD_MAX_US;
    return time_us_32()-t0;
}

static void calibrate_pads(){
    sleep_ms(200);
    float sum=0; int n=0;
    for(int i=0;i<8;++i)
        for(uint8_t c=0;c<4;++c){sum+=float(measure_pad(c));++n;}
    pad_base_t=sum/float(n);
}

static void scan_pads(){
    for(uint8_t c=0;c<4;++c){
        pad_prev[c]=pad_on[c];
        float raw=float(measure_pad(c));
        // Presión: 0=sin toque (timeout), 1=toque fuerte (rápido)
        float p=clamp01(1.f-raw/pad_base_t);
        if(!pad_on[c]){
            if(p>0.28f){if(++pad_conf[c]>=2)pad_on[c]=true;}
            else pad_conf[c]=0;
        } else {
            if(p<0.12f){pad_on[c]=false;pad_conf[c]=0;}
        }
    }
}

// ── ADC ───────────────────────────────────────────────────────────
static float adc_ch(uint ch){adc_select_input(ch);return float(adc_read())/4095.f;}

static void core1_main(){
    gpio_init(PIN_LED);gpio_set_dir(PIN_LED,GPIO_OUT);
    for(uint8_t c=0;c<4;++c){gpio_init(PIN_PAD[c]);gpio_set_dir(PIN_PAD[c],GPIO_IN);gpio_disable_pulls(PIN_PAD[c]);}
    adc_init(); adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);

    gpio_put(PIN_LED,1); calibrate_pads();
    // Triple parpadeo = calibración lista
    for(int i=0;i<3;++i){gpio_put(PIN_LED,0);sleep_ms(120);gpio_put(PIN_LED,1);sleep_ms(120);}
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

    uint32_t sample=0, cr=0;
    while(true){
        // BPM tick counter
        g_tick_accum++;
        if(g_tick_accum>=TICK_SAMP){
            g_tick_accum=0;
            g_tick++;
            // Reset de fases cada SYNC_TICKS (4 beats) → punto de encuentro
            // No reseteamos el estado de síntesis, solo la alineación de tick_b
            // (tick_b = tick_a*2/3 ya garantiza sincronía en LCM=3 beats)
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
        synth.next(sample, g_morph, g_macro1, g_macro2, sl, sr);

        int16_t dl=0,dr=0; int32_t sc=32767;
        drums.process(dl,dr,sc);

        int32_t ol=((int32_t)sl*sc>>15)*19661/32768+(int32_t)dl*29491/32768;
        int32_t or_=((int32_t)sr*sc>>15)*19661/32768+(int32_t)dr*29491/32768;
        if(ol>32767)ol=32767; if(ol<-32768)ol=-32768;
        if(or_>32767)or_=32767; if(or_<-32768)or_=-32768;
        i2s_write((int16_t)ol,(int16_t)or_);
        ++sample;
    }
}

/** Live2D P4 — RGBA8888 + alpha + mask + keyform animation. Pure C, PSRAM. */
#include "lv2_render.h"
#include <string.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#define TAG "LV2"
#define PSRAM MALLOC_CAP_SPIRAM

static inline uint16_t rd16le(const uint8_t* d, int o) { return d[o]|(d[o+1]<<8); }
static inline uint32_t rd32le(const uint8_t* d, int o) { return rd16le(d,o)|((uint32_t)rd16le(d,o+2)<<16); }
static inline float rdfle(const uint8_t* d, int o) { uint32_t u=rd32le(d,o); float f; memcpy(&f,&u,4); return f; }
static inline int min3i(int a,int b,int c){int x=a<b?a:b;return x<c?x:c;}
static inline int max3i(int a,int b,int c){int x=a>b?a:b;return x>c?x:c;}
static inline uint16_t rgba565(uint32_t c){return(((c>>0)&0xFF)>>3)<<11|(((c>>8)&0xFF)>>2)<<5|(((c>>16)&0xFF)>>3);}
static inline uint16_t ablend(uint32_t s,uint16_t d){
    uint8_t sa=(s>>24)&0xFF;
    if(!sa)return d;
    if(sa>=255)return rgba565(s);
    uint8_t sr=s,sg=s>>8,sb=s>>16,dr=((d>>11)&0x1F)<<3,dg=((d>>5)&0x3F)<<2,db=(d&0x1F)<<3;
    int a=sa,iv=255-a;return(((sr*a+dr*iv)/255)>>3)<<11|(((sg*a+dg*iv)/255)>>2)<<5|(((sb*a+db*iv)/255)>>3);
}

Lv2RenderModel* lv2_load(const char* l2d, const char* tex) {
    FILE* f=fopen(l2d,"rb");
    if(!f){ESP_LOGE(TAG,"no %s",l2d);return NULL;}
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    uint8_t* b=heap_caps_malloc(sz,PSRAM);
    if(!b){fclose(f);return NULL;}
    fread(b,1,sz,f);fclose(f);
    uint32_t dc=rd32le(b,0),vt=rd32le(b,4),it=rd32le(b,8),ver=rd32le(b,12);
    ESP_LOGI(TAG,"Load: %lu dwb, %lu vtx, v%lu",(unsigned long)dc,(unsigned long)vt,(unsigned long)ver);
    Lv2RenderModel* m=heap_caps_calloc(1,sizeof(*m),PSRAM);
    if(!m){heap_caps_free(b);return NULL;}
    m->drawable_count=dc;m->total_verts=vt;m->total_indices=it;
    m->drawable_vc=heap_caps_malloc(dc*4,PSRAM);m->drawable_ic=heap_caps_malloc(dc*4,PSRAM);
    m->drawable_vo=heap_caps_malloc(dc*4,PSRAM);m->drawable_io=heap_caps_malloc(dc*4,PSRAM);
    m->positions=heap_caps_malloc(vt*8,PSRAM);m->uvs=heap_caps_malloc(vt*8,PSRAM);
    m->indices=heap_caps_malloc(it*2,PSRAM);m->mask_info=heap_caps_malloc(dc*2,PSRAM);
    if(!m->positions||!m->uvs||!m->indices||!m->mask_info){lv2_free_render(m);heap_caps_free(b);return NULL;}
    int o=16;
    for(int i=0;i<(int)dc;i++){m->drawable_vc[i]=rd32le(b,o);m->drawable_ic[i]=rd32le(b,o+4);m->drawable_vo[i]=rd32le(b,o+8);m->drawable_io[i]=rd32le(b,o+12);o+=16;}
    for(int i=0;i<(int)vt;i++){m->positions[i*2]=rdfle(b,o);m->positions[i*2+1]=rdfle(b,o+4);o+=8;}
    for(int i=0;i<(int)vt;i++){m->uvs[i*2]=rdfle(b,o);m->uvs[i*2+1]=rdfle(b,o+4);o+=8;}
    for(int i=0;i<(int)it;i++)m->indices[i]=rd16le(b,o+i*2);
    o+=it*2;
    for(int i=0;i<(int)dc;i++)m->mask_info[i]=-1;
    if(ver>=1&&o+6<=sz){uint32_t mg=rd32le(b,o);o+=4;
    if(mg==0x4D534B00){for(int i=0;i<(int)dc;i++){m->mask_info[i]=(int16_t)rd16le(b,o);o+=2;}}}
    heap_caps_free(b);
    f=fopen(tex,"rb");
    if(!f){ESP_LOGE(TAG,"no %s",tex);lv2_free_render(m);return NULL;}
    uint8_t h[4];fread(h,1,4,f);m->tex_w=rd16le(h,0);m->tex_h=rd16le(h,2);
    int tp=m->tex_w*m->tex_h;m->texture=heap_caps_malloc(tp*4,PSRAM);
    if(!m->texture){fclose(f);lv2_free_render(m);return NULL;}
    fread(m->texture,4,tp,f);fclose(f);
    ESP_LOGI(TAG,"Tex: %dx%d RGBA (%d KB)",m->tex_w,m->tex_h,tp*4/1024);
    return m;
}

void lv2_free_render(Lv2RenderModel* m){
    if(!m)return;
    if(m->drawable_vc)heap_caps_free(m->drawable_vc);
    if(m->drawable_ic)heap_caps_free(m->drawable_ic);
    if(m->drawable_vo)heap_caps_free(m->drawable_vo);
    if(m->drawable_io)heap_caps_free(m->drawable_io);
    if(m->positions)heap_caps_free(m->positions);
    if(m->uvs)heap_caps_free(m->uvs);
    if(m->indices)heap_caps_free(m->indices);
    if(m->mask_info)heap_caps_free(m->mask_info);
    if(m->mask_buf)heap_caps_free(m->mask_buf);
    if(m->kf_base_pos)heap_caps_free(m->kf_base_pos);
    if(m->kf_offsets)heap_caps_free(m->kf_offsets);
    if(m->texture)heap_caps_free(m->texture);
    heap_caps_free(m);
}

bool lv2_load_keyforms(Lv2RenderModel* m, const char* kf){
    FILE* f=fopen(kf,"rb");
    if(!f)return false;
    uint32_t h[3];fread(h,4,3,f);
    int fv=h[0],fp=h[1];m->kf_verts=fv;
    // We need these specific params: 0(AngleX),1(AngleY),2(AngleZ),7(EyeL),9(EyeR),6(Mouth)
    static const int needed[]={0,1,2,7,9,6}; // file pos: AngleX,Y,Z, EyeL,EyeR,Mouth
    m->kf_param_count=6;
    // Read all param headers, keep only needed ones
    float saved_range[6][3];int slot=0;
    long data_start=ftell(f);
    for(int i=0;i<fp;i++){
        char nm[64];fread(nm,1,64,f);float r[3];fread(r,4,3,f);
        for(int j=0;j<6;j++){if(i==needed[j]){saved_range[j][0]=r[0];saved_range[j][1]=r[1];saved_range[j][2]=r[2];}}
    }
    for(int j=0;j<6;j++){m->kf_param_range[j][0]=saved_range[j][0];m->kf_param_range[j][1]=saved_range[j][1];m->kf_param_range[j][2]=saved_range[j][2];}
    m->kf_base_pos=heap_caps_malloc(m->total_verts*8,PSRAM);
    if(!m->kf_base_pos){fclose(f);return false;}
    memcpy(m->kf_base_pos,m->positions,m->total_verts*8);
    // Load keyform data only for needed params
    int vs=fv*2; // floats per param
    m->kf_offsets=heap_caps_malloc(6*vs*4,PSRAM);
    if(!m->kf_offsets){fclose(f);return false;}
    for(int i=0;i<fp;i++){
        int target=-1;for(int j=0;j<6;j++){if(i==needed[j]){target=j;break;}}
        if(target>=0)fread(m->kf_offsets+target*vs,4,vs,f);
        else fseek(f,vs*4,SEEK_CUR);
    }
    fclose(f);ESP_LOGI(TAG,"KF: 6/%d params, %d KB",fp,6*vs*4/1024);return true;
}

void lv2_animate(Lv2RenderModel* m, float* out, float t){
    if(!m->kf_offsets||!m->kf_base_pos){memcpy(out,m->positions,m->total_verts*8);return;}
    memcpy(out,m->kf_base_pos,m->total_verts*8);
    // AngleX,Y,Z bigger values for visible head rotation (~15% of max 30deg = ~4.5deg)
    float head_x=sinf(t*1.3f)*0.15f,head_y=cosf(t*0.9f)*0.1f,head_z=sinf(t*1.7f+1)*0.08f;
    float blink=fmodf(t,3.0f)<0.35f?1.0f:0.0f; // 0.35s blink every 3s
    float mouth=(sinf(t*0.8f)+1)*0.05f;
    float v[]={head_x,head_y,head_z,blink,blink,mouth};
    // Params already in order during load: 0=AngleX,1=AngleY,2=AngleZ,3=EyeL,4=EyeR,5=Mouth
    for(int ai=0;ai<6&&ai<m->kf_param_count;ai++){
        float def=m->kf_param_range[ai][0],mn=m->kf_param_range[ai][1],mx=m->kf_param_range[ai][2];
        float ex=(mx-mn)*0.5f;
        if(fabsf(ex)<0.001f)ex=1.0f;
        float w=(def+v[ai]*ex-def)/ex,*kf=m->kf_offsets+ai*m->kf_verts*2;
        for(int vi=0;vi<m->kf_verts*2;vi++)out[vi]+=w*kf[vi]*3600.0f; // ppu scale
    }
}

// Forward declarations
static void rasterize_mask(Lv2RenderModel* m, int di, float* sp, int fw, int fh);
static void render_drawable(Lv2RenderModel* m, int di, float* sp, int fw, int fh, uint16_t* fb, bool chk);

static void render_core(Lv2RenderModel* m, uint16_t* fb, int fw, int fh, float* usePos){
    memset(fb,0,fw*fh*2);
    float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
    for(int i=0;i<m->total_verts;i++){
        float x=usePos[i*2],y=usePos[i*2+1];
        if(x<mnx)mnx=x;
        if(y<mny)mny=y;
        if(x>mxx)mxx=x;
        if(y>mxy)mxy=y;
    }
    float cx=(mxx+mnx)*0.5f,cy=(mxy+mny)*0.5f;
    float sx=(mxx-mnx)*0.5f,sy=(mxy-mny)*0.5f;
    if(sx<0.01f)sx=0.01f;
    if(sy<0.01f)sy=0.01f;
    float sc=(fw*0.5f)/sx,s2=(fh*0.5f)/sy;
    if(s2<sc)sc=s2;
    float* sp=(float*)heap_caps_calloc(m->total_verts*2,4,PSRAM);
    if(sp){for(int i=0;i<m->total_verts;i++){sp[i*2]=(usePos[i*2]-cx)*sc+fw*0.5f;sp[i*2+1]=fh-((usePos[i*2+1]-cy)*sc+fh*0.5f);}}
    int mb=(fw*fh+7)/8;
    if(!m->mask_buf)m->mask_buf=heap_caps_calloc(mb,1,PSRAM);
    for(int di=0;di<m->drawable_count;di++){
        int16_t mi=m->mask_info[di];
        if(mi==-2){
            // Mask drawable: update mask buffer AND render as normal
            if(m->mask_buf){memset(m->mask_buf,0,mb);rasterize_mask(m,di,sp,fw,fh);}
            render_drawable(m,di,sp,fw,fh,fb,false);
        }else render_drawable(m,di,sp,fw,fh,fb,mi>=0);
    }
    if(sp)heap_caps_free(sp);
}

void lv2_render_frame(Lv2RenderModel* m, uint16_t* fb, int fw, int fh){render_core(m,fb,fw,fh,m->positions);}

void lv2_render_animated(Lv2RenderModel* m, uint16_t* fb, int fw, int fh, float t){
    float* a=heap_caps_malloc(m->total_verts*8,PSRAM);
    if(!a){render_core(m,fb,fw,fh,m->positions);return;}
    lv2_animate(m,a,t);render_core(m,fb,fw,fh,a);heap_caps_free(a);
}

static void rasterize_mask(Lv2RenderModel* m, int di, float* sp, int fw, int fh){
    int vc=m->drawable_vc[di],ic=m->drawable_ic[di],vo=m->drawable_vo[di],io=m->drawable_io[di];
    float* pos=sp+vo*2;uint16_t* idx=m->indices+io;
    for(int ti=0;ti<ic;ti+=3){
        int i0=idx[ti],i1=idx[ti+1],i2=idx[ti+2];
    if(i0>=vc||i1>=vc||i2>=vc)continue;
        float x0=pos[i0*2],y0=pos[i0*2+1],x1=pos[i1*2],y1=pos[i1*2+1],x2=pos[i2*2],y2=pos[i2*2+1];
        int bmnx=min3i((int)x0,(int)x1,(int)x2),bmxx=max3i((int)x0,(int)x1,(int)x2);
        int bmny=min3i((int)y0,(int)y1,(int)y2),bmxy=max3i((int)y0,(int)y1,(int)y2);
        if(bmnx<0)bmnx=0;
    if(bmxx>=fw)bmxx=fw-1;
    if(bmny<0)bmny=0;
    if(bmxy>=fh)bmxy=fh-1;
        if(bmnx>=bmxx||bmny>=bmxy)continue;
        float area=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0);
    if(fabsf(area)<1e-6f)continue;
        float inv=1.0f/area;
        for(int py=bmny;py<=bmxy;py++){int row=(py*fw)/8;
            for(int px=bmnx;px<=bmxx;px++){
                float w0=((x1-px)*(y2-py)-(x2-px)*(y1-py))*inv,w1=((x2-px)*(y0-py)-(x0-px)*(y2-py))*inv;
                if(w0<-0.001f||w1<-0.001f||1-w0-w1<-0.001f)continue;
                m->mask_buf[row+px/8]|=(1<<(px%8));
            }
        }
    }
}

static void render_drawable(Lv2RenderModel* m, int di, float* sp, int fw, int fh, uint16_t* fb, bool chk){
    int tw=m->tex_w,th=m->tex_h,vc=m->drawable_vc[di],ic=m->drawable_ic[di],vo=m->drawable_vo[di],io=m->drawable_io[di];
    float* pos=sp+vo*2,*uv=m->uvs+vo*2;uint16_t* idx=m->indices+io;
    for(int ti=0;ti<ic;ti+=3){
        int i0=idx[ti],i1=idx[ti+1],i2=idx[ti+2];
    if(i0>=vc||i1>=vc||i2>=vc)continue;
        float x0=pos[i0*2],y0=pos[i0*2+1],u0=uv[i0*2],v0=uv[i0*2+1],x1=pos[i1*2],y1=pos[i1*2+1],u1=uv[i1*2],v1=uv[i1*2+1],x2=pos[i2*2],y2=pos[i2*2+1],u2=uv[i2*2],v2=uv[i2*2+1];
        int bmnx=min3i((int)x0,(int)x1,(int)x2),bmxx=max3i((int)x0,(int)x1,(int)x2),bmny=min3i((int)y0,(int)y1,(int)y2),bmxy=max3i((int)y0,(int)y1,(int)y2);
        if(bmnx<0)bmnx=0;
    if(bmxx>=fw)bmxx=fw-1;
    if(bmny<0)bmny=0;
    if(bmxy>=fh)bmxy=fh-1;
        if(bmnx>=bmxx||bmny>=bmxy)continue;
        float area=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0);
    if(fabsf(area)<1e-6f)continue;
        float inv=1.0f/area;
        for(int py=bmny;py<=bmxy;py++){uint8_t* mr=NULL;
    if(chk)mr=m->mask_buf+(py*fw)/8;
            for(int px=bmnx;px<=bmxx;px++){
                if(chk&&mr&&!(mr[px/8]&(1<<(px%8))))continue;
                float w0=((x1-px)*(y2-py)-(x2-px)*(y1-py))*inv,w1=((x2-px)*(y0-py)-(x0-px)*(y2-py))*inv,w2=1-w0-w1;
                if(w0<-0.001f||w1<-0.001f||w2<-0.001f)continue;
                float tu=u0*w0+u1*w1+u2*w2,tv=1.0f-(v0*w0+v1*w1+v2*w2);
                int tx=((int)(tu*tw)%tw+tw)%tw,ty=((int)(tv*th)%th+th)%th;
                uint32_t c=m->texture[ty*tw+tx];uint8_t a=(c>>24)&0xFF;
                if(!a)continue;
                int pi=py*fw+px;
                fb[pi]=(a>=255)?rgba565(c):ablend(c,fb[pi]);
            }
        }
    }
}

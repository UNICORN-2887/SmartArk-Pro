/** Live2D P4 — RGBA8888 + alpha + mask + keyform animation. Pure C, PSRAM. */
#include "lv2_render.h"
#include <string.h>
#include <math.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define TAG "LV2"
#define PSRAM MALLOC_CAP_SPIRAM

// ── 加载进度回调（加载动画用）──
static Lv2ProgressCb s_lv2_prog_cb = NULL;
void lv2_set_progress_cb(Lv2ProgressCb cb) { s_lv2_prog_cb = cb; }
static inline void lv2_prog(const char* s, int p) { if (s_lv2_prog_cb) s_lv2_prog_cb(s, p); }
#define LV2_CHUNK (256 * 1024)  // 分块读大小：读一块报一次进度并让出 CPU

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

Lv2RenderModel* lv2_load(const char* l2d, const char* tex0, const char* tex1) {
    FILE* f=fopen(l2d,"rb");
    if(!f){ESP_LOGE(TAG,"no %s",l2d);return NULL;}
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    uint8_t* b=heap_caps_malloc(sz,PSRAM);
    if(!b){fclose(f);return NULL;}
    // 分块读：逐块报字节进度并让出 CPU（LVGL 任务趁机刷加载动画）
    size_t rd=0;
    while(rd<(size_t)sz){
        size_t n=((size_t)sz-rd)<LV2_CHUNK?((size_t)sz-rd):LV2_CHUNK;
        if(fread(b+rd,1,n,f)!=n)break;
        rd+=n;lv2_prog("读取模型",(int)(rd*100/sz));
        if(n==LV2_CHUNK)vTaskDelay(1);
    }
    fclose(f);
    uint32_t dc=rd32le(b,0),vt=rd32le(b,4),it=rd32le(b,8),ver=rd32le(b,12);
    lv2_prog("解析模型",-1);  // 解析/分配阶段不可按字节量化
    ESP_LOGI(TAG,"Load: %lu dwb, %lu vtx, v%lu",(unsigned long)dc,(unsigned long)vt,(unsigned long)ver);
    Lv2RenderModel* m=heap_caps_calloc(1,sizeof(*m),PSRAM);
    if(!m){heap_caps_free(b);return NULL;}
    m->drawable_count=dc;m->total_verts=vt;m->total_indices=it;
    m->drawable_vc=heap_caps_malloc(dc*4,PSRAM);m->drawable_ic=heap_caps_malloc(dc*4,PSRAM);
    m->drawable_vo=heap_caps_malloc(dc*4,PSRAM);m->drawable_io=heap_caps_malloc(dc*4,PSRAM);
    m->positions=heap_caps_malloc(vt*8,PSRAM);m->uvs=heap_caps_malloc(vt*8,PSRAM);
    m->indices=heap_caps_malloc(it*2,PSRAM);m->mask_info=heap_caps_malloc(dc*2,PSRAM);
    m->tex_index=heap_caps_malloc(dc*2,PSRAM);
    if(!m->positions||!m->uvs||!m->indices||!m->mask_info||!m->tex_index){lv2_free_render(m);heap_caps_free(b);return NULL;}
    for(int i=0;i<(int)dc;i++)m->tex_index[i]=0;  // default: all use tex0
    int o=16;
    for(int i=0;i<(int)dc;i++){m->drawable_vc[i]=rd32le(b,o);m->drawable_ic[i]=rd32le(b,o+4);m->drawable_vo[i]=rd32le(b,o+8);m->drawable_io[i]=rd32le(b,o+12);o+=16;}
    for(int i=0;i<(int)vt;i++){m->positions[i*2]=rdfle(b,o);m->positions[i*2+1]=rdfle(b,o+4);o+=8;}
    for(int i=0;i<(int)vt;i++){m->uvs[i*2]=rdfle(b,o);m->uvs[i*2+1]=rdfle(b,o+4);o+=8;}
    for(int i=0;i<(int)it;i++)m->indices[i]=rd16le(b,o+i*2);
    o+=it*2;
    for(int i=0;i<(int)dc;i++)m->mask_info[i]=-1;
    if(ver>=1&&o+6<=sz){uint32_t mg=rd32le(b,o);o+=4;
    if(mg==0x4D534B00){for(int i=0;i<(int)dc;i++){m->mask_info[i]=(int16_t)rd16le(b,o);o+=2;}}}
    // v2: per-drawable texture index
    if(ver>=2&&o+6<=sz){uint32_t mg2=rd32le(b,o);o+=4;
    if(mg2==0x54455800){for(int i=0;i<(int)dc;i++){m->tex_index[i]=(int16_t)rd16le(b,o);o+=2;}}}
    // v3: multi-mask support
    m->mask_extra_n=heap_caps_calloc(dc,2,PSRAM);  // uint16 per drawable
    if(ver>=3&&o+6<=sz){uint32_t mg3=rd32le(b,o);o+=4;
    if(mg3==0x4D4D4B00){
        int totalExtra=0;
        for(int i=0;i<(int)dc;i++){m->mask_extra_n[i]=(uint16_t)rd16le(b,o);o+=2;totalExtra+=m->mask_extra_n[i];}
        if(totalExtra>0){
            ESP_LOGI(TAG,"v3 extra masks: %d total",totalExtra);
            m->mask_extras=heap_caps_malloc(totalExtra*2,PSRAM);
            if(m->mask_extras){for(int i=0;i<totalExtra;i++){m->mask_extras[i]=(int16_t)rd16le(b,o);o+=2;}}
            else ESP_LOGW(TAG,"v3 mask_extras alloc FAILED (%d B)",totalExtra*2);
        }
    }}
    heap_caps_free(b);
    // ── mask 槽位分配：同时存活的 mask 各自独立缓冲 ──
    m->mask_slots=heap_caps_calloc(dc,2,PSRAM);
    if(m->mask_slots){
        for(int i=0;i<(int)dc;i++)m->mask_slots[i]=-1;
        int* last_use=heap_caps_calloc(dc,4,PSRAM);
        if(last_use){
            for(int i=0;i<(int)dc;i++){int16_t mi2=m->mask_info[i];if(mi2>=0)last_use[mi2]=i;}
            int busy_until[16]={0};
            for(int i=0;i<(int)dc;i++){
                if(m->mask_info[i]!=-2)continue;
                int slot=0;
                for(int s=0;s<16;s++){if(busy_until[s]<=i){slot=s;break;}}
                busy_until[slot]=last_use[i];
                m->mask_slots[i]=(int16_t)slot;
            }
            heap_caps_free(last_use);
        }
    }
    // Load textures
    const char* texs[2]={tex0,tex1};
    for(int ti=0;ti<2;ti++){
        if(!texs[ti]) break;
        f=fopen(texs[ti],"rb");
        if(!f){if(ti==0){lv2_free_render(m);return NULL;} break;}
        uint8_t h[4];fread(h,1,4,f);m->tex_w[ti]=rd16le(h,0);m->tex_h[ti]=rd16le(h,2);
        int tp=m->tex_w[ti]*m->tex_h[ti];
        m->textures[ti]=heap_caps_malloc(tp*4,PSRAM);
        if(!m->textures[ti]){ESP_LOGE(TAG,"Tex[%d] alloc %d KB FAILED",ti,tp*4/1024);fclose(f);if(ti==0){lv2_free_render(m);return NULL;} break;}
        // 分块读纹理（4MB 级别），逐块报进度
        size_t trd=0;size_t tsz=(size_t)tp*4;
        while(trd<tsz){
            size_t n=(tsz-trd)<LV2_CHUNK?(tsz-trd):LV2_CHUNK;
            if(fread((uint8_t*)m->textures[ti]+trd,1,n,f)!=n)break;
            trd+=n;lv2_prog("读取纹理",(int)(trd*100/tsz));
            if(n==LV2_CHUNK)vTaskDelay(1);
        }
        fclose(f);
        m->tex_count=ti+1;
        ESP_LOGI(TAG,"Tex[%d]: %dx%d RGBA (%d KB)",ti,m->tex_w[ti],m->tex_h[ti],tp*4/1024);
    }
    lv2_prog("模型就绪",100);
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
    if(m->tex_index)heap_caps_free(m->tex_index);
    if(m->mask_extra_n)heap_caps_free(m->mask_extra_n);
    if(m->mask_extras)heap_caps_free(m->mask_extras);
    if(m->mask_buf)heap_caps_free(m->mask_buf);
    if(m->mask_slots)heap_caps_free(m->mask_slots);
    for(int i=0;i<16;i++){if(m->mask_bufs[i])heap_caps_free(m->mask_bufs[i]);}
    if(m->kf_base_pos)heap_caps_free(m->kf_base_pos);
    if(m->kf_offsets)heap_caps_free(m->kf_offsets);
    for(int i=0;i<32;i++){if(m->kf_sparse[i])heap_caps_free(m->kf_sparse[i]);}
    for(int ti=0;ti<4;ti++){if(m->textures[ti])heap_caps_free(m->textures[ti]);}
    heap_caps_free(m);
}

bool lv2_load_keyforms(Lv2RenderModel* m, const char* kf){
    FILE* f=fopen(kf,"rb");
    if(!f)return false;
    uint32_t h[3];fread(h,4,3,f);
    int fv=h[0],fp=h[1];m->kf_verts=fv;
    lv2_prog("读取关键帧",0);
    // Params: AngleX,Y,Z,EyeL,EyeR,Mouth,EyeLS,EyeRS,MouthF, EyeBallX,EyeBallY
    static const int needed[]={0,1,2,7,9,6,8,10,5, 11,12};
    m->kf_param_count=11;
    // Read all param headers, keep only needed ones
    float saved_range[11][3];
    for(int i=0;i<fp;i++){
        char nm[64];fread(nm,1,64,f);float r[3];fread(r,4,3,f);
        for(int j=0;j<11;j++){if(i==needed[j]){saved_range[j][0]=r[0];saved_range[j][1]=r[1];saved_range[j][2]=r[2];}}
    }
    for(int j=0;j<11;j++){m->kf_param_range[j][0]=saved_range[j][0];m->kf_param_range[j][1]=saved_range[j][1];m->kf_param_range[j][2]=saved_range[j][2];}
    m->kf_base_pos=heap_caps_malloc(m->total_verts*8,PSRAM);
    if(!m->kf_base_pos){fclose(f);return false;}
    memcpy(m->kf_base_pos,m->positions,m->total_verts*8);
    int vs=fv*2;
    m->kf_offsets=heap_caps_malloc(11*vs*4,PSRAM);
    if(!m->kf_offsets){fclose(f);return false;}
    for(int i=0;i<fp;i++){
        int target=-1;for(int j=0;j<11;j++){if(i==needed[j]){target=j;break;}}
        if(target>=0)fread(m->kf_offsets+target*vs,4,vs,f);
        else fseek(f,vs*4,SEEK_CUR);
        lv2_prog("读取关键帧",(int)((i+1)*100/fp));
        vTaskDelay(1);  // 逐 param 让出 CPU
    }
    fclose(f);ESP_LOGI(TAG,"KF: 11/%d params, %d KB",fp,11*vs*4/1024);
    lv2_prog("读取关键帧",100);
    return true;
}

// Generic keyform loader with custom param mapping (for Theresia etc.)
bool lv2_load_keyforms_ex(Lv2RenderModel* m, const char* kf, const int* needed, int n_needed){
    FILE* f=fopen(kf,"rb");
    if(!f){ESP_LOGE(TAG,"KF open FAILED: %s (errno=%d)",kf,errno);return false;}
    uint32_t h[3];fread(h,4,3,f);
    int fv=h[0],fp=h[1];m->kf_verts=fv;
    lv2_prog("读取关键帧",0);
    float saved_range[32][3]={0};
    for(int i=0;i<fp;i++){
        char nm[64];fread(nm,1,64,f);float r[3];fread(r,4,3,f);
        for(int j=0;j<n_needed;j++){if(i==needed[j]){saved_range[j][0]=r[0];saved_range[j][1]=r[1];saved_range[j][2]=r[2];}}
    }
    m->kf_param_count=n_needed;
    for(int j=0;j<n_needed;j++){m->kf_param_range[j][0]=saved_range[j][0];m->kf_param_range[j][1]=saved_range[j][1];m->kf_param_range[j][2]=saved_range[j][2];}
    m->kf_base_pos=heap_caps_malloc(m->total_verts*8,PSRAM);
    if(!m->kf_base_pos){ESP_LOGE(TAG,"KF base_pos alloc FAILED (%d B)",m->total_verts*8);fclose(f);return false;}
    memcpy(m->kf_base_pos,m->positions,m->total_verts*8);
    int vs=fv*2;
    m->kf_offsets=heap_caps_malloc(n_needed*vs*4,PSRAM);
    if(!m->kf_offsets){
        // Dense allocation failed — try sparse: read each param, count non-zeros, store compactly
        ESP_LOGW(TAG,"KF dense failed, trying sparse (%d params)...",n_needed);
        float* tmp=heap_caps_malloc(vs*4,PSRAM);
        if(!tmp){fclose(f);return false;}
        bool all_ok=true; int total_nz=0;
        for(int i=0;i<fp;i++){
            int target=-1;
            for(int j=0;j<n_needed;j++){if(i==needed[j]){target=j;break;}}
            if(target>=0){
                fread(tmp,4,vs,f);
                // Count non-zero vertices (check both dx and dy)
                int nz=0;
                for(int v=0;v<fv;v++){float dx=tmp[v*2],dy=tmp[v*2+1];if(fabsf(dx)>0.0001f||fabsf(dy)>0.0001f)nz++;}
                m->kf_sparse_n[target]=nz; total_nz+=nz;
                if(nz>0){
                    // Store as interleaved [vi:uint16 | dx:int16 | dy:int16], 6B per vertex
                    m->kf_sparse[target]=(uint16_t*)heap_caps_malloc(nz*6,PSRAM);
                    if(!m->kf_sparse[target]){ESP_LOGE(TAG,"KF sparse[%d] FAILED (%d entries)",target,nz);all_ok=false;break;}
                    uint16_t* sp=m->kf_sparse[target]; int wi=0;
                    for(int v=0;v<fv;v++){
                        float dx=tmp[v*2], dy=tmp[v*2+1];
                        if(fabsf(dx)>0.0001f||fabsf(dy)>0.0001f){
                            sp[wi++]=(uint16_t)v;
                            sp[wi++]=(uint16_t)(int16_t)(dx*10000.0f);
                            sp[wi++]=(uint16_t)(int16_t)(dy*10000.0f);
                        }
                    }
                }
            }else fseek(f,vs*4,SEEK_CUR);
            lv2_prog("读取关键帧",(int)((i+1)*100/fp));
            vTaskDelay(1);
        }
        heap_caps_free(tmp);
        if(!all_ok){
            for(int j=0;j<n_needed;j++){if(m->kf_sparse[j]){heap_caps_free(m->kf_sparse[j]);m->kf_sparse[j]=NULL;}}
            fclose(f);return false;
        }
        fclose(f);ESP_LOGI(TAG,"KF sparse: %d/%d params, %d entries (%d KB)",n_needed,fp,total_nz,total_nz*6/1024);
        return true;
    }
    ESP_LOGI(TAG,"KF reading %d/%d params, vs=%d...",n_needed,fp,vs);
    for(int i=0;i<fp;i++){
        int target=-1;
        for(int j=0;j<n_needed;j++){if(i==needed[j]){target=j;break;}}
        if(target>=0)fread(m->kf_offsets+target*vs,4,vs,f);
        else fseek(f,vs*4,SEEK_CUR);
        lv2_prog("读取关键帧",(int)((i+1)*100/fp));
        vTaskDelay(1);
    }
    fclose(f);ESP_LOGI(TAG,"KF: %d/%d params, %d KB",n_needed,fp,n_needed*vs*4/1024);
    lv2_prog("读取关键帧",100);
    return true;
}

static void lv2_expr_override(float v[11], int ex, float t);

void lv2_animate(Lv2RenderModel* m, float* out, float t, float eye_x, float eye_y, float shy){
    // Check if ANY keyform data exists (dense OR sparse)
    bool has_kf=(m->kf_offsets!=NULL)||(m->kf_param_count>0&&m->kf_sparse[0]!=NULL);
    if(!has_kf||!m->kf_base_pos){memcpy(out,m->positions,m->total_verts*8);return;}
    memcpy(out,m->kf_base_pos,m->total_verts*8);
    // AngleX,Y,Z bigger values for visible head rotation (~15% of max 30deg = ~4.5deg)
    float head_x=sinf(t*1.3f)*0.15f,head_y=cosf(t*0.9f)*0.1f,head_z=sinf(t*1.7f+1)*0.08f;
    float blink=fmodf(t,3.0f)<0.35f?1.0f:0.0f;
    float mouth=(sinf(t*0.8f)+1)*0.05f;
    float smile=(sinf(t*0.15f+2)+1)*0.5f;
    float tremble=shy*sinf(t*8.0f)*0.06f;  // subtle shiver
    float lean=shy*0.04f;                  // slight body lean
    float tilt_down=shy*0.06f;             // head tilt
    float blush=shy*1.2f;
    float eye_blink = (shy>0.05f)?1.0f:blink; // shy → keep eyes closed
    // Order: AngleX(Yaw),AngleY(Pitch),AngleZ(Roll),EyeL,EyeR,Mouth,EyeLS,EyeRS,MouthF,EyeBX,EyeBY
    float v[]={head_y+lean,head_x+tilt_down,head_z+tremble, eye_blink,eye_blink,mouth, (smile>blush?smile:blush),(smile>blush?smile:blush), smile*0.3f, eye_x,eye_y};

    // ── Expression override (delegated to helper to keep auto path lean) ──
    if(m->expression>0){ lv2_expr_override(v,m->expression,t); }
    // ── 模型专属瞳孔映射（单点注入：默认/追踪/表情路径统一生效）──
    if(m->eye_cx!=0.0f||m->eye_cy!=0.0f){
        v[9]+=m->eye_cx; v[10]+=m->eye_cy;
        if(v[9]<-1.0f) v[9]=-1.0f;
        if(v[9]>1.0f) v[9]=1.0f;
        if(v[10]<-1.0f) v[10]=-1.0f;
        if(v[10]>1.0f) v[10]=1.0f;
    }
    if(m->eye_vm!=0.0f||m->eye_vup!=0.0f||m->eye_vdown!=0.0f){
        float g=v[10];
        if(g<=m->eye_gm) {
            v[10]=m->eye_vm+(m->eye_vup-m->eye_vm)*(g-m->eye_gm)/(-1.0f-m->eye_gm);
        } else {
            v[10]=m->eye_vm+(m->eye_vdown-m->eye_vm)*(g-m->eye_gm)/(1.0f-m->eye_gm);
        }
    }
    static int log_cnt=0;
    if((log_cnt++&63)==0) ESP_LOGI("LV2_EYE","expr=%d",m->expression);
    for(int ai=0;ai<11&&ai<m->kf_param_count;ai++){
        float def=m->kf_param_range[ai][0],mn=m->kf_param_range[ai][1],mx=m->kf_param_range[ai][2];
        float ex=(mx-mn)*0.5f;
        if(fabsf(ex)<0.001f)ex=1.0f;
        float w=(def+v[ai]*ex-def)/ex;
        float ppu=m->ppu>0?m->ppu:3600.0f;
        if(m->kf_sparse[ai]){ // Sparse path (uint16 vertex index, int16 deltas)
            uint16_t* sp=m->kf_sparse[ai]; int n=m->kf_sparse_n[ai];
            float ws=w*ppu/10000.0f;
            for(int e=0;e<n;e++){
                int vi=(int)sp[e*3]*2;
                out[vi]  +=ws*(float)(int16_t)sp[e*3+1];
                out[vi+1]+=ws*(float)(int16_t)sp[e*3+2];
            }
        }else if(m->kf_offsets){ // Dense path
            float*kf=m->kf_offsets+ai*m->kf_verts*2;
            for(int vi=0;vi<m->kf_verts*2;vi++)out[vi]+=w*kf[vi]*ppu;
        }
    }
}

// ── Expression override (separate function = no I-cache penalty for auto mode) ──
static void lv2_expr_override(float v[11], int ex, float t){
    #define FS(x) ({float _w=((x)-(int)(x)-0.5f)*6.2831853f,_w2=_w*_w;_w*(1.0f-_w2*(0.16666667f-_w2*0.00833333f));})
    if(ex!=1&&ex!=11) v[2]+=FS(t*(1.0f/3.0f))*0.30f;
    switch(ex){
    case 1: {float p=t-((int)(t/2.0f))*2.0f;
        v[0]+=FS(p*0.5f+0.75f)*0.30f; v[2]+=FS(p*0.5f+0.75f)*0.30f;
        v[1]+=FS(p+0.25f)*0.10f+0.04f; v[3]=v[4]=1.0f; v[8]=-0.30f;} break;
    case 2: v[9]=FS(t*0.25f)*0.60f; v[1]+=FS(t*0.079577f)*0.03f;
        v[3]=v[4]=0.40f; v[6]=v[7]=-0.50f; v[8]=-0.30f; v[10]=1.0f; break;
    case 3: v[5]=1.0f; v[8]=1.0f; break;
    case 4: v[3]=1.0f; v[5]=0.75f; v[8]=0.30f; break;
    case 5: case 6: v[3]=0.36f;v[4]=0.0f;v[6]=0.25f;v[8]=1.0f;v[9]=1.0f;v[10]=-1.0f; break;
    case 7: v[5]=0.75f; v[8]=0.60f; break;
    case 8: v[3]=v[4]=0.66f;v[5]=0.20f;v[8]=0.30f;v[10]=0.1f; break;
    case 9: v[3]=v[4]=0.30f;v[5]=0.34f;v[10]=1.0f; break;
    case 10: v[3]=v[4]=0.25f;v[5]=0.33f;v[6]=v[7]=0.64f; break;
    case 11: v[2]+=FS(t*8.0f)*0.06f; v[0]+=0.04f; v[1]+=0.06f;
        v[3]=v[4]=1.0f;v[6]=v[7]=0.0f;v[8]=-0.30f;v[9]=v[10]=0.0f; break;
    }
}

static void rasterize_mask(Lv2RenderModel* m, int di, float* sp, int fw, int fh, uint8_t* dst);
static void render_drawable(Lv2RenderModel* m, int di, float* sp, int fw, int fh, uint16_t* fb, bool chk, uint8_t* mbuf);

static uint8_t* mask_slot_buf(Lv2RenderModel* m, int di, int mb) {
    int slot=(m->mask_slots&&m->mask_slots[di]>=0)?m->mask_slots[di]:0;
    if(!m->mask_bufs[slot])m->mask_bufs[slot]=heap_caps_calloc(mb,1,PSRAM);
    return m->mask_bufs[slot];
}

static void render_core(Lv2RenderModel* m, uint16_t* fb, int fw, int fh, float* usePos){
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
    sc*=0.8f; // 80% scale = 64% pixels = ~1.5x faster
    float* sp=(float*)heap_caps_calloc(m->total_verts*2,4,PSRAM);
    if(sp){for(int i=0;i<m->total_verts;i++){sp[i*2]=(usePos[i*2]-cx)*sc+fw*0.5f;sp[i*2+1]=fh-((usePos[i*2+1]-cy)*sc+fh*0.5f);}}
    int mb=(fw*fh+7)/8;
    if(!m->mask_buf)m->mask_buf=heap_caps_calloc(mb,1,PSRAM);
    static uint8_t* mb_extra=NULL;
    for(int di=0;di<m->drawable_count;di++){
        int16_t mi=m->mask_info[di];
        if(mi==-999)continue;
        if(mi==-2){
            uint8_t* sb=mask_slot_buf(m,di,mb);
            if(sb){memset(sb,0,mb);rasterize_mask(m,di,sp,fw,fh,sb);}
            render_drawable(m,di,sp,fw,fh,fb,false,NULL);
        }else{
            // v3 multi-mask: rasterize extra masks, OR into extra buffer
            extern uint8_t* _lv2_mask_extra;
            _lv2_mask_extra=NULL;
            if(mi>=0&&m->mask_extra_n&&m->mask_extra_n[di]>0&&m->mask_extras){
                if(!mb_extra)mb_extra=heap_caps_calloc(mb,1,PSRAM);
                if(mb_extra){
                    memset(mb_extra,0,mb);
                    int eo=0;for(int k=0;k<di;k++)eo+=m->mask_extra_n[k];
                    for(int e=0;e<m->mask_extra_n[di];e++){
                        int ed=m->mask_extras[eo+e];
                        if(ed>=0&&ed<m->drawable_count&&m->mask_info[ed]==-2){
                            uint8_t* tb=mask_slot_buf(m,ed,mb);
                            if(tb){memset(m->mask_buf,0,mb);rasterize_mask(m,ed,sp,fw,fh,m->mask_buf);}
                            for(int b=0;b<mb;b++)mb_extra[b]|=m->mask_buf[b];
                        }
                    }
                    _lv2_mask_extra=mb_extra;
                }
            }
            uint8_t* mbuf=(mi>=0)?mask_slot_buf(m,(int)mi,mb):NULL;
            render_drawable(m,di,sp,fw,fh,fb,mi>=0,mbuf);
        }
    }
    if(sp)heap_caps_free(sp);
}
uint8_t* _lv2_mask_extra=NULL;

void lv2_render_frame(Lv2RenderModel* m, uint16_t* fb, int fw, int fh){render_core(m,fb,fw,fh,m->positions);}

void lv2_render_animated(Lv2RenderModel* m, uint16_t* fb, int fw, int fh, float t){
    float* a=heap_caps_malloc(m->total_verts*8,PSRAM);
    if(!a){render_core(m,fb,fw,fh,m->positions);return;}
    lv2_animate(m,a,t,m->eye_x,m->eye_y,m->shy);render_core(m,fb,fw,fh,a);heap_caps_free(a);
}

static void rasterize_mask(Lv2RenderModel* m, int di, float* sp, int fw, int fh, uint8_t* dst){
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
                dst[row+px/8]|=(1<<(px%8));
            }
        }
    }
}

static void render_drawable(Lv2RenderModel* m, int di, float* sp, int fw, int fh, uint16_t* fb, bool chk, uint8_t* mbuf){
    int ti=m->tex_index[di]; if(ti<0||ti>=m->tex_count||!m->textures[ti])ti=0;
    if(!m->textures[ti]) return;
    uint32_t* tex=m->textures[ti];
    int tw=m->tex_w[ti],th=m->tex_h[ti],vc=m->drawable_vc[di],ic=m->drawable_ic[di],vo=m->drawable_vo[di],io=m->drawable_io[di];
    if(vo+vc>m->total_verts||io+ic>m->total_indices){ESP_LOGE("LV2","BAD drawable %d: vo=%d vc=%d/%d io=%d ic=%d/%d",di,vo,vc,m->total_verts,io,ic,m->total_indices);return;}
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
        for(int py=bmny;py<=bmxy;py++){uint8_t* mr=NULL,*me=NULL;
    if(chk){mr=mbuf?mbuf+(py*fw)/8:NULL; extern uint8_t* _lv2_mask_extra; if(_lv2_mask_extra)me=_lv2_mask_extra+(py*fw)/8;}
            for(int px=bmnx;px<=bmxx;px++){
                if(chk&&!( (mr&&(mr[px/8]&(1<<(px%8)))) || (me&&(me[px/8]&(1<<(px%8)))) ))continue;
                float w0=((x1-px)*(y2-py)-(x2-px)*(y1-py))*inv,w1=((x2-px)*(y0-py)-(x0-px)*(y2-py))*inv,w2=1-w0-w1;
                if(w0<-0.001f||w1<-0.001f||w2<-0.001f)continue;
                float tu=u0*w0+u1*w1+u2*w2,tv=1.0f-(v0*w0+v1*w1+v2*w2);
                int tx=((int)(tu*tw)%tw+tw)%tw,ty=((int)(tv*th)%th+th)%th;
                uint32_t c=tex[ty*tw+tx];uint8_t a=(c>>24)&0xFF;
                if(!a)continue;
                int pi=py*fw+px;
                fb[pi]=(a>=255)?rgba565(c):ablend(c,fb[pi]);
            }
        }
    }
}

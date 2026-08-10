/** MOC3 parser v0.3 — pointer-based, matches real Amiya v5 model. Pure C, PSRAM alloc. */
#include "moc3_parser.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#define TAG "MOC3"
#define PSRAM MALLOC_CAP_SPIRAM
#define ALLOC(n,t) (t*)heap_caps_calloc((n),sizeof(t),PSRAM)
#define ALLOC_RAW(sz) heap_caps_malloc((sz),PSRAM)

static inline uint32_t rd32(const uint8_t* d, size_t off, bool be){
    if(be)return((uint32_t)d[off]<<24)|(d[off+1]<<16)|(d[off+2]<<8)|d[off+3];
    return((uint32_t)d[off+3]<<24)|(d[off+2]<<16)|(d[off+1]<<8)|d[off];
}
static inline float rdf(const uint8_t* d, size_t off, bool be){
    uint32_t u=rd32(d,off,be);float f;memcpy(&f,&u,4);return f;
}

Lv2Model* lv2_parse_moc3(const uint8_t* data, size_t size){
    if(!data||size<128){ESP_LOGE(TAG,"Too small");return NULL;}
    if(data[0]!='M'||data[1]!='O'||data[2]!='C'||data[3]!='3'){ESP_LOGE(TAG,"Bad magic");return NULL;}
    MocVersion ver=(MocVersion)data[4];
    bool be=data[5]!=0;
    ESP_LOGI(TAG,"MOC3 v%d, bigEndian=%d, %u bytes",(int)ver,(int)be,(unsigned)size);

    // ── SectionOffsetTable starts at offset 64 (Header is 64 bytes) ──
    // The hexpat's padding[0x280] is virtual ([[no_unique_address]]).
    // First field: u32 pointer to CountInfoTable
    uint32_t ct_off = rd32(data,64,be);
    // Second field: u32 pointer to CanvasInfo
    uint32_t ci_off = rd32(data,68,be);
    ESP_LOGI(TAG,"CountTable@%lu Canvas@%lu",(unsigned long)ct_off,(unsigned long)ci_off);

    // ── Read CanvasInfo ──
    float ppu=0, ox=0, oy=0, cw=0, ch=0;
    if(ci_off && ci_off+20<=size){
        ppu=rdf(data,ci_off,be);ox=rdf(data,ci_off+4,be);oy=rdf(data,ci_off+8,be);
        cw=rdf(data,ci_off+12,be);ch=rdf(data,ci_off+16,be);
    }

    // ── Read CountInfoTable ──
    MocCounts c; memset(&c,0,sizeof(c));
    if(ct_off){
        #define RCI(i) rd32(data,ct_off+(i)*4,be)
        c.parts=RCI(0);c.deformers=RCI(1);c.warp_deformers=RCI(2);c.rotation_deformers=RCI(3);
        c.art_meshes=RCI(4);c.parameters=RCI(5);c.part_keyforms=RCI(6);c.warp_keyforms=RCI(7);
        c.rotation_keyforms=RCI(8);c.artmesh_keyforms=RCI(9);c.keyform_positions=RCI(10);
        c.param_binding_indices=RCI(11);c.keyform_bindings=RCI(12);c.param_bindings=RCI(13);
        c.keys=RCI(14);c.uvs=RCI(15);c.position_indices=RCI(16);c.drawable_masks=RCI(17);
        c.draw_order_groups=RCI(18);c.draw_order_group_objects=RCI(19);c.glue=RCI(20);
        c.glue_info=RCI(21);c.glue_keyforms=RCI(22);
        if(ver>=MOC3_V4_02){
            c.keyform_multiply_colors=RCI(23);c.keyform_screen_colors=RCI(24);
            c.blend_shape_param_bindings=RCI(25);c.blend_shape_keyform_bindings=RCI(26);
            c.blend_shapes_warp=RCI(27);c.blend_shapes_artmesh=RCI(28);
            c.blend_shape_constraint_indices=RCI(29);c.blend_shape_constraints=RCI(30);
            c.blend_shape_constraint_values=RCI(31);
        }
        #undef RCI
    }
    ESP_LOGI(TAG,"Counts: %lu parts, %lu drawables, %lu params, %lu uvs, %lu posIdx",
        (unsigned long)c.parts,(unsigned long)c.art_meshes,(unsigned long)c.parameters,
        (unsigned long)c.uvs,(unsigned long)c.position_indices);

    // ── Allocate model ──
    Lv2Model* m=ALLOC(1,Lv2Model);if(!m){ESP_LOGE(TAG,"Alloc model failed");return NULL;}
    m->version=ver;m->is_big_endian=be;m->pixels_per_unit=ppu;m->origin_x=ox;m->origin_y=oy;
    m->canvas_w=cw;m->canvas_h=ch;m->counts=c;
    m->parts=ALLOC(c.parts,Lv2Part);
    m->params=ALLOC(c.parameters,Lv2Param);
    m->drawables=ALLOC(c.art_meshes,Lv2Drawable);
    m->warp_deformers=ALLOC(c.warp_deformers,Lv2WarpDeformer);
    if(!m->parts||!m->params||!m->drawables||!m->warp_deformers){ESP_LOGE(TAG,"Alloc arrays failed");lv2_free_model(m);return NULL;}

    // ── Read Parameter IDs + defaults ──
    // SectionOffsetTable stores pointers to each data section. After ct_off and ci_off (2×4=8 bytes),
    // the next pointers point to each section's offset table. The layout (from hexpat):
    //   [2] countInfo, canvasInfo
    //   [N] PartOffsets → part_ids
    //   [N] DeformerOffsets → deformer_ids
    //   ... many sections ...
    //   [N] ParameterOffsets → param_ids, param_defaults, param_mins, param_maxs
    //
    // For MVP we just need parameter IDs. Let's find the Parameter section:
    // It's after: parts(22*7=154) + deformers(177*4=708) + rotation(37*4=148) + artmesh(126*5=630)
    // + partKF(22*4=88) + warpKF(807*4=3228) + rotKF(119*4=476) + amKF(273*4=1092)
    // + kfPos(645792) + pbIdx(78*4=312) + kfBind(63*4=252) + pBind(67*4=268) + keys(700) = huge...
    //
    // Actually each section's offsets are stored as a table of u32 pointers, then each pointer
    // points to the actual data. The SectionOffsetTable stores a POINTER to each section's
    // offset table. The structure is: for each section type:
    //   u32 ptr_to_offset_table  →  [N×u32 offsets]  →  N data items
    //
    // For params (4 sub-tables: ids, defaults, mins, maxs), there are 4 pointer-to-offset-table entries.
    // These are at known positions in the SectionOffsetTable.
    // Let me calculate: after all sections before parameters, offset into SectionOffsetTable.

    // Skip to Parameter section in SectionOffsetTable (offset 64):
    // Part sections: parts(22) → 7 sub-tables each: id, kfSrcIdx, kfSrcBegin, kfSrcCnt, vis, en, parent
    // Deformer sections: warp(140): id, kfSrcIdx, vis → 3 sub-tables
    // Rotation sections: rot(37): id, kfSrcIdx, vis → 3 sub-tables
    // ArtMesh sections: 5 sub-tables
    // Keyform sections: partKF(4), warpKF(4), rotKF(4), amKF(5) = 17 sub-tables
    // kfPos(1), pbIdx(1), kfBind(1), pBind(1), keys(1), uvs(1), posIdx(1) = 7 sub-tables
    // drawMasks(1), drawOrdGrp(1), drawOrdObj(1), glue(1), glueInfo(1), glueKF(1) = 6 sub-tables
    // Total sub-tables before params: 7+3+3+5+17+7+6 = 48 sub-tables
    // Each sub-table = 1 u32 pointer. So params start at SectionOffsetTable + 2 + 48*4 = 64 + 8 + 192 = 264

    size_t sot = 64; // SectionOffsetTable start
    uint32_t param_id_ptr = rd32(data, sot + 8 + 48*4, be);   // param IDs pointer
    uint32_t param_def_ptr = rd32(data, sot + 8 + 49*4, be);  // param defaults pointer
    uint32_t param_min_ptr = rd32(data, sot + 8 + 50*4, be);  // param mins pointer
    uint32_t param_max_ptr = rd32(data, sot + 8 + 51*4, be);  // param maxs pointer

    if(param_id_ptr && param_id_ptr < size){
        for(uint32_t i=0;i<c.parameters;i++){
            uint32_t id_off = rd32(data, param_id_ptr + i*4, be);
            if(id_off && id_off+64<=size){memcpy(m->params[i].id, data+id_off, 64);m->params[i].id[63]=0;}
        }
    }
    if(param_def_ptr && param_def_ptr < size){
        for(uint32_t i=0;i<c.parameters;i++){
            uint32_t o=rd32(data, param_def_ptr + i*4, be);
            if(o && o+4<=size)m->params[i].default_value=m->params[i].current_value=rdf(data,o,be);
        }
    }
    if(param_min_ptr && param_min_ptr < size){
        for(uint32_t i=0;i<c.parameters;i++){
            uint32_t o=rd32(data, param_min_ptr + i*4, be);
            if(o && o+4<=size)m->params[i].min_value=rdf(data,o,be);
        }
    }
    if(param_max_ptr && param_max_ptr < size){
        for(uint32_t i=0;i<c.parameters;i++){
            uint32_t o=rd32(data, param_max_ptr + i*4, be);
            if(o && o+4<=size)m->params[i].max_value=rdf(data,o,be);
        }
    }

    ESP_LOGI(TAG,"MOC3 parsed OK: %lu params loaded",(unsigned long)c.parameters);
    return m;
}

void lv2_free_model(Lv2Model* m){
    if(!m)return;
    if(m->parts)heap_caps_free(m->parts);
    if(m->params)heap_caps_free(m->params);
    if(m->drawables)heap_caps_free(m->drawables);
    if(m->warp_deformers)heap_caps_free(m->warp_deformers);
    if(m->keyform_positions)heap_caps_free(m->keyform_positions);
    if(m->param_binding_indices)heap_caps_free(m->param_binding_indices);
    if(m->keyform_bindings)heap_caps_free(m->keyform_bindings);
    if(m->param_bindings)heap_caps_free(m->param_bindings);
    if(m->keys)heap_caps_free(m->keys);
    if(m->_psram_pool)heap_caps_free(m->_psram_pool);
    heap_caps_free(m);
}
int lv2_find_param(Lv2Model* m, const char* id){
    for(uint32_t i=0;i<m->counts.parameters;i++)if(strcmp(m->params[i].id,id)==0)return(int)i;
    return -1;
}
int lv2_find_drawable(Lv2Model* m, const char* id){
    for(uint32_t i=0;i<m->counts.art_meshes;i++)if(strcmp(m->drawables[i].id,id)==0)return(int)i;
    return -1;
}

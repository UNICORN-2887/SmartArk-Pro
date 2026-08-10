/** Live2D Cubism 4/5 .moc3 file parser — pure C, no Cubism Core dependency.
 *  Based on OpenL2D moc3ingbird format spec v2.1b.
 *  Supports MOC3 v3.0, v3.3, v4.0, v4.2, v5.0.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOC3_V3_00 = 1,
    MOC3_V3_03 = 2,
    MOC3_V4_00 = 3,
    MOC3_V4_02 = 4,
    MOC3_V5_00 = 5,
} MocVersion;

typedef struct {
    float x, y;
} Lv2Vec2;

typedef struct {
    float u, v;
} Lv2UV;

// ── Counts (from CountInfoTable) ──
typedef struct {
    uint32_t parts;
    uint32_t deformers;
    uint32_t warp_deformers;
    uint32_t rotation_deformers;
    uint32_t art_meshes;       // drawables
    uint32_t parameters;
    uint32_t part_keyforms;
    uint32_t warp_keyforms;
    uint32_t rotation_keyforms;
    uint32_t artmesh_keyforms;
    uint32_t keyform_positions;
    uint32_t param_binding_indices;
    uint32_t keyform_bindings;
    uint32_t param_bindings;
    uint32_t keys;
    uint32_t uvs;
    uint32_t position_indices;
    uint32_t drawable_masks;
    uint32_t draw_order_groups;
    uint32_t draw_order_group_objects;
    uint32_t glue;
    uint32_t glue_info;
    uint32_t glue_keyforms;
    // v4.2+
    uint32_t keyform_multiply_colors;
    uint32_t keyform_screen_colors;
    uint32_t blend_shape_param_bindings;
    uint32_t blend_shape_keyform_bindings;
    uint32_t blend_shapes_warp;
    uint32_t blend_shapes_artmesh;
    uint32_t blend_shape_constraint_indices;
    uint32_t blend_shape_constraints;
    uint32_t blend_shape_constraint_values;
    // v5.0
    uint32_t blend_shapes_parts;
    uint32_t blend_shapes_rotation;
    uint32_t blend_shapes_glue;
} MocCounts;

// ── ArtMesh (drawable) ──
typedef struct {
    char    id[64];
    int32_t vertex_count;
    int32_t index_count;
    uint16_t* indices;          // into position/UV arrays (PSRAM)
    Lv2Vec2* positions;          // raw positions (PSRAM)
    Lv2UV*   uvs;                // raw UVs (PSRAM)
    int32_t  texture_index;
    float    opacity;
    int32_t  draw_order;
    int32_t* mask_indices;
    int32_t  mask_count;
    int32_t  parent_part_index;
} Lv2Drawable;

// ── Part ──
typedef struct {
    char    id[64];
    int32_t parent_part_index;
    bool    is_visible;
    bool    is_enabled;
} Lv2Part;

// ── Parameter ──
typedef struct {
    char  id[64];
    float default_value;
    float min_value;
    float max_value;
    float current_value;
} Lv2Param;

// ── Warp Deformer ──
typedef struct {
    char    id[64];
    int32_t keyform_binding_source_index;
    bool    is_visible;
} Lv2WarpDeformer;

// ── Parsed model ──
typedef struct {
    MocVersion  version;
    bool        is_big_endian;
    float       pixels_per_unit;
    float       origin_x, origin_y;
    float       canvas_w, canvas_h;
    MocCounts   counts;

    // Data arrays (PSRAM)
    Lv2Part*         parts;
    Lv2Param*        params;
    Lv2Drawable*     drawables;
    Lv2WarpDeformer* warp_deformers;

    // Raw data for deformer computation
    float*    keyform_positions;     // float[size]
    int32_t*  param_binding_indices; // int32_t[size]
    int32_t*  keyform_bindings;      // int32_t[size]
    int32_t*  param_bindings;        // int32_t[size]
    float*    keys;                  // float[size]

    // Internal: PSRAM backing store (to free later)
    void* _psram_pool;
    size_t _psram_size;
} Lv2Model;

/** Parse .moc3 binary data into Lv2Model. Returns NULL on failure.
 *  Allocates model data in PSRAM via heap_caps_malloc(MALLOC_CAP_SPIRAM).
 *  Caller must call lv2_free_model() to release.
 */
Lv2Model* lv2_parse_moc3(const uint8_t* data, size_t size);

/** Free model and all PSRAM allocations. */
void lv2_free_model(Lv2Model* m);

/** Get parameter index by ID string. Returns -1 if not found. */
int lv2_find_param(Lv2Model* m, const char* id);

/** Get drawable index by ID string. Returns -1 if not found. */
int lv2_find_drawable(Lv2Model* m, const char* id);

#ifdef __cplusplus
}
#endif

/**
 * @file lv_freetype.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_freetype.h"
#if LV_USE_FREETYPE

#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_CACHE_H
#include FT_SIZES_H
#include FT_IMAGE_H
#include FT_OUTLINE_H
#include <errno.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    lv_ll_t  face_ll;
} lv_faces_control_t;

typedef struct name_refer_t {
    const char * name;  /* point to font name string */
    int32_t cnt;        /* reference count */
} name_refer_t;

typedef struct {
    const void * mem;
    const char * name;
    size_t mem_size;
#if LV_FREETYPE_CACHE_SIZE < 0
    FT_Size     size;
#endif
    lv_font_t * font;
    uint16_t    style;
    uint16_t    height;
    /* 首次读取/渲染失败后封锁此后端，避免 GUI 反复读同一损坏字形。 */
    bool failed;
    bool retry_needed;
    uint8_t retry_count;
    uint32_t error_tick;
} lv_font_fmt_ft_dsc_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
#if LV_FREETYPE_CACHE_SIZE >= 0
static FT_Error font_face_requester(FTC_FaceID face_id,
                                    FT_Library library_is, FT_Pointer req_data, FT_Face * aface);
static bool lv_ft_font_init_cache(lv_ft_info_t * info);
static void lv_ft_font_destroy_cache(lv_font_t * font);
#else
static FT_Face face_find_in_list(lv_ft_info_t * info);
static void face_add_to_list(FT_Face face);
static void face_remove_from_list(FT_Face face);
static void face_generic_finalizer(void * object);
static bool lv_ft_font_init_nocache(lv_ft_info_t * info);
static void lv_ft_font_destroy_nocache(lv_font_t * font);
#endif

static const char * name_refer_save(const char * name);
static void name_refer_del(const char * name);
static const char * name_refer_find(const char * name);

/**********************
*  STATIC VARIABLES
**********************/
static FT_Library library;
static lv_ll_t names_ll;

#if LV_FREETYPE_CACHE_SIZE >= 0
    static FTC_Manager cache_manager;
    static FTC_CMapCache cmap_cache;
    static FT_Face current_face = NULL;

    #if LV_FREETYPE_SBIT_CACHE
        static FTC_SBitCache sbit_cache;
        static FTC_SBit sbit;
    #else
        static FTC_ImageCache image_cache;
        static FT_Glyph       image_glyph;
    #endif

#else
    static lv_faces_control_t face_control;
#endif

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

bool lv_freetype_init(uint16_t max_faces, uint16_t max_sizes, uint32_t max_bytes)
{
    FT_Error error = FT_Init_FreeType(&library);
    if(error) {
        LV_LOG_ERROR("init freeType error(%d)", error);
        return false;
    }

    _lv_ll_init(&names_ll, sizeof(name_refer_t));

#if LV_FREETYPE_CACHE_SIZE >= 0
    error = FTC_Manager_New(library, max_faces, max_sizes,
                            max_bytes, font_face_requester, NULL, &cache_manager);
    if(error) {
        FT_Done_FreeType(library);
        LV_LOG_ERROR("Failed to open cache manager");
        return false;
    }

    error = FTC_CMapCache_New(cache_manager, &cmap_cache);
    if(error) {
        LV_LOG_ERROR("Failed to open Cmap Cache");
        goto Fail;
    }

#if LV_FREETYPE_SBIT_CACHE
    error = FTC_SBitCache_New(cache_manager, &sbit_cache);
    if(error) {
        LV_LOG_ERROR("Failed to open sbit cache");
        goto Fail;
    }
#else
    error = FTC_ImageCache_New(cache_manager, &image_cache);
    if(error) {
        LV_LOG_ERROR("Failed to open image cache");
        goto Fail;
    }
#endif

    return true;
Fail:
    FTC_Manager_Done(cache_manager);
    FT_Done_FreeType(library);
    return false;
#else
    LV_UNUSED(max_faces);
    LV_UNUSED(max_sizes);
    LV_UNUSED(max_bytes);
    _lv_ll_init(&face_control.face_ll, sizeof(FT_Face *));
    return true;
#endif/* LV_FREETYPE_CACHE_SIZE */
}

void lv_freetype_destroy(void)
{
#if LV_FREETYPE_CACHE_SIZE >= 0
    FTC_Manager_Done(cache_manager);
#endif
    FT_Done_FreeType(library);
}

bool lv_ft_font_init(lv_ft_info_t * info)
{
#if LV_FREETYPE_CACHE_SIZE >= 0
    return lv_ft_font_init_cache(info);
#else
    return lv_ft_font_init_nocache(info);
#endif
}

void lv_ft_font_destroy(lv_font_t * font)
{
#if LV_FREETYPE_CACHE_SIZE >= 0
    lv_ft_font_destroy_cache(font);
#else
    lv_ft_font_destroy_nocache(font);
#endif
}

bool lv_ft_font_failed(const lv_font_t * font)
{
    return !font || !font->dsc || ((lv_font_fmt_ft_dsc_t *)font->dsc)->failed;
}

bool lv_ft_font_unavailable(const lv_font_t * font)
{
    return lv_ft_font_failed(font) || ((lv_font_fmt_ft_dsc_t *)font->dsc)->retry_needed;
}

bool lv_ft_font_retry_pending(const lv_font_t * font)
{
    if(!font || !font->dsc) return false;
    const lv_font_fmt_ft_dsc_t * dsc = font->dsc;
    return !dsc->failed && dsc->retry_needed && dsc->retry_count < 3;
}

bool lv_ft_font_retry(lv_font_t * font)
{
    if(!lv_ft_font_retry_pending(font)) return false;
    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)font->dsc;
    if(lv_tick_elaps(dsc->error_tick) < 500U * (dsc->retry_count + 1U)) return false;
    ++dsc->retry_count;
    dsc->retry_needed = false;
    return true;
}

static void font_glyph_error(lv_font_fmt_ft_dsc_t * dsc, FT_Error error)
{
    const FT_Error base = FT_ERROR_BASE(error);
    const bool resource_pressure = base == FT_Err_Out_Of_Memory ||
        ((base == FT_Err_Cannot_Open_Resource || base == FT_Err_Cannot_Open_Stream) &&
         (errno == EMFILE || errno == ENFILE || errno == ENOMEM));
    dsc->retry_needed = resource_pressure;
    dsc->failed = !resource_pressure;
    dsc->error_tick = lv_tick_get();
}

static void font_glyph_succeeded(lv_font_fmt_ft_dsc_t * dsc)
{
    /* A different good glyph in the same repaint must not reset a failing
     * glyph's retry budget. Rearm only after a minute of successful use. */
    if(dsc->retry_count && lv_tick_elaps(dsc->error_tick) >= 60000U)
        dsc->retry_count = 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
#if LV_FREETYPE_CACHE_SIZE >= 0

static FT_Error font_face_requester(FTC_FaceID face_id,
                                    FT_Library library_is, FT_Pointer req_data, FT_Face * aface)
{
    LV_UNUSED(library_is);
    LV_UNUSED(req_data);

    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)face_id;
    FT_Error error;
    if(dsc->mem) {
        error = FT_New_Memory_Face(library, dsc->mem, dsc->mem_size, 0, aface);
    }
    else {
        error = FT_New_Face(library, dsc->name, 0, aface);
    }
    if(error) {
        LV_LOG_ERROR("FT_New_Face error:%d\n", error);
        return error;
    }
    return FT_Err_Ok;
}

static FT_Error get_bold_glyph(const lv_font_t * font, FT_Face face,
                           FT_UInt glyph_index, lv_font_glyph_dsc_t * dsc_out)
{
    errno = 0;
    FT_Error error = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
    if(error) return error;

    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if(dsc->style & FT_FONT_STYLE_BOLD) {
            int strength = 1 << 6;
            FT_Outline_Embolden(&face->glyph->outline, strength);
        }
    }

    errno = 0;
    error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    if(error) return error;

    dsc_out->adv_w = (face->glyph->metrics.horiAdvance >> 6);
    dsc_out->box_h = face->glyph->bitmap.rows;         /*Height of the bitmap in [px]*/
    dsc_out->box_w = face->glyph->bitmap.width;         /*Width of the bitmap in [px]*/
    dsc_out->ofs_x = face->glyph->bitmap_left;         /*X offset of the bitmap in [pf]*/
    dsc_out->ofs_y = face->glyph->bitmap_top -
                     face->glyph->bitmap.rows;         /*Y offset of the bitmap measured from the as line*/
    dsc_out->bpp = 8;         /*Bit per pixel: 1/2/4/8*/

    return FT_Err_Ok;
}

static bool get_glyph_dsc_cb_cache(const lv_font_t * font,
                                   lv_font_glyph_dsc_t * dsc_out, uint32_t unicode_letter, uint32_t unicode_letter_next)
{
    LV_UNUSED(unicode_letter_next);
    if(unicode_letter < 0x20) {
        dsc_out->adv_w = 0;
        dsc_out->box_h = 0;
        dsc_out->box_w = 0;
        dsc_out->ofs_x = 0;
        dsc_out->ofs_y = 0;
        dsc_out->bpp = 0;
        return true;
    }

    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(dsc->failed || dsc->retry_needed)
        return false;

    FTC_FaceID face_id = (FTC_FaceID)dsc;
    FT_Size face_size;
    struct FTC_ScalerRec_ scaler;
    scaler.face_id = face_id;
    scaler.width = dsc->height;
    scaler.height = dsc->height;
    scaler.pixel = 1;
    errno = 0;
    FT_Error lookup_error = FTC_Manager_LookupSize(cache_manager, &scaler, &face_size);
    if(lookup_error) {
        font_glyph_error(dsc, lookup_error);
        return false;
    }

    FT_Face face = face_size->face;
    FT_UInt charmap_index = FT_Get_Charmap_Index(face->charmap);
    FT_UInt glyph_index = FTC_CMapCache_Lookup(cmap_cache, face_id, charmap_index, unicode_letter);
    dsc_out->is_placeholder = glyph_index == 0;

    if(dsc->style & FT_FONT_STYLE_ITALIC) {
        FT_Matrix italic_matrix;
        italic_matrix.xx = 1 << 16;
        italic_matrix.xy = 0x5800;
        italic_matrix.yx = 0;
        italic_matrix.yy = 1 << 16;
        FT_Set_Transform(face, &italic_matrix, NULL);
    }

    if(dsc->style & FT_FONT_STYLE_BOLD) {
        current_face = face;
        FT_Error error = get_bold_glyph(font, face, glyph_index, dsc_out);
        if(error) {
            current_face = NULL;
            font_glyph_error(dsc, error);
            return false;
        }
        goto end;
    }

    FTC_ImageTypeRec desc_type;
    desc_type.face_id = face_id;
    desc_type.flags = FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL;
    desc_type.height = dsc->height;
    desc_type.width = dsc->height;

#if LV_FREETYPE_SBIT_CACHE
    errno = 0;
    FT_Error error = FTC_SBitCache_Lookup(sbit_cache, &desc_type, glyph_index, &sbit, NULL);
    /* SBIT 会把非内存不足的加载错误转换成成功，须同时检查不可用标记。
     * 合法空格是 width=0，不能仅凭 buffer=NULL 判定坏字形。 */
    if(error || (sbit->width == 255 && sbit->height == 0 && !sbit->buffer)) {
        font_glyph_error(dsc, error ? error : FT_Err_Invalid_Outline);
        LV_LOG_ERROR("SBitCache_Lookup error");
        return false;
    }

    dsc_out->adv_w = sbit->xadvance;
    dsc_out->box_h = sbit->height;  /*Height of the bitmap in [px]*/
    dsc_out->box_w = sbit->width;   /*Width of the bitmap in [px]*/
    dsc_out->ofs_x = sbit->left;    /*X offset of the bitmap in [pf]*/
    dsc_out->ofs_y = sbit->top - sbit->height; /*Y offset of the bitmap measured from the as line*/
    dsc_out->bpp = 8;               /*Bit per pixel: 1/2/4/8*/
#else
    errno = 0;
    FT_Error error = FTC_ImageCache_Lookup(image_cache, &desc_type, glyph_index, &image_glyph, NULL);
    if(error) {
        font_glyph_error(dsc, error);
        LV_LOG_ERROR("ImageCache_Lookup error");
        return false;
    }
    if(image_glyph->format != FT_GLYPH_FORMAT_BITMAP) {
        LV_LOG_ERROR("Glyph_To_Bitmap error");
        dsc->failed = true;
        return false;
    }

    FT_BitmapGlyph glyph_bitmap = (FT_BitmapGlyph)image_glyph;
    dsc_out->adv_w = (glyph_bitmap->root.advance.x >> 16);
    dsc_out->box_h = glyph_bitmap->bitmap.rows;         /*Height of the bitmap in [px]*/
    dsc_out->box_w = glyph_bitmap->bitmap.width;        /*Width of the bitmap in [px]*/
    dsc_out->ofs_x = glyph_bitmap->left;                /*X offset of the bitmap in [pf]*/
    dsc_out->ofs_y = glyph_bitmap->top -
                     glyph_bitmap->bitmap.rows;         /*Y offset of the bitmap measured from the as line*/
    dsc_out->bpp = 8;         /*Bit per pixel: 1/2/4/8*/
#endif

end:
    if((dsc->style & FT_FONT_STYLE_ITALIC) && (unicode_letter_next == '\0')) {
        dsc_out->adv_w = dsc_out->box_w + dsc_out->ofs_x;
    }

    font_glyph_succeeded(dsc);
    return true;
}

static const uint8_t * get_glyph_bitmap_cb_cache(const lv_font_t * font, uint32_t unicode_letter)
{
    LV_UNUSED(unicode_letter);

    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(dsc->failed || dsc->retry_needed)
        return NULL;
    if(dsc->style & FT_FONT_STYLE_BOLD) {
        if(current_face && current_face->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            return (const uint8_t *)(current_face->glyph->bitmap.buffer);
        }
        return NULL;
    }

#if LV_FREETYPE_SBIT_CACHE
    return (const uint8_t *)sbit->buffer;
#else
    FT_BitmapGlyph glyph_bitmap = (FT_BitmapGlyph)image_glyph;
    return (const uint8_t *)glyph_bitmap->bitmap.buffer;
#endif
}

static bool lv_ft_font_init_cache(lv_ft_info_t * info)
{
    size_t need_size = sizeof(lv_font_fmt_ft_dsc_t) + sizeof(lv_font_t);
    lv_font_fmt_ft_dsc_t * dsc = lv_mem_alloc(need_size);
    if(dsc == NULL) return false;
    lv_memset_00(dsc, need_size);

    dsc->font = (lv_font_t *)(((char *)dsc) + sizeof(lv_font_fmt_ft_dsc_t));
    dsc->mem = info->mem;
    dsc->mem_size = info->mem_size;
    dsc->name = name_refer_save(info->name);
    dsc->height = info->weight;
    dsc->style = info->style;

    /* use to get font info */
    FT_Size face_size;
    struct FTC_ScalerRec_ scaler;
    scaler.face_id = (FTC_FaceID)dsc;
    scaler.width = info->weight;
    scaler.height = info->weight;
    scaler.pixel = 1;
    FT_Error error = FTC_Manager_LookupSize(cache_manager, &scaler, &face_size);
    if(error) {
        LV_LOG_ERROR("Failed to LookupSize");
        goto Fail;
    }

    lv_font_t * font = dsc->font;
    font->dsc = dsc;
    font->get_glyph_dsc = get_glyph_dsc_cb_cache;
    font->get_glyph_bitmap = get_glyph_bitmap_cb_cache;
    font->subpx = LV_FONT_SUBPX_NONE;
    font->line_height = (face_size->face->size->metrics.height >> 6);
    font->base_line = -(face_size->face->size->metrics.descender >> 6);

    FT_Fixed scale = face_size->face->size->metrics.y_scale;
    int8_t thickness = FT_MulFix(scale, face_size->face->underline_thickness) >> 6;
    font->underline_position = FT_MulFix(scale, face_size->face->underline_position) >> 6;
    font->underline_thickness = thickness < 1 ? 1 : thickness;

    /* return to user */
    info->font = font;

    return true;

Fail:
    /* LookupSize may have cached a face before failing to create its size.
     * Remove it while the FaceID and filename are still alive. */
    FTC_Manager_RemoveFaceID(cache_manager, (FTC_FaceID)dsc);
    if(dsc->name) name_refer_del(dsc->name);
    lv_mem_free(dsc);
    return false;
}

void lv_ft_font_destroy_cache(lv_font_t * font)
{
    if(font == NULL) {
        return;
    }

    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(dsc) {
        FTC_Manager_RemoveFaceID(cache_manager, (FTC_FaceID)dsc);
        name_refer_del(dsc->name);
        lv_mem_free(dsc);
    }
}
#else/* LV_FREETYPE_CACHE_SIZE */

static FT_Face face_find_in_list(lv_ft_info_t * info)
{
    lv_font_fmt_ft_dsc_t * dsc;
    FT_Face * pface = _lv_ll_get_head(&face_control.face_ll);
    while(pface) {
        dsc = (lv_font_fmt_ft_dsc_t *)(*pface)->generic.data;
        if(strcmp(dsc->name, info->name) == 0) {
            return *pface;
        }
        pface = _lv_ll_get_next(&face_control.face_ll, pface);
    }

    return NULL;
}

static void face_add_to_list(FT_Face face)
{
    FT_Face * pface;
    pface = (FT_Face *)_lv_ll_ins_tail(&face_control.face_ll);
    *pface = face;
}

static void face_remove_from_list(FT_Face face)
{
    FT_Face * pface = _lv_ll_get_head(&face_control.face_ll);
    while(pface) {
        if(*pface == face) {
            _lv_ll_remove(&face_control.face_ll, pface);
            lv_mem_free(pface);
            break;
        }
        pface = _lv_ll_get_next(&face_control.face_ll, pface);
    }
}

static void face_generic_finalizer(void * object)
{
    FT_Face face = (FT_Face)object;
    face_remove_from_list(face);
    LV_LOG_INFO("face finalizer(%p)\n", face);
}

static bool get_glyph_dsc_cb_nocache(const lv_font_t * font,
                                     lv_font_glyph_dsc_t * dsc_out, uint32_t unicode_letter, uint32_t unicode_letter_next)
{
    LV_UNUSED(unicode_letter_next);
    if(unicode_letter < 0x20) {
        dsc_out->adv_w = 0;
        dsc_out->box_h = 0;
        dsc_out->box_w = 0;
        dsc_out->ofs_x = 0;
        dsc_out->ofs_y = 0;
        dsc_out->bpp = 0;
        return true;
    }

    FT_Error error;
    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(dsc->failed || dsc->retry_needed)
        return false;
    FT_Face face = dsc->size->face;

    FT_UInt glyph_index = FT_Get_Char_Index(face, unicode_letter);

    if(face->size != dsc->size) {
        FT_Activate_Size(dsc->size);
    }
    dsc_out->is_placeholder = glyph_index == 0;

    errno = 0;
    error = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
    if(error) {
        font_glyph_error(dsc, error);
        return false;
    }

    if(face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if(dsc->style & FT_FONT_STYLE_BOLD) {
            int strength = 1 << 6;
            FT_Outline_Embolden(&face->glyph->outline, strength);
        }

        if(dsc->style & FT_FONT_STYLE_ITALIC) {
            FT_Matrix italic_matrix;
            italic_matrix.xx = 1 << 16;
            italic_matrix.xy = 0x5800;
            italic_matrix.yx = 0;
            italic_matrix.yy = 1 << 16;
            FT_Outline_Transform(&face->glyph->outline, &italic_matrix);
        }
    }

    errno = 0;
    error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    if(error) {
        font_glyph_error(dsc, error);
        return false;
    }

    dsc_out->adv_w = (face->glyph->metrics.horiAdvance >> 6);
    dsc_out->box_h = face->glyph->bitmap.rows;         /*Height of the bitmap in [px]*/
    dsc_out->box_w = face->glyph->bitmap.width;         /*Width of the bitmap in [px]*/
    dsc_out->ofs_x = face->glyph->bitmap_left;         /*X offset of the bitmap in [pf]*/
    dsc_out->ofs_y = face->glyph->bitmap_top -
                     face->glyph->bitmap.rows;         /*Y offset of the bitmap measured from the as line*/
    dsc_out->bpp = 8;         /*Bit per pixel: 1/2/4/8*/

    if((dsc->style & FT_FONT_STYLE_ITALIC) && (unicode_letter_next == '\0')) {
        dsc_out->adv_w = dsc_out->box_w + dsc_out->ofs_x;
    }

    font_glyph_succeeded(dsc);
    return true;
}

static const uint8_t * get_glyph_bitmap_cb_nocache(const lv_font_t * font, uint32_t unicode_letter)
{
    LV_UNUSED(unicode_letter);
    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(dsc->failed || dsc->retry_needed)
        return NULL;
    FT_Face face = dsc->size->face;
    return (const uint8_t *)(face->glyph->bitmap.buffer);
}

static bool lv_ft_font_init_nocache(lv_ft_info_t * info)
{
    size_t need_size = sizeof(lv_font_fmt_ft_dsc_t) + sizeof(lv_font_t);
    lv_font_fmt_ft_dsc_t * dsc = lv_mem_alloc(need_size);
    if(dsc == NULL) return false;
    lv_memset_00(dsc, need_size);

    dsc->font = (lv_font_t *)(((char *)dsc) + sizeof(lv_font_fmt_ft_dsc_t));
    dsc->mem = info->mem;
    dsc->mem_size = info->mem_size;
    dsc->name = name_refer_save(info->name);
    dsc->height = info->weight;
    dsc->style = info->style;

    FT_Face face = face_find_in_list(info);
    if(face == NULL) {
        FT_Error error;
        if(dsc->mem) {
            error = FT_New_Memory_Face(library, dsc->mem, (FT_Long) dsc->mem_size, 0, &face);
        }
        else {
            error = FT_New_Face(library, dsc->name, 0, &face);
        }
        if(error) {
            LV_LOG_WARN("create face error(%d)", error);
            goto Fail;
        }

        /* link face and face info */
        face->generic.data = dsc;
        face->generic.finalizer = face_generic_finalizer;
        face_add_to_list(face);
    }
    else {
        FT_Size size;
        FT_Error error = FT_New_Size(face, &size);
        if(error) {
            goto Fail;
        }
        FT_Activate_Size(size);
        FT_Reference_Face(face);
    }

    FT_Set_Pixel_Sizes(face, 0, info->weight);
    dsc->size = face->size;

    lv_font_t * font = dsc->font;
    font->dsc = dsc;
    font->get_glyph_dsc = get_glyph_dsc_cb_nocache;
    font->get_glyph_bitmap = get_glyph_bitmap_cb_nocache;
    font->line_height = (face->size->metrics.height >> 6);
    font->base_line = -(face->size->metrics.descender >> 6);
    font->subpx = LV_FONT_SUBPX_NONE;

    FT_Fixed scale = face->size->metrics.y_scale;
    int8_t thickness = FT_MulFix(scale, face->underline_thickness) >> 6;
    font->underline_position = FT_MulFix(scale, face->underline_position) >> 6;
    font->underline_thickness = thickness < 1 ? 1 : thickness;

    info->font = font;
    return true;

Fail:
    lv_mem_free(dsc);
    return false;
}

static void lv_ft_font_destroy_nocache(lv_font_t * font)
{
    if(font == NULL) {
        return;
    }

    lv_font_fmt_ft_dsc_t * dsc = (lv_font_fmt_ft_dsc_t *)(font->dsc);
    if(dsc) {
        FT_Face face = dsc->size->face;
        FT_Done_Size(dsc->size);
        FT_Done_Face(face);
        name_refer_del(dsc->name);
        lv_mem_free(dsc);
    }
}

#endif/* LV_FREETYPE_CACHE_SIZE */

/**
 * find name string in names list.name string cnt += 1 if find.
 * @param name name string
 * @return the string pointer of name.
 */
static const char * name_refer_find(const char * name)
{
    name_refer_t * refer = _lv_ll_get_head(&names_ll);
    while(refer) {
        if(strcmp(refer->name, name) == 0) {
            refer->cnt += 1;
            return refer->name;
        }
        refer = _lv_ll_get_next(&names_ll, refer);
    }
    return NULL;
}

/**
 * del name string from list.
 */
static void name_refer_del(const char * name)
{
    name_refer_t * refer = _lv_ll_get_head(&names_ll);
    while(refer) {
        if(strcmp(refer->name, name) == 0) {
            refer->cnt -= 1;
            if(refer->cnt <= 0) {
                _lv_ll_remove(&names_ll, refer);
                lv_mem_free((void *)refer->name);
                lv_mem_free(refer);
            }
            return;
        }
        refer = _lv_ll_get_next(&names_ll, refer);
    }

    LV_LOG_WARN("name_in_names_del error(not find:%p).", name);
}

/**
 * save name string to list.
 * @param name name string
 * @return Saved string pointer
 */
static const char * name_refer_save(const char * name)
{
    const char * pos = name_refer_find(name);
    if(pos) {
        return pos;
    }

    name_refer_t * refer = _lv_ll_ins_tail(&names_ll);
    if(refer) {
        uint32_t len = strlen(name) + 1;
        refer->name = lv_mem_alloc(len);
        if(refer->name) {
            lv_memcpy((void *)refer->name, name, len);
            refer->cnt = 1;
            return refer->name;
        }
        _lv_ll_remove(&names_ll, refer);
        lv_mem_free(refer);
    }
    LV_LOG_WARN("save_name_to_names error(not memory).");
    return "";
}

#endif /*LV_USE_FREETYPE*/

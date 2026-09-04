/*
 * Fast video decoder Python C extension with bitcost export.
 *
 * Stripped-down version of cv_reader that does only the essentials:
 *  - open / find stream / init decoder
 *  - decode loop with skip_loop_filter + skip_idct
 *  - optional H.264/HEVC bitcost export from frame->opaque_ref
 *  - returns a lightweight list of dicts with frame metadata
 *
 * Link against the patched FFmpeg static libs with -Wl,-Bsymbolic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <Python.h>
#include <numpy/arrayobject.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* ------------------------------------------------------------------ */
/* H264MbBitCostMap – must match ffmpeg_patch/h264_cabac.c exactly   */
/* ------------------------------------------------------------------ */
#define H264_MB_BIT_COST_MAP_MAGIC   0x4D424354U   /* MKBETAG('M','B','C','T') */
#define H264_MB_BIT_COST_MAP_VERSION 2
#define HEVC_CTU_BIT_COST_MAP_MAGIC  0x48435455U   /* MKBETAG('H','C','T','U') */
#define HEVC_CTU_BIT_COST_MAP_VERSION 2
#define VP9_SB_BIT_COST_MAP_MAGIC    0x56503953U   /* MKBETAG('V','P','9','S') */
#define VP9_SB_BIT_COST_MAP_VERSION  1
#define CVR_TARGET_BITCOST_MAGIC     0x43565254U   /* MKBETAG('C','V','R','T') */

#pragma pack(push, 1)
typedef struct H264MbBitCostMap {
    uint32_t magic;
    int32_t  version;
    int32_t  mb_width;
    int32_t  mb_height;
    int32_t  mb_stride;
    int32_t  sub_width;
    int32_t  sub_height;
    int32_t  sub_stride;
} H264MbBitCostMap;

typedef struct HevcCtuBitCostMap {
    uint32_t magic;
    int32_t  version;
    int32_t  ctb_width;
    int32_t  ctb_height;
    int32_t  ctb_stride;
    int32_t  log2_ctb_size;
    int32_t  sub_width;
    int32_t  sub_height;
    int32_t  sub_stride;
} HevcCtuBitCostMap;

typedef struct Vp9SbBitCostMap {
    uint32_t magic;
    int32_t  version;
    int32_t  sb_width;
    int32_t  sb_height;
    int32_t  sb_stride;
    int32_t  sub_width;
    int32_t  sub_height;
    int32_t  sub_stride;
} Vp9SbBitCostMap;
#pragma pack(pop)

typedef struct CvrTargetBitcostCtx {
    uint32_t magic;
    int enabled;
    int max_frame;
    const uint8_t *frame_bitmap;
    int tolerance;
} CvrTargetBitcostCtx;

static const int32_t *h264_mb_costs_ptr(const H264MbBitCostMap *map) {
    return (const int32_t *)((const uint8_t *)map + sizeof(H264MbBitCostMap));
}

static const float *h264_sub_costs_ptr(const H264MbBitCostMap *map) {
    return (const float *)(h264_mb_costs_ptr(map) + (size_t)map->mb_stride * map->mb_height);
}

static const int32_t *hevc_ctu_costs_ptr(const HevcCtuBitCostMap *map) {
    return (const int32_t *)((const uint8_t *)map + sizeof(HevcCtuBitCostMap));
}

static const float *hevc_sub_costs_ptr(const HevcCtuBitCostMap *map) {
    return (const float *)(hevc_ctu_costs_ptr(map) + (size_t)map->ctb_stride * map->ctb_height);
}

static const int32_t *vp9_sb_costs_ptr(const Vp9SbBitCostMap *map) {
    return (const int32_t *)((const uint8_t *)map + sizeof(Vp9SbBitCostMap));
}

static const float *vp9_sub_costs_ptr(const Vp9SbBitCostMap *map) {
    return (const float *)(vp9_sb_costs_ptr(map) + (size_t)map->sb_stride * map->sb_height);
}

static int cvr_bitcost_diag_enabled_cpp()
{
    const char *v = getenv("CVR_BITCOST_DIAG");
    return v && v[0] && v[0] != '0';
}

static int cvr_bitcost_map_is_valid(AVFrame *frame)
{
    if (!frame->opaque_ref || !frame->opaque_ref->data)
        return 0;
    if ((size_t)frame->opaque_ref->size < sizeof(H264MbBitCostMap))
        return 0;
    const H264MbBitCostMap *map = (const H264MbBitCostMap *)frame->opaque_ref->data;
    if (map->magic != H264_MB_BIT_COST_MAP_MAGIC ||
        map->version != H264_MB_BIT_COST_MAP_VERSION) {
        const HevcCtuBitCostMap *hevc_map = (const HevcCtuBitCostMap *)frame->opaque_ref->data;
        if (hevc_map->magic != HEVC_CTU_BIT_COST_MAP_MAGIC ||
            hevc_map->version != HEVC_CTU_BIT_COST_MAP_VERSION) {
            const Vp9SbBitCostMap *vp9_map = (const Vp9SbBitCostMap *)frame->opaque_ref->data;
            if (vp9_map->magic != VP9_SB_BIT_COST_MAP_MAGIC ||
                vp9_map->version != VP9_SB_BIT_COST_MAP_VERSION)
                return 0;
            size_t sb_count = (size_t)vp9_map->sb_stride * vp9_map->sb_height;
            size_t sub_count = (size_t)vp9_map->sub_stride * vp9_map->sub_height;
            size_t expect_sz = sizeof(Vp9SbBitCostMap) +
                               sb_count * sizeof(int32_t) +
                               sub_count * sizeof(float);
            return (size_t)frame->opaque_ref->size >= expect_sz;
        }
        size_t ctu_count = (size_t)hevc_map->ctb_stride * hevc_map->ctb_height;
        size_t sub_count = (size_t)hevc_map->sub_stride * hevc_map->sub_height;
        size_t expect_sz = sizeof(HevcCtuBitCostMap) +
                           ctu_count * sizeof(int32_t) +
                           sub_count * sizeof(float);
        return (size_t)frame->opaque_ref->size >= expect_sz;
    } else {
        size_t mb_count = (size_t)map->mb_stride * map->mb_height;
        size_t sub_count = (size_t)map->sub_stride * map->sub_height;
        size_t expect_sz = sizeof(H264MbBitCostMap) +
                           mb_count * sizeof(int32_t) +
                           sub_count * sizeof(float);
        return (size_t)frame->opaque_ref->size >= expect_sz;
    }
}

static int cvr_bitcost_map_has_nonzero_mb(AVFrame *frame)
{
    if (!cvr_bitcost_map_is_valid(frame))
        return 0;
    const H264MbBitCostMap *h264_map = (const H264MbBitCostMap *)frame->opaque_ref->data;
    if (h264_map->magic == H264_MB_BIT_COST_MAP_MAGIC) {
        const int32_t *mb = h264_mb_costs_ptr(h264_map);
        size_t mb_count = (size_t)h264_map->mb_stride * h264_map->mb_height;
        for (size_t i = 0; i < mb_count; ++i) {
            if (mb[i] != 0)
                return 1;
        }
    } else {
        const HevcCtuBitCostMap *hevc_map = (const HevcCtuBitCostMap *)frame->opaque_ref->data;
        if (hevc_map->magic == HEVC_CTU_BIT_COST_MAP_MAGIC) {
            const int32_t *ctu = hevc_ctu_costs_ptr(hevc_map);
            size_t ctu_count = (size_t)hevc_map->ctb_stride * hevc_map->ctb_height;
            for (size_t i = 0; i < ctu_count; ++i) {
                if (ctu[i] != 0)
                    return 1;
            }
        } else {
            const Vp9SbBitCostMap *vp9_map = (const Vp9SbBitCostMap *)frame->opaque_ref->data;
            const int32_t *sb = vp9_sb_costs_ptr(vp9_map);
            size_t sb_count = (size_t)vp9_map->sb_stride * vp9_map->sb_height;
            for (size_t i = 0; i < sb_count; ++i) {
                if (sb[i] != 0)
                    return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: build a numpy array from existing data (copies)            */
/* ------------------------------------------------------------------ */
static PyObject *
make_numpy_copy(int ndim, const npy_intp *dims, int typenum,
                const void *src, size_t src_size)
{
    PyObject *arr = PyArray_SimpleNew(ndim, dims, typenum);
    if (!arr)
        return nullptr;
    void *dst = PyArray_DATA((PyArrayObject *)arr);
    memcpy(dst, src, src_size);
    return arr;
}

/* ------------------------------------------------------------------ */
/* Extract bitcost dict from frame->opaque_ref                         */
/* ------------------------------------------------------------------ */
static PyObject *
extract_h264_bitcost(AVFrame *frame)
{
    if (!frame->opaque_ref || !frame->opaque_ref->data)
        Py_RETURN_NONE;

    if ((size_t)frame->opaque_ref->size < sizeof(H264MbBitCostMap))
        Py_RETURN_NONE;

    const H264MbBitCostMap *map = (const H264MbBitCostMap *)frame->opaque_ref->data;

    if (map->magic != H264_MB_BIT_COST_MAP_MAGIC ||
        map->version != H264_MB_BIT_COST_MAP_VERSION)
        Py_RETURN_NONE;

    size_t mb_count   = (size_t)map->mb_stride   * map->mb_height;
    size_t sub_count  = (size_t)map->sub_stride  * map->sub_height;
    size_t expect_sz  = sizeof(H264MbBitCostMap)
                        + mb_count  * sizeof(int32_t)
                        + sub_count * sizeof(float);

    if ((size_t)frame->opaque_ref->size < expect_sz)
        Py_RETURN_NONE;

    const int32_t *mb_costs  = h264_mb_costs_ptr(map);
    const float   *sub_costs = h264_sub_costs_ptr(map);

    npy_intp mb_dims[2]  = {map->mb_height,  map->mb_stride};
    npy_intp sub_dims[2] = {map->sub_height, map->sub_stride};

    PyObject *mb_arr  = make_numpy_copy(2, mb_dims,  NPY_INT32,
                                        mb_costs,  mb_count * sizeof(int32_t));
    PyObject *sub_arr = make_numpy_copy(2, sub_dims, NPY_FLOAT32,
                                        sub_costs, sub_count * sizeof(float));
    if (!mb_arr || !sub_arr) {
        Py_XDECREF(mb_arr);
        Py_XDECREF(sub_arr);
        return nullptr;
    }

    PyObject *dict = PyDict_New();
    if (!dict) {
        Py_DECREF(mb_arr);
        Py_DECREF(sub_arr);
        return nullptr;
    }

    PyDict_SetItemString(dict, "mb_bit_cost",  mb_arr);
    PyDict_SetItemString(dict, "sub_mb_bit_cost", sub_arr);
    PyDict_SetItemString(dict, "mb_width",     PyLong_FromLong(map->mb_width));
    PyDict_SetItemString(dict, "mb_height",    PyLong_FromLong(map->mb_height));
    PyDict_SetItemString(dict, "mb_stride",    PyLong_FromLong(map->mb_stride));
    PyDict_SetItemString(dict, "sub_width",    PyLong_FromLong(map->sub_width));
    PyDict_SetItemString(dict, "sub_height",   PyLong_FromLong(map->sub_height));
    PyDict_SetItemString(dict, "sub_stride",   PyLong_FromLong(map->sub_stride));

    Py_DECREF(mb_arr);
    Py_DECREF(sub_arr);
    return dict;
}

static PyObject *
extract_hevc_bitcost(AVFrame *frame)
{
    if (!frame->opaque_ref || !frame->opaque_ref->data)
        Py_RETURN_NONE;

    if ((size_t)frame->opaque_ref->size < sizeof(HevcCtuBitCostMap))
        Py_RETURN_NONE;

    const HevcCtuBitCostMap *map = (const HevcCtuBitCostMap *)frame->opaque_ref->data;

    if (map->magic != HEVC_CTU_BIT_COST_MAP_MAGIC ||
        map->version != HEVC_CTU_BIT_COST_MAP_VERSION)
        Py_RETURN_NONE;

    size_t ctu_count = (size_t)map->ctb_stride * map->ctb_height;
    size_t sub_count = (size_t)map->sub_stride * map->sub_height;
    size_t expect_sz = sizeof(HevcCtuBitCostMap)
                        + ctu_count * sizeof(int32_t)
                        + sub_count * sizeof(float);

    if ((size_t)frame->opaque_ref->size < expect_sz)
        Py_RETURN_NONE;

    const int32_t *ctu_costs = hevc_ctu_costs_ptr(map);
    const float *sub_costs = hevc_sub_costs_ptr(map);

    npy_intp ctu_dims[2] = {map->ctb_height, map->ctb_stride};
    npy_intp sub_dims[2] = {map->sub_height, map->sub_stride};

    PyObject *ctu_arr = make_numpy_copy(2, ctu_dims, NPY_INT32,
                                        ctu_costs, ctu_count * sizeof(int32_t));
    PyObject *sub_arr = make_numpy_copy(2, sub_dims, NPY_FLOAT32,
                                        sub_costs, sub_count * sizeof(float));
    if (!ctu_arr || !sub_arr) {
        Py_XDECREF(ctu_arr);
        Py_XDECREF(sub_arr);
        return nullptr;
    }

    PyObject *dict = PyDict_New();
    if (!dict) {
        Py_DECREF(ctu_arr);
        Py_DECREF(sub_arr);
        return nullptr;
    }

    PyDict_SetItemString(dict, "ctu_bit_cost", ctu_arr);
    PyDict_SetItemString(dict, "sub_mb_bit_cost", sub_arr);
    PyDict_SetItemString(dict, "ctb_width", PyLong_FromLong(map->ctb_width));
    PyDict_SetItemString(dict, "ctb_height", PyLong_FromLong(map->ctb_height));
    PyDict_SetItemString(dict, "ctb_stride", PyLong_FromLong(map->ctb_stride));
    PyDict_SetItemString(dict, "log2_ctb_size", PyLong_FromLong(map->log2_ctb_size));
    PyDict_SetItemString(dict, "sub_width", PyLong_FromLong(map->sub_width));
    PyDict_SetItemString(dict, "sub_height", PyLong_FromLong(map->sub_height));
    PyDict_SetItemString(dict, "sub_stride", PyLong_FromLong(map->sub_stride));

    Py_DECREF(ctu_arr);
    Py_DECREF(sub_arr);
    return dict;
}

static PyObject *
extract_vp9_bitcost(AVFrame *frame)
{
    if (!frame->opaque_ref || !frame->opaque_ref->data)
        Py_RETURN_NONE;

    if ((size_t)frame->opaque_ref->size < sizeof(Vp9SbBitCostMap))
        Py_RETURN_NONE;

    const Vp9SbBitCostMap *map = (const Vp9SbBitCostMap *)frame->opaque_ref->data;

    if (map->magic != VP9_SB_BIT_COST_MAP_MAGIC ||
        map->version != VP9_SB_BIT_COST_MAP_VERSION)
        Py_RETURN_NONE;

    size_t sb_count  = (size_t)map->sb_stride  * map->sb_height;
    size_t sub_count = (size_t)map->sub_stride * map->sub_height;
    size_t expect_sz = sizeof(Vp9SbBitCostMap)
                        + sb_count  * sizeof(int32_t)
                        + sub_count * sizeof(float);

    if ((size_t)frame->opaque_ref->size < expect_sz)
        Py_RETURN_NONE;

    const int32_t *sb_costs  = vp9_sb_costs_ptr(map);
    const float   *sub_costs = vp9_sub_costs_ptr(map);

    npy_intp sb_dims[2]  = {map->sb_height,  map->sb_stride};
    npy_intp sub_dims[2] = {map->sub_height, map->sub_stride};

    PyObject *sb_arr  = make_numpy_copy(2, sb_dims,  NPY_INT32,
                                        sb_costs,  sb_count * sizeof(int32_t));
    PyObject *sub_arr = make_numpy_copy(2, sub_dims, NPY_FLOAT32,
                                        sub_costs, sub_count * sizeof(float));
    if (!sb_arr || !sub_arr) {
        Py_XDECREF(sb_arr);
        Py_XDECREF(sub_arr);
        return nullptr;
    }

    PyObject *dict = PyDict_New();
    if (!dict) {
        Py_DECREF(sb_arr);
        Py_DECREF(sub_arr);
        return nullptr;
    }

    PyDict_SetItemString(dict, "ctu_bit_cost",  sb_arr);
    PyDict_SetItemString(dict, "sub_mb_bit_cost", sub_arr);
    PyDict_SetItemString(dict, "ctb_width",     PyLong_FromLong(map->sb_width));
    PyDict_SetItemString(dict, "ctb_height",    PyLong_FromLong(map->sb_height));
    PyDict_SetItemString(dict, "ctb_stride",    PyLong_FromLong(map->sb_stride));
    PyDict_SetItemString(dict, "sub_width",    PyLong_FromLong(map->sub_width));
    PyDict_SetItemString(dict, "sub_height",   PyLong_FromLong(map->sub_height));
    PyDict_SetItemString(dict, "sub_stride",   PyLong_FromLong(map->sub_stride));

    Py_DECREF(sb_arr);
    Py_DECREF(sub_arr);
    return dict;
}

static PyObject *
extract_bitcost(AVFrame *frame)
{
    if (!frame->opaque_ref || !frame->opaque_ref->data)
        Py_RETURN_NONE;
    if ((size_t)frame->opaque_ref->size < sizeof(uint32_t))
        Py_RETURN_NONE;
    uint32_t magic = *(const uint32_t *)frame->opaque_ref->data;
    if (magic == H264_MB_BIT_COST_MAP_MAGIC)
        return extract_h264_bitcost(frame);
    if (magic == HEVC_CTU_BIT_COST_MAP_MAGIC)
        return extract_hevc_bitcost(frame);
    if (magic == VP9_SB_BIT_COST_MAP_MAGIC)
        return extract_vp9_bitcost(frame);
    Py_RETURN_NONE;
}

/* ------------------------------------------------------------------ */
/* Build one frame result dict                                         */
/* ------------------------------------------------------------------ */
static PyObject *
build_frame_dict(AVFrame *frame, int frame_count, AVCodecContext *dec_ctx,
                 int export_bitcost)
{
    PyObject *item = PyDict_New();
    if (!item)
        return nullptr;

    PyDict_SetItemString(item, "frame_idx", PyLong_FromLong(frame_count));

    const char *ptype = "?";
    if (frame->pict_type == AV_PICTURE_TYPE_I) ptype = "I";
    else if (frame->pict_type == AV_PICTURE_TYPE_P) ptype = "P";
    else if (frame->pict_type == AV_PICTURE_TYPE_B) ptype = "B";
    PyDict_SetItemString(item, "pict_type", PyUnicode_FromString(ptype));

    PyDict_SetItemString(item, "width",  PyLong_FromLong(frame->width));
    PyDict_SetItemString(item, "height", PyLong_FromLong(frame->height));

    const char *codec_name = dec_ctx->codec ? dec_ctx->codec->name : "unknown";
    PyDict_SetItemString(item, "codec_name", PyUnicode_FromString(codec_name));

    if (export_bitcost) {
        PyObject *bitcost = extract_bitcost(frame);
        if (!bitcost) {
            Py_DECREF(item);
            return nullptr;
        }
        PyDict_SetItemString(item, "bitcost", bitcost);
        Py_DECREF(bitcost);
    }

    return item;
}

/* ------------------------------------------------------------------ */
/* Frame index from PTS or fallback count                               */
/* ------------------------------------------------------------------ */
static int
frame_idx_from_pts_or_count(AVFrame *frame, int fallback_count, AVRational st_time_base, AVRational fr)
{
    int frame_idx = fallback_count;
    int64_t best_ts = frame->best_effort_timestamp;
    if (best_ts != AV_NOPTS_VALUE && fr.num > 0 && fr.den > 0) {
        frame_idx = (int)av_rescale_q(best_ts, st_time_base, av_inv_q(fr));
    }
    return frame_idx;
}

/* ------------------------------------------------------------------ */
/* Convert AVFrame YUV -> BGR numpy array                              */
/* ------------------------------------------------------------------ */
static PyObject *
convert_frame_to_bgr(AVFrame *frame, int out_w, int out_h)
{
    int dst_w = out_w > 0 ? out_w : frame->width;
    int dst_h = out_h > 0 ? out_h : frame->height;

    struct SwsContext *sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        dst_w, dst_h, AV_PIX_FMT_BGR24,
        SWS_AREA, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        PyErr_SetString(PyExc_RuntimeError, "sws_getContext failed");
        return nullptr;
    }

    uint8_t *dst_data[4] = {nullptr};
    int dst_linesize[4] = {0};
    int ret = av_image_alloc(dst_data, dst_linesize, dst_w, dst_h, AV_PIX_FMT_BGR24, 1);
    if (ret < 0) {
        sws_freeContext(sws_ctx);
        PyErr_SetString(PyExc_RuntimeError, "av_image_alloc failed");
        return nullptr;
    }

    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
    sws_freeContext(sws_ctx);

    npy_intp dims[3] = {dst_h, dst_w, 3};
    PyObject *arr = PyArray_SimpleNew(3, dims, NPY_UINT8);
    if (!arr) {
        av_freep(&dst_data[0]);
        return nullptr;
    }

    uint8_t *arr_data = (uint8_t *)PyArray_DATA((PyArrayObject *)arr);
    int row_bytes = dst_w * 3;
    for (int y = 0; y < dst_h; ++y) {
        memcpy(arr_data + y * row_bytes, dst_data[0] + y * dst_linesize[0], row_bytes);
    }
    av_freep(&dst_data[0]);

    return arr;
}

/* ------------------------------------------------------------------ */
/* Python entry point                                                  */
/* ------------------------------------------------------------------ */
static PyObject *
read_video_fast(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *path = nullptr;
    int thread_count = 1;
    int export_bitcost = 0;
    const char *thread_type_str = "auto";

    static const char *kwlist[] = {"path", "thread_count", "export_bitcost", "thread_type", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|iis",
                                     (char **)kwlist,
                                     &path, &thread_count, &export_bitcost,
                                     &thread_type_str))
        return nullptr;

    int thread_type = FF_THREAD_FRAME;
    if (strcmp(thread_type_str, "slice") == 0) {
        thread_type = FF_THREAD_SLICE;
    } else if (strcmp(thread_type_str, "frame") == 0) {
        thread_type = FF_THREAD_FRAME;
    } else {
        // auto: slice when bitcost is needed (frame threading drops opaque_ref)
        thread_type = export_bitcost ? FF_THREAD_SLICE : FF_THREAD_FRAME;
    }

    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path, nullptr, nullptr) < 0) {
        PyErr_SetString(PyExc_IOError, "Failed to open video file");
        return nullptr;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "Failed to find stream info");
        return nullptr;
    }

    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "No video stream found");
        return nullptr;
    }

    AVStream *st = fmt_ctx->streams[stream_idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "No H264/HEVC/VP9 decoder found");
        return nullptr;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, st->codecpar);

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "skip_loop_filter", "all", 0);
    dec_ctx->thread_count = thread_count;
    dec_ctx->thread_type = thread_type;
    dec_ctx->skip_loop_filter = AVDISCARD_ALL;
    dec_ctx->skip_idct = AVDISCARD_ALL;

    if (avcodec_open2(dec_ctx, dec, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "Failed to open codec");
        return nullptr;
    }
    av_dict_free(&opts);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    PyObject *results = PyList_New(0);
    int frame_count = 0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            int ret = avcodec_send_packet(dec_ctx, pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;

                PyObject *item = build_frame_dict(frame, frame_count, dec_ctx,
                                                    export_bitcost);
                if (!item) {
                    Py_DECREF(results);
                    results = nullptr;
                    goto cleanup;
                }
                PyList_Append(results, item);
                Py_DECREF(item);

                frame_count++;
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

    /* flush */
    avcodec_send_packet(dec_ctx, NULL);
    while (1) {
        int ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;

        PyObject *item = build_frame_dict(frame, frame_count, dec_ctx,
                                            export_bitcost);
        if (!item) {
            Py_DECREF(results);
            results = nullptr;
            goto cleanup;
        }
        PyList_Append(results, item);
        Py_DECREF(item);

        frame_count++;
        av_frame_unref(frame);
    }

cleanup:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    return results;
}

/* ------------------------------------------------------------------ */
/* Python entry point: selected frames only                            */
/* ------------------------------------------------------------------ */
static PyObject *
read_video_fast_selected(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *path = nullptr;
    PyObject *frame_ids_obj = nullptr;
    int thread_count = 1;
    int export_bitcost = 0;
    const char *thread_type_str = "auto";

    PyObject *keyframe_pts_obj = nullptr;
    int export_pixels = 0;
    int out_w = 0, out_h = 0;
    static const char *kwlist[] = {"path", "frame_ids", "thread_count", "export_bitcost", "thread_type", "keyframe_pts", "export_pixels", "out_w", "out_h", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|iisOiii",
                                     (char **)kwlist,
                                     &path, &frame_ids_obj, &thread_count, &export_bitcost,
                                     &thread_type_str, &keyframe_pts_obj,
                                     &export_pixels, &out_w, &out_h))
        return nullptr;

    std::unordered_map<int, int> wanted_counts;
    std::vector<int> wanted_frames_sorted;
    int max_wanted = -1;
    PyObject *seq = PySequence_Fast(frame_ids_obj, "frame_ids must be a sequence of ints");
    if (!seq)
        return nullptr;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *obj = PySequence_Fast_GET_ITEM(seq, i);
        long v = PyLong_AsLong(obj);
        if (PyErr_Occurred()) {
            Py_DECREF(seq);
            return nullptr;
        }
        if (v < 0)
            continue;
        wanted_counts[(int)v] += 1;
        if ((int)v > max_wanted)
            max_wanted = (int)v;
    }
    Py_DECREF(seq);

    for (const auto &kv : wanted_counts) {
        wanted_frames_sorted.push_back(kv.first);
    }
    std::sort(wanted_frames_sorted.begin(), wanted_frames_sorted.end());

    std::vector<double> keyframe_pts;
    if (keyframe_pts_obj && keyframe_pts_obj != Py_None) {
        PyObject *seq_pts = PySequence_Fast(keyframe_pts_obj, "keyframe_pts must be a sequence of floats");
        if (!seq_pts) return nullptr;
        Py_ssize_t n_pts = PySequence_Fast_GET_SIZE(seq_pts);
        for (Py_ssize_t i = 0; i < n_pts; ++i) {
            PyObject *obj = PySequence_Fast_GET_ITEM(seq_pts, i);
            double v = PyFloat_AsDouble(obj);
            if (PyErr_Occurred()) { Py_DECREF(seq_pts); return nullptr; }
            keyframe_pts.push_back(v);
        }
        Py_DECREF(seq_pts);
    }

    PyObject *results = PyList_New(0);
    if (!results)
        return nullptr;
    if (wanted_counts.empty())
        return results;

    int thread_type = FF_THREAD_FRAME;
    if (strcmp(thread_type_str, "slice") == 0) {
        thread_type = FF_THREAD_SLICE;
    } else if (strcmp(thread_type_str, "frame") == 0) {
        thread_type = FF_THREAD_FRAME;
    } else {
        thread_type = export_bitcost ? FF_THREAD_SLICE : FF_THREAD_FRAME;
    }

    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path, nullptr, nullptr) < 0) {
        Py_DECREF(results);
        PyErr_SetString(PyExc_IOError, "Failed to open video file");
        return nullptr;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        Py_DECREF(results);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "Failed to find stream info");
        return nullptr;
    }

    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_idx < 0) {
        Py_DECREF(results);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "No video stream found");
        return nullptr;
    }

    AVStream *st = fmt_ctx->streams[stream_idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        Py_DECREF(results);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "No H264/HEVC/VP9 decoder found");
        return nullptr;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, st->codecpar);

    std::vector<uint8_t> wanted_bitmap;
    if (max_wanted >= 0) {
        wanted_bitmap.assign((size_t)max_wanted + 2, 0);
        for (const auto &kv : wanted_counts) {
            if (kv.first >= 0 && kv.first <= max_wanted)
                wanted_bitmap[(size_t)kv.first] = 1;
        }
    }

    CvrTargetBitcostCtx target_ctx;
    memset(&target_ctx, 0, sizeof(target_ctx));
    target_ctx.magic = CVR_TARGET_BITCOST_MAGIC;
    // Decoder frame_number follows decode order, while requested frame IDs
    // follow presentation order. They diverge for B-frames; GOP seek also
    // resets frame_number after every flush. Until pruning keys on timestamps,
    // target-only collection can silently omit valid selected-frame bitcost.
    target_ctx.enabled = 0;
    target_ctx.max_frame = max_wanted;
    target_ctx.frame_bitmap = wanted_bitmap.empty() ? nullptr : wanted_bitmap.data();
    target_ctx.tolerance = 1;
    dec_ctx->opaque = &target_ctx;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "skip_loop_filter", "all", 0);
    dec_ctx->thread_count = thread_count;
    dec_ctx->thread_type = thread_type;
    dec_ctx->skip_loop_filter = AVDISCARD_ALL;
    if (!export_pixels)
        dec_ctx->skip_idct = AVDISCARD_ALL;

    if (avcodec_open2(dec_ctx, dec, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        Py_DECREF(results);
        PyErr_SetString(PyExc_IOError, "Failed to open codec");
        return nullptr;
    }
    av_dict_free(&opts);

    AVRational stream_base = st->time_base;
    AVRational frame_base = AVRational{st->avg_frame_rate.den, st->avg_frame_rate.num};
    if (frame_base.num == 0) {
        frame_base = AVRational{st->r_frame_rate.den, st->r_frame_rate.num};
    }
    int64_t start_time = st->start_time;
    if (start_time == AV_NOPTS_VALUE) start_time = 0;

    std::vector<std::vector<int>> gop_groups;
    std::vector<int64_t> gop_seek_pts;
    if (!keyframe_pts.empty() && !wanted_frames_sorted.empty()) {
        double fps = av_q2d(AVRational{frame_base.den, frame_base.num});
        if (fps <= 0) fps = 30.0;
        int current_gop = -1;
        for (int fid : wanted_frames_sorted) {
            double frame_ts = fid / fps;
            int gop_idx = 0;
            for (int i = 0; i < (int)keyframe_pts.size(); ++i) {
                if (keyframe_pts[i] <= frame_ts)
                    gop_idx = i;
                else
                    break;
            }
            if (gop_idx != current_gop) {
                current_gop = gop_idx;
                gop_groups.emplace_back();
                gop_seek_pts.push_back(av_rescale_q((int64_t)(keyframe_pts[gop_idx] * AV_TIME_BASE), AV_TIME_BASE_Q, stream_base));
            }
            gop_groups.back().push_back(fid);
        }
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int frame_count = 0;
    int diag_selected_total = 0;
    int diag_selected_valid = 0;
    int diag_selected_none = 0;
    int diag_selected_zero = 0;
    bool stop = false;

    if (!gop_groups.empty()) {
        for (size_t g = 0; g < gop_groups.size(); ++g) {
            int ret = av_seek_frame(fmt_ctx, stream_idx, gop_seek_pts[g], AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                fprintf(stderr, "CVR_SEEK_WARN: av_seek_frame failed for GOP %zu pts=%ld\n", g, (long)gop_seek_pts[g]);
                break;
            }
            avcodec_flush_buffers(dec_ctx);

            std::unordered_set<int> gop_wanted(gop_groups[g].begin(), gop_groups[g].end());
            int gop_remaining = (int)gop_groups[g].size();

            while (gop_remaining > 0 && av_read_frame(fmt_ctx, pkt) >= 0) {
                if (pkt->stream_index != stream_idx) {
                    av_packet_unref(pkt);
                    continue;
                }
                int ret2 = avcodec_send_packet(dec_ctx, pkt);
                av_packet_unref(pkt);
                while (ret2 >= 0) {
                    ret2 = avcodec_receive_frame(dec_ctx, frame);
                    if (ret2 == AVERROR(EAGAIN) || ret2 == AVERROR_EOF)
                        break;
                    if (ret2 < 0) {
                        gop_remaining = 0;
                        break;
                    }

                    int64_t frame_pts = frame->pts;
                    if (frame_pts == AV_NOPTS_VALUE)
                        frame_pts = frame->best_effort_timestamp;
                    int fid = (int)av_rescale_q(frame_pts - start_time, stream_base, frame_base);

                    auto it = gop_wanted.find(fid);
                    if (it == gop_wanted.end()) {
                        for (int off = -1; off <= 1; ++off) {
                            if (off == 0) continue;
                            it = gop_wanted.find(fid + off);
                            if (it != gop_wanted.end()) break;
                        }
                    }

                    if (it != gop_wanted.end()) {
                        int target_fid = *it;
                        if (export_bitcost && cvr_bitcost_diag_enabled_cpp()) {
                            diag_selected_total++;
                            if (!cvr_bitcost_map_is_valid(frame))
                                diag_selected_none++;
                            else if (!cvr_bitcost_map_has_nonzero_mb(frame))
                                diag_selected_zero++;
                            else
                                diag_selected_valid++;
                        }
                        PyObject *item = build_frame_dict(frame, target_fid, dec_ctx,
                                                            export_bitcost);
                        if (!item) {
                            Py_DECREF(results);
                            results = nullptr;
                            goto cleanup_selected;
                        }
                        if (export_pixels) {
                            PyObject *px = convert_frame_to_bgr(frame, out_w, out_h);
                            if (!px) {
                                Py_DECREF(item);
                                Py_DECREF(results);
                                results = nullptr;
                                goto cleanup_selected;
                            }
                            PyDict_SetItemString(item, "pixels", px);
                            Py_DECREF(px);
                        }
                        int repeat = wanted_counts[target_fid];
                        for (int k = 0; k < repeat; ++k) {
                            PyList_Append(results, item);
                        }
                        Py_DECREF(item);
                        gop_wanted.erase(it);
                        gop_remaining--;
                    }
                    av_frame_unref(frame);
                    if (gop_remaining <= 0) break;
                }
                if (gop_remaining <= 0) break;
            }

            // Flush decoder to collect any buffered frames for this GOP
            if (gop_remaining > 0) {
                avcodec_send_packet(dec_ctx, NULL);
                while (1) {
                    int ret_flush = avcodec_receive_frame(dec_ctx, frame);
                    if (ret_flush == AVERROR(EAGAIN) || ret_flush == AVERROR_EOF)
                        break;
                    if (ret_flush < 0)
                        break;

                    int64_t frame_pts = frame->pts;
                    if (frame_pts == AV_NOPTS_VALUE)
                        frame_pts = frame->best_effort_timestamp;
                    int fid = (int)av_rescale_q(frame_pts - start_time, stream_base, frame_base);

                    auto it = gop_wanted.find(fid);
                    if (it == gop_wanted.end()) {
                        for (int off = -1; off <= 1; ++off) {
                            if (off == 0) continue;
                            it = gop_wanted.find(fid + off);
                            if (it != gop_wanted.end()) break;
                        }
                    }

                    if (it != gop_wanted.end()) {
                        int target_fid = *it;
                        if (export_bitcost && cvr_bitcost_diag_enabled_cpp()) {
                            diag_selected_total++;
                            if (!cvr_bitcost_map_is_valid(frame))
                                diag_selected_none++;
                            else if (!cvr_bitcost_map_has_nonzero_mb(frame))
                                diag_selected_zero++;
                            else
                                diag_selected_valid++;
                        }
                        PyObject *item = build_frame_dict(frame, target_fid, dec_ctx,
                                                            export_bitcost);
                        if (!item) {
                            Py_DECREF(results);
                            results = nullptr;
                            goto cleanup_selected;
                        }
                        if (export_pixels) {
                            PyObject *px = convert_frame_to_bgr(frame, out_w, out_h);
                            if (!px) {
                                Py_DECREF(item);
                                Py_DECREF(results);
                                results = nullptr;
                                goto cleanup_selected;
                            }
                            PyDict_SetItemString(item, "pixels", px);
                            Py_DECREF(px);
                        }
                        int repeat = wanted_counts[target_fid];
                        for (int k = 0; k < repeat; ++k) {
                            PyList_Append(results, item);
                        }
                        Py_DECREF(item);
                        gop_wanted.erase(it);
                        gop_remaining--;
                    }
                    av_frame_unref(frame);
                    if (gop_remaining <= 0) break;
                }
            }
        }
    } else {
        while (!stop && av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == stream_idx) {
                int ret = avcodec_send_packet(dec_ctx, pkt);
                while (ret >= 0) {
                    ret = avcodec_receive_frame(dec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0) {
                        Py_DECREF(results);
                        results = nullptr;
                        goto cleanup_selected;
                    }

                    auto it = wanted_counts.find(frame_count);
                    if (it != wanted_counts.end() && it->second > 0) {
                        if (export_bitcost && cvr_bitcost_diag_enabled_cpp()) {
                            diag_selected_total++;
                            if (!cvr_bitcost_map_is_valid(frame))
                                diag_selected_none++;
                            else if (!cvr_bitcost_map_has_nonzero_mb(frame))
                                diag_selected_zero++;
                            else
                                diag_selected_valid++;
                        }
                        PyObject *item = build_frame_dict(frame, frame_count, dec_ctx,
                                                            export_bitcost);
                        if (!item) {
                            Py_DECREF(results);
                            results = nullptr;
                            goto cleanup_selected;
                        }
                        if (export_pixels) {
                            PyObject *px = convert_frame_to_bgr(frame, out_w, out_h);
                            if (!px) {
                                Py_DECREF(item);
                                Py_DECREF(results);
                                results = nullptr;
                                goto cleanup_selected;
                            }
                            PyDict_SetItemString(item, "pixels", px);
                            Py_DECREF(px);
                        }
                        for (int k = 0; k < it->second; ++k) {
                            PyList_Append(results, item);
                        }
                        Py_DECREF(item);
                        wanted_counts.erase(it);
                        if (wanted_counts.empty() || frame_count >= max_wanted)
                            stop = true;
                    }

                    frame_count++;
                    av_frame_unref(frame);
                    if (stop)
                        break;
                }
            }
            av_packet_unref(pkt);
        }

        if (!stop) {
            avcodec_send_packet(dec_ctx, NULL);
            while (1) {
                int ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                auto it = wanted_counts.find(frame_count);
                if (it != wanted_counts.end() && it->second > 0) {
                    PyObject *item = build_frame_dict(frame, frame_count, dec_ctx,
                                                        export_bitcost);
                    if (!item) {
                        Py_DECREF(results);
                        results = nullptr;
                        goto cleanup_selected;
                    }
                    if (export_pixels) {
                        PyObject *px = convert_frame_to_bgr(frame, out_w, out_h);
                        if (!px) {
                            Py_DECREF(item);
                            Py_DECREF(results);
                            results = nullptr;
                            goto cleanup_selected;
                        }
                        PyDict_SetItemString(item, "pixels", px);
                        Py_DECREF(px);
                    }
                    for (int k = 0; k < it->second; ++k) {
                        PyList_Append(results, item);
                    }
                    Py_DECREF(item);
                    wanted_counts.erase(it);
                    if (wanted_counts.empty() || frame_count >= max_wanted) {
                        av_frame_unref(frame);
                        break;
                    }
                }
                frame_count++;
                av_frame_unref(frame);
            }
        }
    }

cleanup_selected:
    if (export_bitcost && cvr_bitcost_diag_enabled_cpp()) {
        fprintf(stderr, "CVR_BITCOST_DIAG selected_total=%d valid=%d none=%d zero=%d decoded_until=%d remaining=%zu\n",
                diag_selected_total, diag_selected_valid, diag_selected_none, diag_selected_zero,
                frame_count, wanted_counts.size());
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    return results;
}

/* ------------------------------------------------------------------ */
/* Segment seek + decode for parallel workers                           */
/* ------------------------------------------------------------------ */
static PyObject *
read_video_fast_selected_segment(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *path = nullptr;
    PyObject *frame_ids_obj = nullptr;
    int seek_frame = 0;
    int end_frame = -1;
    int thread_count = 1;
    int export_bitcost = 0;
    const char *thread_type_str = "auto";
    int export_pixels = 0;
    int out_w = 0;
    int out_h = 0;

    static const char *kwlist[] = {
        "path", "frame_ids", "seek_frame", "end_frame",
        "thread_count", "export_bitcost", "thread_type",
        "export_pixels", "out_w", "out_h", nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sOii|iisiii",
                                     (char **)kwlist,
                                     &path, &frame_ids_obj, &seek_frame, &end_frame,
                                     &thread_count, &export_bitcost, &thread_type_str,
                                     &export_pixels, &out_w, &out_h))
        return nullptr;

    std::unordered_map<int, int> wanted_counts;
    int max_wanted = -1;
    PyObject *seq = PySequence_Fast(frame_ids_obj, "frame_ids must be a sequence of ints");
    if (!seq)
        return nullptr;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *obj = PySequence_Fast_GET_ITEM(seq, i);
        long v = PyLong_AsLong(obj);
        if (PyErr_Occurred()) {
            Py_DECREF(seq);
            return nullptr;
        }
        if (v < 0)
            continue;
        wanted_counts[(int)v] += 1;
        if ((int)v > max_wanted)
            max_wanted = (int)v;
    }
    Py_DECREF(seq);

    PyObject *results = PyList_New(0);
    if (!results)
        return nullptr;
    if (wanted_counts.empty())
        return results;
    if (end_frame < 0)
        end_frame = max_wanted;

    int thread_type = FF_THREAD_FRAME;
    if (strcmp(thread_type_str, "slice") == 0) {
        thread_type = FF_THREAD_SLICE;
    } else if (strcmp(thread_type_str, "frame") == 0) {
        thread_type = FF_THREAD_FRAME;
    } else {
        thread_type = export_bitcost ? FF_THREAD_SLICE : FF_THREAD_FRAME;
    }

    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path, nullptr, nullptr) < 0) {
        Py_DECREF(results);
        PyErr_SetString(PyExc_IOError, "Failed to open video file");
        return nullptr;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        Py_DECREF(results);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "Failed to find stream info");
        return nullptr;
    }

    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_idx < 0) {
        Py_DECREF(results);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "No video stream found");
        return nullptr;
    }

    AVStream *st = fmt_ctx->streams[stream_idx];
    AVRational fr = av_guess_frame_rate(fmt_ctx, st, nullptr);
    if (fr.num <= 0 || fr.den <= 0)
        fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = AVRational{30, 1};

    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        Py_DECREF(results);
        avformat_close_input(&fmt_ctx);
        PyErr_SetString(PyExc_IOError, "No H264/HEVC/VP9 decoder found");
        return nullptr;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, st->codecpar);

    CvrTargetBitcostCtx target_ctx;
    memset(&target_ctx, 0, sizeof(target_ctx));
    target_ctx.magic = CVR_TARGET_BITCOST_MAGIC;
    target_ctx.enabled = 0;  /* Segment seek uses global PTS mapping; avoid local decoder frame-number pruning. */
    target_ctx.max_frame = max_wanted;
    target_ctx.frame_bitmap = nullptr;
    target_ctx.tolerance = 1;
    dec_ctx->opaque = &target_ctx;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "skip_loop_filter", "all", 0);
    dec_ctx->thread_count = thread_count;
    dec_ctx->thread_type = thread_type;
    dec_ctx->skip_loop_filter = AVDISCARD_ALL;
    if (!export_pixels)
        dec_ctx->skip_idct = AVDISCARD_ALL;

    if (avcodec_open2(dec_ctx, dec, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        Py_DECREF(results);
        PyErr_SetString(PyExc_IOError, "Failed to open codec");
        return nullptr;
    }
    av_dict_free(&opts);

    int64_t seek_ts = av_rescale_q((int64_t)(seek_frame < 0 ? 0 : seek_frame), av_inv_q(fr), st->time_base);
    if (seek_ts < 0)
        seek_ts = 0;
    av_seek_frame(fmt_ctx, stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int decoded_count = 0;
    bool stop = false;

    while (!stop && av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            int ret = avcodec_send_packet(dec_ctx, pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0) {
                    Py_DECREF(results);
                    results = nullptr;
                    goto cleanup_segment;
                }

                int frame_idx = frame_idx_from_pts_or_count(frame, decoded_count, st->time_base, fr);
                decoded_count++;

                if (frame_idx > end_frame && frame_idx > max_wanted) {
                    av_frame_unref(frame);
                    stop = true;
                    break;
                }

                auto it = wanted_counts.find(frame_idx);
                if (it != wanted_counts.end() && it->second > 0) {
                    PyObject *item = build_frame_dict(frame, frame_idx, dec_ctx, export_bitcost);
                    if (!item) {
                        Py_DECREF(results);
                        results = nullptr;
                        goto cleanup_segment;
                    }
                    if (export_pixels) {
                        PyObject *px = convert_frame_to_bgr(frame, out_w, out_h);
                        if (!px) {
                            Py_DECREF(item);
                            Py_DECREF(results);
                            results = nullptr;
                            goto cleanup_segment;
                        }
                        PyDict_SetItemString(item, "pixels", px);
                        Py_DECREF(px);
                    }
                    for (int k = 0; k < it->second; ++k) {
                        PyList_Append(results, item);
                    }
                    Py_DECREF(item);
                    wanted_counts.erase(it);
                    if (wanted_counts.empty())
                        stop = true;
                }

                av_frame_unref(frame);
                if (stop)
                    break;
            }
        }
        av_packet_unref(pkt);
    }

    if (!stop) {
        avcodec_send_packet(dec_ctx, NULL);
        while (1) {
            int ret = avcodec_receive_frame(dec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            int frame_idx = frame_idx_from_pts_or_count(frame, decoded_count, st->time_base, fr);
            decoded_count++;

            auto it = wanted_counts.find(frame_idx);
            if (it != wanted_counts.end() && it->second > 0) {
                PyObject *item = build_frame_dict(frame, frame_idx, dec_ctx, export_bitcost);
                if (!item) {
                    Py_DECREF(results);
                    results = nullptr;
                    goto cleanup_segment;
                }
                if (export_pixels) {
                    PyObject *px = convert_frame_to_bgr(frame, out_w, out_h);
                    if (!px) {
                        Py_DECREF(item);
                        Py_DECREF(results);
                        results = nullptr;
                        goto cleanup_segment;
                    }
                    PyDict_SetItemString(item, "pixels", px);
                    Py_DECREF(px);
                }
                for (int k = 0; k < it->second; ++k) {
                    PyList_Append(results, item);
                }
                Py_DECREF(item);
                wanted_counts.erase(it);
                if (wanted_counts.empty()) {
                    av_frame_unref(frame);
                    break;
                }
            }
            av_frame_unref(frame);
        }
    }

cleanup_segment:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    return results;
}

static PyMethodDef FastMethods[] = {
    {"read_video_fast", (PyCFunction)read_video_fast, METH_VARARGS | METH_KEYWORDS,
     "read_video_fast(path, thread_count=1, export_bitcost=0, thread_type='auto') -> list of frame metadata dicts"},
    {"read_video_fast_selected", (PyCFunction)read_video_fast_selected, METH_VARARGS | METH_KEYWORDS,
     "read_video_fast_selected(path, frame_ids, thread_count=1, export_bitcost=0, thread_type='auto', keyframe_pts=None, export_pixels=0, out_w=0, out_h=0) -> selected frame metadata dicts"},
    {"read_video_fast_selected_segment", (PyCFunction)read_video_fast_selected_segment, METH_VARARGS | METH_KEYWORDS,
     "read_video_fast_selected_segment(path, frame_ids, seek_frame, end_frame, thread_count=1, export_bitcost=0, thread_type='auto', export_pixels=0, out_w=0, out_h=0) -> selected frame metadata dicts"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef fastmodule = {
    PyModuleDef_HEAD_INIT,
    "cv_reader_fast",
    "Fast stripped-down video decoder with optional bitcost export",
    -1,
    FastMethods
};

PyMODINIT_FUNC PyInit_cv_reader_fast(void) {
    PyObject *m = PyModule_Create(&fastmodule);
    import_array();   /* numpy – safe in C++ with numpy >= 1.19 */
    return m;
}

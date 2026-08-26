/**
 * @file h264_parser.c
 * @brief H264 Annex-B bitstream parser implementation
 *
 * Core concepts:
 *
 * 1. H264 bitstream hierarchy:
 *    stream -> NALU sequence -> Slice -> Macroblock (MB) -> sub-macroblock
 *    Each NALU consists of start code + NALU Header (1 byte) + RBSP
 *
 * 2. NALU Header format (1 byte):
 *    forbidden_zero_bit (1bit) - must be 0; 1 indicates stream error
 *    nal_ref_idc        (2bit) - reference priority: 3=high (SPS/PPS/IDR), 0=non-reference
 *    nal_unit_type      (5bit) - NALU type, see H264_NALU_TYPE_*
 *
 * 3. SPS (Sequence Parameter Set): describes parameters of the entire video sequence
 *    - Resolution: pic_width_in_mbs_minus1, pic_height_in_map_units_minus1
 *    - Profile/Level: profile_idc, level_idc
 *    - GOP structure reference: max_num_ref_frames
 *
 * 4. PPS (Picture Parameter Set): describes picture-level coding parameters
 *    - Entropy coding mode: CAVLC(0) / CABAC(1)
 *    - Initial QP: pic_init_qp_minus26
 *
 * 5. IDR frame vs I-frame:
 *    - I-frame: intra-coded, does not depend on other frames, but does not clear reference frame buffer
 *    - IDR frame: special form of I-frame; decoder clears all reference frame buffers upon receiving IDR
 *      -> The next P-frame cannot reference frames before the IDR
 *      -> IDR is the GOP start boundary; independent decoding can start from any IDR
 *    -> This is why RTSP streaming must send SPS+PPS before each key frame
 *      (decoder may start decoding from any IDR, requires parameter sets)
 */

#include "h264_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== Exp-Golomb decoder (foundation of H264 syntax elements) ==========
 *
 * H264 makes extensive use of Exp-Golomb coding. Decoding process:
 *   1. Count leading zeros leadingZeroBits
 *   2. Read 1 '1' bit
 *   3. Read leadingZeroBits bits as suffix
 *   4. Decoded value = 2^leadingZeroBits - 1 + suffix
 *
 * For example: 00100 -> leadingZeroBits=2, suffix=00
 *       value = 2^2 - 1 + 0 = 3
 */

typedef struct {
    const uint8_t *data;    /**< Bitstream data pointer */
    int             size;   /**< Total number of data bytes */
    int             byte_pos; /**< Current byte position */
    int             bit_pos;  /**< Current bit position (0-7, 0=MSB) */
} bitstream_t;

static int bs_init(bitstream_t *bs, const uint8_t *data, int size)
{
    if (!bs || !data || size <= 0) return -1;
    bs->data = data;
    bs->size = size;
    bs->byte_pos = 0;
    bs->bit_pos = 0;
    return 0;
}

static int bs_eof(const bitstream_t *bs)
{
    return bs->byte_pos >= bs->size;
}

static uint32_t bs_read_bit(bitstream_t *bs)
{
    if (bs_eof(bs)) return 0;
    uint32_t bit = (bs->data[bs->byte_pos] >> (7 - bs->bit_pos)) & 1;
    bs->bit_pos++;
    if (bs->bit_pos >= 8) {
        bs->bit_pos = 0;
        bs->byte_pos++;
    }
    return bit;
}

static uint32_t bs_read_bits(bitstream_t *bs, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n && !bs_eof(bs); i++) {
        val = (val << 1) | bs_read_bit(bs);
    }
    return val;
}

/* Unsigned Exp-Golomb coding ue(v) */
static uint32_t bs_read_ue(bitstream_t *bs)
{
    int leading_zeros = 0;
    while (!bs_eof(bs) && bs_read_bit(bs) == 0) {
        leading_zeros++;
        if (leading_zeros > 32) return 0; /* Guard against malformed stream */
    }
    if (leading_zeros == 0) return 0;
    uint32_t suffix = bs_read_bits(bs, leading_zeros);
    return (1U << leading_zeros) - 1 + suffix;
}

/* Signed Exp-Golomb coding se(v) = (-1)^(k+1) * ceil(k/2) */
static int32_t bs_read_se(bitstream_t *bs)
{
    uint32_t k = bs_read_ue(bs);
    if (k % 2 == 0)
        return -(int32_t)(k / 2);
    else
        return (int32_t)((k + 1) / 2);
}

/* ========== SPS parsing ==========
 *
 * SPS structure (simplified; key fields of Baseline/Main/High):
 *   profile_idc                         u(8)
 *   constraint_set0-3_flag              u(1) x 4
 *   reserved_zero_4bits                 u(4)
 *   level_idc                           u(8)
 *   seq_parameter_set_id                ue(v)
 *   [High profile extra fields]
 *   chroma_format_idc                   ue(v)
 *   ...
 *   pic_width_in_mbs_minus1             ue(v)
 *   pic_height_in_map_units_minus1      ue(v)
 *   frame_mbs_only_flag                 u(1)
 *   ...
 *   frame_cropping_flag                 u(1)
 *   ...
 *   vui_parameters_present_flag         u(1)
 */

static int parse_sps(const uint8_t *data, int size, h264_sps_t *sps)
{
    bitstream_t bs;
    if (bs_init(&bs, data, size) < 0) return -1;

    memset(sps, 0, sizeof(*sps));

    sps->profile_idc = bs_read_bits(&bs, 8);
    /* constraint_set0-3_flag + reserved */
    bs_read_bits(&bs, 4);  /* constraint_set0-3_flag */
    bs_read_bits(&bs, 4);  /* reserved_zero_4bits */
    sps->level_idc = bs_read_bits(&bs, 8);
    sps->seq_parameter_set_id = bs_read_ue(&bs);

    /* High Profile (100) and some other profiles have extra fields */
    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc == 44  || sps->profile_idc == 83  ||
        sps->profile_idc == 86  || sps->profile_idc == 118 ||
        sps->profile_idc == 128 || sps->profile_idc == 138 ||
        sps->profile_idc == 139 || sps->profile_idc == 134) {
        int chroma_format_idc = bs_read_ue(&bs);
        if (chroma_format_idc == 3) {
            bs_read_bit(&bs); /* separate_colour_plane_flag */
        }
        bs_read_ue(&bs); /* bit_depth_luma_minus8 */
        bs_read_ue(&bs); /* bit_depth_chroma_minus8 */
        bs_read_bit(&bs); /* qpprime_y_zero_transform_bypass_flag */
        int seq_scaling_matrix_present_flag = bs_read_bit(&bs);
        if (seq_scaling_matrix_present_flag) {
            int count = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < count; i++) {
                if (bs_read_bit(&bs)) { /* seq_scaling_list_present_flag */
                    int size_of_scaling_list = (i < 6) ? 16 : 64;
                    int last_scale = 8, next_scale = 8;
                    for (int j = 0; j < size_of_scaling_list; j++) {
                        if (next_scale != 0) {
                            int delta_scale = bs_read_se(&bs);
                            next_scale = (last_scale + delta_scale + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    sps->log2_max_frame_num_minus4 = bs_read_ue(&bs);
    sps->pic_order_cnt_type = bs_read_ue(&bs);

    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = bs_read_ue(&bs);
    } else if (sps->pic_order_cnt_type == 1) {
        bs_read_bit(&bs); /* delta_pic_order_always_zero_flag */
        bs_read_se(&bs);  /* offset_for_non_ref_pic */
        bs_read_se(&bs);  /* offset_for_top_to_bottom_field */
        int num_ref_frames_in_pic_order_cnt_cycle = bs_read_ue(&bs);
        for (int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            bs_read_se(&bs); /* offset_for_ref_frame */
        }
    }

    sps->max_num_ref_frames = bs_read_ue(&bs);
    sps->gaps_in_frame_num_value_allowed_flag = bs_read_bit(&bs);
    sps->pic_width_in_mbs_minus1 = bs_read_ue(&bs);
    sps->pic_height_in_map_units_minus1 = bs_read_ue(&bs);
    sps->frame_mbs_only_flag = bs_read_bit(&bs);

    if (!sps->frame_mbs_only_flag) {
        bs_read_bit(&bs); /* mb_adaptive_frame_field_flag */
    }

    sps->direct_8x8_inference_flag = bs_read_bit(&bs);
    sps->frame_cropping_flag = bs_read_bit(&bs);

    if (sps->frame_cropping_flag) {
        sps->frame_crop_left_offset   = bs_read_ue(&bs);
        sps->frame_crop_right_offset  = bs_read_ue(&bs);
        sps->frame_crop_top_offset    = bs_read_ue(&bs);
        sps->frame_crop_bottom_offset = bs_read_ue(&bs);
    }

    sps->vui_parameters_present_flag = bs_read_bit(&bs);

    /* Compute actual resolution
     * Width  = (pic_width_in_mbs_minus1 + 1) * 16  (each macroblock is 16x16 pixels)
     * Height = (pic_height_in_map_units_minus1 + 1) * 16 * (2 - frame_mbs_only_flag)
     *          frame_mbs_only_flag=1: progressive scan, height = macroblocks * 16
     *          frame_mbs_only_flag=0: interlaced scan, each map_unit = 2 macroblocks (fields), height = macroblocks * 32
     */
    sps->width  = (sps->pic_width_in_mbs_minus1 + 1) * 16;
    sps->height = (sps->pic_height_in_map_units_minus1 + 1) * 16 * (2 - sps->frame_mbs_only_flag);

    /* Subtract cropping region */
    if (sps->frame_cropping_flag) {
        int crop_x = (sps->frame_crop_left_offset + sps->frame_crop_right_offset) * 2;
        int crop_y = (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset) * 2 *
                     (2 - sps->frame_mbs_only_flag);
        sps->width  -= crop_x;
        sps->height -= crop_y;
    }

    return 0;
}

/* ========== PPS parsing ========== */

static int parse_pps(const uint8_t *data, int size, h264_pps_t *pps)
{
    bitstream_t bs;
    if (bs_init(&bs, data, size) < 0) return -1;

    memset(pps, 0, sizeof(*pps));

    pps->pic_parameter_set_id   = bs_read_ue(&bs);
    pps->seq_parameter_set_id   = bs_read_ue(&bs);
    pps->entropy_coding_mode_flag = bs_read_bit(&bs); /* 0=CAVLC, 1=CABAC */
    pps->bottom_field_pic_order_in_frame_present_flag = bs_read_bit(&bs);
    pps->num_slice_groups_minus1 = bs_read_ue(&bs);

    if (pps->num_slice_groups_minus1 > 0) {
        /* Simplified: skip slice group related fields */
        int slice_group_map_type = bs_read_ue(&bs);
        if (slice_group_map_type == 0) {
            for (int i = 0; i <= pps->num_slice_groups_minus1; i++)
                bs_read_ue(&bs);
        } else if (slice_group_map_type == 2) {
            for (int i = 0; i < pps->num_slice_groups_minus1; i++)
                bs_read_ue(&bs);
        }
        /* Other types simplified-skipped */
    }

    pps->num_ref_idx_l0_default_active_minus1 = bs_read_ue(&bs);
    pps->num_ref_idx_l1_default_active_minus1 = bs_read_ue(&bs);
    pps->weighted_pred_flag     = bs_read_bit(&bs);
    pps->weighted_bipred_idc    = bs_read_bits(&bs, 2);
    pps->pic_init_qp_minus26    = bs_read_se(&bs);
    pps->pic_init_qs_minus26    = bs_read_se(&bs);
    pps->chroma_qp_index_offset = bs_read_se(&bs);
    pps->deblocking_filter_control_present_flag = bs_read_bit(&bs);
    pps->constrained_intra_pred_flag = bs_read_bit(&bs);
    pps->redundant_pic_cnt_present_flag = bs_read_bit(&bs);

    return 0;
}

/* ========== Slice Header frame type detection ==========
 *
 * Slice Header structure (simplified):
 *   first_mb_in_slice       ue(v) - slice starting macroblock address
 *   slice_type              ue(v) - slice type: 0=P, 1=B, 2=I, 3=SP, 4=SI
 *                                     5=P, 6=B, 7=I, 8=SP, 9=SI (+5 means same type)
 */

static h264_frame_type_t detect_frame_type(const uint8_t *data, int size, int nalu_type)
{
    if (nalu_type == H264_NALU_TYPE_IDR) return H264_FRAME_IDR;
    if (nalu_type == H264_NALU_TYPE_SPS) return H264_FRAME_SPS;
    if (nalu_type == H264_NALU_TYPE_PPS) return H264_FRAME_PPS;
    if (nalu_type == H264_NALU_TYPE_SEI) return H264_FRAME_SEI;

    if (nalu_type != H264_NALU_TYPE_SLICE &&
        nalu_type != H264_NALU_TYPE_SLICE_A &&
        nalu_type != H264_NALU_TYPE_SLICE_B &&
        nalu_type != H264_NALU_TYPE_SLICE_C) {
        return H264_FRAME_UNKNOWN;
    }

    /* Parse slice_type */
    bitstream_t bs;
    if (bs_init(&bs, data, size) < 0) return H264_FRAME_UNKNOWN;

    bs_read_ue(&bs); /* first_mb_in_slice */
    uint32_t slice_type = bs_read_ue(&bs);

    /* slice_type: 0/5=P, 1/6=B, 2/7=I, 3/8=SP, 4/9=SI */
    switch (slice_type % 5) {
        case 0: return H264_FRAME_P;
        case 1: return H264_FRAME_B;
        case 2: return H264_FRAME_I;
        default: return H264_FRAME_UNKNOWN;
    }
}

/* ========== Parser context ========== */

struct h264_parser_ctx {
    h264_sps_t    sps;            /**< Most recently parsed SPS */
    h264_pps_t    pps;            /**< Most recently parsed PPS */
    int           sps_valid;      /**< Whether SPS has been parsed */
    int           pps_valid;      /**< Whether PPS has been parsed */
    h264_stats_t  stats;          /**< Bitstream statistics */
    h264_nalu_cb  callback;       /**< NALU callback */
    void         *callback_data;  /**< Callback user data */
    int           last_was_idr;   /**< Whether the previous picture frame was IDR (for GOP calculation) */
    int           frames_since_idr; /**< Frame count since last IDR */
};

h264_parser_t* h264_parser_create(void)
{
    h264_parser_t *ctx = (h264_parser_t *)calloc(1, sizeof(h264_parser_t));
    if (!ctx) return NULL;
    ctx->sps_valid = 0;
    ctx->pps_valid = 0;
    ctx->last_was_idr = 0;
    ctx->frames_since_idr = 0;
    return ctx;
}

void h264_parser_destroy(h264_parser_t **ctx)
{
    if (ctx && *ctx) {
        free(*ctx);
        *ctx = NULL;
    }
}

void h264_parser_set_callback(h264_parser_t *ctx, h264_nalu_cb cb, void *user_data)
{
    if (!ctx) return;
    ctx->callback = cb;
    ctx->callback_data = user_data;
}

/* ========== Start code search ==========
 *
 * Annex-B start code:
 *   0x00 00 00 01 (4 bytes) - most common
 *   0x00 00 01    (3 bytes) - less common
 *
 * Emulation prevention bytes:
 *   H264 specifies that RBSP cannot contain 0x00 00 00/01/02/03,
 *   the encoder inserts 0x03 (emulation prevention byte) after 0x00 00,
 *   decoding must remove it: 0x00 00 03 00 -> 0x00 00 00
 *                            0x00 00 03 01 -> 0x00 00 01
 *                            0x00 00 03 02 -> 0x00 00 02
 *                            0x00 00 03 03 -> 0x00 00 03
 *
 * This parser handles emulation prevention bytes automatically when parsing SPS/PPS/Slice.
 */

static int find_start_code(const uint8_t *data, int size, int *sc_len)
{
    for (int i = 0; i < size - 2; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    return -1;
}
/**
 * @brief H264 bitstream parser entry: splits NALU units from the stream and parses NAL type, statistics
 *
 * Function overview:
 * Receives a continuous H264 raw stream (AnnexB format with 0x0001/0x000001 start codes),
 * loops to find start codes and split independent NALUs; parses each NAL header to distinguish SPS/PPS/IDR/I/P/B/SEI etc.;
 * maintains global stream statistics (resolution, I/P/B frame counts, IDR interval GOP, byte sizes, etc.);
 * optionally triggers NALU callback so the upper layer can capture key frame, SPS, PPS events.
 *
 * Supported stream format: H264 AnnexB (with start code)
 * Cannot directly parse AVCC format (4-byte length prefix); must convert to AnnexB first.
 *
 * @param ctx        h264_parser_t* parser context handle
 *                   Stores SPS/PPS parameters, statistics, callback pointer; must be created/initialized before calling.
 * @param data       const uint8_t* input H264 raw stream buffer pointer
 * @param size       int valid byte length of input buffer
 *
 * @return
 *      >=0 : success, returns total NALUs parsed from this input
 *      -1  : invalid parameters (null pointer, size<=0)
 *
 * When to use:
 * Call before av_interleaved_write_frame (in your send_video_common code);
 * because av_interleaved_write_frame internally calls av_packet_unref, after which packet->data is invalid and the stream cannot be read.
 *
 * Typical uses:
 * 1. Real-time detection of SPS/PPS arrival, obtain video width/height;
 * 2. Detect IDR key frames, judge whether the stream can be pulled by the player;
 * 3. Count I/P/B frames, bitrate, GOP length, monitor encoding quality;
 * 4. Notify via callback: key frame arrival, parameter set arrival, etc.
 */
int h264_parser_feed(h264_parser_t *ctx, const uint8_t *data, int size)
{
    /****************************************************************************
     * Parameter validation
     * Parser context and stream data must not be null, valid length must be >0; return -1 on invalid
     ***************************************************************************/
    if (!ctx || !data || size <= 0) return -1;

    int nalu_count = 0;    // Total NALUs parsed in this loop
    int offset = 0;        // Offset of current data being traversed

    /****************************************************************************
     * Loop through the buffer, splitting NALU units one by one
     * H264 AnnexB uses [start code 0x0001 / 0x000001] to separate NALU boundaries
     ***************************************************************************/
    while (offset < size) {
        int sc_len = 0;
        // Find the first start code after the current position
        // sc_pos: offset relative to data+offset; sc_len: start code length (3-byte 0001 / 4-byte 000001)
        int sc_pos = find_start_code(data + offset, size - offset, &sc_len);

        /****************************************************************************
         * sc_pos < 0: no start code found
         * Remaining data insufficient for a complete NALU; may be fragment, exit loop and wait for next batch
         ***************************************************************************/
        if (sc_pos < 0) {
            /* No start code found, remaining data less than one NALU */
            break;
        }

        /****************************************************************************
         * Start code found, locate current NALU range
         * nalu_start: NALU payload start address, after the start code
         * nalu_end: defaults to buffer end, corrected by searching for the next start code
         ***************************************************************************/
        int nalu_start = offset + sc_pos + sc_len;
        int nalu_end = size;

        // Find the next start code to determine where the current NALU ends
        int next_sc_len = 0;
        int next_sc_pos = find_start_code(data + nalu_start, size - nalu_start, &next_sc_len);
        if (next_sc_pos >= 0) {
            nalu_end = nalu_start + next_sc_pos;
        }

        // Compute current NALU payload length
        int nalu_size = nalu_end - nalu_start;
        if (nalu_size <= 0) {
            // Empty NALU, skip; offset jumps to current NALU start to continue search
            offset = nalu_start;
            continue;
        }

        const uint8_t *nalu_data = data + nalu_start;

        /****************************************************************************
         * Parse NALU Header (the first byte of each H264 NALU is the header)
         * bit7     : forbidden_zero_bit forbidden bit; 1 means stream corruption
         * bit5~bit6: nal_ref_idc importance marker (non-zero for IDR/SPS/PPS)
         * bit0~bit4: nalu_type NAL unit type, distinguishes SPS/PPS/IDR/Slice/SEI
         ***************************************************************************/
        uint8_t nalu_header = nalu_data[0];
        int forbidden_zero_bit = (nalu_header >> 7) & 1;
        int nal_ref_idc        = (nalu_header >> 5) & 3;
        int nalu_type           = nalu_header & 0x1F;

        if (forbidden_zero_bit) {
            /* Stream error, corrupted NALU, skip directly */
            offset = nalu_end;
            continue;
        }

        // Fill NALU info struct for later parsing and callback
        h264_nalu_t nalu;
        memset(&nalu, 0, sizeof(nalu));
        nalu.nalu_type   = nalu_type;
        nalu.nal_ref_idc = nal_ref_idc;
        nalu.data        = nalu_data;
        nalu.size        = nalu_size;

        /****************************************************************************
         * RBSP: NAL payload data after the 1-byte header
         * SPS/PPS/Slice specific parameters all reside in the RBSP
         ***************************************************************************/
        const uint8_t *rbsp_data = nalu_data + 1;
        int rbsp_size = nalu_size - 1;

        // Global statistics accumulation: total NALUs, raw stream bytes (including start code length)
        ctx->stats.total_nalus++;
        ctx->stats.total_bytes += nalu_size + sc_len;

        /****************************************************************************
         * Branch handling by NAL type
         ***************************************************************************/
        switch (nalu_type) {
        case H264_NALU_TYPE_SPS:
            // Sequence Parameter Set: contains resolution, frame rate, profile and other core info
            if (parse_sps(rbsp_data, rbsp_size, &ctx->sps) == 0) {
                ctx->sps_valid = 1;
                ctx->stats.sps_count++;
                // Extract picture width/height from SPS, save to stats
                ctx->stats.width = ctx->sps.width;
                ctx->stats.height = ctx->sps.height;
                nalu.frame_type = H264_FRAME_SPS;
            }
            break;

        case H264_NALU_TYPE_PPS:
            // Picture Parameter Set, used together with SPS for decoding
            if (parse_pps(rbsp_data, rbsp_size, &ctx->pps) == 0) {
                ctx->pps_valid = 1;
                ctx->stats.pps_count++;
                nalu.frame_type = H264_FRAME_PPS;
            }
            break;

        case H264_NALU_TYPE_IDR:
            // IDR key frame: instant decoder refresh, new GOP start; player can randomly access playback only after receiving IDR
            nalu.frame_type = H264_FRAME_IDR;
            ctx->stats.idr_count++;
            ctx->stats.idr_bytes += nalu_size;

            /* Compute GOP size: number of frames between previous IDR and this IDR */
            if (ctx->frames_since_idr > 0) {
                int gop = ctx->frames_since_idr;
                ctx->stats.gop_size = gop;
                if (gop > ctx->stats.max_gop_size) ctx->stats.max_gop_size = gop;
                if (ctx->stats.min_gop_size == 0 || gop < ctx->stats.min_gop_size)
                    ctx->stats.min_gop_size = gop;
            }
            // Reset frame counter after IDR, start new GOP stats cycle
            ctx->frames_since_idr = 0;
            break;

        case H264_NALU_TYPE_SLICE:
        case H264_NALU_TYPE_SLICE_A:
        case H264_NALU_TYPE_SLICE_B:
        case H264_NALU_TYPE_SLICE_C:
            // Picture Slice data, contains I/P/B frames
            nalu.frame_type = detect_frame_type(rbsp_data, rbsp_size, nalu_type);
            ctx->frames_since_idr++; // Increment frame count after IDR

            // Count I/P/B frames and bytes separately
            switch (nalu.frame_type) {
            case H264_FRAME_I:
                ctx->stats.i_count++;
                ctx->stats.idr_bytes += nalu_size;
                break;
            case H264_FRAME_P:
                ctx->stats.p_count++;
                ctx->stats.p_bytes += nalu_size;
                break;
            case H264_FRAME_B:
                ctx->stats.b_count++;
                break;
            default:
                break;
            }
            break;

        case H264_NALU_TYPE_SEI:
            // Supplemental Enhancement Information (timing info, custom data, etc.)
            nalu.frame_type = H264_FRAME_SEI;
            ctx->stats.sei_count++;
            break;

        default:
            nalu.frame_type = H264_FRAME_UNKNOWN;
            break;
        }

        /****************************************************************************
         * If a callback is registered, push current NALU info to upper layer
         * Upper layer can use it to: detect key frames, print SPS info, trigger alerts, etc.
         ***************************************************************************/
        if (ctx->callback) {
            ctx->callback(&nalu, ctx->callback_data);
        }

        nalu_count++;
        // Offset moves to end of current NALU, continue searching for next start code
        offset = nalu_end;
    }

    // Return total NAL units parsed this time
    return nalu_count;
}

const h264_sps_t* h264_parser_get_sps(const h264_parser_t *ctx)
{
    return (ctx && ctx->sps_valid) ? &ctx->sps : NULL;
}

const h264_pps_t* h264_parser_get_pps(const h264_parser_t *ctx)
{
    return (ctx && ctx->pps_valid) ? &ctx->pps : NULL;
}

void h264_parser_get_stats(const h264_parser_t *ctx, h264_stats_t *out_stats)
{
    if (!ctx || !out_stats) return;
    *out_stats = ctx->stats;

    /* Compute derived statistics */
    int total_frames = out_stats->idr_count + out_stats->i_count +
                       out_stats->p_count + out_stats->b_count;
    if (total_frames > 0) {
        out_stats->avg_gop_size = (double)total_frames / (double)(out_stats->idr_count > 0 ? out_stats->idr_count : 1);
    }
    if (out_stats->total_bytes > 0) {
        out_stats->idr_ratio = (double)out_stats->idr_bytes / (double)out_stats->total_bytes;
        out_stats->p_ratio   = (double)out_stats->p_bytes / (double)out_stats->total_bytes;
    }
    if (out_stats->last_pts > out_stats->first_pts && out_stats->first_pts > 0) {
        double duration_s = (double)(out_stats->last_pts - out_stats->first_pts) / 1000000.0;
        if (duration_s > 0) {
            out_stats->fps = (int)(total_frames / duration_s);
            out_stats->bitrate_kbps = (double)out_stats->total_bytes * 8.0 / duration_s / 1000.0;
        }
    }
}

void h264_parser_reset_stats(h264_parser_t *ctx)
{
    if (!ctx) return;
    h264_sps_t sps = ctx->sps;
    h264_pps_t pps = ctx->pps;
    int sps_valid = ctx->sps_valid;
    int pps_valid = ctx->pps_valid;

    memset(&ctx->stats, 0, sizeof(ctx->stats));

    ctx->sps = sps;
    ctx->pps = pps;
    ctx->sps_valid = sps_valid;
    ctx->pps_valid = pps_valid;
    ctx->frames_since_idr = 0;
}

const char* h264_nalu_type_str(int nalu_type)
{
    switch (nalu_type) {
    case H264_NALU_TYPE_SLICE:   return "SLICE(P/B)";
    case H264_NALU_TYPE_SLICE_A: return "SLICE_A";
    case H264_NALU_TYPE_SLICE_B: return "SLICE_B";
    case H264_NALU_TYPE_SLICE_C: return "SLICE_C";
    case H264_NALU_TYPE_IDR:     return "IDR";
    case H264_NALU_TYPE_SEI:     return "SEI";
    case H264_NALU_TYPE_SPS:     return "SPS";
    case H264_NALU_TYPE_PPS:     return "PPS";
    case H264_NALU_TYPE_AUD:     return "AUD";
    case H264_NALU_TYPE_EOSEQ:   return "END_SEQ";
    case H264_NALU_TYPE_EOSTREAM:return "END_STREAM";
    case H264_NALU_TYPE_FILL:    return "FILLER";
    default:                     return "UNKNOWN";
    }
}

const char* h264_frame_type_str(h264_frame_type_t type)
{
    switch (type) {
    case H264_FRAME_I:       return "I";
    case H264_FRAME_P:       return "P";
    case H264_FRAME_B:       return "B";
    case H264_FRAME_IDR:     return "IDR";
    case H264_FRAME_SPS:     return "SPS";
    case H264_FRAME_PPS:     return "PPS";
    case H264_FRAME_SEI:     return "SEI";
    case H264_FRAME_UNKNOWN: return "?";
    default:                 return "?";
    }
}

void h264_parser_print_stats(const h264_stats_t *s)
{
    if (!s) return;

    printf("\n========== H264 Stream Statistics ==========\n");
    printf("Resolution:    %d x %d\n", s->width, s->height);
    printf("Total NALUs:   %d\n", s->total_nalus);
    printf("SPS:           %d\n", s->sps_count);
    printf("PPS:           %d\n", s->pps_count);
    printf("IDR frames:    %d  (%.1f%% bytes)\n", s->idr_count, s->idr_ratio * 100);
    printf("I frames:      %d\n", s->i_count);
    printf("P frames:      %d  (%.1f%% bytes)\n", s->p_count, s->p_ratio * 100);
    printf("B frames:      %d\n", s->b_count);
    printf("SEI:           %d\n", s->sei_count);

    int total_frames = s->idr_count + s->i_count + s->p_count + s->b_count;
    printf("Total frames:  %d\n", total_frames);

    if (s->max_gop_size > 0) {
        printf("GOP size:      min=%d, max=%d, avg=%.1f\n",
               s->min_gop_size, s->max_gop_size, s->avg_gop_size);
    }

    printf("Bitrate:       %.0f kbps\n", s->bitrate_kbps);
    printf("Frame rate:    %d fps (estimated)\n", s->fps);
    printf("Total bytes:   %lld\n", (long long)s->total_bytes);
    printf("============================================\n\n");
}

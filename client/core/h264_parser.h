#ifndef H264_PARSER_H
#define H264_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file h264_parser.h
 * @brief H264 Annex-B bitstream parser
 */

#define H264_NALU_TYPE_SLICE        1   /**< Coded slice of non-IDR picture (P/B frame) */
#define H264_NALU_TYPE_SLICE_A      2   /**< Coded slice A (partitioned) */
#define H264_NALU_TYPE_SLICE_B      3   /**< Coded slice B (partitioned) */
#define H264_NALU_TYPE_SLICE_C      4   /**< Coded slice C (partitioned) */
#define H264_NALU_TYPE_IDR          5   /**< Coded slice of IDR picture (key frame, GOP start) */
#define H264_NALU_TYPE_SEI          6   /**< Supplemental Enhancement Information (SEI) */
#define H264_NALU_TYPE_SPS          7   /**< Sequence Parameter Set (SPS) */
#define H264_NALU_TYPE_PPS          8   /**< Picture Parameter Set (PPS) */
#define H264_NALU_TYPE_AUD          9   /**< Access Unit Delimiter */
#define H264_NALU_TYPE_EOSEQ       10   /**< End of sequence */
#define H264_NALU_TYPE_EOSTREAM    11   /**< End of stream */
#define H264_NALU_TYPE_FILL        12   /**< Filler data */

typedef enum {
    H264_FRAME_UNKNOWN = 0,
    H264_FRAME_I       = 1,    /**< I-frame: intra-coded, does not depend on other frames */
    H264_FRAME_P       = 2,    /**< P-frame: forward prediction, depends on preceding frames */
    H264_FRAME_B       = 3,    /**< B-frame: bidirectional prediction, depends on preceding and following frames */
    H264_FRAME_IDR     = 4,    /**< IDR frame: instant decoder refresh, GOP boundary, clears reference frames */
    H264_FRAME_SPS     = 5,    /**< SPS parameter set (non-picture frame) */
    H264_FRAME_PPS     = 6,    /**< PPS parameter set (non-picture frame) */
    H264_FRAME_SEI     = 7,    /**< SEI supplemental information (non-picture frame) */
} h264_frame_type_t;

typedef struct {
    int     profile_idc;        /**< Profile: 66=Baseline, 77=Main, 100=High */
    int     level_idc;          /**< Level: 30=Level 3.0, 31=Level 3.1, etc. */
    int     seq_parameter_set_id;
    int     log2_max_frame_num_minus4;
    int     pic_order_cnt_type;
    int     log2_max_pic_order_cnt_lsb_minus4;
    int     max_num_ref_frames; /**< Maximum number of reference frames */
    int     gaps_in_frame_num_value_allowed_flag;
    int     pic_width_in_mbs_minus1;   /**< Picture width (macroblocks - 1) */
    int     pic_height_in_map_units_minus1; /**< Picture height (macroblocks - 1) */
    int     frame_mbs_only_flag;
    int     direct_8x8_inference_flag;
    int     frame_cropping_flag;
    int     frame_crop_left_offset;
    int     frame_crop_right_offset;
    int     frame_crop_top_offset;
    int     frame_crop_bottom_offset;
    int     vui_parameters_present_flag;

    /* Computed actual resolution */
    int     width;              /**< Decoded picture width (pixels) */
    int     height;             /**< Decoded picture height (pixels) */
} h264_sps_t;

typedef struct {
    int     pic_parameter_set_id;
    int     seq_parameter_set_id;
    int     entropy_coding_mode_flag; /**< 0=CAVLC, 1=CABAC */
    int     bottom_field_pic_order_in_frame_present_flag;
    int     num_slice_groups_minus1;
    int     num_ref_idx_l0_default_active_minus1;
    int     num_ref_idx_l1_default_active_minus1;
    int     weighted_pred_flag;
    int     weighted_bipred_idc;
    int     pic_init_qp_minus26;      /**< Initial QP value (related to quantization) */
    int     pic_init_qs_minus26;
    int     chroma_qp_index_offset;
    int     deblocking_filter_control_present_flag;
    int     constrained_intra_pred_flag;
    int     redundant_pic_cnt_present_flag;
} h264_pps_t;

typedef struct {
    int             nalu_type;      /**< NALU type (H264_NALU_TYPE_*) */
    int             nal_ref_idc;    /**< Reference priority (0-3, 3=highest) */
    h264_frame_type_t frame_type;   /**< Frame type */
    const uint8_t  *data;           /**< NALU data pointer (without start code) */
    int             size;           /**< NALU data length (without start code) */
    int64_t         pts;            /**< Time stamp (filled by caller) */
} h264_nalu_t;

typedef struct {
    int64_t total_bytes;            /**< Total bitstream bytes */
    int     total_nalus;            /**< Total NALU count */
    int     sps_count;              /**< SPS count */
    int     pps_count;              /**< PPS count */
    int     idr_count;              /**< IDR frame count */
    int     i_count;                /**< I-frame count */
    int     p_count;                /**< P-frame count */
    int     b_count;                /**< B-frame count */
    int     sei_count;              /**< SEI count */
    int     gop_size;               /**< Current GOP size (frames between two IDR frames) */
    int     max_gop_size;           /**< Maximum GOP size */
    int     min_gop_size;           /**< Minimum GOP size */
    int64_t idr_bytes;              /**< Total IDR frame bytes */
    int64_t p_bytes;                /**< Total P-frame bytes */
    double  avg_gop_size;           /**< Average GOP size */
    double  idr_ratio;              /**< IDR frame ratio (by bytes) */
    double  p_ratio;                /**< P-frame ratio (by bytes) */
    int     width;                  /**< Picture width (parsed from SPS) */
    int     height;                 /**< Picture height (parsed from SPS) */
    int     fps;                    /**< Frame rate estimate */
    double  bitrate_kbps;           /**< Bitrate estimate (kbps) */
    int64_t first_pts;              /**< First frame PTS */
    int64_t last_pts;               /**< Last frame PTS */
} h264_stats_t;

typedef struct h264_parser_ctx h264_parser_t;

/**
 * @brief NALU callback function type
 */
typedef void (*h264_nalu_cb)(const h264_nalu_t *nalu, void *user_data);

/** @brief Create H264 parser */
h264_parser_t* h264_parser_create(void);

/** @brief Destroy H264 parser */
void h264_parser_destroy(h264_parser_t **ctx);

/** @brief Set NALU callback (called per NALU) */
void h264_parser_set_callback(h264_parser_t *ctx, h264_nalu_cb cb, void *user_data);

/**
 * @brief Feed H264 Annex-B data, parse NALU by NALU
 * @return number of parsed NALUs, <0=error
 */
int h264_parser_feed(h264_parser_t *ctx, const uint8_t *data, int size);

/** @brief Get most recent SPS (internal buffer), NULL if none */
const h264_sps_t* h264_parser_get_sps(const h264_parser_t *ctx);

/** @brief Get most recent PPS (internal buffer), NULL if none */
const h264_pps_t* h264_parser_get_pps(const h264_parser_t *ctx);

/** @brief Get bitstream statistics */
void h264_parser_get_stats(const h264_parser_t *ctx, h264_stats_t *out_stats);

/** @brief Reset stats counters (keeps SPS/PPS) */
void h264_parser_reset_stats(h264_parser_t *ctx);

/** @brief NALU type -> string */
const char* h264_nalu_type_str(int nalu_type);

/** @brief Frame type -> string */
const char* h264_frame_type_str(h264_frame_type_t type);

/** @brief Print statistics summary */
void h264_parser_print_stats(const h264_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* H264_PARSER_H */

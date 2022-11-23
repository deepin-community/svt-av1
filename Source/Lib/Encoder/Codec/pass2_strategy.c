/*
 * Copyright (c) 2019, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
 */
/*!\defgroup gf_group_algo Golden Frame Group
  * \ingroup high_level_algo
  * Algorithms regarding determining the length of GF groups and defining GF
  * group structures.
  * @{
  */
/*! @} - end defgroup gf_group_algo */
#include <stdint.h>

#include "EbDefinitions.h"
#include "level.h"
#include "pass2_strategy.h"
#include "EbRateControlProcess.h"
#include "firstpass.h"
#include "EbSequenceControlSet.h"
#include "EbEntropyCoding.h"
//#define INT_MAX 0x7fffffff

#define DEFAULT_KF_BOOST 2300
#define DEFAULT_GF_BOOST 2000
static void init_gf_stats(GF_GROUP_STATS *gf_stats);

// Calculate an active area of the image that discounts formatting
// bars and partially discounts other 0 energy areas.
#define MIN_ACTIVE_AREA 0.5
#define MAX_ACTIVE_AREA 1.0
static double calculate_active_area(const FrameInfo       *frame_info,
                                    const FIRSTPASS_STATS *this_frame) {
    const double active_pct = 1.0 -
        ((this_frame->intra_skip_pct / 2) +
         ((this_frame->inactive_zone_rows * 2) / (double)frame_info->mb_rows));
    return fclamp(active_pct, MIN_ACTIVE_AREA, MAX_ACTIVE_AREA);
}

// Calculate a modified Error used in distributing bits between easier and
// harder frames.
#define ACT_AREA_CORRECTION 0.5
static double calculate_modified_err(const FrameInfo *frame_info, const TWO_PASS *twopass,
                                     const TwoPassCfg      *two_pass_cfg,
                                     const FIRSTPASS_STATS *this_frame) {
    const FIRSTPASS_STATS *const stats = twopass->stats_buf_ctx->total_stats;
    if (stats == NULL) {
        return 0;
    }
    if (twopass->passes == 3)
        return (double)this_frame->stat_struct.total_num_bits;
    const double av_weight      = stats->weight / stats->count;
    const double av_err         = (stats->coded_error * av_weight) / stats->count;
    double       modified_error = av_err *
        pow(this_frame->coded_error * this_frame->weight / DOUBLE_DIVIDE_CHECK(av_err),
            two_pass_cfg->vbrbias / 100.0);

    // Correction for active area. Frames with a reduced active area
    // (eg due to formatting bars) have a higher error per mb for the
    // remaining active MBs. The correction here assumes that coding
    // 0.5N blocks of complexity 2X is a little easier than coding N
    // blocks of complexity X.
    modified_error *= pow(calculate_active_area(frame_info, this_frame), ACT_AREA_CORRECTION);

    return fclamp(modified_error, twopass->modified_error_min, twopass->modified_error_max);
}

// Resets the first pass file to the given position using a relative seek from
// the current position.
static void reset_fpf_position(TWO_PASS *p, const FIRSTPASS_STATS *position) {
    p->stats_in = position;
}

static int input_stats(TWO_PASS *p, FIRSTPASS_STATS *fps) {
    if (p->stats_in >= p->stats_buf_ctx->stats_in_end)
        return EOF;

    *fps = *p->stats_in;
    ++p->stats_in;
    return 1;
}

// Read frame stats at an offset from the current position.
static const FIRSTPASS_STATS *read_frame_stats(const TWO_PASS *p, int offset) {
    if ((offset >= 0 && p->stats_in + offset >= p->stats_buf_ctx->stats_in_end) ||
        (offset < 0 && p->stats_in + offset < p->stats_buf_ctx->stats_in_start)) {
        return NULL;
    }

    return &p->stats_in[offset];
}

static void subtract_stats(FIRSTPASS_STATS *section, const FIRSTPASS_STATS *frame) {
    section->frame -= frame->frame;
    section->weight -= frame->weight;
    section->intra_error -= frame->intra_error;
    section->coded_error -= frame->coded_error;
    section->sr_coded_error -= frame->sr_coded_error;
    section->pcnt_inter -= frame->pcnt_inter;
    section->pcnt_motion -= frame->pcnt_motion;
    section->pcnt_second_ref -= frame->pcnt_second_ref;
    section->pcnt_neutral -= frame->pcnt_neutral;
    section->intra_skip_pct -= frame->intra_skip_pct;
    section->inactive_zone_rows -= frame->inactive_zone_rows;
    section->inactive_zone_cols -= frame->inactive_zone_cols;
    section->mvr_abs -= frame->mvr_abs;
    section->mvc_abs -= frame->mvc_abs;
    section->mv_in_out_count -= frame->mv_in_out_count;
    section->count -= frame->count;
    section->duration -= frame->duration;
}

// This function returns the maximum target rate per frame.
static int frame_max_bits(const RATE_CONTROL *rc, const EncodeContext *encode_context_ptr) {
    int64_t max_bits = ((int64_t)rc->avg_frame_bandwidth *
                        (int64_t)encode_context_ptr->two_pass_cfg.vbrmax_section) /
        100;
    if (max_bits < 0)
        max_bits = 0;
    else if (max_bits > rc->max_frame_bandwidth)
        max_bits = rc->max_frame_bandwidth;

    return (int)max_bits;
}

static const double q_pow_term[(QINDEX_RANGE >> 5) + 1] = {
    0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.95, 0.95, 0.95};
#define ERR_DIVISOR 96.0
static double calc_correction_factor(double err_per_mb, int q) {
    const double error_term = err_per_mb / ERR_DIVISOR;
    const int    index      = q >> 5;
    // Adjustment to power term based on qindex
    const double power_term = q_pow_term[index] +
        (((q_pow_term[index + 1] - q_pow_term[index]) * (q % 32)) / 32.0);
    assert(error_term >= 0.0);
    return fclamp(pow(error_term, power_term), 0.05, 5.0);
}

static int qbpm_enumerator(int rate_err_tol) {
    return 1250000 + ((300000 * AOMMIN(75, AOMMAX(rate_err_tol - 25, 0))) / 75);
}

// Similar to find_qindex_by_rate() function in ratectrl.c, but includes
// calculation of a correction_factor.
static int find_qindex_by_rate_with_correction(int desired_bits_per_mb, aom_bit_depth_t bit_depth,
                                               double error_per_mb, double group_weight_factor,
                                               int rate_err_tol, int best_qindex,
                                               int worst_qindex) {
    assert(best_qindex <= worst_qindex);
    int low  = best_qindex;
    int high = worst_qindex;

    while (low < high) {
        const int    mid             = (low + high) >> 1;
        const double mid_factor      = calc_correction_factor(error_per_mb, mid);
        const double q               = svt_av1_convert_qindex_to_q(mid, bit_depth);
        const int    enumerator      = qbpm_enumerator(rate_err_tol);
        const int    mid_bits_per_mb = (int)((enumerator * mid_factor * group_weight_factor) / q);

        if (mid_bits_per_mb > desired_bits_per_mb) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

// Bits Per MB at different Q (Multiplied by 512)
#define BPER_MB_NORMBITS 9
/*!\brief Choose a target maximum Q for a group of frames
 *
 * \ingroup rate_control
 *
 * This function is used to estimate a suitable maximum Q for a
 * group of frames. Inititally it is called to get a crude estimate
 * for the whole clip. It is then called for each ARF/GF group to get
 * a revised estimate for that group.
 *
 * \param[in]    cpi                 Top-level encoder structure
 * \param[in]    av_frame_err        The average per frame coded error score
 *                                   for frames making up this section/group.
 * \param[in]    inactive_zone       Used to mask off /ignore part of the
 *                                   frame. The most common use case is where
 *                                   a wide format video (e.g. 16:9) is
 *                                   letter-boxed into a more square format.
 *                                   Here we want to ignore the bands at the
 *                                   top and bottom.
 * \param[in]    av_target_bandwidth The target bits per frame
 * \param[in]    group_weight_factor A correction factor allowing the algorithm
 *                                   to correct for errors over time.
 *
 * \return The maximum Q for frames in the group.
 */
static int get_twopass_worst_quality(PictureParentControlSet *pcs_ptr, const double section_err,
                                     double inactive_zone, int section_target_bandwidth,
                                     double group_weight_factor) {
    SequenceControlSet         *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext              *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const         rc                 = &encode_context_ptr->rc;
    const RateControlCfg *const rc_cfg             = &encode_context_ptr->rc_cfg;
    uint32_t                    mb_cols;
    uint32_t                    mb_rows;
    if (scs_ptr->mid_pass_ctrls.ds) {
        mb_cols = 2 * (scs_ptr->max_input_luma_width + 16 - 1) / 16;
        mb_rows = 2 * (scs_ptr->max_input_luma_height + 16 - 1) / 16;
    } else {
        mb_cols = (scs_ptr->max_input_luma_width + 16 - 1) / 16;
        mb_rows = (scs_ptr->max_input_luma_height + 16 - 1) / 16;
    }
    inactive_zone = fclamp(inactive_zone, 0.0, 1.0);

    if (section_target_bandwidth <= 0) {
        return rc->worst_quality; // Highest value allowed
    } else {
        const int num_mbs = mb_cols * mb_rows;
        //(oxcf->resize_cfg.resize_mode != RESIZE_NONE)
        //    ? cpi->initial_mbs
        //    : cpi->common.mi_params.MBs;
        const int    active_mbs    = AOMMAX(1, num_mbs - (int)(num_mbs * inactive_zone));
        const double av_err_per_mb = section_err / active_mbs;
        const int    target_norm_bits_per_mb =
            (int)(((uint64_t)section_target_bandwidth << BPER_MB_NORMBITS) / active_mbs);

        int rate_err_tol = AOMMIN(rc_cfg->under_shoot_pct, rc_cfg->over_shoot_pct);

        // Try and pick a max Q that will be high enough to encode the
        // content at the given rate.
        int q = find_qindex_by_rate_with_correction(target_norm_bits_per_mb,
                                                    scs_ptr->encoder_bit_depth,
                                                    av_err_per_mb,
                                                    group_weight_factor,
                                                    rate_err_tol,
                                                    rc->best_quality,
                                                    rc->worst_quality);
        return q;
    }
}

#define SR_DIFF_PART 0.0015
#define MOTION_AMP_PART 0.003
#define INTRA_PART 0.005
#define DEFAULT_DECAY_LIMIT 0.75
#define LOW_SR_DIFF_TRHESH 0.1
#define SR_DIFF_MAX 128.0
#define NCOUNT_FRAME_II_THRESH 5.0

static double get_sr_decay_rate(const FrameInfo *frame_info, const FIRSTPASS_STATS *frame) {
    const int    num_mbs  = frame_info->num_mbs;
    double       sr_diff  = (frame->sr_coded_error - frame->coded_error) / num_mbs;
    double       sr_decay = 1.0;
    double       modified_pct_inter;
    double       modified_pcnt_intra;
    const double motion_amplitude_factor = frame->pcnt_motion *
        ((frame->mvc_abs + frame->mvr_abs) / 2);

    modified_pct_inter = frame->pcnt_inter;
    if ((frame->intra_error / DOUBLE_DIVIDE_CHECK(frame->coded_error)) <
        (double)NCOUNT_FRAME_II_THRESH) {
        modified_pct_inter = frame->pcnt_inter - frame->pcnt_neutral;
    }
    modified_pcnt_intra = 100 * (1.0 - modified_pct_inter);

    if ((sr_diff > LOW_SR_DIFF_TRHESH)) {
        sr_diff  = AOMMIN(sr_diff, SR_DIFF_MAX);
        sr_decay = 1.0 - (SR_DIFF_PART * sr_diff) - (MOTION_AMP_PART * motion_amplitude_factor) -
            (INTRA_PART * modified_pcnt_intra);
    }
    return AOMMAX(sr_decay, AOMMIN(DEFAULT_DECAY_LIMIT, modified_pct_inter));
}

// This function gives an estimate of how badly we believe the prediction
// quality is decaying from frame to frame.
static double get_zero_motion_factor(const FrameInfo *frame_info, const FIRSTPASS_STATS *frame) {
    const double zero_motion_pct = frame->pcnt_inter - frame->pcnt_motion;
    double       sr_decay        = get_sr_decay_rate(frame_info, frame);
    return AOMMIN(sr_decay, zero_motion_pct);
}

#define ZM_POWER_FACTOR 0.75

static double get_prediction_decay_rate(const FrameInfo       *frame_info,
                                        const FIRSTPASS_STATS *next_frame) {
    const double sr_decay_rate = get_sr_decay_rate(frame_info, next_frame);
    const double zero_motion_factor =
        (0.95 * pow((next_frame->pcnt_inter - next_frame->pcnt_motion), ZM_POWER_FACTOR));

    return AOMMAX(zero_motion_factor,
                  (sr_decay_rate + ((1.0 - sr_decay_rate) * zero_motion_factor)));
}
// This function detects a flash through the high relative pcnt_second_ref
// score in the frame following a flash frame. The offset passed in should
// reflect this.
static int detect_flash(const TWO_PASS *twopass, const int offset) {
    const FIRSTPASS_STATS *const next_frame = read_frame_stats(twopass, offset);

    // What we are looking for here is a situation where there is a
    // brief break in prediction (such as a flash) but subsequent frames
    // are reasonably well predicted by an earlier (pre flash) frame.
    // The recovery after a flash is indicated by a high pcnt_second_ref
    // compared to pcnt_inter.
    return next_frame != NULL && next_frame->pcnt_second_ref > next_frame->pcnt_inter &&
        next_frame->pcnt_second_ref >= 0.5;
}

// Update the motion related elements to the GF arf boost calculation.
static void accumulate_frame_motion_stats(const FIRSTPASS_STATS *stats, GF_GROUP_STATS *gf_stats) {
    const double pct = stats->pcnt_motion;

    // Accumulate Motion In/Out of frame stats.
    gf_stats->this_frame_mv_in_out = stats->mv_in_out_count * pct;
}

static void accumulate_this_frame_stats(const FIRSTPASS_STATS *stats, const double mod_frame_err,
                                        GF_GROUP_STATS *gf_stats) {
    gf_stats->gf_group_err += mod_frame_err;
    gf_stats->gf_group_raw_error += stats->coded_error;
    gf_stats->gf_group_skip_pct += stats->intra_skip_pct;
    gf_stats->gf_group_inactive_zone_rows += stats->inactive_zone_rows;
}

static void accumulate_next_frame_stats(const FIRSTPASS_STATS *stats, const FrameInfo *frame_info,
                                        const int flash_detected, const int frames_since_key,
                                        const int cur_idx, GF_GROUP_STATS *gf_stats) {
    accumulate_frame_motion_stats(stats, gf_stats);
    // sum up the metric values of current gf group

    // Accumulate the effect of prediction quality decay
    if (!flash_detected) {
        gf_stats->decay_accumulator *= get_prediction_decay_rate(frame_info, stats);

        // Monitor for static sections.
        if ((frames_since_key + cur_idx - 1) > 1) {
            gf_stats->zero_motion_accumulator = AOMMIN(gf_stats->zero_motion_accumulator,
                                                       get_zero_motion_factor(frame_info, stats));
        }
    }
}

#define BOOST_FACTOR 12.5
static double baseline_err_per_mb(const FrameInfo *frame_info) {
    unsigned int screen_area = frame_info->frame_height * frame_info->frame_width;

    // Use a different error per mb factor for calculating boost for
    //  different formats.
    if (screen_area <= 640 * 360) {
        return 500.0;
    } else {
        return 1000.0;
    }
}

static double calc_frame_boost(const RATE_CONTROL *rc, const FrameInfo *frame_info,
                               const FIRSTPASS_STATS *this_frame, double this_frame_mv_in_out,
                               double max_boost) {
    double       frame_boost;
    const double lq                 = svt_av1_convert_qindex_to_q(rc->avg_frame_qindex[INTER_FRAME],
                                                  frame_info->bit_depth);
    const double boost_q_correction = AOMMIN((0.5 + (lq * 0.015)), 1.5);
    const double active_area        = calculate_active_area(frame_info, this_frame);
    int          num_mbs            = frame_info->num_mbs;

    // Correct for any inactive region in the image
    num_mbs = (int)AOMMAX(1, num_mbs * active_area);

    // Underlying boost factor is based on inter error ratio.
    frame_boost = AOMMAX(baseline_err_per_mb(frame_info) * num_mbs,
                         this_frame->intra_error * active_area) /
        DOUBLE_DIVIDE_CHECK(this_frame->coded_error);
    frame_boost = frame_boost * BOOST_FACTOR * boost_q_correction;

    // Increase boost for frames where new data coming into frame (e.g. zoom out).
    // Slightly reduce boost if there is a net balance of motion out of the frame
    // (zoom in). The range for this_frame_mv_in_out is -1.0 to +1.0.
    if (this_frame_mv_in_out > 0.0)
        frame_boost += frame_boost * (this_frame_mv_in_out * 2.0);
    // In the extreme case the boost is halved.
    else
        frame_boost += frame_boost * (this_frame_mv_in_out / 2.0);

    return AOMMIN(frame_boost, max_boost * boost_q_correction);
}

static double calc_kf_frame_boost(const RATE_CONTROL *rc, const FrameInfo *frame_info,
                                  const FIRSTPASS_STATS *this_frame, double *sr_accumulator,
                                  double max_boost) {
    double       frame_boost;
    const double lq                 = svt_av1_convert_qindex_to_q(rc->avg_frame_qindex[INTER_FRAME],
                                                  frame_info->bit_depth);
    const double boost_q_correction = AOMMIN((0.50 + (lq * 0.015)), 2.00);
    const double active_area        = calculate_active_area(frame_info, this_frame);
    int          num_mbs            = frame_info->num_mbs;

    // Correct for any inactive region in the image
    num_mbs = (int)AOMMAX(1, num_mbs * active_area);

    // Underlying boost factor is based on inter error ratio.
    frame_boost = AOMMAX(baseline_err_per_mb(frame_info) * num_mbs,
                         this_frame->intra_error * active_area) /
        DOUBLE_DIVIDE_CHECK((this_frame->coded_error + *sr_accumulator) * active_area);

    // Update the accumulator for second ref error difference.
    // This is intended to give an indication of how much the coded error is
    // increasing over time.
    *sr_accumulator += AOMMAX(0.0, (this_frame->sr_coded_error - this_frame->coded_error));
    *sr_accumulator = AOMMAX(0.0, *sr_accumulator);

    // Q correction and scaling
    // The 40.0 value here is an experimentally derived baseline minimum.
    // This value is in line with the minimum per frame boost in the alt_ref
    // boost calculation.
    frame_boost = ((frame_boost + 40.0) * boost_q_correction);

    return AOMMIN(frame_boost, max_boost * boost_q_correction);
}

static int get_projected_gfu_boost(const RATE_CONTROL *rc, int gfu_boost, int frames_to_project,
                                   int num_stats_used_for_gfu_boost) {
    /*
   * If frames_to_project is equal to num_stats_used_for_gfu_boost,
   * it means that gfu_boost was calculated over frames_to_project to
   * begin with(ie; all stats required were available), hence return
   * the original boost.
   */
    if (num_stats_used_for_gfu_boost >= frames_to_project)
        return gfu_boost;

    double min_boost_factor = sqrt(rc->baseline_gf_interval);
    // Get the current tpl factor (number of frames = frames_to_project).
    double tpl_factor = svt_av1_get_gfu_boost_projection_factor(
        min_boost_factor, MAX_GFUBOOST_FACTOR, frames_to_project);
    // Get the tpl factor when number of frames = num_stats_used_for_prior_boost.
    double tpl_factor_num_stats = svt_av1_get_gfu_boost_projection_factor(
        min_boost_factor, MAX_GFUBOOST_FACTOR, num_stats_used_for_gfu_boost);
    int projected_gfu_boost = (int)rint((tpl_factor * gfu_boost) / tpl_factor_num_stats);
    return projected_gfu_boost;
}

#define GF_MAX_BOOST 90.0
#define MIN_DECAY_FACTOR 0.01
#define NORMAL_BOOST 100
static int av1_calc_arf_boost(const TWO_PASS *twopass, const RATE_CONTROL *rc,
                              FrameInfo *frame_info, int offset, int f_frames, int b_frames,
                              int *num_fpstats_used, int *num_fpstats_required) {
    int            i;
    GF_GROUP_STATS gf_stats;
    init_gf_stats(&gf_stats);
    double boost_score = (double)NORMAL_BOOST;
    int    arf_boost;
    int    flash_detected = 0;
    if (num_fpstats_used)
        *num_fpstats_used = 0;

    // Search forward from the proposed arf/next gf position.
    for (i = 0; i < f_frames; ++i) {
        const FIRSTPASS_STATS *this_frame = read_frame_stats(twopass, i + offset);
        if (this_frame == NULL)
            break;

        // Update the motion related elements to the boost calculation.
        accumulate_frame_motion_stats(this_frame, &gf_stats);

        // We want to discount the flash frame itself and the recovery
        // frame that follows as both will have poor scores.
        flash_detected = detect_flash(twopass, i + offset) || detect_flash(twopass, i + offset + 1);

        // Accumulate the effect of prediction quality decay.
        if (!flash_detected) {
            gf_stats.decay_accumulator *= get_prediction_decay_rate(frame_info, this_frame);
            gf_stats.decay_accumulator = gf_stats.decay_accumulator < MIN_DECAY_FACTOR
                ? MIN_DECAY_FACTOR
                : gf_stats.decay_accumulator;
        }

        boost_score += gf_stats.decay_accumulator *
            calc_frame_boost(
                           rc, frame_info, this_frame, gf_stats.this_frame_mv_in_out, GF_MAX_BOOST);
        if (num_fpstats_used)
            (*num_fpstats_used)++;
    }

    arf_boost = (int)boost_score;

    // Reset for backward looking loop.
    boost_score = 0.0;
    init_gf_stats(&gf_stats);
    // Search backward towards last gf position.
    for (i = -1; i >= -b_frames; --i) {
        const FIRSTPASS_STATS *this_frame = read_frame_stats(twopass, i + offset);
        if (this_frame == NULL)
            break;

        // Update the motion related elements to the boost calculation.
        accumulate_frame_motion_stats(this_frame, &gf_stats);

        // We want to discount the the flash frame itself and the recovery
        // frame that follows as both will have poor scores.
        flash_detected = detect_flash(twopass, i + offset) || detect_flash(twopass, i + offset + 1);

        // Cumulative effect of prediction quality decay.
        if (!flash_detected) {
            gf_stats.decay_accumulator *= get_prediction_decay_rate(frame_info, this_frame);
            gf_stats.decay_accumulator = gf_stats.decay_accumulator < MIN_DECAY_FACTOR
                ? MIN_DECAY_FACTOR
                : gf_stats.decay_accumulator;
        }

        boost_score += gf_stats.decay_accumulator *
            calc_frame_boost(
                           rc, frame_info, this_frame, gf_stats.this_frame_mv_in_out, GF_MAX_BOOST);
        if (num_fpstats_used)
            (*num_fpstats_used)++;
    }
    arf_boost += (int)boost_score;

    if (num_fpstats_required) {
        *num_fpstats_required = f_frames + b_frames;
        if (num_fpstats_used) {
            arf_boost = get_projected_gfu_boost(
                rc, arf_boost, *num_fpstats_required, *num_fpstats_used);
        }
    }

    if (arf_boost < ((b_frames + f_frames) * 50))
        arf_boost = ((b_frames + f_frames) * 50);

    return arf_boost;
}
// Calculate the total bits to allocate in this GF/ARF group.
/*!\brief Calculates the bit target for this GF/ARF group
 *
 * \ingroup rate_control
 *
 * Calculates the total bits to allocate in this GF/ARF group.
 *
 * \param[in]    cpi              Top-level encoder structure
 * \param[in]    gf_group_err     Cumulative coded error score for the
 *                                frames making up this group.
 *
 * \return The target total number of bits for this GF/ARF group.
 */
static int64_t calculate_total_gf_group_bits(PictureParentControlSet *pcs_ptr,
                                             double                   gf_group_err) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    TWO_PASS *const     twopass            = &scs_ptr->twopass;
    const int           max_bits           = frame_max_bits(rc, encode_context_ptr);
    int64_t             total_group_bits;
    // Calculate the bits to be allocated to the group as a whole.
    if ((twopass->kf_group_bits > 0) && (twopass->kf_group_error_left > 0)) {
        int64_t kf_group_bits;
        if (scs_ptr->lap_rc &&
            (scs_ptr->lad_mg + 1) * (1 << scs_ptr->static_config.hierarchical_levels) <
                scs_ptr->static_config.intra_period_length)
            kf_group_bits = (int64_t)twopass->kf_group_bits *
                MIN(pcs_ptr->frames_in_sw, rc->frames_to_key) / rc->frames_to_key;
        else
            kf_group_bits = twopass->kf_group_bits;
        total_group_bits = (int64_t)(kf_group_bits * (gf_group_err / twopass->kf_group_error_left));
    } else {
        total_group_bits = 0;
    }

    // Clamp odd edge cases.
    total_group_bits = (total_group_bits < 0)         ? 0
        : (total_group_bits > twopass->kf_group_bits) ? twopass->kf_group_bits
                                                      : total_group_bits;

    // Clip based on user supplied data rate variability limit.
    if (total_group_bits > (int64_t)max_bits * rc->baseline_gf_interval)
        total_group_bits = (int64_t)max_bits * rc->baseline_gf_interval;
    twopass->kf_group_bits = AOMMAX(twopass->kf_group_bits - total_group_bits, 0);
    return total_group_bits;
}
// Calculate the number of bits to assign to boosted frames in a group.
static int calculate_boost_bits(int frame_count, int boost, int64_t total_group_bits) {
    int allocation_chunks;

    // return 0 for invalid inputs (could arise e.g. through rounding errors)
    if (!boost || (total_group_bits <= 0))
        return 0;

    if (frame_count <= 0)
        return (int)(AOMMIN(total_group_bits, INT_MAX));

    allocation_chunks = (frame_count * 100) + boost;

    // Prevent overflow.
    if (boost > 1023) {
        int divisor = boost >> 10;
        boost /= divisor;
        allocation_chunks /= divisor;
    }

    // Calculate the number of extra bits for use in the boosted frame or frames.
    return AOMMAX((int)(((int64_t)boost * total_group_bits) / allocation_chunks), 0);
}
/****************************************************************************************************
* Allocate the rate per frame based on the rate allocation of previous pass with same prediction
* structure and using cross multiplying
****************************************************************************************************/
static void av1_gop_bit_allocation_same_pred(PictureParentControlSet *pcs, int64_t gf_group_bits,
                                             GF_GROUP_STATS gf_stats) {
    // For key frames the frame target rate is already set
    int frame_index = (pcs->slice_type == I_SLICE) ? 1 : 0;
    for (int idx = frame_index; idx < pcs->gf_interval; ++idx) {
        assert(gf_stats.gf_group_err != 0);
        pcs->gf_group[idx]->base_frame_target =
            (int)(gf_group_bits * pcs->gf_group[idx]->stat_struct.total_num_bits /
                  gf_stats.gf_group_err);
    }
}
// Allocate bits to each frame in a GF / ARF group
static double layer_fraction[MAX_ARF_LAYERS + 1] = {1.0, 0.80, 0.7, 0.60, 0.60, 1.0, 1.0};
static void   allocate_gf_group_bits(PictureParentControlSet *pcs, RATE_CONTROL *const rc,
                                     int64_t gf_group_bits, int gf_arf_bits, int gf_interval,
                                     int key_frame, int use_arf) {
    int64_t total_group_bits = gf_group_bits;
    int     base_frame_bits;
    int     layer_frames[MAX_ARF_LAYERS + 1] = {0};

    // Subtract the extra bits set aside for ARF frames from the Group Total
    if (use_arf || !key_frame)
        total_group_bits -= gf_arf_bits;

    if (rc->baseline_gf_interval)
        base_frame_bits = (int)(total_group_bits / rc->baseline_gf_interval);
    else
        base_frame_bits = (int)1;

    // For key frames the frame target rate is already set
    int frame_index = key_frame ? 1 : 0;

    // Check the number of frames in each layer in case we have a
    // non standard group length.
    int max_arf_layer = pcs->hierarchical_levels;
    for (int idx = frame_index; idx < pcs->gf_interval; ++idx) {
        if ((pcs->gf_group[idx]->update_type == ARF_UPDATE) ||
            (pcs->gf_group[idx]->update_type == INTNL_ARF_UPDATE)) {
            layer_frames[pcs->gf_group[idx]->layer_depth]++;
        }
    }
    if (rc->baseline_gf_interval < (gf_interval >> 1)) {
        for (int idx = frame_index; idx < pcs->gf_interval; ++idx) {
            if (pcs->gf_group[idx]->update_type == ARF_UPDATE) {
                layer_frames[pcs->gf_group[idx]->layer_depth] += 1;
            }
            if (pcs->gf_group[idx]->update_type == INTNL_ARF_UPDATE) {
                layer_frames[pcs->gf_group[idx]->layer_depth] += 2;
            }
        }
    }
    // Allocate extra bits to each ARF layer
    int layer_extra_bits[MAX_ARF_LAYERS + 1] = {0};
    for (int i = 1; i <= max_arf_layer; ++i) {
        if (layer_frames[i]) { // to make sure there is a picture with the depth
            double fraction     = (i == max_arf_layer) ? 1.0 : layer_fraction[i];
            layer_extra_bits[i] = (int)((gf_arf_bits * fraction) / AOMMAX(1, layer_frames[i]));
            gf_arf_bits -= (int)(gf_arf_bits * fraction);
        }
    }

    // Now combine ARF layer and baseline bits to give total bits for each frame.
    int arf_extra_bits;
    for (int idx = frame_index; idx < pcs->gf_interval; ++idx) {
        switch (pcs->gf_group[idx]->update_type) {
        case ARF_UPDATE:
        case INTNL_ARF_UPDATE:
            arf_extra_bits = layer_extra_bits[pcs->gf_group[idx]->layer_depth];
            pcs->gf_group[idx]->base_frame_target = base_frame_bits + arf_extra_bits;
            break;
        case INTNL_OVERLAY_UPDATE:
        case OVERLAY_UPDATE: pcs->gf_group[idx]->base_frame_target = 0; break;
        default: pcs->gf_group[idx]->base_frame_target = base_frame_bits; break;
        }
    }
}

#define RC_FACTOR_MIN_GOP_CONST 0.5
#define RC_FACTOR_MIN_1P_VBR 1
#define RC_FACTOR_MIN 0.75
#define RC_FACTOR_MAX 2
static INLINE void set_baseline_gf_interval(PictureParentControlSet *pcs_ptr, int arf_position) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    if (frame_is_intra_only(pcs_ptr) && pcs_ptr->idr_flag)
        rc->baseline_gf_interval = MAX(arf_position - 1, 1);
    else
        rc->baseline_gf_interval = pcs_ptr->gf_interval;
}

// initialize GF_GROUP_STATS
static void init_gf_stats(GF_GROUP_STATS *gf_stats) {
    gf_stats->gf_group_err                  = 0.0;
    gf_stats->gf_stat_struct.poc            = 0;
    gf_stats->gf_stat_struct.total_num_bits = 1;
    gf_stats->gf_stat_struct.qindex         = 172;
    gf_stats->gf_stat_struct.worst_qindex   = 172;
    gf_stats->gf_group_raw_error            = 0.0;
    gf_stats->gf_group_skip_pct             = 0.0;
    gf_stats->gf_group_inactive_zone_rows   = 0.0;

    gf_stats->decay_accumulator       = 1.0;
    gf_stats->zero_motion_accumulator = 1.0;
    gf_stats->this_frame_mv_in_out    = 0.0;
}

#define FRAME_OVERHEAD_BITS 200
static int av1_rc_clamp_iframe_target_size(PictureParentControlSet *pcs_ptr, int target) {
    SequenceControlSet         *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext              *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const         rc                 = &encode_context_ptr->rc;
    const RateControlCfg *const rc_cfg             = &encode_context_ptr->rc_cfg;
    if (rc_cfg->max_intra_bitrate_pct) {
        const int max_rate = rc->avg_frame_bandwidth * rc_cfg->max_intra_bitrate_pct / 100;
        target             = AOMMIN(target, max_rate);
    }
    if (target > rc->max_frame_bandwidth)
        target = rc->max_frame_bandwidth;
    return target;
}

static int av1_calc_pframe_target_size_one_pass_cbr(PictureParentControlSet *pcs_ptr,
                                                    FRAME_UPDATE_TYPE        frame_update_type) {
    SequenceControlSet         *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext              *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const         rc                 = &encode_context_ptr->rc;
    const RateControlCfg *const rc_cfg             = &encode_context_ptr->rc_cfg;
    const int64_t               diff               = rc->optimal_buffer_level - rc->buffer_level;
    const int64_t               one_pct_bits       = 1 + rc->optimal_buffer_level / 100;
    int min_frame_target = AOMMAX(rc->avg_frame_bandwidth >> 4, FRAME_OVERHEAD_BITS);
    int target;

    if (rc_cfg->gf_cbr_boost_pct) {
        const int af_ratio_pct = rc_cfg->gf_cbr_boost_pct + 100;
        if (frame_update_type == GF_UPDATE || frame_update_type == OVERLAY_UPDATE) {
            target = (rc->avg_frame_bandwidth * rc->baseline_gf_interval * af_ratio_pct) /
                (rc->baseline_gf_interval * 100 + af_ratio_pct - 100);
        } else {
            target = (rc->avg_frame_bandwidth * rc->baseline_gf_interval * 100) /
                (rc->baseline_gf_interval * 100 + af_ratio_pct - 100);
        }
    } else {
        target = rc->avg_frame_bandwidth;
    }
    if (diff > 0) {
        // Lower the target bandwidth for this frame.
        const int pct_low = (int)AOMMIN(diff / one_pct_bits, rc_cfg->under_shoot_pct);
        target -= (target * pct_low) / 200;
    } else if (diff < 0) {
        // Increase the target bandwidth for this frame.
        const int pct_high = (int)AOMMIN(-diff / one_pct_bits, rc_cfg->over_shoot_pct);
        target += (target * pct_high) / 200;
    }
    if (rc_cfg->max_inter_bitrate_pct) {
        const int max_rate = rc->avg_frame_bandwidth * rc_cfg->max_inter_bitrate_pct / 100;
        target             = AOMMIN(target, max_rate);
    }
    return AOMMAX(min_frame_target, target);
}
// buffer level weights to calculate the target rate for Key frame
static int av1_calc_iframe_target_size_one_pass_cbr(PictureParentControlSet *pcs_ptr) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    int                 target;
    const int           w = 3;
    if (pcs_ptr->picture_number == 0) {
        target = ((rc->starting_buffer_level / 2) > INT_MAX)
            ? INT_MAX
            : (int)(rc->starting_buffer_level * w / 4);
    } else {
        int    kf_boost  = 32;
        double framerate = scs_ptr->double_frame_rate; //cpi->framerate;

        kf_boost = AOMMAX(kf_boost, (int)(2 * framerate - 16));
        if (rc->frames_since_key < framerate / 2) {
            kf_boost = (int)(kf_boost * rc->frames_since_key / (framerate / 2));
        }
        target = ((16 + kf_boost) * rc->avg_frame_bandwidth) >> 4;
    }
    return av1_rc_clamp_iframe_target_size(pcs_ptr, target);
}

static void av1_gop_bit_allocation(PictureParentControlSet *ppcs, RATE_CONTROL *const rc,
                                   int is_key_frame, int gf_interval, int use_arf,
                                   int64_t gf_group_bits);
int         frame_is_kf_gf_arf(PictureParentControlSet *ppcs_ptr);
#define MAX_GF_BOOST 5400
/***********************************************************************************
* calculate_gf_stats()
* calculate the gf group stat by looping over frames within the gf
************************************************************************************/
static void calculate_gf_stats(PictureParentControlSet *ppcs, GF_GROUP_STATS *gf_stats,
                               FIRSTPASS_STATS *this_frame, int *use_alt_ref) {
    SequenceControlSet          *scs        = ppcs->scs_ptr;
    RATE_CONTROL *const          rc         = &scs->encode_context_ptr->rc;
    TWO_PASS *const              twopass    = &scs->twopass;
    FrameInfo                   *frame_info = &scs->encode_context_ptr->frame_info;
    FIRSTPASS_STATS              next_frame;
    const FIRSTPASS_STATS *const start_pos = twopass->stats_in;

    init_gf_stats(gf_stats);

    // Load stats for the current frame.
    double mod_frame_err = calculate_modified_err(
        frame_info, twopass, &(scs->encode_context_ptr->two_pass_cfg), this_frame);

    // Note the error of the frame at the start of the group. This will be
    // the GF frame error if we code a normal gf.

    // If this is a key frame or the overlay from a previous arf then
    // the error score / cost of this frame has already been accounted for.
    // There is no overlay support for now
    if (frame_is_intra_only(ppcs)) {
        gf_stats->gf_group_err -= mod_frame_err;
        gf_stats->gf_group_raw_error -= this_frame->coded_error;
        gf_stats->gf_group_skip_pct -= this_frame->intra_skip_pct;
        gf_stats->gf_group_inactive_zone_rows -= this_frame->inactive_zone_rows;
    }
    gf_stats->gf_stat_struct = this_frame->stat_struct;
    int i                    = 0;
    while (i < ppcs->gf_interval) {
        int flash_detected;
        ++i;
        // Accumulate error score of frames in this gf group.
        mod_frame_err = calculate_modified_err(
            frame_info, twopass, &(scs->encode_context_ptr->two_pass_cfg), this_frame);
        // accumulate stats for this frame
        accumulate_this_frame_stats(this_frame, mod_frame_err, gf_stats);

        // read in the next frame
        if (EOF == input_stats(twopass, &next_frame))
            break;

        // Test for the case where there is a brief flash but the prediction
        // quality back to an earlier frame is then restored.
        flash_detected = detect_flash(twopass, 0);

        // accumulate stats for next frame
        accumulate_next_frame_stats(
            &next_frame, frame_info, flash_detected, rc->frames_since_key, i, gf_stats);
        *this_frame = next_frame;
    }

    // Was the group length constrained by the requirement for a new KF?
    rc->constrained_gf_group = (i >= rc->frames_to_key) ? 1 : 0;
    *use_alt_ref             = (i > 2);
    int alt_offset           = 0; // left to add code from 2-pass

    // Should we use the alternate reference frame.
    if (*use_alt_ref) {
        // Get from actual minigop size in PD
        set_baseline_gf_interval(ppcs, i);

        const int forward_frames = (rc->frames_to_key - i >= i - 1)
            ? i - 1
            : AOMMAX(0, rc->frames_to_key - i);

        // Calculate the boost for alt ref.
        rc->gfu_boost = av1_calc_arf_boost(
            twopass,
            rc,
            frame_info,
            alt_offset,
            forward_frames,
            (i - 1),
            scs->lap_rc ? &rc->num_stats_used_for_gfu_boost : NULL,
            scs->lap_rc ? &rc->num_stats_required_for_gfu_boost : NULL);
    } else {
        reset_fpf_position(twopass, start_pos);
        set_baseline_gf_interval(ppcs, i);

        rc->gfu_boost = AOMMIN(
            MAX_GF_BOOST,
            av1_calc_arf_boost(twopass,
                               rc,
                               frame_info,
                               alt_offset,
                               (i - 1),
                               0,
                               scs->lap_rc ? &rc->num_stats_used_for_gfu_boost : NULL,
                               scs->lap_rc ? &rc->num_stats_required_for_gfu_boost : NULL));
    }
    rc->arf_boost_factor = 1.0;
    // Reset the file position.
    reset_fpf_position(twopass, start_pos);
}
/***********************************************************************************
* calculate_active_worst_quality()
* Calculate an estimate of the maxq needed for the group.
************************************************************************************/
static void calculate_active_worst_quality(PictureParentControlSet *ppcs, GF_GROUP_STATS gf_stats) {
    SequenceControlSet *scs        = ppcs->scs_ptr;
    RATE_CONTROL *const rc         = &scs->encode_context_ptr->rc;
    FrameInfo          *frame_info = &scs->encode_context_ptr->frame_info;

    // Calculate an estimate of the maxq needed for the group.
    // We are more agressive about correcting for sections
    // where there could be significant overshoot than for easier
    // sections where we do not wish to risk creating an overshoot
    // of the allocated bit budget.
    if (rc->baseline_gf_interval > 1) {
        const int    vbr_group_bits_per_frame = (int)(rc->gf_group_bits / rc->baseline_gf_interval);
        const double group_av_err      = gf_stats.gf_group_raw_error / rc->baseline_gf_interval;
        const double group_av_skip_pct = gf_stats.gf_group_skip_pct / rc->baseline_gf_interval;
        const double group_av_inactive_zone = ((gf_stats.gf_group_inactive_zone_rows * 2) /
                                               (rc->baseline_gf_interval *
                                                (double)frame_info->mb_rows));

        int tmp_q;
        // rc factor is a weight factor that corrects for local rate control drift.
        double  rc_factor = 1.0;
        int64_t bits      = scs->static_config.target_bit_rate; //oxcf->target_bandwidth;

        if (bits > 0) {
            int rate_error;

            rate_error = (int)((rc->vbr_bits_off_target * 100) / bits);
            rate_error = clamp(rate_error, -100, 100);
            if (rate_error > 0) {
                double rc_factor_min = scs->static_config.gop_constraint_rc
                    ? RC_FACTOR_MIN_GOP_CONST
                    : (scs->static_config.pass == ENC_SINGLE_PASS) ? RC_FACTOR_MIN_1P_VBR
                                                                   : RC_FACTOR_MIN;
                rc_factor            = AOMMAX(rc_factor_min, (double)(100 - rate_error) / 100.0);
            } else {
                rc_factor = AOMMIN(RC_FACTOR_MAX, (double)(100 - rate_error) / 100.0);
            }
        }
        tmp_q = get_twopass_worst_quality(ppcs,
                                          group_av_err,
                                          (group_av_skip_pct + group_av_inactive_zone),
                                          vbr_group_bits_per_frame,
                                          rc_factor);
        if (scs->twopass.passes == 3) {
            int          ref_qindex = gf_stats.gf_stat_struct.worst_qindex;
            const double ref_q = svt_av1_convert_qindex_to_q(ref_qindex, scs->encoder_bit_depth);
            int64_t      ref_gf_group_bits    = (int64_t)(gf_stats.gf_group_err);
            int64_t      target_gf_group_bits = rc->gf_group_bits;
            {
                int low  = rc->best_quality;
                int high = rc->worst_quality;

                while (low < high) {
                    const int    mid = (low + high) >> 1;
                    const double q   = svt_av1_convert_qindex_to_q(mid, scs->encoder_bit_depth);
                    const int    mid_bits = (int)(ref_gf_group_bits * ref_q * rc_factor / q);

                    if (mid_bits > target_gf_group_bits) {
                        low = mid + 1;
                    } else {
                        high = mid;
                    }
                }
                tmp_q = low;
            }
        }
        rc->active_worst_quality = AOMMAX(tmp_q, rc->active_worst_quality >> 1);
    }
}

/*!\brief Assign rate to the GF group or mini gop.
 *
 * \ingroup gf_group_algo
 * This function assigns setup the various parameters regarding bit-allocation and quality setup.
 *
 * \param[in]    pcs_ptr         PictureParentControlSet
 * \param[in]    this_frame      First pass statistics structure
 *
 * \return Nothing is returned. Instead, encode_context_ptr->gf_group is changed.
 */
static void gf_group_rate_assingment(PictureParentControlSet *pcs_ptr,
                                     FIRSTPASS_STATS         *this_frame) {
    SequenceControlSet          *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext               *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const          rc                 = &encode_context_ptr->rc;
    TWO_PASS *const              twopass            = &scs_ptr->twopass;
    const FIRSTPASS_STATS *const start_pos          = twopass->stats_in;
#ifdef ARCH_X86_64
    aom_clear_system_state();
#endif
    GF_GROUP_STATS gf_stats;
    int            use_alt_ref;
    calculate_gf_stats(pcs_ptr, &gf_stats, this_frame, &use_alt_ref);

    // Calculate the bits to be allocated to the gf/arf group as a whole
    rc->gf_group_bits = calculate_total_gf_group_bits(pcs_ptr, gf_stats.gf_group_err);
    // Calculate an estimate of the maxq needed for the group.
    calculate_active_worst_quality(pcs_ptr, gf_stats);

    // Adjust KF group bits and error remaining.
    twopass->kf_group_error_left -= (int64_t)gf_stats.gf_group_err;

    // Reset the file position.
    reset_fpf_position(twopass, start_pos);

    if (twopass->passes == 3 && scs_ptr->static_config.pass == ENC_LAST_PASS)
        av1_gop_bit_allocation_same_pred(pcs_ptr, rc->gf_group_bits, gf_stats);
    else
        av1_gop_bit_allocation(pcs_ptr,
                               rc,
                               pcs_ptr->frm_hdr.frame_type == KEY_FRAME,
                               (1 << scs_ptr->static_config.hierarchical_levels),
                               use_alt_ref,
                               rc->gf_group_bits);
}

static void av1_gop_bit_allocation(PictureParentControlSet *ppcs, RATE_CONTROL *const rc,
                                   int is_key_frame, int gf_interval, int use_arf,
                                   int64_t gf_group_bits) {
    // Calculate the extra bits to be used for boosted frame(s)
    int gf_arf_bits = calculate_boost_bits(rc->baseline_gf_interval, rc->gfu_boost, gf_group_bits);
    // Allocate bits to each of the frames in the GF group.
    allocate_gf_group_bits(
        ppcs, rc, gf_group_bits, gf_arf_bits, gf_interval, is_key_frame, use_arf);
}

/*!\brief Variable initialization for lap_rc
 *
 * This function initialized some variable for lap_rc by looping over the look ahead
 *
 */
static void lap_rc_init(PictureParentControlSet *pcs_ptr, FIRSTPASS_STATS this_frame) {
    SequenceControlSet          *scs_ptr              = pcs_ptr->scs_ptr;
    EncodeContext               *encode_context_ptr   = scs_ptr->encode_context_ptr;
    TWO_PASS *const              twopass              = &scs_ptr->twopass;
    FrameInfo *const             frame_info           = &encode_context_ptr->frame_info;
    int                          num_stats            = 0;
    double                       modified_error_total = 0.0;
    double                       coded_error_total    = 0.0;
    const FIRSTPASS_STATS *const start_position       = twopass->stats_in;
    FIRSTPASS_STATS              this_frame_ref       = this_frame;

    // loop over the look ahead and calculate the coded error
    while (twopass->stats_in <= twopass->stats_buf_ctx->stats_in_end) {
        // Accumulate total number of stats available till end of the look ahead
        num_stats++;
        // Accumulate error.
        coded_error_total += this_frame.coded_error;
        // Load the next frame's stats.
        if (input_stats(twopass, &this_frame) == EOF)
            break;
    }
    // Calculate modified_error_min and modified_error_max which is needed in modified_error_total calculation
    const double avg_error = coded_error_total / DOUBLE_DIVIDE_CHECK(num_stats);

    twopass->modified_error_min = (avg_error * encode_context_ptr->two_pass_cfg.vbrmin_section) /
        100;
    twopass->modified_error_max = (avg_error * encode_context_ptr->two_pass_cfg.vbrmax_section) /
        100;
    reset_fpf_position(twopass, start_position);
    this_frame = this_frame_ref;

    // loop over the look ahead and calculate the modified_error_total
    while (twopass->stats_in <= twopass->stats_buf_ctx->stats_in_end) {
        // Accumulate error.
        modified_error_total += calculate_modified_err(
            frame_info, twopass, &(encode_context_ptr->two_pass_cfg), &this_frame);
        // Load the next frame's stats.
        if (input_stats(twopass, &this_frame) == EOF)
            break;
    }

    twopass->modified_error_left = modified_error_total;
    twopass->bits_left += (int64_t)(num_stats *
                                    (scs_ptr->static_config.target_bit_rate /
                                     scs_ptr->double_frame_rate));

    reset_fpf_position(twopass, start_position);
}
/*!\brief calculating group_error for lap_rc
 *
 * This function calculates group error for lap_rc by looping over the look ahead
 *
 */
static double lap_rc_group_error_calc(PictureParentControlSet *pcs_ptr,
                                      FIRSTPASS_STATS          this_frame) {
    SequenceControlSet          *scs_ptr              = pcs_ptr->scs_ptr;
    EncodeContext               *encode_context_ptr   = scs_ptr->encode_context_ptr;
    TWO_PASS *const              twopass              = &scs_ptr->twopass;
    FrameInfo *const             frame_info           = &encode_context_ptr->frame_info;
    int                          num_stats            = 0;
    double                       modified_error_total = 0.0;
    const FIRSTPASS_STATS *const start_position       = twopass->stats_in;

    // loop over the look ahead and calculate the modified_error_total
    while (twopass->stats_in <= twopass->stats_buf_ctx->stats_in_end &&
           num_stats < pcs_ptr->frames_to_key) {
        num_stats++;
        // Accumulate error.
        modified_error_total += calculate_modified_err(
            frame_info, twopass, &(encode_context_ptr->two_pass_cfg), &this_frame);
        // Load the next frame's stats.
        if (input_stats(twopass, &this_frame) == EOF)
            break;
    }
    reset_fpf_position(twopass, start_position);
    return modified_error_total;
}
/*!\brief Sets frames to key.
 *
 * \ingroup gf_group_algo
 * This function sets the frames to key for different scenarios
 *
 * \param[in]    this_frame       Pointer to first pass stats
 * \param[out]   kf_group_err     The total error in the KF group
 * \param[in]    num_frames_to_detect_scenecut Maximum lookahead frames.
 *
 * \return       Number of frames to the next key.
 */
static void set_kf_interval_variables(PictureParentControlSet *pcs_ptr, FIRSTPASS_STATS *this_frame,
                                      double *kf_group_err, int num_frames_to_detect_scenecut) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    TWO_PASS *const     twopass            = &scs_ptr->twopass;

    int              frames_to_key = 0;
    FrameInfo *const frame_info    = &encode_context_ptr->frame_info;
    if (num_frames_to_detect_scenecut == 0)
        return;

    while (twopass->stats_in <= twopass->stats_buf_ctx->stats_in_end &&
           frames_to_key < num_frames_to_detect_scenecut) {
        // Accumulate total number of stats available till next key frame

        // Accumulate kf group error.
        if (kf_group_err != NULL)
            *kf_group_err += calculate_modified_err(
                frame_info, twopass, &(encode_context_ptr->two_pass_cfg), this_frame);

        ++frames_to_key;
        if (input_stats(twopass, this_frame) == EOF)
            break;
    }
    if (scs_ptr->lap_rc && pcs_ptr->end_of_sequence_region)
        ((RateControlIntervalParamContext *)(pcs_ptr->rate_control_param_ptr))->end_of_seq_seen = 1;
    if (scs_ptr->lap_rc && !pcs_ptr->end_of_sequence_region)
        rc->frames_to_key = scs_ptr->static_config.intra_period_length + 1;
    else
        rc->frames_to_key = AOMMIN((scs_ptr->static_config.intra_period_length + 1), frames_to_key);
}
static int64_t get_kf_group_bits(PictureParentControlSet *pcs_ptr, double kf_group_err) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    TWO_PASS *const     twopass            = &scs_ptr->twopass;
    int64_t             kf_group_bits;
    if (scs_ptr->lap_rc && pcs_ptr->frames_in_sw < scs_ptr->static_config.intra_period_length &&
        !pcs_ptr->end_of_sequence_region)
        kf_group_bits = (int64_t)rc->frames_to_key * rc->avg_frame_bandwidth;
    else
        kf_group_bits = (int64_t)(twopass->bits_left *
                                  (kf_group_err / twopass->modified_error_left));

    return kf_group_bits;
}
static int calc_avg_stats(PictureParentControlSet *pcs_ptr, FIRSTPASS_STATS *avg_frame_stat) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    TWO_PASS *const     twopass            = &scs_ptr->twopass;
    FIRSTPASS_STATS     cur_frame;
    av1_zero(cur_frame);
    int num_frames = 0;
    // Accumulate total stat using available number of stats.
    for (num_frames = 0; num_frames < (rc->frames_to_key - 1); ++num_frames) {
        if (EOF == input_stats(twopass, &cur_frame))
            break;
        svt_av1_accumulate_stats(avg_frame_stat, &cur_frame);
    }

    if (num_frames < 2) {
        return num_frames;
    }
    // Average the total stat
    avg_frame_stat->weight             = avg_frame_stat->weight / num_frames;
    avg_frame_stat->intra_error        = avg_frame_stat->intra_error / num_frames;
    avg_frame_stat->coded_error        = avg_frame_stat->coded_error / num_frames;
    avg_frame_stat->sr_coded_error     = avg_frame_stat->sr_coded_error / num_frames;
    avg_frame_stat->pcnt_inter         = avg_frame_stat->pcnt_inter / num_frames;
    avg_frame_stat->pcnt_motion        = avg_frame_stat->pcnt_motion / num_frames;
    avg_frame_stat->pcnt_second_ref    = avg_frame_stat->pcnt_second_ref / num_frames;
    avg_frame_stat->pcnt_neutral       = avg_frame_stat->pcnt_neutral / num_frames;
    avg_frame_stat->intra_skip_pct     = avg_frame_stat->intra_skip_pct / num_frames;
    avg_frame_stat->inactive_zone_rows = avg_frame_stat->inactive_zone_rows / num_frames;
    avg_frame_stat->inactive_zone_cols = avg_frame_stat->inactive_zone_cols / num_frames;
    avg_frame_stat->mvr_abs            = avg_frame_stat->mvr_abs / num_frames;
    avg_frame_stat->mvc_abs            = avg_frame_stat->mvc_abs / num_frames;
    avg_frame_stat->mv_in_out_count    = avg_frame_stat->mv_in_out_count / num_frames;
    avg_frame_stat->count              = avg_frame_stat->count / num_frames;
    avg_frame_stat->duration           = avg_frame_stat->duration / num_frames;

    return num_frames;
}

static double get_kf_boost_score(PictureParentControlSet *pcs_ptr, double kf_raw_err,
                                 double *zero_motion_accumulator, double *sr_accumulator,
                                 int use_avg_stat) {
    SequenceControlSet         *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext              *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const         rc                 = &encode_context_ptr->rc;
    TWO_PASS *const             twopass            = &scs_ptr->twopass;
    FrameInfo                  *frame_info         = &encode_context_ptr->frame_info;
    const RateControlCfg *const rc_cfg             = &encode_context_ptr->rc_cfg;
    FIRSTPASS_STATS             frame_stat;
    av1_zero(frame_stat);
    int          i = 0, num_stat_used = 0;
    double       boost_score  = 0.0;
    const double kf_max_boost = rc_cfg->mode == AOM_Q
        ? AOMMIN(AOMMAX(rc->frames_to_key * 2.0, KF_MIN_FRAME_BOOST), KF_MAX_FRAME_BOOST)
        : KF_MAX_FRAME_BOOST;

    // Calculate the average using available number of stats.
    if (use_avg_stat)
        num_stat_used = calc_avg_stats(pcs_ptr, &frame_stat);

    for (i = num_stat_used; i < (rc->frames_to_key - 1); ++i) {
        if (!use_avg_stat && EOF == input_stats(twopass, &frame_stat))
            break;

        // Monitor for static sections.
        // For the first frame in kf group, the second ref indicator is invalid.
        if (i > 0) {
            *zero_motion_accumulator = AOMMIN(*zero_motion_accumulator,
                                              get_zero_motion_factor(frame_info, &frame_stat));
        } else {
            *zero_motion_accumulator = frame_stat.pcnt_inter - frame_stat.pcnt_motion;
        }

        // Not all frames in the group are necessarily used in calculating boost.
        if ((*sr_accumulator < (kf_raw_err * 1.50)) &&
            (i <= ((1 << scs_ptr->static_config.hierarchical_levels)) * 2)) {
            double frame_boost;
            double zm_factor;

            // Factor 0.75-1.25 based on how much of frame is static.
            zm_factor = (0.75 + (*zero_motion_accumulator / 2.0));

            if (i < 2)
                *sr_accumulator = 0.0;
            frame_boost = calc_kf_frame_boost(
                rc, frame_info, &frame_stat, sr_accumulator, kf_max_boost);
            boost_score += frame_boost * zm_factor;
        }
    }
    return boost_score;
}
#define MAX_KF_BITS_INTERVAL_SINGLE_PASS 5
/*****************************************************************************/
// kf_group_rate_assingment
// Rate assignment for the next kf group
// Only works for key frames, and scene change is not detected for now
/*****************************************************************************/
static void kf_group_rate_assingment(PictureParentControlSet *pcs_ptr, FIRSTPASS_STATS this_frame) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    TWO_PASS *const     twopass            = &scs_ptr->twopass;
    FrameInfo          *frame_info         = &encode_context_ptr->frame_info;
    FIRSTPASS_STATS     next_frame;
    av1_zero(next_frame);

    rc->frames_since_key = 0;

    const FIRSTPASS_STATS *const start_position          = twopass->stats_in;
    int                          kf_bits                 = 0;
    double                       zero_motion_accumulator = 1.0;
    double                       boost_score             = 0.0;
    double                       kf_raw_err;
    double                       kf_mod_err;
    double                       kf_group_err          = 0.0;
    double                       sr_accumulator        = 0.0;
    int                          frames_to_key_clipped = INT_MAX;
    int64_t                      kf_group_bits_clipped = INT64_MAX;

    twopass->kf_group_bits       = 0; // Total bits available to kf group
    twopass->kf_group_error_left = 0; // Group modified error score.
    kf_raw_err                   = this_frame.intra_error;
    kf_mod_err                   = calculate_modified_err(
        frame_info, twopass, &(encode_context_ptr->two_pass_cfg), &this_frame);
    set_kf_interval_variables(
        pcs_ptr, &this_frame, &kf_group_err, scs_ptr->static_config.intra_period_length + 1);

    // Calculate the number of bits that should be assigned to the kf group.
    if ((twopass->bits_left > 0 && twopass->modified_error_left > 0.0) || scs_ptr->lap_rc) {
        // Maximum number of bits for a single normal frame (not key frame).
        const int max_bits = frame_max_bits(rc, encode_context_ptr);

        // Maximum number of bits allocated to the key frame group.
        int64_t max_grp_bits;

        // Default allocation based on bits left and relative
        // complexity of the section.
        twopass->kf_group_bits = get_kf_group_bits(pcs_ptr, kf_group_err /*, kf_group_avg_error*/);
        // Clip based on maximum per frame rate defined by the user.
        max_grp_bits = (int64_t)max_bits * (int64_t)rc->frames_to_key;
        if (twopass->kf_group_bits > max_grp_bits)
            twopass->kf_group_bits = max_grp_bits;
    } else {
        twopass->kf_group_bits = 0;
    }
    twopass->kf_group_bits = AOMMAX(0, twopass->kf_group_bits);
    if (scs_ptr->lap_rc)
        // For 1 PASS VBR, as the lookahead is moving, the bits left is recalculated for the next KF. The second term is added again as it is part of look ahead of the next KF
        twopass->bits_left -=
            (twopass->kf_group_bits +
             (int64_t)(((int64_t)pcs_ptr->frames_in_sw - (int64_t)rc->frames_to_key) *
                       (scs_ptr->static_config.target_bit_rate / scs_ptr->double_frame_rate)));
    else
        twopass->bits_left = AOMMAX(twopass->bits_left - twopass->kf_group_bits, 0);
    if (scs_ptr->lap_rc) {
        // In the case of single pass based on LAP, frames to  key may have an
        // inaccurate value, and hence should be clipped to an appropriate
        // interval.
        frames_to_key_clipped = (int)(MAX_KF_BITS_INTERVAL_SINGLE_PASS *
                                      scs_ptr->double_frame_rate);

        // This variable calculates the bits allocated to kf_group with a clipped
        // frames_to_key.
        if (rc->frames_to_key > frames_to_key_clipped) {
            kf_group_bits_clipped = (int64_t)((double)twopass->kf_group_bits *
                                              frames_to_key_clipped / rc->frames_to_key);
        }
    }
    // Reset the first pass file position.
    reset_fpf_position(twopass, start_position);

    // Scan through the kf group collating various stats used to determine
    // how many bits to spend on it.
    boost_score = get_kf_boost_score(
        pcs_ptr, kf_raw_err, &zero_motion_accumulator, &sr_accumulator, 0);
    reset_fpf_position(twopass, start_position);
    // Store the zero motion percentage
    twopass->kf_zeromotion_pct = (int)(zero_motion_accumulator * 100.0);
    rc->kf_boost               = (int)boost_score;

    if (scs_ptr->lap_rc) {
        boost_score = get_kf_boost_score(
            pcs_ptr, kf_raw_err, &zero_motion_accumulator, &sr_accumulator, 1);
        reset_fpf_position(twopass, start_position);
        rc->kf_boost += (int)boost_score;
    }

    // Special case for static / slide show content but don't apply
    // if the kf group is very short.
    if ((zero_motion_accumulator > STATIC_KF_GROUP_FLOAT_THRESH) && (rc->frames_to_key > 8)) {
        rc->kf_boost = AOMMAX(rc->kf_boost, MIN_STATIC_KF_BOOST);
    } else {
        // Apply various clamps for min and max boost
        rc->kf_boost = AOMMAX(rc->kf_boost, (rc->frames_to_key * 3));
        rc->kf_boost = AOMMAX(rc->kf_boost, MIN_KF_BOOST);
    }
    // Work out how many bits to allocate for the key frame itself.
    // In case of LAP enabled for VBR, if the frames_to_key value is
    // very high, we calculate the bits based on a clipped value of
    // frames_to_key.
    if (twopass->passes == 3)
        kf_bits = (int)(twopass->kf_group_bits *
                        (twopass->stats_in - 1)->stat_struct.total_num_bits / kf_group_err);
    else
        kf_bits = calculate_boost_bits(AOMMIN(rc->frames_to_key, frames_to_key_clipped) - 1,
                                       rc->kf_boost,
                                       AOMMIN(twopass->kf_group_bits, kf_group_bits_clipped));

    twopass->kf_group_bits -= kf_bits;

    // Save the bits to spend on the key frame.
    pcs_ptr->base_frame_target = kf_bits;

    // Note the total error score of the kf group minus the key frame itself.
    twopass->kf_group_error_left = (int)(kf_group_err - kf_mod_err);

    // Adjust the count of total modified error left.
    // The count of bits left is adjusted elsewhere based on real coded frame
    // sizes.
    twopass->modified_error_left -= kf_group_err;
}
#define DEFAULT_GRP_WEIGHT 1.0

static int get_section_target_bandwidth(PictureParentControlSet *pcs_ptr) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    TWO_PASS *const     twopass            = &scs_ptr->twopass;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    int                 section_target_bandwidth;
    const int           frames_left = (int)(twopass->stats_buf_ctx->total_stats->count -
                                  pcs_ptr->picture_number);
    if (scs_ptr->lap_rc)
        section_target_bandwidth = (int)rc->avg_frame_bandwidth;
    else
        section_target_bandwidth = (int)(twopass->bits_left / frames_left);
    return section_target_bandwidth;
}

static void process_first_pass_stats(PictureParentControlSet *pcs_ptr,
                                     FIRSTPASS_STATS         *this_frame) {
    SequenceControlSet         *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext              *encode_context_ptr = scs_ptr->encode_context_ptr;
    TWO_PASS *const             twopass            = &scs_ptr->twopass;
    RATE_CONTROL *const         rc                 = &encode_context_ptr->rc;
    const RateControlCfg *const rc_cfg             = &encode_context_ptr->rc_cfg;
    uint32_t                    mb_rows;
    if (scs_ptr->mid_pass_ctrls.ds) {
        mb_rows = 2 * (scs_ptr->max_input_luma_height + 16 - 1) / 16;
    } else {
        mb_rows = (scs_ptr->max_input_luma_height + 16 - 1) / 16;
    }
    if (pcs_ptr->picture_number == 0 && twopass->stats_buf_ctx->total_stats &&
        twopass->stats_buf_ctx->total_left_stats) {
        if (scs_ptr->lap_rc) {
            /*
       * Accumulate total_stats using available limited number of stats,
       * and assign it to total_left_stats.
       */
            *twopass->stats_buf_ctx->total_left_stats = *twopass->stats_buf_ctx->total_stats;
        }
        // Special case code for first frame.
        const int    section_target_bandwidth = get_section_target_bandwidth(pcs_ptr);
        const double section_length           = twopass->stats_buf_ctx->total_left_stats->count;
        const double section_error = twopass->stats_buf_ctx->total_left_stats->coded_error /
            section_length;
        const double section_intra_skip = twopass->stats_buf_ctx->total_left_stats->intra_skip_pct /
            section_length;
        const double section_inactive_zone =
            (twopass->stats_buf_ctx->total_left_stats->inactive_zone_rows * 2) /
            ((double)/*cm->mi_params.*/ mb_rows * section_length);
        int tmp_q;
        if (scs_ptr->passes == 3) {
            int ref_qindex     = twopass->stats_buf_ctx->stats_in_start->stat_struct.worst_qindex;
            const double ref_q = svt_av1_convert_qindex_to_q(ref_qindex,
                                                             scs_ptr->encoder_bit_depth);
            int64_t      ref_gf_group_bits =
                (int64_t)(twopass->stats_buf_ctx->total_stats->stat_struct.total_num_bits);
            int64_t target_gf_group_bits = twopass->bits_left;
            {
                int low  = rc->best_quality;
                int high = rc->worst_quality;

                while (low < high) {
                    const int    mid = (low + high) >> 1;
                    const double q   = svt_av1_convert_qindex_to_q(mid, scs_ptr->encoder_bit_depth);
                    const int    mid_bits = (int)(ref_gf_group_bits * ref_q / q);

                    if (mid_bits > target_gf_group_bits)
                        low = mid + 1;
                    else
                        high = mid;
                }
                tmp_q = low;
            }
        } else
            tmp_q = get_twopass_worst_quality(pcs_ptr,
                                              section_error,
                                              section_intra_skip + section_inactive_zone,
                                              section_target_bandwidth,
                                              DEFAULT_GRP_WEIGHT);

        rc->active_worst_quality          = tmp_q;
        rc->avg_frame_qindex[INTER_FRAME] = tmp_q;
        rc->avg_frame_qindex[KEY_FRAME]   = (tmp_q + rc_cfg->best_allowed_q) / 2;
    }

    int err = input_stats(twopass, this_frame);
    if (err == EOF)
        return;

    // Update the total stats remaining structure.
    if (twopass->stats_buf_ctx->total_left_stats)
        subtract_stats(twopass->stats_buf_ctx->total_left_stats, this_frame);
}
// Calculates is new gf group and stores in pcs_ptr->is_new_gf_group
// For P pictures in the incomplete minigops, since there is no order, we search all of them and set the flag accordingly
static void is_new_gf_group(PictureParentControlSet *pcs_ptr) {
    pcs_ptr->is_new_gf_group = 0;
    if (pcs_ptr->slice_type != P_SLICE)
        pcs_ptr->is_new_gf_group = pcs_ptr->gf_update_due;
    else {
        for (int pic_i = 0; pic_i < pcs_ptr->gf_interval; ++pic_i)
            // For P-pictures, since the pictures might get released and replaced by other pictures, we check the POC difference
            if (pcs_ptr->gf_group[pic_i] &&
                (int)ABS((int64_t)pcs_ptr->gf_group[pic_i]->picture_number -
                         (int64_t)pcs_ptr->picture_number) <= pcs_ptr->gf_interval &&
                pcs_ptr->gf_group[pic_i]->slice_type == P_SLICE &&
                pcs_ptr->gf_group[pic_i]->gf_update_due)
                pcs_ptr->is_new_gf_group = 1;
        if (pcs_ptr->is_new_gf_group)
            for (int pic_i = 0; pic_i < pcs_ptr->gf_interval; ++pic_i)
                if (pcs_ptr->gf_group[pic_i])
                    pcs_ptr->gf_group[pic_i]->gf_update_due = 0;
    }
}

#define DEFAULT_KF_BOOST_RT 2300
#define DEFAULT_GF_BOOST_RT 2000
static int set_gf_interval_update_onepass_rt(PictureParentControlSet *pcs_ptr) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    int                 gf_update          = 0;
    // GF update based on frames_till_gf_update_due, also
    // force upddate on resize pending frame or for scene change.
    if ((pcs_ptr->frame_offset % MAX_GF_INTERVAL) == 0) {
        rc->baseline_gf_interval = MAX_GF_INTERVAL;
        if (rc->baseline_gf_interval > rc->frames_to_key)
            rc->baseline_gf_interval = rc->frames_to_key;
        rc->gfu_boost            = DEFAULT_GF_BOOST_RT;
        rc->constrained_gf_group = (rc->baseline_gf_interval >= rc->frames_to_key) ? 1 : 0;
        gf_update                = 1;
    }
    return gf_update;
}

void reset_update_frame_target(PictureParentControlSet *ppcs_ptr) {
    SequenceControlSet *scs_ptr            = ppcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL       *rc                 = &encode_context_ptr->rc;
    rc->buffer_level                       = rc->optimal_buffer_level;
    rc->bits_off_target                    = rc->optimal_buffer_level;
    ppcs_ptr->this_frame_target = av1_calc_pframe_target_size_one_pass_cbr(ppcs_ptr, INTER_FRAME);
}

extern void svt_av1_resize_reset_rc(PictureParentControlSet *ppcs_ptr, int32_t resize_width,
                                    int32_t resize_height, int32_t prev_width, int32_t prev_height);
static void dynamic_resize_one_pass_cbr(PictureParentControlSet *ppcs_ptr) {
    SequenceControlSet *scs_ptr            = ppcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL       *rc                 = &encode_context_ptr->rc;
    RESIZE_ACTION       resize_action      = NO_RESIZE;
    const int32_t       avg_qp_thr1        = 70;
    const int32_t       avg_qp_thr2        = 50;
    // Don't allow for resized frame to go below 160x90, resize in steps of 3/4.
    const int32_t min_width    = (160 * 4) / 3;
    const int32_t min_height   = (90 * 4) / 3;
    Bool          down_size_on = TRUE;

    // Step 1: check frame type
    // Don't resize on key frame; reset the counters on key frame.
    if (ppcs_ptr->frm_hdr.frame_type == KEY_FRAME) {
        rc->resize_avg_qp           = 0;
        rc->resize_count            = 0;
        rc->resize_buffer_underflow = 0;
        return;
    }

    // Step 2: check frame size
    // No resizing down if frame size is below some limit.
    if ((ppcs_ptr->frame_width * ppcs_ptr->frame_height) < min_width * min_height)
        down_size_on = FALSE;

    // Step 3: calculate dynamic resize state
    // Resize based on average buffer underflow and QP over some window.
    // Ignore samples close to key frame, since QP is usually high after key.
    if (rc->frames_since_key > scs_ptr->double_frame_rate) {
        const int32_t window = AOMMIN(30, (int32_t)(2 * scs_ptr->double_frame_rate));
        rc->resize_avg_qp += rc->last_q[INTER_FRAME];
        if (rc->buffer_level < (int32_t)(30 * rc->optimal_buffer_level / 100))
            ++rc->resize_buffer_underflow;
        ++rc->resize_count;
        // Check for resize action every "window" frames.
        if (rc->resize_count >= window) {
            int32_t avg_qp = rc->resize_avg_qp / rc->resize_count;
            // Resize down if buffer level has underflowed sufficient amount in past
            // window, and we are at original or 3/4 of original resolution.
            // Resize back up if average QP is low, and we are currently in a resized
            // down state, i.e. 1/2 or 3/4 of original resolution.
            // Currently, use a flag to turn 3/4 resizing feature on/off.
            if (rc->resize_buffer_underflow > (rc->resize_count >> 2) && down_size_on) {
                if (rc->resize_state == THREE_QUARTER) {
                    resize_action = DOWN_ONEHALF;
                    printf("Dynamic resize: %d --> %d\n", rc->resize_state, ONE_HALF);
                    rc->resize_state = ONE_HALF;
                } else if (rc->resize_state == ORIG) {
                    resize_action = DOWN_THREEFOUR;
                    printf("Dynamic resize: %d --> %d\n", rc->resize_state, THREE_QUARTER);
                    rc->resize_state = THREE_QUARTER;
                }
            } else if (rc->resize_state != ORIG && avg_qp < avg_qp_thr1 * rc->worst_quality / 100) {
                if (rc->resize_state == THREE_QUARTER ||
                    avg_qp < avg_qp_thr2 * rc->worst_quality / 100) {
                    resize_action = UP_ORIG;
                    printf("Dynamic resize: %d --> %d\n", rc->resize_state, ORIG);
                    rc->resize_state = ORIG;
                } else if (rc->resize_state == ONE_HALF) {
                    resize_action = UP_THREEFOUR;
                    printf("Dynamic resize: %d --> %d\n", rc->resize_state, THREE_QUARTER);
                    rc->resize_state = THREE_QUARTER;
                }
            }
            // Reset for next window measurement.
            rc->resize_avg_qp           = 0;
            rc->resize_count            = 0;
            rc->resize_buffer_underflow = 0;
        }
    }

    // Step 4: reset rate control configuration
    // If decision is to resize, reset some quantities, and check is we should
    // reduce rate correction factor,
    if (resize_action != NO_RESIZE) {
        int32_t resize_width     = ppcs_ptr->frame_width; // cpi->oxcf.frm_dim_cfg.width;
        int32_t resize_height    = ppcs_ptr->frame_height; // cpi->oxcf.frm_dim_cfg.height;
        int32_t resize_scale_num = 1;
        int32_t resize_scale_den = 1;
        if (resize_action == DOWN_THREEFOUR || resize_action == UP_THREEFOUR) {
            resize_scale_num = 3;
            resize_scale_den = 4;
        } else if (resize_action == DOWN_ONEHALF) {
            resize_scale_num = 1;
            resize_scale_den = 2;
        }
        resize_width  = resize_width * resize_scale_num / resize_scale_den;
        resize_height = resize_height * resize_scale_num / resize_scale_den;
        svt_av1_resize_reset_rc(
            ppcs_ptr, resize_width, resize_height, ppcs_ptr->frame_width, ppcs_ptr->frame_height);
    }
    return;
}

void one_pass_rt_rate_alloc(PictureParentControlSet *pcs_ptr) {
    SequenceControlSet *scs_ptr            = pcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    int                 target             = 0;
    // Set frame type.
    if (frame_is_intra_only(pcs_ptr))
        rc->kf_boost = DEFAULT_KF_BOOST_RT;

    if (frame_is_intra_only(pcs_ptr)) {
        rc->this_key_frame_forced = pcs_ptr->picture_number != 0 && rc->frames_to_key == 0;
        rc->frames_to_key         = scs_ptr->static_config.intra_period_length + 1;
    }

    /* resize dynamic mode make desicion of scaling here and store it in resize_pending_params,
     * the actual resizing performs on the next new input picture in PD, current picture and
     * other pictures already in pipeline use their own resolution without resizing
     */
    // resize dynamic mode only works with 1-pass CBR low delay mode
    if (scs_ptr->static_config.resize_mode == RESIZE_DYNAMIC &&
        scs_ptr->static_config.pass == ENC_SINGLE_PASS &&
        scs_ptr->static_config.pred_structure == 1) {
        dynamic_resize_one_pass_cbr(pcs_ptr);
        if (rc->resize_state != scs_ptr->resize_pending_params.resize_state) {
            if (rc->resize_state == ORIG)
                scs_ptr->resize_pending_params.resize_denom = SCALE_NUMERATOR;
            else if (rc->resize_state == THREE_QUARTER)
                scs_ptr->resize_pending_params.resize_denom = SCALE_THREE_QUATER;
            else if (rc->resize_state == ONE_HALF)
                scs_ptr->resize_pending_params.resize_denom = SCALE_DENOMINATOR_MAX;
            else
                assert_err(0, "unknown resize denom");
            scs_ptr->resize_pending_params.resize_state = rc->resize_state;
        }
    } else if (pcs_ptr->rc_reset_flag) {
        svt_av1_resize_reset_rc(pcs_ptr,
                                pcs_ptr->render_width,
                                pcs_ptr->render_height,
                                scs_ptr->max_input_luma_width,
                                scs_ptr->max_input_luma_height);
    }

    // Set the GF interval and update flag.
    set_gf_interval_update_onepass_rt(pcs_ptr);
    // Set target size.
    if (encode_context_ptr->rc_cfg.mode == AOM_CBR) {
        if (pcs_ptr->frm_hdr.frame_type == KEY_FRAME) {
            target = av1_calc_iframe_target_size_one_pass_cbr(pcs_ptr);
        } else {
            target = av1_calc_pframe_target_size_one_pass_cbr(pcs_ptr, pcs_ptr->update_type);
        }
    }
    pcs_ptr->this_frame_target = target;
    pcs_ptr->base_frame_target = target;
}
void process_rc_stat(PictureParentControlSet *pcs_ptr) {
    SequenceControlSet *scs_ptr = pcs_ptr->scs_ptr;
    TWO_PASS *const     twopass = &scs_ptr->twopass;
#ifdef ARCH_X86_64
    aom_clear_system_state();
#endif
    FIRSTPASS_STATS this_frame;
    av1_zero(this_frame);
    process_first_pass_stats(pcs_ptr, &this_frame);

    // Keyframe and section processing.
    if (frame_is_intra_only(pcs_ptr) && pcs_ptr->idr_flag) {
        if (scs_ptr->lap_rc)
            lap_rc_init(pcs_ptr, this_frame);
        // Rate assignment for the next kf group
        kf_group_rate_assingment(pcs_ptr, this_frame);
    }
    // Define a new GF/ARF group. (Should always enter here for key frames).
    is_new_gf_group(pcs_ptr);
    if (pcs_ptr->is_new_gf_group) {
        // For 1 pass VBR, as the look ahead moves, the kf_group_error_left changes. It is because in modified_error calculation, av_weight and av_err depand on total_stats, which gets updated.
        // So, we recalculate kf_group_error_left for each mini gop except the first one after KF
        if (!(frame_is_intra_only(pcs_ptr) && pcs_ptr->idr_flag) && scs_ptr->lap_rc) {
            twopass->kf_group_error_left = (int)lap_rc_group_error_calc(pcs_ptr, this_frame);
        }

        gf_group_rate_assingment(pcs_ptr, &this_frame);
    }
}

// Max rate target for 1080P and below encodes under normal circumstances
// (1920 * 1080 / (16 * 16)) * MAX_MB_RATE bits per MB
#define MAX_MB_RATE 250
#define MAXRATE_1080P 2025000
static void av1_rc_update_framerate(SequenceControlSet *scs_ptr /*, int width, int height*/) {
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;
    FrameInfo          *frame_info         = &encode_context_ptr->frame_info;
    int                 vbr_max_bits;
    const int           MBs = frame_info->num_mbs; //av1_get_MBs(width, height);

    rc->avg_frame_bandwidth =
        (int)(scs_ptr->static_config.target_bit_rate /*oxcf->target_bandwidth*/ /
              scs_ptr->double_frame_rate);
    // A maximum bitrate for a frame is defined.
    // The baseline for this aligns with HW implementations that
    // can support decode of 1080P content up to a bitrate of MAX_MB_RATE bits
    // per 16x16 MB (averaged over a frame). However this limit is extended if
    // a very high rate is given on the command line or the the rate cannnot
    // be acheived because of a user specificed max q (e.g. when the user
    // specifies lossless encode.
    vbr_max_bits            = (int)(((int64_t)rc->avg_frame_bandwidth *
                          encode_context_ptr->two_pass_cfg.vbrmax_section) /
                         100);
    rc->max_frame_bandwidth = AOMMAX(AOMMAX((MBs * MAX_MB_RATE), MAXRATE_1080P), vbr_max_bits);
}

// from aom encoder.c
void svt_av1_new_framerate(SequenceControlSet *scs_ptr, double framerate) {
    //cpi->framerate = framerate < 0.1 ? 30 : framerate;
    scs_ptr->double_frame_rate = framerate < 0.1 ? 30 : framerate;
    av1_rc_update_framerate(
        scs_ptr /*, scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height*/);
}
void set_rc_param(SequenceControlSet *scs_ptr) {
    EncodeContext *encode_context_ptr = scs_ptr->encode_context_ptr;
    FrameInfo     *frame_info         = &encode_context_ptr->frame_info;

    const int is_vbr = scs_ptr->static_config.rate_control_mode == SVT_AV1_RC_MODE_VBR;
    if (scs_ptr->mid_pass_ctrls.ds) {
        frame_info->frame_width  = scs_ptr->max_input_luma_width << 1;
        frame_info->frame_height = scs_ptr->max_input_luma_height << 1;
        frame_info->mb_cols      = ((scs_ptr->max_input_luma_width + 16 - 1) / 16) << 1;
        frame_info->mb_rows      = ((scs_ptr->max_input_luma_height + 16 - 1) / 16) << 1;
    } else {
        frame_info->frame_width  = scs_ptr->max_input_luma_width;
        frame_info->frame_height = scs_ptr->max_input_luma_height;
        frame_info->mb_cols      = (scs_ptr->max_input_luma_width + 16 - 1) / 16;
        frame_info->mb_rows      = (scs_ptr->max_input_luma_height + 16 - 1) / 16;
    }
    frame_info->num_mbs   = frame_info->mb_cols * frame_info->mb_rows;
    frame_info->bit_depth = scs_ptr->static_config.encoder_bit_depth;
    // input config  from options
    encode_context_ptr->two_pass_cfg.vbrmin_section = scs_ptr->static_config.vbr_min_section_pct;
    encode_context_ptr->two_pass_cfg.vbrmax_section = scs_ptr->static_config.vbr_max_section_pct;
    encode_context_ptr->two_pass_cfg.vbrbias        = scs_ptr->static_config.vbr_bias_pct;
    encode_context_ptr->rc_cfg.gf_cbr_boost_pct     = 0;
    encode_context_ptr->rc_cfg.mode                 = scs_ptr->static_config.rate_control_mode ==
            SVT_AV1_RC_MODE_VBR
                        ? AOM_VBR
                        : (scs_ptr->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR ? AOM_CBR : AOM_Q);
    encode_context_ptr->rc_cfg.best_allowed_q       = (int32_t)
        quantizer_to_qindex[scs_ptr->static_config.min_qp_allowed];
    encode_context_ptr->rc_cfg.worst_allowed_q = (int32_t)
        quantizer_to_qindex[scs_ptr->static_config.max_qp_allowed];

    if (scs_ptr->static_config.gop_constraint_rc) {
        encode_context_ptr->rc_cfg.over_shoot_pct  = 0;
        encode_context_ptr->rc_cfg.under_shoot_pct = 0;
    } else {
        encode_context_ptr->rc_cfg.over_shoot_pct  = scs_ptr->static_config.over_shoot_pct;
        encode_context_ptr->rc_cfg.under_shoot_pct = scs_ptr->static_config.under_shoot_pct;
    }
    encode_context_ptr->rc_cfg.cq_level = quantizer_to_qindex[scs_ptr->static_config.qp];
    encode_context_ptr->rc_cfg.maximum_buffer_size_ms   = is_vbr
          ? 240000
          : scs_ptr->static_config.maximum_buffer_size_ms;
    encode_context_ptr->rc_cfg.starting_buffer_level_ms = is_vbr
        ? 60000
        : scs_ptr->static_config.starting_buffer_level_ms;
    encode_context_ptr->rc_cfg.optimal_buffer_level_ms  = is_vbr
         ? 60000
         : scs_ptr->static_config.optimal_buffer_level_ms;

    // todo: to expose to a cli parameter
    encode_context_ptr->rc_cfg.max_intra_bitrate_pct = 300;

    encode_context_ptr->sf_cfg.sframe_dist = scs_ptr->static_config.sframe_dist;
    encode_context_ptr->sf_cfg.sframe_mode = scs_ptr->static_config.sframe_mode;
}
/******************************************************
 * Read Stat from File
 * reads StatStruct per frame from the file and stores under pcs_ptr
 ******************************************************/
static void read_stat_from_file(SequenceControlSet *scs_ptr) {
    TWO_PASS *const  twopass                                = &scs_ptr->twopass;
    FIRSTPASS_STATS *this_frame                             = (FIRSTPASS_STATS *)twopass->stats_in;
    uint64_t         total_num_bits                         = 0;
    uint64_t         previous_num_bits[MAX_TEMPORAL_LAYERS] = {0};
    while (this_frame < twopass->stats_buf_ctx->stats_in_end) {
        if (this_frame->stat_struct.total_num_bits == 0)
            this_frame->stat_struct.total_num_bits =
                previous_num_bits[MAX((int)this_frame->stat_struct.temporal_layer_index, 0)];
        previous_num_bits[this_frame->stat_struct.temporal_layer_index] =
            this_frame->stat_struct.total_num_bits;
        total_num_bits += this_frame->stat_struct.total_num_bits;
        this_frame++;
    }
    twopass->stats_buf_ctx->total_stats->stat_struct.total_num_bits = total_num_bits;
}
void svt_av1_init_single_pass_lap(SequenceControlSet *scs_ptr) {
    TWO_PASS *const twopass            = &scs_ptr->twopass;
    EncodeContext  *encode_context_ptr = scs_ptr->encode_context_ptr;
    if (!twopass->stats_buf_ctx->stats_in_end)
        return;

    set_rc_param(scs_ptr);

    // This variable monitors how far behind the second ref update is lagging.

    twopass->bits_left           = 0;
    twopass->modified_error_min  = 0.0;
    twopass->modified_error_max  = 0.0;
    twopass->modified_error_left = 0.0;

    // Reset the vbr bits off target counters
    encode_context_ptr->rc.vbr_bits_off_target      = 0;
    encode_context_ptr->rc.vbr_bits_off_target_fast = 0;

    encode_context_ptr->rc.rate_error_estimate = 0;

    // Static sequence monitor variables.
    twopass->kf_zeromotion_pct = 100;
}
void svt_av1_init_second_pass(SequenceControlSet *scs_ptr) {
    TWO_PASS *const twopass            = &scs_ptr->twopass;
    EncodeContext  *encode_context_ptr = scs_ptr->encode_context_ptr;
    FrameInfo      *frame_info         = &encode_context_ptr->frame_info;

    double           frame_rate;
    FIRSTPASS_STATS *stats;

    if (!twopass->stats_buf_ctx->stats_in_end)
        return;

    if (twopass->passes == 3 && scs_ptr->static_config.pass != ENC_MIDDLE_PASS) {
        svt_av1_twopass_zero_stats(twopass->stats_buf_ctx->stats_in_end);
        FIRSTPASS_STATS *this_frame     = (FIRSTPASS_STATS *)scs_ptr->twopass.stats_in;
        uint64_t         total_num_bits = 0;

        while (this_frame < scs_ptr->twopass.stats_buf_ctx->stats_in_end) {
            if (twopass->stats_buf_ctx->stats_in_end != NULL) {
                svt_av1_accumulate_stats(twopass->stats_buf_ctx->stats_in_end, this_frame);
            }
            total_num_bits += this_frame->stat_struct.total_num_bits;
            this_frame++;
        }
        twopass->stats_buf_ctx->stats_in_end->stat_struct.total_num_bits = total_num_bits;
    }
    set_rc_param(scs_ptr);
    stats = twopass->stats_buf_ctx->total_stats;

    *stats                                    = *twopass->stats_buf_ctx->stats_in_end;
    *twopass->stats_buf_ctx->total_left_stats = *stats;

    frame_rate = 10000000.0 * stats->count / stats->duration;
    // Each frame can have a different duration, as the frame rate in the source
    // isn't guaranteed to be constant. The frame rate prior to the first frame
    // encoded in the second pass is a guess. However, the sum duration is not.
    // It is calculated based on the actual durations of all frames from the
    // first pass.
    svt_av1_new_framerate(scs_ptr, frame_rate);
    twopass->bits_left = (int64_t)(stats->duration *
                                   (int64_t)scs_ptr->static_config.target_bit_rate / 10000000.0);

    if (twopass->passes == 3 && scs_ptr->static_config.pass != ENC_MIDDLE_PASS)
        read_stat_from_file(scs_ptr);
    // Scan the first pass file and calculate a modified total error based upon
    // the bias/power function used to allocate bits.
    {
        const double           avg_error = stats->coded_error / DOUBLE_DIVIDE_CHECK(stats->count);
        const FIRSTPASS_STATS *s         = twopass->stats_in;
        double                 modified_error_total = 0.0;
        twopass->modified_error_min                 = (avg_error *
                                       encode_context_ptr->two_pass_cfg.vbrmin_section) /
            100;
        twopass->modified_error_max = (avg_error *
                                       encode_context_ptr->two_pass_cfg.vbrmax_section) /
            100;
        while (s < twopass->stats_buf_ctx->stats_in_end) {
            modified_error_total += calculate_modified_err(
                frame_info, twopass, &(encode_context_ptr->two_pass_cfg), s);
            ++s;
        }
        twopass->modified_error_left = modified_error_total;
    }

    // Reset the vbr bits off target counters
    encode_context_ptr->rc.vbr_bits_off_target      = 0;
    encode_context_ptr->rc.vbr_bits_off_target_fast = 0;
    encode_context_ptr->rc.rate_error_estimate      = 0;

    // Static sequence monitor variables.
    twopass->kf_zeromotion_pct = 100;
}
/*********************************************************************************************
* Find the initial QP to be used in the middle pass based on the target rate
* and stats from previous pass
***********************************************************************************************/
void find_init_qp_middle_pass(SequenceControlSet *scs_ptr, PictureParentControlSet *pcs_ptr) {
    TWO_PASS *const twopass            = &scs_ptr->twopass;
    EncodeContext  *encode_context_ptr = scs_ptr->encode_context_ptr;
    if (scs_ptr->static_config.pass == ENC_MIDDLE_PASS && twopass->stats_buf_ctx->total_stats &&
        twopass->stats_buf_ctx->total_left_stats) {
        RATE_CONTROL *const         rc     = &encode_context_ptr->rc;
        const RateControlCfg *const rc_cfg = &encode_context_ptr->rc_cfg;
        rc->worst_quality                  = rc_cfg->worst_allowed_q;
        rc->best_quality                   = rc_cfg->best_allowed_q;
        uint32_t mb_rows;
        if (scs_ptr->mid_pass_ctrls.ds)
            mb_rows = 2 * (scs_ptr->max_input_luma_height + 16 - 1) / 16;
        else
            mb_rows = (scs_ptr->max_input_luma_height + 16 - 1) / 16;
        const int    section_target_bandwidth = get_section_target_bandwidth(pcs_ptr);
        const double section_length           = twopass->stats_buf_ctx->total_left_stats->count;
        const double section_error = twopass->stats_buf_ctx->total_left_stats->coded_error /
            section_length;
        const double section_intra_skip = twopass->stats_buf_ctx->total_left_stats->intra_skip_pct /
            section_length;
        const double section_inactive_zone =
            (twopass->stats_buf_ctx->total_left_stats->inactive_zone_rows * 2) /
            ((double)mb_rows * section_length);

        const int tmp_q = get_twopass_worst_quality(pcs_ptr,
                                                    section_error,
                                                    section_intra_skip + section_inactive_zone,
                                                    section_target_bandwidth,
                                                    DEFAULT_GRP_WEIGHT);

        rc->active_worst_quality = tmp_q;
        // For same pred pass, use the estimated QP as the sequence QP
        scs_ptr->static_config.qp = (uint8_t)CLIP3(
            (int32_t)scs_ptr->static_config.min_qp_allowed,
            (int32_t)scs_ptr->static_config.max_qp_allowed - 4,
            (int32_t)((tmp_q + 2) >> 2));
        encode_context_ptr->rc_cfg.cq_level = scs_ptr->static_config.qp << 2;
    }
}
int frame_is_kf_gf_arf(PictureParentControlSet *ppcs_ptr) {
    return frame_is_intra_only(ppcs_ptr) || ppcs_ptr->update_type == ARF_UPDATE ||
        ppcs_ptr->update_type == GF_UPDATE;
}
/*********************************************************************************************
* Update the internal RC and TWO_PASS struct stats based on the received feedback
***********************************************************************************************/
void svt_av1_twopass_postencode_update_gop_const(PictureParentControlSet *ppcs) {
    SequenceControlSet              *scs          = ppcs->scs_ptr;
    EncodeContext                   *enc_cont     = scs->encode_context_ptr;
    RATE_CONTROL *const              rc           = &enc_cont->rc;
    const RateControlCfg *const      rc_cfg       = &enc_cont->rc_cfg;
    RateControlIntervalParamContext *rc_param_ptr = ppcs->rate_control_param_ptr;

    // VBR correction is done through rc->vbr_bits_off_target. Based on the
    // sign of this value, a limited % adjustment is made to the target rate
    // of subsequent frames, to try and push it back towards 0. This method
    // is designed to prevent extreme behaviour at the end of a clip
    // or group of frames.
    rc_param_ptr->vbr_bits_off_target += ppcs->base_frame_target - ppcs->projected_frame_size;
    // Target vs actual bits for this arf group.
    int rate_error_estimate_target = 0;
    // Calculate the pct rc error.
    if (rc_param_ptr->total_actual_bits) {
        if (rc_param_ptr->total_target_bits)
            rate_error_estimate_target = (int)((rc_param_ptr->vbr_bits_off_target * 100) /
                                               rc_param_ptr->total_target_bits);
        rc_param_ptr->rate_error_estimate = (int)((rc_param_ptr->vbr_bits_off_target * 100) /
                                                  rc_param_ptr->total_actual_bits);
        rc_param_ptr->rate_error_estimate = clamp(rc_param_ptr->rate_error_estimate, -100, 100);
    } else {
        rc_param_ptr->rate_error_estimate = 0;
    }

    // Update the active best quality pyramid.
    if (!ppcs->is_overlay) {
        const int pyramid_level = ppcs->layer_depth;
        int       i;
        for (i = pyramid_level; i <= MAX_ARF_LAYERS; ++i) {
            rc->active_best_quality[i] = ppcs->frm_hdr.quantization_params.base_q_idx;
        }
    }

    // If the rate control is drifting consider adjustment to min or maxq.
    if (!ppcs->is_overlay) {
        const int maxq_adj_limit = rc->worst_quality - rc->active_worst_quality;
        const int minq_adj_limit = MINQ_ADJ_LIMIT;

        // Undershoot.
        if (rc_param_ptr->rate_error_estimate > rc_cfg->under_shoot_pct) {
            --rc_param_ptr->extend_maxq;
            if (rc_param_ptr->rolling_target_bits >= rc_param_ptr->rolling_actual_bits)
                ++rc_param_ptr->extend_minq;
            // Overshoot.
        } else if (rc_param_ptr->rate_error_estimate < -rc_cfg->over_shoot_pct) {
            --rc_param_ptr->extend_minq;
            if (rc_param_ptr->rolling_target_bits < rc_param_ptr->rolling_actual_bits)
                rc_param_ptr->extend_maxq += (scs->is_short_clip)
                    ? rate_error_estimate_target < -100 ? 10 : 2
                    : 1;
        } else {
            // Adjustment for extreme local overshoot.
            if (ppcs->projected_frame_size > (2 * ppcs->base_frame_target) &&
                ppcs->projected_frame_size > (2 * rc->avg_frame_bandwidth))
                ++rc_param_ptr->extend_maxq;

            // Unwind undershoot or overshoot adjustment.
            if (rc_param_ptr->rolling_target_bits < rc_param_ptr->rolling_actual_bits)
                --rc_param_ptr->extend_minq;
            else if (rc_param_ptr->rolling_target_bits > rc_param_ptr->rolling_actual_bits)
                --rc_param_ptr->extend_maxq;
            if (scs->is_short_clip) {
                if (rc_param_ptr->extend_minq > minq_adj_limit / 3)
                    rc_param_ptr->extend_minq -= 5;
                if (rc_param_ptr->extend_maxq < -maxq_adj_limit / 3)
                    rc_param_ptr->extend_maxq += 5;
            }
        }
        if (scs->is_short_clip)
            rc_param_ptr->extend_minq = clamp(
                rc_param_ptr->extend_minq, -minq_adj_limit / 4, minq_adj_limit);
        else
            rc_param_ptr->extend_minq = clamp(rc_param_ptr->extend_minq, 0, minq_adj_limit);
        if (!scs->is_short_clip)
            rc_param_ptr->extend_maxq = clamp(rc_param_ptr->extend_maxq, 0, maxq_adj_limit);

        // If there is a big and undexpected undershoot then feed the extra
        // bits back in quickly. One situation where this may happen is if a
        // frame is unexpectedly almost perfectly predicted by the ARF or GF
        // but not very well predcited by the previous frame.
        if (!frame_is_kf_gf_arf(ppcs) && !ppcs->is_overlay) {
            int fast_extra_thresh = ppcs->base_frame_target / HIGH_UNDERSHOOT_RATIO;
            if (ppcs->projected_frame_size < fast_extra_thresh &&
                rc_param_ptr->rate_error_estimate > 0) {
                rc_param_ptr->vbr_bits_off_target_fast += fast_extra_thresh -
                    ppcs->projected_frame_size;
                rc_param_ptr->vbr_bits_off_target_fast = AOMMIN(
                    rc_param_ptr->vbr_bits_off_target_fast, (4 * rc->avg_frame_bandwidth));

                // Fast adaptation of minQ if necessary to use up the extra bits.
                if (rc->avg_frame_bandwidth) {
                    rc_param_ptr->extend_minq_fast = (int)(rc_param_ptr->vbr_bits_off_target_fast *
                                                           8 / rc->avg_frame_bandwidth);
                }
                rc_param_ptr->extend_minq_fast = AOMMIN(rc_param_ptr->extend_minq_fast,
                                                        minq_adj_limit - rc_param_ptr->extend_minq);
            } else if (rc_param_ptr->vbr_bits_off_target_fast) {
                rc_param_ptr->extend_minq_fast = AOMMIN(rc_param_ptr->extend_minq_fast,
                                                        minq_adj_limit - rc_param_ptr->extend_minq);
            } else {
                rc_param_ptr->extend_minq_fast = 0;
            }
        }
    }
}
void svt_av1_twopass_postencode_update(PictureParentControlSet *ppcs_ptr) {
    SequenceControlSet         *scs_ptr            = ppcs_ptr->scs_ptr;
    EncodeContext              *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const         rc                 = &encode_context_ptr->rc;
    TWO_PASS *const             twopass            = &scs_ptr->twopass;
    const RateControlCfg *const rc_cfg             = &encode_context_ptr->rc_cfg;

    // VBR correction is done through rc->vbr_bits_off_target. Based on the
    // sign of this value, a limited % adjustment is made to the target rate
    // of subsequent frames, to try and push it back towards 0. This method
    // is designed to prevent extreme behaviour at the end of a clip
    // or group of frames.
    rc->vbr_bits_off_target += ppcs_ptr->base_frame_target - ppcs_ptr->projected_frame_size;
    // Target vs actual bits for this arf group.
    int rate_error_estimate_target = 0;
    // Calculate the pct rc error.
    if (rc->total_actual_bits) {
        if (rc->total_target_bits)
            rate_error_estimate_target = (int)((rc->vbr_bits_off_target * 100) /
                                               rc->total_target_bits);
        rc->rate_error_estimate = (int)((rc->vbr_bits_off_target * 100) / rc->total_actual_bits);
        rc->rate_error_estimate = clamp(rc->rate_error_estimate, -100, 100);
    } else {
        rc->rate_error_estimate = 0;
    }

    // Update the active best quality pyramid.
    if (!ppcs_ptr->is_overlay) {
        const int pyramid_level = ppcs_ptr->layer_depth;
        int       i;
        for (i = pyramid_level; i <= MAX_ARF_LAYERS; ++i) {
            rc->active_best_quality[i] = ppcs_ptr->frm_hdr.quantization_params.base_q_idx;
        }
    }

    // If the rate control is drifting consider adjustment to min or maxq.
    if (!ppcs_ptr->is_overlay) {
        const int maxq_adj_limit = rc->worst_quality - rc->active_worst_quality;
        const int minq_adj_limit = MINQ_ADJ_LIMIT;

        // Undershoot.
        if (rc->rate_error_estimate > rc_cfg->under_shoot_pct) {
            --twopass->extend_maxq;
            if (rc->rolling_target_bits >= rc->rolling_actual_bits)
                ++twopass->extend_minq;
            // Overshoot.
        } else if (rc->rate_error_estimate < -rc_cfg->over_shoot_pct) {
            --twopass->extend_minq;
            if (rc->rolling_target_bits < rc->rolling_actual_bits)
                twopass->extend_maxq += (scs_ptr->is_short_clip)
                    ? rate_error_estimate_target < -100 ? 10 : 2
                    : 1;
        } else {
            // Adjustment for extreme local overshoot.
            if (ppcs_ptr->projected_frame_size > (2 * ppcs_ptr->base_frame_target) &&
                ppcs_ptr->projected_frame_size > (2 * rc->avg_frame_bandwidth))
                ++twopass->extend_maxq;

            // Unwind undershoot or overshoot adjustment.
            if (rc->rolling_target_bits < rc->rolling_actual_bits)
                --twopass->extend_minq;
            else if (rc->rolling_target_bits > rc->rolling_actual_bits)
                --twopass->extend_maxq;
        }

        twopass->extend_minq = clamp(twopass->extend_minq, 0, minq_adj_limit);
        if (!scs_ptr->is_short_clip)
            twopass->extend_maxq = clamp(twopass->extend_maxq, 0, maxq_adj_limit);

        // If there is a big and undexpected undershoot then feed the extra
        // bits back in quickly. One situation where this may happen is if a
        // frame is unexpectedly almost perfectly predicted by the ARF or GF
        // but not very well predcited by the previous frame.
        if (!frame_is_kf_gf_arf(ppcs_ptr) && !ppcs_ptr->is_overlay) {
            int fast_extra_thresh = ppcs_ptr->base_frame_target / HIGH_UNDERSHOOT_RATIO;
            if (ppcs_ptr->projected_frame_size < fast_extra_thresh && rc->rate_error_estimate > 0) {
                rc->vbr_bits_off_target_fast += fast_extra_thresh - ppcs_ptr->projected_frame_size;
                rc->vbr_bits_off_target_fast = AOMMIN(rc->vbr_bits_off_target_fast,
                                                      (4 * rc->avg_frame_bandwidth));

                // Fast adaptation of minQ if necessary to use up the extra bits.
                if (rc->avg_frame_bandwidth) {
                    twopass->extend_minq_fast = (int)(rc->vbr_bits_off_target_fast * 8 /
                                                      rc->avg_frame_bandwidth);
                }
                twopass->extend_minq_fast = AOMMIN(twopass->extend_minq_fast,
                                                   minq_adj_limit - twopass->extend_minq);
            } else if (rc->vbr_bits_off_target_fast) {
                twopass->extend_minq_fast = AOMMIN(twopass->extend_minq_fast,
                                                   minq_adj_limit - twopass->extend_minq);
            } else {
                twopass->extend_minq_fast = 0;
            }
        }
    }
}
int gf_high_tpl_la = 2400;
int gf_low_tpl_la  = 300;
int kf_high        = 5000;
int kf_low         = 400;
/******************************************************
 * crf_assign_max_rate
 * Assign the max frame size for capped VBR in base layer frames
 * Update the qindex and active worse quality based on the already
 *  spent bits in the sliding window
 ******************************************************/
void crf_assign_max_rate(PictureParentControlSet *ppcs_ptr) {
    SequenceControlSet *scs_ptr            = ppcs_ptr->scs_ptr;
    EncodeContext      *encode_context_ptr = scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc                 = &encode_context_ptr->rc;

    uint32_t frame_rate    = ((scs_ptr->frame_rate + (1 << (RC_PRECISION - 1))) >> RC_PRECISION);
    int      frames_in_sw  = (int)rc->rate_average_periodin_frames;
    int64_t  spent_bits_sw = 0, available_bit_sw;
    int      coded_frames_num_sw = 0;
    // Find the start and the end of the sliding window
    int32_t start_index = ((ppcs_ptr->picture_number / frames_in_sw) * frames_in_sw) %
        CODED_FRAMES_STAT_QUEUE_MAX_DEPTH;
    int32_t end_index   = start_index + frames_in_sw;
    frames_in_sw        = (scs_ptr->passes > 1)
               ? MIN(end_index, (int32_t)scs_ptr->twopass.stats_buf_ctx->total_stats->count) - start_index
               : frames_in_sw;
    int64_t max_bits_sw = (int64_t)scs_ptr->static_config.max_bit_rate * (int32_t)frames_in_sw /
        frame_rate;
    max_bits_sw += (max_bits_sw * scs_ptr->static_config.mbr_over_shoot_pct / 100);

    // Loop over the sliding window and calculated the spent bits
    for (int index = start_index; index < end_index; index++) {
        int32_t queue_entry_index                 = (index > CODED_FRAMES_STAT_QUEUE_MAX_DEPTH - 1)
                            ? index - CODED_FRAMES_STAT_QUEUE_MAX_DEPTH
                            : index;
        coded_frames_stats_entry *queue_entry_ptr = rc->coded_frames_stat_queue[queue_entry_index];
        spent_bits_sw += (queue_entry_ptr->frame_total_bit_actual > 0)
            ? queue_entry_ptr->frame_total_bit_actual
            : 0;
        coded_frames_num_sw += (queue_entry_ptr->frame_total_bit_actual > 0) ? 1 : 0;
    }
    available_bit_sw       = MAX(max_bits_sw - spent_bits_sw, 0);
    int64_t max_frame_size = 0;
    // Based on the kf boost, calculate the frame size for I frames
    if (ppcs_ptr->slice_type == I_SLICE) {
        int kf_interval = scs_ptr->static_config.intra_period_length > 0
            ? MIN(frames_in_sw, scs_ptr->static_config.intra_period_length + 1)
            : frames_in_sw;
        max_frame_size  = calculate_boost_bits(kf_interval, rc->kf_boost, available_bit_sw);
        int kf_low_thr  = kf_low + (kf_high - kf_low) / 3;
        if (rc->kf_boost > kf_low_thr)
            max_frame_size = max_frame_size * 14 / 10;
#if DEBUG_RC_CAP_LOG
        printf("SW_POC:%lld\t%lld\t%lld\t%d\tboost:%d\n",
               ppcs_ptr->picture_number,
               max_bits_sw,
               available_bit_sw,
               max_frame_size,
               rc->kf_boost);
#endif
    }
    // Based on the gfu boost, calculate the frame size for I frames
    else if (ppcs_ptr->temporal_layer_index == 0) {
        int64_t gf_group_bits = available_bit_sw * (int64_t)(1 << ppcs_ptr->hierarchical_levels) /
            (frames_in_sw - coded_frames_num_sw);
        max_frame_size = calculate_boost_bits(
            (1 << ppcs_ptr->hierarchical_levels), rc->gfu_boost, gf_group_bits);
        int gfu_low_thr = gf_low_tpl_la + (gf_high_tpl_la - gf_low_tpl_la) / 3;
        if (rc->gfu_boost > gfu_low_thr)
            max_frame_size = max_frame_size * 12 / 10;
#if DEBUG_RC_CAP_LOG
        printf("SW_POC:%lld\t%lld\t%lld\t%d\tboost:%d\n",
               ppcs_ptr->picture_number,
               gf_group_bits,
               available_bit_sw,
               max_frame_size,
               rc->gfu_boost);
#endif
    }
    FrameHeader *frm_hdr    = &ppcs_ptr->frm_hdr;
    int32_t      new_qindex = frm_hdr->quantization_params.base_q_idx;
    // Increase the qindex based on the status of the spent bits in the window
    int remaining_frames       = frames_in_sw - coded_frames_num_sw;
    int available_bit_ratio    = (int)(100 * available_bit_sw / max_bits_sw);
    int available_frames_ratio = 100 * remaining_frames / frames_in_sw;
    int buff_lvl_step          = (OPTIMAL_BUFFER_LEVEL - CRITICAL_BUFFER_LEVEL);
    int adjustment             = 0;
    if (available_bit_ratio <= OPTIMAL_BUFFER_LEVEL) {
        if (available_bit_ratio > CRITICAL_BUFFER_LEVEL) {
            int max_adjustment = (available_bit_ratio + 20 < available_frames_ratio)
                ? rc->active_worst_quality
                : rc->active_worst_quality / 2;
            // Adjust up from assigned QP.
            if (buff_lvl_step && available_bit_ratio < available_frames_ratio + 10) {
                adjustment = (int)(max_adjustment * (OPTIMAL_BUFFER_LEVEL - available_bit_ratio) /
                                   buff_lvl_step);
            }
        } else {
            // Set to worst_quality if buffer is below critical level.
            adjustment = rc->active_worst_quality;
        }
    }
#if DEBUG_RC_CAP_LOG
    printf("SW_POC:%lld\t%lld\t%lld\t%d%%\t%d%%\tadj:\t%d\n",
           ppcs_ptr->picture_number,
           max_bits_sw,
           available_bit_sw,
           available_bit_ratio,
           available_frames_ratio,
           adjustment);
#endif
    new_qindex += adjustment;
    // Increase the active_worse_quality based on the adjustment
    if (ppcs_ptr->temporal_layer_index == 0)
        rc->active_worst_quality += (adjustment / 2);
    // Decrease the active_worse_quality where undershoot happens and active_worst_quality is greater than the input QP
    if (available_bit_ratio > available_frames_ratio + 20 && available_frames_ratio < 10 &&
        rc->active_worst_quality > quantizer_to_qindex[(uint8_t)scs_ptr->static_config.qp]) {
        rc->active_worst_quality -= rc->active_worst_quality / 10;
    }
    rc->active_worst_quality = CLIP3(
        (int32_t)quantizer_to_qindex[(uint8_t)scs_ptr->static_config.qp],
        (int32_t)quantizer_to_qindex[scs_ptr->static_config.max_qp_allowed],
        rc->active_worst_quality);

    frm_hdr->quantization_params.base_q_idx = (uint8_t)CLIP3(
        (int32_t)quantizer_to_qindex[scs_ptr->static_config.min_qp_allowed],
        (int32_t)quantizer_to_qindex[scs_ptr->static_config.max_qp_allowed],
        (int32_t)(new_qindex));

    ppcs_ptr->picture_qp = (uint8_t)CLIP3((int32_t)scs_ptr->static_config.min_qp_allowed,
                                          (int32_t)scs_ptr->static_config.max_qp_allowed,
                                          (frm_hdr->quantization_params.base_q_idx + 2) >> 2);
    // clip the max frame size to 32 bits
    ppcs_ptr->max_frame_size = (int)CLIP3(1, (0xFFFFFFFFull >> 1), (uint64_t)max_frame_size);
    // The target is set to 80% of the max.
    ppcs_ptr->this_frame_target = ppcs_ptr->max_frame_size * 8 / 10;
}

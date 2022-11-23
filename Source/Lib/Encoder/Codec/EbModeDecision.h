/*
* Copyright(c) 2019 Intel Corporation
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#ifndef EbModeDecision_h
#define EbModeDecision_h

#include "EbDefinitions.h"
#include "EbUtility.h"
#include "EbPictureControlSet.h"
#include "EbCodingUnit.h"
#include "EbPredictionUnit.h"
#include "EbSyntaxElements.h"
#include "EbPictureBufferDesc.h"
#include "EbAdaptiveMotionVectorPrediction.h"
#include "EbPictureOperators.h"
#include "EbNeighborArrays.h"
#include "EbObject.h"

#ifdef __cplusplus
extern "C" {
#endif
#define ENABLE_AMVP_MV_FOR_RC_PU 0
#define MAX_MB_PLANE 3
#define MAX_MPM_CANDIDATES 3
#define MERGE_PENALTY 10

// Create incomplete struct definition for the following function pointer typedefs
struct ModeDecisionCandidateBuffer;
struct ModeDecisionContext;

/**************************************
    * Mode Decision Candidate
    **************************************/
typedef struct ModeDecisionCandidate {
    Mv                     mv[MAX_NUM_OF_REF_PIC_LIST];
    Mv                     pred_mv[MAX_NUM_OF_REF_PIC_LIST];
    PaletteInfo           *palette_info;
    uint32_t               interp_filters;
    EbWarpedMotionParams   wm_params_l0;
    EbWarpedMotionParams   wm_params_l1;
    InterInterCompoundData interinter_comp;
    TxType                 transform_type[MAX_TXB_COUNT];
    TxType                 transform_type_uv;
    uint16_t               num_proj_ref;
    uint8_t                palette_size[2];

    CandClass      cand_class;
    PredictionMode pred_mode;
    uint8_t skip_mode; // skip mode_info + coeff. as defined in section 6.10.10 of the av1 text
    Bool    skip_mode_allowed;
    uint8_t use_intrabc;

    // Intra Mode
    int8_t           angle_delta[PLANE_TYPES]; // [-3,3]
    uint8_t          filter_intra_mode;
    UvPredictionMode intra_chroma_mode; // INTRA only
    uint8_t          cfl_alpha_idx; // Index of the alpha Cb and alpha Cr combination
    uint8_t          cfl_alpha_signs; // Joint sign of alpha Cb and alpha Cr

    // Inter Mode
    uint8_t        ref_frame_type;
    uint8_t        drl_index;
    MotionMode     motion_mode;
    uint8_t        tx_depth;
    uint8_t        compound_idx;
    uint8_t        comp_group_idx;
    InterIntraMode interintra_mode;
    uint8_t        is_interintra_used;
    uint8_t        use_wedge_interintra;
    int8_t         interintra_wedge_index;
} ModeDecisionCandidate;

/**************************************
    * Function Ptrs Definitions
    **************************************/
typedef EbErrorType (*EbPredictionFunc)(uint8_t                             hbd_mode_decision,
                                        struct ModeDecisionContext         *context_ptr,
                                        PictureControlSet                  *pcs_ptr,
                                        struct ModeDecisionCandidateBuffer *candidate_buffer_ptr);
typedef uint64_t (*EbFastCostFunc)(struct ModeDecisionContext *context_ptr, BlkStruct *blk_ptr,
                                   struct ModeDecisionCandidateBuffer *candidate_buffer,
                                   uint32_t qp, uint64_t luma_distortion,
                                   uint64_t chroma_distortion, uint64_t lambda,
                                   PictureControlSet *pcs_ptr, CandidateMv *ref_mv_stack,
                                   const BlockGeom *blk_geom, uint32_t miRow, uint32_t miCol,
                                   uint8_t enable_inter_intra, uint32_t left_neighbor_mode,
                                   uint32_t top_neighbor_mode);
typedef EbErrorType (*EbAv1FullCostFunc)(
    PictureControlSet *pcs_ptr, struct ModeDecisionContext *context_ptr,
    struct ModeDecisionCandidateBuffer *candidate_buffer_ptr, BlkStruct *blk_ptr,
    uint64_t *y_distortion, uint64_t *cb_distortion, uint64_t *cr_distortion, uint64_t lambda,
    uint64_t *y_coeff_bits, uint64_t *cb_coeff_bits, uint64_t *cr_coeff_bits, BlockSize bsize);
/**************************************
    * Mode Decision Candidate Buffer
    **************************************/
typedef struct IntraChromacandidate_buffer {
    uint32_t             mode;
    uint64_t             cost;
    uint64_t             distortion;
    EbPictureBufferDesc *prediction_ptr;
    EbPictureBufferDesc *residual_ptr;
} IntraChromacandidate_buffer;

/**************************************
    * Mode Decision Candidate Buffer
    **************************************/
typedef struct ModeDecisionCandidateBuffer {
    EbDctor dctor;
    // Candidate Ptr
    ModeDecisionCandidate *candidate_ptr;

    // Video Buffers
    EbPictureBufferDesc *prediction_ptr;
    EbPictureBufferDesc *recon_coeff_ptr;
    EbPictureBufferDesc *residual_ptr;
    EbPictureBufferDesc *quant_coeff_ptr;

    // *Note - We should be able to combine the recon_coeff_ptr & recon_ptr pictures (they aren't needed at the same time)
    EbPictureBufferDesc *recon_ptr;

    // Costs
    uint64_t *fast_cost_ptr;
    uint64_t *full_cost_ptr;
    uint64_t  fast_luma_rate;
    uint64_t  fast_chroma_rate;
    uint64_t  total_rate;
    uint32_t  luma_fast_distortion;
    uint32_t  full_distortion;
    uint16_t  count_non_zero_coeffs;
    uint16_t  eob[MAX_MB_PLANE][MAX_TXB_COUNT];
    int32_t   quantized_dc[MAX_MB_PLANE][MAX_TXB_COUNT];
    uint8_t   block_has_coeff;
    uint8_t   u_has_coeff;
    uint8_t   v_has_coeff;
    uint16_t  y_has_coeff;
} ModeDecisionCandidateBuffer;

/**************************************
    * Extern Function Declarations
    **************************************/
extern EbErrorType mode_decision_candidate_buffer_ctor(
    ModeDecisionCandidateBuffer *buffer_ptr, EbBitDepth max_bitdepth, uint8_t sb_size,
    uint32_t buffer_mask, EbPictureBufferDesc *temp_residual_ptr,
    EbPictureBufferDesc *temp_recon_ptr, uint64_t *fast_cost_ptr, uint64_t *full_cost_ptr);

extern EbErrorType mode_decision_scratch_candidate_buffer_ctor(
    ModeDecisionCandidateBuffer *buffer_ptr, uint8_t sb_size, EbBitDepth max_bitdepth);

uint32_t product_full_mode_decision_light_pd0(struct ModeDecisionContext   *context_ptr,
                                              BlkStruct                    *blk_ptr,
                                              ModeDecisionCandidateBuffer **buffer_ptr_array);
void     product_full_mode_decision_light_pd1(struct ModeDecisionContext *context_ptr,
                                              BlkStruct *blk_ptr, PictureControlSet *pcs,
                                              uint32_t                     sb_addr,
                                              ModeDecisionCandidateBuffer *candidate_buffer);
uint32_t product_full_mode_decision(struct ModeDecisionContext *context_ptr, BlkStruct *blk_ptr,
                                    PictureControlSet *pcs, uint32_t sb_addr,
                                    ModeDecisionCandidateBuffer **buffer_ptr_array,
                                    uint32_t                      candidate_total_count,
                                    uint32_t                     *best_candidate_index_array);
void     set_tuned_blk_lambda(struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr);

typedef EbErrorType (*EB_INTRA_4x4_FAST_LUMA_COST_FUNC)(
    struct ModeDecisionContext *context_ptr, uint32_t pu_index,
    ModeDecisionCandidateBuffer *candidate_buffer_ptr, uint64_t luma_distortion, uint64_t lambda);

typedef EbErrorType (*EB_INTRA_4x4_FULL_LUMA_COST_FUNC)(
    ModeDecisionCandidateBuffer *candidate_buffer_ptr, uint64_t *y_distortion, uint64_t lambda,
    uint64_t *y_coeff_bits, uint32_t transform_size);

typedef EbErrorType (*EB_FULL_NXN_COST_FUNC)(PictureControlSet           *pcs_ptr,
                                             ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                             uint32_t qp, uint64_t *y_distortion,
                                             uint64_t *cb_distortion, uint64_t *cr_distortion,
                                             uint64_t lambda, uint64_t lambda_chroma,
                                             uint64_t *y_coeff_bits, uint64_t *cb_coeff_bits,
                                             uint64_t *cr_coeff_bits, uint32_t transform_size);
struct CodingLoopContext_s;
/*
      |-------------------------------------------------------------|
      | ref_idx          0            1           2            3       |
      | List0            LAST        LAST2        LAST3        GOLD    |
      | List1            BWD            ALT2            ALT                |
      |-------------------------------------------------------------|
    */
#define INVALID_REF 0xF
uint8_t                 get_ref_frame_idx(uint8_t ref_type);
extern MvReferenceFrame svt_get_ref_frame_type(uint8_t list, uint8_t ref_idx);
uint8_t                 get_list_idx(uint8_t ref_type);
int                     svt_aom_get_sad_per_bit(int qidx, EbBitDepth is_hbd);
#ifdef __cplusplus
}
#endif
#endif // EbModeDecision_h

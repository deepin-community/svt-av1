#
# Copyright(c) 2019 Intel Corporation
#
# This source code is subject to the terms of the BSD 2 Clause License and
# the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
# was not distributed with this source code in the LICENSE file, you can
# obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
# Media Patent License 1.0 was not distributed with this source code in the
# PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
#


# C_DEFAULT Directory CMakeLists.txt

# Include Encoder Subdirectories
include_directories(../../../API
        ../../Encoder/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/C_DEFAULT/)

set(all_files
    EbComputeSAD_C.c
    EbComputeSAD_C.h
    EncodeTxbRef_C.c
    EncodeTxbRef_C.h
    sad_av1.c
    variance.c
    )

add_library(ENCODER_C_DEFAULT OBJECT ${all_files})

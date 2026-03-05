#pragma once

namespace stage0::config {

inline constexpr int kModelBatchSize = 32; // model batch size (large model on CUDA can OOM with 128)
inline constexpr int kModelSeqLen = 400;    // model sequence length
inline constexpr int kLabelSeqLen = 64;     // label sequence length

} // namespace stage0::config

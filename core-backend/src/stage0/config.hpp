#pragma once

namespace stage0::config {

inline constexpr int kModelBatchSize = 512; // model batch size
inline constexpr int kModelSeqLen = 50;     // model sequence length (TODO: input is truncated for performance, set too 400 when have GPU)
inline constexpr int kLabelSeqLen = 32;     // label sequence length

} // namespace stage0::config

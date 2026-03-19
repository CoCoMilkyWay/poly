#pragma once

#include <cstdint>

namespace tracker {

// ============================================================================
// Contract Addresses (Polygon Mainnet)
// ============================================================================

inline constexpr const char *kZeroAddress          = "0x0000000000000000000000000000000000000000";
inline constexpr const char *kConditionalTokens    = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
inline constexpr const char *kCtfExchange          = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
inline constexpr const char *kNegRiskCtfExchange   = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
inline constexpr const char *kNegRiskAdapter       = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
inline constexpr const char *kUsdcE                = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174";
inline constexpr const char *kWrappedCollateral    = "0x3a3bd7bb9528e159577f7c2e685cc81a765002e2";

// ============================================================================
// Event Topics
// ============================================================================

inline constexpr const char *kTransferSingleTopic   = "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62";
inline constexpr const char *kTransferBatchTopic    = "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb";
inline constexpr const char *kConditionResolveTopic = "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894";
inline constexpr const char *kPositionSplitTopic    = "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298";
inline constexpr const char *kPositionMergeTopic    = "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca";
inline constexpr const char *kPositionRedeemTopic   = "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d";
inline constexpr const char *kOrderFillTopic        = "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6";
inline constexpr const char *kTokenRegisterTopic    = "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d";
inline constexpr const char *kPositionConvertTopic  = "0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e";

// ============================================================================
// External APIs
// ============================================================================

inline constexpr const char *kGammaApiBase           = "https://gamma-api.polymarket.com";
inline constexpr const char *kPolymarketSubgraphId   = "81Dm16JjuFSrqz813HysXoUPvzTwE7fsfPk2RTf66nyC";
inline constexpr const char *kPnlSubgraphId          = "6c58N5U4MtQE2Y8njfVrrAfRykzfqajMGeTMEvMmskVz";

// ============================================================================
// Numeric Constants
// ============================================================================

inline constexpr int64_t     kTransferFlatLogScale = 10'000;
inline constexpr long double kUnit                 = 1'000'000.0L;

} // namespace tracker

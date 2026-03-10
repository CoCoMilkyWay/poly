#include "stage0_tag_internal.hpp"
#include "../core/mem.hpp"
#include "../core/mem_json.hpp"

namespace stage0 {

int64_t detail::WordPieceTokenizer::estimated_bytes() const {
  return core::mem::estimate_unordered_map(
      vocab_, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
      [](const int &) { return int64_t{0}; });
}

int64_t detail::OnnxEmbedder::estimated_bytes() const {
  int64_t bytes = 0;
  bytes += core::mem::estimate_vector_plain(input_names_storage_);
  bytes += core::mem::estimate_vector_plain(output_names_storage_);
  bytes += core::mem::estimate_vector_plain(input_names_);
  bytes += core::mem::estimate_vector_plain(output_names_);
  bytes += core::mem::estimate_string_extra(device_name_);
  bytes += allocator_in_use_bytes();
  return bytes;
}

int64_t detail::OnnxEmbedder::allocator_in_use_bytes() const {
  assert(session_ != nullptr);
  const Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const Ort::Allocator alloc(*session_, mem_info);
  const auto stats = alloc.GetStats().GetKeyValuePairs();
  const auto it = stats.find("InUse");
  assert(it != stats.end());
  return std::stoll(it->second);
}

json Tagger::memory_breakdown() const {
  int64_t labels_bytes = core::mem::estimate_vector(impl_->labels, [](const TagLabel &x) {
    return core::mem::estimate_string_extra(x.full_name) + core::mem::estimate_string_extra(x.short_name) +
           core::mem::estimate_string_extra(x.embed_text);
  });
  int64_t embeddings_bytes = core::mem::estimate_vector_plain(impl_->label_embeddings_flat);
  int64_t tokenizer_bytes = impl_->tokenizer.estimated_bytes();
  int64_t embedder_bytes = impl_->embedder.estimated_bytes();

  core::mem::MemRows rows = {
      {"label_embeddings_flat", embeddings_bytes},
      {"labels", labels_bytes},
      {"tokenizer_vocab", tokenizer_bytes},
      {"embedder_runtime", embedder_bytes},
  };
  core::mem::sort_mem_rows_desc(rows);
  return core::mem::build_memory_breakdown_json(rows, labels_bytes + embeddings_bytes + tokenizer_bytes + embedder_bytes);
}

} // namespace stage0

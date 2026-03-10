#pragma once

#include "config.hpp"
#include "stage0_tag.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "onnxruntime/onnxruntime_cxx_api.h"

namespace stage0::detail {

class WordPieceTokenizer {
public:
  static constexpr int kPadId = 0;
  static constexpr int kUnkId = 100;
  static constexpr int kClsId = 101;
  static constexpr int kSepId = 102;
  static constexpr int kMaxSeqLen = config::kModelSeqLen;

  struct TokenIds {
    std::vector<int64_t> input_ids;
    std::vector<int64_t> attention_mask;
    std::vector<int64_t> token_type_ids;
  };

  explicit WordPieceTokenizer(const std::string &vocab_path);
  TokenIds encode(const std::string &text, int max_len = config::kModelSeqLen) const;
  int64_t estimated_bytes() const;

private:
  std::vector<int> tokenize(const std::string &text) const;
  std::vector<std::string> basic_tokenize(const std::string &text) const;
  std::vector<int> wordpiece_tokenize(const std::string &word) const;

  std::unordered_map<std::string, int> vocab_;
};

class OnnxEmbedder {
public:
  explicit OnnxEmbedder(const std::string &model_path);
  std::vector<std::vector<float>> embed(const std::vector<WordPieceTokenizer::TokenIds> &batch) const;
  const std::string &device_name() const;
  int64_t estimated_bytes() const;

private:
  Ort::Env env_;
  std::unique_ptr<Ort::Session> session_;
  std::vector<Ort::AllocatedStringPtr> input_names_storage_;
  std::vector<Ort::AllocatedStringPtr> output_names_storage_;
  std::vector<const char *> input_names_;
  std::vector<const char *> output_names_;
  std::string device_name_;
};

} // namespace stage0::detail

namespace stage0 {

struct Tagger::Impl {
  detail::WordPieceTokenizer tokenizer;
  detail::OnnxEmbedder embedder;
  std::vector<TagLabel> labels;
  std::vector<float> label_embeddings_flat;
  size_t hidden_dim = 0;

  Impl(const std::string &model_dir, const std::string &tag_md_path);
};

} // namespace stage0

#include "stage0_tag.hpp"

#include <cassert>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "onnxruntime/onnxruntime_cxx_api.h"

namespace stage0 {
namespace {

// ============================================================================
// WordPiece Tokenizer (BERT-style)
// ============================================================================

class WordPieceTokenizer {
public:
  static constexpr int kPadId = 0;
  static constexpr int kUnkId = 100;
  static constexpr int kClsId = 101;
  static constexpr int kSepId = 102;
  static constexpr int kMaxSeqLen = 512;

  explicit WordPieceTokenizer(const std::string &vocab_path) {
    std::ifstream f(vocab_path);
    assert(f.is_open());
    std::string line;
    int idx = 0;
    while (std::getline(f, line)) {
      vocab_[line] = idx++;
    }
    assert(!vocab_.empty());
  }

  struct TokenIds {
    std::vector<int64_t> input_ids;
    std::vector<int64_t> attention_mask;
    std::vector<int64_t> token_type_ids;
  };

  TokenIds encode(const std::string &text, int max_len = 128) const {
    std::vector<int> tokens = tokenize(text);
    // Truncate to max_len - 2 (for [CLS] and [SEP])
    if (static_cast<int>(tokens.size()) > max_len - 2) {
      tokens.resize(static_cast<size_t>(max_len - 2));
    }

    TokenIds result;
    result.input_ids.reserve(static_cast<size_t>(max_len));
    result.attention_mask.reserve(static_cast<size_t>(max_len));
    result.token_type_ids.reserve(static_cast<size_t>(max_len));

    // [CLS] + tokens + [SEP]
    result.input_ids.push_back(kClsId);
    for (int t : tokens) {
      result.input_ids.push_back(t);
    }
    result.input_ids.push_back(kSepId);

    size_t seq_len = result.input_ids.size();
    for (size_t i = 0; i < seq_len; ++i) {
      result.attention_mask.push_back(1);
      result.token_type_ids.push_back(0);
    }

    // Pad to max_len
    while (result.input_ids.size() < static_cast<size_t>(max_len)) {
      result.input_ids.push_back(kPadId);
      result.attention_mask.push_back(0);
      result.token_type_ids.push_back(0);
    }

    return result;
  }

private:
  std::vector<int> tokenize(const std::string &text) const {
    std::vector<std::string> words = basic_tokenize(text);
    std::vector<int> tokens;
    for (const auto &word : words) {
      auto sub_tokens = wordpiece_tokenize(word);
      for (int t : sub_tokens) {
        tokens.push_back(t);
      }
    }
    return tokens;
  }

  std::vector<std::string> basic_tokenize(const std::string &text) const {
    // Lowercase and split by whitespace/punctuation
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
      unsigned char uc = static_cast<unsigned char>(c);
      if (std::isspace(uc)) {
        if (!current.empty()) {
          words.push_back(current);
          current.clear();
        }
      } else if (std::ispunct(uc)) {
        if (!current.empty()) {
          words.push_back(current);
          current.clear();
        }
        words.push_back(std::string(1, c));
      } else {
        current += static_cast<char>(std::tolower(uc));
      }
    }
    if (!current.empty()) {
      words.push_back(current);
    }
    return words;
  }

  std::vector<int> wordpiece_tokenize(const std::string &word) const {
    std::vector<int> tokens;
    size_t start = 0;
    while (start < word.size()) {
      size_t end = word.size();
      int cur_token = kUnkId;
      while (start < end) {
        std::string substr = word.substr(start, end - start);
        if (start > 0) {
          substr = "##" + substr;
        }
        auto it = vocab_.find(substr);
        if (it != vocab_.end()) {
          cur_token = it->second;
          break;
        }
        --end;
      }
      if (cur_token == kUnkId) {
        tokens.push_back(kUnkId);
        break;
      }
      tokens.push_back(cur_token);
      start = end;
    }
    return tokens;
  }

  std::unordered_map<std::string, int> vocab_;
};

// ============================================================================
// TAG.md Parser
// ============================================================================

std::vector<TagLabel> parse_tag_md(const std::string &path) {
  std::ifstream f(path);
  assert(f.is_open());

  std::vector<TagLabel> labels;
  std::string level1;
  std::string line;

  while (std::getline(f, line)) {
    // Trim
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
      continue;
    }
    line = line.substr(start);

    if (line.starts_with("## ")) {
      level1 = line.substr(3);
      // Trim level1
      size_t end = level1.find_last_not_of(" \t\r\n");
      if (end != std::string::npos) {
        level1 = level1.substr(0, end + 1);
      }
      continue;
    }

    if (line.starts_with("- ")) {
      std::string rest = line.substr(2);
      size_t hash_pos = rest.find('#');
      std::string level2 = rest.substr(0, hash_pos);
      std::string comment;
      if (hash_pos != std::string::npos) {
        comment = rest.substr(hash_pos + 1);
      }

      // Trim level2
      size_t end = level2.find_last_not_of(" \t\r\n");
      if (end != std::string::npos) {
        level2 = level2.substr(0, end + 1);
      }
      // Trim comment
      start = comment.find_first_not_of(" \t\r\n");
      if (start != std::string::npos) {
        comment = comment.substr(start);
        end = comment.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
          comment = comment.substr(0, end + 1);
        }
      } else {
        comment.clear();
      }

      assert(!level1.empty());
      assert(!level2.empty());

      TagLabel label;
      label.full_name = level1 + " - " + level2;
      label.short_name = level2;
      label.embed_text = level2 + " " + comment;
      labels.push_back(std::move(label));
    }
  }

  assert(!labels.empty());
  return labels;
}

// ============================================================================
// ONNX Inference
// ============================================================================

class OnnxEmbedder {
public:
  explicit OnnxEmbedder(const std::string &model_path)
      : env_(ORT_LOGGING_LEVEL_WARNING, "bge_tagger") {
    Ort::SessionOptions opts;
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.SetIntraOpNumThreads(4);
    opts.SetInterOpNumThreads(1);
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.EnableCpuMemArena();

    session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), opts);
    assert(session_->GetInputCount() == 3);
    assert(session_->GetOutputCount() == 1);

    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < 3; ++i) {
      auto name = session_->GetInputNameAllocated(i, alloc);
      input_names_storage_.push_back(std::move(name));
      input_names_.push_back(input_names_storage_.back().get());
    }
    auto out_name = session_->GetOutputNameAllocated(0, alloc);
    output_names_storage_.push_back(std::move(out_name));
    output_names_.push_back(output_names_storage_.back().get());
  }

  // Returns [batch_size, hidden_dim] embeddings (L2 normalized)
  std::vector<std::vector<float>> embed(const std::vector<WordPieceTokenizer::TokenIds> &batch) const {
    if (batch.empty()) {
      return {};
    }

    int64_t batch_size = static_cast<int64_t>(batch.size());
    int64_t seq_len = static_cast<int64_t>(batch[0].input_ids.size());

    // Flatten inputs
    std::vector<int64_t> input_ids_flat;
    std::vector<int64_t> attention_mask_flat;
    std::vector<int64_t> token_type_ids_flat;
    input_ids_flat.reserve(static_cast<size_t>(batch_size * seq_len));
    attention_mask_flat.reserve(static_cast<size_t>(batch_size * seq_len));
    token_type_ids_flat.reserve(static_cast<size_t>(batch_size * seq_len));

    for (const auto &item : batch) {
      assert(static_cast<int64_t>(item.input_ids.size()) == seq_len);
      for (auto v : item.input_ids) {
        input_ids_flat.push_back(v);
      }
      for (auto v : item.attention_mask) {
        attention_mask_flat.push_back(v);
      }
      for (auto v : item.token_type_ids) {
        token_type_ids_flat.push_back(v);
      }
    }

    std::array<int64_t, 2> shape = {batch_size, seq_len};
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> inputs;
    inputs.reserve(3);
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        mem_info, input_ids_flat.data(), input_ids_flat.size(), shape.data(), shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        mem_info, attention_mask_flat.data(), attention_mask_flat.size(), shape.data(), shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        mem_info, token_type_ids_flat.data(), token_type_ids_flat.size(), shape.data(), shape.size()));

    auto outputs = session_->Run(Ort::RunOptions{nullptr}, input_names_.data(), inputs.data(), inputs.size(),
                                 output_names_.data(), output_names_.size());
    assert(outputs.size() == 1);
    assert(outputs[0].IsTensor());

    auto shape_info = outputs[0].GetTensorTypeAndShapeInfo();
    auto out_shape = shape_info.GetShape();
    // [batch_size, seq_len, hidden_dim]
    assert(out_shape.size() == 3);
    assert(out_shape[0] == batch_size);
    int64_t hidden_dim = out_shape[2];

    const float *data = outputs[0].GetTensorData<float>();

    // Mean pooling + L2 normalization
    std::vector<std::vector<float>> result(static_cast<size_t>(batch_size));
    for (size_t b = 0; b < static_cast<size_t>(batch_size); ++b) {
      std::vector<float> pooled(static_cast<size_t>(hidden_dim), 0.0f);
      float mask_sum = 0.0f;

      for (size_t s = 0; s < static_cast<size_t>(seq_len); ++s) {
        float mask = static_cast<float>(batch[b].attention_mask[s]);
        mask_sum += mask;
        size_t offset = (b * static_cast<size_t>(seq_len) + s) * static_cast<size_t>(hidden_dim);
        for (size_t h = 0; h < static_cast<size_t>(hidden_dim); ++h) {
          pooled[h] += data[offset + h] * mask;
        }
      }

      // Divide by mask sum
      if (mask_sum > 0.0f) {
        for (auto &v : pooled) {
          v /= mask_sum;
        }
      }

      // L2 normalize
      float norm = 0.0f;
      for (auto v : pooled) {
        norm += v * v;
      }
      norm = std::sqrt(norm);
      if (norm > 1e-12f) {
        for (auto &v : pooled) {
          v /= norm;
        }
      }

      result[b] = std::move(pooled);
    }

    return result;
  }

private:
  Ort::Env env_;
  std::unique_ptr<Ort::Session> session_;
  std::vector<Ort::AllocatedStringPtr> input_names_storage_;
  std::vector<Ort::AllocatedStringPtr> output_names_storage_;
  std::vector<const char *> input_names_;
  std::vector<const char *> output_names_;
};

} // namespace

// ============================================================================
// Tagger Implementation
// ============================================================================

struct Tagger::Impl {
  WordPieceTokenizer tokenizer;
  OnnxEmbedder embedder;
  std::vector<TagLabel> labels;
  std::vector<std::vector<float>> label_embeddings;

  Impl(const std::string &model_dir, const std::string &tag_md_path)
      : tokenizer(model_dir + "/vocab.txt"),
        embedder(model_dir + "/model.onnx"),
        labels(parse_tag_md(tag_md_path)) {
    // Pre-compute label embeddings
    std::vector<WordPieceTokenizer::TokenIds> label_tokens;
    label_tokens.reserve(labels.size());
    for (const auto &label : labels) {
      label_tokens.push_back(tokenizer.encode(label.embed_text, 64));
    }
    label_embeddings = embedder.embed(label_tokens);
    assert(label_embeddings.size() == labels.size());
    std::cout << "[Tagger] Loaded " << labels.size() << " labels, dim=" << label_embeddings[0].size() << std::endl;
  }
};

Tagger::Tagger(const std::string &model_dir, const std::string &tag_md_path)
    : impl_(std::make_unique<Impl>(model_dir, tag_md_path)) {}

Tagger::~Tagger() = default;

TagResult Tagger::tag(const std::string &question) const {
  auto results = tag_batch({question});
  assert(results.size() == 1);
  return results[0];
}

std::vector<TagResult> Tagger::tag_batch(const std::vector<std::string> &questions) const {
  if (questions.empty()) {
    return {};
  }

  std::vector<WordPieceTokenizer::TokenIds> tokens;
  tokens.reserve(questions.size());
  for (const auto &q : questions) {
    tokens.push_back(impl_->tokenizer.encode(q, 128));
  }

  auto embeddings = impl_->embedder.embed(tokens);
  assert(embeddings.size() == questions.size());

  std::vector<TagResult> results;
  results.reserve(questions.size());

  for (const auto &emb : embeddings) {
    float best_score = -1.0f;
    size_t best_idx = 0;

    for (size_t i = 0; i < impl_->label_embeddings.size(); ++i) {
      float score = 0.0f;
      for (size_t h = 0; h < emb.size(); ++h) {
        score += emb[h] * impl_->label_embeddings[i][h];
      }
      if (score > best_score) {
        best_score = score;
        best_idx = i;
      }
    }

    TagResult r;
    r.tag_name = impl_->labels[best_idx].short_name;
    r.confidence = best_score;
    results.push_back(std::move(r));
  }

  return results;
}

size_t Tagger::label_count() const {
  return impl_->labels.size();
}

} // namespace stage0

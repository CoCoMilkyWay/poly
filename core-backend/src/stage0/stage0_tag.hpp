#pragma once

#include <memory>
#include <string>
#include <vector>

namespace stage0 {

struct TaggerModelInfo {
  std::string model_path;
  std::string model_size_text;
};

TaggerModelInfo detect_tagger_model_info(const std::string &model_dir);

struct TagLabel {
  std::string full_name;  // "Crypto_Price - Crypto_Price_Bitcoin"
  std::string short_name; // "Crypto_Price_Bitcoin"
  std::string embed_text; // "Crypto_Price_Bitcoin Bitcoin BTC price..."
};

struct TagResult {
  std::string tag_name;
  float confidence;
};

class Tagger {
public:
  explicit Tagger(const std::string &model_dir, const std::string &tag_md_path);
  ~Tagger();

  Tagger(const Tagger &) = delete;
  Tagger &operator=(const Tagger &) = delete;

  TagResult tag(const std::string &question) const;
  std::vector<TagResult> tag_batch(const std::vector<std::string> &questions) const;

  size_t label_count() const;
  const std::string &device_name() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace stage0

#include "stage3.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace stage3 {

namespace {

constexpr uint64_t STORE_MAGIC = 0x0000334547415453ULL; // "STAGE3\0\0"
constexpr uint64_t STORE_VERSION = 1;

constexpr size_t STORE_HEADER_OFFSET = 0;
constexpr size_t STORE_CONDITIONS_OFFSET = sizeof(StoreHeader);
constexpr size_t STORE_TOKENS_OFFSET = STORE_CONDITIONS_OFFSET + sizeof(ConditionMeta) * MAX_CONDITIONS;
constexpr size_t STORE_FEATURES_OFFSET = STORE_TOKENS_OFFSET + sizeof(TokenSlot) * MAX_TOKENS;
constexpr size_t STORE_SHARPE_AGG_OFFSET = STORE_FEATURES_OFFSET + sizeof(FeatureSlot) * MAX_FEATURES;
constexpr size_t STORE_SHARPE_SAMPLE_OFFSET = STORE_SHARPE_AGG_OFFSET + sizeof(SharpeAgg) * MAX_SHARPE_AGGS;
constexpr size_t STORE_USERS_OFFSET = STORE_SHARPE_SAMPLE_OFFSET + sizeof(SharpeSample) * MAX_SHARPE_SAMPLES;
constexpr size_t STORE_TOTAL_SIZE = STORE_USERS_OFFSET + sizeof(UserBlock) * MAX_USERS;

constexpr size_t INDEX_TOTAL_SIZE = sizeof(UserIndexEntry) * USER_INDEX_SLOT_COUNT;

constexpr size_t EVENTS_LOG_INITIAL_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB initial

} // namespace

// ============================================================================
// Address20 utilities
// ============================================================================

Address20 parse_address(const std::string &hex) {
  Address20 addr{};
  size_t start = (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) ? 2 : 0;
  assert(hex.size() - start == 40);
  for (size_t i = 0; i < 20; ++i) {
    char hi = hex[start + i * 2];
    char lo = hex[start + i * 2 + 1];
    auto hex_val = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      assert(false);
      return 0;
    };
    addr[i] = (hex_val(hi) << 4) | hex_val(lo);
  }
  return addr;
}

std::string format_address(const Address20 &addr) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string result = "0x";
  result.reserve(42);
  for (size_t i = 0; i < 20; ++i) {
    result.push_back(hex_chars[addr[i] >> 4]);
    result.push_back(hex_chars[addr[i] & 0x0f]);
  }
  return result;
}

uint64_t address_hash(const Address20 &addr) {
  // FNV-1a hash
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < 20; ++i) {
    h ^= addr[i];
    h *= 1099511628211ULL;
  }
  return h;
}

bool address_equal(const Address20 &a, const Address20 &b) {
  return std::memcmp(a.data(), b.data(), 20) == 0;
}

// ============================================================================
// File operations
// ============================================================================

namespace {

int open_or_create_file(const char *path, size_t size, bool *created) {
  *created = false;
  int fd = open(path, O_RDWR);
  if (fd >= 0) {
    return fd;
  }
  assert(errno == ENOENT);

  fd = open(path, O_RDWR | O_CREAT, 0644);
  assert(fd >= 0);

  int r = ftruncate(fd, static_cast<off_t>(size));
  assert(r == 0);

  *created = true;
  return fd;
}

void *map_file(int fd, size_t size) {
  void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  assert(ptr != MAP_FAILED);
  return ptr;
}

void resize_events_log(Stage3Runtime *rt, size_t new_size) {
  assert(new_size > rt->events_size);

  int r = ftruncate(rt->fd_events, static_cast<off_t>(new_size));
  assert(r == 0);

  void *new_ptr = mremap(rt->events_log, rt->events_size, new_size, MREMAP_MAYMOVE);
  assert(new_ptr != MAP_FAILED);

  rt->events_log = static_cast<EventRecord *>(new_ptr);
  rt->events_size = new_size;
  rt->events_log_capacity = new_size / sizeof(EventRecord);
}

} // namespace

// ============================================================================
// Stage3 open/close
// ============================================================================

Stage3Runtime *stage3_open(const char *data_dir) {
  namespace fs = std::filesystem;

  fs::path dir(data_dir);
  fs::create_directories(dir);

  std::string store_path = (dir / "store.bin").string();
  std::string events_path = (dir / "events.log").string();
  std::string index_path = (dir / "users.idx").string();

  auto *rt = new Stage3Runtime{};

  // Open store.bin
  bool store_created = false;
  rt->fd_store = open_or_create_file(store_path.c_str(), STORE_TOTAL_SIZE, &store_created);
  rt->store_size = STORE_TOTAL_SIZE;

  auto *store_base = static_cast<uint8_t *>(map_file(rt->fd_store, STORE_TOTAL_SIZE));
  rt->header = reinterpret_cast<StoreHeader *>(store_base + STORE_HEADER_OFFSET);
  rt->conditions = reinterpret_cast<ConditionMeta *>(store_base + STORE_CONDITIONS_OFFSET);
  rt->token_pool = reinterpret_cast<TokenSlot *>(store_base + STORE_TOKENS_OFFSET);
  rt->feature_pool = reinterpret_cast<FeatureSlot *>(store_base + STORE_FEATURES_OFFSET);
  rt->sharpe_agg_pool = reinterpret_cast<SharpeAgg *>(store_base + STORE_SHARPE_AGG_OFFSET);
  rt->sharpe_sample_pool = reinterpret_cast<SharpeSample *>(store_base + STORE_SHARPE_SAMPLE_OFFSET);
  rt->users = reinterpret_cast<UserBlock *>(store_base + STORE_USERS_OFFSET);

  // Open events.log
  bool events_created = false;
  rt->fd_events = open_or_create_file(events_path.c_str(), EVENTS_LOG_INITIAL_SIZE, &events_created);

  struct stat st;
  fstat(rt->fd_events, &st);
  rt->events_size = static_cast<size_t>(st.st_size);
  rt->events_log = static_cast<EventRecord *>(map_file(rt->fd_events, rt->events_size));
  rt->events_log_capacity = rt->events_size / sizeof(EventRecord);

  // Open users.idx
  bool index_created = false;
  rt->fd_index = open_or_create_file(index_path.c_str(), INDEX_TOTAL_SIZE, &index_created);
  rt->index_size = INDEX_TOTAL_SIZE;
  rt->user_index = static_cast<UserIndexEntry *>(map_file(rt->fd_index, INDEX_TOTAL_SIZE));

  // Initialize header if newly created
  if (store_created) {
    rt->header->magic = STORE_MAGIC;
    rt->header->version = STORE_VERSION;
    rt->header->cursor_sort_key = -1;
    rt->header->cursor_processed_events = 0;
    rt->header->user_count = 0;
    rt->header->head_bucket = 0;
    rt->header->token_pool_used = 0;
    rt->header->feature_pool_used = 0;
    rt->header->sharpe_agg_pool_used = 0;
    rt->header->sharpe_sample_pool_used = 0;
    rt->header->token_free_head = NULL_IDX;
    rt->header->feature_free_head = NULL_IDX;
    rt->header->sharpe_agg_free_head = NULL_IDX;
    rt->header->sharpe_sample_free_head = NULL_IDX;
    rt->header->events_log_tail = 0;
  } else {
    assert(rt->header->magic == STORE_MAGIC);
  }

  // Initialize user index if newly created
  if (index_created) {
    for (size_t i = 0; i < USER_INDEX_SLOT_COUNT; ++i) {
      rt->user_index[i].user_idx = NULL_IDX;
      rt->user_index[i].next = NULL_IDX;
    }
  }

  rt->cond_market_question_counts.assign(MAX_CONDITIONS, 0);

  return rt;
}

void stage3_close(Stage3Runtime *rt) {
  assert(rt != nullptr);

  stage3_sync(rt);

  munmap(rt->header, rt->store_size);
  munmap(rt->events_log, rt->events_size);
  munmap(rt->user_index, rt->index_size);

  close(rt->fd_store);
  close(rt->fd_events);
  close(rt->fd_index);

  delete rt;
}

void stage3_sync(Stage3Runtime *rt) {
  msync(rt->header, rt->store_size, MS_SYNC);
  msync(rt->events_log, rt->events_size, MS_SYNC);
  msync(rt->user_index, rt->index_size, MS_SYNC);
}

// ============================================================================
// UserIndex operations
// ============================================================================

uint32_t user_index_lookup(const Stage3Runtime *rt, const Address20 &addr) {
  uint64_t h = address_hash(addr);
  size_t slot = h % USER_INDEX_SLOT_COUNT;

  const UserIndexEntry *entry = &rt->user_index[slot];
  while (entry->user_idx != NULL_IDX) {
    if (address_equal(entry->addr, addr)) {
      return entry->user_idx;
    }
    if (entry->next == NULL_IDX)
      break;
    entry = &rt->user_index[entry->next];
  }
  return NULL_IDX;
}

uint32_t user_index_insert(Stage3Runtime *rt, const Address20 &addr) {
  uint64_t h = address_hash(addr);
  size_t slot = h % USER_INDEX_SLOT_COUNT;

  UserIndexEntry *entry = &rt->user_index[slot];

  // Empty slot
  if (entry->user_idx == NULL_IDX) {
    uint32_t user_idx = static_cast<uint32_t>(rt->header->user_count++);
    entry->addr = addr;
    entry->user_idx = user_idx;
    entry->next = NULL_IDX;
    return user_idx;
  }

  // Check existing chain
  while (true) {
    if (address_equal(entry->addr, addr)) {
      return entry->user_idx;
    }
    if (entry->next == NULL_IDX)
      break;
    entry = &rt->user_index[entry->next];
  }

  // Add to chain - find a free slot
  // Simple linear probe for collision chain
  size_t probe = (slot + 1) % USER_INDEX_SLOT_COUNT;
  while (rt->user_index[probe].user_idx != NULL_IDX) {
    probe = (probe + 1) % USER_INDEX_SLOT_COUNT;
    assert(probe != slot); // Table full
  }

  uint32_t user_idx = static_cast<uint32_t>(rt->header->user_count++);
  entry->next = static_cast<uint32_t>(probe);
  rt->user_index[probe].addr = addr;
  rt->user_index[probe].user_idx = user_idx;
  rt->user_index[probe].next = NULL_IDX;

  return user_idx;
}

uint32_t user_get_or_create(Stage3Runtime *rt, const Address20 &addr) {
  uint32_t user_idx = user_index_lookup(rt, addr);
  if (user_idx != NULL_IDX) {
    return user_idx;
  }

  user_idx = user_index_insert(rt, addr);

  // Initialize UserBlock
  UserBlock *user = &rt->users[user_idx];
  user->addr = addr;
  user->flags = 1; // occupied
  user->total_events = 0;
  user->total_realized_pnl = 0;
  user->total_unrealized_pnl = 0;
  user->last_sort_key = 0;
  user->token_head = NULL_IDX;
  user->token_count = 0;
  user->feature_head = NULL_IDX;
  user->feature_count = 0;
  user->sharpe_agg_head = NULL_IDX;
  user->sharpe_agg_count = 0;
  user->timeline_head = NULL_LOG_OFFSET;
  user->timeline_tail = NULL_LOG_OFFSET;
  user->timeline_count = 0;

  return user_idx;
}

// ============================================================================
// Pool operations
// ============================================================================

uint32_t token_alloc(Stage3Runtime *rt) {
  if (rt->header->token_free_head != NULL_IDX) {
    uint32_t idx = rt->header->token_free_head;
    rt->header->token_free_head = rt->token_pool[idx].next;
    return idx;
  }
  assert(rt->header->token_pool_used < MAX_TOKENS);
  return static_cast<uint32_t>(rt->header->token_pool_used++);
}

void token_free(Stage3Runtime *rt, uint32_t idx) {
  rt->token_pool[idx].cond_idx = -1;
  rt->token_pool[idx].next = rt->header->token_free_head;
  rt->header->token_free_head = idx;
}

uint32_t feature_alloc(Stage3Runtime *rt) {
  if (rt->header->feature_free_head != NULL_IDX) {
    uint32_t idx = rt->header->feature_free_head;
    rt->header->feature_free_head = rt->feature_pool[idx].next;
    return idx;
  }
  assert(rt->header->feature_pool_used < MAX_FEATURES);
  return static_cast<uint32_t>(rt->header->feature_pool_used++);
}

void feature_free(Stage3Runtime *rt, uint32_t idx) {
  rt->feature_pool[idx].flags = 0;
  rt->feature_pool[idx].next = rt->header->feature_free_head;
  rt->header->feature_free_head = idx;
}

uint32_t sharpe_agg_alloc(Stage3Runtime *rt) {
  if (rt->header->sharpe_agg_free_head != NULL_IDX) {
    uint32_t idx = rt->header->sharpe_agg_free_head;
    rt->header->sharpe_agg_free_head = rt->sharpe_agg_pool[idx].next;
    return idx;
  }
  assert(rt->header->sharpe_agg_pool_used < MAX_SHARPE_AGGS);
  return static_cast<uint32_t>(rt->header->sharpe_agg_pool_used++);
}

void sharpe_agg_free(Stage3Runtime *rt, uint32_t idx) {
  rt->sharpe_agg_pool[idx].next = rt->header->sharpe_agg_free_head;
  rt->header->sharpe_agg_free_head = idx;
}

uint32_t sharpe_sample_alloc(Stage3Runtime *rt) {
  if (rt->header->sharpe_sample_free_head != NULL_IDX) {
    uint32_t idx = rt->header->sharpe_sample_free_head;
    rt->header->sharpe_sample_free_head = rt->sharpe_sample_pool[idx].next;
    return idx;
  }
  assert(rt->header->sharpe_sample_pool_used < MAX_SHARPE_SAMPLES);
  return static_cast<uint32_t>(rt->header->sharpe_sample_pool_used++);
}

void sharpe_sample_free(Stage3Runtime *rt, uint32_t idx) {
  rt->sharpe_sample_pool[idx].next = rt->header->sharpe_sample_free_head;
  rt->header->sharpe_sample_free_head = idx;
}

// ============================================================================
// Token state operations
// ============================================================================

TokenSlot *token_find(Stage3Runtime *rt, uint32_t user_idx, int32_t cond_idx, int16_t token_idx) {
  uint32_t idx = rt->users[user_idx].token_head;
  while (idx != NULL_IDX) {
    TokenSlot *tok = &rt->token_pool[idx];
    if (tok->cond_idx == cond_idx && tok->token_idx == token_idx) {
      return tok;
    }
    idx = tok->next;
  }
  return nullptr;
}

TokenSlot *token_get_or_create(Stage3Runtime *rt, uint32_t user_idx, int32_t cond_idx, int16_t token_idx, int16_t collateral) {
  TokenSlot *existing = token_find(rt, user_idx, cond_idx, token_idx);
  if (existing)
    return existing;

  uint32_t idx = token_alloc(rt);
  TokenSlot *tok = &rt->token_pool[idx];
  tok->user_idx = user_idx;
  tok->cond_idx = cond_idx;
  tok->token_idx = token_idx;
  tok->collateral = collateral;
  tok->pos = 0;
  tok->cost = 0;
  tok->lp = 0;
  tok->entry_block = 0;
  tok->next = rt->users[user_idx].token_head;

  rt->users[user_idx].token_head = idx;
  rt->users[user_idx].token_count++;

  return tok;
}

void token_remove_if_empty(Stage3Runtime *rt, uint32_t user_idx, TokenSlot *tok) {
  if (tok->pos != 0)
    return;

  // Find and remove from linked list
  UserBlock *user = &rt->users[user_idx];
  uint32_t *prev_ptr = &user->token_head;
  uint32_t idx = user->token_head;

  while (idx != NULL_IDX) {
    TokenSlot *t = &rt->token_pool[idx];
    if (t == tok) {
      *prev_ptr = t->next;
      user->token_count--;
      token_free(rt, idx);
      return;
    }
    prev_ptr = &t->next;
    idx = t->next;
  }
}

// ============================================================================
// Feature operations
// ============================================================================

FeatureSlot *feature_find(Stage3Runtime *rt, uint32_t user_idx, int32_t bucket, int8_t tag_id) {
  uint32_t idx = rt->users[user_idx].feature_head;
  while (idx != NULL_IDX) {
    FeatureSlot *feat = &rt->feature_pool[idx];
    if (feat->bucket == bucket && feat->tag_id == tag_id && (feat->flags & 1)) {
      return feat;
    }
    idx = feat->next;
  }
  return nullptr;
}

FeatureSlot *feature_get_or_create(Stage3Runtime *rt, uint32_t user_idx, int32_t bucket, int8_t tag_id) {
  FeatureSlot *existing = feature_find(rt, user_idx, bucket, tag_id);
  if (existing)
    return existing;

  uint32_t idx = feature_alloc(rt);
  FeatureSlot *feat = &rt->feature_pool[idx];
  std::memset(feat, 0, sizeof(FeatureSlot));
  feat->user_idx = user_idx;
  feat->bucket = bucket;
  feat->tag_id = tag_id;
  feat->flags = 1;
  feat->next = rt->users[user_idx].feature_head;

  rt->users[user_idx].feature_head = idx;
  rt->users[user_idx].feature_count++;

  return feat;
}

// ============================================================================
// Sharpe operations
// ============================================================================

SharpeAgg *sharpe_agg_find(Stage3Runtime *rt, uint32_t user_idx, int32_t bucket) {
  uint32_t idx = rt->users[user_idx].sharpe_agg_head;
  while (idx != NULL_IDX) {
    SharpeAgg *agg = &rt->sharpe_agg_pool[idx];
    if (agg->bucket == bucket) {
      return agg;
    }
    idx = agg->next;
  }
  return nullptr;
}

SharpeAgg *sharpe_agg_get_or_create(Stage3Runtime *rt, uint32_t user_idx, int32_t bucket) {
  SharpeAgg *existing = sharpe_agg_find(rt, user_idx, bucket);
  if (existing)
    return existing;

  uint32_t idx = sharpe_agg_alloc(rt);
  SharpeAgg *agg = &rt->sharpe_agg_pool[idx];
  agg->user_idx = user_idx;
  agg->bucket = bucket;
  agg->close_pnl = 0;
  agg->min_pnl = INT64_MAX;
  agg->max_pnl = INT64_MIN;
  agg->sample_head = NULL_IDX;
  agg->sample_count = 0;
  agg->last_block = -1;
  agg->next = rt->users[user_idx].sharpe_agg_head;

  rt->users[user_idx].sharpe_agg_head = idx;
  rt->users[user_idx].sharpe_agg_count++;

  return agg;
}

// ============================================================================
// Events log operations
// ============================================================================

uint64_t events_log_append(Stage3Runtime *rt, const EventRecord &rec, uint32_t user_idx) {
  // Ensure capacity
  size_t required = rt->header->events_log_tail + sizeof(EventRecord);
  if (required > rt->events_size) {
    size_t new_size = rt->events_size * 2;
    while (new_size < required) {
      new_size *= 2;
    }
    resize_events_log(rt, new_size);
  }

  uint64_t offset = rt->header->events_log_tail;
  EventRecord *target = reinterpret_cast<EventRecord *>(
      reinterpret_cast<uint8_t *>(rt->events_log) + offset);
  *target = rec;
  target->next_user_event_offset = NULL_LOG_OFFSET;

  // Update user timeline chain
  UserBlock *user = &rt->users[user_idx];
  if (user->timeline_head == NULL_LOG_OFFSET) {
    user->timeline_head = offset;
  } else {
    EventRecord *tail = reinterpret_cast<EventRecord *>(
        reinterpret_cast<uint8_t *>(rt->events_log) + user->timeline_tail);
    tail->next_user_event_offset = offset;
  }
  user->timeline_tail = offset;
  user->timeline_count++;

  rt->header->events_log_tail += sizeof(EventRecord);
  return offset;
}

// ============================================================================
// ConditionMeta operations
// ============================================================================

void stage3_set_condition(Stage3Runtime *rt,
                          int32_t cond_idx,
                          uint8_t outcome_count,
                          int8_t tag_id,
                          const int64_t *payout_numerators,
                          uint16_t market_question_count) {
  assert(cond_idx >= 0 && static_cast<size_t>(cond_idx) < MAX_CONDITIONS);

  ConditionMeta *cond = &rt->conditions[cond_idx];
  cond->outcome_count = outcome_count;
  cond->tag_id = tag_id;
  cond->flags = 0; // Not valid yet until mark_valid is called
  rt->cond_market_question_counts[cond_idx] = market_question_count;

  if (payout_numerators) {
    size_t copy_count = std::min<size_t>(outcome_count, OUTCOME_MAX);
    std::memcpy(cond->payout_numerators, payout_numerators, copy_count * sizeof(int64_t));
  }
}

void stage3_mark_condition_valid(Stage3Runtime *rt, int32_t cond_idx) {
  assert(cond_idx >= 0 && static_cast<size_t>(cond_idx) < MAX_CONDITIONS);
  rt->conditions[cond_idx].flags |= 1;
}

const ConditionMeta *stage3_get_condition(const Stage3Runtime *rt, int32_t cond_idx) {
  if (cond_idx < 0 || static_cast<size_t>(cond_idx) >= MAX_CONDITIONS) {
    return nullptr;
  }
  const ConditionMeta *cond = &rt->conditions[cond_idx];
  if (!(cond->flags & 1)) {
    return nullptr;
  }
  return cond;
}

} // namespace stage3

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#if defined(__x86_64__) || defined(__i386__) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
#include <mkl.h>
#endif
#include <random>

#include <ATen/ATen.h>
#include <ATen/record_function.h>

#include <folly/container/F14Map.h>
#include <glog/logging.h>

#include <folly/Random.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/hash/Hash.h>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>
#include <rocksdb/table_properties.h>

#include <ATen/cuda/CUDAContext.h>
#include <cuda_runtime.h>

#include <folly/experimental/coro/Task.h>
#include "fbgemm_gpu/split_embeddings_cache/cachelib_cache.h"
#include "fbgemm_gpu/utils/dispatch_macros.h"
#include "kv_db_cuda_utils.h"

namespace kv_db {

/// @ingroup embedding-ssd
///
/// @brief It holds l2cache lookup results
///
/// num_misses is the number of misses in the l2 cache lookup
/// cached_addr_list is preallocated with number of lookups for better
/// parallelism
///   and invalid spot(cache misses) will stay as sentinel value
class CacheContext {
 public:
  explicit CacheContext(size_t num_keys) {
    cached_addr_list = std::vector<void*>(num_keys, nullptr);
  }
  // invalid spot will stay as sentinel value, this is trading space for better
  // parallelism
  std::atomic<int> num_misses{0};
  std::vector<void*> cached_addr_list;
};

/// @ingroup embedding-ssd
///
/// @brief A class for interacting with different cache layers and storage
/// layers, public calls are executed on cuda stream
///
/// Currently it is used by TBE to offload Key(Embedding Index)
/// Value(Embeddings) to DRAM, SSD or remote storage, to provide better
/// scalability without blowing up HBM resources
class EmbeddingKVDB : public std::enable_shared_from_this<EmbeddingKVDB> {
 public:
  explicit EmbeddingKVDB(
      int64_t num_shards,
      int64_t max_D,
      int64_t cache_size_gb = 0,
      int64_t unique_id = 0)
      : unique_id_(unique_id),
        num_shards_(num_shards),
        max_D_(max_D),
        executor_tp_(
            std::make_unique<folly::CPUThreadPoolExecutor>(num_shards)) {
    assert(num_shards > 0);
    l2_cache_ = cache_size_gb > 0
        ? std::make_unique<l2_cache::CacheLibCache>(
              cache_size_gb * 1024 * 1024 * 1024, num_shards_)
        : nullptr;
  }
  virtual ~EmbeddingKVDB() = default;

  /// Insert non-negative elements in <indices> and its paired embeddings
  /// from <weights> for the first <count> elements in the tensor.
  /// It will simply copy all 3 tensors and inject them into the background
  /// working item queue to be picked up for eviction/promotion handling between
  /// L2 cache and storage tier
  ///
  /// @param indices The 1D embedding index tensor, should skip on negative
  /// value
  /// @param weights The 2D tensor that each row(embeddings) is paired up with
  /// relative element in <indices>
  /// @param count A single element tensor that contains the number of indices
  /// to be processed
  ///
  /// @return None
  void set(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count,
      const bool is_bwd = false);

  /// Find non-negative embedding indices in <indices> and fill up the
  /// related embeddings into <weights> for the first <count> elements in the
  /// tensor.
  /// It contains 3 steps:
  /// - L2 cache lookup
  /// - filling weights concurrently from L2 cache(cache hit) and
  /// storage tier(Rocksdb/PS) on cache miss
  /// - for cache misses, copy 3 tensors and inject them into the background
  /// working item queue to be picked up for eviction/promotion handling between
  /// L2 cache and storage tier
  ///
  /// @param indices The 1D embedding index tensor, should skip on negative
  /// value
  /// @param weights The 2D tensor that each row(embeddings) is paired up with
  /// relative element in <indices>
  /// @param count A single element tensor that contains the number of indices
  /// to be processed
  ///
  /// @return None
  /// @note weights will be updated from either L2 cache or storage tier
  void get(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count);

  /// storage tier counterpart of function get()
  virtual folly::coro::Task<void> get_kv_db_async(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count) = 0;

  /// storage tier counterpart of function set()
  virtual folly::coro::Task<void> set_kv_db_async(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count,
      const bool is_bwd = false) = 0;

  virtual void compact() = 0;

  virtual void flush() = 0;

  // The function attaches the CUDA callback logic to the compute
  // stream to ensure that the data retrieval is carried out properly.
  // It internally invokes get to fetch values from the KV database.
  void get_cuda(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count);

  void set_cuda(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count,
      const int64_t timestep,
      const bool is_bwd = false);

 private:
  /// Find non-negative embedding indices in <indices> and shard them into
  /// #cachelib_pools pieces to be lookedup in parallel
  ///
  /// @param indices The 1D embedding index tensor, should skip on negative
  /// value
  /// @param count A single element tensor that contains the number of indices
  /// to be processed
  ///
  /// @return preallocated list of memory pointer with <count> size, cache miss
  /// or invalid embedding indices will have sentinel pointer(nullptr)
  /// @note element in <indices> will be updated to sentinel value on cache hit
  std::shared_ptr<CacheContext> get_cache(
      const at::Tensor& indices,
      const at::Tensor& count);

  /// Find non-negative embedding indices in <indices> and shard them into
  /// #cachelib_pools pieces, insert into Cachelib in parallel with their paired
  /// embeddings from <weights>
  ///
  /// @param indices The 1D embedding index tensor, should skip on negative
  /// value
  /// @param weights The 2D tensor that each row(embeddings) is paired up with
  /// relative element in <indices>
  /// @param count A single element tensor that contains the number of indices
  /// to be processed
  ///
  /// @return pair of tensors with length of <count> containing L2 evicted
  /// embedding indices and embeddings, invalid pairs will have
  /// sentinel value(-1) on <indices>
  void set_cache(
      const at::Tensor& indices,
      const at::Tensor& weights,
      const at::Tensor& count);

  /// Find valid cache pointer from <cached_addr_list> and memcopy the
  /// embeddings from L2 cache to the same slot(row id) in <weights>
  ///
  /// @param cached_addr_list The 1D vector containing L2 cache addr pointer,
  /// sentinel pointer on cache miss in the related slot value
  /// @param weights The 2D tensor that each row(embeddings) is paired up with
  /// relative slot in <cached_addr_list>
  ///
  /// @return None
  /// @note weigths will be updated on the slot that paired up with valid cache
  /// addr pointer
  folly::coro::Task<void> cache_memcpy(
      const at::Tensor& weights,
      const std::vector<void*>& cached_addr_list);

  virtual void flush_or_compact(const int64_t timestep) = 0;

  std::unique_ptr<l2_cache::CacheLibCache> l2_cache_;
  const int64_t unique_id_;
  const int64_t num_shards_;
  const int64_t max_D_;
  std::unique_ptr<folly::CPUThreadPoolExecutor> executor_tp_;
}; // class EmbeddingKVDB

} // namespace kv_db

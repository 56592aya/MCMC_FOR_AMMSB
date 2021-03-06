/*
 * Copyright notice
 *
 * @author Rutger Hofman rutger@cs.vu.nl
 * @date Feb 2015
 */

/*
 * Distributed Key-Value Store that offers just enough functionality to
 * support the MCMC Stochastical applications.
 *
 * This is an Interface definition.
 */

#ifndef APPS_MCMC_D_KV_STORE_RAMCLOUD_DKV_STORE_H__
#define APPS_MCMC_D_KV_STORE_RAMCLOUD_DKV_STORE_H__

#include <mcmc/config.h>

#ifndef MCMC_ENABLE_RAMCLOUD
#error "This file should not be included if the project is not setup to support RamCloud"
#endif

#ifndef __INTEL_COMPILER
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic push
#endif
#include <RamCloud.h>
#ifndef __INTEL_COMPILER
#pragma GCC diagnostic pop
#endif

#include "dkvstore/DKVStore.h"

namespace DKV {
namespace DKVRamCloud {

class DKVStoreRamCloudOptions : public DKVStoreOptions {
 public:
  DKVStoreRamCloudOptions();

  void Parse(const std::vector<std::string> &args) override;
  boost::program_options::options_description* GetMutable() override {
    return &desc_;
  }
  
  inline const std::string& table() const { return table_; }
  inline const std::string& proto() const { return proto_; }
  inline const std::string& host() const { return host_; }
  inline const std::string& port() const { return port_; }

 private:
  std::string table_;
  std::string proto_;
  std::string host_;
  std::string port_;
  boost::program_options::options_description desc_;

  friend std::ostream& operator<<(std::ostream& out,
                                  const DKVStoreRamCloudOptions& opts);
};

inline std::ostream& operator<<(std::ostream& out,
                                const DKVStoreRamCloudOptions& opts) {
  out << opts.desc_;
  return out;
}

class DKVStoreRamCloud : public DKVStoreInterface {

 public:
  typedef DKVStoreInterface::KeyType KeyType;
  typedef DKVStoreInterface::ValueType ValueType;

  DKVStoreRamCloud(const std::vector<std::string> &args);

  ~DKVStoreRamCloud();

  virtual void Init(::size_t value_size, ::size_t total_values,
                    ::size_t max_cache_capacity, ::size_t max_write_capacity,

  virtual void ReadKVRecords(std::vector<ValueType *> &cache,
                             const std::vector<KeyType> &key,
                             RW_MODE::RWMode rw_mode);

  virtual void WriteKVRecords(const std::vector<KeyType> &key,
                              const std::vector<const ValueType *> &value);

  virtual std::vector<ValueType *> GetWriteKVRecords(::size_t n);

  virtual void FlushKVRecords(const std::vector<KeyType> &key);

  virtual void PurgeKVRecords();

 private:
  DKVStoreRamCloudOptions options_;
  RAMCloud::RamCloud *client_ = NULL;
  uint64_t table_id_;
  std::string table_ = "table1";
  std::unordered_map<KeyType, RAMCloud::Tub<RAMCloud::ObjectBuffer>*> obj_buffer_map_;
};

} // namespace DKVRamCloud
} // namespace DKV

#endif  // def APPS_MCMC_D_KV_STORE_RAMCLOUD_DKV_STORE_H__

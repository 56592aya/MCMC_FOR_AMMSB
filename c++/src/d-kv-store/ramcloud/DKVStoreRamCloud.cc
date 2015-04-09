/*
 * Copyright notice
 */

#include "d-kv-store/ramcloud/DKVStoreRamCloud.h"

#include <cassert>
#include <sstream>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace DKV {
namespace DKVRamCloud {

DKVStoreRamCloud::~DKVStoreRamCloud() {
  delete client_;
  delete[] cache_;
}

void DKVStoreRamCloud::Init(::size_t value_size, ::size_t total_values,
                            ::size_t max_capacity,
                            const std::vector<std::string> &args) {
  value_size_ = value_size;
  total_values_ = total_values;
  max_capacity_ = max_capacity;

  std::string table;
  std::string proto;
  std::string host;
  std::string port;

  std::cerr << "DKVStoreRamCloud::Init args ";
  for (auto a : args) {
    std::cerr << a << " ";
  }
  std::cerr << std::endl;

  po::options_description desc("Ramcloud options");
  desc.add_options()
    ("ramcloud.table,t", po::value<std::string>(&table)->default_value("0.0.0.0"), "Coordinator table")
    ("ramcloud.coordinator,c", po::value<std::string>(&host)->default_value("0.0.0.0"), "Coordinator host")
    ("ramcloud.port,p", po::value<std::string>(&port)->default_value("1100"), "Coordinator port")
    ("ramcloud.protocol,P", po::value<std::string>(&proto)->default_value("infrc"), "Coordinator protocol") 
    ;

  po::variables_map vm;
  po::basic_command_line_parser<char> clp(args);
  clp.options(desc);
  po::store(clp.run(), vm);
  po::notify(vm);

  std::ostringstream coordinator;
  coordinator << proto << ":host=" << host << ",port=" << port;
  std::cerr << "coordinator description: " << coordinator.str() << std::endl;

  cache_ = new ValueType[max_capacity_ * value_size];

  client_ = new RAMCloud::RamCloud(coordinator.str().c_str());
  try {
    table_id_ = client_->getTableId(table.c_str());
  } catch (RAMCloud::TableDoesntExistException& e) {
    table_id_ = client_->createTable(table.c_str(), 1);
  }
}

void DKVStoreRamCloud::ReadKVRecords(std::vector<ValueType *> &cache,
                                     const std::vector<KeyType> &key,
                                     RW_MODE::RWMode rw_mode) {
  std::vector<RAMCloud::Tub<RAMCloud::ObjectBuffer> *> bufs;
  // vals place holder
  for (auto k : key) {
    bufs[k] = new RAMCloud::Tub<RAMCloud::ObjectBuffer>();
    if (rw_mode == RW_MODE::READ_ONLY) {
      obj_buffer_map_[k] = bufs[k];
    }
  }
  // batch requests
  std::vector<RAMCloud::MultiReadObject*> reqs(key.size());
  for (::size_t i = 0; i < key.size(); ++i) {
    reqs[i] = new RAMCloud::MultiReadObject(table_id_,
                                            &key[i], sizeof key[i],
                                            bufs[i]);
  }
  client_->multiRead(reqs.data(), key.size());

  for (::size_t i = 0; i < key.size(); ++i) {
    /**     
     * BufferObject contains key and value data. Need to fetch the value only
     * bufs[i]->copy(0, K*sizeof(ValueType), vals[i].data());
     */
    assert((*bufs[i])->getValue() != NULL);
    if (rw_mode == RW_MODE::READ_ONLY) {
      cache[i] = (ValueType *)(*bufs[i])->getValue();
    } else {
      ValueType *cache_pointer = cache_ + next_free_ * value_size_;
      next_free_++;
      cache[i] = cache_pointer;
      value_of_[key[i]] = cache_pointer;
      memcpy(cache[i], (*bufs[i])->getValue(), value_size_ * sizeof(ValueType));
    }
  }

  for (auto e : reqs) {
    delete e;
  }
  if (rw_mode == RW_MODE::READ_ONLY) {
    for (auto b : bufs) {
      delete b;
    }
  }
}

void DKVStoreRamCloud::WriteKVRecords(const std::vector<KeyType> &key,
                                      const std::vector<const ValueType *> &value) {
  std::vector<RAMCloud::MultiWriteObject *> req(key.size());
  for (::size_t i = 0; i < key.size(); i++) {
    req[i] = new RAMCloud::MultiWriteObject(table_id_,
                                            &key[i], sizeof key[i],
                                            value[i],
                                            value_size_ * sizeof(ValueType));
  }
  client_->multiWrite(req.data(), key.size());

  for (auto e : req) {
    delete e;
  }
}

void DKVStoreRamCloud::FlushKVRecords(const std::vector<KeyType> &key) {
  std::vector<const ValueType *> value(key.size());
  for (::size_t i = 0; i < key.size(); i++) {
    value[i] = value_of_[key[i]];
  }
  DKVStoreRamCloud::WriteKVRecords(key, value);
}

void DKVStoreRamCloud::PurgeKVRecords() {
  // Clear the cached read-only buffers
  for (auto b : obj_buffer_map_) {
    delete b.second;
  }
  obj_buffer_map_.clear();

  // Clear the copied read/write buffer(s)
  next_free_ = 0;
  value_of_.clear();
}

} // namespace DKVRamCloud
} // namespace DKV

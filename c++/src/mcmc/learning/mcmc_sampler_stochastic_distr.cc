#include <mcmc/learning/mcmc_sampler_stochastic_distr.h>

#include <cinttypes>
#include <cmath>

#include <utility>
#include <numeric>
#include <algorithm>	// min, max
#include <chrono>

#include "mcmc/exception.h"
#include "mcmc/config.h"

#include "mcmc/fixed-size-set.h"

#ifdef MCMC_SINGLE_PRECISION
#  define FLOATTYPE_MPI MPI_FLOAT
#else
#  define FLOATTYPE_MPI MPI_DOUBLE
#endif

#ifdef MCMC_ENABLE_DISTRIBUTED
#  include <mpi.h>
#else

#define MPI_SUCCESS		0

typedef int MPI_Comm;
#define MPI_COMM_WORLD	0

enum MPI_ERRORS {
  MPI_ERRORS_RETURN,
  MPI_ERRORS_ARE_FATAL,
};

enum MPI_Datatype {
  MPI_INT                    = 0x4c000405,
  MPI_LONG                   = 0x4c000407,
  MPI_UNSIGNED_LONG          = 0x4c000408,
  MPI_DOUBLE                 = 0x4c00080b,
  MPI_BYTE                   = 0x4c00010d,
};

enum MPI_Op {
  MPI_SUM,
};

void *MPI_IN_PLACE = (void *)0x88888888;


int MPI_Init(int *argc, char ***argv) {
  return MPI_SUCCESS;
}

int MPI_Finalize() {
  return MPI_SUCCESS;
}

int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype,
              int root, MPI_Comm comm) {
  return MPI_SUCCESS;
}

int MPI_Barrier(MPI_Comm comm) {
  return MPI_SUCCESS;
}

int MPI_Comm_set_errhandler(MPI_Comm comm, int mode) {
  return MPI_SUCCESS;
}

int MPI_Comm_size(MPI_Comm comm, int *mpi_size) {
  *mpi_size = 1;
  return MPI_SUCCESS;
}

int MPI_Comm_rank(MPI_Comm comm, int *mpi_rank) {
  *mpi_rank = 0;
  return MPI_SUCCESS;
}

::size_t mpi_datatype_size(MPI_Datatype type) {
  switch (type) {
  case MPI_INT:
    return sizeof(int32_t);
  case MPI_LONG:
    return sizeof(int64_t);
  case MPI_UNSIGNED_LONG:
    return sizeof(uint64_t);
  case MPI_FLOAT:
    return sizeof(float);
  case MPI_DOUBLE:
    return sizeof(double);
  case MPI_BYTE:
    return 1;
  default:
    std::cerr << "Unknown MPI datatype" << std::cerr;
    return 0;
  }
}

int MPI_Scatter(void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm) {
  memcpy(recvbuf, sendbuf, sendcount * mpi_datatype_size(sendtype));
  return MPI_SUCCESS;
}

int MPI_Scatterv(void *sendbuf, int *sendcounts, int *displs, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int root, MPI_Comm comm) {
  return MPI_Scatter((char *)sendbuf + displs[0] * mpi_datatype_size(sendtype),
                     sendcounts[0], sendtype,
                     recvbuf, recvcount, recvtype,
                     root, comm);
}

int MPI_Allreduce(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype,
                  MPI_Op op, MPI_Comm comm) {
  if (sendbuf != MPI_IN_PLACE) {
    memcpy(recvbuf, sendbuf, count * mpi_datatype_size(datatype));
  }
  return MPI_SUCCESS;
}

#endif

#include "dkvstore/DKVStoreFile.h"
#ifdef MCMC_ENABLE_RAMCLOUD
#include "dkvstore/DKVStoreRamCloud.h"
#endif
#ifdef MCMC_ENABLE_RDMA
#include "dkvstore/DKVStoreRDMA.h"
#endif

#include "mcmc/np.h"


namespace mcmc {
namespace learning {

#define PRINT_MEM_USAGE() \
do { \
  std::cerr << __func__ << "():" << __LINE__ << " "; \
  print_mem_usage(std::cerr); \
} while (0)

using ::mcmc::timer::Timer;


// **************************************************************************
//
// class LocalNetwork
//
// **************************************************************************

void LocalNetwork::unmarshall_local_graph(::size_t index, const Vertex* linked,
                                          ::size_t size) {
  if (linked_edges_.size() <= index) {
    linked_edges_.resize(index + 1);
  }
  linked_edges_[index] = EndpointSet();
  for (::size_t i = 0; i < size; ++i) {
    linked_edges_[index].insert(linked[i]);
  }
}

void LocalNetwork::reset() {
  linked_edges_.clear();
}

bool LocalNetwork::find(const Edge& edge) const {
  const auto &adj = linked_edges_[edge.first];

  return adj.find(edge.second) != adj.end();
}

const LocalNetwork::EndpointSet& LocalNetwork::linked_edges(::size_t i) const {
  return linked_edges_[i];
}


// **************************************************************************
//
// class PerpData
//
// **************************************************************************

void PerpData::Init(::size_t max_perplexity_chunk) {
  // Convert the vertices into their rank, that is all we need

  // Find the ranks
  nodes_.resize(data_.size() * 2);
  Vertex ix = 0;
  for (auto edge : data_) {
    const Edge &e = edge.edge;
    nodes_[ix] = e.first;
    ++ix;
    nodes_[ix] = e.second;
    ++ix;
  }

  pi_.resize(2 * max_perplexity_chunk);

  accu_.resize(omp_get_max_threads());
}



// **************************************************************************
//
// class MCMCSamplerStochasticDistributed
//
// **************************************************************************
MCMCSamplerStochasticDistributed::MCMCSamplerStochasticDistributed(
    const Options &args) : MCMCSamplerStochastic(args), mpi_master_(0) {
  t_load_network_          = Timer("  load network graph");
  t_init_dkv_              = Timer("  initialize DKV store");
  t_populate_pi_           = Timer("  populate pi");
  t_outer_                 = Timer("  iteration");
  t_deploy_minibatch_      = Timer("    deploy minibatch");
  t_mini_batch_            = Timer("      sample_mini_batch");
  t_scatter_subgraph_      = Timer("      scatter subgraph");
  t_scatter_subgraph_marshall_edge_count_ = Timer("        marshall edge count");
  t_scatter_subgraph_scatterv_edge_count_ = Timer("        scatterv edges");
  t_scatter_subgraph_marshall_edges_      = Timer("        marshall edges");
  t_scatter_subgraph_scatterv_edges_      = Timer("        scatterv edges");
  t_scatter_subgraph_unmarshall_          = Timer("        unmarshall edges");
  t_nodes_in_mini_batch_   = Timer("      nodes_in_mini_batch");
  t_broadcast_theta_beta_  = Timer("    broadcast theta/beta");
  t_sample_neighbor_nodes_ = Timer("      sample_neighbor_nodes");
  t_sample_neighbors_sample_ = Timer("        sample");
  t_sample_neighbors_flatten_ = Timer("        flatten");
  t_update_phi_pi_         = Timer("    update_phi_pi");
  t_load_pi_minibatch_     = Timer("      load minibatch pi");
  t_load_pi_neighbor_      = Timer("      load neighbor pi");
  t_update_phi_            = Timer("      update_phi");
  t_barrier_phi_           = Timer("      barrier after update phi");
  t_update_pi_             = Timer("      update_pi");
  t_store_pi_minibatch_    = Timer("      store minibatch pi");
  t_barrier_pi_            = Timer("      barrier after update pi");
  t_update_beta_           = Timer("    update_beta_theta");
  t_beta_zero_             = Timer("      zero beta grads");
  t_beta_rank_             = Timer("      rank minibatch nodes");
  t_beta_calc_grads_       = Timer("      beta calc grads");
  t_beta_sum_grads_        = Timer("      beta sum grads");
  t_beta_reduce_grads_     = Timer("      beta reduce(+) grads");
  t_beta_update_theta_     = Timer("      update theta");
  t_load_pi_beta_          = Timer("      load pi update_beta");
  t_perplexity_            = Timer("  perplexity");
  t_load_pi_perp_          = Timer("      load perplexity pi");
  t_cal_edge_likelihood_   = Timer("      calc edge likelihood");
  t_purge_pi_perp_         = Timer("      purge perplexity pi");
  t_reduce_perp_           = Timer("      reduce/plus perplexity");
  c_minibatch_chunk_size_  = Counter("minibatch chunk size");
  Timer::setTabular(true);
}


MCMCSamplerStochasticDistributed::~MCMCSamplerStochasticDistributed() {
  for (auto &p : pi_update_) {
    delete[] p;
  }

  (void)MPI_Finalize();
}

void MCMCSamplerStochasticDistributed::InitSlaveState(const NetworkInfo &info,
                                                      ::size_t world_rank) {
  Learner::InitRandom(world_rank);
  network = Network(info);
  Learner::Init(false);
}


void MCMCSamplerStochasticDistributed::BroadcastNetworkInfo() {
  NetworkInfo info;
  int r;

  if (mpi_rank_ == mpi_master_) {
    network.FillInfo(&info);
  }

  r = MPI_Bcast(&info, sizeof info, MPI_BYTE, mpi_master_, MPI_COMM_WORLD);
  mpi_error_test(r, "MPI_Bcast of Network stub info fails");

  if (mpi_rank_ != mpi_master_) {
    InitSlaveState(info, mpi_rank_);
  }
}


void MCMCSamplerStochasticDistributed::BroadcastHeldOut() {
  int r;
  int32_t my_held_out_size;

  if (mpi_rank_ == mpi_master_) {
    std::vector<int32_t> count(mpi_size_);	// FIXME: lift to class
    std::vector<int32_t> displ(mpi_size_);	// FIXME: lift to class

    if (args_.REPLICATED_NETWORK) {
      // Ensure perplexity is centrally calculated at the master's
      for (int i = 0; i < mpi_size_; ++i) {
        if (i == mpi_master_) {
          count[i] = network.get_held_out_set().size();
        } else {
          count[i] = 0;
        }
      }
    } else {
      int32_t held_out_marshall_size = network.get_held_out_set().size() /
                                         mpi_size_;
      ::size_t surplus = network.get_held_out_set().size() % mpi_size_;
      for (::size_t i = 0; i < surplus; ++i) {
        count[i] = held_out_marshall_size + 1;
      }
      for (::size_t i = surplus; i < static_cast< ::size_t>(mpi_size_); ++i) {
        count[i] = held_out_marshall_size;
      }
    }

    // Scatter the size of each held-out set subset
    r = MPI_Scatter(count.data(), 1, MPI_INT,
                    &my_held_out_size, 1, MPI_INT,
                    mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatter of held_out_set size fails");

    // Marshall the subsets
    std::vector<EdgeMapItem> buffer(network.get_held_out_set().size());
    struct EdgeMapItem* p = buffer.data();

    for (auto e : network.get_held_out_set()) {
      p->edge = e.first;
      p->is_edge = e.second;
      ++p;
    }

    std::vector<int32_t> bytes(mpi_size_);
    for (::size_t i = 0; i < count.size(); ++i) {
      bytes[i] = count[i] * sizeof(EdgeMapItem);
    }
    displ[0] = 0;
    for (int i = 1; i < mpi_size_; ++i) {
      displ[i] = displ[i - 1] + bytes[i];
    }
    // Scatter the marshalled subgraphs
    perp_.data_.resize(my_held_out_size);
    r = MPI_Scatterv(buffer.data(), bytes.data(), displ.data(), MPI_BYTE,
                     perp_.data_.data(),
                     perp_.data_.size() * sizeof(EdgeMapItem), MPI_BYTE,
                     mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of held-out set data fails");

  } else {
    // Scatter the fanout of each minibatch node
    r = MPI_Scatter(NULL, 1, MPI_INT,
                    &my_held_out_size, 1, MPI_INT,
                    mpi_master_,
                    MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatter of held_out_set size fails");

    // Scatter the marshalled subgraphs
    perp_.data_.resize(my_held_out_size);
    r = MPI_Scatterv(NULL, NULL, NULL, MPI_BYTE,
                     perp_.data_.data(),
                     perp_.data_.size() * sizeof(EdgeMapItem), MPI_BYTE,
                     mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of held-out set data fails");
    std::cerr << "My held-out size " << my_held_out_size << std::endl;
  }

  /* Implies broadcast of the keys in held_out set */
  GoogleHashEdgeSet held_out(network.get_held_out_set(),
                             mpi_rank_, mpi_master_, MPI_COMM_WORLD);
  /* Implies broadcast of the keys in test set */
  GoogleHashEdgeSet test(network.get_test_set(),
                         mpi_rank_, mpi_master_, MPI_COMM_WORLD);

  held_out_test_.insert(held_out.begin(), held_out.end());
  held_out_test_.insert(test.begin(), test.end());

  std::cerr << "Held-out+test size " << held_out_test_.size() << std::endl;
  std::cerr << "Test size " << network.get_test_set().size() << std::endl;
  std::cerr << "Held-out size " << network.get_held_out_set().size() << std::endl;
}


void MCMCSamplerStochasticDistributed::MasterAwareLoadNetwork() {
  if (args_.REPLICATED_NETWORK) {
    LoadNetwork(mpi_rank_, false);
  } else {
    if (mpi_rank_ == mpi_master_) {
      LoadNetwork(mpi_rank_, false);
    }
    BroadcastNetworkInfo();
    // No need to broadcast the Network aux stuff, fan_out_cumul_distro and
    // cumulative_edges: it is used at the master only
  }
  BroadcastHeldOut();
}


void MCMCSamplerStochasticDistributed::InitDKVStore() {
  t_init_dkv_.start();

  std::cerr << "Use D-KV store type " << args_.dkv_type << std::endl;
  switch (args_.dkv_type) {
  case DKV::TYPE::FILE:
    d_kv_store_ = std::unique_ptr<DKV::DKVFile::DKVStoreFile>(
                    new DKV::DKVFile::DKVStoreFile(args_.getRemains()));
    break;
#ifdef MCMC_ENABLE_RAMCLOUD
  case DKV::TYPE::RAMCLOUD:
    d_kv_store_ = std::unique_ptr<DKV::DKVRamCloud::DKVStoreRamCloud>(
                    new DKV::DKVRamCloud::DKVStoreRamCloud(args_.getRemains()));
    break;
#endif
#ifdef MCMC_ENABLE_RDMA
  case DKV::TYPE::RDMA:
    d_kv_store_ = std::unique_ptr<DKV::DKVRDMA::DKVStoreRDMA>(
                    new DKV::DKVRDMA::DKVStoreRDMA(args_.getRemains()));
    break;
#endif
  }

  if (args_.max_pi_cache_entries_ == 0) {
    std::ifstream meminfo("/proc/meminfo");
    int64_t mem_total = -1;
    while (meminfo.good()) {
      char buffer[256];
      char* colon;

      meminfo.getline(buffer, sizeof buffer);
      if (strncmp("MemTotal", buffer, 8) == 0 &&
           (colon = strchr(buffer, ':')) != 0) {
        if (sscanf(colon + 2, "%ld", &mem_total) != 1) {
          throw NumberFormatException("MemTotal must be a longlong");
        }
        break;
      }
    }
    if (mem_total == -1) {
      throw InvalidArgumentException(
              "/proc/meminfo has no line for MemTotal");
    }
    // /proc/meminfo reports KB
    ::size_t pi_total = (1024 * mem_total) / ((K + 1) * sizeof(Float));
    args_.max_pi_cache_entries_ = pi_total / 32;
    std::cerr << "mem_total " << mem_total << " pi_total " << pi_total << " max pi cache entries " << args_.max_pi_cache_entries_ << std::endl;
  }

  // Calculate DKV store buffer requirements
  max_minibatch_nodes_ = network.max_minibatch_nodes_for_strategy(
                            mini_batch_size, strategy);
  ::size_t workers;
  if (master_is_worker_) {
    workers = mpi_size_;
  } else {
    workers = mpi_size_ - 1;
  }

  // pi cache hosts chunked subset of minibatch nodes + their neighbors
  max_minibatch_chunk_ = args_.max_pi_cache_entries_ / (1 + real_num_node_sample());
  max_dkv_write_entries_ = (max_minibatch_nodes_ + workers - 1) / workers;
  ::size_t max_my_minibatch_nodes = std::min(max_minibatch_chunk_,
                                             max_dkv_write_entries_);
  ::size_t max_minibatch_neighbors = max_my_minibatch_nodes *
                                      real_num_node_sample();

  // for perplexity, cache pi for both vertexes of each edge
  max_perplexity_chunk_ = args_.max_pi_cache_entries_ / 2;
  ::size_t num_perp_nodes = 2 * (network.get_held_out_size() +
                                 mpi_size_ - 1) / mpi_size_;
  ::size_t max_my_perp_nodes = std::min(2 * max_perplexity_chunk_,
                                        num_perp_nodes);

  // must cache pi[minibatch slice] for update_beta
  ::size_t max_beta_nodes = (max_minibatch_nodes_ + mpi_size_ - 1) / mpi_size_;
  max_minibatch_neighbors = std::max(max_minibatch_neighbors, max_beta_nodes);
  if (max_minibatch_neighbors > args_.max_pi_cache_entries_) {
    throw MCMCException("pi cache cannot contain pi[minibatch] for beta, "
                        "refactor so update_beta is chunked");
  }

  ::size_t max_pi_cache = std::max(max_my_minibatch_nodes +
                                     max_minibatch_neighbors,
                                   max_my_perp_nodes);

  std::cerr << "minibatch size param " << mini_batch_size <<
    " max " << max_minibatch_nodes_ <<
    " my max " << max_my_minibatch_nodes <<
    " chunk " << max_minibatch_chunk_ <<
    " #neighbors(total) " << max_minibatch_neighbors <<
    " cache max entries " << max_pi_cache <<
    " computed max pi cache entries " << args_.max_pi_cache_entries_ <<
    std::endl;
  std::cerr << "perplexity nodes total " <<
    (network.get_held_out_size() * 2) <<
    " local " << num_perp_nodes <<
    " mine " << max_my_perp_nodes <<
    " chunk " << max_perplexity_chunk_ << std::endl;

  d_kv_store_->Init(K + 1, N, max_pi_cache, max_dkv_write_entries_);
  t_init_dkv_.stop();

  master_hosts_pi_ = d_kv_store_->include_master();

  std::cerr << "Master is " << (master_is_worker_ ? "" : "not ") <<
    "a worker, does " << (master_hosts_pi_ ? "" : "not ") <<
    "host pi values" << std::endl;
}


void MCMCSamplerStochasticDistributed::init() {
  int r;

  // In an OpenMP program: no need for thread support
  r = MPI_Init(NULL, NULL);
  mpi_error_test(r, "MPI_Init() fails");

  r = MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
  mpi_error_test(r, "MPI_Comm_set_errhandler fails");

  r = MPI_Comm_size(MPI_COMM_WORLD, &mpi_size_);
  mpi_error_test(r, "MPI_Comm_size() fails");
  r = MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank_);
  mpi_error_test(r, "MPI_Comm_rank() fails");

  std::cerr << "MPI_Init() done, rank " << mpi_rank_ <<
    " size " << mpi_size_ << std::endl;

  if (args_.forced_master_is_worker) {
    master_is_worker_ = true;
  } else {
    master_is_worker_ = (mpi_size_ == 1);
  }

  t_load_network_.start();
  MasterAwareLoadNetwork();
  t_load_network_.stop();

  // control parameters for learning
  //num_node_sample = static_cast< ::size_t>(std::sqrt(network.get_num_nodes()));
  if (args_.num_node_sample == 0) {
    // TODO: automative update..... 
    num_node_sample = N/50;
  } else {
    num_node_sample = args_.num_node_sample;
  }
  if (args_.mini_batch_size == 0) {
    // old default for STRATIFIED_RANDOM_NODE_SAMPLING
    mini_batch_size = N / 10;
  }

  sampler_stochastic_info(std::cerr);

  InitDKVStore();

  // Need to know max_perplexity_chunk_ to Init perp_
  perp_.Init(max_perplexity_chunk_);

  init_theta();

  t_populate_pi_.start();
  init_pi();
  t_populate_pi_.stop();

  pi_update_.resize(max_dkv_write_entries_);
  for (auto &p : pi_update_) {
    p = new Float[K + 1];
  }
  phi_node_.resize(max_dkv_write_entries_);
  for (auto &p : phi_node_) {
    p.resize(K + 1);
  }
  grads_beta_.resize(omp_get_max_threads());
  for (auto &g : grads_beta_) {
    g = std::vector<std::vector<Float> >(2, std::vector<Float>(K));    // gradients K*2 dimension
  }
}


std::ostream& MCMCSamplerStochasticDistributed::PrintStats(
    std::ostream& out) const {
  Timer::printHeader(out);
  out << t_load_network_ << std::endl;
  out << t_init_dkv_ << std::endl;
  out << t_populate_pi_ << std::endl;
  out << t_outer_ << std::endl;
  out << t_deploy_minibatch_ << std::endl;
  out << t_scatter_subgraph_ << std::endl;
  out << t_scatter_subgraph_marshall_edge_count_ << std::endl;
  out << t_scatter_subgraph_scatterv_edge_count_ << std::endl;
  out << t_scatter_subgraph_marshall_edges_ << std::endl;
  out << t_scatter_subgraph_scatterv_edges_ << std::endl;
  out << t_scatter_subgraph_unmarshall_ << std::endl;
  out << t_mini_batch_ << std::endl;
  out << t_nodes_in_mini_batch_ << std::endl;
  out << t_broadcast_theta_beta_ << std::endl;
  out << t_update_phi_pi_ << std::endl;
  out << t_sample_neighbor_nodes_ << std::endl;
  out << t_sample_neighbors_sample_ << std::endl;
  out << t_sample_neighbors_flatten_ << std::endl;
  out << t_load_pi_minibatch_ << std::endl;
  out << t_load_pi_neighbor_ << std::endl;
  out << t_update_phi_ << std::endl;
  out << t_barrier_phi_ << std::endl;
  out << t_update_pi_ << std::endl;
  out << t_store_pi_minibatch_ << std::endl;
  out << t_barrier_pi_ << std::endl;
  out << t_update_beta_ << std::endl;
  out << t_beta_zero_ << std::endl;
  out << t_beta_rank_ << std::endl;
  out << t_load_pi_beta_ << std::endl;
  out << t_beta_calc_grads_ << std::endl;
  out << t_beta_sum_grads_ << std::endl;
  out << t_beta_reduce_grads_ << std::endl;
  out << t_beta_update_theta_ << std::endl;
  out << t_perplexity_ << std::endl;
  out << t_load_pi_perp_ << std::endl;
  out << t_cal_edge_likelihood_ << std::endl;
  out << t_purge_pi_perp_ << std::endl;
  out << t_reduce_perp_ << std::endl;
  out << c_minibatch_chunk_size_ << std::endl;

  return out;
}


void MCMCSamplerStochasticDistributed::run() {
  /** run mini-batch based MCMC sampler, based on the sungjin's note */

  using namespace std::chrono;

  PRINT_MEM_USAGE();

  int r;

  r = MPI_Barrier(MPI_COMM_WORLD);
  mpi_error_test(r, "MPI_Barrier(initial) fails");

  t_start_ = std::chrono::system_clock::now();

  while (step_count < max_iteration && ! is_converged()) {

    t_outer_.start();
    // auto l1 = std::chrono::system_clock::now();
    //if (step_count > 200000){
    //interval = 2;
    //}

    broadcast_theta_beta();

    // requires beta at the workers
    check_perplexity(false);

    t_deploy_minibatch_.start();
    // edgeSample is nonempty only at the master
    // assigns nodes_
    EdgeSample edgeSample = deploy_mini_batch();
    t_deploy_minibatch_.stop();
    // std::cerr << "Minibatch nodes " << nodes_.size() << std::endl;

    t_update_phi_pi_.start();
    update_phi(&phi_node_);

    // barrier: peers must not read pi already updated in the current iteration
    t_barrier_phi_.start();
    r = MPI_Barrier(MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Barrier(post pi) fails");
    t_barrier_phi_.stop();

    update_pi(phi_node_);

    // barrier: ensure we read pi/phi_sum from current iteration
    t_barrier_pi_.start();
    r = MPI_Barrier(MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Barrier(post pi) fails");
    t_barrier_pi_.stop();
    t_update_phi_pi_.stop();

    t_update_beta_.start();
    update_beta(*edgeSample.first, edgeSample.second);
    t_update_beta_.stop();

    if (mpi_rank_ == mpi_master_) {
      delete edgeSample.first;
    }

    ++step_count;
    t_outer_.stop();
    // auto l2 = std::chrono::system_clock::now();

    if (step_count % stats_print_interval_ == 0) {
      PrintStats(std::cout);
    }
  }

  r = MPI_Barrier(MPI_COMM_WORLD);
  mpi_error_test(r, "MPI_Barrier(post pi) fails");

  check_perplexity(true);

  r = MPI_Barrier(MPI_COMM_WORLD);
  mpi_error_test(r, "MPI_Barrier(post pi) fails");

  PrintStats(std::cout);
}


::size_t MCMCSamplerStochasticDistributed::real_num_node_sample() const {
  return num_node_sample + 1;
}


void MCMCSamplerStochasticDistributed::init_theta() {
  if (mpi_rank_ == mpi_master_) {
    // model parameters and re-parameterization
    // since the model parameter - \pi and \beta should stay in the simplex,
    // we need to restrict the sum of probability equals to 1.  The way we
    // restrict this is using re-reparameterization techniques, where we
    // introduce another set of variables, and update them first followed by
    // updating \pi and \beta.
    // parameterization for \beta
    theta = rng_[0]->gamma(eta[0], eta[1], K, 2);
  } else {
    theta = std::vector<std::vector<Float> >(K, std::vector<Float>(2));
  }
  // std::cerr << "Ignore eta[] in random.gamma: use 100.0 and 0.01" << std::endl;
  // parameterization for \beta
  // theta = rng_[0]->->gamma(100.0, 0.01, K, 2);
}

void MCMCSamplerStochasticDistributed::beta_from_theta() {
#pragma omp parallel for
  for (::size_t k = 0; k < K; ++k) {
    beta[k] = theta[k][1] / (theta[k][0] + theta[k][1]);
  }
}


// Calculate pi[0..K> ++ phi_sum from phi[0..K>
void MCMCSamplerStochasticDistributed::pi_from_phi(
    Float* pi, const std::vector<Float> &phi) {
  Float phi_sum = std::accumulate(phi.begin(), phi.begin() + K, 0.0);
  for (::size_t k = 0; k < K; ++k) {
    pi[k] = phi[k] / phi_sum;
  }

  pi[K] = phi_sum;
}


void MCMCSamplerStochasticDistributed::init_pi() {
  std::vector<Float*> pi(max_dkv_write_entries_);
  for (auto & p : pi) {
    p = new Float[K + 1];
  }

  ::size_t servers = master_hosts_pi_ ? mpi_size_ : mpi_size_ - 1;
  ::size_t my_max = N / servers;
  int my_server = master_hosts_pi_ ? mpi_rank_ : mpi_rank_ - 1;
  if (my_server < 0) {
    my_max = 0;
  } else if (static_cast<::size_t>(my_server) < N - my_max * servers) {
    ++my_max;
  }
  int last_node = my_server;
  while (my_max > 0) {
    ::size_t chunk = std::min(max_dkv_write_entries_, my_max);
    my_max -= chunk;
    std::vector<std::vector<Float> > phi_pi(chunk);
#pragma omp parallel for // num_threads (12)
    for (::size_t j = 0; j < chunk; ++j) {
      phi_pi[j] = rng_[omp_get_thread_num()]->gamma(1, 1, 1, K)[0];
    }
#ifndef NDEBUG
    for (auto & phs : phi_pi) {
      for (auto ph : phs) {
        assert(ph >= 0.0);
      }
    }
#endif

#pragma omp parallel for // num_threads (12)
    for (::size_t j = 0; j < chunk; ++j) {
      pi_from_phi(pi[j], phi_pi[j]);
    }

    std::vector<int32_t> node(chunk);
    for (::size_t j = 0; j < chunk; ++j) {
      node[j] = last_node;
      last_node += servers;
    }

    d_kv_store_->WriteKVRecords(node, constify(pi));
    d_kv_store_->PurgeKVRecords();
    std::cerr << ".";
  }
  std::cerr << std::endl;

  for (auto & p : pi) {
    delete[] p;
  }
}


void MCMCSamplerStochasticDistributed::check_perplexity(bool force) {
  if (force || (step_count - 1) % interval == 0) {
    using namespace std::chrono;

    t_perplexity_.start();
    // TODO load pi for the held-out set to calculate perplexity
    Float ppx_score = cal_perplexity_held_out();
    t_perplexity_.stop();
    if (mpi_rank_ == mpi_master_) {
      auto t_now = system_clock::now();
      auto t_ms = duration_cast<milliseconds>(t_now - t_start_).count();
      std::cout << "average_count is: " << average_count << " ";
      std::cout << std::fixed
                << "step count: " << step_count
                << " time: " << std::setprecision(3) << (t_ms / 1000.0)
                << " perplexity for hold out set: " << std::setprecision(12) <<
                ppx_score << std::endl;
      double seconds = t_ms / 1000.0;
      timings_.push_back(seconds);
    }

    ppxs_heldout_cb_.push_back(ppx_score);
  }
}


void MCMCSamplerStochasticDistributed::ScatterSubGraph(
    const std::vector<std::vector<int32_t> > &subminibatch) {
  std::vector<int32_t> set_size(nodes_.size());
  std::vector<Vertex> flat_subgraph;
  int r;

  local_network_.reset();

  if (mpi_rank_ == mpi_master_) {
    std::vector<int32_t> size_count(mpi_size_);	        // FIXME: lift to class
    std::vector<int32_t> size_displ(mpi_size_);	        // FIXME: lift to class
    std::vector<int32_t> subgraph_count(mpi_size_);	// FIXME: lift to class
    std::vector<int32_t> subgraph_displ(mpi_size_);	// FIXME: lift to class
    std::vector<int32_t> workers_set_size;

    // Data dependency on workers_set_size
    t_scatter_subgraph_marshall_edge_count_.start();
    for (int i = 0; i < mpi_size_; ++i) {
      subgraph_count[i] = 0;
      for (::size_t j = 0; j < subminibatch[i].size(); ++j) {
        int32_t fan_out = network.get_fan_out(subminibatch[i][j]);
        workers_set_size.push_back(fan_out);
        subgraph_count[i] += fan_out;
      }
      size_count[i] = subminibatch[i].size();
    }

    size_displ[0] = 0;
    subgraph_displ[0] = 0;
    for (int i = 1; i < mpi_size_; ++i) {
      size_displ[i] = size_displ[i - 1] + size_count[i - 1];
      subgraph_displ[i] = subgraph_displ[i - 1] + subgraph_count[i - 1];
    }
    t_scatter_subgraph_marshall_edge_count_.stop();

    // Scatter the fanout of each minibatch node
    t_scatter_subgraph_scatterv_edge_count_.start();
    r = MPI_Scatterv(workers_set_size.data(),
                     size_count.data(),
                     size_displ.data(),
                     MPI_INT,
                     set_size.data(),
                     set_size.size(),
                     MPI_INT,
                     mpi_master_,
                     MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of minibatch fails");
    t_scatter_subgraph_scatterv_edge_count_.stop();

    // Marshall the subgraphs
    t_scatter_subgraph_marshall_edges_.start();
    ::size_t total_edges = np::sum(workers_set_size);
    std::vector<Vertex> subgraphs(total_edges);
#pragma omp parallel for // num_threads (12)
    for (int i = 0; i < mpi_size_; ++i) {
      ::size_t marshalled = subgraph_displ[i];
      for (::size_t j = 0; j < subminibatch[i].size(); ++j) {
        Vertex* marshall = subgraphs.data() + marshalled;
        ::size_t n = network.marshall_edges_from(subminibatch[i][j],
                                                 marshall);
        // std::cerr << "Marshall to peer " << i << ": " << n <<
        //   " edges" << std::endl;
        marshalled += n;
      }
    }
    t_scatter_subgraph_marshall_edges_.stop();

    // Scatter the marshalled subgraphs
    t_scatter_subgraph_scatterv_edges_.start();
    ::size_t total_set_size = np::sum(set_size);
    flat_subgraph.resize(total_set_size);
    r = MPI_Scatterv(subgraphs.data(),
                     subgraph_count.data(),
                     subgraph_displ.data(),
                     MPI_INT,
                     flat_subgraph.data(),
                     flat_subgraph.size(),
                     MPI_INT,
                     mpi_master_,
                     MPI_COMM_WORLD);
    t_scatter_subgraph_scatterv_edges_.stop();

  } else {
    // Scatter the fanout of each minibatch node
    t_scatter_subgraph_scatterv_edge_count_.start();
    r = MPI_Scatterv(NULL,
                     NULL,
                     NULL,
                     MPI_INT,
                     set_size.data(),
                     set_size.size(),
                     MPI_INT,
                     mpi_master_,
                     MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of minibatch fails");
    t_scatter_subgraph_scatterv_edge_count_.stop();

    // Scatter the marshalled subgraphs
    t_scatter_subgraph_scatterv_edges_.start();
    ::size_t total_set_size = np::sum(set_size);
    flat_subgraph.resize(total_set_size);
    r = MPI_Scatterv(NULL,
                     NULL,
                     NULL,
                     MPI_INT,
                     flat_subgraph.data(),
                     flat_subgraph.size(),
                     MPI_INT,
                     mpi_master_,
                     MPI_COMM_WORLD);
    t_scatter_subgraph_scatterv_edges_.stop();
  }

  t_scatter_subgraph_unmarshall_.start();
  ::size_t offset = 0;
  for (::size_t i = 0; i < set_size.size(); ++i) {
    Vertex* marshall = &flat_subgraph[offset];
    local_network_.unmarshall_local_graph(i, marshall, set_size[i]);
    offset += set_size[i];
  }
  t_scatter_subgraph_unmarshall_.stop();
}


EdgeSample MCMCSamplerStochasticDistributed::deploy_mini_batch() {
  std::vector<std::vector<int> > subminibatch;
  std::vector<int32_t> minibatch_chunk(mpi_size_);      // FIXME: lift to class
  std::vector<int32_t> scatter_minibatch;               // FIXME: lift to class
  std::vector<int32_t> scatter_displs(mpi_size_);       // FIXME: lift to class
  int		r;
  EdgeSample edgeSample;

  if (mpi_rank_ == mpi_master_) {
    // std::cerr << "Invoke sample_mini_batch" << std::endl;
    t_mini_batch_.start();
    edgeSample = network.sample_mini_batch(mini_batch_size, strategy);
    t_mini_batch_.stop();
    const MinibatchSet &mini_batch = *edgeSample.first;
    // std::cerr << "Done sample_mini_batch" << std::endl;

    // iterate through each node in the mini batch.
    t_nodes_in_mini_batch_.start();
    MinibatchNodeSet nodes = nodes_in_batch(mini_batch);
    t_nodes_in_mini_batch_.stop();
    // std::cerr << "mini_batch size " << mini_batch.size() <<
    //   " num_node_sample " << num_node_sample << std::endl;

    subminibatch.resize(mpi_size_);	// FIXME: lift to class, size is static

    ::size_t workers = master_is_worker_ ? mpi_size_ : mpi_size_ - 1;
    ::size_t upper_bound = (nodes.size() + workers - 1) / workers;
    std::unordered_set<Vertex> unassigned;
    for (auto n: nodes) {
      ::size_t owner = node_owner(n);
      if (subminibatch[owner].size() == upper_bound) {
        unassigned.insert(n);
      } else {
        subminibatch[owner].push_back(n);
      }
    }

    ::size_t i = master_is_worker_ ? 0 : 1;
    for (auto n: unassigned) {
      while (subminibatch[i].size() == upper_bound) {
        ++i;
        assert(i < static_cast< ::size_t>(mpi_size_));
      }
      subminibatch[i].push_back(n);
    }

    scatter_minibatch.clear();
    int32_t running_sum = 0;
    for (int i = 0; i < mpi_size_; ++i) {
      minibatch_chunk[i] = subminibatch[i].size();
      scatter_displs[i] = running_sum;
      running_sum += subminibatch[i].size();
      scatter_minibatch.insert(scatter_minibatch.end(),
                               subminibatch[i].begin(),
                               subminibatch[i].end());
    }
  }

  int32_t my_minibatch_size;
  r = MPI_Scatter(minibatch_chunk.data(), 1, MPI_INT,
                  &my_minibatch_size, 1, MPI_INT,
                  mpi_master_, MPI_COMM_WORLD);
  mpi_error_test(r, "MPI_Scatter of minibatch chunks fails");
  nodes_.resize(my_minibatch_size);
  if (nodes_.size() > pi_update_.size()) {
    PRINT_MEM_USAGE();
    std::ostringstream msg;
    msg << "Out of bounds for pi_update_/phi_node_: bounds " << pi_update_.size() << " required " << nodes_.size();
    throw BufferSizeException(msg.str());
  }

  if (mpi_rank_ == mpi_master_) {
    // TODO Master scatters the minibatch nodes over the workers,
    // preferably with consideration for both load balance and locality
    r = MPI_Scatterv(scatter_minibatch.data(),
                     minibatch_chunk.data(),
                     scatter_displs.data(),
                     MPI_INT,
                     nodes_.data(),
                     my_minibatch_size,
                     MPI_INT,
                     mpi_master_,
                     MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of minibatch fails");

  } else {
    r = MPI_Scatterv(NULL, NULL, NULL, MPI_INT,
                     nodes_.data(), my_minibatch_size, MPI_INT,
                     mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of minibatch fails");
  }


  if (! args_.REPLICATED_NETWORK) {
    t_scatter_subgraph_.start();
    ScatterSubGraph(subminibatch);
    t_scatter_subgraph_.stop();
  }

  return edgeSample;
}


void MCMCSamplerStochasticDistributed::DrawNeighbors(
    const int32_t* chunk_nodes,
    ::size_t n_chunk_nodes,
    int32_t *flat_neighbors) {
  const ::size_t p = real_num_node_sample();
  t_sample_neighbors_sample_.start();
  c_minibatch_chunk_size_.tick(n_chunk_nodes);
#pragma omp parallel for // schedule(static, 1)
  for (::size_t i = 0; i < n_chunk_nodes; ++i) {
    const Vertex node = chunk_nodes[i];
    auto* rng = rng_[omp_get_thread_num()];
    // std::unordered_set neighbors;
    FixedSizeSet neighbors(p);
    // std::vector<Vertex> neighbors;
    // sample a mini-batch of neighbors
    while (neighbors.size() < p) {
      const Vertex neighborId = rng->randint(0, N - 1);
      if (neighborId != node
          && neighbors.find(neighborId) == neighbors.end()
          // && std::find(neighbors.begin(), neighbors.end(), neighborId) == neighbors.end()
          ) {
        const Edge edge = Edge(std::min(node, neighborId),
                               std::max(node, neighborId));
        if (! edge.in(held_out_test_)) {
          neighbors.insert(neighborId);
          // neighbors.push_back(neighborId);
        }
      }
    }

    // Cannot use flat_neighbors.insert() because it may (concurrently)
    // attempt to resize flat_neighbors.
    ::size_t j = i * p;
    for (auto n : neighbors) {
      flat_neighbors[j] = n;
      ++j;
    }
  }
  t_sample_neighbors_sample_.stop();
}


void MCMCSamplerStochasticDistributed::update_phi(
    std::vector<std::vector<Float> >* phi_node) {
  std::vector<Float*> pi_node;
  std::vector<Float*> pi_neighbor;
  std::vector<int32_t> flat_neighbors;

  Float eps_t = get_eps_t();

  for (::size_t chunk_start = 0;
       chunk_start < nodes_.size();
       chunk_start += max_minibatch_chunk_) {
    ::size_t chunk = std::min(max_minibatch_chunk_,
                              nodes_.size() - chunk_start);

    std::vector<int32_t> chunk_nodes(nodes_.begin() + chunk_start,
                                     nodes_.begin() + chunk_start + chunk);

    // ************ sample neighbor nodes in parallel at each host ******
    // std::cerr << "Sample neighbor nodes" << std::endl;
    pi_neighbor.resize(chunk_nodes.size() * real_num_node_sample());
    flat_neighbors.resize(chunk_nodes.size() * real_num_node_sample());
    t_sample_neighbor_nodes_.start();
    DrawNeighbors(chunk_nodes.data(), chunk_nodes.size(), flat_neighbors.data());
    t_sample_neighbor_nodes_.stop();

    // ************ load minibatch node pi from D-KV store **************
    t_load_pi_minibatch_.start();
    pi_node.resize(chunk_nodes.size());
    d_kv_store_->ReadKVRecords(pi_node, chunk_nodes, DKV::RW_MODE::READ_ONLY);
    t_load_pi_minibatch_.stop();

    // ************ load neighor pi from D-KV store **********
    t_load_pi_neighbor_.start();
    d_kv_store_->ReadKVRecords(pi_neighbor,
                               flat_neighbors,
                               DKV::RW_MODE::READ_ONLY);
    t_load_pi_neighbor_.stop();

    t_update_phi_.start();
#pragma omp parallel for // num_threads (12)
    for (::size_t i = 0; i < chunk_nodes.size(); ++i) {
      Vertex node = chunk_nodes[i];
      update_phi_node(chunk_start + i, node, pi_node[i],
                      flat_neighbors.begin() + i * real_num_node_sample(),
                      pi_neighbor.begin() + i * real_num_node_sample(),
                      eps_t, rng_[omp_get_thread_num()],
                      &(*phi_node)[chunk_start + i]);
    }
    t_update_phi_.stop();

    d_kv_store_->PurgeKVRecords();
  }
}


void MCMCSamplerStochasticDistributed::update_phi_node(
    ::size_t index, Vertex i, const Float* pi_node,
    const std::vector<int32_t>::iterator &neighbors,
    const std::vector<Float*>::iterator &pi,
    Float eps_t, Random::Random* rnd,
    std::vector<Float>* phi_node	// out parameter
    ) {

  Float phi_i_sum = pi_node[K];
  if (phi_i_sum == FLOAT(0.0)) {
    std::cerr << "Ooopppssss.... phi_i_sum " << phi_i_sum << std::endl;
  }
  std::vector<Float> grads(K, 0.0);	// gradient for K classes

  for (::size_t ix = 0; ix < real_num_node_sample(); ++ix) {
    int32_t neighbor = neighbors[ix];
    if (i != neighbor) {
      int y_ab = 0;		// observation
      if (args_.REPLICATED_NETWORK) {
        Edge edge(std::min(i, neighbor), std::max(i, neighbor));
        if (edge.in(network.get_linked_edges())) {
          y_ab = 1;
        }
      } else {
        Edge edge(index, neighbor);
        if (local_network_.find(edge)) {
          y_ab = 1;
        }
      }

      std::vector<Float> probs(K);
      Float e = (y_ab == 1) ? epsilon : 1.0 - epsilon;
      for (::size_t k = 0; k < K; ++k) {
        Float f = (y_ab == 1) ? (beta[k] - epsilon) : (epsilon - beta[k]);
        probs[k] = pi_node[k] * (pi[ix][k] * f + e);
      }

      Float prob_sum = np::sum(probs);
      // std::cerr << std::fixed << std::setprecision(12) << "node " << i <<
      //    " neighb " << neighbor << " prob_sum " << prob_sum <<
      //    " phi_i_sum " << phi_i_sum <<
      //    " #sample " << real_num_node_sample() << std::endl;
      for (::size_t k = 0; k < K; ++k) {
        assert(phi_i_sum > 0);
        grads[k] += ((probs[k] / prob_sum) / pi_node[k] - 1.0) / phi_i_sum;
      }
    } else {
      std::cerr << "Skip self loop <" << i << "," << neighbor << ">" <<
        std::endl;
    }
  }

  std::vector<Float> noise = rnd->randn(K);	// random gaussian noise.
  Float Nn = (1.0 * N) / num_node_sample;
  // update phi for node i
  for (::size_t k = 0; k < K; ++k) {
    Float phi_node_k = pi_node[k] * phi_i_sum;
    assert(phi_node_k > FLOAT(0.0));
    phi_node_k = std::abs(phi_node_k + eps_t / 2 * (alpha - phi_node_k +
                                                    Nn * grads[k])
                          + sqrt(eps_t * phi_node_k) * noise[k]
                         );
    if (phi_node_k < MCMC_NONZERO_GUARD) {
      (*phi_node)[k] = MCMC_NONZERO_GUARD;
    } else {
      (*phi_node)[k] = phi_node_k;
    }
    assert((*phi_node)[k] > FLOAT(0.0));
  }
}


void MCMCSamplerStochasticDistributed::update_pi(
    const std::vector<std::vector<Float> >& phi_node) {
  // calculate and store updated values for pi/phi_sum

  if (mpi_rank_ != mpi_master_ || master_is_worker_) {
    t_update_pi_.start();
#pragma omp parallel for // num_threads (12)
    for (::size_t i = 0; i < nodes_.size(); ++i) {
      pi_from_phi(pi_update_[i], phi_node[i]);
    }
    t_update_pi_.stop();

    t_store_pi_minibatch_.start();
    d_kv_store_->WriteKVRecords(nodes_, constify(pi_update_));
    t_store_pi_minibatch_.stop();
    d_kv_store_->PurgeKVRecords();
  }
}


void MCMCSamplerStochasticDistributed::broadcast_theta_beta() {
  t_broadcast_theta_beta_.start();
  if (! args_.REPLICATED_NETWORK) {
    std::vector<Float> theta_marshalled(2 * K);   // FIXME: lift to class level
    if (mpi_rank_ == mpi_master_) {
      for (::size_t k = 0; k < K; ++k) {
        for (::size_t i = 0; i < 2; ++i) {
          theta_marshalled[2 * k + i] = theta[k][i];
        }
      }
    }
    int r = MPI_Bcast(theta_marshalled.data(), theta_marshalled.size(),
                      FLOATTYPE_MPI, mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Bcast(theta) fails");
    if (mpi_rank_ != mpi_master_) {
      for (::size_t k = 0; k < K; ++k) {
        for (::size_t i = 0; i < 2; ++i) {
          theta[k][i] = theta_marshalled[2 * k + i];
        }
      }
    }
  }
  //-------- after broadcast of theta, replicate this at all peers:
  beta_from_theta();
  t_broadcast_theta_beta_.stop();
}


void MCMCSamplerStochasticDistributed::scatter_minibatch_for_theta(
    const MinibatchSet &mini_batch,
    std::vector<EdgeMapItem>* mini_batch_slice) {
  int   r;
  std::vector<unsigned char> flattened_minibatch;
  std::vector<int32_t> scatter_size(mpi_size_);
  std::vector<int32_t> scatter_displs(mpi_size_);

  if (mpi_rank_ == mpi_master_) {
    flattened_minibatch.resize(mini_batch.size() * sizeof(EdgeMapItem));
    ::size_t chunk = mini_batch.size() / mpi_size_;
    ::size_t surplus = mini_batch.size() - chunk * mpi_size_;
    ::size_t running_sum = 0;
    ::size_t i;
    for (i = 0; i < surplus; ++i) {
      scatter_size[i] = (chunk + 1) * sizeof(EdgeMapItem);
      scatter_displs[i] = running_sum;
      running_sum += (chunk + 1) * sizeof(EdgeMapItem);
    }
    for (; i < (::size_t)mpi_size_; ++i) {
      scatter_size[i] = chunk * sizeof(EdgeMapItem);
      scatter_displs[i] = running_sum;
      running_sum += chunk * sizeof(EdgeMapItem);
    }
    auto *marshall = flattened_minibatch.data();
    for (auto e: mini_batch) {
      EdgeMapItem ei(e, e.in(network.get_linked_edges()));
      memcpy(marshall, &ei, sizeof ei);
      marshall += sizeof ei;
    }
  }

  int32_t my_minibatch_bytes;
  r = MPI_Scatter(scatter_size.data(), 1, MPI_INT,
                  &my_minibatch_bytes, 1, MPI_INT,
                  mpi_master_, MPI_COMM_WORLD);
  mpi_error_test(r, "MPI_Scatter of minibatch sizes for update_beta fails");

  mini_batch_slice->resize(my_minibatch_bytes / sizeof(EdgeMapItem));
  if (mpi_rank_ == mpi_master_) {
    r = MPI_Scatterv(flattened_minibatch.data(), scatter_size.data(),
                     scatter_displs.data(), MPI_BYTE,
                     mini_batch_slice->data(), my_minibatch_bytes, MPI_BYTE,
                     mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of minibatch for update_beta fails");

  } else {
    r = MPI_Scatterv(NULL, NULL,
                     NULL, MPI_BYTE,
                     mini_batch_slice->data(), my_minibatch_bytes, MPI_BYTE,
                     mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "MPI_Scatterv of minibatch for update_beta fails");
  }
}


void MCMCSamplerStochasticDistributed::beta_calc_grads(
    const std::vector<EdgeMapItem>& mini_batch_slice) {
  t_beta_zero_.start();
#pragma omp parallel for
  for (int i = 0; i < omp_get_max_threads(); ++i) {
    for (::size_t k = 0; k < K; ++k) {
      grads_beta_[i][0][k] = 0.0;
      grads_beta_[i][1][k] = 0.0;
    }
  }

  // sums = np.sum(self.__theta,1)
  std::vector<Float> theta_sum(theta.size());
  std::transform(theta.begin(), theta.end(), theta_sum.begin(),
                 np::sum<Float>);
  t_beta_zero_.stop();

  t_beta_rank_.start();
  std::unordered_map<Vertex, Vertex> node_rank;
  std::vector<Vertex> nodes;
  for (auto e : mini_batch_slice) {
    Vertex i = e.edge.first;
    Vertex j = e.edge.second;
    if (node_rank.find(i) == node_rank.end()) {
      ::size_t next = node_rank.size();
      node_rank[i] = next;
      nodes.push_back(i);
    }
    if (node_rank.find(j) == node_rank.end()) {
      ::size_t next = node_rank.size();
      node_rank[j] = next;
      nodes.push_back(j);
    }
    assert(node_rank.size() == nodes.size());
  }
  t_beta_rank_.stop();

  t_load_pi_beta_.start();
  std::vector<Float*> pi(node_rank.size());
  d_kv_store_->ReadKVRecords(pi, nodes, DKV::RW_MODE::READ_ONLY);
  t_load_pi_beta_.stop();

  // update gamma, only update node in the grad
  t_beta_calc_grads_.start();
#pragma omp parallel for // num_threads (12)
  for (::size_t e = 0; e < mini_batch_slice.size(); ++e) {
    const auto *edge = &mini_batch_slice[e];
    std::vector<Float> probs(K);

    int y = (int)edge->is_edge;
    Vertex i = node_rank[edge->edge.first];
    Vertex j = node_rank[edge->edge.second];

    Float pi_sum = 0.0;
    for (::size_t k = 0; k < K; ++k) {
      // Note: this is the KV-store cached pi, not the Learner item
      Float f = pi[i][k] * pi[j][k];
      pi_sum += f;
      if (y == 1) {
        probs[k] = beta[k] * f;
      } else {
        probs[k] = (1.0 - beta[k]) * f;
      }
    }

    Float prob_0 = ((y == 1) ? epsilon : (1.0 - epsilon)) * (1.0 - pi_sum);
    Float prob_sum = np::sum(probs) + prob_0;
    for (::size_t k = 0; k < K; ++k) {
      Float f = probs[k] / prob_sum;
      Float one_over_theta_sum = 1.0 / theta_sum[k];
      grads_beta_[omp_get_thread_num()][0][k] += f * ((1 - y) / theta[k][0] -
                                                      one_over_theta_sum);
      grads_beta_[omp_get_thread_num()][1][k] += f * (y / theta[k][1] -
                                                      one_over_theta_sum);
    }
  }
  t_beta_calc_grads_.stop();
}


void MCMCSamplerStochasticDistributed::beta_sum_grads() {
  int r;

  t_beta_sum_grads_.start();
#pragma omp parallel for
  for (::size_t k = 0; k < K; ++k) {
    for (int i = 1; i < omp_get_max_threads(); ++i) {
      grads_beta_[0][0][k] += grads_beta_[i][0][k];
      grads_beta_[0][1][k] += grads_beta_[i][1][k];
    }
  }
  t_beta_sum_grads_.stop();

  //-------- reduce(+) of the grads_[0][*][0,1] to the master
  t_beta_reduce_grads_.start();
  if (mpi_rank_ == mpi_master_) {
    r = MPI_Reduce(MPI_IN_PLACE, grads_beta_[0][0].data(), K, FLOATTYPE_MPI,
                   MPI_SUM, mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "Reduce/plus of grads_beta_[0][0] fails");
    r = MPI_Reduce(MPI_IN_PLACE, grads_beta_[0][1].data(), K, FLOATTYPE_MPI,
                   MPI_SUM, mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "Reduce/plus of grads_beta_[0][1] fails");
  } else {
    r = MPI_Reduce(grads_beta_[0][0].data(), NULL, K, FLOATTYPE_MPI,
                   MPI_SUM, mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "Reduce/plus of grads_beta_[0][0] fails");
    r = MPI_Reduce(grads_beta_[0][1].data(), NULL, K, FLOATTYPE_MPI,
                   MPI_SUM, mpi_master_, MPI_COMM_WORLD);
    mpi_error_test(r, "Reduce/plus of grads_beta_[0][1] fails");
  }
  t_beta_reduce_grads_.stop();
}


void MCMCSamplerStochasticDistributed::beta_update_theta(Float scale) {
  if (mpi_rank_ == mpi_master_) {
    t_beta_update_theta_.start();
    Float eps_t = get_eps_t();
    // random noise.
    std::vector<std::vector<Float> > noise = rng_[0]->randn(K, 2);
#pragma omp parallel for
    for (::size_t k = 0; k < K; ++k) {
      for (::size_t i = 0; i < 2; ++i) {
        Float f = std::sqrt(eps_t * theta[k][i]);
        theta[k][i] = std::abs(theta[k][i] +
                               eps_t / 2.0 * (eta[i] - theta[k][i] +
                                              scale * grads_beta_[0][i][k])
                               + f * noise[k][i]
                              );
        if (theta[k][i] < MCMC_NONZERO_GUARD) {
          theta[k][i] = MCMC_NONZERO_GUARD;
        }
      }
    }
    t_beta_update_theta_.stop();
  }
}


void MCMCSamplerStochasticDistributed::update_beta(
    const MinibatchSet &mini_batch, Float scale) {
  std::vector<EdgeMapItem> mini_batch_slice;

  scatter_minibatch_for_theta(mini_batch, &mini_batch_slice);

  beta_calc_grads(mini_batch_slice);

  beta_sum_grads();

  beta_update_theta(scale);

  d_kv_store_->PurgeKVRecords();
}


void MCMCSamplerStochasticDistributed::reduce_plus(const perp_accu &in,
                                                   perp_accu* accu) {
  int r;
  uint64_t count[2] = { in.link.count, in.non_link.count };
  Float likelihood[2] = { in.link.likelihood, in.non_link.likelihood };

  r = MPI_Allreduce(MPI_IN_PLACE, count, 2, MPI_UNSIGNED_LONG, MPI_SUM,
                    MPI_COMM_WORLD);
  mpi_error_test(r, "Reduce/plus of perplexity counts fails");
  r = MPI_Allreduce(MPI_IN_PLACE, likelihood, 2, FLOATTYPE_MPI, MPI_SUM,
                    MPI_COMM_WORLD);
  mpi_error_test(r, "Reduce/plus of perplexity likelihoods fails");

  accu->link.count = count[0];
  accu->non_link.count = count[1];
  accu->link.likelihood = likelihood[0];
  accu->non_link.likelihood = likelihood[1];
}


/**
 * calculate the perplexity for data.
 * perplexity defines as exponential of negative average log likelihood. 
 * formally:
 *     ppx = exp(-1/N * \sum){i}^{N}log p(y))
 * 
 * we calculate average log likelihood for link and non-link separately, with the 
 * purpose of weighting each part proportionally. (the reason is that we sample 
 * the equal number of link edges and non-link edges for held out data and test data,
 * which is not true representation of actual data set, which is extremely sparse.
 */
Float MCMCSamplerStochasticDistributed::cal_perplexity_held_out() {

  for (auto & a : perp_.accu_) {
    a.link.reset();
    a.non_link.reset();
  }

  for (::size_t chunk_start = 0;
       chunk_start < perp_.data_.size();
       chunk_start += max_perplexity_chunk_) {
    ::size_t chunk = std::min(max_perplexity_chunk_,
                              perp_.data_.size() - chunk_start);

    // chunk_size is about edges; nodes are at 2i and 2i+1
    std::vector<int32_t> chunk_nodes(perp_.nodes_.begin() + 2 * chunk_start,
                                     perp_.nodes_.begin() + 2 * (chunk_start +
                                                                 chunk));

    t_load_pi_perp_.start();
    d_kv_store_->ReadKVRecords(perp_.pi_, chunk_nodes, DKV::RW_MODE::READ_ONLY);
    t_load_pi_perp_.stop();

    t_cal_edge_likelihood_.start();
#pragma omp parallel for
    for (::size_t i = chunk_start; i < chunk_start + chunk; ++i) {
      const auto& edge_in = perp_.data_[i];
      // the index into the nodes/pi vectors is double the index into the
      // edge vector (+ 1)
      Vertex a = 2 * (i - chunk_start);
      Vertex b = 2 * (i - chunk_start) + 1;
      Float edge_likelihood = cal_edge_likelihood(perp_.pi_[a], perp_.pi_[b],
                                                   edge_in.is_edge, beta);
      if (std::isnan(edge_likelihood)) {
        std::cerr << "edge_likelihood is NaN; potential bug" << std::endl;
      }

      //cout<<"AVERAGE COUNT: " <<average_count;
      ppx_per_heldout_edge_[i] = (ppx_per_heldout_edge_[i] * (average_count-1) + edge_likelihood)/(average_count);
      // Edge e(chunk_nodes[a], chunk_nodes[b]);
      // std::cout << std::fixed << std::setprecision(12) << e <<
      //   " in? " << (edge_in.is_edge ? "True" : "False") <<
      //   " -> " << edge_likelihood << " av. " << average_count <<
      //   " ppx[" << i << "] " << ppx_per_heldout_edge_[i] << std::endl;
      if (edge_in.is_edge) {
        perp_.accu_[omp_get_thread_num()].link.count++;
        perp_.accu_[omp_get_thread_num()].link.likelihood += std::log(ppx_per_heldout_edge_[i]);
        //link_likelihood += edge_likelihood;

        if (std::isnan(perp_.accu_[omp_get_thread_num()].link.likelihood)){
          std::cerr << "link_likelihood is NaN; potential bug" << std::endl;
        }
      } else {
        perp_.accu_[omp_get_thread_num()].non_link.count++;
        //perp_.accu_[omp_get_thread_num()].non_link.likelihood +=
        //  edge_likelihood;
        perp_.accu_[omp_get_thread_num()].non_link.likelihood +=
          std::log(ppx_per_heldout_edge_[i]);
        if (std::isnan(perp_.accu_[omp_get_thread_num()].non_link.likelihood)){
          std::cerr << "non_link_likelihood is NaN; potential bug" << std::endl;
        }
      }
    }

    t_purge_pi_perp_.start();
    d_kv_store_->PurgeKVRecords();
    t_purge_pi_perp_.stop();
  }

  for (auto i = 1; i < omp_get_max_threads(); ++i) {
    perp_.accu_[0].link.count += perp_.accu_[i].link.count;
    perp_.accu_[0].link.likelihood += perp_.accu_[i].link.likelihood;
    perp_.accu_[0].non_link.count += perp_.accu_[i].non_link.count;
    perp_.accu_[0].non_link.likelihood += perp_.accu_[i].non_link.likelihood;
  }

  t_cal_edge_likelihood_.stop();

  // std::cout << std::setprecision(12) << "ratio " << link_ratio <<
  //   " count: link " << link_count << " " << link_likelihood <<
  //   " non-link " << non_link_count << " " << non_link_likelihood <<
  //   std::endl;

  // weight each part proportionally.
  /*
     avg_likelihood = self._link_ratio*(link_likelihood/link_count) + \
     (1-self._link_ratio)*(non_link_likelihood/non_link_count)
     */

  perp_accu accu;
  t_reduce_perp_.start();
  reduce_plus(perp_.accu_[0], &accu);
  t_reduce_perp_.stop();

  // direct calculation.
  Float avg_likelihood = 0.0;
  if (accu.link.count + accu.non_link.count != 0){
    avg_likelihood = (accu.link.likelihood + accu.non_link.likelihood) /
      (accu.link.count + accu.non_link.count);
  }

  average_count = average_count + 1;

  return (-avg_likelihood);
}


int MCMCSamplerStochasticDistributed::node_owner(Vertex node) const {
  if (master_hosts_pi_) {
    return node % mpi_size_;
  } else {
    return 1 + (node % (mpi_size_ - 1));
  }
}


void MCMCSamplerStochasticDistributed::mpi_error_test(
    int r, const std::string &message) {
  if (r != MPI_SUCCESS) {
    throw MCMCException("MPI error " + r + message);
  }
}

}	// namespace learning
}	// namespace mcmc

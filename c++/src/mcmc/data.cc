#include "mcmc/data.h"

#include <chrono>

#include "mcmc/fileio.h"

#include <fstream>

namespace mcmc {

// FIXME: does not belong here, but in ? np? misc? stats?
void print_mem_usage(std::ostream &s) {
  using namespace std::chrono;

  static auto t_start = system_clock::now();

  static const int64_t MEGA = 1 << 20;
  static int64_t pagesize = 0;
  static std::string proc_statm;
  if (pagesize == 0) {
    pagesize = sysconf(_SC_PAGESIZE);
    std::ostringstream ss;
    ss << "/proc/" << getpid() << "/statm";
    proc_statm = ss.str();
    s << "For memory query file " << proc_statm << std::endl;
  }

  auto t_now = system_clock::now();
  auto t_ms = duration_cast<milliseconds>(t_now - t_start).count();

  std::ifstream statm(proc_statm);
  if (!statm) {
    std::cerr << "Cannot open input file \"" << proc_statm << "\"" << std::endl;
    return;
  }

  ::size_t total;
  ::size_t resident;
  ::size_t shared;
  ::size_t text;
  ::size_t data;
  ::size_t library;
  ::size_t dirty;
  statm >> total >> resident >> shared >> text >> data >> library >> dirty;

  s << std::fixed << std::setprecision(3) << (t_ms / 1000.0) << " Memory usage: total " << ((total * pagesize) / MEGA) << "MB "
    << "resident " << ((resident * pagesize) / MEGA) << "MB " << std::endl;
}


#ifdef MCMC_ENABLE_DISTRIBUTED

GoogleHashEdgeSet::GoogleHashEdgeSet(const GoogleHashMap& hash_map,
                                     int rank, int root, MPI_Comm comm) {
  uint64_t size = static_cast<uint64_t>(hash_map.size());

  int r;
  r = MPI_Bcast(&size, 1, MPI_LONG, root, comm);
  if (r != MPI_SUCCESS) {
    throw MCMCException(std::string("MPI error ") + std::to_string(r) + std::string(": broadcast graph size fails"));
  }
  std::vector<Vertex> flat(2 * size);
  if (rank == root) {
    // marshall
    ::size_t i = 0;
    for (auto e : hash_map) {
      flat[i] = e.first.first;
      i++;
      flat[i] = e.first.second;
      i++;
    }
  }

  r = MPI_Bcast(flat.data(), flat.size(), MPI_INT, root, comm);
  if (r != MPI_SUCCESS) {
    throw MCMCException(std::string("MPI error ") + std::to_string(r) + std::string(": broadcast graph data fails"));
  }

  // unmarshall
  for (::size_t i = 0; i < flat.size(); ++i) {
    Edge e;
    e.first = flat[i];
    i++;
    e.second = flat[i];
    insert(e);
  }
}

#endif  // def MCMC_ENABLE_DISTRIBUTED


NetworkGraph::NetworkGraph(const std::string &filename, ::size_t progress) {
  FileHandle f(filename, true, "r");

  // Read linked_edges
  int32_t N;
  f.read_fully(&N, sizeof N);
  ::size_t num_edges = 0;
  edges_at_.resize(N);
  for (int32_t i = 0; i < N; ++i) {
    if (progress != 0 && i % progress == 0) { 
      std::cerr << "Node + edgeset read " << i << std::endl;
      print_mem_usage(std::cerr);
    }
    edges_at_[i].read_metadata(f.handle());
    edges_at_[i].read_nopointer_data(f.handle());
    num_edges += edges_at_[i].size();
  }

  if (progress != 0) {
    print_mem_usage(std::cerr);
  }
}

Edge::Edge(std::istream &s) { (void)get(s); }

std::ostream &Edge::put(std::ostream &s) const {
  s << std::setw(1) << "(" << first << ", " << second << ")";
  return s;
}

char Edge::consume(std::istream &s, char expect) {
  char c;
  while (true) {
    c = s.get();
    if (isspace(c)) {
      continue;
    }
    if (c != expect) {
      std::ostringstream os;
      os << "Expect " << expect << ", get '" << c << "'";
      throw MalformattedException(os.str());
    }
    return c;
  }
}

std::istream &Edge::get(std::istream &s) {
  consume(s, '(');
  s >> first;
  consume(s, ',');
  s >> second;
  consume(s, ')');

  return s;
}

std::ostream &operator<<(std::ostream &s, const Edge &e) { return e.put(s); }

std::istream &operator>>(std::istream &s, Edge &e) { return e.get(s); }

std::ostream& dump(std::ostream& out, const NetworkGraph& graph) {
  for (auto e : graph) {
    if (e.first < e.second) {
      out << e.first << "\t" << e.second << std::endl;
    }
  }

  return out;
}

std::ostream& dump(std::ostream& out, const EdgeMap &s) {
  for (auto e = s.begin(); e != s.end(); e++) {
    out << e->first << ": " << e->second << std::endl;
  }

  return out;
}

Data::Data(const void *V, const NetworkGraph *E, Vertex N,
           const std::string &header)
    : V(V), E(E), N(N), header_(header) {}

Data::~Data() {
  // delete const_cast<void *>(V); FIXME: somebody must delete V; the 'owner'
  // of this dataset, I presume
  delete const_cast<NetworkGraph *>(E);
}

void Data::dump_data() const {
  // std::cout << "Edge set size " << N << std::endl;
  std::cout << header_;
  dump(std::cout, *E);
}

void Data::save(const std::string &filename, bool compressed) const {
  FileHandle f(filename, compressed, "w");
  int32_t num_nodes = N;
  f.write_fully(&num_nodes, sizeof num_nodes);
  for (::size_t v = 0; v < E->edges_at_size(); ++v) {
    auto r = E->edges_at(v);
    GoogleHashSet &rc = const_cast<GoogleHashSet &>(r);
    rc.write_metadata(f.handle());
    rc.write_nopointer_data(f.handle());
  }
}

} // namespace mcmc

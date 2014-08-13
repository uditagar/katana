/** Breadth-first search -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Example breadth-first search application for demoing Galois system. For optimized
 * version, use SSSP application with BFS option instead.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Bag.h"
#include "Galois/Accumulator.h"
#include "Galois/Timer.h"
#include "Galois/Statistic.h"
#include "Galois/Graph/LCGraph.h"
#include "Galois/ParallelSTL/ParallelSTL.h"
#ifdef GALOIS_USE_EXP
#include "Galois/PriorityScheduling.h"
//#include "Galois/Runtime/ParallelWorkInline.h"
#include "Galois/Runtime/BulkSynchronousWork.h"
#endif
#ifdef USE_TBB
#include "tbb/parallel_for.h"
#include "tbb/parallel_for_each.h"
#include "tbb/cache_aligned_allocator.h"
#include "tbb/concurrent_vector.h"
#include "tbb/task_scheduler_init.h"
#include "tbb/enumerable_thread_specific.h"
#endif
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/SmallVector.h"
#include "Lonestar/BoilerPlate.h"

#include <string>
#include <deque>
#include <sstream>
#include <limits>
#include <iostream>
#include <deque>

static const char* name = "Breadth-first Search Example";
static const char* desc =
  "Computes the shortest path from a source node to all nodes in a directed "
  "graph using a modified Bellman-Ford algorithm";
static const char* url = "breadth_first_search";

//****** Command Line Options ******
enum BFSAlgo {
  serial,
  serialAsync,
  serialMin,
  parallelAsync,
  parallelBarrier,
  parallelBarrierCas,
  parallelBarrierInline,
  parallelBarrierExp,
  parallelUndirected,
  parallelTBBBarrier,
  parallelTBBAsync,
  detParallelBarrier,
  detDisjointParallelBarrier,
};

enum DetAlgo {
  nondet,
  detBase,
  detDisjoint
};

namespace cll = llvm::cl;
static cll::opt<unsigned int> startNode("startnode",
    cll::desc("Node to start search from"),
    cll::init(0));
static cll::opt<unsigned int> reportNode("reportnode",
    cll::desc("Node to report distance to"),
    cll::init(1));
static cll::opt<BFSAlgo> algo(cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumVal(serial, "Serial"),
      clEnumVal(serialAsync, "Serial optimized"),
      clEnumVal(serialMin, "Serial optimized with minimal runtime"),
      clEnumVal(parallelAsync, "Parallel"),
      clEnumVal(parallelBarrier, "Parallel optimized with barrier"),
      clEnumVal(parallelBarrierCas, "Parallel optimized with barrier but using CAS"),
      clEnumVal(parallelUndirected, "Parallel specialization for undirected graphs"),
      clEnumVal(detParallelBarrier, "Deterministic parallelBarrier"),
      clEnumVal(detDisjointParallelBarrier, "Deterministic parallelBarrier with disjoint optimization"),
#ifdef GALOIS_USE_EXP
      clEnumVal(parallelBarrierExp, "Parallel optimized with inlined workset and barrier"),
      clEnumVal(parallelBarrierInline, "Parallel optimized with inlined workset and barrier"),
#endif
#ifdef GALOIS_USE_TBB
      clEnumVal(parallelTBBAsync, "TBB"),
      clEnumVal(parallelTBBBarrier, "TBB with barrier"),
#endif
      clEnumValEnd), cll::init(parallelBarrierCas));
static cll::opt<std::string> filename(cll::Positional,
    cll::desc("<input file>"),
    cll::Required);

static const unsigned int DIST_INFINITY =
  std::numeric_limits<unsigned int>::max() - 1;

//****** Work Item and Node Data Defintions ******
struct SNode {
  unsigned int dist;
  unsigned int id;
};

//! ICC + GLIB 4.6 + C++0X (at least) has a faulty implementation of std::pair
#if __INTEL_COMPILER <= 1310
template<typename A,typename B>
struct Pair {
  A first;
  B second;
  Pair(const A& a, const B& b): first(a), second(b) { }
  Pair<A,B>& operator=(const Pair<A,B>& other) {
    if (this != &other) {
      first = other.first;
      second = other.second;
    }
    return *this;
  }
};
#else
template<typename A,typename B>
struct Pair: std::pair<A,B> { };
#endif

typedef Galois::Graph::LC_CSR_Graph<SNode, void> Graph;
typedef Graph::GraphNode GNode;

Graph graph;

struct UpdateRequest {
  GNode n;
  unsigned int w;

  UpdateRequest(): w(0) { }
  UpdateRequest(const GNode& N, unsigned int W): n(N), w(W) { }
  bool operator<(const UpdateRequest& o) const {
    if (w < o.w) return true;
    if (w > o.w) return false;
    return n < o.n;
  }
  bool operator>(const UpdateRequest& o) const {
    if (w > o.w) return true;
    if (w < o.w) return false;
    return n > o.n;
  }
  unsigned getID() const { return /* graph.getData(n).id; */ 0; }
};

std::ostream& operator<<(std::ostream& out, const SNode& n) {
  out <<  "(dist: " << n.dist << ")";
  return out;
}

struct UpdateRequestIndexer: public std::unary_function<UpdateRequest,unsigned int> {
  unsigned int operator()(const UpdateRequest& val) const {
    unsigned int t = val.w;
    return t;
  }
};

struct GNodeIndexer: public std::unary_function<GNode,unsigned int> {
  unsigned int operator()(const GNode& val) const {
    return graph.getData(val, Galois::MethodFlag::NONE).dist;
  }
};

struct not_consistent {
  bool operator()(GNode n) const {
    unsigned int dist = graph.getData(n).dist;
    for (Graph::edge_iterator 
	   ii = graph.edge_begin(n),
	   ee = graph.edge_end(n); ii != ee; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      unsigned int ddist = graph.getData(dst).dist;
      if (ddist > dist + 1) {
        std::cerr << "bad level value: " << ddist << " > " << (dist + 1) << "\n";
	return true;
      }
    }
    return false;
  }
};

struct not_visited {
  bool operator()(GNode n) const {
    unsigned int dist = graph.getData(n).dist;
    if (dist >= DIST_INFINITY) {
      std::cerr << "unvisted node: " << dist << " >= INFINITY\n";
      return true;
    }
    return false;
  }
};

struct max_dist {
  Galois::GReduceMax<unsigned int>& m;
  max_dist(Galois::GReduceMax<unsigned int>& _m): m(_m) { }

  void operator()(GNode n) const {
    m.update(graph.getData(n).dist);
  }
};

//! Simple verifier
static bool verify(GNode source) {
  if (graph.getData(source).dist != 0) {
    std::cerr << "source has non-zero dist value\n";
    return false;
  }
  
  bool okay = Galois::ParallelSTL::find_if(graph.begin(), graph.end(), not_consistent()) == graph.end()
    && Galois::ParallelSTL::find_if(graph.begin(), graph.end(), not_visited()) == graph.end();

  if (okay) {
    Galois::GReduceMax<unsigned int> m;
    Galois::do_all(graph.begin(), graph.end(), max_dist(m));
    std::cout << "max dist: " << m.reduce() << "\n";
  }
  
  return okay;
}

static void readGraph(GNode& source, GNode& report) {
  Galois::Graph::readGraph(graph, filename); 

  source = *graph.begin();
  report = *graph.begin();

  std::cout << "Read " << graph.size() << " nodes\n";
  
  size_t id = 0;
  bool foundReport = false;
  bool foundSource = false;
  for (Graph::iterator src = graph.begin(), ee =
      graph.end(); src != ee; ++src, ++id) {
    SNode& node = graph.getData(*src, Galois::MethodFlag::NONE);
    node.dist = DIST_INFINITY;
    node.id = id;
    if (id == startNode) {
      source = *src;
      foundSource = true;
    } 
    if (id == reportNode) {
      foundReport = true;
      report = *src;
    }
  }

  if (!foundReport || !foundSource) {
    std::cerr 
      << "failed to set report: " << reportNode 
      << "or failed to set source: " << startNode << "\n";
    assert(0);
    abort();
  }
}

//! Serial BFS using optimized flags 
struct SerialAsyncAlgo {
  std::string name() const { return "Serial (Async)"; }

  void operator()(const GNode source) const {
    std::deque<GNode> wl;
    graph.getData(source, Galois::MethodFlag::NONE).dist = 0;

    for (Graph::edge_iterator ii = graph.edge_begin(source, Galois::MethodFlag::NONE), 
           ei = graph.edge_end(source, Galois::MethodFlag::NONE); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);
      ddata.dist = 1;
      wl.push_back(dst);
    }

    while (!wl.empty()) {
      GNode n = wl.front();
      wl.pop_front();

      SNode& data = graph.getData(n, Galois::MethodFlag::NONE);

      unsigned int newDist = data.dist + 1;

      for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
            ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

        if (newDist < ddata.dist) {
          ddata.dist = newDist;
          wl.push_back(dst);
        }
      }
    }
  }
};

//! Galois BFS using optimized flags
struct AsyncAlgo {
  typedef int tt_does_not_need_aborts;

  std::string name() const { return "Parallel (Async)"; }

  void operator()(const GNode& source) const {
    using namespace Galois::WorkList;
    typedef dChunkedFIFO<64> dChunk;
    typedef ChunkedFIFO<64> Chunk;
    typedef OrderedByIntegerMetric<GNodeIndexer,dChunk> OBIM;
    
    std::deque<GNode> initial;
    graph.getData(source).dist = 0;
    for (Graph::edge_iterator ii = graph.edge_begin(source),
          ei = graph.edge_end(source); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst);
      ddata.dist = 1;
      initial.push_back(dst);
    }

    Galois::for_each(initial.begin(), initial.end(), *this, Galois::wl<OBIM>());
  }

  void operator()(GNode& n, Galois::UserContext<GNode>& ctx) const {
    SNode& data = graph.getData(n, Galois::MethodFlag::NONE);

    unsigned int newDist = data.dist + 1;

    for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
          ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

      unsigned int oldDist;
      while (true) {
        oldDist = ddata.dist;
        if (oldDist <= newDist)
          break;
        if (__sync_bool_compare_and_swap(&ddata.dist, oldDist, newDist)) {
          ctx.push(dst);
          break;
        }
      }
    }
  }
};

//! Algorithm for undirected graphs from SC'12
// TODO add cite
// TODO implementation
template<typename WL,bool useCas>
struct UndirectedAlgo {
  std::string name() const { return ""; }
  void operator()(const GNode& source) const { abort(); }
};

//! BFS using optimized flags and barrier scheduling 
template<typename WL,bool useCas>
struct BarrierAlgo {
  typedef int tt_does_not_need_aborts;

  std::string name() const { return "Parallel (Barrier)"; }
  typedef Pair<GNode,int> ItemTy;

  void operator()(const GNode& source) const {
    std::deque<ItemTy> initial;

    graph.getData(source).dist = 0;
    for (Graph::edge_iterator ii = graph.edge_begin(source),
          ei = graph.edge_end(source); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst);
      ddata.dist = 1;
      initial.push_back(ItemTy(dst, 2));
    }
    Galois::for_each(initial.begin(), initial.end(), *this, Galois::wl<WL>());
  }

  void operator()(const ItemTy& item, Galois::UserContext<ItemTy>& ctx) const {
    GNode n = item.first;

    unsigned int newDist = item.second;

    for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
          ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

      unsigned int oldDist;
      while (true) {
        oldDist = ddata.dist;
        if (oldDist <= newDist)
          break;
        if (!useCas || __sync_bool_compare_and_swap(&ddata.dist, oldDist, newDist)) {
          if (!useCas)
            ddata.dist = newDist;
          ctx.push(ItemTy(dst, newDist + 1));
          break;
        }
      }
    }
  }
};

#ifdef GALOIS_USE_EXP
//! BFS using optimized flags and barrier scheduling 
struct BarrierExpAlgo {
  std::string name() const { return "Parallel (Exp)"; }
  typedef Pair<GNode,int> ItemTy;

  struct Initialize {
    template<typename Context>
    void operator()(const GNode& source, Context& ctx) const {
      graph.getData(source).dist = 0;
      for (Graph::edge_iterator ii = graph.edge_begin(source),
            ei = graph.edge_end(source); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        SNode& ddata = graph.getData(dst);
        ddata.dist = 1;
        ctx.push(ItemTy(dst, 2));
      }
    }
  };

  struct Process {
    template<typename Context>
    void operator()(const ItemTy& item, Context& ctx) const {
      GNode n = item.first;

      unsigned int newDist = item.second;

      for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
            ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

        unsigned int oldDist;
        while (true) {
          oldDist = ddata.dist;
          if (oldDist <= newDist)
            break;
          if (__sync_bool_compare_and_swap(&ddata.dist, oldDist, newDist)) {
            ctx.push(ItemTy(dst, newDist + 1));
            break;
          }
        }
      }
    }
  };

  void operator()(const GNode& source) {
    GNode initial[1] = { source };
    typedef boost::fusion::vector<ItemTy> Items;
#ifndef GALOIS_HAS_NO_BULKSYNCHRONOUS_EXECUTOR
    Galois::do_all_bs<Items>(&initial[0], &initial[1],
        boost::fusion::make_vector(Process()),
        Initialize());
#else
    abort();
#endif
  }
};
#else
struct BarrierExpAlgo {
  std::string name() const { return "Parallel (Exp)"; }
  void operator()(const GNode& source) const { abort(); }
};

#endif

//! BFS using optimized flags and barrier scheduling 
template<DetAlgo Version>
struct DetBarrierAlgo {
  typedef int tt_needs_per_iter_alloc; // For LocalState
  static_assert(Galois::needs_per_iter_alloc<DetBarrierAlgo>::value, "Oops");

  std::string name() const { return "Parallel (Deterministic Barrier)"; }
  typedef Pair<GNode,int> ItemTy;

  struct LocalState {
    typedef std::deque<GNode,typename Galois::PerIterAllocTy::template rebind<GNode>::other> Pending;
    Pending pending;
    LocalState(DetBarrierAlgo<Version>& self, Galois::PerIterAllocTy& alloc): pending(alloc) { }
  };
  typedef LocalState GaloisDeterministicLocalState;
  static_assert(Galois::has_deterministic_local_state<DetBarrierAlgo>::value, "Oops");

  uintptr_t galoisDeterministicId(const ItemTy& item) const {
    return graph.getData(item.first, Galois::MethodFlag::NONE).id;
  }
  static_assert(Galois::has_deterministic_id<DetBarrierAlgo>::value, "Oops");

  void operator()(const GNode& source) const {
#ifdef GALOIS_USE_EXP
    typedef Galois::WorkList::BulkSynchronousInline WL;
#else
  typedef Galois::WorkList::BulkSynchronous<Galois::WorkList::dChunkedLIFO<256> > WL;
#endif
    std::deque<ItemTy> initial;

    graph.getData(source).dist = 0;
    for (Graph::edge_iterator ii = graph.edge_begin(source),
          ei = graph.edge_end(source); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst);
      ddata.dist = 1;
      initial.push_back(ItemTy(dst, 2));
    }
    switch (Version) {
      case nondet: 
        Galois::for_each(initial.begin(), initial.end(), *this, Galois::wl<WL>()); break;
      case detBase:
        Galois::for_each_det(initial.begin(), initial.end(), *this); break;
      case detDisjoint:
        Galois::for_each_det(initial.begin(), initial.end(), *this); break;
      default: std::cerr << "Unknown algorithm " << Version << "\n"; abort();
    }
  }

  void build(const ItemTy& item, typename LocalState::Pending* pending) const {
    GNode n = item.first;

    unsigned int newDist = item.second;
    
    for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
          ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst, Galois::MethodFlag::ALL);

      unsigned int oldDist;
      while (true) {
        oldDist = ddata.dist;
        if (oldDist <= newDist)
          break;
        pending->push_back(dst);
        break;
      }
    }
  }

  void modify(const ItemTy& item, Galois::UserContext<ItemTy>& ctx, typename LocalState::Pending* ppending) const {
    unsigned int newDist = item.second;
    bool useCas = false;

    for (typename LocalState::Pending::iterator ii = ppending->begin(), ei = ppending->end(); ii != ei; ++ii) {
      GNode dst = *ii;
      SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

      unsigned int oldDist;
      while (true) {
        oldDist = ddata.dist;
        if (oldDist <= newDist)
          break;
        if (!useCas || __sync_bool_compare_and_swap(&ddata.dist, oldDist, newDist)) {
          if (!useCas)
            ddata.dist = newDist;
          ctx.push(ItemTy(dst, newDist + 1));
          break;
        }
      }
    }
  }

  void operator()(const ItemTy& item, Galois::UserContext<ItemTy>& ctx) const {
    typename LocalState::Pending* ppending;
    if (Version == detDisjoint) {
      bool used;
      LocalState* localState = (LocalState*) ctx.getLocalState(used);
      ppending = &localState->pending;
      if (used) {
        modify(item, ctx, ppending);
        return;
      }
    }
    if (Version == detDisjoint) {
      build(item, ppending);
    } else {
      typename LocalState::Pending pending(ctx.getPerIterAlloc());
      build(item, &pending);
      graph.getData(item.first, Galois::MethodFlag::WRITE); // Failsafe point
      modify(item, ctx, &pending);
    }
  }
};

#ifdef GALOIS_USE_TBB
//! TBB version based off of AsyncAlgo
struct TBBAsyncAlgo {
  std::string name() const { return "Parallel (TBB)"; }

  struct Fn {
    void operator()(const GNode& n, tbb::parallel_do_feeder<GNode>& feeder) const {
      SNode& data = graph.getData(n, Galois::MethodFlag::NONE);

      unsigned int newDist = data.dist + 1;

      for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
            ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

        unsigned int oldDist;
        while (true) {
          oldDist = ddata.dist;
          if (oldDist <= newDist)
            break;
          if (__sync_bool_compare_and_swap(&ddata.dist, oldDist, newDist)) {
            feeder.add(dst);
            break;
          }
        }
      }
    }
  };

  void operator()(const GNode& source) const {
    tbb::task_scheduler_init init(numThreads);
    
    std::vector<GNode> initial;
    graph.getData(source).dist = 0;
    for (Graph::edge_iterator ii = graph.edge_begin(source),
          ei = graph.edge_end(source); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst);
      ddata.dist = 1;
      initial.push_back(dst);
    }

    tbb::parallel_do(initial.begin(), initial.end(), Fn());
  }
};

//! TBB version based off of BarrierAlgo
struct TBBBarrierAlgo {
  std::string name() const { return "Parallel (TBB Barrier)"; }
  typedef tbb::enumerable_thread_specific<std::vector<GNode> > ContainerTy;
  //typedef tbb::concurrent_vector<GNode,tbb::cache_aligned_allocator<GNode> > ContainerTy;

  struct Fn {
    ContainerTy& wl;
    unsigned int newDist;
    Fn(ContainerTy& w, unsigned int d): wl(w), newDist(d) { }

    void operator()(const GNode& n) const {
      for (Graph::edge_iterator ii = graph.edge_begin(n, Galois::MethodFlag::NONE),
            ei = graph.edge_end(n, Galois::MethodFlag::NONE); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        SNode& ddata = graph.getData(dst, Galois::MethodFlag::NONE);

        // Racy but okay
        if (ddata.dist <= newDist)
          continue;
        ddata.dist = newDist;
        //wl.push_back(dst);
        wl.local().push_back(dst);
      }
    }
  };

  struct Clear {
    ContainerTy& wl;
    Clear(ContainerTy& w): wl(w) { }
    template<typename Range>
    void operator()(const Range&) const {
      wl.local().clear();
    }
  };

  struct Initialize {
    ContainerTy& wl;
    Initialize(ContainerTy& w): wl(w) { }
    template<typename Range>
    void operator()(const Range&) const {
      wl.local().reserve(graph.size() / numThreads);
    }
  };

  void operator()(const GNode& source) const {
    tbb::task_scheduler_init init(numThreads);
    
    ContainerTy wls[2];
    unsigned round = 0;

    tbb::parallel_for(tbb::blocked_range<unsigned>(0, numThreads, 1), Initialize(wls[round]));

    graph.getData(source).dist = 0;
    for (Graph::edge_iterator ii = graph.edge_begin(source),
          ei = graph.edge_end(source); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      SNode& ddata = graph.getData(dst);
      ddata.dist = 1;
      //wls[round].push_back(dst);
      wls[round].local().push_back(dst);
    }

    unsigned int newDist = 2;

    Galois::StatTimer Tparallel("ParallelTime");
    Tparallel.start();
    while (true) {
      unsigned cur = round & 1;
      unsigned next = (round + 1) & 1;
      //tbb::parallel_for_each(wls[round].begin(), wls[round].end(), Fn(wls[next], newDist));
      tbb::flattened2d<ContainerTy> flatView = tbb::flatten2d(wls[cur]);
      tbb::parallel_for_each(flatView.begin(), flatView.end(), Fn(wls[next], newDist));
      tbb::parallel_for(tbb::blocked_range<unsigned>(0, numThreads, 1), Clear(wls[cur]));
      //wls[cur].clear();

      ++newDist;
      ++round;
      //if (next_wl.begin() == next_wl.end())
      tbb::flattened2d<ContainerTy> flatViewNext = tbb::flatten2d(wls[next]);
      if (flatViewNext.begin() == flatViewNext.end())
        break;
    }
    Tparallel.stop();
  }
};
#else
struct TBBAsyncAlgo {
  std::string name() const { return "Parallel (TBB)"; }
  void operator()(const GNode& source) const { abort(); }
};

struct TBBBarrierAlgo {
  std::string name() const { return "Parallel (TBB Barrier)"; }
  void operator()(const GNode& source) const { abort(); }
};
#endif

template<typename AlgoTy>
void run() {
  AlgoTy algo;
  GNode source, report;
  readGraph(source, report);
  Galois::preAlloc((numThreads + (graph.size() * sizeof(SNode) * 2) / Galois::Runtime::MM::hugePageSize)*8);
  Galois::reportPageAlloc("MeminfoPre");

  Galois::StatTimer T;
  std::cout << "Running " << algo.name() << " version\n";
  T.start();
  algo(source);
  T.stop();
  
  Galois::reportPageAlloc("MeminfoPost");

  std::cout << "Report node: " << reportNode << " " << graph.getData(report) << "\n";

  if (!skipVerify) {
    if (verify(source)) {
      std::cout << "Verification successful.\n";
    } else {
      std::cerr << "Verification failed.\n";
      assert(0 && "Verification failed");
      abort();
    }
  }
}

int main(int argc, char **argv) {
  Galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  using namespace Galois::WorkList;
  typedef BulkSynchronous<dChunkedLIFO<256> > BSWL;

#ifdef GALOIS_USE_EXP
  typedef BulkSynchronousInline BSInline;
#else
  typedef BSWL BSInline;
#endif

  switch (algo) {
    case serialAsync: run<SerialAsyncAlgo>(); break;
    case serialMin: run<BarrierAlgo<GFIFO<int,false>,false> >(); break;
    case parallelAsync: run<AsyncAlgo>();  break;
    case parallelBarrierCas: run<BarrierAlgo<BSWL,true> >(); break;
    case parallelBarrier: run<BarrierAlgo<BSWL,false> >(); break;
    case parallelBarrierInline: run<BarrierAlgo<BSInline,false> >(); break;
    case parallelBarrierExp: run<BarrierExpAlgo>(); break;
    case parallelUndirected: run<UndirectedAlgo<BSInline,true> >(); break;
    case parallelTBBAsync: run<TBBAsyncAlgo>(); break;
    case parallelTBBBarrier: run<TBBBarrierAlgo>(); break;
    case detParallelBarrier: run<DetBarrierAlgo<detBase> >(); break;
    case detDisjointParallelBarrier: run<DetBarrierAlgo<detDisjoint> >(); break;
    default: std::cerr << "Unknown algorithm " << algo << "\n"; abort();
  }

  return 0;
}

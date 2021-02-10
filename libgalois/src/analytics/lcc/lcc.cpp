/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
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
 */

#include "katana/analytics/lcc/lcc.h"
#include "katana/AtomicHelpers.h"

using namespace katana::analytics;

/*struct NodeTriangleCount {
  using ArrowType = arrow::CTypeTraits<uint64_t>::ArrowType;
  using ViewType = katana::PODPropertyView<std::atomic<uint64_t>>;
};

using NodeData = typename std::tuple<NodeTriangleCount>;
using EdgeData = typename std::tuple<>;

using PropertyGraph = katana::TypedPropertyGraph<NodeData, EdgeData>;

using Node = PropertyGraph::Node;
*/

constexpr static const unsigned kChunkSize = 64U;

/**
 * Like std::lower_bound but doesn't dereference iterators. Returns the first
 * element for which comp is not true.
 */
template <typename Iterator, typename Compare>
Iterator
LowerBound(Iterator first, Iterator last, Compare comp) {
  using difference_type =
      typename std::iterator_traits<Iterator>::difference_type;

  Iterator it;
  difference_type count;
  difference_type half;

  count = std::distance(first, last);
  while (count > 0) {
    it = first;
    half = count / 2;
    std::advance(it, half);
    if (comp(it)) {
      first = ++it;
      count -= half + 1;
    } else {
      count = half;
    }
  }
  return first;
}

/**
 * std::set_intersection over edge_iterators.
 */
size_t
CountEqual(
    PropertyGraph* g, PropertyGraph::edge_iterator aa, PropertyGraph::edge_iterator ea,
    PropertyGraph::edge_iterator bb, PropertyGraph::edge_iterator eb) {
  size_t retval = 0;
  while (aa != ea && bb != eb) {
    PropertyGraph::Node a = *(g->GetEdgeDest(aa));
    PropertyGraph::Node b = *(g->GetEdgeDest(bb));
    if (a < b) {
      ++aa;
    } else if (b < a) {
      ++bb;
    } else {
	    retval += 1;
      katana::atomicAdd(g->GetData<NodeTriangleCount>(*(g->GetEdgeDest(aa))), (uint64_t) 1);
      ++aa;
      ++bb;
    }
  }
  return retval;
}

template <typename G>
struct LessThan {
  const G& g;
  typename G::Node n;
  LessThan(const G& g, typename G::Node n) : g(g), n(n) {}
  bool operator()(typename G::edge_iterator it) {
    return *g.GetEdgeDest(it) < n;
  }
};

template <typename G>
struct GreaterThanOrEqual {
  const G& g;
  typename G::Node n;
  GreaterThanOrEqual(const G& g, typename G::Node n) : g(g), n(n) {}
  bool operator()(typename G::edge_iterator it) {
    return n >= *g.GetEdgeDest(it);
  }
};

template <typename G>
struct GetDegree {
  typedef typename G::Node N;
  const G& g;
  GetDegree(const G& g) : g(g) {}

  ptrdiff_t operator()(const N& n) const { return g.edges(n).size(); }
};

template <typename Node, typename EdgeTy>
struct IdLess {
  bool operator()(
      const katana::EdgeSortValue<Node, EdgeTy>& e1,
      const katana::EdgeSortValue<Node, EdgeTy>& e2) const {
    return e1.dst < e2.dst;
  }
};

/**
 * Node Iterator algorithm for counting triangles.
 * <code>
 * for (v in G)
 *   for (all pairs of neighbors (a, b) of v)
 *     if ((a,b) in G and a < v < b)
 *       triangle += 1
 * </code>
 *
 * Thomas Schank. Algorithmic Aspects of Triangle-Based Network Analysis. PhD
 * Thesis. Universitat Karlsruhe. 2007.
 */
void
NodeIteratingAlgo(PropertyGraph* graph) {

  katana::do_all(
      katana::iterate(*graph),
      [&](const PropertyGraph::Node& n) {
        // Partition neighbors
        // [first, ea) [n] [bb, last)
        PropertyGraph::edge_iterator first = graph->edges(n).begin();
        PropertyGraph::edge_iterator last = graph->edges(n).end();
        PropertyGraph::edge_iterator ea =
            LowerBound(first, last, LessThan<PropertyGraph>(*graph, n));
        PropertyGraph::edge_iterator bb = LowerBound(
            first, last, GreaterThanOrEqual<PropertyGraph>(*graph, n));

        for (; bb != last; ++bb) {
          Node B = *graph->GetEdgeDest(bb);
          for (auto aa = first; aa != ea; ++aa) {
            Node A = *graph->GetEdgeDest(aa);
            PropertyGraph::edge_iterator vv = graph->edges(A).begin();
            PropertyGraph::edge_iterator ev = graph->edges(A).end();
            PropertyGraph::edge_iterator it =
                LowerBound(vv, ev, LessThan<PropertyGraph>(*graph, B));
            if (it != ev && *graph->GetEdgeDest(it) == B) {

		    katana::atomicAdd(graph->GetData<NodeTriangleCount>(n), (uint64_t) 1);
		    katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(A), (uint64_t) 1);
		    katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(B), (uint64_t) 1);
            }
          }
        }
      },
      katana::chunk_size<kChunkSize>(), katana::steal(),
      katana::loopname("TriangleCount_NodeIteratingAlgo"));
}

/**
 * Lambda function to count triangles
 */
void
OrderedCountFunc(
    PropertyGraph* graph, Node n) {
  for (auto it_v : graph->edges(n)) {
    auto v = *graph->GetEdgeDest(it_v);
    if (v > n) {
      break;
    }
    PropertyGraph::edge_iterator it_n = graph->edges(n).begin();

    for (auto it_vv : graph->edges(v)) {
      auto vv = *graph->GetEdgeDest(it_vv);
      if (vv > v) {
        break;
      }
      while (*graph->GetEdgeDest(it_n) < vv) {
        it_n++;
      }
      if (vv == *graph->GetEdgeDest(it_n)) {
        katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(n), (uint64_t) 1);
	katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(v), (uint64_t) 1);
	katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(vv), (uint64_t) 1);
      }
    }
  }
}

/*
 * Simple counting loop, instead of binary searching.
 */
void
OrderedCountAlgo(PropertyGraph* graph) {
  
  katana::do_all(
      katana::iterate(*graph),
      [&](const Node& n) { OrderedCountFunc(graph, n); },
      katana::chunk_size<kChunkSize>(), katana::steal(),
      katana::loopname("TriangleCount_OrderedCountAlgo"));
}

/**
 * Edge Iterator algorithm for counting triangles.
 * <code>
 * for ((a, b) in E)
 *   if (a < b)
 *     for (v in intersect(neighbors(a), neighbors(b)))
 *       if (a < v < b)
 *         triangle += 1
 * </code>
 *
 * Thomas Schank. Algorithmic Aspects of Triangle-Based Network Analysis. PhD
 * Thesis. Universitat Karlsruhe. 2007.
 */
void
EdgeIteratingAlgo(PropertyGraph* graph) {
  struct WorkItem {
    Node src;
    Node dst;
    WorkItem(const Node& a1, const Node& a2) : src(a1), dst(a2) {}
  };

  katana::InsertBag<WorkItem> items;

  katana::do_all(
      katana::iterate(*graph),
      [&](Node n) {
        for (auto edge : graph->edges(n)) {
          auto dest = graph->GetEdgeDest(edge);
          if (n < *dest) {
            items.push(WorkItem(n, *dest));
          }
        }
      },
      katana::loopname("TriangleCount_Initialize"));

  katana::do_all(
      katana::iterate(items),
      [&](const WorkItem& w) {
        // Compute intersection of range (w.src, w.dst) in neighbors of
        // w.src and w.dst
        PropertyGraph::edge_iterator abegin = graph->edges(w.src).begin();
        PropertyGraph::edge_iterator aend = graph->edges(w.src).end();
        PropertyGraph::edge_iterator bbegin = graph->edges(w.dst).begin();
        PropertyGraph::edge_iterator bend = graph->edges(w.dst).end();

        PropertyGraph::edge_iterator aa = LowerBound(
            abegin, aend, GreaterThanOrEqual<PropertyGraph>(*graph, w.src));
        PropertyGraph::edge_iterator ea =
            LowerBound(abegin, aend, LessThan<PropertyGraph>(*graph, w.dst));
        PropertyGraph::edge_iterator bb = LowerBound(
            bbegin, bend, GreaterThanOrEqual<PropertyGraph>(*graph, w.src));
        PropertyGraph::edge_iterator eb =
            LowerBound(bbegin, bend, LessThan<PropertyGraph>(*graph, w.dst));

        uint64_t num_triangles = CountEqual(graph, aa, ea, bb, eb);
	katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(w.src), num_triangles);
        katana::atomicAdd<uint64_t>(graph->GetData<NodeTriangleCount>(w.dst), num_triangles);
      },
      katana::loopname("TriangleCount_EdgeIteratingAlgo"),
      katana::chunk_size<kChunkSize>(), katana::steal());
}

std::vector<double> 
ComputeLCC(PropertyGraph* graph) {

	katana::LargeArray<uint32_t> degree;
	katana::LargeArray<double> lcc;

	degree.allocateBlocked(graph->size());
	lcc.allocateBlocked(graph->size());

	katana::do_all(
      katana::iterate(*graph),
      [&](Node n) {

      	degree[n] = std::distance(graph->edges(n).begin(), graph->edges(n).end());

	lcc[n] = ((double) (2 * graph->template GetData<NodeTriangleCount>(n)))/(degree[n] * (degree[n] - 1));
      });

	std::vector<double> lcc_vector;

	for(Node n = 0 ; n < graph->size() ; n++) {
	
		lcc_vector.push_back(lcc[n]);
	}

	degree.destroy();
	degree.deallocate();

	lcc.destroy();
	lcc.deallocate();

	return lcc_vector;
}

katana::Result<std::vector<double>>
katana::analytics::LCC(
    PropertyGraph* pg, LCCPlan plan) {
  katana::StatTimer timer_graph_read("GraphReadingTime", "LCC");
  katana::StatTimer timer_auto_algo("AutoRelabel", "LCC");

  bool relabel;
  timer_graph_read.start();
  switch (plan.relabeling()) {
  case LCCPlan::kNoRelabel:
    relabel = false;
    break;
  case LCCPlan::kRelabel:
    relabel = true;
    break;
  default:
    return katana::ErrorCode::AssertionFailed;
  }

  std::unique_ptr<katana::PropertyGraph> mutable_pfg;
  if (relabel || !plan.edges_sorted()) {
    // Copy the graph so we don't mutate the users graph.
    auto mutable_pfg_result = pg->Copy({}, {});
    if (!mutable_pfg_result) {
      return mutable_pfg_result.error();
    }
    mutable_pfg = std::move(mutable_pfg_result.value());
    pg = mutable_pfg.get();
  }

  if (relabel) {
    katana::StatTimer timer_relabel("GraphRelabelTimer", "LCC");
    timer_relabel.start();
    if (auto r = katana::SortNodesByDegree(pg); !r) {
      return r.error();
    }
    timer_relabel.stop();
  }

  // If we relabel we must also sort. Relabeling will break the sorting.
  if (relabel || !plan.edges_sorted()) {
    if (auto r = katana::SortAllEdgesByDest(pg); !r) {
      return r.error();
    }
  }

  timer_graph_read.stop();

  katana::Prealloc(1, 16 * (pg->num_nodes() + pg->num_edges()));
  katana::reportPageAlloc("LCC_MeminfoPre");

  katana::StatTimer execTime("LCC", "LCC");
  execTime.start();

  //auto graph = PropertyGraph::Make(pg, {});

  //auto g = graph.value();
  switch (plan.algorithm()) {
  case LCCPlan::kNodeIteration:
    NodeIteratingAlgo(pg);
    break;
  case LCCPlan::kEdgeIteration:
    EdgeIteratingAlgo(pg);
    break;
  case LCCPlan::kOrderedCount:
    OrderedCountAlgo(pg);
    break;
  default:
    return katana::ErrorCode::InvalidArgument;
  }

  std::vector<double> lcc_vector = ComputeLCC(pg);

  execTime.stop();

  katana::reportPageAlloc("LCC_MeminfoPost");

  return lcc_vector;
}

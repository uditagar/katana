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

#include <iostream>

#include "Lonestar/BoilerPlate.h"
#include "katana/analytics/lcc/lcc.h"

using namespace katana::analytics;

const char* name = "Local Clustering Coefficient";
const char* desc = "Computes the local clustering coefficient for each node";

namespace cll = llvm::cl;

static cll::opt<std::string> inputFile(
    cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<LCCPlan::Algorithm> algo(
    "algo", cll::desc("Choose an algorithm:"),
    cll::values(
        clEnumValN(
            LCCPlan::kNodeIteration, "nodeiterator", "Node Iterator"),
        clEnumValN(
            LCCPlan::kEdgeIteration, "edgeiterator", "Edge Iterator"),
        clEnumValN(
            LCCPlan::kOrderedCount, "orderedCount",
            "Ordered Simple Count (default)")),
    cll::init(LCCPlan::kOrderedCount));

static cll::opt<bool> relabel(
    "relabel",
    cll::desc("Relabel nodes of the graph (default value of false => "
              "choose automatically)"),
    cll::init(false));
int
main(int argc, char** argv) {
  std::unique_ptr<katana::SharedMemSys> G =
      LonestarStart(argc, argv, name, desc, nullptr, &inputFile);

  katana::StatTimer totalTime("TimerTotal");
  totalTime.start();

  if (!symmetricGraph) {
    KATANA_DIE(
        "This application requires a symmetric graph input;"
        " please use the -symmetricGraph flag "
        " to indicate the input is a symmetric graph.");
  }

  std::cout << "Reading from file: " << inputFile << "\n";
  std::unique_ptr<katana::PropertyGraph> pg =
      MakeFileGraph(inputFile, edge_property_name);

  LCCPlan plan;

  LCCPlan::Relabeling relabeling_flag =
      relabel ? LCCPlan::kRelabel : LCCPlan::kAutoRelabel;

  switch (algo) {
  case LCCPlan::kNodeIteration:
    plan = LCCPlan::NodeIteration(relabeling_flag);
    break;

  case LCCPlan::kEdgeIteration:
    plan = LCCPlan::EdgeIteration(relabeling_flag);
    break;

  case LCCPlan::kOrderedCount:
    plan = LCCPlan::OrderedCount(relabeling_flag);
    break;

  default:
    std::cerr << "Unknown algo: " << algo << "\n";
  }

  auto lcc_result = LCC(pg.get(), plan);
  if (!lcc_result) {
    KATANA_LOG_FATAL(
        "failed to run algorithm: {}", lcc_result.error());
  }
  auto lcc_vector = lcc_result.value();

  std::cout << "lcc[0]: " << lcc_vector[0] << "\n";

  totalTime.stop();

  return 0;
}

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Huffman/HuffmanCoding.h"
#include "Lonestar/BoilerPlate.h"
#include "NeuralNetwork/SkipGramModelTrainer.h"

namespace cll = llvm::cl;

static const char* name = "Embeddings";
static const char* desc = "Generate embeddings";
static const char* url = "embeddings";

static cll::opt<std::string> inputFile(
    cll::Positional, cll::desc("<input file>"), cll::Required);

static cll::opt<std::string> outputFile(
    cll::Positional, cll::desc("<output file>"), cll::Required);

static cll::opt<uint32_t> embeddingSize(
    "embeddingSize",
    cll::desc("Size of the embedding vector (default value 100)"),
    cll::init(100));

static cll::opt<double> alpha(
    "alpha", cll::desc("alpha (default value 0.025)"), cll::init(0.025f));

static cll::opt<uint32_t> window(
    "window", cll::desc("window size (default value 5)"), cll::init(5));

static cll::opt<double> downSampleRate(
    "downSampleRate", cll::desc("down-sampling rate (default value 0.001)"),
    cll::init(0.001f));

static cll::opt<bool> hierarchicalSoftmax(
    "hierarchicalSoftmax",
    cll::desc("Enable/disable hierarchical softmax (default value false)"),
    cll::init(false));

static cll::opt<uint32_t> numNegSamples(
    "numNegSamples", cll::desc("Number of negative samples (default value 5)"),
    cll::init(5));

static cll::opt<uint32_t> numIterations(
    "numIterations",
    cll::desc("Number of Training Iterations (default value 5)"), cll::init(5));

static cll::opt<uint32_t> mininumFrequency(
    "mininumFrequency", cll::desc("Mininum Frequency (default 5)"),
    cll::init(5));

void
ReadRandomWalks(
    std::ifstream& input_file,
    std::vector<std::vector<uint32_t>>* random_walks) {
  std::string line;

  while (std::getline(input_file, line)) {
    std::vector<uint32_t> walk;
    std::stringstream ss(line);

    uint32_t val;

    while (ss >> val) {
      walk.push_back(val);
    }

    random_walks->push_back(std::move(walk));
  }
}

//builds a vocabulary of nodes using the
//provided random walks
void
BuildVocab(
    std::vector<std::vector<uint32_t>>& random_walks, std::set<uint32_t>* vocab,
    katana::gstl::Map<uint32_t, uint32_t>& vocab_multiset,
    uint32_t* num_trained_tokens) {
  for (auto walk : random_walks) {
    for (auto val : walk) {
      vocab->insert(val);
      (*num_trained_tokens)++;
    }
  }
  using Map = katana::gstl::Map<uint32_t, uint32_t>;

  auto reduce = [](Map& lhs, Map&& rhs) -> Map& {
    Map v{std::move(rhs)};

    for (auto& kv : v) {
      if (lhs.count(kv.first) == 0) {
        lhs[kv.first] = 0;
      }
      lhs[kv.first] += kv.second;
    }

    return lhs;
  };

  auto mapIdentity = []() { return Map(); };

  auto accumMap = katana::make_reducible(reduce, mapIdentity);

  katana::do_all(
      katana::iterate(random_walks),
      [&](const std::vector<uint32_t>& walk) {
        for (auto val : walk) {
          accumMap.update(Map{std::make_pair(val, 1)});
        }
      },
      katana::loopname("countFrequency"));

  vocab_multiset = accumMap.reduce();

  std::vector<uint32_t> to_remove;
  std::set<uint32_t>::iterator iter = vocab->begin();

  //remove nodes occurring less than minCount times
  while (iter != vocab->end()) {
    uint32_t node = *iter;
    if (vocab_multiset[node] < mininumFrequency) {
      to_remove.push_back(node);
    }

    iter++;
  }

  for (auto node : to_remove) {
    vocab->erase(node);
    vocab_multiset.erase(node);
  }
}

//outputs the embedding to a file
void
PrintEmbeddings(
    std::map<unsigned int, HuffmanCoding::HuffmanNode*>& huffman_nodes,
    SkipGramModelTrainer& skip_gram_model_trainer, uint32_t max_id) {
  std::ofstream of(outputFile.c_str());

  HuffmanCoding::HuffmanNode* node;
  uint32_t node_idx;

  for (uint32_t id = 1; id <= max_id; id++) {
    if (huffman_nodes.find(id) != huffman_nodes.end()) {
      node = huffman_nodes.find(id)->second;
      node_idx = node->GetIdx();

      of << id;

      for (uint32_t i = 0; i < embeddingSize; i++) {
        of << " " << skip_gram_model_trainer.GetSyn0(node_idx, i);
      }
      of << "\n";
    }
  }

  of.close();
}

//constructs a new set of random walks
//by pruning nodes (from the walks)
//that are not in the vocabulary
void
RefineRandomWalks(
    std::vector<std::vector<uint32_t>>& random_walks,
    std::vector<std::vector<uint32_t>>* refined_random_walks,
    std::set<uint32_t>& vocab) {
  for (auto walk : random_walks) {
    std::vector<uint32_t> w;
    for (auto val : walk) {
      if (vocab.find(val) != vocab.end()) {
        w.push_back(val);
      }
    }
    refined_random_walks->push_back(std::move(w));
  }
}

int
main(int argc, char** argv) {
  std::unique_ptr<katana::SharedMemSys> G =
      LonestarStart(argc, argv, name, desc, url, &inputFile);

  std::ifstream input_file(inputFile.c_str());

  std::vector<std::vector<uint32_t>> random_walks;

  ReadRandomWalks(input_file, &random_walks);

  std::set<uint32_t> vocab;
  katana::gstl::Map<uint32_t, uint32_t> vocab_multiset;

  uint32_t num_trained_tokens;

  BuildVocab(random_walks, &vocab, vocab_multiset, &num_trained_tokens);

  std::vector<std::vector<uint32_t>> refined_random_walks;

  RefineRandomWalks(random_walks, &refined_random_walks, vocab);

  HuffmanCoding huffman_coding(&vocab, &vocab_multiset);
  katana::gPrint("Huffman Coding init done");

  std::vector<HuffmanCoding::HuffmanNode> huffman_nodes;
  huffman_nodes.resize(vocab.size());

  std::map<uint32_t, HuffmanCoding::HuffmanNode*> huffman_nodes_map;
  huffman_coding.Encode(&huffman_nodes_map, &huffman_nodes);

  katana::gPrint("Huffman Encoding done");

  SkipGramModelTrainer skip_gram_model_trainer(
      embeddingSize, alpha, window, downSampleRate, hierarchicalSoftmax,
      numNegSamples, numIterations, vocab.size(), num_trained_tokens,
      huffman_nodes_map);

  katana::gPrint("Skip-Gram Trainer Init done");

  skip_gram_model_trainer.InitExpTable();

  katana::gPrint("Skip-Gram Init exp table \n");

  for (uint32_t iter = 0; iter < numIterations; iter++) {
    skip_gram_model_trainer.Train(
        refined_random_walks, huffman_nodes_map, vocab_multiset);
  }

  uint32_t max_id = *(vocab.crbegin());

  PrintEmbeddings(huffman_nodes_map, skip_gram_model_trainer, max_id);

  return 0;
}
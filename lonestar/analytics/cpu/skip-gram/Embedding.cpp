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
#include "NeuralNetwork/NeuralNetworkTrainer.h"
#include "NeuralNetwork/SkipGramModelTrainer.h"

//#include "galois/graphs/Util.h"
//#include "galois/Timer.h"
//#include "Lonestar/BoilerPlate.h"
//#include "galois/graphs/FileGraph.h"
//#include "galois/LargeArray.h"

//#include "galois/AtomicHelpers.h"
//#include "galois/AtomicWrapper.h"

namespace cll = llvm::cl;

static const char* name = "Embeddings";
static const char* desc =
    "Generate embeddings";
static const char* url = "embeddings";

static cll::opt<std::string> inputFile(
    cll::Positional, cll::desc("<input file>"), cll::Required);

static cll::opt<std::string> outputFile(
    cll::Positional, cll::desc("<output file>"), cll::Required);

static cll::opt<unsigned int> numIterations(
    "numIterations", cll::desc("Number of Training Iterations (default value 50)"),
    cll::init(50));

void ReadRandomWalks(std::ifstream input_file, std::vector<std::vector<uint32_t>>* random_walks) {

	std::string line;

	while(std::getline(input_file, line)){
		std::vector<uint32_t> walk;
                std::stringstream ss(line);
	
		uint32_t val;

		while(ss >> val) {
			walk.push_back(val);
		}

		random_walks->push_back(std::move(walk));
	}
}

void BuildVocab(std::vector<std::vector<uint32_t>>& random_walks, std::set<uint32_t>* vocab, 
		std::map<uint32_t, uint32_t>* vocab_multiset, uint32_t* num_trained_tokens) {

	for(auto walk: walks) {
		for(auto val : walk) {
			vocab->insert(val);
			if(vocab_multiset->find(val) == vocab_multiset->end()){
				vocab_multiset[val] = 1;
			}
			else{
				vocab_multiset[val] = vocab_multiset[val] + 1;
			}
			(*num_trained_tokens)++;
		}
	}
}

void PrintEmbeddings(std::map<unsigned int, HuffmanCoding::HuffmanNode*>& huffman_nodes,
	             std::vector<std::vector<katana::CopyableAtomic<double>>>& syn0, uint32_t max_id){

	std::ofstream of(outputFile.c_str());

        HuffmanCoding::HuffmanNode* node;
  	uint32_t node_idx;

  	for(uint32_t id = 1; id <= max_id; id++){

                if(huffman_nodes->find(id) != huffman_nodes->end()){
                        node = huffman_nodes->find(id)->second;
                        node_idx = node->idx;

                        of << id;

                        for(uint32_t i = 0 ; i < NeuralNetworkTrainer::kLayer1Size ; i++){
                                of << " " << syn0[node_idx][i];
			}
                }
                else
                {
                        of << id;

      			for(uint32_t i = 0 ; i < NeuralNetworkTrainer::kLayer1Size ; i++){
        			of << " " << 0.0f;
			}
                }
		of << "\n";
        }

        of.close();
}

int main(int argc, char** argv){

	std::unique_ptr<katana::SharedMemSys> G =
  LonestarStart(argc, argv, name, desc, url);

	std::ifstream input_file(inputFile.c_str());

	std::vector<std::vector<uint32_t>> random_walks;

	ReadRandomWalks(input_file, &random_walks);

	std::set<uint32_t> vocab;
	std::map<uint32_t, uint32_t> vocab_multiset;
	
	uint32_t num_trained_tokens;
       
	BuildVocab(random_walks, &vocab, &vocab_multiset, &num_trained_tokens);

	HuffmanCoding huffman_coding(&vocab, &vocab_multiset);
	katana::gPrint("Huffman Coding init done");

	std::vector<HuffmanCoding::HuffmanNode> huffman_nodes;
	huffman_nodes.reserve(vocab.size());

	std::map<uint32_t, HuffmanCoding::HuffmanNode*> huffman_nodes_map;
       	huffman_coding->encode(&huffman_nodes_map, &huffman_nodes);

	katana::gPrint("Huffman Encoding done");

	SkipGramModelTrainer skip_gram_model_trainer(vocab.size(), num_trained_tokens, huffman_nodes_map);

	katana::gPrint("Skip-Gram Trainer Init done");
	
	skip_gram_model_trainer->InitArray();
	skip_gram_model_trainer->InitExpTable();	

	katana::gPrint("Skip-Gram Init exp table \n");

	for(uint32_t iter =0; iter<numIterations; iter++){
		
		skip_gram_model_trainer->train(random_walks, vocab_multiset, huffman_nodes_map);		
	}

	uint32_t max_id = *(vocab.crbegin());

	PrintEmbeddings(huffman_nodes, syn0,max_id);

	return 0;
}


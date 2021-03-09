#pragma once

#include "common/config.h"
#include "common/definitions.h"
#include "common/file_stream.h"
#include "data/corpus_base.h"
#include "data/types.h"

#include <common/timer.h>
#include <algorithm>
#include <iostream>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace marian {
namespace data {

class Shortlist {
private:
  std::vector<WordIndex> indices_;    // // [packed shortlist index] -> word index, used to select columns from output embeddings

public:
  Shortlist(const std::vector<WordIndex>& indices)
    : indices_(indices) {}

  const std::vector<WordIndex>& indices() const { return indices_; }
  WordIndex reverseMap(int idx) { return indices_[idx]; }

  int tryForwardMap(WordIndex wIdx) {
    auto first = std::lower_bound(indices_.begin(), indices_.end(), wIdx);
    if(first != indices_.end() && *first == wIdx)         // check if element not less than wIdx has been found and if equal to wIdx
      return (int)std::distance(indices_.begin(), first); // return coordinate if found
    else
      return -1;                                          // return -1 if not found
  }

};

class ShortlistGenerator {
public:
  virtual ~ShortlistGenerator() {}

  virtual Ptr<Shortlist> generate(Ptr<data::CorpusBatch> batch) const = 0;

  // Writes text version of (possibly) pruned short list to file
  // with given prefix and implementation-specific suffixes.
  virtual void dump(const std::string& /*prefix*/) const {
    ABORT("Not implemented");
  }
};


// Intended for use during training in the future, currently disabled
#if 0
class SampledShortlistGenerator : public ShortlistGenerator {
private:
  Ptr<Options> options_;
  size_t maxVocab_{50000};

  size_t total_{10000};
  size_t firstNum_{1000};

  size_t srcIdx_;
  size_t trgIdx_;
  bool shared_{false};

  // static thread_local std::random_device rd_;
  static thread_local std::unique_ptr<std::mt19937> gen_;

public:
  SampledShortlistGenerator(Ptr<Options> options,
                            size_t srcIdx = 0,
                            size_t trgIdx = 1,
                            bool shared = false)
      : options_(options),
        srcIdx_(srcIdx),
        trgIdx_(trgIdx),
        shared_(shared)
        { }

  virtual Ptr<Shortlist> generate(Ptr<data::CorpusBatch> batch) const override {
    auto srcBatch = (*batch)[srcIdx_];
    auto trgBatch = (*batch)[trgIdx_];

    // add firstNum most frequent words
    std::unordered_set<WordIndex> indexSet;
    for(WordIndex i = 0; i < firstNum_ && i < maxVocab_; ++i)
      indexSet.insert(i);

    // add all words from ground truth
    for(auto i : trgBatch->data())
      indexSet.insert(i.toWordIndex());

    // add all words from source
    if(shared_)
      for(auto i : srcBatch->data())
        indexSet.insert(i.toWordIndex());

    std::uniform_int_distribution<> dis((int)firstNum_, (int)maxVocab_);
    if (gen_ == NULL)
      gen_.reset(new std::mt19937(std::random_device{}()));
    while(indexSet.size() < total_ && indexSet.size() < maxVocab_)
      indexSet.insert(dis(*gen_));

    // turn into vector and sort (selected indices)
    std::vector<WordIndex> idx(indexSet.begin(), indexSet.end());
    std::sort(idx.begin(), idx.end());

    // assign new shifted position
    std::unordered_map<WordIndex, WordIndex> pos;
    std::vector<WordIndex> reverseMap;

    for(WordIndex i = 0; i < idx.size(); ++i) {
      pos[idx[i]] = i;
      reverseMap.push_back(idx[i]);
    }

    Words mapped;
    for(auto i : trgBatch->data()) {
      // mapped postions for cross-entropy
      mapped.push_back(Word::fromWordIndex(pos[i.toWordIndex()]));
    }

    return New<Shortlist>(idx, mapped, reverseMap);
  }
};
#endif

class LexicalShortlistGenerator : public ShortlistGenerator {
private:
  Ptr<Options> options_;
  Ptr<const Vocab> srcVocab_;
  Ptr<const Vocab> trgVocab_;

  size_t srcIdx_;
  bool shared_{false};

  size_t firstNum_{100};
  size_t bestNum_{100};

  std::vector<std::unordered_map<WordIndex, float>> data_; // [WordIndex src] -> [WordIndex tgt] -> P_trans(tgt|src) --@TODO: rename data_ accordingly

  void load(const std::string& fname) {
    io::InputFileStream in(fname);

    std::string src, trg;
    float prob;
    while(in >> trg >> src >> prob) {
      // @TODO: change this to something safer other than NULL
      if(src == "NULL" || trg == "NULL")
        continue;

      auto sId = (*srcVocab_)[src].toWordIndex();
      auto tId = (*trgVocab_)[trg].toWordIndex();

      if(data_.size() <= sId)
        data_.resize(sId + 1);
      data_[sId][tId] = prob;
    }
  }

  void prune(float threshold = 0.f) {
    size_t i = 0;
    for(auto& probs : data_) {
      std::vector<std::pair<float, WordIndex>> sorter;
      for(auto& it : probs)
        sorter.emplace_back(it.second, it.first);

      std::sort(
          sorter.begin(), sorter.end(), std::greater<std::pair<float, WordIndex>>()); // sort by prob

      probs.clear();
      for(auto& it : sorter) {
        if(probs.size() < bestNum_ && it.first > threshold)
          probs[it.second] = it.first;
        else
          break;
      }

      ++i;
    }
  }

public:
  LexicalShortlistGenerator(Ptr<Options> options,
                            Ptr<const Vocab> srcVocab,
                            Ptr<const Vocab> trgVocab,
                            size_t srcIdx = 0,
                            size_t /*trgIdx*/ = 1,
                            bool shared = false)
      : options_(options),
        srcVocab_(srcVocab),
        trgVocab_(trgVocab),
        srcIdx_(srcIdx),
        shared_(shared) {
    std::vector<std::string> vals = options_->get<std::vector<std::string>>("shortlist");

    ABORT_IF(vals.empty(), "No path to filter path given");
    std::string fname = vals[0];

    firstNum_ = vals.size() > 1 ? std::stoi(vals[1]) : 100;
    bestNum_ = vals.size() > 2 ? std::stoi(vals[2]) : 100;
    float threshold = vals.size() > 3 ? std::stof(vals[3]) : 0;
    std::string dumpPath = vals.size() > 4 ? vals[4] : "";
    LOG(info,
        "[data] Loading lexical shortlist as {} {} {} {}",
        fname,
        firstNum_,
        bestNum_,
        threshold);

    // @TODO: Load and prune in one go.
    load(fname);
    prune(threshold);

    if(!dumpPath.empty())
      dump(dumpPath);
  }

  virtual void dump(const std::string& prefix) const override {
    // Dump top most frequent words from target vocabulary
    LOG(info, "[data] Saving shortlist dump to {}", prefix + ".{top,dic}");
    io::OutputFileStream outTop(prefix + ".top");
    for(WordIndex i = 0; i < firstNum_ && i < trgVocab_->size(); ++i)
      outTop << (*trgVocab_)[Word::fromWordIndex(i)] << std::endl;

    // Dump translation pairs from dictionary
    io::OutputFileStream outDic(prefix + ".dic");
    for(WordIndex srcId = 0; srcId < data_.size(); srcId++) {
      for(auto& it : data_[srcId]) {
        auto trgId = it.first;
        outDic << (*srcVocab_)[Word::fromWordIndex(srcId)] << "\t" << (*trgVocab_)[Word::fromWordIndex(trgId)] << std::endl;
      }
    }
  }

  virtual Ptr<Shortlist> generate(Ptr<data::CorpusBatch> batch) const override {
    auto srcBatch = (*batch)[srcIdx_];

    // add firstNum most frequent words
    std::unordered_set<WordIndex> indexSet;
    for(WordIndex i = 0; i < firstNum_ && i < trgVocab_->size(); ++i)
      indexSet.insert(i);

    // add all words from ground truth
    // for(auto i : trgBatch->data())
    //  indexSet.insert(i.toWordIndex());

    // collect unique words form source
    std::unordered_set<WordIndex> srcSet;
    for(auto i : srcBatch->data())
      srcSet.insert(i.toWordIndex());

    // add aligned target words
    for(auto i : srcSet) {
      if(shared_)
        indexSet.insert(i);
      for(auto& it : data_[i])
        indexSet.insert(it.first);
    }
    // Ensure that the generated vocabulary items from a shortlist are a multiple-of-eight
    // This is necessary until intgemm supports non-multiple-of-eight matrices.
    // TODO better solution here? This could potentially be slow.
    WordIndex i = static_cast<WordIndex>(firstNum_);
    while (indexSet.size() % 8 != 0) {
      indexSet.insert(i);
      i++;
    }

    // turn into vector and sort (selected indices)
    std::vector<WordIndex> indices(indexSet.begin(), indexSet.end());
    std::sort(indices.begin(), indices.end());

    return New<Shortlist>(indices);
  }
};

class BinaryShortlistGenerator : public ShortlistGenerator {
private:
  Ptr<Options> options_;
  Ptr<const Vocab> srcVocab_;
  Ptr<const Vocab> trgVocab_;

  size_t srcIdx_;
  bool shared_{false};

  size_t firstNum_{100};
  //size_t bestNum_{100}; not in use for binary shortlist

  // shortlist is stored in a skip list
  std::vector<size_t> word_to_offset_;
  std::vector<WordIndex> short_lists_;

  void load(const std::string& fname) {
    size_t word_to_offset_size = 0;
    size_t short_lists_size = 0;

    // read the skip list from binary file
    std::fstream binIn(fname, std::ios::in | std::ios::binary);

    // read the lengths of the vectors from header
    binIn.read((char*)(&word_to_offset_size), sizeof(size_t));
    binIn.read((char*)(&short_lists_size), sizeof(size_t));

    // create the vectors for reading
    size_t word_to_offset_buf[word_to_offset_size];
    WordIndex short_lists_buf[short_lists_size];

    // read the contents of the vectors
    binIn.read((char*)(&word_to_offset_buf), sizeof(size_t)* word_to_offset_size);
    binIn.read((char*)(&short_lists_buf), sizeof(WordIndex)* short_lists_size);
    word_to_offset_.assign(word_to_offset_buf, word_to_offset_buf+ word_to_offset_size);
    short_lists_.assign(short_lists_buf, short_lists_buf+ short_lists_size);
    binIn.close();
  }

public:
  BinaryShortlistGenerator(Ptr<Options> options,
                           Ptr<const Vocab> srcVocab,
                           Ptr<const Vocab> trgVocab,
                           size_t srcIdx = 0,
                           size_t /*trgIdx*/ = 1,
                           bool shared = false)
      : options_(options),
        srcVocab_(srcVocab),
        trgVocab_(trgVocab),
        srcIdx_(srcIdx),
        shared_(shared) {

    std::vector<std::string> vals = options_->get<std::vector<std::string>>("shortlist");

    ABORT_IF(vals.empty(), "No path to filter path given");
    std::string fname = vals[0];
    ABORT_IF(!io::isBin(fname),"Not a binary file");

    firstNum_ = vals.size() > 1 ? std::stoi(vals[1]) : 100;
    float threshold = vals.size() > 2 ? std::stof(vals[2]) : 0;
    std::string dumpPath = vals.size() > 3 ? vals[3] : "";

    LOG(info,
        "[data] Loading binary shortlist as {} {} {}",
        fname,
        firstNum_,
        threshold);

    load(fname);

  }

  virtual Ptr<Shortlist> generate(Ptr<data::CorpusBatch> batch) const override {
    auto srcBatch = (*batch)[srcIdx_];

    // Since V=trgVocab_->size() is not large, anchor the time and space complexity to O(V).
    // Attempt to squeeze the truth tables into CPU cache
    bool *srcTruthTable = new bool[srcVocab_->size()]();    // holds selected source words
    bool *trgTruthTable = new bool[trgVocab_->size()]();    // holds selected target words

    // add firstNum most frequent words
    for(WordIndex i = 0; i < firstNum_ && i < trgVocab_->size(); ++i)
      trgTruthTable[i] = 1;

    // collect unique words from source
    // add aligned target words: mark trgTruthTable[word] to 1
    for(auto word : srcBatch->data()) {
      WordIndex srcIndex = word.toWordIndex();
      if(shared_)
        trgTruthTable[srcIndex] = 1;
      // If srcIndex has not been encountered, add the corresponding target words
      if (!srcTruthTable[srcIndex]) {
        for (size_t j = word_to_offset_[srcIndex]; j < word_to_offset_[srcIndex+1]; j++)
          trgTruthTable[short_lists_[j]] = 1;
        srcTruthTable[srcIndex] = 1;
      }
    }

    // Due to the 'multiple-of-eight' issue, the following O(N) patch is inserted
    size_t trgTruthTableOnes = 0;   // counter for no. of selected target words
    for (size_t i = 0; i < trgVocab_->size(); i++) {
      if(trgTruthTable[i])
        trgTruthTableOnes++;
    }

    // Ensure that the generated vocabulary items from a shortlist are a multiple-of-eight
    // This is necessary until intgemm supports non-multiple-of-eight matrices.
    for (size_t i = firstNum_; i < trgVocab_->size() && trgTruthTableOnes%8!=0; i++){
      if (!trgTruthTable[i]){
        trgTruthTable[i] = 1;
        trgTruthTableOnes++;
      }
    }

    // turn selected indices into vector and sort (Bucket sort: O(V))
    std::vector<WordIndex> indices;
    for (WordIndex i = 0; i < trgVocab_->size(); i++) {
      if(trgTruthTable[i])
        indices.push_back(i);
    }

    return New<Shortlist>(indices);
  }

  virtual void dump(const std::string& prefix) const override {
      // Dump top most frequent words from target vocabulary
      LOG(info, "[data] Saving shortlist dump to {}", prefix + ".{top,dic}");
      io::OutputFileStream outTop(prefix + ".top");
      for(WordIndex i = 0; i < firstNum_ && i < trgVocab_->size(); ++i)
        outTop << (*trgVocab_)[Word::fromWordIndex(i)] << std::endl;

      // Dump translation pairs from dictionary
      io::OutputFileStream outDic(prefix + ".dic");
      int slowIndex = 0;
      for(int i =1; i <word_to_offset_.size(); i++){
        for (slowIndex=word_to_offset_[i-1]; slowIndex<word_to_offset_[i]; slowIndex++) {
          WordIndex srcId = i-1;
          WordIndex trgId = short_lists_[slowIndex];
          outDic << (*srcVocab_)[Word::fromWordIndex(srcId)] << "\t" << (*trgVocab_)[Word::fromWordIndex(trgId)] << std::endl;
        }
      }
  }

};

class FakeShortlistGenerator : public ShortlistGenerator {
private:
  std::vector<WordIndex> indices_;

public:
  FakeShortlistGenerator(const std::unordered_set<WordIndex>& indexSet)
      : indices_(indexSet.begin(), indexSet.end()) {
    std::sort(indices_.begin(), indices_.end());
  }

  Ptr<Shortlist> generate(Ptr<data::CorpusBatch> /*batch*/) const override {
    return New<Shortlist>(indices_);
  }
};

}  // namespace data
}  // namespace marian

#pragma once

#include <deque>
#include <queue>

#include <boost/timer/timer.hpp>

#include "dataset.h"

namespace marian {

namespace data {

class Batch {
  private:
    std::vector<Input> inputs_;

  public:
    std::vector<Input>& inputs() {
      return inputs_;
    }

    const std::vector<Input>& inputs() const {
      return inputs_;
    }

    void push_back(Input input) {
      inputs_.push_back(input);
    }

    int dim() const {
      return inputs_[0].shape()[0];
    }

    size_t size() const {
      return inputs_.size();
    }
};

typedef std::shared_ptr<Batch> BatchPtr;

class BatchGenerator {
  private:
    DataBasePtr data_;
    ExampleIterator current_;

    size_t batchSize_;
    size_t maxiBatchSize_;

    std::deque<BatchPtr> bufferedBatches_;
    BatchPtr currentBatch_;

    void fillBatches() {
      auto cmp = [](const ExamplePtr& a, const ExamplePtr& b) {
        return (*a)[0]->size() < (*b)[0]->size();
      };

      std::priority_queue<ExamplePtr, Examples, decltype(cmp)> maxiBatch(cmp);

      while(current_ != data_->end() && maxiBatch.size() < maxiBatchSize_) {
        maxiBatch.push(*current_);
        current_++;
      }

      Examples batchVector;
      while(!maxiBatch.empty()) {
        batchVector.push_back(maxiBatch.top());
        maxiBatch.pop();
        if(batchVector.size() == batchSize_) {
          bufferedBatches_.push_back(toBatch(batchVector));
          batchVector.clear();
        }
      }
      if(!batchVector.empty())
        bufferedBatches_.push_back(toBatch(batchVector));
      //std::cerr << "Total: " << total.format(5, "%ws") << std::endl;
    }

    BatchPtr toBatch(const Examples& batchVector) {
      int batchSize = batchVector.size();

      std::vector<int> maxDims;
      for(auto& ex : batchVector) {
        if(maxDims.size() < ex->size())
          maxDims.resize(ex->size(), 0);
        for(int i = 0; i < ex->size(); ++i) {
          if((*ex)[i]->size() > maxDims[i])
          maxDims[i] = (*ex)[i]->size();
        }
      }

      BatchPtr batch(new Batch());
      std::vector<Input::iterator> iterators;
      for(auto& m : maxDims) {
        batch->push_back(Shape({batchSize, m}));
        iterators.push_back(batch->inputs().back().begin());
      }

      for(auto& ex : batchVector) {
        for(int i = 0; i < ex->size(); ++i) {
          DataPtr d = (*ex)[i];
          d->resize(maxDims[i], 0.0f);
          iterators[i] = std::copy(d->begin(), d->end(), iterators[i]);
        }
      }
      return batch;
    }

  public:
    BatchGenerator(DataBasePtr data,
                   size_t batchSize=100,
                   size_t maxiBatchSize=1000)
    : data_(data),
      batchSize_(batchSize),
      maxiBatchSize_(maxiBatchSize),
      current_(data_->begin()) { }

    operator bool() const {
      return !bufferedBatches_.empty();
    }

    BatchPtr next() {
      UTIL_THROW_IF2(bufferedBatches_.empty(),
                     "No batches to fetch");
      currentBatch_ = bufferedBatches_.front();
      bufferedBatches_.pop_front();

      if(bufferedBatches_.empty())
        fillBatches();

      return currentBatch_;
    }

    void prepare(bool shuffle=true) {
      //boost::timer::cpu_timer total;
      if(shuffle)
        data_->shuffle();
      //std::cerr << "shuffle: " << total.format(5, "%ws") << std::endl;
      current_ = data_->begin();
      fillBatches();
    }
};

}

}
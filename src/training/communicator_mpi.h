#include "training/communicator.h"
#include "3rd_party/threadpool.h"

namespace marian {

// This communicator should be used when on CPU with multiple nodes
class MpiCommunicator : public ICommunicator {
private:
  std::vector<Ptr<TensorAllocator>> paramsAllocs_;
  std::vector<Tensor> tmpTensors_;

  mutable ThreadPool threadPool_;
  mutable std::vector<std::future<void>> threadResults_; // [device index]

  /*Copied from DefaultCommunicator*/
  void lazyInit() {
    if(tmpTensors_.size() == 0) {
      int totalSize = (int)graphs_[0]->params()->vals()->size();
      int shardSize = (int)ceil(totalSize / (float)graphs_.size());

      int pos = 0;
      for(auto graph : graphs_) {
        // LOG(info, "Lazy init for pos {}.", pos);
        int __size__ = std::min(shardSize, totalSize);

        auto paramsAlloc = New<TensorAllocator>(graph->getBackend());
        paramsAllocs_.push_back(paramsAlloc);

        paramsAlloc->reserveExact(__size__ * sizeof(float));

        Tensor tmp;

        paramsAlloc->allocate(tmp, {1, __size__});
        tmpTensors_.push_back(tmp);

        // move to next shard
        pos += __size__;
        totalSize -= __size__;
        // LOG(info, "Lazy init for pos {} complete.", pos);
      }
    }
  }
  /*These are copied from the NCCL codebase*/
  size_t myRank(size_t localDeviceIndex) const { // map local device index to a global rank
      // LOG(info, "Myrank {}", mpi_->myMPIRank() * graphs_.size() + localDeviceIndex);
      return mpi_->myMPIRank() * graphs_.size() + localDeviceIndex;
  }

  size_t numRanks() const { // total number of devices across all MPI processes
      // LOG(info, "NumRanks {}", mpi_->numMPIProcesses() * graphs_.size());
      return mpi_->numMPIProcesses() * graphs_.size();
  }

  size_t dataSize() const { // total number of floats that comprise the concatenated parameter and gradient vector
    // LOG(info, "DataSize {}", graphs_[0]->params()->vals()->size());
    return graphs_[0]->params()->vals()->size();
  }

  // determine the (max) shard size
  // All shards except the last one have this size.
  // Presently, even all shards must have identical size
  size_t shardSize() const {
    size_t numShards = numRanks();
    size_t size = (dataSize() + numShards - 1) / numShards;
#if 1 // This is a good sanity check
    ABORT_IF(size * numShards != dataSize(), "presently, all shards must have the same size");
#endif
    // LOG(info, "ShardSize {}", size);
    return size;
  }

  // determine the index range (begin, end) of a shard
  std::pair<size_t, size_t> RankShardRange(size_t rank) const {
    size_t size = shardSize();
    size_t begin = rank * size;
    size_t end = begin + size;
    end = std::min(end, dataSize()); // clip last shard. Note: Presently this never happens, since shardSize() enforces uniform shard size.
    return{ begin, end };
  }

  // determine the index range (begin, end) of a shard
  std::pair<size_t, size_t> localShardRange(size_t localDeviceIndex) const {
    return RankShardRange(myRank(localDeviceIndex));
  }

public:

  //std::vector<ccl::stream> streams_;
  Ptr<IMPIWrapper> mpi_; // Can not be null!
  MpiCommunicator(const std::vector<Ptr<ExpressionGraph>>& graphs, Ptr<IMPIWrapper> mpi)
      : ICommunicator(graphs),
        //streams_(graphs.size()),
        threadPool_(graphs.size(), graphs.size()), threadResults_(graphs.size()),
        mpi_(mpi) {
      ABORT_IF(!mpi_, "We must have a valid MPI backend"); //We can't be null
      LOG(info, "Using MPI as a communication backend.");
      // Create one stream per communicator. Apparently there's no default stream constructor in this version.
      //for (int i=0; i< streams_.size(); i++) {
      //  streams_[i] = ccl::create_stream(); // Hopefully it's a CPU stream
      //}
  }

  ~MpiCommunicator() override {/*delete comm_;*/}

  /*Copied from NCCL communicator*/
  void foreach(const ForeachFunc& func, bool parallel = true) const override {
    parallel &= graphs_.size() > 1;

    for(size_t i = 0; i < graphs_.size(); ++i) {
      size_t begin, end; std::tie
      (begin, end) = localShardRange(i);
      if (parallel)
        threadResults_[i] = threadPool_.enqueue(func, i, begin, end);
      else
        func(i, begin, end);
    }
    if (parallel)
      for(size_t i = 0; i < graphs_.size(); ++i)
        threadResults_[i].wait();
  }
/*
  void scatterReduceAndResetGrads() const override {
    const_cast<MpiCommunicator*>(this)->lazyInit();

    // Gather gradients from different devices into current gradient shards
    auto scatter = [this](size_t idx, size_t begin, size_t end) {
      auto curGrad = graphs_[idx]->params()->grads()->subtensor(begin, end-begin);

      // collect and sum gradients
      for(auto graph : graphs_) {
        if(graph != graphs_[idx]) {
          auto subGrad = graph->params()->grads()->subtensor(begin, end - begin);
          tmpTensors_[idx]->copyFrom(subGrad);

          using namespace functional;
          Element(_1 = _1 + _2, curGrad, tmpTensors_[idx]);
        }
      }
    };

    // reset gradients outside current shard
    auto reset = [this](size_t idx, size_t begin, size_t end) {
      auto grad = graphs_[idx]->params()->grads();
      if (begin > 0)
        grad->subtensor(0, begin)->set(0);
      if (end < grad->size())
        grad->subtensor(end, grad->size()-end)->set(0);
    };

    foreach(scatter);
    foreach(reset);
  }*/

  void scatterReduceAndResetGrads() const override {

    // We are all here;
    thread_local std::vector<float> tmpsendbff(shardSize());
    for(int i = 0; i < graphs_.size(); ++i) {
      size_t begin, end; std::tie
      (begin, end) = localShardRange(i);

      auto grads = graphs_[i]->params()->grads();
      const auto* sendbuf = grads->data();
      auto*       recvbuf = grads->subtensor(begin, end-begin)->data();
      size_t      bufsize = shardSize();

      //LOG(info, "Shard range for graph {} begin {} end {} shardsize {}, rank {}", i, begin, end, bufsize, myRank(i));
      ABORT_IF(grads->subtensor(begin, end-begin)->size() != bufsize, "unexpected subtensor size??");
      // MPI prohibits aliasing because of ancient fortran requirement. MPI Is stupid
      std::memcpy(&tmpsendbff[0], &sendbuf[begin], sizeof(float)*bufsize);
      mpi_->barrier();
      // LOG(info, "ScatterReduceAndReset graph {} ReduceScatter start.", i);
      mpi_->reduceScatterBlock((const void *)&tmpsendbff[0],
                          (void *)recvbuf,
                          bufsize,
                          MPI_FLOAT,
                          MPI_SUM);
      //std::memcpy(recvbuf, &tmprecvbff[0], sizeof(float)*(end-begin)); //@TODO this might not be correct
      // LOG(info, "ScatterReduceAndReset graph {} ReduceScatter end.", i);
      mpi_->barrier();
      // LOG(info, "ScatterReduceAndReset graph {} barrier end.", i);
    }

    // reset gradients
    // In the future, we can keep quantization residuals here straight in the grads themselves.
    auto resetGrads = [&](size_t i, size_t begin, size_t end) {
      auto grads = graphs_[i]->params()->grads();
      auto size = grads->size();
      // reset everything outside the shard that we reduce in
      if (begin > 0)
        grads->subtensor(0, begin)->set(0.f);
      if (end < size)
        grads->subtensor(end, size - end)->set(0.f);
    };
    foreach(resetGrads);
  }//*/

  // Non-mpi version
  void allGatherParams() const override {

    // Update all graphs with parameter shard
    auto gather = [this](size_t idx, size_t begin, size_t end) {
      auto getShard = [&](Ptr<ExpressionGraph> graph) {
        return graph->params()->vals()->subtensor(begin, end-begin);
      };
      auto curShard = getShard(graphs_[idx]);

      // Copy parameter shard to each graph
      for(auto graph : graphs_) {
        if(graph != graphs_[idx]) {
          auto subShard = getShard(graph);
          subShard->copyFrom(curShard);
        }
      }
    };

    foreach(gather);
  }
/*
 void allGatherParams() const override {

    for(int i = 0; i < graphs_.size(); ++i) {
      // LOG(info, "AllGather graph {}.", i);
      //ccl::stream stream = ccl::create_stream();
      // LOG(info, "AllGather stream create {}.", i);
      mpi_->barrier();
      // LOG(info, "AllGather pass barrier {}.", i);
      size_t begin, end; std::tie
      (begin, end) = localShardRange(i);

      auto vals = graphs_[i]->params()->vals();
      const auto* sendbuf = vals->subtensor(begin, end-begin)->data();
      auto*       recvbuf = vals->data();
      size_t      bufsize = shardSize();

      //std::vector<size_t> counts(numRanks(), bufsize);
      std::vector<float> tmpsendbff(end-begin);
      std::memcpy(&tmpsendbff[0], sendbuf, sizeof(float)*(end-begin));

      // LOG(info, "AllGather graph {} allgatherv start.", i);
      // LOG(info, "AllgatherV, buffsize {}", bufsize);
      mpi_->Allgather((const void *)&tmpsendbff[0],
                      bufsize,
                      MPI_FLOAT,
                      (void *)&recvbuf[begin],
                      bufsize,
                      MPI_FLOAT);

      //std::memcpy(sendbuf, &tmpsendbff[0], sizeof(float)*(end-begin));
      // LOG(info, "AllGather graph {} allgatherv end.", i);
      //NCCL_CHECK(ncclAllGather(sendbuf, recvbuf, bufsize, ncclFloat, comms_[i], streams_[i]));
      mpi_->barrier();
      // LOG(info, "AllGather graph {} barrier end end.", i);
    }

  }
*/
  // swap distributed paramShards with model params()
  // It is assumed that all model params() on all devices and MPI processes are identical.
  // This is used for the smoothed parameters.
  void swapParams(const std::vector<Tensor>& distributedParamShards) const override {
    // get everything onto the CPU @TODO we're already on the CPU, remove this
    auto distributedParams = gatherState([&](size_t localDeviceIndex) {
      std::vector<float> tmp;
      distributedParamShards[localDeviceIndex]->get(tmp);
      return tmp;
    });
    // Now all MPI processes hold an identical copy of a concatenation of all distributedParamShards[] across local and remote devices.
    std::vector<float> localParams;
    graphs_[0]->params()->vals()->get(localParams);
    // Now all MPI processes hold an identical copy of params() (remember, we assumed all devices hold the same params()).
    // LOG(info, "SwapParams distrubuted size {} local size {}.", distributedParams.size(), localParams.size());
    ABORT_IF(distributedParams.size() != localParams.size(), "distributed sharded and local params have different size??");

    // swap
    std::swap(distributedParams, localParams);

    // distribute it back
    scatterState(distributedParams, [&](size_t localDeviceIndex,
                                        std::vector<float>::const_iterator begin,
                                        std::vector<float>::const_iterator end){
      ABORT_IF(distributedParamShards[localDeviceIndex]->size() != end-begin, "swapParams size mismatch??"); // @TODO: move check to set()
      distributedParamShards[localDeviceIndex]->set(std::vector<float>(begin, end)); // @TODO: directly pass iterators to set()
    });
    for (auto& graph : graphs_) // broadcast to every local graph
      graph->params()->vals()->set(localParams);
    // LOG(info, "SwapParams ended.");
  }

  // Distribute a single CPU-side vector to shards across multiple devices and MPI processes.
  // This is used when restoring optimizer state, which is sharded, and as part of swapParams().
  // It is assumed that all MPI processes get the same data() passed. Hence, no MPI transfers are needed here.
  void scatterState(const std::vector<float>& data, const OptimizerBase::ScatterStateSetFunc& setFn) const override {
    //std::cerr << "ScatterState" << std::endl;
    size_t dataSize = data.size();
    size_t numShards = numRanks();
    size_t shardSize = (dataSize + numShards - 1) / numShards;
    for(size_t localDeviceIndex = 0; localDeviceIndex < graphs_.size(); localDeviceIndex++) {
      // We only slice out data that is kept in our MPI process. Remember that all MPI processes receive the same, complete data.
      auto rank = myRank(localDeviceIndex);
      size_t begin = rank * shardSize;
      size_t end   = std::min(begin + shardSize, dataSize);
      setFn(localDeviceIndex, data.begin() + begin, data.begin() + end);
    }
  }

  // Collect shards across multiple devices and MPI processes in the NCCL configuration into a single CPU-side vector.
  // This is used when persisting optimizer state, which is sharded, and as part of swapParams().
  std::vector<float> gatherState(const OptimizerBase::GatherStateGetFunc& getFn) const override {
    std::vector<float> tmp; // (temp buffer used multiple times)
    // first, concatenate over all local devices
    std::vector<float> localData;
    for(size_t localDeviceIndex = 0; localDeviceIndex < graphs_.size(); localDeviceIndex++) {
      tmp = getFn(localDeviceIndex);
      localData.insert(localData.end(), tmp.begin(), tmp.end());
    }
    // second, concatenate across MPI processes
    // Note that all local devices occupy consecutive ncclRanks in order.
    // LOG(info, "Gather State before mpi branch.");
    std::vector<float> data;
    if (mpi_) {
      // push one rank's data at a time using broadcast
      for(size_t mpiRank = 0; mpiRank < mpi_->numMPIProcesses(); mpiRank++) {
        // LOG(info, "Loop rank {}.", mpiRank);
        // broadcast mpiRank's localData to all
        if(mpiRank == mpi_->myMPIRank())
          tmp = localData;
        mpi_->bCast(tmp, /*rootRank=*/mpiRank);
        // LOG(info, "Loop rank {} after broadcast.", mpiRank);
        // now all ranks have the same slice: concatenate (we will end up with the same on all MPI processes)
        data.insert(data.end(), tmp.begin(), tmp.end());
      }
    }
    else { // no MPI: localData is the complete result already
      data = std::move(localData);
    }
    // LOG(info, "Gather State ended.");
    return data;
  }

};

} // Namespace Marian

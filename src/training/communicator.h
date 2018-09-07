#pragma once

// clang-format off
#include "graph/expression_graph.h"
#include "functional/functional.h"
#include "tensors/tensor_operators.h"
#if MPI_FOUND
#include "mpi.h"
#endif
// clang-format on

namespace marian {

struct/*interface*/ IMPIWrapper; // @TODO: Should we use a separate header, or move this declaration up here?

// This class implements the cross-GPU operations for distributed training within a single box.
// @TODO: This should absorb the multi-node version as well.
class Communicator {
protected:
  const std::vector<Ptr<ExpressionGraph>> graphs_;

public:
  Communicator(const std::vector<Ptr<ExpressionGraph>>& graphs)
      : graphs_(graphs) {}

  virtual ~Communicator() {}

  // helper to apply a function to each graph, in parallel threads
  virtual void foreach(const std::function<void(size_t, int)>& func) const {
    int totalSize = (int)graphs_[0]->params()->vals()->size();
    int shardSize = (int)ceil(totalSize / (float)graphs_.size());

    int pos = 0;
    std::vector<std::thread> group;
    // iterate over all shards
    for(size_t idx = 0; idx < graphs_.size(); ++idx) {
      int size = std::min(shardSize, totalSize);

      group.emplace_back(func, idx, pos); // @BUGBUG: It seems the callee must guess the shard size again. Better pass the actual size.

      pos += size;
      totalSize -= size;
      // @TODO: safer variant is pos = totalSize * idx / graphs_.size() and endpos = same for (id+1)
    }
    for(auto& t : group)
      t.join();
  }

  virtual void scatterReduce() = 0; // @TODO: indicate by the name that this is scattering gradients
  virtual void allGather(bool vals) = 0;
  virtual void allReduceGrads() = 0;
  virtual void reduceGrads(size_t root = 0) = 0;

  virtual void pushParams(std::vector<Tensor>& params) = 0;
  virtual void pullParams(const std::vector<Tensor>& params) = 0;
  virtual void swapParams(const std::vector<Tensor>& params) = 0;
};

class DefaultCommunicator : public Communicator {
private:
  std::vector<Ptr<TensorAllocator>> paramsAllocs_;
  std::vector<Tensor> tmpTensors_;

  void init() {
    if(tmpTensors_.size() == 0) {
      int totalSize = (int)graphs_[0]->params()->vals()->size();
      int shardSize = (int)ceil(totalSize / (float)graphs_.size());

      int pos = 0;
      for(auto graph : graphs_) {
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
      }
    }
  }

public:
  DefaultCommunicator(const std::vector<Ptr<ExpressionGraph>>& graphs, Ptr<IMPIWrapper> mpi)
      : Communicator(graphs) {
    ABORT_IF(mpi != nullptr, "DefaultCommunicator support for MPI is not yet implemented");
  }

  ~DefaultCommunicator() override {}

  void scatterReduce() override {
    init();

    int totalSize = (int)graphs_[0]->params()->vals()->size();
    int shardSize = (int)ceil(totalSize / (float)graphs_.size());

    // Gather gradients from different devices into current gradient shards
    auto scatter = [this, shardSize](size_t idx, int pos) {
      auto curGrad = graphs_[idx]->params()->grads()->subtensor(pos, shardSize);

      // collect and sum gradients
      // to be replaced with ncclScatterReduce
      for(auto graph : graphs_) {
        if(graph != graphs_[idx]) {
          auto subGrad = graph->params()->grads()->subtensor(pos, shardSize);
          tmpTensors_[idx]->copyFrom(subGrad);

          using namespace functional;
          Element(_1 = _1 + _2, curGrad, tmpTensors_[idx]);
        }
      }
    };

    this->foreach(scatter);
  }

  void allGather(bool vals) override {
    int totalSize = (int)graphs_[0]->params()->vals()->size();
    int shardSize = (int)ceil(totalSize / (float)graphs_.size());

    // Update all graphs with parameter shard
    auto gather = [this, shardSize, vals](size_t idx, int pos) {
      auto getShard = [&](Ptr<ExpressionGraph> graph) {
        auto tensor = vals ? graph->params()->vals() : graph->params()->grads();
        return tensor->subtensor(pos, shardSize);
      };
      auto curShard = getShard(graphs_[idx]);

      // copy parameter shard to each graph
      for(auto graph : graphs_) {
        if(graph != graphs_[idx]) {
          auto subShard = getShard(graph);
          subShard->copyFrom(curShard);
        }
      }
    };

    this->foreach(gather);
  }

  void allReduceGrads() override {
    if (graphs_.size() > 1) { // @TODO: perf bug: this is not efficient
      scatterReduce();
      allGather(/*vals=*/false);
    }
  }

  void reduceGrads(size_t /*root*/) override {
    allReduceGrads(); // @BUGBUG: This is a hack that is slow and also overwriting some gradients (which is OK in practice)
  }

  void pushParams(std::vector<Tensor>& params) override {
    // Copy paramter shard from i-th graph to shard params[i].
    // Graphs and shards with the same index live on the same device.

    auto copy = [this, params](size_t idx, int pos) {
      // copy parameter shard to each graph
      auto subParam
          = graphs_[idx]->params()->vals()->subtensor(pos, (int)params[idx]->size());
      params[idx]->copyFrom(subParam);
    };

    this->foreach(copy);
  }

  void pullParams(const std::vector<Tensor>& params) override {
    // Update all graphs with parameter shard

    auto gather = [this, params](size_t idx, int pos) {
      // copy parameter shard to each graph
      for(auto graph : graphs_) {
        auto subParam
            = graph->params()->vals()->subtensor(pos, (int)params[idx]->size());
        subParam->copyFrom(params[idx]);
      }
    };
    this->foreach(gather);
  }

  void swapParams(const std::vector<Tensor>& params) override {
    // Update all graphs with parameter shard
    ABORT_IF(graphs_.size() < 2, "Swap requires at least two graphs");

    auto gather = [this, params](size_t idx, int pos) {
      // copy parameter shard to each graph, apart from last graph
      for(int i = 0; i < (int)graphs_.size() - 1; ++i) {
        auto subParam
            = graphs_[i]->params()->vals()->subtensor(pos, (int)params[idx]->size());
        subParam->copyFrom(params[idx]);
      }

      // back-up shard from last graph
      auto subParamLast =
          graphs_.back()->params()->vals()->subtensor(pos, (int)params[idx]->size());
      params[idx]->copyFrom(subParamLast);

      auto subParamFirst
          = graphs_[0]->params()->vals()->subtensor(pos, (int)params[idx]->size());
      subParamLast->copyFrom(subParamFirst);
    };
    // execute for each shard
    this->foreach(gather);
  }
};

Ptr<Communicator> createCommunicator(
    const std::vector<Ptr<ExpressionGraph>>& graphs,
    bool noNccl, Ptr<IMPIWrapper> mpi);

// Abstracts MPI operations, allowing alternative implementations (specifically fake (for debugging) and NCCL.
// This implements the MPI APIs we use here, with the following modifications:
//  * throws exception instead of returning an error
//  * swapped out some strange MPI-specific data types to more correct C++ ones where appropriate
#if MPI_FOUND
#else
enum MPI_Comm { MPI_COMM_WORLD };
enum MPI_Datatype { MPI_FLOAT, MPI_UNSIGNED_LONG_LONG, MPI_UNSIGNED_LONG };
enum MPI_Op { MPI_SUM };
struct MPI_Status { int MPI_SOURCE; };
#define MPI_ANY_SOURCE ((size_t)-2)
#define MPI_STATUS_IGNORE ((MPI_Status*)nullptr)
#endif
struct/*interface*/ IMPIWrapper
{
  virtual size_t myRank() const = 0;
  virtual size_t commWorldSize() const = 0;
  virtual void barrier(MPI_Comm comm = MPI_COMM_WORLD) const = 0;
  virtual void bCast(void* buf, size_t count, MPI_Datatype datatype, size_t rootRank = 0, MPI_Comm comm = MPI_COMM_WORLD) const = 0;
  virtual void sSend(void* buf, size_t count, MPI_Datatype datatype, size_t destRank, int tag, MPI_Comm comm = MPI_COMM_WORLD) const = 0;
  virtual void recv(void* buf, size_t count, MPI_Datatype datatype, size_t sourceRank, int tag, MPI_Comm comm = MPI_COMM_WORLD, MPI_Status* status = MPI_STATUS_IGNORE) const = 0;
  virtual void allReduce(const void* sendbuf, void* recvbuf, size_t count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm = MPI_COMM_WORLD) const = 0;
  virtual void finalize() = 0;
  static const size_t RECV_ANY_SOURCE = (size_t)MPI_ANY_SOURCE;
  // helper templates
private:
  static MPI_Datatype getDataType(const float*) { return MPI_FLOAT; }
  static MPI_Datatype getDataType(const unsigned long*) { return MPI_UNSIGNED_LONG; }
  static MPI_Datatype getDataType(const unsigned long long*) { return MPI_UNSIGNED_LONG_LONG; }
public:
  template<typename T>
  void bCast(std::vector<T>& v, size_t rootRank = 0, MPI_Comm comm = MPI_COMM_WORLD) {
    size_t vecLen = v.size();
    bCast(&vecLen, 1, getDataType(&vecLen), rootRank, comm);
    v.resize(vecLen);
    bCast(v.data(), v.size(), getDataType(v.data()), rootRank, comm);
  }
};

Ptr<IMPIWrapper> initMPI(bool multiThreaded);
void finalizeMPI(Ptr<IMPIWrapper>&&);

}  // namespace marian

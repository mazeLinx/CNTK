#pragma once
#include "stdafx.h"
#include "DataReader.h"
#include "DataWriter.h"
#include "RandomOrdering.h"
#include <string>
#include <map>
#include <vector>
#include <random>
#include <future>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#if DEBUG
#include <cvmarkersobj.h>
using namespace Concurrency::diagnostic;
#endif

namespace Microsoft {
	namespace MSR {
		namespace CNTK {

			template <typename T>
			class DenseBlockingQueue
			{
			private:
				std::mutex              d_mutex;
				std::condition_variable d_condition;
				std::deque<T>           d_queue;
			public:
				void push(T const& value) {
					{
						std::unique_lock<std::mutex> lock(this->d_mutex);
						d_queue.push_front(value);
					}
					this->d_condition.notify_one();
				}
				T pop() {
					std::unique_lock<std::mutex> lock(this->d_mutex);
					this->d_condition.wait(lock, [=]{ return !this->d_queue.empty(); });
					T rc(std::move(this->d_queue.back()));
					this->d_queue.pop_back();
					return rc;
				}
			};


			template<class ElemType>
			class BDenseBinaryMatrix {
			public:
				BDenseBinaryMatrix(wstring name, int deviceID, size_t numRows, size_t numCols) : m_matrixName(name), m_deviceID(deviceID), m_maxNumRows(numRows), m_numRows(0), m_maxNumCols(numCols), m_values(nullptr) {};
				//BinaryMatrix(wstring name, size_t numRows, size_t numCols) : m_matrixName(name), m_maxNumRows(numRows), m_numRows(0), m_maxNumCols(numCols), m_values(nullptr) {};
				virtual void Clear() = 0;
				virtual void Dispose() = 0;
				virtual void Fill(Matrix<ElemType>*) = 0;
				virtual void AddValues(void*, size_t) = 0;
				virtual void AddColIndices(void*, size_t) = 0;
				virtual void AddRowIndices(void*, size_t) = 0;
				virtual void UpdateNNz(size_t) = 0;
				virtual void UpdateCurMB(size_t mb) { m_numRows += mb; }
				virtual void ResizeArrays(size_t) = 0;
				virtual void SetMaxRows(size_t maxRows) = 0;
			protected:
				wstring m_matrixName;
				int m_deviceID;
				ElemType* m_values;
				size_t m_maxNumRows;
				size_t m_maxNumCols;

				size_t m_numRows;

			};

			template<class ElemType>
			class SDenseBinaryMatrix : public BDenseBinaryMatrix<ElemType> {
			public:
				SDenseBinaryMatrix(wstring name, int deviceID, size_t numRows, size_t numCols);
				//DenseBinaryMatrix(wstring name, size_t numRows, size_t numCols);
				virtual void Clear();
				virtual void Dispose();
				virtual void Fill(Matrix<ElemType>* matrix) override;
				virtual void AddValues(void* values, size_t numRows) override;
				virtual void AddColIndices(void* /*colIndices*/, size_t /*numCols*/) override { NOT_IMPLEMENTED }
				virtual void AddRowIndices(void* /*rowIndices*/, size_t /*nnz*/) override { NOT_IMPLEMENTED }
				virtual void UpdateNNz(size_t /*nnz*/) override { NOT_IMPLEMENTED }
				virtual void ResizeArrays(size_t) { NOT_IMPLEMENTED }
				virtual void SetMaxRows(size_t maxRows) override;
			protected:
			};

			template<class ElemType>
			class DenseBinaryInput {
			public:
				DenseBinaryInput(std::wstring fileName);
				~DenseBinaryInput();
				template<class ConfigRecordType> 
				void Init(std::map<std::wstring, std::wstring> rename, const ConfigRecordType & config);
				void StartDistributedMinibatchLoop(size_t mbSize, size_t subsetNum, size_t numSubsets);
				void ReadMinibatches(size_t* read_order, size_t numToRead);
				size_t ReadMinibatch(void* data_buffer, std::map<std::wstring, shared_ptr<BDenseBinaryMatrix<ElemType>>>& matrices);
				//void GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices);
				size_t FillMatrices(std::map<std::wstring, shared_ptr<BDenseBinaryMatrix<ElemType>>>& matrices);
				size_t GetMBSize() { return m_mbSize; }
				size_t GetNumMB() { return m_numBatches / (m_mbSize / m_microBatchSize); }
				void Shuffle();
				shared_ptr<BDenseBinaryMatrix<ElemType>> CreateMatrix(std::wstring matName, int deviceId);
				//shared_ptr<BinaryMatrix<ElemType>> CreateMatrix(std::wstring matName);
				virtual bool DataEnd();


			private:
				void ReadOffsets(size_t startMB, size_t numMBs);
				void FillReadOrder(size_t windowSize);
				void* GetTempDataPointer(size_t numVals);
				bool Randomize();

				ifstream m_inFile;
				std::wstring m_fileName;
				size_t m_fileSize;

				size_t m_offsetsStart;
				int64_t* m_offsets;

				int64_t* m_totalReadOffsets;

				size_t m_dataStart;

				size_t m_nextMB; // starting sample # of the next minibatch
				size_t m_epochSize; // size of an epoch


				size_t m_numRows;    // size of minibatch requested
				size_t m_numBatches;    // size of minibatch requested

				int32_t m_microBatchSize;
				size_t m_mbSize;

				size_t m_startMB;
				size_t m_endMB;
				size_t m_curLower;

				size_t m_subsetNum;
				size_t m_numSubsets;

				size_t m_windowSize;
				size_t m_curWindowSize;

				bool m_randomize;
				size_t* m_readOrder; // array to shuffle to reorder the dataset
				size_t m_readOrderLength;
				size_t m_maxMBSize;

				size_t m_totalDim;

				size_t m_microbatchFileSize;
				bool m_bQueueBufferAllocated;


				std::vector<std::wstring> m_features;
				std::vector<std::wstring> m_labels;
				std::map<std::wstring, int32_t> m_mappedNumCols;
				std::map<std::wstring, int32_t> m_mappedStartIndex;

				std::map<std::wstring, void*> m_mappedBuffer;
				std::map<std::wstring, void*> m_mappedBufferForConsumption;



				int32_t m_tempValuesSize;
				void* m_tempValues;


				RandomOrdering m_randomordering;   // randomizing class
				std::mt19937_64 m_randomEngine;
#ifdef _WIN32
				DWORD sysGran;
#else
				int32_t sysGran;
#endif
				DenseBlockingQueue<void*> m_dataToProduce;
				DenseBlockingQueue<void*> m_dataToConsume;
			};

			template<class ElemType>
			class DenseReader : public IDataReader <ElemType>
			{
			public:
				using LabelType = typename IDataReader<ElemType>::LabelType;
				using LabelIdType = typename IDataReader<ElemType>::LabelIdType;

				virtual void Init(const ConfigParameters & config) override { InitFromConfig(config); }
				virtual void Init(const ScriptableObjects::IConfigRecord & config) override { InitFromConfig(config); }

				template<class ConfigRecordType> void InitFromConfig(const ConfigRecordType &);

				virtual void Destroy();

				DenseReader() : DSSMLabels(nullptr), DSSMCols(0) { m_pMBLayout = make_shared<MBLayout>(); };

				virtual ~DenseReader();

				virtual void StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples = requestDataSize);
				virtual void StartDistributedMinibatchLoop(size_t mbSize, size_t epoch, size_t subsetNum, size_t numSubsets, size_t requestedEpochSamples) override;
				virtual bool GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices);

				virtual bool SupportsDistributedMBRead() const override { return true; }

				template<class ConfigRecordType> void RenamedMatrices(const ConfigRecordType& readerConfig, std::map<std::wstring, std::wstring>& rename);
				virtual void SetLabelMapping(const std::wstring& /*sectionName*/, const std::map<LabelIdType, LabelType>& /*labelMapping*/) { NOT_IMPLEMENTED };
				virtual bool GetData(const std::wstring& /*sectionName*/, size_t /*numRecords*/, void* /*data*/, size_t& /*dataBufferSize*/, size_t /*recordStart = 0*/) { NOT_IMPLEMENTED };
				virtual bool DataEnd();

				size_t GetNumParallelSequences() { return m_pMBLayout->GetNumParallelSequences(); }
				void CopyMBLayoutTo(MBLayoutPtr pMBLayout) { pMBLayout->CopyFrom(m_pMBLayout); };

				//virtual bool DataEnd(EndDataType endDataType);

				size_t NumberSlicesInEachRecurrentIter() { return 1; }
				void SetNbrSlicesEachRecurrentIter(const size_t) { };
				void SetSentenceEndInBatch(std::vector<size_t> &/*sentenceEnd*/){};

			private:
#if DEBUG
				marker_series* reader_series;
				size_t cur_read;
#endif
				clock_t timer;
				void DoDSSMMatrix(Matrix<ElemType>& mat, size_t actualMBSize);

				void CheckDataMatrices(std::map<std::wstring, Matrix<ElemType>*>& matrices);
				MBLayoutPtr m_pMBLayout;
				ConfigParameters m_readerConfig;

				std::shared_ptr<DenseBinaryInput<ElemType>> m_dataInput;

				std::map<std::wstring, shared_ptr<BDenseBinaryMatrix<ElemType>>> m_dataMatrices;

				unsigned long m_randomize; // randomization range

				ElemType* DSSMLabels;
				size_t DSSMCols;


				size_t m_mbSize;    // size of minibatch requested

				size_t m_requestedEpochSize; // size of an epoch

				size_t m_epoch; // which epoch are we on

				bool m_partialMinibatch;    // a partial minibatch is allowed

				bool m_prefetchEnabled;
				std::future<size_t> m_pendingAsyncGetMinibatch;

			};
		}
	}
}

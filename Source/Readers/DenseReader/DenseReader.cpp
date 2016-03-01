//
// <copyright file="DenseReader.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// DenseReader.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#define DATAREADER_EXPORTS  // creating the exports here
#include "DataReader.h"
#include "DenseReader.h"
#include "fileutil.h"   // for fexists()
#include <random>
#include <map>
#include <ctime>
#include <time.h>
#include "CUDAPageLockedMemAllocator.h"
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif


namespace Microsoft {
	namespace MSR {
		namespace CNTK {

			DWORD HIDWORD(size_t size) { return size >> 32; }
			DWORD LODWORD(size_t size) { return size & 0xFFFFFFFF; }

			template <class ElemType>
			SDenseBinaryMatrix<ElemType>::SDenseBinaryMatrix(wstring name, int deviceID, size_t numRows, size_t numCols) : BDenseBinaryMatrix<ElemType>(name, deviceID, numRows, numCols) {
				//this->m_values = (ElemType*)malloc(sizeof(ElemType)*numRows*numCols);
				this->m_values = (ElemType*)CUDAPageLockedMemAllocator::Malloc(sizeof(ElemType)*numRows*numCols, deviceID);
			}

			template <class ElemType>
			void SDenseBinaryMatrix<ElemType>::Clear() {
				this->m_numRows = 0;
			}

			template <class ElemType>
			void SDenseBinaryMatrix<ElemType>::Dispose() {
				if (this->m_values != nullptr) {
					//free(this->m_values);
					CUDAPageLockedMemAllocator::Free(this->m_values, this->m_deviceID);
				}
				this->m_values = nullptr;
			}


			template <class ElemType>
			void SDenseBinaryMatrix<ElemType>::Fill(Matrix<ElemType>* matrix) {
				matrix->SetValue(this->m_maxNumCols, this->m_numRows, matrix->GetDeviceId(), this->m_values, matrixFlagNormal);
#if DEBUG
				matrix->Print("testname");
#endif
			}

			template <class ElemType>
			void SDenseBinaryMatrix<ElemType>::SetMaxRows(size_t maxRows) {
				if (maxRows > this->m_maxNumRows) {
					//ElemType* values = (ElemType*)malloc(sizeof(ElemType)*this->m_maxNumCols*maxRows);
					ElemType* values = (ElemType*)CUDAPageLockedMemAllocator::Malloc(sizeof(ElemType)*this->m_maxNumCols*maxRows, this->m_deviceID);
					if (this->m_values != nullptr) {
						if (this->m_numRows > 0) {
							memcpy(values, this->m_values, sizeof(ElemType)*this->m_numRows*this->m_maxNumCols);
						}
						//free(this->m_values);
						CUDAPageLockedMemAllocator::Free(this->m_values, this->m_deviceID);
					}
					this->m_values = values;
					this->m_maxNumRows = maxRows;
				}
			}

			template <class ElemType>
			void SDenseBinaryMatrix<ElemType>::AddValues(void* values, size_t numRows) {
				memcpy(this->m_values + this->m_numRows*this->m_maxNumCols, values, sizeof(ElemType)*numRows*this->m_maxNumCols);
				this->m_numRows += numRows;
			}

			template<class ElemType>
			DenseBinaryInput<ElemType>::DenseBinaryInput(std::wstring fileName) : m_fileName(fileName), m_readOrder(nullptr), m_readOrderLength(0), m_randomize(false), m_tempValues(nullptr), m_tempValuesSize(0), m_offsets(nullptr), m_offsetsStart(0), m_startMB(0), m_endMB(0) {
				std::string name = msra::strfun::utf8(m_fileName);
				m_inFile.open(name, ifstream::binary | ifstream::in);
			}

			template<class ElemType>
			DenseBinaryInput<ElemType>::~DenseBinaryInput() {

			}

			template<class ElemType>
			template<class ConfigRecordType>
			void DenseBinaryInput<ElemType>::Init(std::map<std::wstring, std::wstring> rename, const ConfigRecordType & config) {


				//Get # total columns

				m_totalDim = config(L"totalDim", (int32_t)0);
				m_microBatchSize = config(L"microBatchSize", (int32_t)(1024));

				m_mbSize = (size_t)m_microBatchSize;


				m_inFile.seekg(0, ios::end);
				m_fileSize = (size_t)m_inFile.tellg();

				if (m_fileSize < 0)
				{
					RuntimeError("Your Data file Does not exists, Check your Path");
				}

				m_numRows = m_fileSize / (sizeof(ElemType)) / m_totalDim;

				if (m_fileSize %(sizeof(ElemType) * m_totalDim) !=0 )
				{
					RuntimeError("file size does not match total Dim %zd, please check your setting", m_totalDim);
				}

				m_numBatches = m_numRows / m_microBatchSize;

				if (m_numRows% m_microBatchSize)
				{
					m_numBatches += 1;
				}

				m_microbatchFileSize = (sizeof(ElemType)) * m_totalDim * m_microBatchSize;

				m_totalReadOffsets = (int64_t*)malloc(sizeof(int64_t)*(m_numBatches + 1));

				int64_t temp_offset = 0;
				for (size_t b = 0; b < m_numBatches; b++)
				{
					m_totalReadOffsets[b] = temp_offset;
					temp_offset += m_microbatchFileSize;
				}

				m_totalReadOffsets[m_numBatches] = m_fileSize;



				for (const auto & id : config.GetMemberIds())
				{
					if (!config.CanBeConfigRecord(id))
						continue;
					const ConfigRecordType & temp = config(id);
					// see if we have a config parameters that contains a "dim" element, it's a sub key, use it
					if (temp.ExistsCurrent(L"dim"))
					{

						wstring wname = id;
						int32_t singlefeature_dim = temp(L"dim", (int32_t)(0));
						int32_t singlefeature_startIndex = temp(L"startIndex", (int32_t)(0));
						if (rename.find(wname) == rename.end())
						{
							m_labels.emplace_back(wname);
						}
						else
						{
							//m_features.emplace_back(rename[wname]);
							m_labels.emplace_back(rename[wname]);
						}
						m_mappedNumCols[m_labels.back()] = singlefeature_dim;
						m_mappedStartIndex[m_labels.back()] = singlefeature_startIndex;

						m_mappedBuffer[m_labels.back()] = malloc(m_microbatchFileSize);
						m_mappedBufferForConsumption[m_labels.back()] = malloc(m_microbatchFileSize);

					}
				}

				m_bQueueBufferAllocated = false;


				//size_t base_offset = 0;
				//m_inFile.seekg(0, ifstream::beg);
				//m_inFile.read((char*)&m_numRows, sizeof(int64_t));
				//base_offset += sizeof(int64_t);

				//m_inFile.read((char*)&m_numBatches, sizeof(int64_t));
				//base_offset += sizeof(int64_t);

				//int32_t numFeatures;
				//int32_t numLabels;
				//m_inFile.read((char*)&numFeatures, sizeof(int32_t));
				//base_offset += sizeof(int32_t);

				//m_inFile.read((char*)&numLabels, sizeof(int32_t));
				//base_offset += sizeof(int32_t);

				//int32_t len;
				//int32_t numCols;
				//int32_t maxLen = 100;
				//char* tempName = (char*)malloc(maxLen);
				//for (int32_t c = 0; c < numFeatures; c++)
				//{
				//	m_inFile.read((char*)&len, sizeof(int32_t));
				//	if (len + 1 > maxLen) {
				//		maxLen = len + 1;
				//		free(tempName);
				//		tempName = (char*)malloc(maxLen);
				//	}
				//	base_offset += sizeof(int32_t);

				//	m_inFile.read((char*)tempName, len);
				//	tempName[len] = '\0';
				//	//std::string name((char*)header_buffer + base_offset, len);
				//	std::wstring wname = msra::strfun::utf16(tempName);
				//	if (rename.find(wname) == rename.end())
				//	{
				//		m_features.emplace_back(wname);
				//	}
				//	else
				//	{
				//		m_features.emplace_back(rename[wname]);
				//	}
				//	base_offset += sizeof(int8_t)*len;

				//	m_inFile.read((char*)&numCols, sizeof(numCols));
				//	//numCols = *(int32_t*)((char*)header_buffer + base_offset);
				//	base_offset += sizeof(int32_t);

				//	m_mappedNumCols[m_features.back()] = numCols;

				//}
				//for (int32_t c = 0; c < numLabels; c++)
				//{
				//	m_inFile.read((char*)&len, sizeof(int32_t));
				//	if (len + 1 > maxLen) {
				//		maxLen = len + 1;
				//		free(tempName);
				//		tempName = (char*)malloc(maxLen);
				//	}
				//	base_offset += sizeof(int32_t);

				//	//std::string name((char*)header_buffer + base_offset, len);
				//	m_inFile.read((char*)tempName, len);
				//	tempName[len] = '\0';
				//	std::wstring wname = msra::strfun::utf16(tempName);
				//	if (rename.find(wname) == rename.end())
				//	{
				//		m_labels.emplace_back(wname);
				//	}
				//	else
				//	{
				//		//m_features.emplace_back(rename[wname]);
				//		m_labels.emplace_back(rename[wname]);
				//	}
				//	base_offset += sizeof(int8_t)*len;

				//	//numCols = *(int32_t*)((char*)header_buffer + base_offset);
				//	m_inFile.read((char*)&numCols, sizeof(numCols));
				//	base_offset += sizeof(int32_t);
				//	m_mappedNumCols[m_labels.back()] = numCols;

				//}
				//free(tempName);

				//m_offsetsStart = base_offset;
				//m_dataStart = m_offsetsStart + m_numBatches * sizeof(int64_t);

				///*Read in the microbatch size here*/
				//m_inFile.seekg(m_dataStart, ios::beg);
				//m_inFile.read((char*)&m_microBatchSize, sizeof(int32_t));
				//m_mbSize = (size_t)m_microBatchSize;


			}

			template<class ElemType>
			bool DenseBinaryInput<ElemType>::Randomize()
			{
				return false;
				/*
				if (m_randomize > 0)
				{
				return true;
				}
				return false;
				*/
			}


			template<class ElemType>
			void DenseBinaryInput<ElemType>::FillReadOrder(size_t windowSize)
			{
				if (m_readOrder != nullptr)
				{
					free(m_readOrder);
				}
				m_readOrder = (size_t*)malloc(sizeof(size_t)*windowSize);
				for (size_t c = 0; c < windowSize; c++)
				{
					m_readOrder[c] = c;
				}
			}

			template<class ElemType>
			void DenseBinaryInput<ElemType>::ReadOffsets(size_t startMB, size_t numMBs)
			{
				if (startMB == m_startMB && m_endMB == startMB + numMBs) {
					return;
				}
				if (m_offsets != nullptr) {
					free(m_offsets);
				}
				m_offsets = (int64_t*)malloc(sizeof(int64_t)*(numMBs + 1));

				memcpy(m_offsets, m_totalReadOffsets + startMB, numMBs*sizeof(int64_t));

				if (startMB + numMBs < m_numBatches) {
					m_offsets[numMBs] = m_totalReadOffsets[startMB + numMBs];
				}
				else {
					m_offsets[numMBs] = m_fileSize;
				}
				m_startMB = startMB;
				m_endMB = startMB + numMBs;
			}

			template<class ElemType>
			void DenseBinaryInput<ElemType>::Shuffle()
			{
				if (Randomize())
				{
					shuffle(&(m_readOrder[0]), &(m_readOrder[m_readOrderLength - 1]), m_randomEngine);
				}
			}

			template<class ElemType>
			void DenseBinaryInput<ElemType>::StartDistributedMinibatchLoop(size_t mbSize, size_t subsetNum, size_t numSubsets) {

				m_nextMB = 0;

				m_mbSize = mbSize;

				m_subsetNum = subsetNum;
				m_numSubsets = numSubsets;

				m_epochSize = m_numBatches / numSubsets;

				size_t startMB = m_epochSize * subsetNum;
				size_t endMB = m_epochSize * (subsetNum + 1);

				size_t remainder = m_numBatches % numSubsets;

				size_t lb = min(remainder, subsetNum);
				size_t ub = min(remainder, subsetNum + 1);

				m_epochSize += (subsetNum < remainder) ? 1 : 0;

				startMB += lb;
				endMB += ub;
				m_windowSize = endMB - startMB;

				if (m_windowSize != m_readOrderLength) {
					FillReadOrder(m_windowSize);
					m_readOrderLength = m_windowSize;
				}


				Shuffle();

				ReadOffsets(startMB, m_windowSize);

				m_maxMBSize = 0;
				for (size_t c = 0; c < m_windowSize; c++) {
					m_maxMBSize = max(m_maxMBSize, (size_t)(m_offsets[c + 1] - m_offsets[c]));
					//fprintf(stderr, "m_offsets[%lu] = %lu\n", c, m_offsets[c]);
				}
				//fprintf(stderr, "max mb size: %ld\n", m_maxMBSize);
				size_t maxMem = 1024 * 1024 * 1024; // 1GB
				size_t maxPointers = maxMem / m_maxMBSize;

				if (!m_bQueueBufferAllocated)
				{

					for (size_t c = 0; c < maxPointers; c++) {
						void* dataBuffer = malloc(m_maxMBSize + sizeof(int32_t)); //modified by weiren, to add a # of sample
						m_dataToProduce.push(dataBuffer);
					}

					m_bQueueBufferAllocated = true;
				}

				std::thread readData([this] { this->ReadMinibatches(m_readOrder, m_readOrderLength); });
				readData.detach();

			}
			template<class ElemType>
			void* DenseBinaryInput<ElemType>::GetTempDataPointer(size_t numBytes) {
				if (m_tempValuesSize < numBytes) {
					if (m_tempValues != nullptr) {
						free(m_tempValues);
					}
					m_tempValuesSize = (int32_t)(numBytes * 1.3);
					m_tempValues = malloc(m_tempValuesSize);
				}
				return m_tempValues;
			}

			template<class ElemType>
			shared_ptr<BDenseBinaryMatrix<ElemType>> DenseBinaryInput<ElemType>::CreateMatrix(std::wstring matName, int deviceId) {
				shared_ptr<BDenseBinaryMatrix<ElemType>> retVal;// = nullptr;
				if (std::find(m_labels.begin(), m_labels.end(), matName) != m_labels.end()) {
					retVal = make_shared<SDenseBinaryMatrix<ElemType>>(matName, deviceId, m_mbSize, m_mappedNumCols[matName]);
				}

				return retVal;
			}

			template<class ElemType>
			void DenseBinaryInput<ElemType>::ReadMinibatches(size_t* read_order, size_t numToRead) {
#if DEBUG
				marker_series series(L"Read Minibatches");
				//diagnostic::span span(series, L"Reading Data");
				span* read_span;
#endif
				for (size_t c = 0; c < numToRead; c++) {
#if DEBUG
					read_span = new span(series, 1, L"Getting Buffer %ld\n", c);
#endif
					//fprintf(stderr, "start reading data %ld\n", c);
					size_t readSize = m_offsets[read_order[c] + 1] - m_offsets[read_order[c]];
					//void* data_buffer = GetTempDataPointer(readSize);

					int32_t sampleNumber = (int32_t)(readSize / sizeof(ElemType) / m_totalDim);
#if DEBUG
						series.write_flag(_T("Getting buffer."));
#endif
					void* data_buffer = m_dataToProduce.pop();

					memcpy(data_buffer, &sampleNumber, sizeof(int32_t));
					//set number of samples
#if DEBUG
					delete read_span;
					series.write_flag(_T("Got buffer."));
					read_span = new span(series, 2, L"Reading Data %ld\n", c);
#endif

					m_inFile.clear();
#if DEBUG
					series.write_flag(_T("seeking."));
#endif
					m_inFile.seekg(m_offsets[c], ios::beg);
#if DEBUG
					series.write_flag(_T("reading."));
#endif
					m_inFile.read((char*)(data_buffer) + sizeof(int32_t), readSize);

				

					for (int32_t c = 0; c < m_labels.size(); c++)
					{
						void* labelBuffer = m_mappedBuffer[m_labels[c]];
						int32_t labelStartIndex = m_mappedStartIndex[m_labels[c]];
						int32_t labelDim = m_mappedNumCols[m_labels[c]];
						void* pSource = (char*)data_buffer + sizeof(int32_t) + labelStartIndex*sizeof(ElemType);
						void* pDest = (char*)labelBuffer;

						//Put them into the buffer
						for (int32_t s = 0; s < sampleNumber; s++)
						{

							memcpy(pDest, pSource, labelDim*sizeof(ElemType));
							pSource = (char*)pSource + m_totalDim*sizeof(ElemType);
							pDest = (char*)pDest + labelDim*sizeof(ElemType);


						}
					}


					void* pDest = (char*)data_buffer + sizeof(int32_t);

			

					for (int32_t c = 0; c < m_labels.size(); c++)
					{
						void* labelBuffer = m_mappedBuffer[m_labels[c]];
						int32_t labelDim = m_mappedNumCols[m_labels[c]];
						memcpy(pDest, labelBuffer, sampleNumber* sizeof(ElemType)*labelDim);
						pDest = (char*)pDest + sampleNumber* sizeof(ElemType)*labelDim;

					}
				

					m_dataToConsume.push(data_buffer);
					//fprintf(stderr, "done reading data %ld\n", c);
#if DEBUG
					series.write_flag(_T("Done read, pushed buffer."));
					delete read_span;
#endif
				}
				//m_dataToConsume.push(nullptr);
#if DEBUG
				series.write_flag(_T("Done reading."));
#endif

			}

			template<class ElemType>
			size_t DenseBinaryInput<ElemType>::ReadMinibatch(void* data_buffer, std::map<std::wstring, shared_ptr<BDenseBinaryMatrix<ElemType>>>& matrices) {

				int32_t curMBSize;

				int64_t buffer_offset = 0;

				curMBSize = *(int32_t*)((char*)data_buffer + buffer_offset);
				buffer_offset += sizeof(int32_t);

				void* pSource = (char*)data_buffer + sizeof(int32_t);


				for (int32_t c = 0; c < m_labels.size(); c++)
				{
					//void* labelBuffer = m_mappedBufferForConsumption[m_labels[c]];
					int32_t labelDim = m_mappedNumCols[m_labels[c]];
					
					//void* pDest = (char*)labelBuffer;

					auto findMat = matrices.find(m_labels[c]);
					if (findMat != matrices.end())
					{
						auto mat = findMat->second;
						mat->AddValues(pSource, curMBSize);
#ifdef DEBUG
						mat->Print("labels");
#endif
					}

					pSource = (char*)pSource + sizeof(ElemType)* curMBSize*labelDim;
				}
				return (size_t)curMBSize;
			}

			template<class ElemType>
			size_t DenseBinaryInput<ElemType>::FillMatrices(std::map<std::wstring, shared_ptr<BDenseBinaryMatrix<ElemType>>>& matrices) {

				//fprintf(stderr, "start fill matrices\n");
				size_t curSize = 0;
				for (auto mat : matrices) {
					mat.second->SetMaxRows(m_mbSize);
					mat.second->Clear();
				}
				void* data_buffer;
				//fprintf(stderr, "start while\n");
				//clock_t start_w = clock();
				//while (curSize + m_microBatchSize <= m_mbSize && (data_buffer = m_dataToConsume.pop()) != nullptr) {
				while (curSize + m_microBatchSize <= m_mbSize && m_nextMB < m_epochSize) {
					data_buffer = m_dataToConsume.pop();
					//clock_t in_w = clock();
					//start_w = in_w - start_w;
					//fprintf(stderr, "start read mb\tIt took me %d clicks (%f seconds).\n", start_w, ((float)start_w) / CLOCKS_PER_SEC);
					//start_w = in_w;
					//fprintf(stderr, "start read mb\n");
					curSize += ReadMinibatch(data_buffer, matrices);
					//fprintf(stderr, "end read mb\n");
					m_nextMB++;
					m_dataToProduce.push(data_buffer);
				}
				//fprintf(stderr, "end fill matrices\n");
				return curSize;
			}

			template<class ElemType>
			template<class ConfigRecordType>
			void DenseReader<ElemType>::InitFromConfig(const ConfigRecordType & readerConfig) {

				std::map<std::wstring, std::wstring> rename;
				RenamedMatrices(readerConfig, rename);

				if (readerConfig.Exists(L"randomize"))
				{
					string randomizeString = readerConfig(L"randomize");
					if (randomizeString == "None")
					{
						m_randomize = 0L;
					}
					else if (randomizeString == "Auto")
					{
						time_t rawtime;
						struct tm* timeinfo;
						time(&rawtime);
						timeinfo = localtime(&rawtime);
						m_randomize = (unsigned long)(timeinfo->tm_sec + timeinfo->tm_min * 60 + timeinfo->tm_hour * 60 * 60);
					}
					else
					{
						m_randomize = readerConfig(L"randomize", 0);
					}
				}
				else
				{
					m_randomize = 0L;
				}

				m_partialMinibatch = true;
				std::string minibatchMode(readerConfig(L"minibatchMode", "Partial"));
				m_partialMinibatch = !_stricmp(minibatchMode.c_str(), "Partial");

				std::wstring file = readerConfig(L"file", L"");

				m_dataInput = make_shared<DenseBinaryInput<ElemType>>(file);
				m_dataInput->Init(rename, readerConfig);

				m_mbSize = (size_t)readerConfig(L"minibatch", 0);
				if (m_mbSize > 0)
				{
					if (m_dataInput->GetMBSize() != m_mbSize)
					{
						RuntimeError("Data file and config file have mismatched minibatch sizes.\n");
						return;
					}
				}
				else
				{
					m_mbSize = m_dataInput->GetMBSize();
				}

				m_prefetchEnabled = true;
			}

			template<class ElemType>
			void DenseReader<ElemType>::Destroy() {

			}

			template<class ElemType>
			DenseReader<ElemType>::~DenseReader()
			{
				Destroy();
			}

			template<class ElemType>
			void DenseReader<ElemType>::StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples) {
				return StartDistributedMinibatchLoop(mbSize, epoch, 0, 1, requestedEpochSamples);
			}

			template<class ElemType>
			void DenseReader<ElemType>::StartDistributedMinibatchLoop(size_t mbSize, size_t epoch, size_t subsetNum, size_t numSubsets, size_t /*requestedEpochSamples*/) {
				m_epoch = epoch;
				m_mbSize = mbSize;
#if DEBUG
				if (reader_series != NULL) {
					delete reader_series;
				}
				reader_series = new marker_series(L"Base Reader");
				cur_read = 0;
#endif
				m_dataInput->StartDistributedMinibatchLoop(mbSize, subsetNum, numSubsets);

			}

			template<class ElemType>
			void DenseReader<ElemType>::CheckDataMatrices(std::map<std::wstring, Matrix<ElemType>*>& matrices) {
				if (m_dataMatrices.empty())
				{
					for (auto inmat : matrices) {
						shared_ptr<BDenseBinaryMatrix<ElemType>> mat = m_dataInput->CreateMatrix(inmat.first, inmat.second->GetDeviceId());
						if (mat != nullptr) {
							m_dataMatrices[inmat.first] = mat;
						}
					}
				}
			}
			template<class ElemType>
			void DenseReader<ElemType>::DoDSSMMatrix(Matrix<ElemType>& mat, size_t actualMBSize) {
				size_t numRows = mat.GetNumRows();
				if (DSSMCols < actualMBSize) {
					if (DSSMLabels != nullptr) {
						//free(DSSMLabels);
						CUDAPageLockedMemAllocator::Free(DSSMLabels, mat.GetDeviceId());
					}
					DSSMCols = actualMBSize;
					//DSSMLabels = (ElemType*)malloc(sizeof(ElemType)*numRows*actualMBSize);
					DSSMLabels = (ElemType*)CUDAPageLockedMemAllocator::Malloc(sizeof(ElemType)*numRows*actualMBSize, mat.GetDeviceId());
					memset(DSSMLabels, 0, sizeof(ElemType)*numRows*actualMBSize);
					for (size_t c = 0; c < numRows*actualMBSize; c += numRows) {
						DSSMLabels[c] = 1;
					}
				}
				if (mat.GetNumCols() != actualMBSize) {
					mat.SetValue(numRows, actualMBSize, mat.GetDeviceId(), DSSMLabels, matrixFlagNormal);
				}
			}


			template<class ElemType>
			bool DenseReader<ElemType>::GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices) {
				//timer = clock();
#if DEBUG
				span minibatch_span(*reader_series, 1, L"Get Minibatch: %ld", cur_read);
#endif
				size_t actualMBSize = 0;
				if (m_prefetchEnabled)
				{
					if (!m_pendingAsyncGetMinibatch.valid()) {
						//fprintf(stderr, "not valid\n");
						CheckDataMatrices(matrices);
						m_pendingAsyncGetMinibatch = std::async(std::launch::async, [this]()
						{
							return m_dataInput->FillMatrices(m_dataMatrices);
						});
					}
					//fprintf(stderr, "before get.\n");
					//timer = clock();
#if DEBUG
					reader_series->write_flag(_T("before get."));
#endif
					actualMBSize = m_pendingAsyncGetMinibatch.get();
#if DEBUG
					reader_series->write_flag(_T("after get."));
#endif
					//timer = clock() - timer;
					//fprintf(stderr, "done get\tIt took me %d clicks (%f seconds).\n", timer, ((float)timer) / CLOCKS_PER_SEC);

					if (actualMBSize == 0) {
						return false;
					}

					m_pMBLayout->InitAsFrameMode(actualMBSize);
#if DEBUG
					reader_series->write_flag(_T("starting fill."));
#endif
					for (auto matrix : m_dataMatrices) {
						auto findMat = matrices.find(matrix.first);
						if (findMat != matrices.end()) {
							matrix.second->Fill(findMat->second);
						}
					}
#if DEBUG
					reader_series->write_flag(_T("done fill."));
#endif
					auto findMat = matrices.find(L"DSSMLabel");
					if (findMat != matrices.end())
					{
						DoDSSMMatrix(*(findMat->second), actualMBSize);

					}
					m_pendingAsyncGetMinibatch = std::async(std::launch::async, [this]()
					{
						//CheckDataMatrices(matrices);
						return m_dataInput->FillMatrices(m_dataMatrices);
					});
				}
#if DEBUG
				cur_read++;
#endif
				/*

				timer = clock() - timer;
				fprintf(stderr, "It took me %d clicks (%f seconds).\n", timer, ((float)timer) / CLOCKS_PER_SEC);
				*/
				//fprintf(stderr, "done\n");

				m_pMBLayout->InitAsFrameMode(actualMBSize);
				return true;
			}


			template<class ElemType>
			template<class ConfigRecordType>
			void DenseReader<ElemType>::RenamedMatrices(const ConfigRecordType& config, std::map<std::wstring, std::wstring>& rename) {
				for (const auto & id : config.GetMemberIds())
				{
					if (!config.CanBeConfigRecord(id))
						continue;
					const ConfigRecordType & temp = config(id);
					// see if we have a config parameters that contains a "dim" element, it's a sub key, use it
					if (temp.ExistsCurrent(L"rename"))
					{

						std::wstring ren = temp(L"rename");
						rename.emplace(msra::strfun::utf16(id), msra::strfun::utf16(ren));
					}
				}
			}

			template<class ElemType>
			bool DenseReader<ElemType>::DataEnd()
			{
				return m_dataInput->DataEnd();
			}

			template<class ElemType>
			bool DenseBinaryInput<ElemType>::DataEnd()
			{
				return true;
			}

			// instantiate all the combinations we expect to be used
			template class DenseReader<double>;
			template class DenseReader<float>;
		}
	}

}

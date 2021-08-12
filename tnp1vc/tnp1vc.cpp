// tnp1vc.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <thread>

namespace tnp1 {
	const uint64_t STOP_AFTER = 1000;

	const uint64_t WORKER_COUNT = 16;
	const uint64_t WORK_CHUNK_SIZE = 64 * 1024;
	const size_t WORK_CHUNK_BYTES = WORK_CHUNK_SIZE * sizeof(uint16_t);
	const size_t CACHE_SIZE = 4ull * (1024 * 1024 * 1024) / 2 - 1;

	std::atomic_bool programRunning = true;

	// sharedCache layout
	// [ done_read_only    |readLimit|    in_progress_mutable    |writeBase|     waiting to work]
	uint16_t* globalCache = NULL;
	std::atomic_size_t globalReadLimit;
	std::atomic_size_t globalWriteBase;

	// Tracking max things (I think we can guarantee these are only accessed from one thread)
	uint64_t globalMaxN = 0;
	uint64_t globalMaxIterations = 0;

	void workerThread(uint16_t workerId) {
		uint16_t localCache[WORK_CHUNK_SIZE];
		uint64_t localReadLimit;
		uint64_t localWriteBase;

		uint64_t localMaxNOffset;
		uint64_t localMaxIterations;

		for (uint64_t idx = 0; idx < WORK_CHUNK_SIZE; ++idx) {
			localCache[idx] = 1000;
		}

		while (programRunning.load(std::memory_order::memory_order_relaxed)) {
			localWriteBase = globalWriteBase.fetch_add(WORK_CHUNK_SIZE, std::memory_order::memory_order_acq_rel);
			localReadLimit = globalReadLimit.load(std::memory_order::memory_order_acquire);

			localMaxNOffset = 0;
			localMaxIterations = 0;

			for (uint64_t offset = 0; offset < WORK_CHUNK_SIZE; ++offset) {
				uint64_t iteration_count = 0;
				uint64_t n = localWriteBase + offset;

				do {
					if (n & 1) {
						// n is odd
						iteration_count += 2;
						n = (3 * n + 1) >> 1;
					}
					else {
						// n is even
						iteration_count += 1;
						n >>= 1;
					}
				} while (n >= localReadLimit);

				uint64_t localIterations = globalCache[n] + iteration_count;

				localCache[offset] = localIterations;

				if (localIterations > localMaxIterations) {
					localMaxIterations = localIterations;
					localMaxNOffset = offset;
				}
			} 

			// Wait for the read limit to catch up to here, once localReadLimit == localWriteBase, it's our turn
			while (globalReadLimit.load(std::memory_order::memory_order_relaxed) != localWriteBase) {
				_mm_pause();
			};

			localReadLimit = globalReadLimit.load(std::memory_order::memory_order_acquire);

			if (localMaxIterations > globalMaxIterations) {
				globalMaxIterations = localMaxIterations;
				globalMaxN = localWriteBase + localMaxNOffset;
				//std::cout << "new max iterations " << globalMaxIterations << " for n " << globalMaxN << std::endl;
			}

			if (localWriteBase + WORK_CHUNK_SIZE < CACHE_SIZE) {
				std::memcpy(&globalCache[localWriteBase], &localCache[0], WORK_CHUNK_BYTES);
			}
			else {
				size_t copyBytes = std::max((size_t)0, CACHE_SIZE - localWriteBase) * sizeof(uint16_t);
				std::memcpy(&globalCache[localWriteBase], &localCache[0], copyBytes);
			}

			globalReadLimit.store(localReadLimit + WORK_CHUNK_SIZE, std::memory_order::memory_order_release);

			if (globalMaxIterations >= STOP_AFTER) {
				programRunning.store(false, std::memory_order::memory_order_relaxed);
			}
		}
	}
}

int main()
{
	auto begin = std::chrono::high_resolution_clock::now();

	tnp1::globalCache = new uint16_t[tnp1::CACHE_SIZE];
	tnp1::globalCache[0] = USHRT_MAX;
	tnp1::globalCache[1] = 0;
	tnp1::globalWriteBase = 2;
	tnp1::globalReadLimit = 2;

	std::thread workers[tnp1::WORKER_COUNT];

	for (uint16_t workerId = 0; workerId < tnp1::WORKER_COUNT; workerId++) {
		workers[workerId] = std::thread(tnp1::workerThread, workerId);
	}

	for (uint16_t workerId = 0; workerId < tnp1::WORKER_COUNT; workerId++) {
		workers[workerId].join();
	}

	delete[] tnp1::globalCache;

	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin);

	std::cout << "Found that " << tnp1::globalMaxN << " took " << tnp1::globalMaxIterations << " iterations. Total run time " 
		<< duration.count() << " milliseconds" << std::endl;

	return 0;
}

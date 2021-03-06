#pragma once

#include <atomic>
#include <vector>
#include <concurrentqueue.h>

#include "timsort.h"

#include "ConfigurationHandler.h"

constexpr uint_fast32_t LOG_DEQUE_SIZE = 1024;


struct QueueAndSize
{
	std::atomic<uint64_t> _insertPos;
	std::atomic<int_fast32_t> _writers;
	moodycamel::ConcurrentQueue<LogData> _queue;

	QueueAndSize() : _insertPos(0), _writers(0), _queue() {}

	void AddToQueueUnordered(LogData && l)
	{
		l._insertionPoint = _insertPos++;
		_queue.enqueue(std::move(l));
	}

	void AddToQueueOrdered(LogData&& l)
	{
		++_writers;
		l._insertionPoint = _insertPos++;
		_queue.enqueue(std::move(l));
		--_writers;
	}

	void Reset()
	{
		_writers = 0;
		_insertPos = 0;
	}
};


// ------------------------------------------------------------------------------------------------------
// Assumptions:
// - Multiple producers, single consumer.  
//
// If there's multiple consumers, things will almost certainly go awry.
// ------------------------------------------------------------------------------------------------------
class ConcurrentQueueWrapper
{
private:
	std::atomic<uint64_t> _requestsRemaining;

	//std::shared_ptr<QueueAndSize> _activeQueue;

	std::function<void(LogData&&)> _handleIn;
	std::function<void(std::vector<LogData>&)> _handleOut;

	QueueAndSize _queue1;
	QueueAndSize _queue2;

	QueueAndSize* _standbyQueue;
	std::atomic<QueueAndSize*> _activeQueue;

	std::vector<LogData> _tmpDequeue;

	// ------------------------------------------------------------------------------------------------------
	// Specialization for sorted queues (preserve dequeue order)
	// ------------------------------------------------------------------------------------------------------
	void EnqueueSorted(LogData&& l)
	{
		_activeQueue.load(std::memory_order_acquire)->AddToQueueOrdered(std::move(l));
	}

	// ------------------------------------------------------------------------------------------------------
	// Specialization for unsorted queues (preserve speed)
	// ------------------------------------------------------------------------------------------------------
	void EnqueueUnsorted(LogData&& l)
	{
		_queue1.AddToQueueUnordered(std::move(l));
	}

	// ------------------------------------------------------------------------------------------------------
	// Dequeue data in an ordered way.
	// ------------------------------------------------------------------------------------------------------
	void DequeueSorted(std::vector<LogData>& toWhere)
	{
		_standbyQueue = _activeQueue.exchange(_standbyQueue);

		while (_standbyQueue->_writers.load(std::memory_order_relaxed) != 0) { std::this_thread::yield(); }
		const uint64_t maxSize = _standbyQueue->_insertPos.load(std::memory_order_relaxed) + 1;
		
		if (maxSize <= 1) 
		{
			toWhere.clear();
			return;
		}
		
		// Timsort to sort by input ID.
		toWhere.resize(maxSize);
		const uint64_t actualSize = _standbyQueue->_queue.try_dequeue_bulk(toWhere.data(), maxSize);
		toWhere.resize(actualSize);
		gfx::timsort(toWhere.begin(), toWhere.end());
		
		// The sorted variant - iterate through a raw dequeue_bulk and pointer swap into a container
		// in a sorted way - isn't as fast as timsort by far on average.

		_requestsRemaining -= actualSize;
		_standbyQueue->Reset();
	}

	// ------------------------------------------------------------------------------------------------------
	// Dequeue data without concern for preserving the order of the queue.
	// ------------------------------------------------------------------------------------------------------
	void DequeueUnsorted(std::vector<LogData>& toWhere)
	{
		toWhere.resize(LOG_DEQUE_SIZE);
		//const size_t numDequeued = _activeQueue->_queue.try_dequeue_bulk(toWhere.data(), LOG_DEQUE_SIZE);
		const size_t numDequeued = _queue1._queue.try_dequeue_bulk(toWhere.data(), LOG_DEQUE_SIZE);
		toWhere.resize(numDequeued);
		_requestsRemaining -= numDequeued;
	}

public:

	ConcurrentQueueWrapper() :
		_requestsRemaining(0),
		_handleIn([this](LogData&& l) { EnqueueSorted(std::move(l)); }),
		_handleOut([this](std::vector<LogData>& toLog) { DequeueSorted(toLog); }),
		_queue1(),
		_queue2(),
		_standbyQueue(nullptr),
		_activeQueue(nullptr),
		_tmpDequeue()
	{
		_standbyQueue = &_queue2;
		_activeQueue = &_queue1;
	}

	~ConcurrentQueueWrapper() {}

	// ------------------------------------------------------------------------------------------------------
	// Load number of requests not yet processed in the queue (num in - num dequed)
	// ------------------------------------------------------------------------------------------------------
	uint64_t GetRequestsRemaining() const { return _requestsRemaining.load(std::memory_order_relaxed); }

	void AddToQueue(LogData&& l)
	{
		++_requestsRemaining;
		_handleIn(std::move(l));
	}

	void Dequeue(std::vector<LogData>& toLog)
	{
		_handleOut(toLog);
	}

	void HandleDataUnordered()
	{
		_handleIn = [this](LogData&& l) { EnqueueUnsorted(std::move(l)); };
		_handleOut = [this](std::vector<LogData>& toLog) { DequeueUnsorted(toLog); };
	}
	void HandleDataOrdered()
	{
		_handleIn = [this](LogData&& l) { EnqueueSorted(std::move(l)); };
		_handleOut = [this](std::vector<LogData>& toLog) { DequeueSorted(toLog); };
	}

};

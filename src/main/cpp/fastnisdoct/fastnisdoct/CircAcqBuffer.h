#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>

/*
Push-only ring buffer inspired by ring buffer interface of National Instruments IMAQ software.

Elements pushed to the ring are given a count corresponding to the number of times push() has been called 
since the buffer was initialized. A push() constitutes a copy into buffer-managed memory.

Any n-th element can be locked out of the ring for processing, copy or display and then subsequently released.

If the n-th element isn't available yet, is already locked out, or is being accessed by another thread,
lock_out() returns -1 after timing out.

If the n-th element has been overwritten, the buffer where the n-th element would have been
is returned instead along with the count of the element you have actually locked out.

github.com/sstucker
2021
*/

typedef std::chrono::high_resolution_clock clk;
typedef std::chrono::microseconds us;

inline int mod2(int a, int b)
{
	int r = a % b;
	return r < 0 ? r + b : r;
}

template <typename T>
struct CircAcqElement
{
	T* arr;  // the buffer
	int index;  // position of data in ring 
	std::atomic_int count;  // the count of the data currently in the buffer. Needs to be atomic as it is polled from outside the lock
};


template <class T>
class CircAcqBuffer
{
protected:

	CircAcqElement<T>** ring;
	CircAcqElement<T>* locked_out_buffer;
	int ring_size;
	uint64_t element_size;
	std::atomic_long count;  // cumulative count
	std::atomic_int locked;  // index of currently locked out buffer
	std::atomic_int head;  // Head of buffer (receives push)
	std::deque<std::mutex> locks;

	inline void _swap(int n)
	{
		locked.store(n);  // Update locked out value

		// Pointer swap
		CircAcqElement<T>* tmp = locked_out_buffer;
		locked_out_buffer = ring[n];
		ring[n] = tmp;

		// Update index to buffer's new position in ring
		ring[n]->index = n;
	}

	inline long _lock_out(int n, T** buffer, int timeout_ms)
	{
		auto start = clk::now();  // Start timeout timer
		int timeout_us = timeout_ms * 1000;  // Compare using integer microseconds
		while (locked.load() != -1)
		{
			if (std::chrono::duration_cast<us>(clk::now() - start).count() > timeout_us)
			{
				printf("CircAcqBuffer: Timed out waiting for locked out buffer to be released.\n");
				return -1;
			}
		}
		int requested = mod2(n, ring_size);  // Get index of buffer where requested element is/was
		while (n > ring[requested]->count.load())
		{
			if (std::chrono::duration_cast<us>(clk::now() - start).count() > timeout_us)
			{
				printf("CircAcqBuffer: Timed out trying to acquire %i for %i ms.\n", n, timeout_ms);
				return -1;
			}
		}
		while (!locks[requested].try_lock())
		{
			if (std::chrono::duration_cast<us>(clk::now() - start).count() > timeout_us)
			{
				printf("CircAcqBuffer: Timed out trying to unlock buffer %i for %i ms.\n", requested, timeout_ms);
				return -1;
			}
		}
		_swap(requested);
		*buffer = locked_out_buffer->arr;  // Return pointer to locked out buffer's array by reference
		auto locked_out = locked_out_buffer->count.load();  // Return true count of the locked out buffer
		locks[requested].unlock();
		return locked_out;
	}

public:

	CircAcqBuffer()
	{
		ring_size = 0;
		element_size = 0;
		head = ATOMIC_VAR_INIT(0);
	}

	CircAcqBuffer(int number_of_buffers, uint64_t frame_size)
	{
		ring_size = number_of_buffers;
		element_size = frame_size;
		head = ATOMIC_VAR_INIT(0);
		locked = ATOMIC_VAR_INIT(-1);
		ring = new CircAcqElement<T>*[ring_size];
		locks.resize(ring_size);
		for (int i = 0; i < ring_size; i++)
		{
			ring[i] = new(CircAcqElement<T>);
			ring[i]->arr = new T[element_size];
			ring[i]->index = i;
			ring[i]->count = -1;
		}
		// locked_out_buffer maps to memory swapped in to replace a buffer when it is locked out
		locked_out_buffer = new(CircAcqElement<T>);
		locked_out_buffer->arr = new T[element_size];
		locked_out_buffer->index = -1;
		locked_out_buffer->count = -1;
		count = ATOMIC_VAR_INIT(-1);
	}

	long lock_out(int n, T** buffer, int timeout_ms)
	{
		return _lock_out(n, buffer, timeout_ms);
	}

	long lock_out(int n, T** buffer)
	{
		return _lock_out(n, buffer, 0);
	}

	void release()
	{
		locked.store(-1);
	}

	int push(T* src)
	{
		int oldhead = head;
		locks[head].lock();
		memcpy(ring[head]->arr, src, sizeof(T) * element_size);
		ring[head]->count.store(count);
		head = mod2(head + 1, ring_size);
		count += 1;
		locks[oldhead].unlock();
		return oldhead;
	}

	T* lock_out_head()
	{
		locks[head].lock();
		return ring[head]->arr;
	}

	int release_head()
	{
		count += 1;
		ring[head]->count = count;
		int oldhead = head;
		head = mod2(head + 1, ring_size);
		locks[oldhead].unlock();
		return oldhead;
	}

	int get_count()
	{
		return count.load();
	}

	void clear()
	{
		for (int i = 0; i < ring_size; i++)
		{
			locks[i].lock();
			ring[i]->index = i;
			ring[i]->count = -1;
			locks[i].unlock();
		}
		count.store(-1);
		head.store(0);
		locked.store(-1);
		locked_out_buffer->index = -1;
		locked_out_buffer->count = -1;
	}

	~CircAcqBuffer()
	{
		for (int i = 0; i < ring_size; i++)
		{
			delete[] ring[i]->arr;
		}
		delete[] ring;
		delete[] locked_out_buffer->arr;
		delete locked_out_buffer;
	}

};

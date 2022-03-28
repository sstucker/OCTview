#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>

// Push-only ring buffer inspired by buffer interface of National Instruments IMAQ software. Elements
// pushed to the ring are given a count corresponding to the number of times push() has been called
// since the buffer was initialized. A push() constitutes a copy into buffer-managed memory. The n-th element
// can be locked out of the ring for processing, copy or display and then subsequently released. If the n-th
// element isn't available yet, lock_out_nowait() function returns -1 and lock_out_wait() spinlocks until the requested
// count is available. If the n-th element has been overwritten, the buffer where the n-th element would have been
// is returned instead along with the count of the element you have actually locked out. Somewhat thread-safe but
// only designed for single-producer single-consumer use.
//
// sstucker 2021
//

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
	long element_size;
	int head;
	std::atomic_long count;  // cumulative count
	std::mutex* locks;  // Locks on each ring pointer
	std::atomic_int locked;  // index of currently locked buffer

	inline void _lock_out(int n)
	{
		locked.store(n);  // Update locked out value

		// Pointer swap
		CircAcqElement<T>* tmp = locked_out_buffer;
		locked_out_buffer = ring[n];
		ring[n] = tmp;

		// Update index to buffer's new position in ring
		ring[n]->index = n;
	}

public:

	CircAcqBuffer()
	{
		ring_size = 0;
		element_size = 0;
		head = 1;
	}

	const T* operator [](int i) const { return ring[mod2(i, ring_size)]->arr; }

	CircAcqBuffer(int number_of_buffers, long frame_size)
	{
		ring_size = number_of_buffers;
		element_size = frame_size;
		head = 0;
		locked = ATOMIC_VAR_INIT(-1);
		ring = new CircAcqElement<T>*[ring_size];
		locks = new std::mutex[ring_size];
		for (int i = 0; i < ring_size; i++)
		{
			ring[i] = new(CircAcqElement<T>);
			ring[i]->arr = new T[element_size];
			ring[i]->index = i;
			ring[i]->count = -1;
		}
		// locked_out_buffer maps to actual storage swapped in to replace a buffer when it is locked out
		locked_out_buffer = new(CircAcqElement<T>);
		locked_out_buffer->arr = new T[element_size];
		locked_out_buffer->index = -1;
		locked_out_buffer->count = -1;
		count = ATOMIC_VAR_INIT(-1);
	}

	long lock_out_nowait(int n, T** buffer)
	{
		if (locked.load() != -1)  // Only one buffer can be locked out at a time
		{
			return -1;
		}
		else
		{
			int requested = mod2(n, ring_size);  // Get index of buffer where requested element is/was
			if (!locks[requested].try_lock())  // Can't lock out/push to same element from two threads at once
			{
				_lock_out(requested);
				locks[requested].unlock();  // Exit critical section
				*buffer = locked_out_buffer->arr;  // Return pointer to locked out buffer's array
				return locked_out_buffer->count.load();  // Return n-th buffer you actually got
			}
			else
			{
				return -1;
			}
		}
	}

	long lock_out_wait(int n, T** buffer)
	{
		while (locked.load() != -1);  // Only one buffer can be locked out at a time
		int got = -1;
		int requested = mod2(n, ring_size);
		while (n > ring[requested]->count.load() || ring[requested]->count.load() == -1);  // Spinlock if buffer n-th buffer hasn't been pushed yet. You might wait forever!
		while (!locks[requested].try_lock());
		_lock_out(requested);
		locks[requested].unlock();  // Exit critical section
		*buffer = locked_out_buffer->arr;  // Return pointer to locked out buffer's array
		return locked_out_buffer->count.load();  // Return n-th buffer you actually got
	}

	void release()
	{
		locked.store(-1);
	}

	int push(T* src)
	{
		while (!locks[head].try_lock());
		memcpy(ring[head]->arr, src, sizeof(T) * element_size);
		ring[head]->count.store(count);
		int oldhead = head;
		head = mod2(head + 1, ring_size);
		locks[oldhead].unlock();
		count += 1;
		return oldhead;
	}

	// Interface to copy into buffer head directly if a push is too expensive.

	T* lock_out_head()
	{
		while (!locks[head].try_lock());
		printf("Locked out %i\n", head);
		return ring[head]->arr;
	}

	int release_head()
	{
		count += 1;
		ring[head]->count = count;
		int oldhead = head;
		head = mod2(head + 1, ring_size);
		locks[oldhead].unlock();
		printf("Released %i, head now %i\n", oldhead, head);
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
			while (!locks[i].try_lock());
			ring[i]->index = i;
			ring[i]->count = -1;
			locks[i].unlock();
		}
		count = 0;
		head = 0;
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
		delete[] locks;
		delete locked_out_buffer;
	}

};

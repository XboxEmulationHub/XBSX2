// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Threading.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace UWPHost
{
	class CPUEventQueue
	{
	private:
		struct Completion
		{
			Threading::KernelSemaphore sema;
			std::atomic_bool executed{false};

			void Complete(bool was_executed)
			{
				executed.store(was_executed, std::memory_order_release);
				sema.Post();
			}

			bool Wait()
			{
				sema.Wait();
				return executed.load(std::memory_order_acquire);
			}
		};

	public:
		/// Returns whether the function was queued, or executed if block is true.
		/// Blocking calls must come from a thread other than the CPU thread.
		bool Queue(std::function<void()> function, bool block = false)
		{
			auto completion = block ? std::make_shared<Completion>() : nullptr;
			{
				std::lock_guard lock(m_mutex);
				if (!m_accept_events)
					return false;
				m_events.push_back({std::move(function), completion});
			}
			m_cv.notify_one();
			return !completion || completion->Wait();
		}

		/// Executes the next function. Must be called on the CPU thread.
		bool ProcessNext() noexcept
		{
			Event event;
			{
				std::lock_guard lock(m_mutex);
				if (m_events.empty())
					return false;
				event = std::move(m_events.front());
				m_events.pop_front();
			}
			event.function();
			if (event.completion)
				event.completion->Complete(true);
			return true;
		}

		void WaitForEvent(std::chrono::milliseconds timeout)
		{
			std::unique_lock lock(m_mutex);
			m_cv.wait_for(lock, timeout, [this]() { return !m_events.empty() || !m_accept_events; });
		}

		/// Rejects new events and cancels pending callbacks, releasing blocked callers.
		/// Does not wait for a running callback or join the CPU thread.
		void Close()
		{
			std::deque<Event> pending;
			{
				std::lock_guard lock(m_mutex);
				m_accept_events = false;
				pending.swap(m_events);
			}
			// Release blocked callers whose callbacks were discarded during shutdown.
			for (Event& event : pending)
			{
				if (event.completion)
					event.completion->Complete(false);
			}
			m_cv.notify_all();
		}

	private:
		struct Event
		{
			std::function<void()> function;
			std::shared_ptr<Completion> completion;
		};

		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::deque<Event> m_events;
		bool m_accept_events = true;
	};
} // namespace UWPHost

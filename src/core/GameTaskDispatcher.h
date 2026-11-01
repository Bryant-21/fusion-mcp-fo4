#pragma once

// Project-owned GPL-3.0 code. Engine-free, bounded F4SE task handoff policy.
#include "core/RequestBroker.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace fusionmcp
{
	enum class GameWakeResult { Posted, Outstanding, Disabled, StaleEpoch, Stopped, PostFailed };
	const char* GameWakeResultName(GameWakeResult result) noexcept;
	struct GameDispatcherSnapshot
	{
		EpochId epoch = 0;
		bool enabled = false;
		bool stopped = false;
		bool outstanding = false;
		bool running = false;
	};

	class GameTaskDispatcher
	{
	public:
		using Task = std::function<void()>;
		using PostTask = std::function<bool(Task)>;
		using Pump = std::function<void(EpochId, std::size_t)>;
		// Inject process-lifetime-safe wrappers, not captured engine pointers or
		// references to a listener's stack. The task itself owns only shared policy
		// state and its ticket. No engine object or request argument is retained.
		GameTaskDispatcher(PostTask post, Pump pump, std::size_t budget = 4);
		~GameTaskDispatcher();
		GameTaskDispatcher(const GameTaskDispatcher&) = delete;
		GameTaskDispatcher& operator=(const GameTaskDispatcher&) = delete;
		GameTaskDispatcher(GameTaskDispatcher&&) = delete;
		GameTaskDispatcher& operator=(GameTaskDispatcher&&) = delete;

		// Default disabled, epoch zero. Changes invalidate queued wake authority
		// without resetting the physical outstanding latch. Zero cannot enable.
		// Epochs version the enabled state. Publish one coherent provider snapshot;
		// stale epochs and conflicting same-epoch states are ignored. A legitimate
		// enabled-state change needs a newer epoch (or terminal Stop()).
		void SetLifecycle(EpochId epoch, bool enabled);
		// Cancel logical authority even within the same epoch. F4SE cancellation
		// cannot remove a physical task; another wake waits for that task to run
		// or for its last retained closure to be destroyed.
		void Invalidate();
		void Stop();
		GameDispatcherSnapshot Snapshot() const;

		// Post and Pump run outside the policy mutex. At most one physical wake
		// is posting, queued or running. A callback pumps at most once (1..4 jobs),
		// catches all exceptions and never posts a successor. Only an external
		// submission/poll may ask for another wake after the prior callback ends.
		// Lifecycle changes cannot retract an already-started pump: it receives
		// the claimed epoch and must gate every broker job/publication on it.
		// Posted means PostTask returned true, not an enqueue acknowledgement or
		// operation completion (F4SE's AddTask returns void). A false/throwing post
		// invalidates any retained callback but preserves its physical latch.
		GameWakeResult RequestWake(EpochId expectedEpoch);

	private:
		struct State;
		struct Ticket;
		std::shared_ptr<State> state_;
	};
}

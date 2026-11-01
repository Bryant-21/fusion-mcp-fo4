#include "core/GameTaskDispatcher.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace fusionmcp
{
	const char* GameWakeResultName(GameWakeResult result) noexcept
	{
		switch (result) {
		case GameWakeResult::Posted: return "posted";
		case GameWakeResult::Outstanding: return "outstanding";
		case GameWakeResult::Disabled: return "disabled";
		case GameWakeResult::StaleEpoch: return "stale_epoch";
		case GameWakeResult::Stopped: return "stopped";
		case GameWakeResult::PostFailed: return "post_failed";
		}
		return "unknown";
	}

	struct GameTaskDispatcher::State
	{
		std::mutex mutex;
		PostTask post;
		Pump pump;
		std::size_t budget = 4;
		EpochId epoch = 0;
		std::uint64_t generation = 1;
		std::uint64_t nextToken = 1;
		std::uint64_t outstandingToken = 0;
		bool enabled = false;
		bool stopped = false;
		bool posting = false;
		bool running = false;
		void Invalidate() noexcept
		{
			if (generation == std::numeric_limits<std::uint64_t>::max()) {
				stopped = true;
				enabled = false;
			} else {
				++generation;
			}
		}
	};

	struct GameTaskDispatcher::Ticket
	{
		std::shared_ptr<State> state;
		const EpochId epoch;
		const std::uint64_t generation;
		const std::uint64_t token;
		// All mutable ticket fields share State::mutex. Ticket completion is
		// idempotent even if a posting wrapper copies or invokes the task twice.
		bool claimed = false;
		bool completed = false;
		bool cancelled = false;
		Ticket(std::shared_ptr<State> owner, EpochId currentEpoch, std::uint64_t currentGeneration, std::uint64_t currentToken) :
			state(std::move(owner)), epoch(currentEpoch), generation(currentGeneration), token(currentToken) {}
		~Ticket() { Finish(); }
		void Finish() noexcept
		{
			std::lock_guard lock(state->mutex);
			completed = true;
			if (state->outstandingToken == token) {
				state->running = false;
				// An inline callback may finish while PostTask is still active.
				// Preserve its reservation until that external call also returns.
				if (!state->posting) state->outstandingToken = 0;
			}
		}
		void Run() noexcept
		{
			bool execute = false;
			{
				std::lock_guard lock(state->mutex);
				if (claimed || completed) return;
				claimed = true;
				execute = state->outstandingToken == token && !cancelled &&
					!state->stopped && state->enabled && epoch == state->epoch && generation == state->generation;
				if (execute) state->running = true;
			}
			if (execute) {
				try { state->pump(epoch, state->budget); }
				catch (...) { /* No exception may cross F4SE TaskDelegate::Run. */ }
			}
			Finish();
		}
	};

	GameTaskDispatcher::GameTaskDispatcher(PostTask post, Pump pump, std::size_t budget) : state_(std::make_shared<State>())
	{
		if (!post || !pump || budget == 0 || budget > 4) throw std::invalid_argument("Invalid game task dispatcher callbacks or budget.");
		state_->post = std::move(post);
		state_->pump = std::move(pump);
		state_->budget = budget;
	}
	GameTaskDispatcher::~GameTaskDispatcher() { Stop(); }
	void GameTaskDispatcher::SetLifecycle(EpochId epoch, bool enabled)
	{
		const auto state = state_;
		std::lock_guard lock(state->mutex);
		if (epoch < state->epoch) return;
		enabled = enabled && epoch != 0 && !state->stopped;
		if (epoch == state->epoch && enabled != state->enabled) return;
		if (state->epoch != epoch || state->enabled != enabled) state->Invalidate();
		state->epoch = epoch;
		state->enabled = enabled && !state->stopped;
	}
	void GameTaskDispatcher::Invalidate()
	{
		const auto state = state_;
		std::lock_guard lock(state->mutex);
		state->Invalidate();
	}
	void GameTaskDispatcher::Stop()
	{
		const auto state = state_;
		std::lock_guard lock(state->mutex);
		state->stopped = true;
		state->enabled = false;
		state->Invalidate();
	}
	GameDispatcherSnapshot GameTaskDispatcher::Snapshot() const
	{
		const auto state = state_;
		std::lock_guard lock(state->mutex);
		return { state->epoch, state->enabled, state->stopped, state->outstandingToken != 0, state->running };
	}
	GameWakeResult GameTaskDispatcher::RequestWake(EpochId expectedEpoch)
	{
		const auto state = state_;
		std::shared_ptr<Ticket> ticket;
		{
			std::lock_guard lock(state->mutex);
			if (state->stopped) return GameWakeResult::Stopped;
			if (!state->enabled) return GameWakeResult::Disabled;
			if (expectedEpoch != state->epoch) return GameWakeResult::StaleEpoch;
			if (state->outstandingToken != 0) return GameWakeResult::Outstanding;
			if (state->nextToken == 0) {
				state->stopped = true;
				state->enabled = false;
				return GameWakeResult::Stopped;
			}
			ticket = std::make_shared<Ticket>(state, expectedEpoch, state->generation, state->nextToken);
			state->outstandingToken = state->nextToken;
			state->nextToken = state->nextToken == std::numeric_limits<std::uint64_t>::max() ? 0 : state->nextToken + 1;
			state->posting = true;
		}
		bool posted = false;
		try {
			posted = state->post([ticket] {
				// Hold a local owner even if a reentrant fake queue destroys the
				// callable object while its callback is executing.
				const auto active = ticket;
				active->Run();
			});
		} catch (...) { /* Retained task copies remain covered by the latch. */ }
		{
			std::lock_guard lock(state->mutex);
			if (!posted) ticket->cancelled = true;
			state->posting = false;
			if (ticket->completed && state->outstandingToken == ticket->token) state->outstandingToken = 0;
		}
		// If posting did not retain the callback, the local ticket's destruction
		// releases its reservation here. If it did, the stale physical callback
		// must run or be destroyed; a reported failure cannot erase that fact.
		return posted ? GameWakeResult::Posted : GameWakeResult::PostFailed;
	}
}

#pragma once

// Project-owned GPL-3.0 code. Engine-free owner-thread request handoff.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace fusionmcp
{
	using RequestId = std::uint64_t;
	using EpochId = std::uint64_t;
	using RequestClock = std::chrono::steady_clock;
	using RequestTime = RequestClock::time_point;
	enum class Owner { Render, Game, UI, VM };
	enum class RequestState { Queued, Running, Completed, Failed, Cancelled, Interrupted };
	enum class SubmitReason { Accepted, Stopped, QueueFull, StaleEpoch, DeadlineExpired, EmptyWork, IdExhausted, InvalidOwner };
	enum class CancelResult { Cancelled, AlreadyRunning, Terminal, NotFound };

	const char* StateName(RequestState state) noexcept;
	const char* SubmitReasonName(SubmitReason reason) noexcept;
	const char* CancelResultName(CancelResult result) noexcept;

	struct Outcome
	{
		bool ok = true;
		std::string resultJson;
		std::string errorCode;
		std::string errorMessage;
		static Outcome Success(std::string json = "{}");
		static Outcome Failure(std::string code, std::string message);
	};

	struct SubmitResult
	{
		bool accepted = false;
		RequestId id = 0;
		SubmitReason reason = SubmitReason::Stopped;
	};

	struct RequestSnapshot
	{
		RequestId id = 0;
		Owner owner = Owner::Render;
		RequestState state = RequestState::Queued;
		EpochId epoch = 0;
		// Failed callbacks may have performed work before failing. Only requests
		// with started == false can be described as never executed.
		bool started = false;
		bool invalidatedWhileRunning = false;
		std::string resultJson;
		std::string errorCode;
		std::string errorMessage;
	};

	class RequestBroker
	{
	public:
		using Work = std::function<Outcome()>;
		// A clock must be thread-safe, non-reentrant and independent of the
		// broker. It is sampled under the state lock at the execution boundary.
		using NowFn = std::function<RequestTime()>;
		struct Limits
		{
			std::size_t pending = 32;
			std::size_t history = 128;
			std::size_t resultBytes = 262144;
		};

		RequestBroker();
		explicit RequestBroker(NowFn now);
		RequestBroker(NowFn now, Limits limits);
		~RequestBroker();
		RequestBroker(const RequestBroker&) = delete;
		RequestBroker& operator=(const RequestBroker&) = delete;
		RequestBroker(RequestBroker&&) = delete;
		RequestBroker& operator=(RequestBroker&&) = delete;

		// Closures own copied, bounded request arguments. They must not retain
		// engine/COM pointers or references to listener stack storage. The caller
		// supplies a deadline; there is no implicit unbounded timeout.
		SubmitResult Submit(Owner owner, Work work, EpochId expectedEpoch, RequestTime deadline);
		// Reserve capacity before bounded preparation. The queued callback is
		// inactive until Activate succeeds; it still counts against pending32.
		// Work may own a shared, bounded preparation slot. Fill it completely
		// before Activate; activation publishes those writes to the owner pump.
		// Cancellation/expiry/invalidation can retire the callback meanwhile.
		SubmitResult Reserve(Owner owner, Work work, EpochId expectedEpoch, RequestTime deadline);
		// Activate once, only while queued, unexpired and in the current epoch.
		// Refusal does not imply work ran: Query retains the actual request state.
		bool Activate(RequestId id, EpochId expectedEpoch);
		std::optional<RequestSnapshot> Query(RequestId id) const;
		CancelResult Cancel(RequestId id);
		EpochId Epoch() const;
		std::size_t Pending() const;
		std::size_t Pending(Owner owner) const;

		// Call only from the named engine owner thread. Callbacks and destruction
		// of their captures run outside the broker mutex. Concurrent/reentrant
		// pumps for one owner return immediately. At most 32 callbacks run per
		// pump even if callbacks submit more work; there are no completion waits.
		// A nonzero expectedEpoch gates every job start under the state lock.
		// This prevents an old F4SE wake from draining newly submitted world work.
		// Zero preserves the existing caller-owned, ungated render-pump contract.
		std::size_t Pump(Owner owner, std::size_t maxJobs = 4, EpochId expectedEpoch = 0);

		// Invalidation and execution start serialize under the same mutex.
		// Queued work is interrupted and released immediately. Running work is
		// allowed to finish and marked invalidatedWhileRunning, never cancelled.
		EpochId AdvanceEpoch();
		void Stop();

	private:
		SubmitResult Queue(Owner owner, Work work, EpochId expectedEpoch, RequestTime deadline, bool ready);
		struct Impl;
		std::shared_ptr<Impl> impl_;
	};
}

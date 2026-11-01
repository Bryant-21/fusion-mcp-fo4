#include "RequestBroker.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace fusionmcp
{
	const char* StateName(RequestState state) noexcept
	{
		switch (state) {
		case RequestState::Queued: return "queued";
		case RequestState::Running: return "running";
		case RequestState::Completed: return "completed";
		case RequestState::Failed: return "failed";
		case RequestState::Cancelled: return "cancelled";
		case RequestState::Interrupted: return "interrupted";
		}
		return "unknown";
	}

	const char* SubmitReasonName(SubmitReason reason) noexcept
	{
		switch (reason) {
		case SubmitReason::Accepted: return "accepted";
		case SubmitReason::Stopped: return "stopped";
		case SubmitReason::QueueFull: return "queue_full";
		case SubmitReason::StaleEpoch: return "stale_epoch";
		case SubmitReason::DeadlineExpired: return "deadline_expired";
		case SubmitReason::EmptyWork: return "empty_work";
		case SubmitReason::IdExhausted: return "id_exhausted";
		case SubmitReason::InvalidOwner: return "invalid_owner";
		}
		return "unknown";
	}

	const char* CancelResultName(CancelResult result) noexcept
	{
		switch (result) {
		case CancelResult::Cancelled: return "cancelled";
		case CancelResult::AlreadyRunning: return "already_running";
		case CancelResult::Terminal: return "terminal";
		case CancelResult::NotFound: return "not_found";
		}
		return "unknown";
	}

	Outcome Outcome::Success(std::string json)
	{
		return { true, std::move(json), {}, {} };
	}

	Outcome Outcome::Failure(std::string code, std::string message)
	{
		return { false, {}, std::move(code), std::move(message) };
	}

	struct RequestBroker::Impl
	{
		static constexpr std::size_t kMaxPending = 32;
		static constexpr std::size_t kMaxHistory = 128;
		static constexpr std::size_t kMaxResultBytes = 262144;
		struct Record
		{
			RequestSnapshot snapshot;
			RequestTime deadline;
			Work work;
			bool ready = true;
			// Internal failures use static text so stop/destruction and exception
			// completion need no allocations while changing request state.
			const char* internalCode = nullptr;
			const char* internalMessage = nullptr;
		};
		struct RetiredWork
		{
			std::array<Work, kMaxPending> work;
			std::size_t count = 0;
			void Retire(Work& source) noexcept { work[count++] = std::move(source); }
		};

		NowFn now;
		Limits limits;
		mutable std::mutex mutex;
		std::deque<std::shared_ptr<Record>> records;
		RequestId nextId = 1;
		EpochId epoch = 1;
		std::size_t pending = 0;
		std::array<bool, 4> pumping{};
		bool stopped = false;

		static bool ValidOwner(Owner owner) noexcept { return owner >= Owner::Render && owner <= Owner::VM; }
		static std::size_t OwnerIndex(Owner owner) noexcept { return static_cast<std::size_t>(owner); }
		static bool Terminal(RequestState state) noexcept
		{
			return state != RequestState::Queued && state != RequestState::Running;
		}
		void Interrupt(Record& record, RetiredWork& retired, const char* code, const char* message) noexcept
		{
			record.snapshot.state = RequestState::Interrupted;
			record.internalCode = code;
			record.internalMessage = message;
			retired.Retire(record.work);
			--pending;
		}
		void Expire(RequestTime time, RetiredWork& retired) noexcept
		{
			for (const auto& record : records) {
				if (record->snapshot.state == RequestState::Queued && time >= record->deadline) {
					Interrupt(*record, retired, "deadline_expired", "The request expired before execution.");
				}
			}
		}
		void Invalidate(RetiredWork& retired, const char* code, const char* message) noexcept
		{
			for (const auto& record : records) {
				if (record->snapshot.state == RequestState::Queued) {
					Interrupt(*record, retired, code, message);
				} else if (record->snapshot.state == RequestState::Running) {
					record->snapshot.invalidatedWhileRunning = true;
				}
			}
		}
	};

	RequestBroker::RequestBroker() : RequestBroker([] { return RequestClock::now(); }, Limits{}) {}
	RequestBroker::RequestBroker(NowFn now) : RequestBroker(std::move(now), Limits{}) {}
	RequestBroker::RequestBroker(NowFn now, Limits limits) : impl_(std::make_shared<Impl>())
	{
		if (!now || limits.pending == 0 || limits.pending > Impl::kMaxPending ||
			limits.history < limits.pending || limits.history > Impl::kMaxHistory ||
			limits.resultBytes == 0 || limits.resultBytes > Impl::kMaxResultBytes) {
			throw std::invalid_argument("Invalid remote request broker clock or limits.");
		}
		impl_->now = std::move(now);
		impl_->limits = limits;
	}
	RequestBroker::~RequestBroker() { Stop(); }

	SubmitResult RequestBroker::Submit(Owner owner, Work work, EpochId expectedEpoch, RequestTime deadline)
	{
		return Queue(owner, std::move(work), expectedEpoch, deadline, true);
	}
	SubmitResult RequestBroker::Reserve(Owner owner, Work work, EpochId expectedEpoch, RequestTime deadline)
	{
		return Queue(owner, std::move(work), expectedEpoch, deadline, false);
	}
	SubmitResult RequestBroker::Queue(Owner owner, Work work, EpochId expectedEpoch, RequestTime deadline, bool ready)
	{
		const auto impl = impl_;
		if (!Impl::ValidOwner(owner)) return { false, 0, SubmitReason::InvalidOwner };
		if (!work) return { false, 0, SubmitReason::EmptyWork };
		Impl::RetiredWork retired;
		std::lock_guard lock(impl->mutex);
		const auto time = impl->now();
		impl->Expire(time, retired);
		if (impl->stopped) return { false, 0, SubmitReason::Stopped };
		if (expectedEpoch != impl->epoch) return { false, 0, SubmitReason::StaleEpoch };
		if (time >= deadline) return { false, 0, SubmitReason::DeadlineExpired };
		if (impl->pending >= impl->limits.pending) return { false, 0, SubmitReason::QueueFull };
		if (impl->nextId == 0) return { false, 0, SubmitReason::IdExhausted };

		auto record = std::make_shared<Impl::Record>();
		record->snapshot.id = impl->nextId;
		record->snapshot.owner = owner;
		record->snapshot.epoch = expectedEpoch;
		record->deadline = deadline;
		record->ready = ready;
		// Allocate the retained slot before transferring the callback. If an
		// allocation fails, the incoming work is still destroyed after unlock.
		impl->records.push_back(record);
		record->work = std::move(work);
		++impl->pending;
		impl->nextId = impl->nextId == std::numeric_limits<RequestId>::max() ? 0 : impl->nextId + 1;
		if (impl->records.size() > impl->limits.history) {
			const auto oldestTerminal = std::find_if(impl->records.begin(), impl->records.end(),
				[](const auto& item) { return Impl::Terminal(item->snapshot.state); });
			// history >= pending: at least one terminal record must be present.
			impl->records.erase(oldestTerminal);
		}
		return { true, record->snapshot.id, SubmitReason::Accepted };
	}

	bool RequestBroker::Activate(RequestId id, EpochId expectedEpoch)
	{
		const auto impl = impl_;
		Impl::RetiredWork retired;
		std::lock_guard lock(impl->mutex);
		impl->Expire(impl->now(), retired);
		if (impl->stopped || expectedEpoch != impl->epoch) return false;
		const auto found = std::find_if(impl->records.begin(), impl->records.end(),
			[id](const auto& record) { return record->snapshot.id == id; });
		if (found == impl->records.end()) return false;
		auto& record = **found;
		if (record.snapshot.state != RequestState::Queued || record.ready || record.snapshot.epoch != expectedEpoch) return false;
		record.ready = true;
		return true;
	}

	std::optional<RequestSnapshot> RequestBroker::Query(RequestId id) const
	{
		const auto impl = impl_;
		Impl::RetiredWork retired;
		std::optional<RequestSnapshot> snapshot;
		const char* code = nullptr;
		const char* message = nullptr;
		{
			std::lock_guard lock(impl->mutex);
			const auto time = impl->now();
			impl->Expire(time, retired);
			const auto found = std::find_if(impl->records.begin(), impl->records.end(),
				[id](const auto& record) { return record->snapshot.id == id; });
			if (found != impl->records.end()) {
				snapshot = (*found)->snapshot;
				code = (*found)->internalCode;
				message = (*found)->internalMessage;
			}
		}
		if (snapshot && code) {
			snapshot->errorCode = code;
			snapshot->errorMessage = message;
		}
		return snapshot;
	}

	CancelResult RequestBroker::Cancel(RequestId id)
	{
		const auto impl = impl_;
		Work retired;
		std::lock_guard lock(impl->mutex);
		const auto found = std::find_if(impl->records.begin(), impl->records.end(),
			[id](const auto& record) { return record->snapshot.id == id; });
		if (found == impl->records.end()) return CancelResult::NotFound;
		auto& record = **found;
		if (record.snapshot.state == RequestState::Running) return CancelResult::AlreadyRunning;
		if (record.snapshot.state != RequestState::Queued) return CancelResult::Terminal;
		record.snapshot.state = RequestState::Cancelled;
		record.internalCode = "cancelled";
		record.internalMessage = "The request was cancelled before execution.";
		retired = std::move(record.work);
		--impl->pending;
		return CancelResult::Cancelled;
	}

	EpochId RequestBroker::Epoch() const
	{
		const auto impl = impl_;
		std::lock_guard lock(impl->mutex);
		return impl->epoch;
	}
	std::size_t RequestBroker::Pending() const
	{
		const auto impl = impl_;
		std::lock_guard lock(impl->mutex);
		return impl->pending;
	}
	std::size_t RequestBroker::Pending(Owner owner) const
	{
		const auto impl = impl_;
		if (!Impl::ValidOwner(owner)) return 0;
		std::lock_guard lock(impl->mutex);
		return static_cast<std::size_t>(std::count_if(impl->records.begin(), impl->records.end(), [owner](const auto& record) {
			return record->snapshot.owner == owner && !Impl::Terminal(record->snapshot.state);
		}));
	}

	std::size_t RequestBroker::Pump(Owner owner, std::size_t maxJobs, EpochId expectedEpoch)
	{
		const auto impl = impl_;
		if (!Impl::ValidOwner(owner) || maxJobs == 0) return 0;
		const auto ownerIndex = Impl::OwnerIndex(owner);
		{
			std::lock_guard lock(impl->mutex);
			if (impl->stopped || impl->pumping[ownerIndex] || (expectedEpoch != 0 && expectedEpoch != impl->epoch)) return 0;
			impl->pumping[ownerIndex] = true;
		}
		struct PumpGuard
		{
			std::shared_ptr<Impl> impl;
			std::size_t ownerIndex;
			~PumpGuard()
			{
				std::lock_guard lock(impl->mutex);
				impl->pumping[ownerIndex] = false;
			}
		} guard{ impl, ownerIndex };
		maxJobs = std::min(maxJobs, Impl::kMaxPending);
		std::size_t executed = 0;
		while (executed < maxJobs) {
			std::shared_ptr<Impl::Record> record;
			Work work;
			{
				Impl::RetiredWork retired;
				std::lock_guard lock(impl->mutex);
				const auto time = impl->now();
				impl->Expire(time, retired);
				if (impl->stopped || (expectedEpoch != 0 && expectedEpoch != impl->epoch)) break;
				const auto found = std::find_if(impl->records.begin(), impl->records.end(), [owner](const auto& item) {
					return item->snapshot.owner == owner && item->snapshot.state == RequestState::Queued && item->ready;
				});
				if (found == impl->records.end()) break;
				record = *found;
				record->snapshot.state = RequestState::Running;
				record->snapshot.started = true;
				work = std::move(record->work);
			}

			Outcome outcome;
			bool callbackThrew = false;
			try {
				outcome = work();
			} catch (...) {
				callbackThrew = true;
			}
			const auto oversized = outcome.resultJson.size() > impl->limits.resultBytes;
			if (outcome.errorCode.size() > 64) outcome.errorCode.resize(64);
			if (outcome.errorMessage.size() > 1024) outcome.errorMessage.resize(1024);
			{
				std::lock_guard lock(impl->mutex);
				if (callbackThrew || oversized) {
					record->snapshot.state = RequestState::Failed;
					record->internalCode = callbackThrew ? "callback_exception" : "result_too_large";
					record->internalMessage = callbackThrew ? "The callback failed after execution started." : "The callback result exceeds the byte limit.";
				} else if (outcome.ok) {
					record->snapshot.state = RequestState::Completed;
					record->snapshot.resultJson = std::move(outcome.resultJson);
				} else {
					record->snapshot.state = RequestState::Failed;
					record->snapshot.errorCode = std::move(outcome.errorCode);
					record->snapshot.errorMessage = std::move(outcome.errorMessage);
				}
				--impl->pending;
			}
			++executed;
		}
		return executed;
	}

	EpochId RequestBroker::AdvanceEpoch()
	{
		const auto impl = impl_;
		Impl::RetiredWork retired;
		std::lock_guard lock(impl->mutex);
		if (impl->epoch == std::numeric_limits<EpochId>::max()) {
			impl->stopped = true;
		} else if (!impl->stopped) {
			++impl->epoch;
		}
		impl->Invalidate(retired, "epoch_changed", "The owner lifecycle changed before execution.");
		return impl->epoch;
	}
	void RequestBroker::Stop()
	{
		const auto impl = impl_;
		Impl::RetiredWork retired;
		std::lock_guard lock(impl->mutex);
		impl->stopped = true;
		impl->Invalidate(retired, "stopped", "The request broker stopped before execution.");
	}
}

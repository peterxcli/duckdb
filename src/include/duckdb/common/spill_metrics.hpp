//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/spill_metrics.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/memory_tag.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace duckdb {

struct SpillMetricEntry {
	const char *name;
	idx_t value;
};

struct SpillEventEntry {
	idx_t sequence;
	const char *event_type;
	idx_t time_ns;
	idx_t thread_id;
	idx_t operator_id;
	idx_t pipeline_id;
	idx_t memory_tag_id;
	idx_t radix_bits;
	idx_t row_count;
	idx_t byte_count;
};

class SpillMetrics {
public:
	static void Increment(std::atomic<idx_t> &counter, idx_t delta = 1) {
		counter.fetch_add(delta, std::memory_order_relaxed);
	}

	static void Max(std::atomic<idx_t> &counter, idx_t value) {
		auto current = counter.load(std::memory_order_relaxed);
		while (current < value && !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
		}
	}

	static void OnAggregateAbandon(idx_t radix_bits) {
		Increment(AggregateAbandonEvents());
		Max(AggregateMaxRadixBits(), radix_bits);
		LogEvent("aggregate_abandon", 0, 0, 0, radix_bits, 0, 0);
	}

	static void OnAggregateExternalTransition(idx_t radix_bits) {
		Increment(AggregateExternalTransitions());
		Max(AggregateMaxRadixBits(), radix_bits);
		LogEvent("aggregate_external_transition", 0, 0, 0, radix_bits, 0, 0);
	}

	static void OnAggregateRepartition(idx_t old_radix_bits, idx_t new_radix_bits) {
		Increment(AggregateRepartitionEvents());
		Max(AggregateMaxRadixBits(), MaxValue(old_radix_bits, new_radix_bits));
		LogEvent("aggregate_repartition", 0, 0, 0, MaxValue(old_radix_bits, new_radix_bits), 0, 0);
	}

	static void OnHashJoinRepartition(idx_t radix_bits, idx_t task_count, idx_t operator_id = 0, idx_t pipeline_id = 0) {
		Increment(HashJoinRepartitionEvents());
		Increment(HashJoinRepartitionTasks(), task_count);
		Max(HashJoinMaxRadixBits(), radix_bits);
		LogEvent("hash_join_repartition", operator_id, pipeline_id, 0, radix_bits, task_count, 0);
	}

	static void OnHashJoinRepartitionDecision(bool skipped_for_skew, bool repartition_required, idx_t radix_bits,
	                                         idx_t reservation, idx_t total_size, idx_t max_partition_ht_size,
	                                         idx_t probe_side_requirement) {
		Increment(HashJoinRepartitionDecisions());
		if (skipped_for_skew) {
			Increment(HashJoinRepartitionSkewSkips());
		}
		if (repartition_required) {
			Increment(HashJoinRepartitionRequired());
		}
		Max(HashJoinMaxRadixBits(), radix_bits);
		Max(HashJoinRepartitionMaxReservation(), reservation);
		Max(HashJoinRepartitionMaxTotalSize(), total_size);
		Max(HashJoinRepartitionMaxPartitionHTSize(), max_partition_ht_size);
		Max(HashJoinRepartitionMaxProbeRequirement(), probe_side_requirement);
		LogEvent(repartition_required ? "hash_join_repartition_required" : "hash_join_repartition_not_required", 0, 0,
		         0, radix_bits, max_partition_ht_size, reservation);
	}

	static void OnHashJoinExternalBuildRound(idx_t radix_bits) {
		Increment(HashJoinExternalBuildRounds());
		Max(HashJoinMaxRadixBits(), radix_bits);
		LogEvent("hash_join_external_build_round", 0, 0, 0, radix_bits, 0, 0);
	}

	static void OnHashJoinProbeSpillInit(idx_t radix_bits) {
		Increment(HashJoinProbeSpillInitializations());
		Max(HashJoinMaxRadixBits(), radix_bits);
		LogEvent("hash_join_probe_spill_init", 0, 0, 0, radix_bits, 0, 0);
	}

	static void OnHashJoinProbeSpillRound() {
		Increment(HashJoinProbeSpillRounds());
		LogEvent("hash_join_probe_spill_round", 0, 0, 0, 0, 0, 0);
	}

	static void OnHashJoinProbeSpillAppend(idx_t row_count) {
		Increment(HashJoinProbeSpillChunks());
		Increment(HashJoinProbeSpillRows(), row_count);
		LogEvent("hash_join_probe_spill_append", 0, 0, 0, 0, row_count, 0);
	}

	static void OnTupleRepartition(idx_t row_count, idx_t byte_count) {
		Increment(TupleRepartitionEvents());
		Increment(TupleRepartitionRows(), row_count);
		Increment(TupleRepartitionBytes(), byte_count);
		LogEvent("tuple_repartition", 0, 0, 0, 0, row_count, byte_count);
	}

	static void OnTemporaryBufferWrite(MemoryTag tag, idx_t byte_count) {
		Increment(TempBufferWriteEvents());
		Increment(TempBufferWriteBytes(), byte_count);
		if (IsPersistentTag(tag)) {
			Increment(PersistentBufferWriteEvents());
			Increment(PersistentBufferWriteBytes(), byte_count);
		} else if (IsTemporaryIntermediateTag(tag)) {
			Increment(TemporaryIntermediateWriteEvents());
			Increment(TemporaryIntermediateWriteBytes(), byte_count);
		} else {
			Increment(OtherBufferWriteEvents());
			Increment(OtherBufferWriteBytes(), byte_count);
		}
		LogEvent("buffer_write", 0, 0, static_cast<idx_t>(tag), 0, 0, byte_count);
	}

	static void OnTemporaryBufferRead(MemoryTag tag, idx_t byte_count) {
		Increment(TempBufferReadEvents());
		Increment(TempBufferReadBytes(), byte_count);
		if (IsPersistentTag(tag)) {
			Increment(PersistentBufferReadEvents());
			Increment(PersistentBufferReadBytes(), byte_count);
		} else if (IsTemporaryIntermediateTag(tag)) {
			Increment(TemporaryIntermediateReadEvents());
			Increment(TemporaryIntermediateReadBytes(), byte_count);
		} else {
			Increment(OtherBufferReadEvents());
			Increment(OtherBufferReadBytes(), byte_count);
		}
		LogEvent("buffer_read", 0, 0, static_cast<idx_t>(tag), 0, 0, byte_count);
	}

	static vector<SpillMetricEntry> GetEntries() {
		return {
		    {"aggregate_abandon_events", AggregateAbandonEvents().load(std::memory_order_relaxed)},
		    {"aggregate_external_transitions", AggregateExternalTransitions().load(std::memory_order_relaxed)},
		    {"aggregate_repartition_events", AggregateRepartitionEvents().load(std::memory_order_relaxed)},
		    {"aggregate_max_radix_bits", AggregateMaxRadixBits().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_events", HashJoinRepartitionEvents().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_tasks", HashJoinRepartitionTasks().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_decisions", HashJoinRepartitionDecisions().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_required", HashJoinRepartitionRequired().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_skew_skips", HashJoinRepartitionSkewSkips().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_max_reservation_bytes",
		     HashJoinRepartitionMaxReservation().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_max_total_size_bytes",
		     HashJoinRepartitionMaxTotalSize().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_max_partition_ht_bytes",
		     HashJoinRepartitionMaxPartitionHTSize().load(std::memory_order_relaxed)},
		    {"hash_join_repartition_max_probe_requirement_bytes",
		     HashJoinRepartitionMaxProbeRequirement().load(std::memory_order_relaxed)},
		    {"hash_join_max_radix_bits", HashJoinMaxRadixBits().load(std::memory_order_relaxed)},
		    {"hash_join_external_build_rounds", HashJoinExternalBuildRounds().load(std::memory_order_relaxed)},
		    {"hash_join_probe_spill_initializations",
		     HashJoinProbeSpillInitializations().load(std::memory_order_relaxed)},
		    {"hash_join_probe_spill_rounds", HashJoinProbeSpillRounds().load(std::memory_order_relaxed)},
		    {"hash_join_probe_spill_chunks", HashJoinProbeSpillChunks().load(std::memory_order_relaxed)},
		    {"hash_join_probe_spill_rows", HashJoinProbeSpillRows().load(std::memory_order_relaxed)},
		    {"tuple_repartition_events", TupleRepartitionEvents().load(std::memory_order_relaxed)},
		    {"tuple_repartition_rows", TupleRepartitionRows().load(std::memory_order_relaxed)},
		    {"tuple_repartition_bytes", TupleRepartitionBytes().load(std::memory_order_relaxed)},
		    {"temp_buffer_write_events", TempBufferWriteEvents().load(std::memory_order_relaxed)},
		    {"temp_buffer_write_bytes", TempBufferWriteBytes().load(std::memory_order_relaxed)},
		    {"temp_buffer_read_events", TempBufferReadEvents().load(std::memory_order_relaxed)},
		    {"temp_buffer_read_bytes", TempBufferReadBytes().load(std::memory_order_relaxed)},
		    {"persistent_buffer_write_events", PersistentBufferWriteEvents().load(std::memory_order_relaxed)},
		    {"persistent_buffer_write_bytes", PersistentBufferWriteBytes().load(std::memory_order_relaxed)},
		    {"persistent_buffer_read_events", PersistentBufferReadEvents().load(std::memory_order_relaxed)},
		    {"persistent_buffer_read_bytes", PersistentBufferReadBytes().load(std::memory_order_relaxed)},
		    {"temporary_intermediate_write_events",
		     TemporaryIntermediateWriteEvents().load(std::memory_order_relaxed)},
		    {"temporary_intermediate_write_bytes",
		     TemporaryIntermediateWriteBytes().load(std::memory_order_relaxed)},
		    {"temporary_intermediate_read_events", TemporaryIntermediateReadEvents().load(std::memory_order_relaxed)},
		    {"temporary_intermediate_read_bytes", TemporaryIntermediateReadBytes().load(std::memory_order_relaxed)},
		    {"other_buffer_write_events", OtherBufferWriteEvents().load(std::memory_order_relaxed)},
		    {"other_buffer_write_bytes", OtherBufferWriteBytes().load(std::memory_order_relaxed)},
		    {"other_buffer_read_events", OtherBufferReadEvents().load(std::memory_order_relaxed)},
		    {"other_buffer_read_bytes", OtherBufferReadBytes().load(std::memory_order_relaxed)},
		    {"spill_event_count", SpillEventSequence().load(std::memory_order_relaxed)},
		};
	}

	static vector<SpillEventEntry> GetEvents() {
		lock_guard<mutex> guard(SpillEventLock());
		return SpillEvents();
	}

private:
	static bool IsPersistentTag(MemoryTag tag) {
		return tag == MemoryTag::BASE_TABLE || tag == MemoryTag::METADATA || tag == MemoryTag::OVERFLOW_STRINGS ||
		       tag == MemoryTag::ART_INDEX;
	}

	static bool IsTemporaryIntermediateTag(MemoryTag tag) {
		return tag == MemoryTag::HASH_TABLE || tag == MemoryTag::ORDER_BY || tag == MemoryTag::COLUMN_DATA ||
		       tag == MemoryTag::WINDOW || tag == MemoryTag::IN_MEMORY_TABLE;
	}

	static void LogEvent(const char *event_type, idx_t operator_id, idx_t pipeline_id, idx_t memory_tag_id,
	                     idx_t radix_bits, idx_t row_count, idx_t byte_count) {
		auto sequence = SpillEventSequence().fetch_add(1, std::memory_order_relaxed) + 1;
		auto now = std::chrono::steady_clock::now().time_since_epoch();
		auto time_ns = static_cast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
		auto thread_id = static_cast<idx_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));

		lock_guard<mutex> guard(SpillEventLock());
		SpillEvents().push_back({sequence, event_type, time_ns, thread_id, operator_id, pipeline_id, memory_tag_id,
		                         radix_bits, row_count, byte_count});
	}

	static std::atomic<idx_t> &AggregateAbandonEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &AggregateExternalTransitions() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &AggregateRepartitionEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &AggregateMaxRadixBits() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionTasks() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionDecisions() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionRequired() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionSkewSkips() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionMaxReservation() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionMaxTotalSize() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionMaxPartitionHTSize() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinRepartitionMaxProbeRequirement() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinMaxRadixBits() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinExternalBuildRounds() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinProbeSpillInitializations() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinProbeSpillRounds() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinProbeSpillChunks() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &HashJoinProbeSpillRows() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TupleRepartitionEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TupleRepartitionRows() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TupleRepartitionBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TempBufferWriteEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TempBufferWriteBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TempBufferReadEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TempBufferReadBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &PersistentBufferWriteEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &PersistentBufferWriteBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &PersistentBufferReadEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &PersistentBufferReadBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TemporaryIntermediateWriteEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TemporaryIntermediateWriteBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TemporaryIntermediateReadEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &TemporaryIntermediateReadBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &OtherBufferWriteEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &OtherBufferWriteBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &OtherBufferReadEvents() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &OtherBufferReadBytes() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static std::atomic<idx_t> &SpillEventSequence() {
		static std::atomic<idx_t> value {0};
		return value;
	}
	static mutex &SpillEventLock() {
		static mutex value;
		return value;
	}
	static vector<SpillEventEntry> &SpillEvents() {
		static vector<SpillEventEntry> value;
		return value;
	}
};

} // namespace duckdb

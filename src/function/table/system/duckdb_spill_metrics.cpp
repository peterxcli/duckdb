#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/common/spill_metrics.hpp"

namespace duckdb {

struct DuckDBSpillMetricsData : public GlobalTableFunctionState {
	vector<SpillMetricEntry> entries;
	idx_t offset = 0;
};

struct DuckDBSpillEventsData : public GlobalTableFunctionState {
	vector<SpillEventEntry> entries;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBSpillMetricsBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("metric");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("value");
	return_types.emplace_back(LogicalType::BIGINT);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBSpillMetricsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSpillMetricsData>();
	result->entries = SpillMetrics::GetEntries();
	return std::move(result);
}

void DuckDBSpillMetricsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSpillMetricsData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		output.SetValue(0, count, entry.name);
		output.SetValue(1, count, Value::BIGINT(NumericCast<int64_t>(entry.value)));
		count++;
	}
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> DuckDBSpillEventsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("sequence");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("event_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("time_ns");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("thread_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("operator_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("pipeline_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("memory_tag_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("radix_bits");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("row_count");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("byte_count");
	return_types.emplace_back(LogicalType::UBIGINT);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBSpillEventsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSpillEventsData>();
	result->entries = SpillMetrics::GetEvents();
	return std::move(result);
}

void DuckDBSpillEventsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSpillEventsData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		output.SetValue(0, count, Value::UBIGINT(entry.sequence));
		output.SetValue(1, count, entry.event_type);
		output.SetValue(2, count, Value::UBIGINT(entry.time_ns));
		output.SetValue(3, count, Value::UBIGINT(entry.thread_id));
		output.SetValue(4, count, Value::UBIGINT(entry.operator_id));
		output.SetValue(5, count, Value::UBIGINT(entry.pipeline_id));
		output.SetValue(6, count, Value::UBIGINT(entry.memory_tag_id));
		output.SetValue(7, count, Value::UBIGINT(entry.radix_bits));
		output.SetValue(8, count, Value::UBIGINT(entry.row_count));
		output.SetValue(9, count, Value::UBIGINT(entry.byte_count));
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBSpillMetricsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_spill_metrics", {}, DuckDBSpillMetricsFunction, DuckDBSpillMetricsBind,
	                              DuckDBSpillMetricsInit));
	set.AddFunction(TableFunction("duckdb_spill_events", {}, DuckDBSpillEventsFunction, DuckDBSpillEventsBind,
	                              DuckDBSpillEventsInit));
}

} // namespace duckdb

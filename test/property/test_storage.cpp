// Storage round trip: data written to a persistent database (with forced compression schemes)
// must read back identically after checkpoint + reopen, also after updates and deletes.
#include "property_test.hpp"

#include "duckdb/main/appender.hpp"

#include <cstdio>
#include <unistd.h>

using namespace duckdb_prop;

namespace {

string ScratchDir() {
	const char *dir = getenv("PROPERTY_TEST_TMP");
	if (dir) {
		return dir;
	}
	return "/tmp";
}

struct ScopedDBFile {
	string path;
	explicit ScopedDBFile(const string &name) {
		static int counter = 0;
		path = ScratchDir() + "/" + name + "_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ".db";
		Cleanup();
	}
	~ScopedDBFile() {
		Cleanup();
	}
	void Cleanup() {
		std::remove(path.c_str());
		std::remove((path + ".wal").c_str());
	}
};

// chimp/patas are deprecated and rejected by force_compression
const vector<string> compressions = {"uncompressed", "rle",   "bitpacking", "dictionary", "fsst",
                                     "alp",          "alprd", "zstd",       "roaring",    "dict_fsst"};

} // namespace

TEST_CASE("Persistent storage round trip", "[property][storage]") {
	rc::prop("write, checkpoint, reopen, read back", [&] {
		auto type = *GenType(1);
		auto compression = *rc::gen::elementOf(compressions);
		auto values = *rc::gen::scale(8.0, GenValues(type, 0.15));
		auto do_delete = *rc::gen::arbitrary<bool>();
		auto do_update = *rc::gen::arbitrary<bool>();
		RC_TAG(compression);

		ScopedDBFile file("storage_rt");
		{
			PropDB db(file.path);
			auto set_res = db.Query("SET force_compression='" + compression + "'");
			PROP_REQUIRE_NO_ERROR(set_res, "set force_compression");
			db.Exec("CREATE TABLE t(id INTEGER, v " + type.ToString() + ")");
			{
				Appender appender(db.con, "t");
				for (idx_t i = 0; i < values.size(); i++) {
					appender.BeginRow();
					appender.Append<int32_t>(int32_t(i));
					appender.Append<Value>(values[i]);
					appender.EndRow();
				}
				appender.Close();
			}
			db.Exec("CHECKPOINT");
		}
		// modify + verify against the in-memory model
		auto model = values;
		{
			PropDB db(file.path);
			if (do_delete && !model.empty()) {
				// delete every third row
				db.Exec("DELETE FROM t WHERE id % 3 = 1");
				vector<Value> kept;
				for (idx_t i = 0; i < model.size(); i++) {
					if (i % 3 != 1) {
						kept.push_back(model[i]);
					}
				}
				// model keyed by original ids: rebuild as pairs below instead
				model = kept;
			}
			if (do_update && !values.empty()) {
				// overwrite v with the first generated value for every second remaining row
				auto res = db.Query("UPDATE t SET v = $1 WHERE id % 2 = 0", {values[0]});
				PROP_REQUIRE_NO_ERROR(res, "update");
			}
			db.Exec("CHECKPOINT");
		}
		{
			PropDB db(file.path);
			auto res = db.Query("SELECT id, v FROM t ORDER BY id");
			PROP_REQUIRE_NO_ERROR(res, "select after reopen");
			// rebuild expected from the original values + the applied modifications
			vector<std::pair<int32_t, Value>> expected;
			for (idx_t i = 0; i < values.size(); i++) {
				if (do_delete && i % 3 == 1) {
					continue;
				}
				auto v = (do_update && i % 2 == 0) ? values[0] : values[i];
				expected.emplace_back(int32_t(i), v);
			}
			if (res->RowCount() != expected.size()) {
				RC_FAIL("row count mismatch after reopen: got " + std::to_string(res->RowCount()) + " expected " +
				        std::to_string(expected.size()));
			}
			for (idx_t i = 0; i < expected.size(); i++) {
				auto id = res->GetValue(0, i);
				if (id.GetValue<int32_t>() != expected[i].first) {
					RC_FAIL("id mismatch at row " + std::to_string(i));
				}
				auto v = res->GetValue(1, i);
				if (!ValuesEqual(v, expected[i].second)) {
					RC_FAIL("value mismatch at id " + std::to_string(expected[i].first) + " (compression " +
					        compression + ")\n  stored:   " + Describe(v) +
					        "\n  expected: " + Describe(expected[i].second));
				}
			}
		}
	});
}

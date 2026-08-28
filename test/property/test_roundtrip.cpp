// Round-trip properties: a value must survive conversions to text/SQL/JSON and back unchanged.
#include "property_test.hpp"

using namespace duckdb_prop;

namespace {

//! Tag the test case with the top-level type id so the distribution is visible in the output
void TagType(const LogicalType &type) {
	RC_TAG(LogicalTypeIdToString(type.id()));
}

//! Known text-format round-trip bugs (see FINDINGS.md): filtered so they do not mask other failures
bool HasKnownTextIssue(const Value &v) {
	return ValueContains(v, [](const Value &x) {
		if (x.IsNull()) {
			return false;
		}
		switch (x.type().id()) {
		case LogicalTypeId::TIME_TZ: {
			// KNOWN ISSUE: offsets with 0 minutes but nonzero seconds format incorrectly (-05:00:59 -> -05:59)
			auto offset = std::abs(x.GetValue<dtime_tz_t>().offset());
			return offset % 3600 != 0 && (offset % 3600) / 60 == 0;
		}
		case LogicalTypeId::INTERVAL: {
			// KNOWN ISSUE: Interval::ToString prints 10-digit hours but the parser only accepts 9 digits
			auto micros = x.GetValue<interval_t>().micros;
			const int64_t limit = 1000000000LL * Interval::MICROS_PER_HOUR;
			return micros >= limit || micros <= -limit;
		}
		case LogicalTypeId::UHUGEINT:
			// LIMITATION: UHUGEINT literals above HUGEINT range promote sibling list entries to DOUBLE
			return x.GetValue<uhugeint_t>() > uhugeint_t(0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
		default:
			return false;
		}
	});
}

//! Dump raw bits of temporal leaf values (some print identically but differ in raw representation)
string DumpBits(const Value &v) {
	string out;
	ValueContains(v, [&](const Value &x) {
		if (x.IsNull()) {
			return false;
		}
		switch (x.type().id()) {
		case LogicalTypeId::TIME_TZ:
			out += " timetz_bits=" + std::to_string(x.GetValue<dtime_tz_t>().bits);
			break;
		default:
			break;
		}
		return false;
	});
	return out;
}

} // namespace

TEST_CASE("VARCHAR cast round trip", "[property][roundtrip]") {
	PropDB db;
	rc::prop("CAST(CAST(v AS VARCHAR) AS T) IS NOT DISTINCT FROM v", [&] {
		// VARCHAR -> UNION only tries members that VARCHAR implicitly casts to, so unions do not round trip by design
		auto type = *rc::gen::suchThat(GenType(2), [](const LogicalType &t) {
			return !TypeContainsAny(t, {LogicalTypeId::UNION}) && TypeHasSafeEnumValues(t) && TypeHasSafeFieldNames(t);
		});
		// the nested text format does not quote strings, so only safe strings round trip inside nested types
		GenOptions options(0.05);
		options.safe_strings = type.IsNested();
		auto v = *GenValue(type, options);
		TagType(type);
		RC_PRE(!HasKnownTextIssue(v));
		auto res = db.Query("SELECT CAST(CAST($1 AS VARCHAR) AS " + type.ToString() + "), CAST($1 AS VARCHAR)", {v});
		PROP_REQUIRE_NO_ERROR(res, "type " + type.ToString() + " value " + Describe(v));
		auto &mat = res->Cast<MaterializedQueryResult>();
		auto back = mat.GetValue(0, 0);
		auto str = mat.GetValue(1, 0);
		if (!ValuesEqual(back, v)) {
			RC_FAIL("Round trip through VARCHAR failed\n  original: " + Describe(v) + DumpBits(v) +
			        "\n  as text:  " + str.ToString() + "\n  back:     " + Describe(back) + DumpBits(back));
		}
	});
}

TEST_CASE("ToSQLString round trip", "[property][roundtrip]") {
	PropDB db;
	rc::prop("SELECT <v.ToSQLString()> returns v with the same type", [&] {
		// KNOWN ISSUES: ToSQLString does not escape quotes/backslashes in STRUCT keys, and emits BIT unquoted
		auto type = *rc::gen::suchThat(GenType(2), [](const LogicalType &t) {
			if (TypeContainsAny(t, {LogicalTypeId::BIT, LogicalTypeId::UNION})) {
				return false;
			}
			return !TypeContains(t, [](const LogicalType &c) {
				if (c.id() != LogicalTypeId::STRUCT) {
					return false;
				}
				for (auto &child : StructType::GetChildTypes(c)) {
					auto &name = child.first.GetIdentifierName();
					if (name.find('\'') != string::npos || name.find('\\') != string::npos) {
						return true;
					}
				}
				return false;
			});
		});
		auto v = *GenValue(type, 0.05);
		TagType(type);
		RC_PRE(!HasKnownTextIssue(v));
		auto sql = v.ToSQLString();
		auto res = db.Query("SELECT " + sql);
		PROP_REQUIRE_NO_ERROR(res, "SELECT " + sql);
		if (res->RowCount() != 1 || res->ColumnCount() != 1) {
			RC_FAIL("unexpected result shape for SELECT " + sql);
		}
		auto back = res->GetValue(0, 0);
		// literals may bind to a different (wider) type, e.g. SMALLINT 0 -> INTEGER 0: compare loosely.
		// nested literals can even change type shape (ARRAY -> LIST), which NotDistinctFrom cannot bridge,
		// and FLOAT text reparsed as DOUBLE differs from the widened FLOAT, so the loose check is scalars-only
		if (!type.IsNested() && type.id() != LogicalTypeId::FLOAT && !ValuesEqualLoose(back, v)) {
			RC_FAIL("ToSQLString round trip failed\n  original: " + Describe(v) + "\n  sql:      " + sql +
			        "\n  back:     " + Describe(back));
		}
		// with an explicit cast the type must match exactly
		auto typed = db.Scalar("SELECT CAST(" + sql + " AS " + type.ToString() + ")");
		if (!ValuesEqual(typed, v)) {
			RC_FAIL("ToSQLString round trip with cast failed\n  original: " + Describe(v) + "\n  sql:      " + sql +
			        "\n  back:     " + Describe(typed));
		}
	});
}

TEST_CASE("Prepared statement parameter round trip", "[property][roundtrip]") {
	PropDB db;
	rc::prop("SELECT $1 returns v", [&] {
		auto type = *GenType(2);
		auto v = *GenValue(type, 0.05);
		TagType(type);
		auto back = db.Scalar("SELECT $1", {v});
		PROP_ASSERT_VALUES_EQUAL(back, v);
	});
}

TEST_CASE("In-memory table round trip", "[property][roundtrip]") {
	PropDB db;
	rc::prop("INSERT values, SELECT them back", [&] {
		auto type = *GenType(2);
		auto values = *GenValues(type, 0.1);
		TagType(type);
		db.Exec("CREATE OR REPLACE TABLE t(i INTEGER, v " + type.ToString() + ")");
		for (idx_t i = 0; i < values.size(); i++) {
			auto res = db.Query("INSERT INTO t VALUES ($1, $2)", {Value::INTEGER(int32_t(i)), values[i]});
			PROP_REQUIRE_NO_ERROR(res, "insert " + Describe(values[i]));
		}
		auto res = db.Query("SELECT v FROM t ORDER BY i");
		PROP_REQUIRE_NO_ERROR(res, "select");
		if (res->RowCount() != values.size()) {
			RC_FAIL("row count mismatch");
		}
		for (idx_t i = 0; i < values.size(); i++) {
			PROP_ASSERT_VALUES_EQUAL(res->GetValue(0, i), values[i]);
		}
	});
}

TEST_CASE("JSON cast round trip", "[property][roundtrip][json]") {
	PropDB db;
	auto check = db.Query("SELECT '1'::JSON");
	if (check->HasError()) {
		WARN("JSON extension not available, skipping");
		return;
	}
	rc::prop("CAST(to_json(v) AS T) IS NOT DISTINCT FROM v", [&] {
		// JSON cannot represent everything: restrict to types with a lossless JSON representation
		auto type = *rc::gen::suchThat(GenType(2), [](const LogicalType &t) {
			if (TypeContainsAny(t,
			                    {LogicalTypeId::BLOB, LogicalTypeId::BIT, LogicalTypeId::FLOAT, LogicalTypeId::DOUBLE,
			                     LogicalTypeId::MAP, LogicalTypeId::UNION, LogicalTypeId::ENUM, LogicalTypeId::INTERVAL,
			                     LogicalTypeId::TIME_TZ, LogicalTypeId::TIMESTAMP_TZ})) {
				return false;
			}
			// KNOWN ISSUE: JSON -> DECIMAL goes through a double and loses precision beyond ~15 digits
			return !TypeContains(t, [](const LogicalType &c) {
				return c.id() == LogicalTypeId::DECIMAL && DecimalType::GetWidth(c) > 15;
			});
		});
		auto v = *GenValue(type, 0.05);
		TagType(type);
		// wrap in a struct: CAST(JSON AS VARCHAR) returns the raw JSON text, nested strings are unwrapped
		auto res = db.Query(
		    "SELECT (CAST(to_json({'v': $1}) AS STRUCT(v " + type.ToString() + "))).v, to_json($1)::VARCHAR", {v});
		PROP_REQUIRE_NO_ERROR(res, "type " + type.ToString() + " value " + Describe(v));
		auto &mat = res->Cast<MaterializedQueryResult>();
		auto back = mat.GetValue(0, 0);
		auto str = mat.GetValue(1, 0);
		if (!ValuesEqual(back, v)) {
			RC_FAIL("Round trip through JSON failed\n  original: " + Describe(v) + "\n  as json:  " + str.ToString() +
			        "\n  back:     " + Describe(back));
		}
	});
}

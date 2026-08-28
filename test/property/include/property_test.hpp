//===----------------------------------------------------------------------===//
//                         DuckDB
//
// test/property/include/property_test.hpp
//
// Shared helpers for RapidCheck-based property tests.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/prepared_statement.hpp"

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <string>
#include <vector>

namespace duckdb_prop {
using namespace duckdb;

//===--------------------------------------------------------------------===//
// Database helpers
//===--------------------------------------------------------------------===//
struct PropDB {
	DuckDB db;
	Connection con;

	explicit PropDB(const string &path = "") : db(path.empty() ? nullptr : path.c_str()), con(db) {
	}

	//! Run a query, return the (possibly errored) result
	unique_ptr<MaterializedQueryResult> Query(const string &sql) {
		return con.Query(sql);
	}
	//! Run a prepared query with positional parameters ($1, $2, ...)
	unique_ptr<QueryResult> Query(const string &sql, vector<Value> params) {
		auto prepared = con.Prepare(sql);
		if (prepared->HasError()) {
			auto res = make_uniq<MaterializedQueryResult>(prepared->GetErrorObject());
			return std::move(res);
		}
		return prepared->Execute(params, false);
	}
	//! Run a statement that must succeed (fails the property otherwise)
	void Exec(const string &sql) {
		auto res = con.Query(sql);
		if (res->HasError()) {
			RC_FAIL("Query failed: " + sql + "\n" + res->GetError());
		}
	}
	//! Run a query that must succeed and return exactly one value
	Value Scalar(const string &sql) {
		auto res = con.Query(sql);
		if (res->HasError()) {
			RC_FAIL("Query failed: " + sql + "\n" + res->GetError());
		}
		if (res->RowCount() != 1 || res->ColumnCount() != 1) {
			RC_FAIL("Query did not return a single value: " + sql);
		}
		return res->GetValue(0, 0);
	}
	Value Scalar(const string &sql, vector<Value> params) {
		auto res = Query(sql, std::move(params));
		if (res->HasError()) {
			RC_FAIL("Query failed: " + sql + "\n" + res->GetError());
		}
		auto &mat = res->Cast<MaterializedQueryResult>();
		if (mat.RowCount() != 1 || mat.ColumnCount() != 1) {
			RC_FAIL("Query did not return a single value: " + sql);
		}
		return mat.GetValue(0, 0);
	}
};

//! Describe a value for failure messages
inline string Describe(const Value &v) {
	string result = v.type().ToString() + ": ";
	if (v.IsNull()) {
		return result + "NULL";
	}
	result += v.ToString();
	result += "  (sql: " + v.ToSQLString() + ")";
	return result;
}

//! Value equality treating NULL == NULL and NaN == NaN, also requiring identical types
inline bool ValuesEqual(const Value &a, const Value &b) {
	if (a.type() != b.type()) {
		return false;
	}
	return Value::NotDistinctFrom(a, b);
}

//! Value equality ignoring type differences (e.g. INTEGER 1 vs BIGINT 1)
inline bool ValuesEqualLoose(const Value &a, const Value &b) {
	return Value::NotDistinctFrom(a, b);
}

//! Fail the property unless a == b, printing both values
#define PROP_ASSERT_VALUES_EQUAL(a, b)                                                                                 \
	do {                                                                                                               \
		const duckdb::Value &pa_ = (a);                                                                                \
		const duckdb::Value &pb_ = (b);                                                                                \
		if (!duckdb_prop::ValuesEqual(pa_, pb_)) {                                                                     \
			RC_FAIL(std::string("Values differ:\n  actual:   ") + duckdb_prop::Describe(pa_) +                         \
			        "\n  expected: " + duckdb_prop::Describe(pb_));                                                    \
		}                                                                                                              \
	} while (0)

//! Fail the property with the error of a result
#define PROP_REQUIRE_NO_ERROR(result, context)                                                                         \
	do {                                                                                                               \
		if ((result)->HasError()) {                                                                                    \
			RC_FAIL(std::string("Query failed (") + (context) + "): " + (result)->GetError());                         \
		}                                                                                                              \
	} while (0)

//===--------------------------------------------------------------------===//
// Generators
//===--------------------------------------------------------------------===//
//! Valid UTF-8 string with a mix of ASCII, punctuation, control chars, multi-byte code points and special strings
rc::Gen<string> GenUtf8String();
//! Printable ASCII string (0x20-0x7e)
rc::Gen<string> GenAsciiString();
//! Lower-case identifier [a-z][a-z0-9_]*
rc::Gen<string> GenIdentifier();
//! Random bytes
rc::Gen<string> GenBytes();

//! Pick one of the given elements (avoids narrowing issues of rc::gen::element)
template <class T>
rc::Gen<T> Elements(std::vector<T> elements) {
	return rc::gen::elementOf(std::move(elements));
}

//! Integer generators with extremes mixed in
template <class T>
rc::Gen<T> GenInt() {
	return rc::gen::weightedOneOf<T>(
	    {{8, rc::gen::arbitrary<T>()},
	     {2, rc::gen::element<T>(NumericLimits<T>::Minimum(), NumericLimits<T>::Maximum(), T(0), T(1), T(T(0) - T(1)))},
	     {2, rc::gen::cast<T>(rc::gen::inRange<int>(-100, 100))}});
}
rc::Gen<hugeint_t> GenHugeint();
rc::Gen<uhugeint_t> GenUhugeint();
rc::Gen<double> GenDouble();
rc::Gen<float> GenFloat();
//! Finite double/float only
rc::Gen<double> GenFiniteDouble();
rc::Gen<float> GenFiniteFloat();
rc::Gen<date_t> GenDate();
rc::Gen<date_t> GenFiniteDate();
rc::Gen<dtime_t> GenTime();
rc::Gen<timestamp_t> GenTimestamp();
rc::Gen<timestamp_t> GenFiniteTimestamp();
rc::Gen<interval_t> GenInterval();

//! Random scalar logical type (no nested types)
rc::Gen<LogicalType> GenScalarType();
//! Random logical type, including nested types up to max_depth
rc::Gen<LogicalType> GenType(int max_depth = 2);
//! Random integer (signed/unsigned, incl HUGEINT/UHUGEINT) type
rc::Gen<LogicalType> GenIntegerType();
//! Random numeric type (integers, floats, decimals)
rc::Gen<LogicalType> GenNumericType();
//! Random DECIMAL type
rc::Gen<LogicalType> GenDecimalType();
//! Types that can be compared/sorted and hashed
rc::Gen<LogicalType> GenSortableType(int max_depth = 1);

struct GenOptions {
	//! Probability of NULL at every nesting level
	double null_probability = 0.1;
	//! Only generate "safe" strings (alphanumeric, no NULL-like words) - for properties that go through the lossy
	//! nested ToString format where strings are not quoted
	bool safe_strings = false;

	GenOptions() {
	}
	explicit GenOptions(double null_probability_p) : null_probability(null_probability_p) {
	}
};

//! Random non-NULL value of the given type
rc::Gen<Value> GenNonNullValue(const LogicalType &type, const GenOptions &options = GenOptions());
//! Random value of the given type, NULL with the given probability (also applies to nested children)
rc::Gen<Value> GenValue(const LogicalType &type, double null_probability = 0.1);
rc::Gen<Value> GenValue(const LogicalType &type, const GenOptions &options);
//! Random list of values of the given type
rc::Gen<vector<Value>> GenValues(const LogicalType &type, double null_probability = 0.1);
rc::Gen<vector<Value>> GenValues(const LogicalType &type, const GenOptions &options);
//! Alphanumeric strings that survive the (unquoted) nested ToString format
rc::Gen<string> GenSafeString();
bool IsSafeString(const string &s);
//! True if all ENUM values in the type are safe strings
bool TypeHasSafeEnumValues(const LogicalType &type);
//! True if all STRUCT field names in the type are safe strings
bool TypeHasSafeFieldNames(const LogicalType &type);

//! Returns true if the type or any nested child type satisfies the predicate
template <class F>
bool TypeContains(const LogicalType &type, F &&pred) {
	if (pred(type)) {
		return true;
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return TypeContains(ListType::GetChildType(type), pred);
	case LogicalTypeId::ARRAY:
		return TypeContains(ArrayType::GetChildType(type), pred);
	case LogicalTypeId::MAP:
		return TypeContains(MapType::KeyType(type), pred) || TypeContains(MapType::ValueType(type), pred);
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::UNION:
		for (auto &child : StructType::GetChildTypes(type)) {
			if (TypeContains(child.second, pred)) {
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}
//! Returns true if the value or any nested child value satisfies the predicate
template <class F>
bool ValueContains(const Value &value, F &&pred) {
	if (pred(value)) {
		return true;
	}
	if (value.IsNull()) {
		return false;
	}
	switch (value.type().id()) {
	case LogicalTypeId::LIST:
		for (auto &child : ListValue::GetChildren(value)) {
			if (ValueContains(child, pred)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::ARRAY:
		for (auto &child : ArrayValue::GetChildren(value)) {
			if (ValueContains(child, pred)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::MAP:
		for (auto &child : MapValue::GetChildren(value)) {
			if (ValueContains(child, pred)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::UNION:
		for (auto &child : StructValue::GetChildren(value)) {
			if (ValueContains(child, pred)) {
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}
//! Returns true if the type (or any nested child) has one of the given type ids
bool TypeContainsAny(const LogicalType &type, const vector<LogicalTypeId> &ids);

//! Join a list of SQL snippets with a separator
string Join(const vector<string> &parts, const string &sep);
//! Convert a (valid UTF-8) string to code points
vector<uint32_t> ToCodepoints(const string &s);
//! Encode code points as UTF-8
string FromCodepoints(const vector<uint32_t> &cps);

} // namespace duckdb_prop

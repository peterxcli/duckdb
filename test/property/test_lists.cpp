// List function properties checked against reference implementations.
#include "property_test.hpp"

#include <algorithm>

using namespace duckdb_prop;

namespace {

Value ListOf(const LogicalType &child, vector<Value> values) {
	return Value::LIST(child, std::move(values));
}

//! DuckDB's ORDER BY comparison for values: NULLs sort with NULLS LAST in ASC
bool OrderByLess(const Value &a, const Value &b) {
	if (a.IsNull()) {
		return false;
	}
	if (b.IsNull()) {
		return true;
	}
	return a < b;
}

} // namespace

TEST_CASE("List functions match reference implementations", "[property][lists]") {
	PropDB db;

	rc::prop("list_sort matches std::stable_sort with Value comparison", [&] {
		// KNOWN ISSUE: create_sort_key encodes INTERVALs unnormalized, so list_sort disagrees with ORDER BY
		auto type = *rc::gen::suchThat(
		    GenSortableType(1), [](const LogicalType &t) { return !TypeContainsAny(t, {LogicalTypeId::INTERVAL}); });
		auto values = *GenValues(type, 0.15);
		RC_TAG(LogicalTypeIdToString(type.id()));
		auto l = ListOf(type, values);

		auto sorted = values;
		std::stable_sort(sorted.begin(), sorted.end(), OrderByLess);
		auto actual = db.Scalar("SELECT list_sort($1)", {l});
		PROP_ASSERT_VALUES_EQUAL(actual, ListOf(type, sorted));

		auto rsorted = values;
		std::stable_sort(rsorted.begin(), rsorted.end(), [](const Value &a, const Value &b) {
			// DESC with NULLS LAST (DuckDB's global default null order)
			if (a.IsNull()) {
				return false;
			}
			if (b.IsNull()) {
				return true;
			}
			return b < a;
		});
		auto actual_r = db.Scalar("SELECT list_reverse_sort($1)", {l});
		PROP_ASSERT_VALUES_EQUAL(actual_r, ListOf(type, rsorted));
	});

	rc::prop("list_contains/list_position/list_distinct", [&] {
		// KNOWN ISSUE: INTERVAL sort order (see above); interval equality also normalizes (1 month = 30 days)
		auto type = *rc::gen::suchThat(
		    GenSortableType(1), [](const LogicalType &t) { return !TypeContainsAny(t, {LogicalTypeId::INTERVAL}); });
		auto values = *GenValues(type, 0.15);
		auto l = ListOf(type, values);
		// needle: either random or an element of the list
		auto needle =
		    values.empty() ? *GenValue(type, 0.2) : *rc::gen::oneOf(GenValue(type, 0.2), rc::gen::elementOf(values));

		// list_contains: NULL needle -> NULL, NULL elements ignored;
		// list_position: NULL needle matches NULL elements (IS NOT DISTINCT semantics)
		Value expected_contains;
		Value expected_position;
		{
			int32_t pos = 0;
			for (idx_t i = 0; i < values.size(); i++) {
				if (Value::NotDistinctFrom(values[i], needle)) {
					pos = int32_t(i) + 1;
					break;
				}
			}
			expected_position = pos ? Value::INTEGER(pos) : Value(LogicalType::INTEGER);
			if (needle.IsNull()) {
				expected_contains = Value(LogicalType::BOOLEAN);
			} else {
				expected_contains = Value::BOOLEAN(pos != 0 && !values[pos - 1].IsNull());
			}
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_contains($1, $2)", {l, needle}), expected_contains);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_position($1, $2)", {l, needle}), expected_position);

		// list_distinct: distinct non-NULL elements, order unspecified -> compare as sorted multiset
		vector<Value> distinct;
		for (auto &v : values) {
			if (v.IsNull()) {
				continue;
			}
			bool dup = false;
			for (auto &d : distinct) {
				if (Value::NotDistinctFrom(d, v)) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				distinct.push_back(v);
			}
		}
		std::stable_sort(distinct.begin(), distinct.end(), OrderByLess);
		auto actual_distinct = db.Scalar("SELECT list_sort(list_distinct($1))", {l});
		PROP_ASSERT_VALUES_EQUAL(actual_distinct, ListOf(type, distinct));
	});

	rc::prop("list slicing matches the substring model", [&] {
		auto type = *GenScalarType();
		auto values = *GenValues(type, 0.1);
		auto l = ListOf(type, values);
		int64_t n = int64_t(values.size());
		auto begin = *rc::gen::inRange<int64_t>(-n - 3, n + 4);
		auto end = *rc::gen::inRange<int64_t>(-n - 3, n + 4);
		// list[begin:end]: 1-based inclusive bounds, negative from the end
		int64_t lo = begin < 0 ? std::max<int64_t>(n + begin + 1, 1) : std::max<int64_t>(begin, 1);
		int64_t hi = end < 0 ? n + end + 1 : std::min<int64_t>(end, n);
		vector<Value> expected;
		for (int64_t p = lo; p <= hi && p >= 1 && p <= n; p++) {
			expected.push_back(values[p - 1]);
		}
		auto actual = db.Scalar("SELECT list_slice($1, $2, $3)", {l, Value::BIGINT(begin), Value::BIGINT(end)});
		PROP_ASSERT_VALUES_EQUAL(actual, ListOf(type, expected));
	});

	rc::prop("list_reverse/list_resize/flatten/list_concat", [&] {
		auto type = *GenScalarType();
		auto values = *GenValues(type, 0.1);
		auto l = ListOf(type, values);

		auto reversed = values;
		std::reverse(reversed.begin(), reversed.end());
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_reverse($1)", {l}), ListOf(type, reversed));

		auto new_size = *rc::gen::inRange<int64_t>(0, int64_t(values.size()) + 5);
		vector<Value> resized;
		for (int64_t i = 0; i < new_size; i++) {
			resized.push_back(i < int64_t(values.size()) ? values[i] : Value(type));
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_resize($1, $2)", {l, Value::BIGINT(new_size)}),
		                         ListOf(type, resized));

		auto values2 = *GenValues(type, 0.1);
		auto l2 = ListOf(type, values2);
		auto concat = values;
		concat.insert(concat.end(), values2.begin(), values2.end());
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_concat($1, $2)", {l, l2}), ListOf(type, concat));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT $1 || $2", {l, l2}), ListOf(type, concat));

		// flatten: [l, l2] -> l || l2; NULL sublists are skipped
		auto nested = Value::LIST(LogicalType::LIST(type), {l, l2});
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT flatten($1)", {nested}), ListOf(type, concat));
	});

	rc::prop("range/generate_series match the closed-form count", [&] {
		auto start =
		    *rc::gen::weightedOneOf<int64_t>({{5, rc::gen::inRange<int64_t>(-1000, 1000)}, {2, GenInt<int64_t>()}});
		auto stop =
		    *rc::gen::weightedOneOf<int64_t>({{5, rc::gen::inRange<int64_t>(-1000, 1000)}, {2, GenInt<int64_t>()}});
		auto step = *rc::gen::weightedOneOf<int64_t>({{5, rc::gen::inRange<int64_t>(-50, 51)}, {2, GenInt<int64_t>()}});
		RC_PRE(step != 0);
		// closed-form expected count using __int128 to dodge overflow
		__int128 s = start, e = stop, st = step;
		__int128 range_count = 0, series_count = 0;
		if (st > 0) {
			if (e > s) {
				range_count = (e - s + st - 1) / st;
			}
			if (e >= s) {
				series_count = (e - s) / st + 1;
			}
		} else {
			if (e < s) {
				range_count = (s - e + (-st) - 1) / (-st);
			}
			if (e <= s) {
				series_count = (s - e) / (-st) + 1;
			}
		}
		// keep result sizes sane
		RC_PRE(range_count < 100000 && series_count < 100000);
		auto res = db.Query("SELECT count(*), min(r), max(r) FROM range($1, $2, $3) t(r)",
		                    {Value::BIGINT(start), Value::BIGINT(stop), Value::BIGINT(step)});
		PROP_REQUIRE_NO_ERROR(res, "range");
		auto &mat = res->Cast<MaterializedQueryResult>();
		PROP_ASSERT_VALUES_EQUAL(mat.GetValue(0, 0), Value::BIGINT(int64_t(range_count)));
		if (range_count > 0) {
			auto expected_min = step > 0 ? start : start + (int64_t(range_count) - 1) * step;
			auto expected_max = step > 0 ? start + (int64_t(range_count) - 1) * step : start;
			PROP_ASSERT_VALUES_EQUAL(mat.GetValue(1, 0), Value::BIGINT(expected_min));
			PROP_ASSERT_VALUES_EQUAL(mat.GetValue(2, 0), Value::BIGINT(expected_max));
		}
		auto res2 = db.Query("SELECT count(*), min(r), max(r) FROM generate_series($1, $2, $3) t(r)",
		                     {Value::BIGINT(start), Value::BIGINT(stop), Value::BIGINT(step)});
		PROP_REQUIRE_NO_ERROR(res2, "generate_series");
		auto &mat2 = res2->Cast<MaterializedQueryResult>();
		PROP_ASSERT_VALUES_EQUAL(mat2.GetValue(0, 0), Value::BIGINT(int64_t(series_count)));
	});

	rc::prop("list aggregates match scalar aggregates", [&] {
		auto values = *GenValues(LogicalType::BIGINT, 0.15);
		auto l = ListOf(LogicalType::BIGINT, values);
		int64_t cnt = 0;
		__int128 sum = 0;
		Value min_v, max_v;
		for (auto &v : values) {
			if (v.IsNull()) {
				continue;
			}
			auto x = v.GetValue<int64_t>();
			cnt++;
			sum += x;
			if (min_v.IsNull() || x < min_v.GetValue<int64_t>()) {
				min_v = v;
			}
			if (max_v.IsNull() || x > max_v.GetValue<int64_t>()) {
				max_v = v;
			}
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_count($1)", {l}), Value::BIGINT(cnt));
		if (cnt > 0) {
			PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_min($1)", {l}), Value::BIGINT(min_v.GetValue<int64_t>()));
			PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT list_max($1)", {l}), Value::BIGINT(max_v.GetValue<int64_t>()));
		}
		// list_sum promotes to HUGEINT
		auto actual_sum = db.Scalar("SELECT list_sum($1)", {l});
		if (cnt == 0) {
			if (!actual_sum.IsNull()) {
				RC_FAIL("expected NULL sum, got " + Describe(actual_sum));
			}
		} else {
			PROP_ASSERT_VALUES_EQUAL(actual_sum.DefaultCastAs(LogicalType::HUGEINT),
			                         Value::HUGEINT(hugeint_t(int64_t(sum >> 64), uint64_t(sum))));
		}
	});
}

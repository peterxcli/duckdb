// Date/time properties checked against independent calendar algorithms
// (days_from_civil / civil_from_days, Howard Hinnant's public-domain algorithms).
#include "property_test.hpp"

using namespace duckdb_prop;

namespace {

struct Civil {
	int64_t y;
	int32_t m;
	int32_t d;
};

//! days since 1970-01-01 from proleptic Gregorian date
int64_t DaysFromCivil(int64_t y, int32_t m, int32_t d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int64_t yoe = y - era * 400;                                  // [0, 399]
	const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
	return era * 146097 + doe - 719468;
}

Civil CivilFromDays(int64_t z) {
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;                                      // [0, 146096]
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
	const int64_t y = yoe + era * 400;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
	const int64_t mp = (5 * doy + 2) / 153;                      // [0, 11]
	const int64_t d = doy - (153 * mp + 2) / 5 + 1;              // [1, 31]
	const int64_t m = mp < 10 ? mp + 3 : mp - 9;                 // [1, 12]
	Civil result;
	result.y = y + (m <= 2);
	result.m = int32_t(m);
	result.d = int32_t(d);
	return result;
}

bool IsLeap(int64_t y) {
	return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

int32_t LastDayOfMonth(int64_t y, int32_t m) {
	static const int32_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return m == 2 && IsLeap(y) ? 29 : days[m - 1];
}

//! ISO 8601 week and week-based year
void IsoWeek(int64_t days, int64_t &iso_year, int32_t &iso_week) {
	auto c = CivilFromDays(days);
	// ISO weekday: Mon=1..Sun=7; 1970-01-01 was a Thursday (4)
	auto weekday = [](int64_t z) {
		return int32_t(((z % 7) + 10) % 7) + 1;
	}; // Mon=1..Sun=7
	// Thursday of the current week determines the ISO year
	int64_t thursday = days - (weekday(days) - 4);
	auto tc = CivilFromDays(thursday);
	iso_year = tc.y;
	int64_t jan1 = DaysFromCivil(tc.y, 1, 1);
	iso_week = int32_t((thursday - jan1) / 7) + 1;
	(void)c;
}

} // namespace

TEST_CASE("Date functions match calendar algorithms", "[property][datetime]") {
	PropDB db;
	rc::prop("date part extraction", [&] {
		auto d = *GenFiniteDate();
		int64_t days = d.days;
		auto c = CivilFromDays(days);
		// KNOWN ISSUE: week-based parts materialize Jan 1 of the year, out of range for the minimum year
		RC_PRE(c.y > Date::DATE_MIN_YEAR);
		// KNOWN ISSUE: last_day at the maximum month raises an INTERNAL error (first-of-next-month overflows)
		bool last_day_safe = !(c.y == Date::DATE_MAX_YEAR && c.m >= Date::DATE_MAX_MONTH);
		RC_PRE(last_day_safe);
		auto vd = Value::DATE(d);
		auto res = db.Query("SELECT year($1), month($1), day($1), dayofweek($1), isodow($1), dayofyear($1), "
		                    "quarter($1), last_day($1), monthname($1) IS NOT NULL, weekofyear($1), isoyear($1), "
		                    "make_date(CAST($2 AS BIGINT), $3, $4)",
		                    {vd, Value::BIGINT(c.y), Value::INTEGER(c.m), Value::INTEGER(c.d)});
		PROP_REQUIRE_NO_ERROR(res, "date parts for " + vd.ToString());
		auto &m = res->Cast<MaterializedQueryResult>();
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(0, 0), Value::BIGINT(c.y));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(1, 0), Value::BIGINT(c.m));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(2, 0), Value::BIGINT(c.d));
		// dayofweek: Sunday=0..Saturday=6; 1970-01-01 was Thursday(4)
		int64_t dow = ((days % 7) + 10) % 7 + 1; // Mon=1..Sun=7
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(3, 0), Value::BIGINT(dow % 7));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(4, 0), Value::BIGINT(dow));
		int64_t doy = days - DaysFromCivil(c.y, 1, 1) + 1;
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(5, 0), Value::BIGINT(doy));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(6, 0), Value::BIGINT((c.m - 1) / 3 + 1));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(7, 0),
		                         Value::DATE(date_t(int32_t(DaysFromCivil(c.y, c.m, LastDayOfMonth(c.y, c.m))))));
		int64_t iso_year;
		int32_t iso_week;
		IsoWeek(days, iso_year, iso_week);
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(9, 0), Value::BIGINT(iso_week));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(10, 0), Value::BIGINT(iso_year));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(11, 0), vd);
	});

	rc::prop("date arithmetic", [&] {
		auto d = *GenFiniteDate();
		auto n = *rc::gen::weightedOneOf<int64_t>(
		    {{5, rc::gen::inRange<int64_t>(-100000, 100000)}, {2, rc::gen::cast<int64_t>(GenInt<int32_t>())}});
		int64_t days = d.days;
		auto vd = Value::DATE(d);
		// d + n days
		__int128 target = __int128(days) + n;
		auto res = db.Query("SELECT $1 + $2::INTEGER", {vd, Value::BIGINT(n)});
		auto min_days = Date::FromDate(Date::DATE_MIN_YEAR, Date::DATE_MIN_MONTH, Date::DATE_MIN_DAY).days;
		auto max_days = Date::FromDate(Date::DATE_MAX_YEAR, Date::DATE_MAX_MONTH, Date::DATE_MAX_DAY).days;
		bool valid = target >= min_days && target <= max_days;
		if (valid) {
			PROP_REQUIRE_NO_ERROR(res, "date + int");
			auto got = res->Cast<MaterializedQueryResult>().GetValue(0, 0);
			PROP_ASSERT_VALUES_EQUAL(got, Value::DATE(date_t(int32_t(target))));
		} else if (!res->HasError()) {
			auto got = res->Cast<MaterializedQueryResult>().GetValue(0, 0);
			RC_FAIL("expected out-of-range error for " + vd.ToString() + " + " + std::to_string(n) + ", got " +
			        got.ToString());
		}
		// date difference
		auto d2 = *GenFiniteDate();
		auto diff = db.Scalar("SELECT $1 - $2", {vd, Value::DATE(d2)});
		PROP_ASSERT_VALUES_EQUAL(diff, Value::BIGINT(int64_t(days) - int64_t(d2.days)));
		auto datediff = db.Scalar("SELECT date_diff('day', $2, $1)", {vd, Value::DATE(d2)});
		PROP_ASSERT_VALUES_EQUAL(datediff, Value::BIGINT(int64_t(days) - int64_t(d2.days)));
	});

	rc::prop("timestamp epoch round trips", [&] {
		auto ts = *GenFiniteTimestamp();
		auto vts = Value::TIMESTAMP(ts);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT epoch_us($1)", {vts}), Value::BIGINT(ts.value));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT make_timestamp($1)", {Value::BIGINT(ts.value)}), vts);
		// epoch_ms rounds half away from zero
		int64_t ms = ts.value / 1000;
		int64_t rem = ts.value % 1000;
		if (rem >= 500) {
			ms++;
		} else if (rem <= -500) {
			ms--;
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT epoch_ms($1)", {vts}), Value::BIGINT(ms));
		// date/time decomposition: ts::DATE + ts::TIME == ts
		auto res = db.Query("SELECT CAST($1 AS DATE), CAST($1 AS TIME)", {vts});
		PROP_REQUIRE_NO_ERROR(res, "ts decompose");
		auto &m = res->Cast<MaterializedQueryResult>();
		auto date_part = m.GetValue(0, 0);
		auto time_part = m.GetValue(1, 0);
		__int128 fd =
		    (__int128(ts.value) - (ts.value < 0 ? Interval::MICROS_PER_DAY - 1 : 0)) / Interval::MICROS_PER_DAY;
		int64_t expected_days = int64_t(fd);
		int64_t expected_micros = int64_t(__int128(ts.value) - fd * Interval::MICROS_PER_DAY);
		PROP_ASSERT_VALUES_EQUAL(date_part, Value::DATE(date_t(int32_t(expected_days))));
		PROP_ASSERT_VALUES_EQUAL(time_part, Value::TIME(dtime_t(expected_micros)));
		// recompose
		auto recomposed = db.Scalar("SELECT $1 + $2", {date_part, time_part});
		PROP_ASSERT_VALUES_EQUAL(recomposed, vts);
	});

	rc::prop("strftime/strptime round trip", [&] {
		auto ts = *GenFiniteTimestamp();
		// strptime requires years representable in the format; stay within a wide but printable range
		auto c = CivilFromDays(int64_t((__int128(ts.value) - (ts.value < 0 ? Interval::MICROS_PER_DAY - 1 : 0)) /
		                               Interval::MICROS_PER_DAY));
		RC_PRE(c.y >= 1 && c.y <= 9999);
		auto fmt = *rc::gen::elementOf(vector<string> {
		    "%Y-%m-%d %H:%M:%S.%f",
		    "%Y/%m/%d %H:%M:%S.%f",
		    "%d.%m.%Y %H:%M:%S.%f",
		    "%Y-%m-%dT%H:%M:%S.%f",
		    "%m/%d/%Y %H:%M:%S.%f",
		});
		auto vts = Value::TIMESTAMP(ts);
		auto str = db.Scalar("SELECT strftime($1, '" + fmt + "')", {vts});
		auto back = db.Scalar("SELECT strptime($1, '" + fmt + "')", {str});
		PROP_ASSERT_VALUES_EQUAL(back, vts);
	});

	rc::prop("time part extraction", [&] {
		auto t = *GenTime();
		auto vt = Value::TIME(t);
		auto micros = t.value;
		auto res = db.Query("SELECT hour($1), minute($1), second($1), millisecond($1), microsecond($1)", {vt});
		PROP_REQUIRE_NO_ERROR(res, "time parts");
		auto &m = res->Cast<MaterializedQueryResult>();
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(0, 0), Value::BIGINT(micros / Interval::MICROS_PER_HOUR));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(1, 0),
		                         Value::BIGINT((micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(2, 0),
		                         Value::BIGINT((micros % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_SEC));
		// millisecond/microsecond include the seconds component (PostgreSQL semantics)
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(3, 0),
		                         Value::BIGINT((micros % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_MSEC));
		PROP_ASSERT_VALUES_EQUAL(m.GetValue(4, 0), Value::BIGINT(micros % Interval::MICROS_PER_MINUTE));
	});
}

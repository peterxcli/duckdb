#include "property_test.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/bit.hpp"

#include <cmath>

namespace duckdb_prop {

//===--------------------------------------------------------------------===//
// Strings
//===--------------------------------------------------------------------===//
string FromCodepoints(const vector<uint32_t> &cps) {
	string result;
	for (auto cp : cps) {
		if (cp < 0x80) {
			result.push_back(char(cp));
		} else if (cp < 0x800) {
			result.push_back(char(0xC0 | (cp >> 6)));
			result.push_back(char(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			result.push_back(char(0xE0 | (cp >> 12)));
			result.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back(char(0x80 | (cp & 0x3F)));
		} else {
			result.push_back(char(0xF0 | (cp >> 18)));
			result.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
			result.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back(char(0x80 | (cp & 0x3F)));
		}
	}
	return result;
}

vector<uint32_t> ToCodepoints(const string &s) {
	vector<uint32_t> result;
	idx_t i = 0;
	while (i < s.size()) {
		auto c = uint8_t(s[i]);
		uint32_t cp;
		idx_t len;
		if (c < 0x80) {
			cp = c;
			len = 1;
		} else if ((c >> 5) == 0x6) {
			cp = c & 0x1F;
			len = 2;
		} else if ((c >> 4) == 0xE) {
			cp = c & 0x0F;
			len = 3;
		} else {
			cp = c & 0x07;
			len = 4;
		}
		for (idx_t j = 1; j < len; j++) {
			cp = (cp << 6) | (uint8_t(s[i + j]) & 0x3F);
		}
		result.push_back(cp);
		i += len;
	}
	return result;
}

static rc::Gen<uint32_t> GenCodepoint() {
	return rc::gen::weightedOneOf<uint32_t>({
	    {12, rc::gen::inRange<uint32_t>(0x61, 0x7B)}, // a-z
	    {8, rc::gen::inRange<uint32_t>(0x20, 0x7F)},  // printable ASCII
	    {6, Elements<uint32_t>({'\'', '"', '\\', ',', '{', '}', '[', ']', '(', ')', ':', ';', '=', '>',  '<',  '%',
	                            '_',  ' ', '.',  '-', '+', '0', '1', '9', 'e', 'E', 'N', 'U', 'L', '\t', '\n', '\r'})},
	    {1, rc::gen::inRange<uint32_t>(0x01, 0x20)},      // control chars
	    {1, rc::gen::just<uint32_t>(0)},                  // NUL
	    {2, rc::gen::inRange<uint32_t>(0x7F, 0x100)},     // DEL + latin-1
	    {2, rc::gen::inRange<uint32_t>(0x100, 0x800)},    // 2-byte
	    {1, rc::gen::inRange<uint32_t>(0x300, 0x370)},    // combining diacritics
	    {2, rc::gen::inRange<uint32_t>(0x800, 0xD800)},   // 3-byte (CJK etc)
	    {1, rc::gen::inRange<uint32_t>(0xE000, 0x10000)}, // 3-byte private use/specials
	    {1, Elements<uint32_t>({0xFEFF, 0x200D, 0xFFFD, 0x2028, 0x00A0, 0x1F600, 0x1F1E6, 0xFFFF})},
	    {2, rc::gen::inRange<uint32_t>(0x10000, 0x110000)}, // 4-byte
	});
}

rc::Gen<string> GenUtf8String() {
	static const vector<string> specials = {"",
	                                        "NULL",
	                                        "null",
	                                        "'",
	                                        "''",
	                                        "\"",
	                                        "\\",
	                                        "{",
	                                        "}",
	                                        "[",
	                                        "]",
	                                        ",",
	                                        " ",
	                                        "\n",
	                                        "\t",
	                                        "1",
	                                        "-0",
	                                        "0",
	                                        "true",
	                                        "false",
	                                        "inf",
	                                        "-inf",
	                                        "nan",
	                                        "1e10",
	                                        "::",
	                                        "=",
	                                        "=>",
	                                        ">",
	                                        "a, b",
	                                        "'a'",
	                                        "\\x00",
	                                        "\\x",
	                                        "[]",
	                                        "{}",
	                                        "{'a':1}",
	                                        "[1, 2]",
	                                        "  x  ",
	                                        "x\0y",
	                                        "NaN",
	                                        "Infinity",
	                                        "\xF0\x9F\x98\x80",
	                                        "e\xCC\x81",
	                                        "\xE2\x80\x8B",
	                                        "%",
	                                        "_",
	                                        "%_%"};
	return rc::gen::weightedOneOf<string>(
	    {{1, rc::gen::elementOf(specials)},
	     {6, rc::gen::map(rc::gen::container<vector<uint32_t>>(GenCodepoint()), FromCodepoints)},
	     {2,
	      rc::gen::map(rc::gen::container<vector<uint32_t>>(rc::gen::inRange<uint32_t>(0x20, 0x7F)), FromCodepoints)}});
}

rc::Gen<string> GenAsciiString() {
	return rc::gen::map(rc::gen::container<vector<uint32_t>>(rc::gen::inRange<uint32_t>(0x20, 0x7F)), FromCodepoints);
}

rc::Gen<string> GenIdentifier() {
	return rc::gen::map(rc::gen::container<vector<uint32_t>>(rc::gen::weightedOneOf<uint32_t>(
	                        {{10, rc::gen::inRange<uint32_t>('a', 'z' + 1)}, {1, rc::gen::just<uint32_t>('_')}})),
	                    [](const vector<uint32_t> &cps) {
		                    auto s = FromCodepoints(cps);
		                    return "c" + s;
	                    });
}

rc::Gen<string> GenBytes() {
	return rc::gen::map(rc::gen::container<vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()),
	                    [](const vector<uint8_t> &bytes) { return string(bytes.begin(), bytes.end()); });
}

bool TypeContainsAny(const LogicalType &type, const vector<LogicalTypeId> &ids) {
	return TypeContains(type, [&](const LogicalType &t) {
		for (auto id : ids) {
			if (t.id() == id) {
				return true;
			}
		}
		return false;
	});
}

bool IsSafeString(const string &s) {
	if (s.empty() || s.front() == ' ' || s.back() == ' ') {
		return false;
	}
	for (auto c : s) {
		if (!isalnum(uint8_t(c)) && c != ' ') {
			return false;
		}
	}
	auto lower = StringUtil::Lower(s);
	return lower != "null";
}

rc::Gen<string> GenSafeString() {
	return rc::gen::suchThat(rc::gen::map(rc::gen::container<vector<uint32_t>>(rc::gen::weightedOneOf<uint32_t>(
	                                          {{10, rc::gen::inRange<uint32_t>('a', 'z' + 1)},
	                                           {3, rc::gen::inRange<uint32_t>('A', 'Z' + 1)},
	                                           {3, rc::gen::inRange<uint32_t>('0', '9' + 1)},
	                                           {1, rc::gen::just<uint32_t>(' ')}})),
	                                      FromCodepoints),
	                         IsSafeString);
}

bool TypeHasSafeEnumValues(const LogicalType &type) {
	return !TypeContains(type, [](const LogicalType &t) {
		if (t.id() != LogicalTypeId::ENUM) {
			return false;
		}
		auto &values = EnumType::GetValuesInsertOrder(t);
		auto size = EnumType::GetSize(t);
		auto data = FlatVector::GetData<string_t>(values);
		for (idx_t i = 0; i < size; i++) {
			if (!IsSafeString(data[i].GetString())) {
				return true;
			}
		}
		return false;
	});
}

bool TypeHasSafeFieldNames(const LogicalType &type) {
	return !TypeContains(type, [](const LogicalType &t) {
		if (t.id() != LogicalTypeId::STRUCT) {
			return false;
		}
		for (auto &child : StructType::GetChildTypes(t)) {
			if (!IsSafeString(child.first.GetIdentifierName())) {
				return true;
			}
		}
		return false;
	});
}

string Join(const vector<string> &parts, const string &sep) {
	string result;
	for (idx_t i = 0; i < parts.size(); i++) {
		if (i > 0) {
			result += sep;
		}
		result += parts[i];
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Numerics
//===--------------------------------------------------------------------===//
rc::Gen<hugeint_t> GenHugeint() {
	return rc::gen::weightedOneOf<hugeint_t>(
	    {{6, rc::gen::map(
	             rc::gen::tuple(rc::gen::arbitrary<int64_t>(), rc::gen::arbitrary<uint64_t>()),
	             [](const std::tuple<int64_t, uint64_t> &t) { return hugeint_t(std::get<0>(t), std::get<1>(t)); })},
	     {3, rc::gen::map(GenInt<int64_t>(), [](int64_t v) { return hugeint_t(v); })},
	     {2, rc::gen::element<hugeint_t>(NumericLimits<hugeint_t>::Minimum(), NumericLimits<hugeint_t>::Maximum(),
	                                     hugeint_t(0), hugeint_t(1), hugeint_t(-1),
	                                     hugeint_t(NumericLimits<int64_t>::Maximum()) + 1,
	                                     hugeint_t(NumericLimits<int64_t>::Minimum()) - 1)}});
}

rc::Gen<uhugeint_t> GenUhugeint() {
	return rc::gen::weightedOneOf<uhugeint_t>(
	    {{6, rc::gen::map(
	             rc::gen::tuple(rc::gen::arbitrary<uint64_t>(), rc::gen::arbitrary<uint64_t>()),
	             [](const std::tuple<uint64_t, uint64_t> &t) { return uhugeint_t(std::get<0>(t), std::get<1>(t)); })},
	     {3, rc::gen::map(GenInt<uint64_t>(), [](uint64_t v) { return uhugeint_t(v); })},
	     {2, rc::gen::element<uhugeint_t>(NumericLimits<uhugeint_t>::Minimum(), NumericLimits<uhugeint_t>::Maximum(),
	                                      uhugeint_t(0), uhugeint_t(1),
	                                      uhugeint_t(NumericLimits<uint64_t>::Maximum()) + 1)}});
}

rc::Gen<double> GenFiniteDouble() {
	return rc::gen::weightedOneOf<double>(
	    {{4, rc::gen::map(rc::gen::tuple(rc::gen::arbitrary<int64_t>(), rc::gen::inRange<int>(-1074, 1024)),
	                      [](const std::tuple<int64_t, int> &t) {
		                      // random mantissa and exponent, covers the whole range incl. denormals
		                      double m = double(std::get<0>(t)) / 9223372036854775808.0; // in (-1, 1)
		                      double r = std::ldexp(m, std::get<1>(t));
		                      return std::isfinite(r) ? r : 0.0;
	                      })},
	     {3, rc::gen::map(rc::gen::inRange<int64_t>(-1000000, 1000000), [](int64_t v) { return double(v) / 1000.0; })},
	     {2, rc::gen::map(GenInt<int64_t>(), [](int64_t v) { return double(v); })},
	     {1, rc::gen::map(rc::gen::inRange<int>(-40, 40), [](int e) { return std::pow(10.0, e); })},
	     {2, rc::gen::element<double>(0.0, -0.0, 1.0, -1.0, 0.5, 0.1, 0.3, 1e-7, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20,
	                                  1e38, 1e308, -1e308, 4.9e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
	                                  9007199254740992.0, 9007199254740993.0, 0.30000000000000004, 123456789.123456789,
	                                  2147483647.5, 2147483648.0, -2147483648.5, 9223372036854775807.0,
	                                  -9223372036854775808.0, 18446744073709551616.0, 1.5, 2.5, -0.5, -1.5,
	                                  0.49999999999999994)}});
}

rc::Gen<double> GenDouble() {
	return rc::gen::weightedOneOf<double>(
	    {{9, GenFiniteDouble()},
	     {1, rc::gen::element<double>(std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
	                                  std::numeric_limits<double>::quiet_NaN())}});
}

rc::Gen<float> GenFiniteFloat() {
	return rc::gen::weightedOneOf<float>(
	    {{4, rc::gen::map(rc::gen::tuple(rc::gen::arbitrary<int32_t>(), rc::gen::inRange<int>(-149, 128)),
	                      [](const std::tuple<int32_t, int> &t) {
		                      float m = float(std::get<0>(t)) / 2147483648.0f;
		                      float r = std::ldexp(m, std::get<1>(t));
		                      return std::isfinite(r) ? r : 0.0f;
	                      })},
	     {3, rc::gen::map(rc::gen::inRange<int32_t>(-1000000, 1000000), [](int32_t v) { return float(v) / 1000.0f; })},
	     {2, rc::gen::map(GenInt<int32_t>(), [](int32_t v) { return float(v); })},
	     {2, rc::gen::element<float>(0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 0.1f, 1e-7f, 1e15f, 1e16f, 3.4028235e38f,
	                                 -3.4028235e38f, 1.4e-45f, 1.1754944e-38f, 16777216.0f, 16777217.0f, 0.3f,
	                                 2147483648.0f, 1.5f, 2.5f, -0.5f)}});
}

rc::Gen<float> GenFloat() {
	return rc::gen::weightedOneOf<float>(
	    {{9, GenFiniteFloat()},
	     {1, rc::gen::element<float>(std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
	                                 std::numeric_limits<float>::quiet_NaN())}});
}

//===--------------------------------------------------------------------===//
// Temporal
//===--------------------------------------------------------------------===//
static int32_t MinDateDays() {
	static const int32_t days = Date::FromDate(Date::DATE_MIN_YEAR, Date::DATE_MIN_MONTH, Date::DATE_MIN_DAY).days;
	return days;
}
static int32_t MaxDateDays() {
	static const int32_t days = Date::FromDate(Date::DATE_MAX_YEAR, Date::DATE_MAX_MONTH, Date::DATE_MAX_DAY).days;
	return days;
}

rc::Gen<date_t> GenFiniteDate() {
	return rc::gen::weightedOneOf<date_t>(
	    {{4, rc::gen::map(rc::gen::inRange<int32_t>(MinDateDays(), MaxDateDays() + 1),
	                      [](int32_t d) { return date_t(d); })},
	     {4, rc::gen::map(rc::gen::inRange<int32_t>(-800000, 800000), [](int32_t d) { return date_t(d); })},
	     {2, rc::gen::map(rc::gen::inRange<int32_t>(-1000, 30000), [](int32_t d) { return date_t(d); })},
	     {1, rc::gen::element<date_t>(date_t(MinDateDays()), date_t(MaxDateDays()), date_t(0), date_t(-1), date_t(1),
	                                  Date::FromDate(1, 1, 1), Date::FromDate(0, 1, 1), Date::FromDate(-1, 12, 31),
	                                  Date::FromDate(1582, 10, 15), Date::FromDate(1900, 3, 1),
	                                  Date::FromDate(2000, 2, 29), Date::FromDate(9999, 12, 31),
	                                  Date::FromDate(10000, 1, 1), Date::FromDate(-4713, 11, 24))}});
}

rc::Gen<date_t> GenDate() {
	return rc::gen::weightedOneOf<date_t>(
	    {{15, GenFiniteDate()}, {1, rc::gen::element<date_t>(date_t::infinity(), date_t::ninfinity())}});
}

rc::Gen<dtime_t> GenTime() {
	return rc::gen::weightedOneOf<dtime_t>(
	    {{6,
	      rc::gen::map(rc::gen::inRange<int64_t>(0, Interval::MICROS_PER_DAY), [](int64_t v) { return dtime_t(v); })},
	     {2, rc::gen::map(rc::gen::inRange<int64_t>(0, 86400), [](int64_t v) { return dtime_t(v * 1000000); })},
	     {1, rc::gen::element<dtime_t>(dtime_t(0), dtime_t(1), dtime_t(Interval::MICROS_PER_DAY - 1),
	                                   dtime_t(Interval::MICROS_PER_DAY), dtime_t(12 * Interval::MICROS_PER_HOUR))}});
}

//! Minimum finite TIMESTAMP in micros (290309-12-22 (BC) 00:00:00); the int64 range below it cannot be
//! converted to a date and is rejected by all SQL-level constructors
static const int64_t MIN_TS_MICROS = -9223372022400000000LL;

rc::Gen<timestamp_t> GenFiniteTimestamp() {
	return rc::gen::weightedOneOf<timestamp_t>(
	    {{4, rc::gen::map(rc::gen::inRange<int64_t>(MIN_TS_MICROS, NumericLimits<int64_t>::Maximum()),
	                      [](int64_t v) { return timestamp_t(v); })},
	     {4, rc::gen::map(rc::gen::inRange<int64_t>(-100000000000000000LL, 100000000000000000LL),
	                      [](int64_t v) { return timestamp_t(v); })},
	     {2, rc::gen::map(rc::gen::inRange<int64_t>(-2000000000, 2000000000),
	                      [](int64_t v) { return timestamp_t(v * 1000000); })},
	     {1, rc::gen::element<timestamp_t>(timestamp_t(0), timestamp_t(1), timestamp_t(-1), timestamp_t(MIN_TS_MICROS),
	                                       timestamp_t(MIN_TS_MICROS + 1),
	                                       timestamp_t(NumericLimits<int64_t>::Maximum() - 1),
	                                       timestamp_t(-62135596800000000LL),     // 0001-01-01
	                                       timestamp_t(-62167219200000000LL),     // 0000-01-01
	                                       timestamp_t(253402300799999999LL))}}); // 9999-12-31 23:59:59.999999
}

rc::Gen<timestamp_t> GenTimestamp() {
	return rc::gen::weightedOneOf<timestamp_t>(
	    {{15, GenFiniteTimestamp()},
	     {1, rc::gen::element<timestamp_t>(timestamp_t::infinity(), timestamp_t::ninfinity())}});
}

rc::Gen<interval_t> GenInterval() {
	return rc::gen::map(
	    rc::gen::tuple(
	        rc::gen::weightedOneOf<int32_t>(
	            {{3, rc::gen::inRange<int32_t>(-1000, 1000)}, {2, GenInt<int32_t>()}, {2, rc::gen::just<int32_t>(0)}}),
	        rc::gen::weightedOneOf<int32_t>(
	            {{3, rc::gen::inRange<int32_t>(-1000, 1000)}, {2, GenInt<int32_t>()}, {2, rc::gen::just<int32_t>(0)}}),
	        rc::gen::weightedOneOf<int64_t>(
	            {{3, rc::gen::inRange<int64_t>(-Interval::MICROS_PER_DAY * 2, Interval::MICROS_PER_DAY * 2)},
	             {2, GenInt<int64_t>()},
	             {2, rc::gen::just<int64_t>(0)}})),
	    [](const std::tuple<int32_t, int32_t, int64_t> &t) {
		    interval_t result;
		    result.months = std::get<0>(t);
		    result.days = std::get<1>(t);
		    result.micros = std::get<2>(t);
		    return result;
	    });
}

//===--------------------------------------------------------------------===//
// Types
//===--------------------------------------------------------------------===//
rc::Gen<LogicalType> GenIntegerType() {
	return rc::gen::element<LogicalType>(LogicalType::TINYINT, LogicalType::SMALLINT, LogicalType::INTEGER,
	                                     LogicalType::BIGINT, LogicalType::UTINYINT, LogicalType::USMALLINT,
	                                     LogicalType::UINTEGER, LogicalType::UBIGINT, LogicalType::HUGEINT,
	                                     LogicalType::UHUGEINT);
}

rc::Gen<LogicalType> GenDecimalType() {
	return rc::gen::exec([] {
		auto width = *rc::gen::weightedOneOf<int>(
		    {{6, rc::gen::inRange<int>(1, 39)}, {2, rc::gen::element<int>(1, 4, 5, 9, 10, 18, 19, 38)}});
		auto scale = *rc::gen::inRange<int>(0, width + 1);
		return LogicalType::DECIMAL(uint8_t(width), uint8_t(scale));
	});
}

rc::Gen<LogicalType> GenNumericType() {
	return rc::gen::weightedOneOf<LogicalType>(
	    {{5, GenIntegerType()},
	     {2, rc::gen::element<LogicalType>(LogicalType::FLOAT, LogicalType::DOUBLE)},
	     {2, GenDecimalType()}});
}

static rc::Gen<LogicalType> GenEnumType() {
	// NUL bytes excluded: an enum value containing chr(0) breaks the internal sort-key type round trip (FINDINGS.md #9)
	auto value_gen = rc::gen::suchThat(rc::gen::weightedOneOf<string>({{6, GenIdentifier()}, {2, GenUtf8String()}}),
	                                   [](const string &s) { return s.find('\0') == string::npos; });
	return rc::gen::map(rc::gen::nonEmpty(rc::gen::unique<vector<string>>(value_gen)),
	                    [](const vector<string> &values) {
		                    Vector vec(LogicalType::VARCHAR, values.size());
		                    auto data = FlatVector::GetDataMutable<string_t>(vec);
		                    for (idx_t i = 0; i < values.size(); i++) {
			                    data[i] = StringVector::AddString(vec, string_t(values[i]));
		                    }
		                    return LogicalType::ENUM(vec, values.size());
	                    });
}

rc::Gen<LogicalType> GenScalarType() {
	return rc::gen::weightedOneOf<LogicalType>(
	    {{10, GenNumericType()},
	     {3, rc::gen::just(LogicalType(LogicalType::VARCHAR))},
	     {6, rc::gen::element<LogicalType>(LogicalType::BOOLEAN, LogicalType::BLOB, LogicalType::BIT, LogicalType::UUID,
	                                       LogicalType::DATE, LogicalType::TIME, LogicalType::TIME_TZ,
	                                       LogicalType::TIME_NS, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP_S,
	                                       LogicalType::TIMESTAMP_MS, LogicalType::TIMESTAMP_NS,
	                                       LogicalType::TIMESTAMP_TZ, LogicalType::INTERVAL)},
	     {1, GenEnumType()}});
}

rc::Gen<LogicalType> GenSortableType(int max_depth) {
	if (max_depth <= 0) {
		return GenScalarType();
	}
	return rc::gen::weightedOneOf<LogicalType>(
	    {{6, GenScalarType()},
	     {1, rc::gen::map(GenSortableType(max_depth - 1),
	                      [](const LogicalType &child) { return LogicalType::LIST(child); })},
	     {1, rc::gen::exec([max_depth] {
		      auto n = *rc::gen::inRange<int>(1, 4);
		      child_list_t<LogicalType> children;
		      for (int i = 0; i < n; i++) {
			      children.emplace_back("f" + std::to_string(i), *GenSortableType(max_depth - 1));
		      }
		      return LogicalType::STRUCT(std::move(children));
	      })}});
}

static rc::Gen<string> GenFieldName() {
	return rc::gen::weightedOneOf<string>(
	    {{8, GenIdentifier()},
	     {1, rc::gen::element<string>("a b", "A", "x\"y", "k'", "NULL", "select", "é", "a,b", "a:b", "{a}", "a b c",
	                                  "1", "__", "a\\b")},
	     {1, rc::gen::nonEmpty(GenUtf8String())}});
}

rc::Gen<LogicalType> GenType(int max_depth) {
	if (max_depth <= 0) {
		return GenScalarType();
	}
	return rc::gen::weightedOneOf<LogicalType>(
	    {{8, GenScalarType()},
	     {2, rc::gen::map(GenType(max_depth - 1), [](const LogicalType &child) { return LogicalType::LIST(child); })},
	     {1, rc::gen::exec([max_depth] {
		      auto child = *GenType(max_depth - 1);
		      auto size = *rc::gen::inRange<int>(1, 5);
		      return LogicalType::ARRAY(child, optional_idx(size));
	      })},
	     {2, rc::gen::exec([max_depth] {
		      // struct field names are case-insensitive
		      auto names = *rc::gen::nonEmpty(rc::gen::uniqueBy<vector<string>>(GenFieldName(), StringUtil::Lower));
		      child_list_t<LogicalType> children;
		      for (auto &name : names) {
			      children.emplace_back(name, *GenType(max_depth - 1));
		      }
		      return LogicalType::STRUCT(std::move(children));
	      })},
	     {1, rc::gen::exec([max_depth] {
		      auto key = *GenType(max_depth - 1);
		      auto value = *GenType(max_depth - 1);
		      return LogicalType::MAP(key, value);
	      })},
	     {1, rc::gen::exec([max_depth] {
		      auto names = *rc::gen::nonEmpty(rc::gen::uniqueBy<vector<string>>(GenIdentifier(), StringUtil::Lower));
		      if (names.size() > 8) {
			      names.resize(8);
		      }
		      child_list_t<LogicalType> members;
		      for (auto &name : names) {
			      members.emplace_back(name, *GenType(max_depth - 1));
		      }
		      return LogicalType::UNION(std::move(members));
	      })}});
}

//===--------------------------------------------------------------------===//
// Values
//===--------------------------------------------------------------------===//
static Value DecimalValue(hugeint_t v, uint8_t width, uint8_t scale) {
	if (width <= Decimal::MAX_WIDTH_INT16) {
		return Value::DECIMAL(int16_t(v.lower), width, scale);
	} else if (width <= Decimal::MAX_WIDTH_INT32) {
		return Value::DECIMAL(int32_t(v.lower), width, scale);
	} else if (width <= Decimal::MAX_WIDTH_INT64) {
		return Value::DECIMAL(int64_t(v.lower), width, scale);
	} else {
		return Value::DECIMAL(v, width, scale);
	}
}

static hugeint_t PowerOfTen(int n) {
	hugeint_t result(1);
	for (int i = 0; i < n; i++) {
		result *= hugeint_t(10);
	}
	return result;
}

static rc::Gen<Value> GenDecimalValue(const LogicalType &type) {
	auto width = DecimalType::GetWidth(type);
	auto scale = DecimalType::GetScale(type);
	auto limit = PowerOfTen(width); // exclusive
	return rc::gen::exec([width, scale, limit] {
		auto kind = *rc::gen::inRange<int>(0, 10);
		hugeint_t v;
		if (kind < 5) {
			// uniform over the full range
			auto h = *GenHugeint();
			v = h % limit;
		} else if (kind < 8) {
			// small values
			v = hugeint_t(*rc::gen::inRange<int64_t>(-100000, 100000)) % limit;
		} else {
			// extremes
			v = *rc::gen::element<hugeint_t>(limit - 1, -(limit - 1), hugeint_t(0), hugeint_t(1), hugeint_t(-1),
			                                 limit / 2, -(limit / 2));
		}
		return DecimalValue(v, width, scale);
	});
}

rc::Gen<Value> GenNonNullValue(const LogicalType &type, const GenOptions &options) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) { return Value::BOOLEAN(b); });
	case LogicalTypeId::TINYINT:
		return rc::gen::map(GenInt<int8_t>(), [](int8_t v) { return Value::TINYINT(v); });
	case LogicalTypeId::SMALLINT:
		return rc::gen::map(GenInt<int16_t>(), [](int16_t v) { return Value::SMALLINT(v); });
	case LogicalTypeId::INTEGER:
		return rc::gen::map(GenInt<int32_t>(), [](int32_t v) { return Value::INTEGER(v); });
	case LogicalTypeId::BIGINT:
		return rc::gen::map(GenInt<int64_t>(), [](int64_t v) { return Value::BIGINT(v); });
	case LogicalTypeId::UTINYINT:
		return rc::gen::map(GenInt<uint8_t>(), [](uint8_t v) { return Value::UTINYINT(v); });
	case LogicalTypeId::USMALLINT:
		return rc::gen::map(GenInt<uint16_t>(), [](uint16_t v) { return Value::USMALLINT(v); });
	case LogicalTypeId::UINTEGER:
		return rc::gen::map(GenInt<uint32_t>(), [](uint32_t v) { return Value::UINTEGER(v); });
	case LogicalTypeId::UBIGINT:
		return rc::gen::map(GenInt<uint64_t>(), [](uint64_t v) { return Value::UBIGINT(v); });
	case LogicalTypeId::HUGEINT:
		return rc::gen::map(GenHugeint(), [](hugeint_t v) { return Value::HUGEINT(v); });
	case LogicalTypeId::UHUGEINT:
		return rc::gen::map(GenUhugeint(), [](uhugeint_t v) { return Value::UHUGEINT(v); });
	case LogicalTypeId::FLOAT:
		return rc::gen::map(GenFloat(), [](float v) { return Value::FLOAT(v); });
	case LogicalTypeId::DOUBLE:
		return rc::gen::map(GenDouble(), [](double v) { return Value::DOUBLE(v); });
	case LogicalTypeId::DECIMAL:
		return GenDecimalValue(type);
	case LogicalTypeId::VARCHAR:
		return rc::gen::map(options.safe_strings ? GenSafeString() : GenUtf8String(),
		                    [](const string &s) { return Value(s); });
	case LogicalTypeId::BLOB:
		return rc::gen::map(options.safe_strings ? GenSafeString() : GenBytes(),
		                    [](const string &s) { return Value::BLOB(const_data_ptr_cast(s.data()), s.size()); });
	case LogicalTypeId::BIT:
		return rc::gen::map(rc::gen::nonEmpty(rc::gen::container<string>(rc::gen::element<char>('0', '1'))),
		                    [](const string &s) { return Value::BIT(s); });
	case LogicalTypeId::UUID:
		return rc::gen::map(GenHugeint(), [](hugeint_t v) { return Value::UUID(v); });
	case LogicalTypeId::DATE:
		return rc::gen::map(GenDate(), [](date_t d) { return Value::DATE(d); });
	case LogicalTypeId::TIME:
		return rc::gen::map(GenTime(), [](dtime_t t) { return Value::TIME(t); });
	case LogicalTypeId::TIME_NS:
		return rc::gen::map(
		    rc::gen::weightedOneOf<int64_t>(
		        {{6, rc::gen::inRange<int64_t>(0, Interval::NANOS_PER_DAY)},
		         {1, rc::gen::element<int64_t>(0, 1, Interval::NANOS_PER_DAY - 1, Interval::NANOS_PER_DAY)}}),
		    [](int64_t v) { return Value::TIME_NS(dtime_ns_t(v)); });
	case LogicalTypeId::TIME_TZ:
		return rc::gen::map(
		    rc::gen::tuple(
		        GenTime(),
		        rc::gen::weightedOneOf<int32_t>(
		            {{4, rc::gen::inRange<int32_t>(dtime_tz_t::MIN_OFFSET, dtime_tz_t::MAX_OFFSET + 1)},
		             {2, rc::gen::map(rc::gen::inRange<int32_t>(-15, 16), [](int32_t h) { return h * 3600; })},
		             {1, rc::gen::just<int32_t>(0)}})),
		    [](const std::tuple<dtime_t, int32_t> &t) {
			    return Value::TIMETZ(dtime_tz_t(std::get<0>(t), std::get<1>(t)));
		    });
	case LogicalTypeId::TIMESTAMP:
		return rc::gen::map(GenTimestamp(), [](timestamp_t t) { return Value::TIMESTAMP(t); });
	case LogicalTypeId::TIMESTAMP_TZ:
		return rc::gen::map(GenTimestamp(), [](timestamp_t t) { return Value::TIMESTAMPTZ(timestamp_tz_t(t.value)); });
	case LogicalTypeId::TIMESTAMP_SEC:
		return rc::gen::map(
		    rc::gen::weightedOneOf<int64_t>(
		        {{6, rc::gen::inRange<int64_t>(MIN_TS_MICROS / 1000000, NumericLimits<int64_t>::Maximum() / 1000000)},
		         {3, rc::gen::inRange<int64_t>(-100000000000LL, 100000000000LL)},
		         {1, rc::gen::element<int64_t>(0, 1, -1)}}),
		    [](int64_t v) { return Value::TIMESTAMPSEC(timestamp_sec_t(v)); });
	case LogicalTypeId::TIMESTAMP_MS:
		return rc::gen::map(
		    rc::gen::weightedOneOf<int64_t>(
		        {{6, rc::gen::inRange<int64_t>(MIN_TS_MICROS / 1000, NumericLimits<int64_t>::Maximum() / 1000)},
		         {3, rc::gen::inRange<int64_t>(-100000000000000LL, 100000000000000LL)},
		         {1, rc::gen::element<int64_t>(0, 1, -1)}}),
		    [](int64_t v) { return Value::TIMESTAMPMS(timestamp_ms_t(v)); });
	case LogicalTypeId::TIMESTAMP_NS:
		return rc::gen::map(rc::gen::weightedOneOf<int64_t>(
		                        {{6, rc::gen::inRange<int64_t>(NumericLimits<int64_t>::Minimum() + 1,
		                                                       NumericLimits<int64_t>::Maximum())},
		                         {3, rc::gen::inRange<int64_t>(-100000000000000000LL, 100000000000000000LL)},
		                         {1, rc::gen::element<int64_t>(0, 1, -1, NumericLimits<int64_t>::Maximum() - 1)}}),
		                    [](int64_t v) { return Value::TIMESTAMPNS(timestamp_ns_t(v)); });
	case LogicalTypeId::INTERVAL:
		return rc::gen::map(GenInterval(), [](interval_t i) { return Value::INTERVAL(i); });
	case LogicalTypeId::ENUM: {
		auto size = EnumType::GetSize(type);
		return rc::gen::map(rc::gen::inRange<uint64_t>(0, size),
		                    [type](uint64_t idx) { return Value::ENUM(idx, type); });
	}
	case LogicalTypeId::LIST: {
		auto child = ListType::GetChildType(type);
		return rc::gen::map(GenValues(child, options),
		                    [child](const vector<Value> &values) { return Value::LIST(child, values); });
	}
	case LogicalTypeId::ARRAY: {
		auto child = ArrayType::GetChildType(type);
		auto size = ArrayType::GetSize(type);
		return rc::gen::map(rc::gen::container<vector<Value>>(size, GenValue(child, options)),
		                    [child](const vector<Value> &values) { return Value::ARRAY(child, values); });
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructType::GetChildTypes(type);
		return rc::gen::exec([type, children, options] {
			vector<Value> values;
			for (auto &child : children) {
				values.push_back(*GenValue(child.second, options));
			}
			return Value::STRUCT(type, std::move(values));
		});
	}
	case LogicalTypeId::MAP: {
		auto key_type = MapType::KeyType(type);
		auto value_type = MapType::ValueType(type);
		return rc::gen::exec([key_type, value_type, options] {
			GenOptions key_options = options;
			key_options.null_probability = 0.0;
			auto raw_keys = *GenValues(key_type, key_options);
			vector<Value> keys;
			vector<Value> values;
			for (auto &key : raw_keys) {
				bool duplicate = false;
				for (auto &existing : keys) {
					if (Value::NotDistinctFrom(existing, key)) {
						duplicate = true;
						break;
					}
				}
				if (duplicate) {
					continue;
				}
				keys.push_back(key);
				values.push_back(*GenValue(value_type, options));
			}
			return Value::MAP(key_type, value_type, std::move(keys), std::move(values));
		});
	}
	case LogicalTypeId::UNION: {
		auto members = UnionType::CopyMemberTypes(type);
		return rc::gen::exec([members, options] {
			auto tag = *rc::gen::inRange<idx_t>(0, members.size());
			auto value = *GenValue(members[tag].second, options);
			return Value::UNION(members, uint8_t(tag), std::move(value));
		});
	}
	default:
		throw InternalException("GenNonNullValue: unsupported type " + type.ToString());
	}
}

rc::Gen<Value> GenValue(const LogicalType &type, const GenOptions &options) {
	if (options.null_probability <= 0.0) {
		return GenNonNullValue(type, options);
	}
	auto null_weight = std::size_t(options.null_probability * 100);
	auto value_weight = std::size_t(100 - null_weight);
	return rc::gen::weightedOneOf<Value>(
	    {{null_weight, rc::gen::just(Value(type))}, {value_weight, GenNonNullValue(type, options)}});
}

rc::Gen<Value> GenValue(const LogicalType &type, double null_probability) {
	return GenValue(type, GenOptions(null_probability));
}

rc::Gen<vector<Value>> GenValues(const LogicalType &type, const GenOptions &options) {
	return rc::gen::container<vector<Value>>(GenValue(type, options));
}

rc::Gen<vector<Value>> GenValues(const LogicalType &type, double null_probability) {
	return GenValues(type, GenOptions(null_probability));
}

} // namespace duckdb_prop

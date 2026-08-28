// Arithmetic properties: overflow detection and results checked against __int128 oracles.
#include "property_test.hpp"

using namespace duckdb_prop;

namespace {

//! Run a scalar binary operation, returning either the value or the error
struct OpResult {
	Value value;
	bool error = false;
	string error_message;
};

OpResult RunOp(PropDB &db, const string &expr, vector<Value> params) {
	OpResult result;
	auto res = db.Query("SELECT " + expr, std::move(params));
	if (res->HasError()) {
		result.error = true;
		result.error_message = res->GetError();
		return result;
	}
	result.value = res->Cast<MaterializedQueryResult>().GetValue(0, 0);
	return result;
}

//! Format an int128 decimal raw value at the given scale the way DuckDB renders decimals
Value DecimalToStringValue(__int128 raw, uint8_t scale) {
	bool neg = raw < 0;
	unsigned __int128 u = neg ? (unsigned __int128)(-raw) : (unsigned __int128)raw;
	string digits;
	while (u) {
		digits.insert(digits.begin(), char('0' + int(u % 10)));
		u /= 10;
	}
	if (digits.empty()) {
		digits = "0";
	}
	string result;
	if (scale > 0) {
		while (digits.size() < idx_t(scale) + 1) {
			digits.insert(digits.begin(), '0');
		}
		result = digits.substr(0, digits.size() - scale) + "." + digits.substr(digits.size() - scale);
	} else {
		result = digits;
	}
	if (neg && result.find_first_not_of("0.") != string::npos) {
		result = "-" + result;
	}
	return Value(result);
}

inline __int128 MakeInt128(int64_t upper, uint64_t lower) {
	return __int128((unsigned __int128)(uint64_t(upper)) << 64 | lower);
}

const __int128 HUGEINT_MIN128 =
    MakeInt128(NumericLimits<hugeint_t>::Minimum().upper, NumericLimits<hugeint_t>::Minimum().lower);

__int128 ToInt128(const Value &v) {
	if (v.type().id() == LogicalTypeId::HUGEINT) {
		auto h = v.GetValue<hugeint_t>();
		return MakeInt128(h.upper, h.lower);
	}
	if (v.type().id() == LogicalTypeId::UBIGINT) {
		return __int128(v.GetValue<uint64_t>());
	}
	return __int128(v.GetValue<int64_t>());
}

bool InRange(__int128 x, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return x >= -128 && x <= 127;
	case LogicalTypeId::SMALLINT:
		return x >= -32768 && x <= 32767;
	case LogicalTypeId::INTEGER:
		return x >= -2147483648LL && x <= 2147483647LL;
	case LogicalTypeId::BIGINT:
		return x >= __int128(NumericLimits<int64_t>::Minimum()) && x <= __int128(NumericLimits<int64_t>::Maximum());
	case LogicalTypeId::UTINYINT:
		return x >= 0 && x <= 255;
	case LogicalTypeId::USMALLINT:
		return x >= 0 && x <= 65535;
	case LogicalTypeId::UINTEGER:
		return x >= 0 && x <= 4294967295LL;
	case LogicalTypeId::UBIGINT:
		return x >= 0 && x <= __int128(NumericLimits<uint64_t>::Maximum());
	default:
		throw InternalException("InRange: unexpected type");
	}
}

Value MakeTyped(__int128 x, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return Value::TINYINT(int8_t(x));
	case LogicalTypeId::SMALLINT:
		return Value::SMALLINT(int16_t(x));
	case LogicalTypeId::INTEGER:
		return Value::INTEGER(int32_t(x));
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(int64_t(x));
	case LogicalTypeId::UTINYINT:
		return Value::UTINYINT(uint8_t(x));
	case LogicalTypeId::USMALLINT:
		return Value::USMALLINT(uint16_t(x));
	case LogicalTypeId::UINTEGER:
		return Value::UINTEGER(uint32_t(x));
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(uint64_t(x));
	default:
		throw InternalException("MakeTyped: unexpected type");
	}
}

} // namespace

TEST_CASE("Integer arithmetic overflow oracle", "[property][arithmetic]") {
	PropDB db;
	rc::prop("a op b errors iff the exact result is out of range", [&] {
		auto type = *rc::gen::elementOf(vector<LogicalType> {
		    LogicalType::TINYINT, LogicalType::SMALLINT, LogicalType::INTEGER, LogicalType::BIGINT,
		    LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::UBIGINT});
		auto a = *GenNonNullValue(type);
		auto b = *GenNonNullValue(type);
		auto op = *rc::gen::element<char>('+', '-', '*');
		RC_TAG(string(1, op) + " " + LogicalTypeIdToString(type.id()));

		__int128 x = ToInt128(a), y = ToInt128(b);
		__int128 exact = 0;
		bool int128_overflow;
		switch (op) {
		case '+':
			int128_overflow = __builtin_add_overflow(x, y, &exact);
			break;
		case '-':
			int128_overflow = __builtin_sub_overflow(x, y, &exact);
			break;
		default:
			int128_overflow = __builtin_mul_overflow(x, y, &exact);
			break;
		}
		// if even int128 overflows (UBIGINT * UBIGINT), the result is certainly out of range
		bool expect_error = int128_overflow || !InRange(exact, type);

		auto tn = type.ToString();
		auto result = RunOp(db, string("$1::") + tn + " " + op + " $2::" + tn, {a, b});
		if (expect_error != result.error) {
			RC_FAIL("overflow mismatch for " + a.ToString() + " " + op + " " + b.ToString() + " (" + type.ToString() +
			        "): expected " + (expect_error ? "error" : "success") + ", got " +
			        (result.error ? ("error: " + result.error_message) : ("value " + result.value.ToString())));
		}
		if (!result.error) {
			PROP_ASSERT_VALUES_EQUAL(result.value, MakeTyped(exact, type));
		}
	});

	rc::prop("integer division and modulo", [&] {
		auto type = *rc::gen::elementOf(vector<LogicalType> {LogicalType::TINYINT, LogicalType::SMALLINT,
		                                                     LogicalType::INTEGER, LogicalType::BIGINT});
		auto a = *GenNonNullValue(type);
		auto b = *GenNonNullValue(type);
		__int128 x = ToInt128(a), y = ToInt128(b);

		auto tn = type.ToString();
		// a // b: truncating division; division by zero yields NULL
		auto div_result = RunOp(db, "$1::" + tn + " // $2::" + tn, {a, b});
		auto mod_result = RunOp(db, "$1::" + tn + " % $2::" + tn, {a, b});
		if (y == 0) {
			if (div_result.error || !div_result.value.IsNull()) {
				RC_FAIL("x // 0 should be NULL, got " +
				        (div_result.error ? div_result.error_message : Describe(div_result.value)));
			}
			if (mod_result.error || !mod_result.value.IsNull()) {
				RC_FAIL("x % 0 should be NULL, got " +
				        (mod_result.error ? mod_result.error_message : Describe(mod_result.value)));
			}
		} else {
			__int128 q = x / y; // C++ truncating division
			__int128 r = x % y;
			bool expect_error = !InRange(q, type); // only INT_MIN // -1 overflows
			if (expect_error != div_result.error) {
				RC_FAIL("division overflow mismatch: " + a.ToString() + " // " + b.ToString() + " -> " +
				        (div_result.error ? div_result.error_message : div_result.value.ToString()));
			}
			if (!div_result.error) {
				PROP_ASSERT_VALUES_EQUAL(div_result.value, MakeTyped(q, type));
			}
			// KNOWN ISSUE: INT_MIN % -1 errors instead of returning 0
			bool mod_overflows = expect_error;
			if (mod_overflows != mod_result.error) {
				RC_FAIL("modulo mismatch: " + a.ToString() + " % " + b.ToString() + " -> " +
				        (mod_result.error ? mod_result.error_message : mod_result.value.ToString()));
			}
			if (!mod_result.error) {
				PROP_ASSERT_VALUES_EQUAL(mod_result.value, MakeTyped(r, type));
			}
		}
	});

	rc::prop("negation and abs", [&] {
		auto type = *rc::gen::elementOf(vector<LogicalType> {LogicalType::TINYINT, LogicalType::SMALLINT,
		                                                     LogicalType::INTEGER, LogicalType::BIGINT});
		auto a = *GenNonNullValue(type);
		__int128 x = ToInt128(a);
		auto neg = RunOp(db, "-($1::" + type.ToString() + ")", {a});
		bool expect_error = !InRange(-x, type);
		if (expect_error != neg.error) {
			RC_FAIL("negation mismatch for " + a.ToString() + ": got " +
			        (neg.error ? neg.error_message : neg.value.ToString()));
		}
		if (!neg.error) {
			PROP_ASSERT_VALUES_EQUAL(neg.value, MakeTyped(-x, type));
		}
		auto abs_r = RunOp(db, "abs($1::" + type.ToString() + ")", {a});
		__int128 ax = x < 0 ? -x : x;
		expect_error = !InRange(ax, type);
		if (expect_error != abs_r.error) {
			RC_FAIL("abs mismatch for " + a.ToString() + ": got " +
			        (abs_r.error ? abs_r.error_message : abs_r.value.ToString()));
		}
		if (!abs_r.error) {
			PROP_ASSERT_VALUES_EQUAL(abs_r.value, MakeTyped(ax, type));
		}
	});

	rc::prop("bitwise operations match C semantics", [&] {
		auto a = *GenInt<int64_t>();
		auto b = *GenInt<int64_t>();
		auto va = Value::BIGINT(a), vb = Value::BIGINT(b);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT $1 & $2", {va, vb}), Value::BIGINT(a & b));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT $1 | $2", {va, vb}), Value::BIGINT(a | b));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT xor($1, $2)", {va, vb}), Value::BIGINT(a ^ b));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT ~$1", {va}), Value::BIGINT(~a));
		int64_t pc = 0;
		uint64_t ua = uint64_t(a);
		while (ua) {
			pc += ua & 1;
			ua >>= 1;
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT bit_count($1)", {va}), Value::TINYINT(int8_t(pc)));
	});

	rc::prop("greatest/least/sign", [&] {
		auto a = *GenInt<int64_t>();
		auto b = *GenInt<int64_t>();
		auto c = *GenInt<int64_t>();
		auto va = Value::BIGINT(a), vb = Value::BIGINT(b), vc = Value::BIGINT(c);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT greatest($1, $2, $3)", {va, vb, vc}),
		                         Value::BIGINT(std::max(std::max(a, b), c)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT least($1, $2, $3)", {va, vb, vc}),
		                         Value::BIGINT(std::min(std::min(a, b), c)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT sign($1)", {va}), Value::TINYINT(a > 0 ? 1 : (a < 0 ? -1 : 0)));
	});
}

TEST_CASE("HUGEINT arithmetic oracle", "[property][arithmetic]") {
	PropDB db;
	rc::prop("hugeint add/sub/mul with builtin int128 overflow check", [&] {
		auto a = *GenHugeint();
		auto b = *GenHugeint();
		auto op = *rc::gen::element<char>('+', '-', '*');
		__int128 x = MakeInt128(a.upper, a.lower);
		__int128 y = MakeInt128(b.upper, b.lower);
		__int128 exact;
		bool overflow;
		switch (op) {
		case '+':
			overflow = __builtin_add_overflow(x, y, &exact);
			break;
		case '-':
			overflow = __builtin_sub_overflow(x, y, &exact);
			break;
		default:
			overflow = __builtin_mul_overflow(x, y, &exact);
			break;
		}
		// DuckDB reserves true INT128_MIN as an invalid HUGEINT value
		if (!overflow && exact < HUGEINT_MIN128) {
			overflow = true;
		}
		auto result = RunOp(db, string("$1 ") + op + " $2", {Value::HUGEINT(a), Value::HUGEINT(b)});
		if (overflow != result.error) {
			RC_FAIL("hugeint overflow mismatch: " + Value::HUGEINT(a).ToString() + " " + op + " " +
			        Value::HUGEINT(b).ToString() + " -> " +
			        (result.error ? result.error_message : result.value.ToString()));
		}
		if (!result.error) {
			PROP_ASSERT_VALUES_EQUAL(result.value, Value::HUGEINT(hugeint_t(int64_t(exact >> 64), uint64_t(exact))));
		}
	});

	rc::prop("hugeint div/mod", [&] {
		auto a = *GenHugeint();
		auto b = *GenHugeint();
		__int128 x = MakeInt128(a.upper, a.lower);
		__int128 y = MakeInt128(b.upper, b.lower);
		auto div_result = RunOp(db, "$1 // $2", {Value::HUGEINT(a), Value::HUGEINT(b)});
		auto mod_result = RunOp(db, "$1 % $2", {Value::HUGEINT(a), Value::HUGEINT(b)});
		if (y == 0) {
			if (div_result.error || !div_result.value.IsNull()) {
				RC_FAIL("hugeint // 0 should be NULL");
			}
			if (mod_result.error || !mod_result.value.IsNull()) {
				RC_FAIL("hugeint % 0 should be NULL");
			}
			return;
		}
		bool overflow = (y == -1) && (x == HUGEINT_MIN128);
		if (overflow != div_result.error) {
			RC_FAIL("hugeint division mismatch: " + Value::HUGEINT(a).ToString() + " // " +
			        Value::HUGEINT(b).ToString() + " -> " +
			        (div_result.error ? div_result.error_message : div_result.value.ToString()));
		}
		if (!div_result.error) {
			__int128 q = x / y;
			PROP_ASSERT_VALUES_EQUAL(div_result.value, Value::HUGEINT(hugeint_t(int64_t(q >> 64), uint64_t(q))));
		}
		if (!mod_result.error) {
			__int128 r = x % y;
			PROP_ASSERT_VALUES_EQUAL(mod_result.value, Value::HUGEINT(hugeint_t(int64_t(r >> 64), uint64_t(r))));
		} else if (!overflow) {
			RC_FAIL("hugeint modulo errored: " + mod_result.error_message);
		}
	});
}

TEST_CASE("DECIMAL arithmetic oracle", "[property][arithmetic]") {
	PropDB db;
	rc::prop("decimal addition/subtraction is exact", [&] {
		auto t1 = *GenDecimalType();
		auto t2 = *GenDecimalType();
		auto a = *GenNonNullValue(t1);
		auto b = *GenNonNullValue(t2);
		auto w1 = DecimalType::GetWidth(t1), s1 = DecimalType::GetScale(t1);
		auto w2 = DecimalType::GetWidth(t2), s2 = DecimalType::GetScale(t2);
		// result type: scale = max(s1, s2), width = max(w1 - s1, w2 - s2) + scale + 1 (capped at 38)
		auto rs = std::max(s1, s2);
		auto rw = std::max(w1 - s1, w2 - s2) + rs + 1;
		RC_PRE(rw <= 38);
		// compute exact result in int128 at the result scale
		auto raw = [](const Value &v) {
			__int128 r;
			switch (v.type().InternalType()) {
			case PhysicalType::INT16:
				r = v.GetValueUnsafe<int16_t>();
				break;
			case PhysicalType::INT32:
				r = v.GetValueUnsafe<int32_t>();
				break;
			case PhysicalType::INT64:
				r = v.GetValueUnsafe<int64_t>();
				break;
			default: {
				auto h = v.GetValueUnsafe<hugeint_t>();
				r = MakeInt128(h.upper, h.lower);
				break;
			}
			}
			return r;
		};
		__int128 xa = raw(a), xb = raw(b);
		for (auto i = s1; i < rs; i++) {
			xa *= 10;
		}
		for (auto i = s2; i < rs; i++) {
			xb *= 10;
		}
		auto sum = xa + xb;
		auto diff = xa - xb;
		auto add_result = RunOp(db, "$1 + $2", {a, b});
		auto sub_result = RunOp(db, "$1 - $2", {a, b});
		if (add_result.error) {
			RC_FAIL("decimal + errored: " + add_result.error_message);
		}
		if (sub_result.error) {
			RC_FAIL("decimal - errored: " + sub_result.error_message);
		}
		auto expected_type = LogicalType::DECIMAL(uint8_t(rw), rs);
		PROP_ASSERT_VALUES_EQUAL(add_result.value.DefaultCastAs(LogicalType::VARCHAR), DecimalToStringValue(sum, rs));
		PROP_ASSERT_VALUES_EQUAL(sub_result.value.DefaultCastAs(LogicalType::VARCHAR), DecimalToStringValue(diff, rs));
		if (add_result.value.type() != expected_type) {
			RC_FAIL("unexpected decimal + result type: " + add_result.value.type().ToString() + " expected " +
			        expected_type.ToString());
		}
	});
}

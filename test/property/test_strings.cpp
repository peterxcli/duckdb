// String function properties checked against simple reference implementations.
#include "property_test.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>

using namespace duckdb_prop;

namespace {

using Codepoints = vector<uint32_t>;

bool IsAscii(const string &s) {
	for (auto c : s) {
		if (uint8_t(c) >= 0x80) {
			return false;
		}
	}
	return true;
}

//! Reference LIKE implementation on code points: '_' matches one code point, '%' any sequence,
//! escape followed by a code point matches that code point literally
bool LikeOracle(const Codepoints &s, const Codepoints &p, int64_t escape, idx_t si = 0, idx_t pi = 0) {
	while (pi < p.size()) {
		auto pc = p[pi];
		if (escape >= 0 && pc == uint32_t(escape)) {
			pi++;
			if (pi >= p.size()) {
				throw std::runtime_error("pattern ends with escape");
			}
			if (si >= s.size() || s[si] != p[pi]) {
				return false;
			}
			si++;
			pi++;
		} else if (pc == '%') {
			// collapse consecutive %
			while (pi < p.size() && p[pi] == '%') {
				pi++;
			}
			if (pi == p.size()) {
				return true;
			}
			for (idx_t k = si; k <= s.size(); k++) {
				if (LikeOracle(s, p, escape, k, pi)) {
					return true;
				}
			}
			return false;
		} else if (pc == '_') {
			if (si >= s.size()) {
				return false;
			}
			si++;
			pi++;
		} else {
			if (si >= s.size() || s[si] != pc) {
				return false;
			}
			si++;
			pi++;
		}
	}
	return si == s.size();
}

Codepoints LowerAscii(Codepoints cps) {
	for (auto &c : cps) {
		if (c >= 'A' && c <= 'Z') {
			c += 32;
		}
	}
	return cps;
}

//! Generator for LIKE patterns: strings with a higher density of wildcards
rc::Gen<string> GenLikePattern() {
	return rc::gen::map(rc::gen::container<vector<uint32_t>>(rc::gen::weightedOneOf<uint32_t>(
	                        {{3, Elements<uint32_t>({'%', '_'})},
	                         {2, Elements<uint32_t>({'\\', '$', '#'})},
	                         {6, rc::gen::inRange<uint32_t>('a', 'd' + 1)},
	                         {1, rc::gen::inRange<uint32_t>('A', 'D' + 1)},
	                         {1, rc::gen::inRange<uint32_t>(0x80, 0x800)},
	                         {1, Elements<uint32_t>({0x4E2D, 0x1F600, 0xE9, 0x0130, 0x00DF})}})),
	                    FromCodepoints);
}

//! Generator for strings to match against LIKE patterns (same alphabet)
rc::Gen<string> GenLikeSubject() {
	return rc::gen::map(rc::gen::container<vector<uint32_t>>(rc::gen::weightedOneOf<uint32_t>(
	                        {{1, Elements<uint32_t>({'%', '_'})},
	                         {1, Elements<uint32_t>({'\\', '$', '#'})},
	                         {8, rc::gen::inRange<uint32_t>('a', 'd' + 1)},
	                         {1, rc::gen::inRange<uint32_t>('A', 'D' + 1)},
	                         {1, rc::gen::inRange<uint32_t>(0x80, 0x800)},
	                         {1, Elements<uint32_t>({0x4E2D, 0x1F600, 0xE9, 0x0130, 0x00DF})}})),
	                    FromCodepoints);
}

string SqlString(const string &s) {
	return Value(s).ToSQLString();
}

idx_t Levenshtein(const Codepoints &a, const Codepoints &b) {
	vector<idx_t> prev(b.size() + 1), cur(b.size() + 1);
	for (idx_t j = 0; j <= b.size(); j++) {
		prev[j] = j;
	}
	for (idx_t i = 1; i <= a.size(); i++) {
		cur[0] = i;
		for (idx_t j = 1; j <= b.size(); j++) {
			idx_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
			cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
		}
		std::swap(prev, cur);
	}
	return prev[b.size()];
}

idx_t DamerauLevenshtein(const Codepoints &a, const Codepoints &b) {
	// true (unrestricted) Damerau-Levenshtein distance, which is what DuckDB implements
	idx_t n = a.size(), m = b.size();
	idx_t inf = n + m;
	std::map<uint32_t, idx_t> last_row;
	vector<vector<idx_t>> d(n + 2, vector<idx_t>(m + 2, 0));
	d[0][0] = inf;
	for (idx_t i = 0; i <= n; i++) {
		d[i + 1][0] = inf;
		d[i + 1][1] = i;
	}
	for (idx_t j = 0; j <= m; j++) {
		d[0][j + 1] = inf;
		d[1][j + 1] = j;
	}
	for (idx_t i = 1; i <= n; i++) {
		idx_t last_col = 0;
		for (idx_t j = 1; j <= m; j++) {
			idx_t i2 = last_row.count(b[j - 1]) ? last_row[b[j - 1]] : 0;
			idx_t j2 = last_col;
			idx_t cost = 0;
			if (a[i - 1] == b[j - 1]) {
				last_col = j;
			} else {
				cost = 1;
			}
			d[i + 1][j + 1] = std::min(std::min(d[i][j] + cost, std::min(d[i + 1][j] + 1, d[i][j + 1] + 1)),
			                           d[i2][j2] + (i - i2 - 1) + 1 + (j - j2 - 1));
		}
		last_row[a[i - 1]] = i;
	}
	return d[n + 1][m + 1];
}

vector<string> SplitOracle(const string &s, const string &sep) {
	vector<string> result;
	idx_t start = 0;
	while (true) {
		auto pos = s.find(sep, start);
		if (pos == string::npos) {
			result.push_back(s.substr(start));
			break;
		}
		result.push_back(s.substr(start, pos - start));
		start = pos + sep.size();
	}
	return result;
}

string ReplaceOracle(const string &s, const string &from, const string &to) {
	if (from.empty()) {
		return s;
	}
	string result;
	idx_t start = 0;
	while (true) {
		auto pos = s.find(from, start);
		if (pos == string::npos) {
			result += s.substr(start);
			break;
		}
		result += s.substr(start, pos - start) + to;
		start = pos + from.size();
	}
	return result;
}

Value ListOfStrings(const vector<string> &strings) {
	vector<Value> values;
	for (auto &s : strings) {
		values.emplace_back(s);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

} // namespace

TEST_CASE("LIKE matches the reference implementation", "[property][strings]") {
	PropDB db;
	rc::prop("s LIKE p (parameterized and constant pattern)", [&] {
		auto s = *GenLikeSubject();
		auto p = *GenLikePattern();
		auto expected = LikeOracle(ToCodepoints(s), ToCodepoints(p), -1);
		RC_CLASSIFY(expected, "match");
		// parameterized (generic vectorized path)
		auto actual = db.Scalar("SELECT $1 LIKE $2", {Value(s), Value(p)});
		PROP_ASSERT_VALUES_EQUAL(actual, Value::BOOLEAN(expected));
		// constant pattern (optimizer may rewrite to prefix/suffix/contains)
		auto actual_const = db.Scalar("SELECT $1 LIKE " + SqlString(p), {Value(s)});
		PROP_ASSERT_VALUES_EQUAL(actual_const, Value::BOOLEAN(expected));
		// NOT LIKE
		auto actual_not = db.Scalar("SELECT $1 NOT LIKE " + SqlString(p), {Value(s)});
		PROP_ASSERT_VALUES_EQUAL(actual_not, Value::BOOLEAN(!expected));
	});
	rc::prop("s LIKE p ESCAPE e", [&] {
		auto s = *GenLikeSubject();
		auto p = *GenLikePattern();
		auto escape = *rc::gen::element<char>('\\', '$', '#');
		auto pcps = ToCodepoints(p);
		// DuckDB errors when the pattern ends with the escape character
		// (RC_PRE uses expression decomposition which breaks || short-circuiting, so compute the bool first)
		bool valid_pattern = pcps.empty() || pcps.back() != uint32_t(escape);
		RC_PRE(valid_pattern);
		auto expected = LikeOracle(ToCodepoints(s), pcps, escape);
		RC_CLASSIFY(expected, "match");
		auto esc = string(1, escape);
		auto actual = db.Scalar("SELECT $1 LIKE $2 ESCAPE $3", {Value(s), Value(p), Value(esc)});
		PROP_ASSERT_VALUES_EQUAL(actual, Value::BOOLEAN(expected));
		auto actual_const = db.Scalar("SELECT $1 LIKE " + SqlString(p) + " ESCAPE " + SqlString(esc), {Value(s)});
		PROP_ASSERT_VALUES_EQUAL(actual_const, Value::BOOLEAN(expected));
		auto actual_fun = db.Scalar("SELECT like_escape($1, $2, $3)", {Value(s), Value(p), Value(esc)});
		PROP_ASSERT_VALUES_EQUAL(actual_fun, Value::BOOLEAN(expected));
	});
	rc::prop("s ILIKE p (ASCII)", [&] {
		auto s = *rc::gen::suchThat(GenLikeSubject(), IsAscii);
		auto p = *rc::gen::suchThat(GenLikePattern(), IsAscii);
		auto escape = *rc::gen::element<int>(-1, '\\', '$', '#');
		auto pcps = ToCodepoints(p);
		bool valid_pattern = pcps.empty() || escape < 0 || pcps.back() != uint32_t(escape);
		RC_PRE(valid_pattern);
		auto expected = LikeOracle(LowerAscii(ToCodepoints(s)), LowerAscii(pcps), escape);
		RC_CLASSIFY(expected, "match");
		string escape_clause = escape < 0 ? "" : " ESCAPE " + SqlString(string(1, char(escape)));
		auto actual_const = db.Scalar("SELECT $1 ILIKE " + SqlString(p) + escape_clause, {Value(s)});
		PROP_ASSERT_VALUES_EQUAL(actual_const, Value::BOOLEAN(expected));
		if (escape < 0) {
			auto actual = db.Scalar("SELECT $1 ILIKE $2", {Value(s), Value(p)});
			PROP_ASSERT_VALUES_EQUAL(actual, Value::BOOLEAN(expected));
		} else {
			auto actual =
			    db.Scalar("SELECT $1 ILIKE $2 ESCAPE $3", {Value(s), Value(p), Value(string(1, char(escape)))});
			PROP_ASSERT_VALUES_EQUAL(actual, Value::BOOLEAN(expected));
			auto actual_fun =
			    db.Scalar("SELECT ilike_escape($1, $2, $3)", {Value(s), Value(p), Value(string(1, char(escape)))});
			PROP_ASSERT_VALUES_EQUAL(actual_fun, Value::BOOLEAN(expected));
		}
	});
}

TEST_CASE("String functions match reference implementations", "[property][strings]") {
	PropDB db;
	rc::prop("length/strlen/substring/left/right", [&] {
		auto s = *GenUtf8String();
		auto cps = ToCodepoints(s);
		auto n = int64_t(cps.size());
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT length($1)", {Value(s)}), Value::BIGINT(n));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT strlen($1)", {Value(s)}), Value::BIGINT(int64_t(s.size())));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT octet_length(encode($1))", {Value(s)}),
		                         Value::BIGINT(int64_t(s.size())));

		auto start = *rc::gen::inRange<int64_t>(-n - 3, n + 4);
		auto len = *rc::gen::inRange<int64_t>(-n - 3, n + 4);
		// substring(s, start, len): 1-based positions; negative start counts from the end (-1 = last char);
		// negative len takes the |len| characters before start
		int64_t eff = start < 0 ? n + start + 1 : start;
		int64_t lo = len >= 0 ? eff : eff + len;
		int64_t hi = len >= 0 ? eff + len : eff;
		Codepoints expected;
		for (int64_t pos = lo; pos < hi; pos++) {
			if (pos >= 1 && pos <= n) {
				expected.push_back(cps[pos - 1]);
			}
		}
		auto actual = db.Scalar("SELECT substring($1, $2, $3)", {Value(s), Value::BIGINT(start), Value::BIGINT(len)});
		PROP_ASSERT_VALUES_EQUAL(actual, Value(FromCodepoints(expected)));

		auto k = *rc::gen::inRange<int64_t>(-n - 2, n + 3);
		// left(s, k): first k chars; negative k: all but the last |k| chars
		Codepoints left_expected, right_expected;
		int64_t left_count = k >= 0 ? std::min(k, n) : std::max<int64_t>(0, n + k);
		int64_t right_count = k >= 0 ? std::min(k, n) : std::max<int64_t>(0, n + k);
		left_expected.assign(cps.begin(), cps.begin() + left_count);
		right_expected.assign(cps.end() - right_count, cps.end());
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT left($1, $2)", {Value(s), Value::BIGINT(k)}),
		                         Value(FromCodepoints(left_expected)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT right($1, $2)", {Value(s), Value::BIGINT(k)}),
		                         Value(FromCodepoints(right_expected)));
	});

	rc::prop("strpos/contains/starts_with/ends_with/replace/split", [&] {
		auto s = *GenUtf8String();
		// sub is either a random string or an actual substring of s
		auto sub = *rc::gen::oneOf(GenUtf8String(), rc::gen::exec([&] {
			                           auto cps = ToCodepoints(s);
			                           auto a = *rc::gen::inRange<idx_t>(0, cps.size() + 1);
			                           auto b = *rc::gen::inRange<idx_t>(a, cps.size() + 1);
			                           return FromCodepoints(Codepoints(cps.begin() + a, cps.begin() + b));
		                           }));
		auto cps = ToCodepoints(s);
		auto pos = s.find(sub);
		RC_CLASSIFY(pos != string::npos, "found");
		// strpos is 1-based and counts characters
		int64_t expected_pos = 0;
		if (pos != string::npos) {
			expected_pos = int64_t(ToCodepoints(s.substr(0, pos)).size()) + 1;
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT strpos($1, $2)", {Value(s), Value(sub)}),
		                         Value::BIGINT(expected_pos));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT instr($1, $2)", {Value(s), Value(sub)}),
		                         Value::BIGINT(expected_pos));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT position($2 IN $1)", {Value(s), Value(sub)}),
		                         Value::BIGINT(expected_pos));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT contains($1, $2)", {Value(s), Value(sub)}),
		                         Value::BOOLEAN(pos != string::npos));
		// constant-folded / optimized variants
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT contains($1, " + SqlString(sub) + ")", {Value(s)}),
		                         Value::BOOLEAN(pos != string::npos));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT starts_with($1, $2)", {Value(s), Value(sub)}),
		                         Value::BOOLEAN(s.compare(0, sub.size(), sub) == 0));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT prefix($1, $2)", {Value(s), Value(sub)}),
		                         Value::BOOLEAN(s.compare(0, sub.size(), sub) == 0));
		bool ends = s.size() >= sub.size() && s.compare(s.size() - sub.size(), sub.size(), sub) == 0;
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT ends_with($1, $2)", {Value(s), Value(sub)}), Value::BOOLEAN(ends));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT suffix($1, $2)", {Value(s), Value(sub)}), Value::BOOLEAN(ends));

		auto to = *GenUtf8String();
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT replace($1, $2, $3)", {Value(s), Value(sub), Value(to)}),
		                         Value(ReplaceOracle(s, sub, to)));

		if (!sub.empty()) {
			auto parts = SplitOracle(s, sub);
			PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT string_split($1, $2)", {Value(s), Value(sub)}),
			                         ListOfStrings(parts));
			PROP_ASSERT_VALUES_EQUAL(
			    db.Scalar("SELECT array_to_string(string_split($1, $2), $2)", {Value(s), Value(sub)}), Value(s));
			auto idx = *rc::gen::inRange<int64_t>(-int64_t(parts.size()) - 1, int64_t(parts.size()) + 2);
			string expected_part;
			if (idx > 0 && idx <= int64_t(parts.size())) {
				expected_part = parts[idx - 1];
			} else if (idx < 0 && -idx <= int64_t(parts.size())) {
				expected_part = parts[parts.size() + idx];
			}
			PROP_ASSERT_VALUES_EQUAL(
			    db.Scalar("SELECT split_part($1, $2, $3)", {Value(s), Value(sub), Value::BIGINT(idx)}),
			    Value(expected_part));
		}
	});

	rc::prop("repeat/reverse/upper/lower/lpad/rpad/translate", [&] {
		auto s = *GenUtf8String();
		auto cps = ToCodepoints(s);
		auto n = *rc::gen::inRange<int64_t>(-3, 6);
		string repeated;
		for (int64_t i = 0; i < n; i++) {
			repeated += s;
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT repeat($1, $2)", {Value(s), Value::BIGINT(n)}), Value(repeated));

		auto ascii = *GenAsciiString();
		string upper = ascii, lower = ascii;
		for (auto &c : upper) {
			c = char(toupper(c));
		}
		for (auto &c : lower) {
			c = char(tolower(c));
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT upper($1)", {Value(ascii)}), Value(upper));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT lower($1)", {Value(ascii)}), Value(lower));
		// reverse on ASCII (grapheme clusters == code points, except CRLF)
		RC_PRE(ascii.find("\r\n") == string::npos);
		auto reversed = ascii;
		std::reverse(reversed.begin(), reversed.end());
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT reverse($1)", {Value(ascii)}), Value(reversed));

		auto width = *rc::gen::inRange<int64_t>(-2, int64_t(cps.size()) + 5);
		auto fill = *rc::gen::suchThat(GenUtf8String(), [](const string &f) { return !f.empty(); });
		auto fill_cps = ToCodepoints(fill);
		Codepoints lpad, rpad;
		if (width <= int64_t(cps.size())) {
			auto cnt = std::max<int64_t>(0, width);
			lpad.assign(cps.begin(), cps.begin() + cnt);
			rpad = lpad;
		} else {
			idx_t missing = idx_t(width) - cps.size();
			Codepoints padding;
			while (padding.size() < missing) {
				for (auto c : fill_cps) {
					if (padding.size() >= missing) {
						break;
					}
					padding.push_back(c);
				}
			}
			lpad = padding;
			lpad.insert(lpad.end(), cps.begin(), cps.end());
			rpad = cps;
			rpad.insert(rpad.end(), padding.begin(), padding.end());
		}
		PROP_ASSERT_VALUES_EQUAL(
		    db.Scalar("SELECT lpad($1, $2, $3)", {Value(s), Value::INTEGER(int32_t(width)), Value(fill)}),
		    Value(FromCodepoints(lpad)));
		PROP_ASSERT_VALUES_EQUAL(
		    db.Scalar("SELECT rpad($1, $2, $3)", {Value(s), Value::INTEGER(int32_t(width)), Value(fill)}),
		    Value(FromCodepoints(rpad)));

		// translate(s, from, to): replace each char in from with the char at the same position in to (or remove)
		auto from = *GenUtf8String();
		auto to = *GenUtf8String();
		auto from_cps = ToCodepoints(from), to_cps = ToCodepoints(to);
		Codepoints translated;
		for (auto c : cps) {
			idx_t i = 0;
			for (; i < from_cps.size(); i++) {
				if (from_cps[i] == c) {
					break;
				}
			}
			if (i == from_cps.size()) {
				translated.push_back(c);
			} else if (i < to_cps.size()) {
				translated.push_back(to_cps[i]);
			}
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT translate($1, $2, $3)", {Value(s), Value(from), Value(to)}),
		                         Value(FromCodepoints(translated)));
	});

	rc::prop("trim/ltrim/rtrim with characters", [&] {
		auto s = *GenUtf8String();
		auto chars = *GenUtf8String();
		auto cps = ToCodepoints(s);
		auto set = ToCodepoints(chars);
		auto in_set = [&](uint32_t c) {
			return std::find(set.begin(), set.end(), c) != set.end();
		};
		idx_t l = 0, r = cps.size();
		while (l < r && in_set(cps[l])) {
			l++;
		}
		while (r > l && in_set(cps[r - 1])) {
			r--;
		}
		idx_t r2 = cps.size();
		while (r2 > 0 && in_set(cps[r2 - 1])) {
			r2--;
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT trim($1, $2)", {Value(s), Value(chars)}),
		                         Value(FromCodepoints(Codepoints(cps.begin() + l, cps.begin() + r))));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT ltrim($1, $2)", {Value(s), Value(chars)}),
		                         Value(FromCodepoints(Codepoints(cps.begin() + l, cps.end()))));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT rtrim($1, $2)", {Value(s), Value(chars)}),
		                         Value(FromCodepoints(Codepoints(cps.begin(), cps.begin() + r2))));
	});

	rc::prop("levenshtein/damerau_levenshtein/hamming", [&] {
		auto a = *GenUtf8String();
		auto b =
		    *rc::gen::oneOf(GenUtf8String(), rc::gen::map(GenUtf8String(), [&](const string &x) { return a + x; }));
		// NOTE: these functions operate on bytes, not characters (see FINDINGS.md)
		Codepoints acps(a.begin(), a.end()), bcps(b.begin(), b.end());
		RC_PRE(acps.size() < 200 && bcps.size() < 200);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT levenshtein($1, $2)", {Value(a), Value(b)}),
		                         Value::BIGINT(int64_t(Levenshtein(acps, bcps))));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT damerau_levenshtein($1, $2)", {Value(a), Value(b)}),
		                         Value::BIGINT(int64_t(DamerauLevenshtein(acps, bcps))));
		// hamming() rejects empty strings
		if (acps.size() == bcps.size() && !acps.empty()) {
			int64_t dist = 0;
			for (idx_t i = 0; i < acps.size(); i++) {
				dist += acps[i] != bcps[i];
			}
			PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT hamming($1, $2)", {Value(a), Value(b)}), Value::BIGINT(dist));
		}
	});

	rc::prop("encoding round trips: hex/base64/url/encode/nfc", [&] {
		auto bytes = *GenBytes();
		auto blob = Value::BLOB(const_data_ptr_cast(bytes.data()), bytes.size());
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT unhex(hex($1))", {blob}), blob);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT from_base64(base64($1))", {blob}), blob);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT from_base64(to_base64($1))", {blob}), blob);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT $1::BLOB::VARCHAR::BLOB", {blob}), blob);
		auto s = *GenUtf8String();
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT decode(encode($1))", {Value(s)}), Value(s));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT url_decode(url_encode($1))", {Value(s)}), Value(s));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT nfc_normalize(nfc_normalize($1)) = nfc_normalize($1)", {Value(s)}),
		                         Value::BOOLEAN(true));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT format('{}', $1)", {Value(s)}), Value(s));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT printf('%s', $1)", {Value(s)}), Value(s));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT concat($1, $2)", {Value(s), Value(bytes.empty() ? "" : "x")}),
		                         Value(s + (bytes.empty() ? "" : "x")));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT $1 || $2", {Value(s), Value(s)}), Value(s + s));
	});

	rc::prop("chr/ascii/unicode/ord", [&] {
		auto cp = *rc::gen::weightedOneOf<uint32_t>({{3, rc::gen::inRange<uint32_t>(1, 0x80)},
		                                             {3, rc::gen::inRange<uint32_t>(0x80, 0xD800)},
		                                             {2, rc::gen::inRange<uint32_t>(0xE000, 0x110000)}});
		auto s = FromCodepoints({cp});
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT chr($1)", {Value::INTEGER(int32_t(cp))}), Value(s));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT unicode($1)", {Value(s)}), Value::INTEGER(int32_t(cp)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT ord($1)", {Value(s)}), Value::INTEGER(int32_t(cp)));
		auto tail = *GenUtf8String();
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT unicode($1)", {Value(s + tail)}), Value::INTEGER(int32_t(cp)));
		auto ascii = *GenAsciiString();
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT ascii($1)", {Value(ascii)}),
		                         Value::INTEGER(ascii.empty() ? 0 : int32_t(uint8_t(ascii[0]))));
	});

	rc::prop("numeric formatting: format/printf/to_base/bin/hex", [&] {
		auto v = *GenInt<int64_t>();
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT format('{}', $1)", {Value::BIGINT(v)}), Value(std::to_string(v)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT format('{:d}', $1)", {Value::BIGINT(v)}), Value(std::to_string(v)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT printf('%d', $1)", {Value::BIGINT(v)}), Value(std::to_string(v)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT printf('%lld', $1)", {Value::BIGINT(v)}), Value(std::to_string(v)));
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT $1::VARCHAR", {Value::BIGINT(v)}), Value(std::to_string(v)));
		char buf[64];
		snprintf(buf, sizeof(buf), "%llX", (unsigned long long)v);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT hex($1)", {Value::BIGINT(v)}), Value(string(buf)));
		snprintf(buf, sizeof(buf), "%llx", (unsigned long long)v);
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT printf('%x', $1)", {Value::BIGINT(v)}), Value(string(buf)));
		// bin
		string bin;
		uint64_t u = uint64_t(v);
		if (u == 0) {
			bin = "0";
		}
		while (u) {
			bin.insert(bin.begin(), char('0' + (u & 1)));
			u >>= 1;
		}
		PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT bin($1)", {Value::BIGINT(v)}), Value(bin));
		// to_base for non-negative values
		auto base = *rc::gen::inRange<int32_t>(2, 37);
		if (v >= 0) {
			string digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
			string expected;
			uint64_t x = uint64_t(v);
			if (x == 0) {
				expected = "0";
			}
			while (x) {
				expected.insert(expected.begin(), digits[x % base]);
				x /= base;
			}
			PROP_ASSERT_VALUES_EQUAL(db.Scalar("SELECT to_base($1, $2)", {Value::BIGINT(v), Value::INTEGER(base)}),
			                         Value(expected));
		}
	});
}

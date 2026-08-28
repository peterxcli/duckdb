# Property-based tests (RapidCheck)

Property-based tests for DuckDB using [RapidCheck](https://github.com/emil-e/rapidcheck), in the spirit of
Rust's `proptest` (compare [apache/arrow-rs#10352](https://github.com/apache/arrow-rs/pull/10352)).

Instead of fixed inputs, each test states a *property* ("any value must survive a cast to VARCHAR and back",
"`LIKE` must agree with a 20-line reference matcher", "integer arithmetic must error exactly when the `__int128`
result is out of range") and RapidCheck runs it against hundreds of randomly generated inputs, shrinking failures
to minimal counterexamples.

## Building

This is a standalone CMake project that links against an existing DuckDB build directory, so iterating on tests
does not require rebuilding DuckDB:

```bash
# build DuckDB with assertions + sanitizers (recommended for bug hunting)
GEN=ninja CORE_EXTENSIONS='json' make relassert

# configure + build the property tests
cmake -S test/property -B build/property -G Ninja -DDUCKDB_BUILD_DIR=$PWD/build/relassert
cmake --build build/property
```

RapidCheck is fetched via CMake `FetchContent` by default; pass `-DRAPIDCHECK_SOURCE_DIR=/path/to/rapidcheck`
to use a local checkout. When linking a sanitized DuckDB build the tests are built with
`-fsanitize=address,undefined` as well (`-DPROPERTY_TEST_SANITIZE=OFF` to disable).

## Running

The binary is a regular Catch2 test runner; RapidCheck is configured through the `RC_PARAMS` environment variable:

```bash
# run everything (100 cases per property by default)
build/property/property_test "[property]"

# more iterations, bounded value sizes
RC_PARAMS="max_success=1000 max_size=50" build/property/property_test "[property]"

# one suite
build/property/property_test "[strings]"

# reproduce a failure deterministically
RC_PARAMS="seed=12345" build/property/property_test "VARCHAR cast round trip"

# disable shrinking (useful for a fast first survey; shrinking large nested values can be slow)
RC_PARAMS="max_success=1000 noshrink=1" build/property/property_test "[property]"
```

Set `PROPERTY_TEST_TMP` to control where `[storage]` writes its temporary database files (default `/tmp`).

## Layout

| file | contents |
|---|---|
| `include/property_test.hpp` | `PropDB` (in-memory/persistent connection helpers), value comparison, assertion macros, generator declarations |
| `generators.cpp` | random `LogicalType`s (incl. nested STRUCT/LIST/ARRAY/MAP/UNION/ENUM) and random `Value`s for any type, plus UTF-8 string, numeric and temporal generators with adversarial special values |
| `test_roundtrip.cpp` | value → VARCHAR/SQL-literal/JSON/table → value round trips |
| `test_strings.cpp` | LIKE/ILIKE vs a reference matcher; substring/split/pad/trim/translate/distance/encoding functions vs oracles |
| `test_lists.cpp` | list_sort/contains/position/distinct/slice/resize/concat/aggregates, range/generate_series vs closed forms |
| `test_arithmetic.cpp` | integer/HUGEINT/DECIMAL arithmetic vs `__int128` oracles (overflow must error exactly when out of range) |
| `test_datetime.cpp` | date parts vs independent civil-calendar algorithms, epoch round trips, strftime/strptime |
| `test_storage.cpp` | persistent DB round trip across `force_compression` settings, incl. update/delete + checkpoint + reopen |

## Known issues found by these tests

Failures caused by already-identified DuckDB bugs are skipped with `RC_PRE(...)` guards marked `KNOWN ISSUE`,
so the properties keep hunting for new bugs. See `FINDINGS.md` for the list of bugs these tests have found,
with minimal reproductions.

## Writing a new property

```cpp
TEST_CASE("my property", "[property][mytag]") {
	PropDB db;
	rc::prop("what must hold", [&] {
		auto type = *GenType(2);              // random type, nested up to depth 2
		auto v = *GenValue(type, 0.1);        // random value, 10% NULLs
		auto out = db.Scalar("SELECT ...", {v});
		PROP_ASSERT_VALUES_EQUAL(out, v);
	});
}
```

Gotchas:
- `RC_PRE`/`RC_ASSERT` use expression decomposition, which silences `||`/`&&` short-circuiting —
  compute the condition into a `bool` first.
- prefer an *independent oracle* (a small reference implementation) over comparing DuckDB with itself;
  where that is impossible, comparing two DuckDB code paths (constant vs parameterized, stored vs computed)
  still finds inconsistencies.
- `duckdb::vector` (pulled in by `using namespace duckdb`) bounds-checks `back()`/`operator[]` and throws
  `InternalException` — do not index blindly in test code.

# Bugs and quirks found by the property tests

Found against `v1.6.0-dev13151` (relassert build, macOS arm64). Ordered roughly by severity.
Each entry that the tests must work around is referenced by a `KNOWN ISSUE` comment in the test code.

## 1. `last_day` at the maximum date raises an INTERNAL error (invalidates the database)

```sql
SELECT last_day(DATE '5881580-07-10');
-- INTERNAL Error: Scalar function ""last_day"" threw an execution error,
-- but the function is not marked as fallible - the function must call SetFallible().
-- Error: Date out of range: 5881580-8-1
```

`last_day` computes first-of-next-month, which is out of range in the maximum month. Because the function is
not marked fallible, the recoverable conversion error escalates to an `InternalException`, which invalidates
the whole database. Fix: mark the function fallible (or compute the result without materializing the
out-of-range date).

## 2. `date_trunc('week', ...)` near the minimum date: signed integer overflow (UB)

```sql
SELECT date_trunc('week', DATE '5877642-06-25 (BC)');
SELECT date_trunc('isoyear', DATE '5877642-06-25 (BC)');   -- same overflow
-- UBSan: src/include/duckdb/common/types/date.hpp:58: signed integer overflow: -2147483646 - 3
```

`date_t::operator-` does raw `int32` arithmetic; the week/isoyear truncation paths subtract the weekday offset
without a range check. Undefined behavior in release builds; aborts under `-fsanitize=undefined -fno-sanitize-recover`.

## 3. TIMETZ offsets with zero minutes but nonzero seconds format incorrectly

```sql
SELECT '12:00:00-05:00:59'::TIMETZ;
-- prints 12:00:00-05:59  (which denotes offset -05:59:00, a different value)
```

`StringCast::Operation(dtime_tz_t)` (src/common/operator/string_cast.cpp) only prints the minutes field
`if (mm)`, but prints seconds independently, so `-05:00:59` collapses to `-05:59`. Displayed values are wrong
and the text round trip yields a different value. Fix: print minutes whenever minutes or seconds are nonzero.

## 4. `list_sort` orders INTERVALs inconsistently with comparisons and ORDER BY

```sql
SELECT INTERVAL '31 days' > INTERVAL '1 month';                    -- true
SELECT i FROM (VALUES (INTERVAL '31 days'), (INTERVAL '1 month')) t(i) ORDER BY i;
-- 1 month, 31 days
SELECT list_sort([INTERVAL '31 days', INTERVAL '1 month']);
-- [31 days, 1 month]   <- disagrees with both
```

`create_sort_key` encodes intervals by raw `(months, days, micros)` without the normalization
(1 month = 30 days, 1 day = 24 h) that comparisons, ORDER BY, aggregates and window functions apply.
Affects `list_sort`, `list_reverse_sort`, `list_grade_up`, `list_distinct` and any other `create_sort_key` consumer.

## 5. `Value::ToSQLString()` produces unparseable/wrong SQL for several cases

- STRUCT keys are not escaped: `Value::STRUCT({{"k'", ...}})` renders as `{'k'': ...}` → parser error.
  (src/common/types/value.cpp, `ret += "'" + name + "': "`)
- BIT values are rendered bare: `Value::BIT("10")` renders as `10`, which re-parses as INTEGER and casts to a
  32-bit bit string. Should be `'10'::BIT`.
- UNION values render as `union_value(tag := x)`, which loses the other members;
  `SELECT [union_value(a := 1), union_value(b := 'x')]` then fails to bind.

## 6. INTERVAL text output cannot always be parsed back (hours ≥ 10 digits)

```sql
SELECT to_microseconds(9223372036854775807)::VARCHAR;   -- '2562047788:00:54.775807'
SELECT '2562047788:00:54.775807'::INTERVAL;             -- Conversion Error
```

`Interval::ToString` emits up to 10-digit hour counts, but `Time::TryConvertInternal`
(src/common/types/time.cpp, "Allow up to 9 digit hours") rejects more than 9 digits.

## 7. `list_contains` and `list_position` disagree on NULL semantics

```sql
SELECT list_contains([false, true, NULL], NULL);  -- NULL
SELECT list_position([false, true, NULL], NULL);  -- 3
```

`list_position` treats a NULL needle as matchable (IS NOT DISTINCT semantics), `list_contains` propagates NULL.

## 8. Week-based date parts fail on the minimum year

```sql
SELECT weekofyear(DATE '5877642-06-25 (BC)');
-- Conversion Error: Date out of range: -5877641-1-1
```

`weekofyear`/`isoyear`/`yearweek` (and `date_trunc('year'/'quarter'/'month')`) materialize Jan 1 of the year
as a `date_t`, which is out of range for dates in the minimum year even though the requested result is
representable.

## 9. Sort binds `decode_sort_key` by round-tripping types through SQL strings

`Sort::Sort` passes column types as strings that `DecodeSortKeyBind` re-parses with `Parser::ParseColumnList`
(src/function/scalar/create_sort_key.cpp). Types whose string form is not parseable break sorting; e.g. an
API-created ENUM with a value containing a NUL byte (possible via Arrow dictionaries):

```
list_sort(<LIST(ENUM('a', chr(0), 'b'))>)
-- Invalid Input Error: Value "ENUM('a', chr(0), 'b')" can not be converted to a DuckDB Type
```

## 10. JSON → DECIMAL loses precision (goes through a double)

```sql
SELECT '883406386745030.3'::JSON::DECIMAL(16,1);   -- 883406386745030.2
```

JSON numbers with a fractional part are parsed as doubles before conversion to DECIMAL; parsing the raw JSON
number text directly into DECIMAL would be exact.

## 11. `INT_MIN % -1` errors instead of returning 0

```sql
SELECT (-32768)::SMALLINT % (-1)::SMALLINT;
-- Out of Range Error: Overflow in division of -32768 / -1
```

The mathematical result (0) is representable (PostgreSQL returns 0); the error message also says "division"
for a modulo.

## 12. `levenshtein`/`damerau_levenshtein`/`hamming` operate on bytes, not characters

```sql
SELECT levenshtein('中', 'a');   -- 3 (bytes), documentation says "single-character edits"
```

Multi-byte UTF-8 characters count once per byte. (Also: `damerau_levenshtein` implements the unrestricted
distance, not the more common optimal-string-alignment variant, and `hamming` rejects empty strings.)

---

### Notable non-bugs the tests must account for (DuckDB semantics)

- the nested-value text format is lossy by design (strings inside lists/structs are not quoted), so only
  "safe" strings round trip through `CAST(v AS VARCHAR)` inside nested types;
- `VARCHAR → UNION` casts only try members that VARCHAR *implicitly* casts to;
- `substring` counts negative starts from the end and interprets negative lengths as a window before the start;
- prepared-statement parameters may bind to wider types than the passed value (`-$1` with a SMALLINT parameter
  binds INTEGER negation);
- `epoch_ms` rounds half away from zero rather than flooring;
- `millisecond()`/`microsecond()` include the seconds component (PostgreSQL semantics);
- HUGEINT and UHUGEINT literals in one list unify to DOUBLE (documented promotion rule).

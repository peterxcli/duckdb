#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test CREATE VIEW with prepared parameters", "[view]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t1(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t1 VALUES (1), (2)"));

	// CREATE TABLE AS with parameters
	auto result = con.Query("CREATE TABLE t2 AS SELECT * FROM t1 WHERE i = ?", 1);
	REQUIRE_NO_FAIL(*result);

	// CREATE VIEW AS with parameters
	auto prepare = con.Prepare("CREATE VIEW v1 AS SELECT * FROM t1 WHERE i = ?");
	REQUIRE(prepare->success);
	result = prepare->Execute(2);
	REQUIRE_NO_FAIL(*result);

	result = con.Query("SELECT * FROM v1")
	
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));

	// Test with read_csv if possible
	// We'll just use a simple values list as a table function if read_csv is too complex to setup
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW v2 AS SELECT * FROM range(?)", 3));
	result = con.Query("SELECT COUNT(*) FROM v2");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {3}));
}

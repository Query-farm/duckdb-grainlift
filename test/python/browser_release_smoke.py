"""Check signed Haybarn 2.0 community artifacts over HTTP and Iroh.

Run browser-test/serve.py and scripts/test-server.sh first. The gateway must
serve sqlite_demo. For Iroh, map the browser endpoint reported by this script
to the test principal and pass --iroh-server and --iroh-key on the next run.
"""

import argparse
import json
from urllib.parse import urlencode, urljoin

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--page", default="http://127.0.0.1:18080/")
    parser.add_argument("--server", default="grainlift+http://127.0.0.1:18484")
    parser.add_argument("--iroh-server")
    parser.add_argument("--iroh-key", default="01" * 32)
    parser.add_argument("--identity-only", action="store_true")
    parser.add_argument(
        "--local",
        action="store_true",
        help="Test unsigned artifacts from the page's /repo directory",
    )
    args = parser.parse_args()
    results = []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            bundles = (
                ["threads"]
                if args.iroh_server or args.identity_only
                else ["eh", "threads"]
            )
            for bundle in bundles:
                page = browser.new_page()
                page_errors = []
                page.on("pageerror", lambda error: page_errors.append(str(error)))
                params = {
                    "autoload": "0",
                    "signed": "0" if args.local else "1",
                    "bundle": bundle,
                    "irohKey": args.iroh_key,
                }
                page.goto(args.page + "?" + urlencode(params))
                page.wait_for_function(
                    "window.grainliftTest !== undefined", timeout=120_000
                )
                if args.identity_only:
                    endpoint = page.evaluate("window.grainliftTest.irohEndpointId()")
                    assert endpoint and len(endpoint) == 64
                    print(json.dumps({"browser_endpoint": endpoint}))
                    return

                def query(sql):
                    result = page.evaluate("sql => window.grainliftTest.sql(sql)", sql)
                    if not result["ok"]:
                        raise AssertionError(result["error"])
                    return result["rows"]

                assert query("SELECT version() AS version")[0]["version"] == "v2.0.0"
                if args.local:
                    repo = urljoin(args.page, "/repo").replace("'", "''")
                    query(f"INSTALL grainlift FROM '{repo}'")
                else:
                    query("INSTALL grainlift FROM community")
                query("LOAD grainlift")
                version = query(
                    "SELECT extension_version FROM duckdb_extensions() WHERE extension_name='grainlift'"
                )[0]["extension_version"]
                assert version == "v0.6.1", version
                if args.iroh_server:
                    page.evaluate("window.grainliftTest.irohEndpointId()")
                    uri = "grainlift+iroh://" + args.iroh_server
                    auth = ""
                else:
                    uri = args.server
                    auth = ", BEARER_TOKEN 'grainlift-test-token'"
                uri = uri.replace("'", "''")
                query(
                    f"CREATE SECRET gateway (TYPE grainlift, URI '{uri}', TARGET 'sqlite_demo'{auth})"
                )
                query(f"ATTACH '{uri}' AS remote (TYPE grainlift)")
                assert query("SELECT count(*)::VARCHAR AS n FROM remote.cities") == [
                    {"n": "8"}
                ]
                assert query(
                    "SELECT sum(i)::VARCHAR AS n FROM remote.numbers WHERE i > 90000"
                ) == [{"n": "950005000"}]
                assert query(
                    "SELECT * FROM grainlift_scan('remote', 'SELECT ? AS answer', params := row(42), columns := {'answer': 'BIGINT'})"
                ) == [{"answer": "42"}]
                query(
                    "CREATE TABLE remote.browser_release_probe AS SELECT 1::BIGINT AS id"
                )
                query("BEGIN")
                query("INSERT INTO remote.browser_release_probe VALUES (2)")
                query("ROLLBACK")
                assert query(
                    "SELECT count(*)::VARCHAR AS n FROM remote.browser_release_probe"
                ) == [{"n": "1"}]
                query("DROP TABLE remote.browser_release_probe")
                query("DETACH remote")
                assert not page_errors, page_errors
                results.append(
                    {
                        "bundle": bundle,
                        "transport": "iroh" if args.iroh_server else "http",
                        "extension_version": version,
                        "signed": not args.local,
                        "passed": True,
                    }
                )
                page.close()
        finally:
            browser.close()
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()

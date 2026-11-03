"""Exercise a built extension with released DuckDB and a real HTTP gateway."""

import argparse
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

import duckdb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extension", type=Path)
    args = parser.parse_args()
    assert duckdb.__version__ == "2.0.0", duckdb.__version__
    root = Path(__file__).resolve().parents[2]
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="grainlift-release-") as work:
        env = dict(os.environ, GRAINLIFT_TEST_DIR=work, GRAINLIFT_PG="0")
        with open(Path(work) / "server.log", "w") as log:
            server = subprocess.Popen(
                ["bash", str(root / "scripts/test-server.sh"), str(port)],
                env=env,
                stdout=log,
                stderr=log,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 30
                while True:
                    if server.poll() is not None:
                        raise RuntimeError("Gateway exited before readiness")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=1):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Gateway did not become ready") from None
                        time.sleep(0.1)
                # Unsigned loading is confined to the locally built CI artifact.
                with duckdb.connect(config={"allow_unsigned_extensions": "true"}) as db:
                    db.execute("INSTALL httpfs; LOAD httpfs")
                    extension = str(args.extension.resolve()).replace("'", "''")
                    db.execute(f"LOAD '{extension}'")
                    assert db.execute(
                        "SELECT extension_version FROM duckdb_extensions() WHERE extension_name='grainlift'"
                    ).fetchone() == ("v0.6.1",)
                    uri = f"grainlift+http://127.0.0.1:{port}"
                    db.execute(
                        f"CREATE SECRET gateway (TYPE grainlift, URI '{uri}', TARGET 'sqlite_demo', BEARER_TOKEN 'grainlift-test-token')"
                    )
                    db.execute(f"ATTACH '{uri}' AS remote (TYPE grainlift)")
                    assert db.execute(
                        "SELECT count(*) FROM remote.cities"
                    ).fetchone() == (8,)
                    assert db.execute(
                        "SELECT sum(i) FROM remote.numbers WHERE i > 90000"
                    ).fetchone() == (950005000,)
                    assert db.execute(
                        "SELECT * FROM grainlift_scan('remote', 'SELECT ? AS value', params := row(42), columns := {'value': 'BIGINT'})"
                    ).fetchone() == (42,)
                    assert db.execute(
                        "SELECT * FROM grainlift_scan('remote', 'SELECT ? AS value', params := {'named': 43}, columns := {'value': 'BIGINT'})"
                    ).fetchone() == (43,)
                    db.execute(
                        "CREATE TABLE remote.release_probe AS SELECT 1::BIGINT AS id, 'before'::VARCHAR AS label"
                    )
                    db.execute(
                        "BEGIN; INSERT INTO remote.release_probe VALUES (2, 'rollback'); ROLLBACK"
                    )
                    assert db.execute(
                        "SELECT count(*) FROM remote.release_probe"
                    ).fetchone() == (1,)
                    db.execute(
                        "BEGIN; INSERT INTO remote.release_probe VALUES (3, 'committed'); COMMIT"
                    )
                    assert db.execute(
                        "SELECT id, label FROM remote.release_probe ORDER BY id"
                    ).fetchall() == [(1, "before"), (3, "committed")]
                    assert db.execute(
                        f"SELECT grainlift_which_secret('{uri}.evil') IS NULL"
                    ).fetchone() == (True,)
                    db.execute("DETACH remote")
                    try:
                        db.execute(
                            f"ATTACH '{uri}' AS denied (TYPE grainlift, TARGET 'sqlite_demo', BEARER_TOKEN 'invalid', SECRET '')"
                        )
                    except duckdb.Error:
                        pass
                    else:
                        raise AssertionError("Gateway accepted an invalid bearer token")
                print(
                    "DuckDB 2.0 release smoke passed: scans, parameters, ingestion, transactions, secret scope, authentication"
                )
            finally:
                if server.poll() is None:
                    os.killpg(server.pid, signal.SIGTERM)
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(server.pid, signal.SIGKILL)
                        server.wait(timeout=5)


if __name__ == "__main__":
    main()

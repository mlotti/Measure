import json
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[1]


def main():
    project = "measure-smoke-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory() as temporary:
        override = Path(temporary) / "ports.yaml"
        override.write_text(
            "services:\n"
            "  measure-server:\n"
            "    ports: !override\n"
            '      - "127.0.0.1::8080"\n'
        )
        compose = [
            "docker", "compose", "-p", project,
            "-f", str(ROOT / "compose.yaml"), "-f", str(override),
        ]

        def run(*args, check=True):
            return subprocess.run(
                [*compose, *args], check=check, capture_output=True,
                text=True, timeout=180,
            )

        def endpoint():
            address = run("port", "measure-server", "8080").stdout.strip()
            assert address.startswith("127.0.0.1:"), address
            return "http://" + address

        def dashboard():
            with urllib.request.urlopen(base + "/api/dashboard", timeout=5) as response:
                return json.load(response)

        def wait_for_device():
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                for device in dashboard()["devices"]:
                    if (
                        device["client_id"] == "demo-device"
                        and device["connected"]
                        and device["desired_revision"] == device["acknowledged_revision"]
                    ):
                        return device
                time.sleep(0.2)
            raise AssertionError("Device did not connect and acknowledge desired state")

        try:
            run("up", "-d", "--wait", "--wait-timeout", "120")
            base = endpoint()
            with urllib.request.urlopen(base + "/", timeout=5) as response:
                assert b'<div id="root">' in response.read()
            wait_for_device()

            for service, role in (
                ("measure-server", "server"),
                ("demo-device", "device"),
                ("demo-operator", "operator"),
            ):
                script = (
                    "set -eu; test ! -e /run/measure/demo-certificates; "
                    'find /run/measure/certs -maxdepth 1 -type f -printf "%f\\n"'
                )
                if role == "operator":
                    result = run("run", "--rm", "--no-deps", "--entrypoint",
                                 "sh", service, "-c", script)
                else:
                    result = run("exec", "-T", service, "sh", "-c", script)
                assert sorted(result.stdout.splitlines()) == sorted(
                    ["ca.crt", role + ".crt", role + ".key"]
                ), (service, result.stdout)

            config = json.loads(run("--profile", "demo", "config", "--format", "json").stdout)
            for service, role in (
                ("measure-server", "server"),
                ("demo-device", "device"),
                ("demo-operator", "operator"),
            ):
                mounts = config["services"][service]["volumes"]
                credentials = [m for m in mounts if m["target"] == "/run/measure/certs"]
                assert len(credentials) == 1
                assert credentials[0]["read_only"]
                assert credentials[0]["source"] == "demo-" + role + "-certificates"

            denied = run(
                "run", "--rm", "--no-deps", "demo-device",
                "--target=measure-server:50051", "--calibrate=true",
                "--root_cert=/run/measure/certs/ca.crt",
                "--client_cert=/run/measure/certs/device.crt",
                "--client_key=/run/measure/certs/device.key", check=False,
            )
            assert denied.returncode == 1, denied.stdout + denied.stderr
            assert "operator" in (denied.stdout + denied.stderr).lower()
            operator = run("run", "--rm", "demo-operator")
            assert "Calibration accepted" in operator.stdout

            # The dashboard checks the Origin against its container listener port.
            request = urllib.request.Request(
                base + "/api/threshold", data=b"threshold=0",
                headers={"Origin": "http://127.0.0.1:8080",
                         "Content-Type": "application/x-www-form-urlencoded"},
            )
            with urllib.request.urlopen(request, timeout=5) as response:
                assert response.status == 200
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                device = wait_for_device()
                if device["latest_point"] is not None:
                    break
                time.sleep(0.2)
            else:
                raise AssertionError("Device did not deliver a measurement")
            reading_time = device["timestamp_unix_ms"]
            reading_point = device["latest_point"]
            ca_before = run("exec", "-T", "demo-device", "sha256sum",
                            "/run/measure/certs/ca.crt").stdout
            run("down")
            run("up", "-d", "--wait", "--wait-timeout", "120")
            base = endpoint()
            assert dashboard()["threshold"] == 0
            wait_for_device()
            with urllib.request.urlopen(
                base + "/api/history?client_id=demo-device", timeout=5
            ) as response:
                history = json.load(response)["events"]
            assert any(
                event["timestamp_unix_ms"] == reading_time
                and event["point"] == reading_point
                for event in history
            ), "The measurement stored before recreation was lost"
            ca_after = run("exec", "-T", "demo-device", "sha256sum",
                           "/run/measure/certs/ca.crt").stdout
            assert ca_before == ca_after
            print("Compose startup, device delivery, credential isolation, authorization, "
                  "and persistence passed")
        except Exception:
            logs = run("logs", "--no-color", check=False)
            print(logs.stdout, logs.stderr)
            raise
        finally:
            run("down", "-v", "--remove-orphans")


if __name__ == "__main__":
    main()

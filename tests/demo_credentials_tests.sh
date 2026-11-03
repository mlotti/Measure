#!/bin/sh
set -eu

generator=${1:-/usr/local/bin/generate-demo-certs}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

mkdir -p "$work_dir/original/server" "$work_dir/original/device" \
  "$work_dir/original/operator"
"$generator" "$work_dir/original" > "$work_dir/generation.log" 2>&1 || {
  cat "$work_dir/generation.log" >&2
  exit 1
}
before=$(sha256sum "$work_dir/original/server/server.crt")
"$generator" "$work_dir/original"
[ "$before" = "$(sha256sum "$work_dir/original/server/server.crt")" ]
echo "Valid credentials are reused unchanged"

expect_rejection() {
  snapshot=$(find "$case_dir" -type f -exec sha256sum {} \; | sort)
  if "$generator" "$case_dir" > "$work_dir/rejection.log" 2>&1; then
    echo "Invalid credentials were accepted: $1" >&2
    exit 1
  fi
  grep -q "$2" "$work_dir/rejection.log"
  grep -q "certificate-only reset" "$work_dir/rejection.log"
  [ "$snapshot" = "$(find "$case_dir" -type f -exec sha256sum {} \; | sort)" ]
  echo "$1: rejected"
}

case_dir="$work_dir/expired"
cp -R "$work_dir/original" "$case_dir"
openssl x509 -in "$case_dir/server/server.crt" \
  -signkey "$case_dir/server/server.key" -days -1 \
  -out "$work_dir/expired.crt"
cp "$work_dir/expired.crt" "$case_dir/server/server.crt"
expect_rejection "Expired certificate" "server certificate"

case_dir="$work_dir/malformed"
cp -R "$work_dir/original" "$case_dir"
printf 'not a certificate\n' > "$case_dir/device/device.crt"
expect_rejection "Malformed certificate" "device certificate"

case_dir="$work_dir/mismatched"
cp -R "$work_dir/original" "$case_dir"
cp "$case_dir/operator/operator.key" "$case_dir/device/device.key"
expect_rejection "Mismatched key" "device private key does not match"

case_dir="$work_dir/unreadable-key"
cp -R "$work_dir/original" "$case_dir"
printf 'not a key\n' > "$case_dir/operator/operator.key"
expect_rejection "Malformed key" "operator private key cannot be read"

case_dir="$work_dir/untrusted"
cp -R "$work_dir/original" "$case_dir"
openssl x509 -in "$case_dir/device/device.crt" \
  -signkey "$case_dir/device/device.key" -days 1 \
  -out "$work_dir/untrusted.crt"
cp "$work_dir/untrusted.crt" "$case_dir/device/device.crt"
expect_rejection "Untrusted certificate" "device certificate"

case_dir="$work_dir/wrong-identity"
cp -R "$work_dir/original" "$case_dir"
cp "$case_dir/operator/operator.crt" "$case_dir/device/device.crt"
cp "$case_dir/operator/operator.key" "$case_dir/device/device.key"
expect_rejection "Wrong role identity" "device certificate has the wrong identity"

case_dir="$work_dir/partial"
cp -R "$work_dir/original" "$case_dir"
rm "$case_dir/device/device.crt"
expect_rejection "Incomplete credentials" "incomplete"

case_dir="$work_dir/expired-ca"
cp -R "$work_dir/original" "$case_dir"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj /CN=Expired-Test-CA -keyout "$work_dir/ca.key" \
  -out "$work_dir/ca.crt" > "$work_dir/ca.log" 2>&1
openssl x509 -in "$work_dir/ca.crt" -signkey "$work_dir/ca.key" \
  -days -1 -out "$work_dir/expired-ca.crt"
for role in server device operator; do
  cp "$work_dir/expired-ca.crt" "$case_dir/$role/ca.crt"
done
expect_rejection "Expired CA" "CA is malformed or expired"

case_dir="$work_dir/different-ca"
cp -R "$work_dir/original" "$case_dir"
cp "$work_dir/ca.crt" "$case_dir/operator/ca.crt"
expect_rejection "Inconsistent CA" "operator CA differs"

set -- --port=0 --http_port=0 --http_bind_address=not-an-ip \
  "--database_path=$work_dir/startup.db" \
  "--server_cert=$work_dir/original/server/server.crt" \
  "--server_key=$work_dir/original/server/server.key" \
  "--client_ca=$work_dir/original/server/ca.crt"
result=0
timeout 5 measure_server "$@" > "$work_dir/startup.log" 2>&1 || result=$?
[ "$result" -eq 1 ]
grep -q "Server failed: Invalid dashboard bind address" "$work_dir/startup.log"
echo "Invalid dashboard address terminates the server with exit status 1"

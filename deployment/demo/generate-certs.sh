#!/bin/sh
set -eu

cert_dir=/run/measure/certs
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

if [ -s "$cert_dir/ca.crt" ] &&
   [ -s "$cert_dir/server.crt" ] &&
   [ -s "$cert_dir/server.key" ] &&
   [ -s "$cert_dir/device.crt" ] &&
   [ -s "$cert_dir/device.key" ] &&
   [ -s "$cert_dir/operator.crt" ] &&
   [ -s "$cert_dir/operator.key" ]; then
  exit 0
fi

if [ -n "$(ls -A "$cert_dir")" ]; then
  echo "Demo certificate volume is incomplete; run 'docker compose down -v' to reset it." >&2
  exit 1
fi

openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout "$work_dir/ca.key" -out "$work_dir/ca.crt" \
  -subj "/CN=Measure-Demo-CA"

create_client_certificate() {
  name=$1
  common_name=$2
  openssl req -new -newkey rsa:2048 -nodes \
    -keyout "$work_dir/$name.key" -out "$work_dir/$name.csr" \
    -subj "/CN=$common_name"
  printf '%s\n' "extendedKeyUsage=clientAuth" \
    > "$work_dir/$name.ext"
  openssl x509 -req -days 365 -sha256 \
    -in "$work_dir/$name.csr" \
    -CA "$work_dir/ca.crt" -CAkey "$work_dir/ca.key" \
    -CAcreateserial -out "$work_dir/$name.crt" \
    -extfile "$work_dir/$name.ext"
}

openssl req -new -newkey rsa:2048 -nodes \
  -keyout "$work_dir/server.key" -out "$work_dir/server.csr" \
  -subj "/CN=measure-server"
printf '%s\n' \
  "subjectAltName=DNS:measure-server,DNS:localhost,IP:127.0.0.1" \
  "extendedKeyUsage=serverAuth" > "$work_dir/server.ext"
openssl x509 -req -days 365 -sha256 \
  -in "$work_dir/server.csr" \
  -CA "$work_dir/ca.crt" -CAkey "$work_dir/ca.key" \
  -CAcreateserial -out "$work_dir/server.crt" \
  -extfile "$work_dir/server.ext"

create_client_certificate device "device:demo-device"
create_client_certificate operator "operator:demo"

cp "$work_dir/ca.crt" "$cert_dir/ca.crt"
cp "$work_dir/server.crt" "$cert_dir/server.crt"
cp "$work_dir/server.key" "$cert_dir/server.key"
cp "$work_dir/device.crt" "$cert_dir/device.crt"
cp "$work_dir/device.key" "$cert_dir/device.key"
cp "$work_dir/operator.crt" "$cert_dir/operator.crt"
cp "$work_dir/operator.key" "$cert_dir/operator.key"
chown -R 10001:10001 "$cert_dir"
chmod 0400 "$cert_dir"/*.key
chmod 0444 "$cert_dir"/*.crt

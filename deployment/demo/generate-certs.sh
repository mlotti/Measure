#!/bin/sh
set -eu

umask 077
cert_dir=/run/measure/demo-certificates
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

complete=true
nonempty=false
for role in server device operator; do
  role_dir="$cert_dir/$role"
  if [ ! -s "$role_dir/ca.crt" ] ||
     [ ! -s "$role_dir/$role.crt" ] ||
     [ ! -s "$role_dir/$role.key" ]; then
    complete=false
  fi
  if [ -n "$(ls -A "$role_dir")" ]; then
    nonempty=true
  fi
done

if [ "$complete" = true ]; then exit 0; fi

if [ "$nonempty" = true ]; then
  echo "Demo certificate volumes are incomplete; reset all three role certificate volumes before retrying." >&2
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

for role in server device operator; do
  role_dir="$cert_dir/$role"
  install -o 10001 -g 10001 -m 0444 "$work_dir/ca.crt" "$role_dir/ca.crt"
  install -o 10001 -g 10001 -m 0444 "$work_dir/$role.crt" "$role_dir/$role.crt"
  install -o 10001 -g 10001 -m 0400 "$work_dir/$role.key" "$role_dir/$role.key"
done

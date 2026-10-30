#!/bin/sh
set -eu

umask 077
cert_dir=${1:-/run/measure/demo-certificates}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

invalid_credentials() {
  echo "Demo credentials are invalid: $1. Follow the certificate-only reset in README.md; measurement data can be preserved." >&2
  exit 1
}

validate_credentials() {
  for role in server device operator; do
    role_dir="$cert_dir/$role"
    cmp -s "$cert_dir/server/ca.crt" "$role_dir/ca.crt" ||
      invalid_credentials "$role CA differs from the server CA"
    openssl x509 -in "$role_dir/ca.crt" -noout -checkend 0 ||
      invalid_credentials "$role CA is malformed or expired"
    purpose=sslclient
    identity="device:demo-device"
    case "$role" in
      server) purpose=sslserver; identity=measure-server ;;
      operator) identity="operator:demo" ;;
    esac
    set -- -purpose "$purpose"
    if [ "$role" = server ]; then
      set -- "$@" -verify_hostname measure-server
    fi
    openssl verify "$@" -CAfile "$role_dir/ca.crt" \
      "$role_dir/$role.crt" ||
      invalid_credentials "$role certificate is expired, not yet valid, malformed, or untrusted"
    subject=$(openssl x509 -in "$role_dir/$role.crt" -noout \
      -subject -nameopt RFC2253) ||
      invalid_credentials "$role certificate cannot be read"
    [ "$subject" = "subject=CN=$identity" ] ||
      invalid_credentials "$role certificate has the wrong identity"
    openssl x509 -in "$role_dir/$role.crt" -noout -pubkey \
      > "$work_dir/certificate.pub" ||
      invalid_credentials "$role certificate cannot be read"
    openssl pkey -in "$role_dir/$role.key" -passin pass: -pubout \
      > "$work_dir/key.pub" ||
      invalid_credentials "$role private key cannot be read"
    cmp -s "$work_dir/certificate.pub" "$work_dir/key.pub" ||
      invalid_credentials "$role private key does not match its certificate"
  done
}

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

if [ "$complete" = true ]; then
  validate_credentials
  exit 0
fi

if [ "$nonempty" = true ]; then
  invalid_credentials "certificate volumes are incomplete"
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

validate_credentials

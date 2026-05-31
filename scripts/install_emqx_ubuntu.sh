#!/usr/bin/env bash
set -Eeuo pipefail

if ! command -v curl >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y curl ca-certificates
fi

curl -s https://packagecloud.io/install/repositories/emqx/emqx-enterprise5/script.deb.sh | sudo bash
sudo apt-get update

package_name=""
for candidate in emqx emqx-enterprise; do
  if apt-cache show "$candidate" >/dev/null 2>&1; then
    package_name="$candidate"
    break
  fi
done

if [ -z "$package_name" ]; then
  echo "unable to find EMQX package from apt; try: apt-cache search '^emqx'" >&2
  exit 1
fi

echo "Installing EMQX package: $package_name"
sudo apt-get install -y "$package_name"

if command -v systemctl >/dev/null 2>&1; then
  sudo systemctl enable --now emqx
  sudo systemctl --no-pager --full status emqx || true
else
  sudo service emqx start
fi

echo
echo "EMQX Dashboard: http://127.0.0.1:18083"
echo "Default login: admin / public"

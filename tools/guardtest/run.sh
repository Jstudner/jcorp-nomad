#!/usr/bin/env bash
# Compile and run the path-rule tests against firmware/JcorpNomadProject/NomadGuard.h.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -o "$out/guardtest" "$here/guardtest.cpp"
"$out/guardtest"

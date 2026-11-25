#!/usr/bin/env bash
# Initialise this project's west workspace in the parent directory of this repo.
# This repo is the manifest repository (see west.yml).
# Safe to re-run: skips `west init` if the workspace already exists.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
ws_dir="$(dirname "$repo_dir")"

cd "$ws_dir"
[ -w . ] || sudo chown "$(id -u):$(id -g)" .

if [ ! -d .west ]; then
    west init -l "$(basename "$repo_dir")"
fi
west update --narrow -o=--depth=1
# No module argument: fetch blobs for every module pulled in by west.yml
west blobs fetch

#!/usr/bin/env bash
# Incremental dev build: mirrors the worktree into a persistent directory and
# lets AMBuild recompile only what changed. Prints the path of the built .so.
# build_empires.sh stays the clean, from-scratch build.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/dev_cpu.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CLEAN="${RCBOT_DEPS_DIR:-$(dirname "$ROOT")/rcbot2-clean}"
MIRROR="${RCBOT_DEV_MIRROR:-$HOME/.cache/rcbot2-dev-build}"
SDK="$CLEAN/alliedmodders"

mkdir -p "$MIRROR"
# -t keeps mtimes, so untouched files stay up to date for AMBuild.
rsync -rlt --delete \
  --exclude=/.git --exclude=/post --exclude='.ambuild2' \
  --exclude=/hl2sdk-manifests --exclude=/alliedmodders \
  "$ROOT/" "$MIRROR/"

mkdir -p "$MIRROR/alliedmodders"
ln -sfn "$CLEAN/hl2sdk-manifests" "$MIRROR/hl2sdk-manifests"
for dep in hl2sdk-sdk2013 metamod-source sourcemod; do
  ln -sfn "$SDK/$dep" "$MIRROR/alliedmodders/$dep"
done

cd "$MIRROR"
# versioning/ reads the commit from git; a fixed stub commit keeps it stable.
if [[ ! -d .git ]]; then
  git init -q . && git -c user.name=dev -c user.email=dev@localhost commit -q --allow-empty -m dev-build
fi
if [[ ! -f post/.ambuild2/vars ]]; then
  rm -rf post
  CC=gcc CXX=g++ CXXFLAGS='-Wno-stringop-truncation' PYTHONDONTWRITEBYTECODE=1 \
    "$(sed -n "1s/^#!//p" "$(command -v ambuild)")" configure.py -s sdk2013 --targets x86 --enable-optimize \
      --hl2sdk-root "$MIRROR/alliedmodders" \
      --sm-path "$MIRROR/alliedmodders/sourcemod" \
      --mms-path "$MIRROR/alliedmodders/metamod-source" >&2
fi
(cd post && ambuild >&2)

echo "$MIRROR/post/package/addons/rcbot2/bin/rcbot.2.sdk2013.so"

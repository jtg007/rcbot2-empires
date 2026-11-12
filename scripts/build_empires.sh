#!/usr/bin/env bash
set -euo pipefail

# Worktree-safe SDK2013 build wrapper.  The source checkout may have a .git
# file (linked worktree), so the build itself runs from a disposable local
# clone with a real .git directory and a working-tree snapshot applied.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
SDK_ROOT="${SDK_ROOT:-$ROOT_DIR/alliedmodders}"
SDK2013_DIR="$SDK_ROOT/hl2sdk-sdk2013"
MMS_DIR="$SDK_ROOT/metamod-source"
SM_DIR="$SDK_ROOT/sourcemod"
MANIFEST_DIR="${MANIFEST_DIR:-$ROOT_DIR/hl2sdk-manifests}"
LOG_DIR="${EMPIRES_BUILD_LOG_DIR:-/tmp/rcbot2-build-logs}"
ARTIFACT_DIR="${EMPIRES_BUILD_ARTIFACT_DIR:-/tmp/rcbot2-build-output-v2}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="$LOG_DIR/build-$STAMP.log"
ARTIFACT="$ARTIFACT_DIR/rcbot.2.sdk2013.so"
PROVENANCE_FILE="$ARTIFACT_DIR/rcbot.2.sdk2013.build.json"
BUILD_STARTED_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

PYTHON_OVERRIDE_SET=0
AMBUILD_OVERRIDE_SET=0
if [[ "${AMBUILD_PYTHON+x}" == "x" ]]; then
  PYTHON_OVERRIDE_SET=1
fi
if [[ "${AMBUILD_BIN+x}" == "x" ]]; then
  AMBUILD_OVERRIDE_SET=1
fi

declare -a PYTHON_CANDIDATES=()
declare -a AMBUILD_CANDIDATES=()
declare -a PYTHON_REJECTED=()
declare -a AMBUILD_REJECTED=()
declare -a UNTRACKED_SOURCE_PATHS=()

SELECTED_PYTHON=""
SELECTED_AMBUILD=""
TRACKED_MODIFIED=0
UNTRACKED_SOURCE=0
EXCLUDED_GENERATED=0

mkdir -p "$LOG_DIR" "$ARTIFACT_DIR"
exec > >(tee -a "$LOG_FILE") 2>&1

TEMP_SRC=""
SOURCE_IDENTITY_FILE=""
cleanup() {

  if [[ -n "$TEMP_SRC" && -d "$TEMP_SRC" ]]; then
    rm -rf "$TEMP_SRC"
  fi
  if [[ -n "$SOURCE_IDENTITY_FILE" && -f "$SOURCE_IDENTITY_FILE" ]]; then
    rm -f "$SOURCE_IDENTITY_FILE"
  fi
}
trap cleanup EXIT

fail_check() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    printf 'BUILD FAIL: missing required path: %s\n' "$path"
    return 1
  fi
}

add_python_candidate() {
  local candidate="$1"
  local existing
  [[ -n "$candidate" ]] || return 0
  for existing in "${PYTHON_CANDIDATES[@]}"; do
    [[ "$existing" == "$candidate" ]] && return 0
  done
  PYTHON_CANDIDATES+=("$candidate")
}

add_ambuild_candidate() {
  local candidate="$1"
  local existing
  [[ -n "$candidate" ]] || return 0
  for existing in "${AMBUILD_CANDIDATES[@]}"; do
    [[ "$existing" == "$candidate" ]] && return 0
  done
  AMBUILD_CANDIDATES+=("$candidate")
}

resolve_executable() {
  local candidate="$1"
  local resolved

  if [[ "$candidate" == */* ]]; then
    [[ -x "$candidate" ]] || return 1
    printf '%s\n' "$candidate"
    return 0
  fi

  resolved="$(command -v "$candidate" 2>/dev/null || true)"
  [[ -n "$resolved" && -x "$resolved" ]] || return 1
  printf '%s\n' "$resolved"
}

python_can_import_ambuild() {
  local candidate="$1"
  [[ -x "$candidate" ]] || return 1
  PYTHONDONTWRITEBYTECODE=1 "$candidate" -c 'import ambuild2' >/dev/null 2>&1
}

ambuild_can_run() {
  local candidate="$1"
  [[ -x "$candidate" ]] || return 1
  "$candidate" --help >/dev/null 2>&1
}

join_candidates() {
  local separator=""
  local candidate
  if [[ "$#" -eq 0 ]]; then
    printf '(none)'
    return 0
  fi
  for candidate in "$@"; do
    printf '%s%s' "$separator" "$candidate"
    separator=', '
  done
}

record_ambuild_shebang_candidate() {
  local executable="$1"
  local first_line=""
  local shebang=""
  local interpreter=""

  IFS= read -r first_line < "$executable" || true
  [[ "$first_line" == '#!'* ]] || return 0
  shebang="${first_line#\#!}"

  if [[ "$shebang" =~ ^[[:space:]]*/usr/bin/env[[:space:]]+(-S[[:space:]]+)?([^[:space:]]+) ]]; then
    interpreter="${BASH_REMATCH[2]}"
  elif [[ "$shebang" =~ ^[[:space:]]*([^[:space:]]+) ]]; then
    interpreter="${BASH_REMATCH[1]}"
  fi

  add_python_candidate "$interpreter"
}

discover_ambuild_candidates() {
  local name
  local resolved

  if [[ "$AMBUILD_OVERRIDE_SET" -eq 1 ]]; then
    add_ambuild_candidate "${AMBUILD_BIN:-}"
    return 0
  fi

  for name in ambuild ambuild2; do
    resolved="$(resolve_executable "$name" || true)"
    add_ambuild_candidate "$resolved"
  done
}

select_ambuild() {
  local candidate
  local resolved

  for candidate in "${AMBUILD_CANDIDATES[@]}"; do
    resolved="$(resolve_executable "$candidate" || true)"
    if [[ -z "$resolved" ]]; then
      AMBUILD_REJECTED+=("$candidate (not executable or not found)")
    elif ! ambuild_can_run "$resolved"; then
      AMBUILD_REJECTED+=("$resolved (--help failed)")
    else
      SELECTED_AMBUILD="$resolved"
      return 0
    fi
  done
  return 1
}

discover_python_candidates() {
  local path_entry
  local candidate
  local candidate_name
  local pyenv_bin
  local pyenv_root
  local -a path_entries=()

  if [[ -n "$SELECTED_AMBUILD" ]]; then
    record_ambuild_shebang_candidate "$SELECTED_AMBUILD"
  fi

  IFS=: read -r -a path_entries <<< "${PATH:-}"
  for path_entry in "${path_entries[@]}"; do
    [[ -n "$path_entry" ]] || path_entry='.'
    for candidate in "$path_entry"/python*; do
      [[ -x "$candidate" ]] || continue
      candidate_name="${candidate##*/}"
      [[ "$candidate_name" =~ ^python([0-9]+([.][0-9]+)*)?$ ]] || continue
      add_python_candidate "$candidate"
    done
  done

  pyenv_bin="$(command -v pyenv 2>/dev/null || true)"
  if [[ -n "$pyenv_bin" ]]; then
    pyenv_root="$("$pyenv_bin" root 2>/dev/null || true)"
    if [[ -d "$pyenv_root/versions" ]]; then
      while IFS= read -r -d '' candidate; do
        add_python_candidate "$candidate"
      done < <(
        find "$pyenv_root/versions" -mindepth 2 -maxdepth 4 -type f -perm /111 \
          \( -name python -o -name python3 -o -name 'python3.*' \) -print0 2>/dev/null | sort -z
      )
    fi
  fi
}

select_python() {
  local candidate
  local resolved

  for candidate in "${PYTHON_CANDIDATES[@]}"; do
    resolved="$(resolve_executable "$candidate" || true)"
    if [[ -z "$resolved" ]]; then
      PYTHON_REJECTED+=("$candidate (not executable or not found)")
    elif ! python_can_import_ambuild "$resolved"; then
      PYTHON_REJECTED+=("$resolved (import ambuild2 failed)")
    else
      SELECTED_PYTHON="$resolved"
      return 0
    fi
  done
  return 1
}

discover_toolchain() {
  discover_ambuild_candidates

  if [[ "$PYTHON_OVERRIDE_SET" -eq 1 ]]; then
    add_python_candidate "${AMBUILD_PYTHON:-}"
    select_python || true
  fi

  select_ambuild || true

  if [[ "$PYTHON_OVERRIDE_SET" -eq 0 ]]; then
    discover_python_candidates
    select_python || true
  fi

  if [[ -z "$SELECTED_PYTHON" || -z "$SELECTED_AMBUILD" ]]; then
    printf 'BUILD FAIL: no valid AMBuild toolchain found.\n'
    printf 'BUILD INFO: python_candidates=%s\n' "$(join_candidates "${PYTHON_CANDIDATES[@]}")"
    printf 'BUILD INFO: python_rejected=%s\n' "$(join_candidates "${PYTHON_REJECTED[@]}")"
    printf 'BUILD INFO: ambuild_candidates=%s\n' "$(join_candidates "${AMBUILD_CANDIDATES[@]}")"
    printf 'BUILD INFO: ambuild_rejected=%s\n' "$(join_candidates "${AMBUILD_REJECTED[@]}")"
    return 1
  fi

  AMBUILD_PYTHON="$SELECTED_PYTHON"
  AMBUILD_BIN="$SELECTED_AMBUILD"
}

is_excluded_snapshot_path() {
  local path="$1"
  case "$path" in
    post|post/*|.ambuild2|.ambuild2/*|*/.ambuild2|*/.ambuild2/*)
      return 0
      ;;
    build|build/*|Build|Build/*|build_linux|build_linux/*|premake_build|premake_build/*|\
    out|out/*|dist|dist/*|artifacts|artifacts/*|runs|runs/*|__pycache__|__pycache__/*|*/__pycache__|*/__pycache__/*)
      return 0
      ;;
    *.log|*.log.*|*.tmp|*.temp|*.swp|*~|*.pyc|*.pyo|*.so|*.dylib|*.dll|*.exe|*.a|*.o|*.obj|core|core.[0-9]*|*.core|*.dmp|*.dump)
      return 0
      ;;
  esac
  return 1
}

collect_worktree_inputs() {
  local path
  local source_path

  TRACKED_MODIFIED="$(git -C "$ROOT_DIR" diff --name-only HEAD -- \
    . ':(exclude)post/**' ':(exclude)**/.ambuild2/**' | sed '/^$/d' | wc -l)"

  while IFS= read -r -d '' path; do
    if is_excluded_snapshot_path "$path"; then
      EXCLUDED_GENERATED=$((EXCLUDED_GENERATED + 1))
      continue
    fi

    source_path="$ROOT_DIR/$path"
    if [[ -f "$source_path" || -L "$source_path" ]]; then
      UNTRACKED_SOURCE_PATHS+=("$path")
      UNTRACKED_SOURCE=$((UNTRACKED_SOURCE + 1))
    else
      EXCLUDED_GENERATED=$((EXCLUDED_GENERATED + 1))
    fi
  done < <(git -C "$ROOT_DIR" ls-files --others --exclude-standard -z)
}

print_source_summary() {
  local path
  printf 'BUILD SOURCE HEAD=%s\n' "$(git -C "$ROOT_DIR" rev-parse HEAD)"
  printf 'BUILD SOURCE tracked_modified=%s\n' "$TRACKED_MODIFIED"
  printf 'BUILD SOURCE untracked_source=%s\n' "$UNTRACKED_SOURCE"
  printf 'BUILD SOURCE excluded_generated=%s\n' "$EXCLUDED_GENERATED"
  printf 'BUILD SOURCE python=%s\n' "$AMBUILD_PYTHON"
  printf 'BUILD SOURCE ambuild=%s\n' "$AMBUILD_BIN"
  for path in "${UNTRACKED_SOURCE_PATHS[@]}"; do
    printf 'BUILD SOURCE include_untracked=%s\n' "$path"
  done
}

build() {
  discover_toolchain || return $?
  fail_check "$AMBUILD_PYTHON" || return $?
  fail_check "$AMBUILD_BIN" || return $?
  fail_check "$SDK2013_DIR" || return $?
  fail_check "$MMS_DIR" || return $?
  fail_check "$SM_DIR" || return $?
  fail_check "$MANIFEST_DIR/SdkHelpers.ambuild" || return $?

  collect_worktree_inputs || return $?

  TEMP_SRC="$(mktemp -d /tmp/rcbot2-build-src.XXXXXX)" || return $?
  printf 'BUILD INFO: source=%s\n' "$ROOT_DIR"
  printf 'BUILD INFO: temporary_clone=%s\n' "$TEMP_SRC"
  print_source_summary
  printf 'BUILD INFO: sdk=sdk2013 target=x86 optimize=1 compiler=gcc/g++\n'
  printf 'BUILD INFO: CXXFLAGS=-Wno-stringop-truncation symbol_files=disabled\n'

  git clone --local --no-hardlinks "$ROOT_DIR" "$TEMP_SRC" || return $?
  git -C "$ROOT_DIR" diff HEAD --binary -- \
    . ':(exclude)post/**' ':(exclude)**/.ambuild2/**' > "$TEMP_SRC/current-working-tree.patch" || return $?
  if [[ -s "$TEMP_SRC/current-working-tree.patch" ]]; then
    git -C "$TEMP_SRC" apply "$TEMP_SRC/current-working-tree.patch" || return $?
  fi

  # Existing post/ and .ambuild2 contents are generated build state.  Remove
  # them from the snapshot; configure.py recreates a clean post/ directory.
  rm -rf "$TEMP_SRC/post"
  find "$TEMP_SRC" -type d -name .ambuild2 -prune -exec rm -rf -- {} +

  for path in "${UNTRACKED_SOURCE_PATHS[@]}"; do
    mkdir -p "$TEMP_SRC/$(dirname "$path")" || return $?
    cp -a -- "$ROOT_DIR/$path" "$TEMP_SRC/$path" || return $?
  done

  SOURCE_IDENTITY_FILE="$(mktemp /tmp/rcbot2-build-source-identity.XXXXXX)" || return $?
  "$AMBUILD_PYTHON" "$TEMP_SRC/scripts/build_provenance.py" source-identity \
    --repo "$TEMP_SRC" > "$SOURCE_IDENTITY_FILE" || return $?

  # The local clone has empty submodule directories.  Link only the already
  # provisioned dependencies; do not initialize or fetch submodules.
  rm -rf \
    "$TEMP_SRC/hl2sdk-manifests" \
    "$TEMP_SRC/alliedmodders/hl2sdk-sdk2013" \
    "$TEMP_SRC/alliedmodders/metamod-source" \
    "$TEMP_SRC/alliedmodders/sourcemod" || return $?
  ln -s "$MANIFEST_DIR" "$TEMP_SRC/hl2sdk-manifests" || return $?
  ln -s "$SDK2013_DIR" "$TEMP_SRC/alliedmodders/hl2sdk-sdk2013" || return $?
  ln -s "$MMS_DIR" "$TEMP_SRC/alliedmodders/metamod-source" || return $?
  ln -s "$SM_DIR" "$TEMP_SRC/alliedmodders/sourcemod" || return $?

  (
    cd "$TEMP_SRC"
    CC=gcc \
    CXX=g++ \
    CXXFLAGS='-Wno-stringop-truncation' \
      PYTHONDONTWRITEBYTECODE=1 "$AMBUILD_PYTHON" configure.py \
        -s sdk2013 \
        --targets x86 \
        --enable-optimize \
        --hl2sdk-root "$TEMP_SRC/alliedmodders" \
        --sm-path "$TEMP_SRC/alliedmodders/sourcemod" \
        --mms-path "$TEMP_SRC/alliedmodders/metamod-source"
  ) || return $?

  (
    cd "$TEMP_SRC/post"
    "$AMBUILD_BIN"
  ) || return $?

  local built_binary="$TEMP_SRC/post/package/addons/rcbot2/bin/rcbot.2.sdk2013.so"
  fail_check "$built_binary" || return $?
  cp "$built_binary" "$ARTIFACT" || return $?
  printf 'BUILD INFO: artifact_sha256='
  sha256sum "$ARTIFACT"
  printf 'BUILD INFO: artifact=%s\n' "$ARTIFACT"
}

set +e
build
BUILD_STATUS=$?
set -e

if [[ "$BUILD_STATUS" -eq 0 ]]; then
  if ! "$AMBUILD_PYTHON" "$TEMP_SRC/scripts/build_provenance.py" write-manifest \
      --source-root "$TEMP_SRC" \
      --source-identity-file "$SOURCE_IDENTITY_FILE" \
      --repository-path "$ROOT_DIR" \
      --artifact "$ARTIFACT" \
      --log-path "$LOG_FILE" \
      --result PASS \
      --build-started-utc "$BUILD_STARTED_UTC" \
      --python-path "$AMBUILD_PYTHON" \
      --ambuild-path "$AMBUILD_BIN"; then
    printf 'BUILD FAIL: could not write build provenance manifest: %s\n' "$PROVENANCE_FILE"
    BUILD_STATUS=1
  fi
fi

if [[ "$BUILD_STATUS" -eq 0 ]]; then
  printf 'BUILD PASS\n'
  printf 'BUILD LOG: %s\n' "$LOG_FILE"
  printf 'BINARY: %s\n' "$ARTIFACT"
  printf 'PROVENANCE: %s\n' "$PROVENANCE_FILE"
else
  printf 'BUILD FAIL (exit %s)\n' "$BUILD_STATUS"
  printf 'BUILD LOG: %s\n' "$LOG_FILE"
fi
exit "$BUILD_STATUS"

#!/usr/bin/env python3
"""Build-source fingerprinting and provenance manifest helpers.

This module uses only the Python standard library. The shell build wrapper
calls it on the disposable source snapshot that was actually compiled; the
V2.2 runner uses the same fingerprint code to verify that snapshot against the
current checkout before starting a server.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import shlex
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Mapping, Sequence


SCHEMA = "empires-rcbot2-build-provenance-v2"
LEGACY_SCHEMA = "empires-rcbot2-build-provenance-v1"
PLUGIN_BUILD_SCHEMA = "empires-rcbot2-plugin-build-inputs-v1"
HARNESS_RUNTIME_SCHEMA = "empires-rcbot2-harness-runtime-v1"
ARTIFACT_NAME = "rcbot.2.sdk2013.so"
MANIFEST_NAME = "rcbot.2.sdk2013.build.json"

BUILD_RECIPE_FILES = (
    "configure.py",
    "AMBuildScript",
    "AMBuilder",
    "loader/AMBuilder",
    "PackageScript",
    "versioning/AMBuildScript",
    "versioning/generate.py",
    "scripts/build_empires.sh",
)
HARNESS_RUNTIME_FILES = (
    "scripts/combat_relevance_observer.py",
    "scripts/harness_v2_orchestrator.py",
    "scripts/build_provenance.py",
)
V1_RUNTIME_FILES = ("run_harness.py", "soak_triage.py")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _compiler_text_sha256(path: Path) -> str | None:
    """Hash compiler text after canonicalizing CRLF line endings to LF."""
    content = path.read_bytes()
    if b"\0" in content:
        return None
    return hashlib.sha256(content.replace(b"\r\n", b"\n")).hexdigest()


def _json_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _file_mode(path: Path) -> int:
    return path.stat().st_mode & 0o777


def _input_record(path: Path, repo_root: Path, role: str) -> dict[str, Any]:
    resolved = path.resolve(strict=True)
    try:
        relative = resolved.relative_to(repo_root.resolve())
    except ValueError:
        scope = "absolute"
        stored_path = str(resolved)
    else:
        scope = "repository"
        stored_path = relative.as_posix()
    record = {
        "scope": scope,
        "path": stored_path,
        "roles": [role],
        "sha256": _sha256_file(resolved),
        "mode": _file_mode(resolved),
    }
    if role == "compiler_input":
        semantic_digest = _compiler_text_sha256(resolved)
        if semantic_digest is not None:
            record["compiler_semantic_sha256"] = semantic_digest
    return record


def _parse_make_depfile(path: Path) -> list[str]:
    """Read GCC's Make-style dependency list, including escaped spaces."""
    raw = path.read_text(encoding="utf-8", errors="strict")
    raw = re.sub(r"\\\r?\n", " ", raw)
    escaped = False
    separator = None
    for index, character in enumerate(raw):
        if character == ":" and not escaped:
            separator = index
            break
        if character == "\\" and not escaped:
            escaped = True
        else:
            escaped = False
    if separator is None:
        raise ValueError(f"compiler dependency file has no target separator: {path}")
    try:
        return shlex.split(raw[separator + 1 :], posix=True)
    except ValueError as error:
        raise ValueError(f"cannot parse compiler dependency file {path}: {error}") from error


def _normalize_build_value(value: Any, source_root: Path) -> Any:
    if isinstance(value, str):
        return value.replace(str(source_root.resolve()), "<SOURCE_ROOT>")
    if isinstance(value, list):
        return [_normalize_build_value(item, source_root) for item in value]
    if isinstance(value, tuple):
        return [_normalize_build_value(item, source_root) for item in value]
    if isinstance(value, dict):
        return {
            str(key): _normalize_build_value(item, source_root)
            for key, item in value.items()
        }
    return value


def _commands_from_build_log(log_path: Path, source_root: Path) -> tuple[list[str], list[str]]:
    compile_commands: list[str] = []
    plugin_link_commands: list[str] = []
    compiler_pattern = re.compile(r"(?:^|\s)(?:[^\s]*/)?(?:gcc|g\+\+|clang|clang\+\+)\s")
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = compiler_pattern.search(line)
        if match is None:
            continue
        command = line[match.start() :].strip()
        try:
            arguments = shlex.split(command, posix=True)
        except ValueError:
            continue
        normalized = command.replace(str(source_root.resolve()), "<SOURCE_ROOT>")
        if "-c" in arguments and "-MF" in arguments:
            compile_commands.append(normalized)
        for index, argument in enumerate(arguments[:-1]):
            if (
                argument == "-o"
                and Path(arguments[index + 1]).name == ARTIFACT_NAME
                and "-shared" in arguments
            ):
                plugin_link_commands.append(normalized)
                break
    return sorted(set(compile_commands)), sorted(set(plugin_link_commands))


def plugin_build_identity(
    source_root: Path,
    *,
    build_configuration: Mapping[str, Any],
    log_path: Path,
    recipe_files: Sequence[str] = BUILD_RECIPE_FILES,
    depfile_root: Path | None = None,
    tool_paths: Sequence[Path] = (),
) -> dict[str, Any]:
    """Fingerprint actual compiler dependencies, build recipes, tools and options.

    Compiler-generated .d files supply the exact source/header inputs. Build
    scripts and AMBuild recipes are included separately so changes that alter
    source selection or flags still invalidate the artifact identity.
    """
    source_root = source_root.resolve()
    dependency_root = depfile_root or source_root / "post"
    depfiles = sorted(dependency_root.rglob("*.d")) if dependency_root.exists() else []
    if not depfiles:
        raise ValueError(f"build contains no compiler dependency files under {dependency_root}")
    compile_commands, plugin_link_commands = _commands_from_build_log(log_path, source_root)
    if not compile_commands:
        raise ValueError(f"build log contains no compiler commands with dependency output: {log_path}")
    if not plugin_link_commands:
        raise ValueError(f"build log contains no link command for {ARTIFACT_NAME}: {log_path}")

    inputs: dict[tuple[str, str], dict[str, Any]] = {}

    def add_input(path: Path, role: str) -> None:
        record = _input_record(path, source_root, role)
        key = (record["scope"], record["path"])
        existing = inputs.get(key)
        if existing is None:
            inputs[key] = record
            return
        existing["roles"] = sorted(set(existing["roles"]) | {role})
        if role == "compiler_input" and "compiler_semantic_sha256" in record:
            existing["compiler_semantic_sha256"] = record["compiler_semantic_sha256"]
        if existing["sha256"] != record["sha256"] or existing["mode"] != record["mode"]:
            raise ValueError(f"build input changed while provenance was being collected: {path}")

    recipe_records: list[dict[str, Any]] = []
    for relative in recipe_files:
        path = source_root / relative
        if not path.is_file():
            raise ValueError(f"build recipe input is missing: {path}")
        add_input(path, "build_recipe")
        recipe_records.append({"path": relative, "sha256": _sha256_file(path), "mode": _file_mode(path)})

    for depfile in depfiles:
        for entry in _parse_make_depfile(depfile):
            dependency = Path(entry)
            if not dependency.is_absolute():
                dependency = depfile.parent / dependency
            try:
                resolved = dependency.resolve(strict=True)
            except (OSError, RuntimeError):
                raise ValueError(f"compiler dependency is missing or unreadable: {dependency}")
            try:
                relative = resolved.relative_to(source_root)
            except ValueError:
                relative = None
            # Generated AMBuild outputs are not source inputs. Their source
            # recipes and generator are included in BUILD_RECIPE_FILES.
            if relative is not None and (relative.parts[0] == "post" or ".ambuild2" in relative.parts):
                continue
            if not resolved.is_file():
                raise ValueError(f"compiler dependency is not a regular file: {resolved}")
            add_input(resolved, "compiler_input")

    tool_records: list[dict[str, Any]] = []
    for tool in tool_paths:
        if tool.is_file():
            add_input(tool, "toolchain")
            tool_records.append({"path": str(tool.resolve()), "sha256": _sha256_file(tool), "mode": _file_mode(tool)})

    input_records = sorted(inputs.values(), key=lambda item: (item["scope"], item["path"]))
    configuration = _normalize_build_value(dict(build_configuration), source_root)
    configuration["compile_commands"] = compile_commands
    configuration["plugin_link_commands"] = plugin_link_commands
    dependencies = configuration.get("compiler_sdk", {}).get("dependencies", {})
    payload = {
        "schema": PLUGIN_BUILD_SCHEMA,
        "source_head": _git(source_root, "rev-parse", "HEAD").decode().strip(),
        "inputs": input_records,
        "configuration": configuration,
    }
    return {
        **payload,
        "input_fingerprint_sha256": _json_sha256(payload),
        "recipe_files": recipe_records,
        "toolchain_files": tool_records,
        "dependency_repositories": dependencies,
    }


def verify_plugin_build_identity(recorded: Mapping[str, Any], current_repo: Path) -> dict[str, Any]:
    """Check recorded per-file build inputs against the current source/dependencies."""
    if recorded.get("schema") != PLUGIN_BUILD_SCHEMA:
        return {"verified": False, "status": "LEGACY_INSUFFICIENT", "error": "manifest lacks per-file plugin build inputs"}
    repo = current_repo.resolve()
    current_head = _git(repo, "rev-parse", "HEAD").decode().strip()
    build_source_head = recorded.get("source_head")
    line_ending_only_differences: list[str] = []
    for item in recorded.get("inputs", []):
        if not isinstance(item, dict) or item.get("scope") not in {"repository", "absolute"}:
            return {"verified": False, "status": "INVALID_INPUT_RECORD", "error": "invalid plugin build input record"}
        path = repo / str(item.get("path", "")) if item["scope"] == "repository" else Path(str(item.get("path", "")))
        if not path.is_file():
            return {"verified": False, "status": "MISSING_INPUT", "error": f"plugin build input is missing: {path}"}
        byte_identical = _sha256_file(path) == item.get("sha256")
        if not byte_identical and "compiler_input" in item.get("roles", []):
            actual_semantic_digest = _compiler_text_sha256(path)
            expected_semantic_digest = item.get("compiler_semantic_sha256", item.get("sha256"))
            if actual_semantic_digest is not None and actual_semantic_digest == expected_semantic_digest:
                line_ending_only_differences.append(str(path))
                byte_identical = True
        if not byte_identical or _file_mode(path) != item.get("mode"):
            return {"verified": False, "status": "INPUT_CHANGED", "error": f"plugin build input changed: {path}"}
    dependency_repositories = recorded.get("dependency_repositories", {})
    if not isinstance(dependency_repositories, dict):
        return {"verified": False, "status": "INVALID_DEPENDENCIES", "error": "dependency repository evidence is invalid"}
    for name, dependency in dependency_repositories.items():
        if not isinstance(dependency, dict) or not dependency.get("available"):
            return {"verified": False, "status": "DEPENDENCY_UNVERIFIED", "error": f"build dependency {name} was not captured"}
        current = _dependency_identity(Path(str(dependency.get("path", ""))))
        if current != dependency:
            return {"verified": False, "status": "DEPENDENCY_CHANGED", "error": f"build dependency changed since artifact creation: {name}"}
    payload = {
        "schema": recorded.get("schema"),
        "source_head": recorded.get("source_head"),
        "inputs": recorded.get("inputs"),
        "configuration": recorded.get("configuration"),
    }
    if _json_sha256(payload) != recorded.get("input_fingerprint_sha256"):
        return {"verified": False, "status": "FINGERPRINT_INVALID", "error": "plugin build input fingerprint is inconsistent"}
    return {
        "verified": True,
        "status": "VERIFIED",
        "input_fingerprint_sha256": recorded["input_fingerprint_sha256"],
        "build_source_head": build_source_head,
        "current_source_head": current_head,
        "source_head_matches": build_source_head == current_head,
        "input_count": len(recorded.get("inputs", [])),
        "compiler_line_ending_only_differences": line_ending_only_differences,
    }


def verify_artifact_deployment(artifact_path: Path, deployment_path: Path) -> dict[str, Any]:
    """Verify proof B independently of proof A."""
    if not artifact_path.is_file() or not deployment_path.is_file():
        return {"verified": False, "artifact_sha256": None, "deployment_sha256": None}
    artifact_sha = _sha256_file(artifact_path)
    deployment_sha = _sha256_file(deployment_path)
    return {
        "verified": artifact_sha == deployment_sha,
        "artifact_sha256": artifact_sha,
        "deployment_sha256": deployment_sha,
    }


def verify_loaded_plugin(artifact_sha256: str | None, deployment_sha256: str | None, loaded_sha256: str | None) -> dict[str, bool]:
    """Verify proof C without conflating the mapping with source/build proof."""
    return {
        "artifact_to_deployment_verified": bool(artifact_sha256 and artifact_sha256 == deployment_sha256),
        "loaded_plugin_matches_deployment": bool(deployment_sha256 and deployment_sha256 == loaded_sha256),
        "loaded_plugin_matches_artifact": bool(artifact_sha256 and artifact_sha256 == loaded_sha256),
    }


def harness_runtime_identity(bot_repo: Path, harness_repo: Path) -> dict[str, Any]:
    """Capture Python test infrastructure independently from plugin inputs."""
    components: list[dict[str, str]] = []
    for root, filenames, label in (
        (bot_repo.resolve(), HARNESS_RUNTIME_FILES, "v2"),
        (harness_repo.resolve(), V1_RUNTIME_FILES, "v1"),
    ):
        for relative in filenames:
            path = root / relative
            if not path.is_file():
                raise ValueError(f"harness runtime component is missing: {path}")
            components.append({"layer": label, "path": str(path), "sha256": _sha256_file(path)})
    payload = {"schema": HARNESS_RUNTIME_SCHEMA, "version": "2.2", "components": components}
    return {**payload, "fingerprint_sha256": _json_sha256(payload)}


def _git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def _excluded_snapshot_path(raw: str) -> bool:
    path = raw.rstrip("/")
    if path == "current-working-tree.patch":
        return True
    if (
        path == "post" or path.startswith("post/")
        or path == ".ambuild2" or path.startswith(".ambuild2/")
        or "/.ambuild2/" in path or path.endswith("/.ambuild2")
        or path in {"build", "Build", "build_linux", "premake_build", "out", "dist", "artifacts", "runs"}
        or any(path.startswith(prefix + "/") for prefix in (
            "build", "Build", "build_linux", "premake_build", "out", "dist", "artifacts", "runs",
        ))
        or path == "__pycache__" or path.startswith("__pycache__/")
        or "/__pycache__/" in path or path.endswith("/__pycache__")
    ):
        return True
    return path.endswith((
        ".log", ".tmp", ".temp", ".swp", "~", ".pyc", ".pyo", ".so", ".dylib",
        ".dll", ".exe", ".a", ".o", ".obj", ".core", ".dmp", ".dump",
    )) or bool(re.search(r"\.log\..+$", path)) or path == "core" or bool(re.search(r"(^|/)core\.[0-9]+$", path))


def source_identity(repo_root: Path) -> dict[str, Any]:
    """Return a stable identity for precisely the source inputs copied by the build wrapper."""
    repo = repo_root.resolve()
    head = _git(repo, "rev-parse", "HEAD").decode().strip()
    branch = _git(repo, "branch", "--show-current").decode().strip()
    tracked_patch = _git(
        repo,
        "diff",
        "HEAD",
        "--binary",
        "--",
        ".",
        ":(exclude)post/**",
        ":(exclude)**/.ambuild2/**",
    )
    changed_paths = _git(
        repo,
        "diff",
        "--name-only",
        "HEAD",
        "--",
        ".",
        ":(exclude)post/**",
        ":(exclude)**/.ambuild2/**",
    ).decode().splitlines()
    untracked_raw = _git(repo, "ls-files", "--others", "--exclude-standard", "-z")
    untracked_paths = [
        value.decode("utf-8", errors="surrogateescape")
        for value in untracked_raw.split(b"\0")
        if value and not _excluded_snapshot_path(value.decode("utf-8", errors="surrogateescape"))
    ]
    untracked_entries: list[dict[str, str]] = []
    for relative in sorted(untracked_paths):
        path = repo / relative
        if path.is_symlink():
            content_digest = hashlib.sha256(os.readlink(path).encode()).hexdigest()
            kind = "symlink"
        elif path.is_file():
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            content_digest = digest.hexdigest()
            kind = "file"
        else:
            continue
        untracked_entries.append({"path": relative, "kind": kind, "sha256": content_digest})
    tracked_patch_sha256 = hashlib.sha256(tracked_patch).hexdigest()
    fingerprint_payload = {
        "branch": branch,
        "head": head,
        "tracked_patch_sha256": tracked_patch_sha256,
        "untracked_source": untracked_entries,
    }
    fingerprint = hashlib.sha256(
        json.dumps(fingerprint_payload, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    status = _git(repo, "status", "--short", "--untracked-files=all").decode().splitlines()
    return {
        "repository_path": str(repo),
        "branch": branch,
        "head": head,
        "working_tree": "DIRTY" if changed_paths or untracked_entries else "CLEAN",
        "status_short": status,
        "tracked_changed_files": sorted(changed_paths),
        "tracked_patch_sha256": tracked_patch_sha256,
        "untracked_source_files": untracked_entries,
        "fingerprint_sha256": fingerprint,
    }


def _first_line(command: Sequence[str]) -> str | None:
    executable = shutil.which(command[0])
    if executable is None:
        return None
    try:
        result = subprocess.run(
            [executable, *command[1:]],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return next((line.strip() for line in result.stdout.splitlines() if line.strip()), None)


def _elf_build_id(artifact: Path) -> str | None:
    readelf = shutil.which("readelf")
    if readelf is None:
        return None
    try:
        result = subprocess.run(
            [readelf, "-n", str(artifact)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", result.stdout)
    return match.group(1).lower() if result.returncode == 0 and match else None


def _dependency_identity(path: Path) -> dict[str, Any]:
    """Record the checked-out revision and dirty state of one build dependency."""
    resolved = path.resolve()
    try:
        head = _git(resolved, "rev-parse", "HEAD").decode().strip()
        branch = _git(resolved, "branch", "--show-current").decode().strip()
        status = _git(resolved, "status", "--short", "--untracked-files=all").decode().splitlines()
        diff = _git(resolved, "diff", "HEAD", "--binary")
        untracked_raw = _git(resolved, "ls-files", "--others", "--exclude-standard", "-z")
    except (OSError, subprocess.CalledProcessError) as error:
        return {"path": str(resolved), "available": False, "error": str(error)}
    untracked_files: list[dict[str, Any]] = []
    for raw in untracked_raw.split(b"\0"):
        if not raw:
            continue
        relative = raw.decode("utf-8", errors="surrogateescape")
        candidate = resolved / relative
        if candidate.is_symlink():
            digest = hashlib.sha256(os.readlink(candidate).encode()).hexdigest()
            mode = _file_mode(candidate)
            kind = "symlink"
        elif candidate.is_file():
            digest = _sha256_file(candidate)
            mode = _file_mode(candidate)
            kind = "file"
        else:
            continue
        untracked_files.append({"path": relative, "kind": kind, "sha256": digest, "mode": mode})
    return {
        "path": str(resolved),
        "available": True,
        "head": head,
        "branch": branch,
        "working_tree": "DIRTY" if status else "CLEAN",
        "status_short": status,
        "tracked_patch_sha256": hashlib.sha256(diff).hexdigest(),
        "untracked_files": sorted(untracked_files, key=lambda item: item["path"]),
    }


def write_manifest(
    *,
    source_root: Path,
    source_identity_data: dict[str, Any],
    repository_path: Path,
    artifact: Path,
    log_path: Path,
    result: str,
    build_started_utc: str,
    python_path: str,
    ambuild_path: str,
) -> Path:
    artifact = artifact.resolve()
    digest = _sha256_file(artifact)
    source = dict(source_identity_data)
    source["build_snapshot_path"] = source.get("repository_path")
    source["repository_path"] = str(repository_path.resolve())
    compiler = {
        "cc": _first_line(["gcc", "--version"]),
        "cxx": _first_line(["g++", "--version"]),
        "python": _first_line([python_path, "--version"]),
        "ambuild": _first_line([ambuild_path, "--version"]) or str(Path(ambuild_path).resolve()),
    }
    compiler_sdk = {
        "compiler": "GCC/G++",
        "compiler_versions": compiler,
        "sdk": "sdk2013",
        "target": "x86",
        "optimized": True,
        "cxxflags": ["-Wno-stringop-truncation"],
        "symbol_files": False,
        "sdk_root": str((source_root / "alliedmodders").resolve()),
        "sdk2013_path": str((source_root / "alliedmodders/hl2sdk-sdk2013").resolve()),
        "metamod_path": str((source_root / "alliedmodders/metamod-source").resolve()),
        "sourcemod_path": str((source_root / "alliedmodders/sourcemod").resolve()),
        "manifest_path": str((source_root / "hl2sdk-manifests").resolve()),
        "dependencies": {
            "sdk2013": _dependency_identity(source_root / "alliedmodders/hl2sdk-sdk2013"),
            "metamod_source": _dependency_identity(source_root / "alliedmodders/metamod-source"),
            "sourcemod": _dependency_identity(source_root / "alliedmodders/sourcemod"),
            "sdk_manifests": _dependency_identity(source_root / "hl2sdk-manifests"),
        },
    }
    environment = {
        "CC": "gcc",
        "CXX": "g++",
        "CXXFLAGS": "-Wno-stringop-truncation",
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    configure_command = [
        python_path,
        "configure.py",
        "-s",
        "sdk2013",
        "--targets",
        "x86",
        "--enable-optimize",
        "--hl2sdk-root",
        str((source_root / "alliedmodders").resolve()),
        "--sm-path",
        str((source_root / "alliedmodders/sourcemod").resolve()),
        "--mms-path",
        str((source_root / "alliedmodders/metamod-source").resolve()),
    ]
    build = {
        "result": result,
        "environment": environment,
        "configure_command": configure_command,
        "wrapper_command": ["./scripts/build_empires.sh"],
        "wrapper_working_directory": str(repository_path.resolve()),
        "build_command": [str(Path(ambuild_path).resolve())],
        "build_working_directory": str((source_root / "post").resolve()),
        "compiler_sdk": compiler_sdk,
        "log_path": str(log_path.resolve()),
    }
    build_configuration = {
        key: build[key]
        for key in (
            "environment",
            "configure_command",
            "wrapper_command",
            "build_command",
            "build_working_directory",
            "compiler_sdk",
        )
    }
    tool_paths = [
        Path(value)
        for value in (
            shutil.which("gcc"),
            shutil.which("g++"),
            python_path,
            ambuild_path,
        )
        if value
    ]
    plugin_build = plugin_build_identity(
        source_root,
        build_configuration=build_configuration,
        log_path=log_path,
        tool_paths=tool_paths,
    )
    provenance_tool = source_root / "scripts/build_provenance.py"
    manifest = {
        "schema": SCHEMA,
        "build_id": str(uuid.uuid4()),
        "build_started_utc": build_started_utc,
        "build_completed_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source": source,
        "plugin_build_provenance": plugin_build,
        "harness_build_tool": {
            "path": "scripts/build_provenance.py",
            "sha256": _sha256_file(provenance_tool),
        },
        "build": build,
        "artifact": {
            "path": str(artifact),
            "sha256": digest,
            "elf_build_id": _elf_build_id(artifact),
        },
    }
    destination = artifact.parent / MANIFEST_NAME
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(destination)
    return destination


def _legacy_runtime_only_path(path: str) -> bool:
    normalized = path.replace("\\", "/").lstrip("./")
    if normalized == "EMPIRES_BUILD.md":
        return True
    return normalized.startswith("scripts/") and Path(normalized).suffix.lower() in {".py", ".md"}


def verify_legacy_source_identity(recorded: Mapping[str, Any], current: Mapping[str, Any]) -> dict[str, Any]:
    """Reconcile v1 manifests only when every changed path is harness-only.

    V1 recorded one aggregate tracked-patch hash, so a build helper or product
    source among the changed paths cannot be separated from a Python change.
    Such manifests remain explicitly insufficient and require a fresh build.
    """
    if recorded.get("head") != current.get("head"):
        return {"verified": False, "status": "SOURCE_HEAD_MISMATCH", "error": "legacy manifest source HEAD differs from current HEAD"}
    if recorded.get("branch") != current.get("branch"):
        return {"verified": False, "status": "SOURCE_BRANCH_MISMATCH", "error": "legacy manifest source branch differs from current branch"}
    if recorded.get("fingerprint_sha256") == current.get("fingerprint_sha256"):
        return {"verified": True, "status": "LEGACY_FULL_TREE_MATCH", "verification_scope": "full working-tree fingerprint"}

    changed_paths = set(recorded.get("tracked_changed_files", [])) | set(current.get("tracked_changed_files", []))
    recorded_untracked = {
        str(item.get("path")): item
        for item in recorded.get("untracked_source_files", [])
        if isinstance(item, dict) and item.get("path")
    }
    current_untracked = {
        str(item.get("path")): item
        for item in current.get("untracked_source_files", [])
        if isinstance(item, dict) and item.get("path")
    }
    for path in set(recorded_untracked) | set(current_untracked):
        if recorded_untracked.get(path) != current_untracked.get(path):
            changed_paths.add(path)
    if changed_paths and all(_legacy_runtime_only_path(path) for path in changed_paths):
        return {
            "verified": True,
            "status": "LEGACY_RUNTIME_ONLY_CHANGES_RECONCILED",
            "verification_scope": "same HEAD; changed paths are harness Python or documentation only",
            "reconciled_paths": sorted(changed_paths),
            "limitation": "v1 does not contain per-file hashes for build-relevant paths",
        }
    build_relevant = sorted(path for path in changed_paths if not _legacy_runtime_only_path(path))
    return {
        "verified": False,
        "status": "LEGACY_INSUFFICIENT",
        "error": "legacy manifest cannot isolate build-relevant changes from its aggregate working-tree fingerprint",
        "build_relevant_paths": build_relevant,
    }


def load_and_verify_manifest(
    manifest_path: Path,
    artifact_path: Path,
    current_repo: Path,
) -> dict[str, Any]:
    """Verify proof A; proof B is calculated independently by the caller."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schema") not in {SCHEMA, LEGACY_SCHEMA}:
        raise ValueError("unsupported build provenance manifest")
    build = manifest.get("build")
    if not isinstance(build, dict) or build.get("result") != "PASS":
        raise ValueError("build manifest does not record a successful build")
    artifact = manifest.get("artifact")
    if not isinstance(artifact, dict):
        raise ValueError("build manifest has no artifact identity")
    if not isinstance(manifest.get("source"), dict):
        raise ValueError("build manifest has no source identity")
    expected_path = Path(str(artifact.get("path", ""))).resolve()
    actual_path = artifact_path.resolve()
    if expected_path != actual_path or not actual_path.is_file():
        raise ValueError("build manifest artifact path does not match the expected build artifact")
    actual_digest = hashlib.sha256(actual_path.read_bytes()).hexdigest()
    if artifact.get("sha256") != actual_digest:
        raise ValueError("build artifact SHA-256 differs from its manifest")
    recorded_source = manifest.get("source")
    if not isinstance(recorded_source, dict):
        raise ValueError("build manifest has no source identity")
    current_source = source_identity(current_repo)
    if manifest["schema"] == SCHEMA:
        plugin_build_result = verify_plugin_build_identity(
            manifest.get("plugin_build_provenance", {}), current_repo
        )
    else:
        plugin_build_result = verify_legacy_source_identity(recorded_source, current_source)
    return {
        "manifest_path": str(manifest_path.resolve()),
        "build_id": manifest.get("build_id"),
        "source_to_artifact_verified": bool(plugin_build_result.get("verified")),
        "source_to_artifact_status": plugin_build_result.get("status", "UNKNOWN"),
        "source_to_artifact_error": plugin_build_result.get("error"),
        "plugin_build_verification": plugin_build_result,
        "source_head": current_source["head"],
        "source_branch": current_source["branch"],
        "source_fingerprint_sha256": current_source["fingerprint_sha256"],
        "manifest_schema": manifest.get("schema"),
        "artifact_path": str(actual_path),
        "artifact_sha256": actual_digest,
        "artifact_elf_build_id": artifact.get("elf_build_id"),
        "build_id_expected": manifest.get("build_id"),
    }


def _main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    snapshot = subparsers.add_parser("source-identity")
    snapshot.add_argument("--repo", type=Path, required=True)
    write = subparsers.add_parser("write-manifest")
    write.add_argument("--source-root", type=Path, required=True)
    write.add_argument("--source-identity-file", type=Path, required=True)
    write.add_argument("--repository-path", type=Path, required=True)
    write.add_argument("--artifact", type=Path, required=True)
    write.add_argument("--log-path", type=Path, required=True)
    write.add_argument("--result", choices=("PASS", "FAIL"), required=True)
    write.add_argument("--build-started-utc", required=True)
    write.add_argument("--python-path", required=True)
    write.add_argument("--ambuild-path", required=True)
    args = parser.parse_args(argv)
    if args.command == "source-identity":
        print(json.dumps(source_identity(args.repo), sort_keys=True))
        return 0
    if args.command == "write-manifest":
        path = write_manifest(
            source_root=args.source_root,
            source_identity_data=json.loads(args.source_identity_file.read_text(encoding="utf-8")),
            repository_path=args.repository_path,
            artifact=args.artifact,
            log_path=args.log_path,
            result=args.result,
            build_started_utc=args.build_started_utc,
            python_path=args.python_path,
            ambuild_path=args.ambuild_path,
        )
        print(path)
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(_main())

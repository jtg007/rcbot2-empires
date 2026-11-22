#!/usr/bin/env python3
"""Deterministic tests for build provenance verification."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_provenance import (  # noqa: E402
    BUILD_RECIPE_FILES,
    LEGACY_SCHEMA,
    SCHEMA,
    _dependency_identity,
    harness_runtime_identity,
    load_and_verify_manifest,
    plugin_build_identity,
    source_identity,
    verify_artifact_deployment,
    verify_loaded_plugin,
)


class BuildProvenanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="rcbot2-build-provenance-test-")
        self.root = Path(self.temporary.name)
        self.artifact = self.root / "rcbot.2.sdk2013.so"
        self.artifact.write_bytes(b"test plugin binary")
        self.manifest_path = self.root / "rcbot.2.sdk2013.build.json"
        self.source = {
            "repository_path": "/repo",
            "branch": "test-branch",
            "head": "a" * 40,
            "working_tree": "DIRTY",
            "fingerprint_sha256": "b" * 64,
            "tracked_changed_files": [],
            "untracked_source_files": [],
        }
        self.manifest = {
            "schema": LEGACY_SCHEMA,
            "build_id": "build-test",
            "source": dict(self.source),
            "build": {"result": "PASS"},
            "artifact": {
                "path": str(self.artifact.resolve()),
                "sha256": hashlib.sha256(self.artifact.read_bytes()).hexdigest(),
                "elf_build_id": "1234abcd",
            },
        }
        self._write_manifest()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write_manifest(self) -> None:
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def _verify(self, current_source: dict[str, object] | None = None) -> dict[str, object]:
        with patch("build_provenance.source_identity", return_value=current_source or self.source):
            return load_and_verify_manifest(self.manifest_path, self.artifact, Path("/repo"))

    def test_source_build_and_artifact_identity_are_verified(self) -> None:
        result = self._verify()
        self.assertTrue(result["source_to_artifact_verified"])
        self.assertEqual(result["artifact_sha256"], self.manifest["artifact"]["sha256"])

    def test_legacy_fingerprint_mismatch_without_path_details_stays_unverified(self) -> None:
        current = dict(self.source)
        current["fingerprint_sha256"] = "c" * 64
        result = self._verify(current)
        self.assertFalse(result["source_to_artifact_verified"])
        self.assertEqual(result["source_to_artifact_status"], "LEGACY_INSUFFICIENT")

    def test_legacy_python_harness_only_change_is_reconciled(self) -> None:
        recorded = dict(self.source)
        recorded["tracked_changed_files"] = ["scripts/harness_v2_orchestrator_test.py"]
        self.manifest["source"] = recorded
        self._write_manifest()
        current = dict(recorded)
        current["fingerprint_sha256"] = "c" * 64
        result = self._verify(current)
        self.assertTrue(result["source_to_artifact_verified"])
        self.assertEqual(
            result["source_to_artifact_status"],
            "LEGACY_RUNTIME_ONLY_CHANGES_RECONCILED",
        )

    def test_legacy_changed_build_helper_is_not_reconciled(self) -> None:
        recorded = dict(self.source)
        recorded["tracked_changed_files"] = ["scripts/build_empires.sh"]
        self.manifest["source"] = recorded
        self._write_manifest()
        current = dict(recorded)
        current["fingerprint_sha256"] = "c" * 64
        result = self._verify(current)
        self.assertFalse(result["source_to_artifact_verified"])
        self.assertEqual(result["source_to_artifact_status"], "LEGACY_INSUFFICIENT")

    def test_artifact_content_must_match_manifest_sha256(self) -> None:
        self.artifact.write_bytes(b"changed plugin binary")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self._verify()

    def test_failed_build_manifest_is_rejected(self) -> None:
        self.manifest["build"]["result"] = "FAIL"
        self._write_manifest()
        with self.assertRaisesRegex(ValueError, "successful build"):
            self._verify()


class PluginBuildProvenanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="rcbot2-plugin-inputs-test-")
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self._git(self.repo, "init", "-q")
        self._git(self.repo, "config", "user.name", "Provenance Test")
        self._git(self.repo, "config", "user.email", "provenance@example.invalid")
        for relative in BUILD_RECIPE_FILES:
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"recipe: {relative}\n", encoding="utf-8")
        for relative in (
            "scripts/combat_relevance_observer.py",
            "scripts/harness_v2_orchestrator.py",
            "scripts/build_provenance.py",
        ):
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"runtime: {relative}\n", encoding="utf-8")
        self.source_cpp = self.repo / "utils/example.cpp"
        self.source_cpp.parent.mkdir(parents=True, exist_ok=True)
        self.source_cpp.write_text("int plugin_source() { return 1; }\n", encoding="utf-8")
        self.header = self.repo / "utils/example.h"
        self.header.write_text("#define PLUGIN_VALUE 1\n", encoding="utf-8")
        self._git(self.repo, "add", "--all")
        self._git(self.repo, "commit", "-qm", "build input baseline")

        self.dependency = self.root / "sdk-dependency"
        self.dependency.mkdir()
        self._git(self.dependency, "init", "-q")
        self._git(self.dependency, "config", "user.name", "Dependency Test")
        self._git(self.dependency, "config", "user.email", "dependency@example.invalid")
        self.external_header = self.dependency / "public/sdk_header.h"
        self.external_header.parent.mkdir(parents=True)
        self.external_header.write_text("#define SDK_VALUE 1\n", encoding="utf-8")
        self._git(self.dependency, "add", "public/sdk_header.h")
        self._git(self.dependency, "commit", "-qm", "dependency baseline")

        self.post = self.repo / "post/obj"
        self.post.mkdir(parents=True)
        self.depfile = self.post / "example.d"
        self.object_file = self.post / "example.o"
        self.depfile.write_text(
            f"{self.object_file}: {self.source_cpp} {self.header} {self.external_header}\n",
            encoding="utf-8",
        )
        self.artifact = self.root / "rcbot.2.sdk2013.so"
        self.artifact.write_bytes(b"test plugin binary")
        self.log = self.root / "build.log"
        self.log.write_text(
            f"g++ -MD -MF {self.depfile} -c {self.source_cpp} -o {self.object_file}\n"
            f"g++ -shared -o {self.artifact} {self.object_file}\n",
            encoding="utf-8",
        )
        self.dependency_identity = _dependency_identity(self.dependency)
        self.configuration = {
            "environment": {"CC": "gcc", "CXX": "g++", "CXXFLAGS": "-O2"},
            "configure_command": ["python3", "configure.py", "-s", "sdk2013"],
            "compiler_sdk": {
                "sdk": "sdk2013",
                "target": "x86",
                "cxxflags": ["-O2"],
                "dependencies": {"sdk2013": self.dependency_identity},
            },
        }
        self.plugin_build = self._plugin_build()
        self.harness_repo = self.root / "external-harness"
        self.harness_repo.mkdir()
        for relative in ("run_harness.py", "soak_triage.py"):
            (self.harness_repo / relative).write_text(f"v1: {relative}\n", encoding="utf-8")
        self.manifest_path = self.root / "manifest.json"
        self.manifest = {
            "schema": SCHEMA,
            "build_id": "build-v2-test",
            "source": source_identity(self.repo),
            "plugin_build_provenance": self.plugin_build,
            "build": {"result": "PASS"},
            "artifact": {
                "path": str(self.artifact.resolve()),
                "sha256": hashlib.sha256(self.artifact.read_bytes()).hexdigest(),
                "elf_build_id": "1234abcd",
            },
        }
        self._write_manifest()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def _git(repo: Path, *args: str) -> None:
        subprocess.run(["git", "-C", str(repo), *args], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def _plugin_build(self) -> dict[str, object]:
        return plugin_build_identity(
            self.repo,
            build_configuration=deepcopy(self.configuration),
            log_path=self.log,
        )

    def _write_manifest(self) -> None:
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def _verify(self) -> dict[str, object]:
        return load_and_verify_manifest(self.manifest_path, self.artifact, self.repo)

    def test_harness_python_change_does_not_invalidate_plugin_inputs(self) -> None:
        runtime_before = harness_runtime_identity(self.repo, self.harness_repo)
        runtime_component = self.repo / "scripts/harness_v2_orchestrator.py"
        runtime_component.write_text("runtime changed after plugin build\n", encoding="utf-8")
        self._git(self.repo, "add", "--", "scripts/harness_v2_orchestrator.py")
        self._git(self.repo, "commit", "-qm", "harness-only runtime update")
        runtime_after = harness_runtime_identity(self.repo, self.harness_repo)
        result = self._verify()
        self.assertTrue(result["source_to_artifact_verified"])
        self.assertNotEqual(runtime_before["fingerprint_sha256"], runtime_after["fingerprint_sha256"])
        self.assertEqual(result["plugin_build_verification"]["input_fingerprint_sha256"], self.plugin_build["input_fingerprint_sha256"])
        self.assertFalse(result["plugin_build_verification"]["source_head_matches"])

    def test_changed_compiled_cpp_source_invalidates_plugin_inputs(self) -> None:
        self.source_cpp.write_text("int plugin_source() { return 2; }\n", encoding="utf-8")
        result = self._verify()
        self.assertFalse(result["source_to_artifact_verified"])
        self.assertEqual(result["source_to_artifact_status"], "INPUT_CHANGED")

    def test_crlf_checkout_of_compiler_input_keeps_equivalent_source_verified(self) -> None:
        original = self.source_cpp.read_bytes()
        self.source_cpp.write_bytes(original.replace(b"\n", b"\r\n"))
        result = self._verify()
        self.assertTrue(result["source_to_artifact_verified"])
        self.assertEqual(
            result["plugin_build_verification"]["compiler_line_ending_only_differences"],
            [str(self.source_cpp)],
        )

    def test_changed_used_header_invalidates_plugin_inputs(self) -> None:
        self.header.write_text("#define PLUGIN_VALUE 2\n", encoding="utf-8")
        result = self._verify()
        self.assertFalse(result["source_to_artifact_verified"])
        self.assertEqual(result["source_to_artifact_status"], "INPUT_CHANGED")

    def test_changed_build_configuration_recipe_invalidates_plugin_inputs(self) -> None:
        (self.repo / "configure.py").write_text("recipe: changed configure options\n", encoding="utf-8")
        result = self._verify()
        self.assertFalse(result["source_to_artifact_verified"])
        self.assertEqual(result["source_to_artifact_status"], "INPUT_CHANGED")

    def test_external_dependency_change_invalidates_plugin_inputs(self) -> None:
        self.external_header.write_text("#define SDK_VALUE 2\n", encoding="utf-8")
        result = self._verify()
        self.assertFalse(result["source_to_artifact_verified"])
        self.assertEqual(result["source_to_artifact_status"], "INPUT_CHANGED")

    def test_artifact_deployment_mismatch_is_reported_as_proof_b(self) -> None:
        deployment = self.root / "deployed.so"
        deployment.write_bytes(b"different deployed plugin")
        result = verify_artifact_deployment(self.artifact, deployment)
        self.assertFalse(result["verified"])
        self.assertNotEqual(result["artifact_sha256"], result["deployment_sha256"])

    def test_loaded_mapping_mismatch_is_reported_as_proof_c(self) -> None:
        result = verify_loaded_plugin("a" * 64, "a" * 64, "b" * 64)
        self.assertTrue(result["artifact_to_deployment_verified"])
        self.assertFalse(result["loaded_plugin_matches_deployment"])
        self.assertFalse(result["loaded_plugin_matches_artifact"])

    def test_new_report_records_harness_version_separately(self) -> None:
        runtime = harness_runtime_identity(self.repo, self.harness_repo)
        result = self._verify()
        report_provenance = {
            "build": result,
            "harness_runtime": runtime,
        }
        self.assertEqual(report_provenance["harness_runtime"]["version"], "2.2")
        self.assertNotIn("harness_runtime", report_provenance["build"])
        self.assertTrue(report_provenance["build"]["source_to_artifact_verified"])


if __name__ == "__main__":
    unittest.main()

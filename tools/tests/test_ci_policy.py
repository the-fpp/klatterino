import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RUN_REQUIRED_CTEST = REPOSITORY_ROOT / "tools" / "ci" / "run-required-ctest"
CI_WORKFLOW = REPOSITORY_ROOT / ".github" / "workflows" / "ci.yml"
DEPENDABOT = REPOSITORY_ROOT / ".github" / "dependabot.yml"


def job_block(workflow, job_name):
    match = re.search(
        rf"^  {re.escape(job_name)}:\n(?P<body>.*?)(?=^  [a-z0-9-]+:\n|\Z)",
        workflow,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        return ""
    return match.group("body")


def ci_policy_errors(workflow, dependabot):
    errors = []

    action_pattern = re.compile(
        r"^\s*uses:\s*(?P<action>[^@\s]+)@(?P<ref>[^\s#]+)"
        r"(?:\s+#\s*(?P<comment>.*))?$",
        flags=re.MULTILINE,
    )
    actions = list(action_pattern.finditer(workflow))
    if not actions:
        errors.append("workflow must use pinned actions")
    for match in actions:
        action = match.group("action")
        if action.startswith("./"):
            continue
        if re.fullmatch(r"[0-9a-f]{40}", match.group("ref")) is None:
            errors.append(f"{action} must use a full commit SHA")
        comment = match.group("comment") or ""
        if re.match(r"v[0-9]", comment) is None:
            errors.append(f"{action} must retain a readable version comment")
        node24_actions = {"actions/checkout", "actions/upload-artifact"}
        if action in node24_actions and not comment.startswith("v7."):
            errors.append(f"{action} must use its Node-24 v7 release")

    containers = re.findall(r"^\s*container:\s*(\S+)", workflow, re.MULTILINE)
    if not containers:
        errors.append("test container must be declared")
    for container in containers:
        if re.search(r"@sha256:[0-9a-f]{64}$", container) is None:
            errors.append("every job container must use a sha256 digest")

    required_pairs = {
        "HTTPBOX_TAG": r"HTTPBOX_TAG:\s*v[0-9][^\s]*",
        "HTTPBOX_SHA256": r"HTTPBOX_SHA256:\s*[0-9a-f]{64}",
        "TWITCH_PUBSUB_SERVER_TAG": (
            r"TWITCH_PUBSUB_SERVER_TAG:\s*v[0-9][^\s]*"
        ),
        "TWITCH_PUBSUB_SERVER_SHA256": (
            r"TWITCH_PUBSUB_SERVER_SHA256:\s*[0-9a-f]{64}"
        ),
    }
    for name, pattern in required_pairs.items():
        if re.search(pattern, workflow) is None:
            errors.append(f"{name} must be pinned")

    checksum_extract_pairs = (
        (
            'printf \'%s  %s\\n\' "$HTTPBOX_SHA256" /tmp/httpbox.tar.xz | sha256sum --check',
            "tar -xJf /tmp/httpbox.tar.xz",
        ),
        (
            'printf \'%s  %s\\n\' "$TWITCH_PUBSUB_SERVER_SHA256" /tmp/pubsub-server.tar.gz | sha256sum --check',
            "tar -xzf /tmp/pubsub-server.tar.gz",
        ),
    )
    for checksum, extraction in checksum_extract_pairs:
        checksum_index = workflow.find(checksum)
        extraction_index = workflow.find(extraction)
        if (
            checksum_index < 0
            or extraction_index < 0
            or checksum_index > extraction_index
        ):
            errors.append(f"checksum must precede extraction: {extraction}")

    forbidden_inputs = (
        "--impure",
        "nixos-unstable",
        "builtins.getFlake",
        "until-pass",
    )
    for forbidden in forbidden_inputs:
        if forbidden in workflow:
            errors.append(f"workflow must not contain {forbidden}")

    expected_contracts = {
        ("chatterino-nix-package", "evaluate"),
        ("chatterino-windows-portable", "evaluate"),
        ("chatterino-linux-appimage", "evaluate"),
        ("rumble-validation", "build"),
        ("rumble-credential-storage", "build"),
    }
    contract_entries = re.findall(
        r"^\s+- attribute:\s*([a-z0-9-]+)\n"
        r"\s+operation:\s*(evaluate|build)$",
        workflow,
        re.MULTILINE,
    )
    if len(contract_entries) != len(expected_contracts) or set(
        contract_entries
    ) != expected_contracts:
        errors.append("locked Nix matrix must preserve exact contract operations")

    nix_contract = job_block(workflow, "nix-contract")
    preflight = job_block(workflow, "preflight")
    integration_state = job_block(workflow, "integration-state")
    tests = job_block(workflow, "tests")
    required = job_block(workflow, "required")
    legacy = job_block(workflow, "legacy-nix-package")
    if not all((preflight, integration_state, nix_contract, tests, required, legacy)):
        errors.append("required workflow jobs must exist")
    for job_name, block in (
        ("integration-state", integration_state),
        ("nix-contract", nix_contract),
        ("tests", tests),
        ("full-nix-validation", job_block(workflow, "full-nix-validation")),
        ("portable-artifacts", job_block(workflow, "portable-artifacts")),
    ):
        if re.search(r"^    needs: preflight$", block, re.MULTILINE) is None:
            errors.append(f"{job_name} must wait for cheap preflight")
    for job_name in ("nix-contract", "tests"):
        block = job_block(workflow, job_name)
        if re.search(r"^    needs:\s+(?!preflight$)", block, re.MULTILINE):
            errors.append("ordinary Nix and C++ lanes must remain independent after preflight")
    if re.search(r"^\s+max-parallel:", nix_contract, re.MULTILINE):
        errors.append("locked Nix matrix must not cap parallel execution")
    for dependency in (
        "preflight",
        "integration-state",
        "nix-contract",
        "tests",
        "full-nix-validation",
        "portable-artifacts",
    ):
        if f"- {dependency}" not in required:
            errors.append(f"aggregate must depend on {dependency}")
    if "needs: required" not in legacy:
        errors.append("legacy context must depend only on the aggregate")
    if "needs.required.result" not in legacy:
        errors.append("legacy context must propagate aggregate failure")
    if "if: github.event_name == 'pull_request'" not in integration_state:
        errors.append("live integration state must run only for pull requests")
    if "INTEGRATION_STATE_RESULT" not in required:
        errors.append("aggregate must consume live integration state")
    if "PREFLIGHT_RESULT" not in required or 'test "$PREFLIGHT_RESULT" = success' not in required:
        errors.append("aggregate must require successful preflight")
    if '[[ "$EVENT_NAME" = pull_request ]]' not in required:
        errors.append("aggregate must distinguish pull-request integration state")
    if 'test "$INTEGRATION_STATE_RESULT" = skipped' not in required:
        errors.append("non-PR aggregate must accept skipped integration state")

    pull_request_events = (
        "types: [opened, synchronize, reopened, labeled, unlabeled]"
    )
    if pull_request_events not in workflow:
        errors.append("pull-request label changes must trigger CI")
    if "portable-artifacts'))" not in job_block(workflow, "full-nix-validation"):
        errors.append("full Nix validation must honor portable-artifacts opt-in")
    if "portable-artifacts'))" not in job_block(workflow, "portable-artifacts"):
        errors.append("portable builds must honor portable-artifacts opt-in")
    if "cancel-in-progress: true" not in workflow:
        errors.append("superseded workflow runs must be cancelled")
    preflight_checks = (
        "python3 -m compileall -q tools",
        "tools.tests.test_ci_policy",
        "tools.tests.test_pr_integration",
        "tools.tests.test_rumble_validation",
        "tools.tests.test_rumble_streamlink",
        "tools.tests.test_user_facing_string_audit",
    )
    for check in preflight_checks:
        if check not in preflight:
            errors.append(f"preflight must run {check}")
    policy_index = tests.find("Verify CI policy")
    install_index = tests.find("Install test dependencies")
    if policy_index < 0 or install_index < 0 or policy_index > install_index:
        errors.append("offline CI policy must run before dependency installation")

    if "package-ecosystem: github-actions" not in dependabot:
        errors.append("GitHub Actions updates must be automated")

    return errors


class RequiredTestPolicyTests(unittest.TestCase):
    def test_fail_then_pass_fake_stays_failed_without_retry(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            counter = temporary / "attempts"
            log = temporary / "ctest.log"
            fake_ctest = temporary / "fake-ctest"
            fake_ctest.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env bash
                    set -u
                    count=0
                    if [[ -f "$FAKE_CTEST_COUNTER" ]]; then
                        count="$(<"$FAKE_CTEST_COUNTER")"
                    fi
                    count=$((count + 1))
                    printf '%s\\n' "$count" >"$FAKE_CTEST_COUNTER"
                    printf 'synthetic ctest attempt %s\\n' "$count"
                    if [[ "$count" -eq 1 ]]; then
                        exit 17
                    fi
                    exit 0
                    """
                ),
                encoding="utf-8",
            )
            fake_ctest.chmod(0o755)

            environment = os.environ.copy()
            environment.update(
                {
                    "CTEST_COMMAND": str(fake_ctest),
                    "CTEST_LOG_PATH": str(log),
                    "FAKE_CTEST_COUNTER": str(counter),
                }
            )
            result = subprocess.run(
                [str(RUN_REQUIRED_CTEST), "--output-on-failure"],
                cwd=REPOSITORY_ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 17)
            self.assertEqual(counter.read_text(encoding="utf-8"), "1\n")
            self.assertIn("synthetic ctest attempt 1", result.stdout)
            self.assertIn("synthetic ctest attempt 1", log.read_text(encoding="utf-8"))

    def test_required_workflow_uses_the_tested_wrapper_without_retry(self):
        workflow = CI_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("tools/ci/run-required-ctest", workflow)
        self.assertNotIn("until-pass", workflow)

    def test_successful_ctest_does_not_mask_log_write_failure(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            fake_ctest = temporary / "fake-ctest"
            fake_ctest.write_text(
                "#!/usr/bin/env bash\nprintf 'synthetic success\\n'\n",
                encoding="utf-8",
            )
            fake_ctest.chmod(0o755)

            environment = os.environ.copy()
            environment.update(
                {
                    "CTEST_COMMAND": str(fake_ctest),
                    "CTEST_LOG_PATH": str(temporary),
                }
            )
            result = subprocess.run(
                [str(RUN_REQUIRED_CTEST)],
                cwd=REPOSITORY_ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("synthetic success", result.stdout)
            self.assertTrue(result.stderr.strip())


class WorkflowPolicyTests(unittest.TestCase):
    def setUp(self):
        self.workflow = CI_WORKFLOW.read_text(encoding="utf-8")
        self.dependabot = DEPENDABOT.read_text(encoding="utf-8")

    def assert_policy_rejects(self, workflow, expected):
        errors = ci_policy_errors(workflow, self.dependabot)
        self.assertTrue(
            any(expected in error for error in errors),
            f"expected {expected!r} in {errors!r}",
        )

    def test_repository_workflow_satisfies_offline_policy(self):
        self.assertEqual(ci_policy_errors(self.workflow, self.dependabot), [])

    def test_policy_rejects_floating_action(self):
        mutated = re.sub(
            r"actions/checkout@[0-9a-f]{40}",
            "actions/checkout@v7",
            self.workflow,
            count=1,
        )
        self.assert_policy_rejects(mutated, "full commit SHA")

    def test_policy_rejects_mutable_container(self):
        mutated = re.sub(
            r"@sha256:[0-9a-f]{64}(?=\s+# ubuntu-26.04)",
            ":latest",
            self.workflow,
            count=1,
        )
        self.assert_policy_rejects(mutated, "sha256 digest")

    def test_policy_rejects_checksum_after_extraction(self):
        checksum = (
            'printf \'%s  %s\\n\' "$HTTPBOX_SHA256" '
            "/tmp/httpbox.tar.xz | sha256sum --check"
        )
        extraction = "tar -xJf /tmp/httpbox.tar.xz -C /tmp"
        mutated = self.workflow.replace(
            f"          {checksum}\n          {extraction}",
            f"          {extraction}\n          {checksum}",
            1,
        )
        self.assertNotEqual(mutated, self.workflow)
        self.assert_policy_rejects(mutated, "checksum must precede extraction")

    def test_policy_rejects_impure_nix(self):
        mutated = self.workflow.replace(
            "nix flake metadata --no-write-lock-file",
            "nix flake metadata --no-write-lock-file --impure",
            1,
        )
        self.assert_policy_rejects(mutated, "--impure")

    def test_policy_rejects_serial_required_lanes(self):
        mutated = self.workflow.replace(
            "  tests:\n",
            "  tests:\n    needs: nix-contract\n",
            1,
        )
        self.assert_policy_rejects(mutated, "remain independent after preflight")

    def test_policy_rejects_missing_preflight_dependency(self):
        mutated = self.workflow.replace(
            "  portable-artifacts:\n    name: Build portable application artifacts\n    needs: preflight\n",
            "  portable-artifacts:\n    name: Build portable application artifacts\n",
            1,
        )
        self.assertNotEqual(mutated, self.workflow)
        self.assert_policy_rejects(mutated, "portable-artifacts must wait")

    def test_policy_rejects_missing_preflight_check(self):
        mutated = self.workflow.replace(
            "          tools.tests.test_rumble_streamlink\n",
            "",
            1,
        )
        self.assertNotEqual(mutated, self.workflow)
        self.assert_policy_rejects(mutated, "preflight must run tools.tests.test_rumble_streamlink")

    def test_policy_rejects_serial_nix_matrix(self):
        mutated = self.workflow.replace(
            "      fail-fast: false\n",
            "      fail-fast: false\n      max-parallel: 1\n",
            1,
        )
        self.assert_policy_rejects(mutated, "must not cap parallel")

    def test_policy_rejects_deterministic_build_downgrade(self):
        mutated = self.workflow.replace(
            "          - attribute: rumble-validation\n"
            "            operation: build\n",
            "          - attribute: rumble-validation\n"
            "            operation: evaluate\n",
            1,
        )
        self.assert_policy_rejects(mutated, "exact contract operations")

    def test_policy_rejects_missing_aggregate_dependency(self):
        mutated = self.workflow.replace(
            "      - portable-artifacts\n",
            "",
            1,
        )
        self.assert_policy_rejects(mutated, "aggregate must depend")

    def test_policy_rejects_unconditional_integration_state(self):
        conditional = (
            '          if [[ "$EVENT_NAME" = pull_request ]]; then\n'
            '            test "$INTEGRATION_STATE_RESULT" = success\n'
            "          else\n"
            '            test "$INTEGRATION_STATE_RESULT" = skipped\n'
            "          fi\n"
        )
        mutated = self.workflow.replace(
            conditional,
            '          test "$INTEGRATION_STATE_RESULT" = success\n',
            1,
        )
        self.assertNotEqual(mutated, self.workflow)
        self.assert_policy_rejects(mutated, "must distinguish pull-request")


if __name__ == "__main__":
    unittest.main()

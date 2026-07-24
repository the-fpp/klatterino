import argparse
import unittest
from unittest.mock import patch

from tools.pr_integration import (
    IntegrationError,
    classic_required_checks,
    command_merge,
    live_snapshot,
    parse_stack,
    required_policy_from_rulesets,
    verify,
)


def snapshot(number=2, head="child-head", base="parent-head"):
    return {
        "number": number,
        "state": "OPEN",
        "isDraft": False,
        "mergeable": "MERGEABLE",
        "headRefName": "child",
        "headRefOid": head,
        "baseRefName": "parent",
        "defaultBranch": "main",
        "baseRefOid": base,
        "mergeBaseCommit": {"oid": base},
        "unresolvedReviewThreads": 0,
        "reviewDecision": "APPROVED",
        "reviews": [{
            "state": "APPROVED",
            "commit": {"oid": head},
            "author": {"login": "reviewer"},
            "authorCanPushToRepository": True,
        }],
        "checks": [{"__typename": "CheckRun", "name": "CI", "status": "COMPLETED", "conclusion": "SUCCESS"}],
        "requiredChecks": ["CI"],
        "stack": {"parent": 1, "base": "parent", "tested_base": base},
    }


def parent(head="parent-head", state="OPEN"):
    value = snapshot(1, head, "main-head")
    value.update({"state": state, "headRefName": "parent", "stack": None})
    return value


def default_branch_ruleset(*rules):
    return {
        "enforcement": "active",
        "target": "branch",
        "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
        "rules": list(rules),
    }


def graphql_pull_request():
    return {
        "number": 9,
        "url": "https://example.invalid/pr/9",
        "state": "OPEN",
        "isDraft": False,
        "mergeable": "MERGEABLE",
        "reviewDecision": "APPROVED",
        "body": "",
        "headRefName": "feature",
        "headRefOid": "head-sha",
        "baseRefName": "main",
        "baseRefOid": "stale-base-sha",
        "baseRef": {
            "target": {"oid": "live-base-sha"},
            "branchProtectionRule": None,
        },
        "reviewThreads": {
            "nodes": [],
            "pageInfo": {"hasNextPage": False},
        },
        "reviews": {
            "nodes": [{
                "state": "APPROVED",
                "commit": {"oid": "head-sha"},
                "author": {"login": "reviewer"},
                "authorCanPushToRepository": True,
            }],
            "pageInfo": {"hasNextPage": False},
        },
        "statusCheckRollup": {
            "contexts": {
                "nodes": [{
                    "__typename": "CheckRun",
                    "name": "CI",
                    "status": "COMPLETED",
                    "conclusion": "SUCCESS",
                    "detailsUrl": "https://example.invalid/check",
                    "checkSuite": {
                        "app": {"databaseId": 42, "slug": "actions"}
                    },
                }],
                "pageInfo": {"hasNextPage": False},
            }
        },
    }


class StackFreshnessTest(unittest.TestCase):
    def test_current_linear_stack_is_ready(self):
        self.assertEqual(verify(snapshot(), parent()), [])

    def test_draft_is_reportable_but_not_merge_ready(self):
        child = snapshot()
        child["isDraft"] = True
        self.assertIn("pull request is draft", verify(child, parent()))
        self.assertEqual(verify(child, parent(), allow_draft=True), [])

    def test_merge_commit_containing_complete_parent_is_ready(self):
        # GitHub's compare result has the live parent as merge base when the
        # child merged that complete parent tree, even if it is not linear.
        child = snapshot(head="merge-commit", base="parent-head")
        self.assertEqual(verify(child, parent()), [])

    def test_green_child_becomes_stale_when_parent_advances(self):
        failures = verify(snapshot(), parent("new-parent-head"))
        self.assertIn("live child base differs from live parent head", failures)

    def test_green_child_becomes_stale_when_base_ref_advances(self):
        child = snapshot()
        child["baseRefOid"] = "new-parent-head"
        child["mergeBaseCommit"]["oid"] = "new-parent-head"
        failures = verify(child, parent("new-parent-head"))
        self.assertIn("tested stack base is stale", failures)

    def test_parent_merge_invalidates_child(self):
        self.assertIn("declared parent is no longer open", verify(snapshot(), parent(state="MERGED")))

    def test_divergent_child_is_rejected(self):
        child = snapshot()
        child["mergeBaseCommit"]["oid"] = "older-parent"
        self.assertIn(
            "child is not based on the complete live parent tree", verify(child, parent())
        )

    def test_conflict_is_rejected(self):
        child = snapshot()
        child["mergeable"] = "CONFLICTING"
        self.assertIn("mergeability is CONFLICTING", verify(child, parent()))

    def test_failed_exact_head_check_is_rejected(self):
        child = snapshot()
        child["checks"][0]["conclusion"] = "FAILURE"
        self.assertTrue(any("required checks not successful" in item for item in verify(child, parent())))

    def test_missing_required_check_is_rejected(self):
        child = snapshot()
        child["checks"] = []
        self.assertTrue(any("required checks not successful" in item for item in verify(child, parent())))

    def test_optional_failed_check_does_not_override_required_success(self):
        child = snapshot()
        child["checks"].append(
            {"__typename": "CheckRun", "name": "optional", "status": "COMPLETED", "conclusion": "FAILURE"}
        )
        self.assertEqual(verify(child, parent()), [])

    def test_unresolved_thread_is_rejected(self):
        child = snapshot()
        child["unresolvedReviewThreads"] = 1
        self.assertIn("1 unresolved review thread(s)", verify(child, parent()))

    def test_requested_changes_are_rejected(self):
        child = snapshot()
        child["reviewDecision"] = "CHANGES_REQUESTED"
        self.assertIn("review decision has requested changes", verify(child, parent()))

    def test_required_review_is_rejected_when_policy_requires_approval(self):
        child = snapshot()
        child.update({"reviewDecision": "REVIEW_REQUIRED", "requiredApprovals": 1})
        self.assertIn("review decision is not ready", verify(child, parent()))

    def test_review_required_is_rejected_even_without_required_approvals(self):
        child = snapshot()
        child.update({"reviewDecision": "REVIEW_REQUIRED", "requiredApprovals": 0})
        self.assertIn("review decision is not ready", verify(child, parent()))

    def test_stale_approval_is_not_exact_head_review_evidence(self):
        child = snapshot()
        child["reviews"][0]["commit"]["oid"] = "superseded-head"
        self.assertIn(
            "exact-head approving reviews are not ready: 0/1",
            verify(child, parent()),
        )

    def test_unqualified_exact_head_approval_cannot_launder_stale_approval(self):
        child = snapshot()
        child["reviews"] = [{
            "state": "APPROVED",
            "commit": {"oid": "superseded-head"},
            "author": {"login": "qualified-reviewer"},
            "authorCanPushToRepository": True,
        }, {
            "state": "APPROVED",
            "commit": {"oid": child["headRefOid"]},
            "author": {"login": "outside-reviewer"},
            "authorCanPushToRepository": False,
        }]
        self.assertIn(
            "exact-head approving reviews are not ready: 0/1",
            verify(child, parent()),
        )

    def test_required_approval_count_needs_distinct_exact_head_reviewers(self):
        child = snapshot()
        child["requiredApprovals"] = 2
        child["reviews"].append({
            "state": "APPROVED",
            "commit": {"oid": child["headRefOid"]},
            "author": {"login": "second-reviewer"},
            "authorCanPushToRepository": True,
        })
        self.assertEqual(verify(child, parent()), [])

    def test_ruleset_only_required_checks_are_collected(self):
        checks, approvals = required_policy_from_rulesets(
            [default_branch_ruleset({
                "type": "required_status_checks",
                "parameters": {"required_status_checks": [
                    {"context": "Build and run Chatterino tests", "integration_id": 42},
                    {"context": "Build Nix package"},
                ]},
            })],
            "main", "main",
        )
        self.assertEqual(checks, [
            {"context": "Build Nix package", "integrationId": None},
            {"context": "Build and run Chatterino tests", "integrationId": 42},
        ])
        self.assertEqual(approvals, 0)
        child = snapshot()
        child["requiredChecks"] = checks
        child["checks"] = []
        self.assertTrue(any(
            "required checks not successful" in failure
            for failure in verify(child, parent())
        ))

        child["checks"] = [{
            "__typename": "CheckRun",
            "name": "Build and run Chatterino tests",
            "status": "COMPLETED",
            "conclusion": "SUCCESS",
            "app": {"databaseId": 41, "slug": "wrong-app"},
        }, {
            "__typename": "CheckRun",
            "name": "Build Nix package",
            "status": "COMPLETED",
            "conclusion": "SUCCESS",
        }]
        failures = verify(child, parent())
        self.assertTrue(any("integration 42" in failure for failure in failures))
        child["checks"][0]["app"] = {"databaseId": 42, "slug": "right-app"}
        self.assertEqual(verify(child, parent()), [])

    def test_ruleset_only_review_requirement_is_collected(self):
        checks, approvals = required_policy_from_rulesets(
            [default_branch_ruleset({
                "type": "pull_request",
                "parameters": {"required_approving_review_count": 1},
            })],
            "main", "main",
        )
        self.assertEqual(checks, [])
        self.assertEqual(approvals, 1)

    def test_unknown_active_ruleset_pattern_fails_closed(self):
        with self.assertRaises(IntegrationError):
            required_policy_from_rulesets(
                [{**default_branch_ruleset(),
                  "conditions": {"ref_name": {"include": ["release/*"], "exclude": []}}}],
                "main", "main",
            )

    def test_incomplete_active_ruleset_summary_fails_closed(self):
        with self.assertRaisesRegex(IntegrationError, "detail is incomplete"):
            required_policy_from_rulesets(
                [{"id": 7, "enforcement": "active", "target": "branch"}],
                "main",
                "main",
            )

    def test_classic_required_check_preserves_graphql_app_binding(self):
        checks = classic_required_checks({
            "requiredStatusChecks": [{
                "context": "CI",
                "app": {"databaseId": 42, "slug": "actions"},
            }],
        })
        self.assertEqual(checks, [{"context": "CI", "integrationId": 42}])
        child = snapshot()
        child["requiredChecks"] = checks
        child["checks"][0]["app"] = {"databaseId": 999}
        self.assertTrue(any("integration 42" in item for item in verify(child, parent())))
        child["checks"][0]["app"] = {"databaseId": 42}
        self.assertEqual(verify(child, parent()), [])

    def test_specialized_ruleset_review_identity_fails_closed(self):
        for parameter in (
            "require_code_owner_review",
            "require_last_push_approval",
            "required_reviewers",
        ):
            value = [{"repository_role_database_id": 4}] if parameter == "required_reviewers" else True
            with self.subTest(parameter=parameter), self.assertRaisesRegex(
                IntegrationError, "cannot verify ruleset"
            ):
                required_policy_from_rulesets(
                    [default_branch_ruleset({
                        "type": "pull_request",
                        "parameters": {
                            "required_approving_review_count": 1,
                            parameter: value,
                        },
                    })],
                    "main",
                    "main",
                )

    @patch("tools.pr_integration.gh")
    def test_live_snapshot_uses_live_base_target_and_detailed_ruleset(self, mock_gh):
        pull_request = graphql_pull_request()
        pull_request["baseRef"]["branchProtectionRule"] = {
            "requiresStatusChecks": True,
            "requiredStatusChecks": [{
                "context": "Classic CI",
                "app": {"databaseId": 43, "slug": "classic-actions"},
            }],
            "requiresApprovingReviews": False,
            "requiredApprovingReviewCount": 0,
            "requiresCodeOwnerReviews": False,
            "requireLastPushApproval": False,
        }
        ruleset = default_branch_ruleset({
            "type": "required_status_checks",
            "parameters": {
                "required_status_checks": [{
                    "context": "CI",
                    "integration_id": 42,
                }]
            },
        })
        ruleset["id"] = 7
        mock_gh.side_effect = [
            {"data": {"repository": {
                "defaultBranchRef": {"name": "main"},
                "pullRequest": pull_request,
            }}},
            [[{"id": 7, "enforcement": "active", "target": "branch"}]],
            ruleset,
            {"merge_base_commit": {"sha": "live-base-sha"}},
        ]

        result = live_snapshot("owner/repo", 9)

        self.assertEqual(result["reportedBaseRefOid"], "stale-base-sha")
        self.assertEqual(result["baseRefOid"], "live-base-sha")
        self.assertEqual(result["mergeBaseCommit"]["oid"], "live-base-sha")
        self.assertEqual(result["requiredChecks"], [
            {"context": "CI", "integrationId": 42},
            {"context": "Classic CI", "integrationId": 43},
        ])
        self.assertEqual(len(mock_gh.call_args_list), 4)
        compare_call = mock_gh.call_args_list[-1].args[-1]
        self.assertIn("compare/live-base-sha...head-sha", compare_call)

    @patch("tools.pr_integration.live_rulesets")
    @patch("tools.pr_integration.gh")
    def test_live_snapshot_fails_closed_when_rulesets_cannot_be_loaded(
        self, mock_gh, mock_live_rulesets
    ):
        pull_request = graphql_pull_request()
        pull_request["baseRef"]["branchProtectionRule"] = {
            "requiresStatusChecks": False,
            "requiresApprovingReviews": True,
            "requiredApprovingReviewCount": 1,
            "requiresCodeOwnerReviews": False,
        }
        mock_gh.return_value = {"data": {"repository": {
            "defaultBranchRef": {"name": "main"},
            "pullRequest": pull_request,
        }}}
        mock_live_rulesets.side_effect = IntegrationError("API unavailable")

        with self.assertRaisesRegex(
            IntegrationError, "cannot resolve required policy from repository rulesets"
        ):
            live_snapshot("owner/repo", 9)

    @patch("tools.pr_integration.gh")
    def test_classic_last_push_approval_identity_fails_closed(self, mock_gh):
        pull_request = graphql_pull_request()
        pull_request["baseRef"]["branchProtectionRule"] = {
            "requiresStatusChecks": False,
            "requiresApprovingReviews": True,
            "requiredApprovingReviewCount": 1,
            "requiresCodeOwnerReviews": False,
            "requireLastPushApproval": True,
        }
        mock_gh.return_value = {"data": {"repository": {
            "defaultBranchRef": {"name": "main"},
            "pullRequest": pull_request,
        }}}

        with self.assertRaisesRegex(
            IntegrationError, "cannot verify classic last-push approval identity"
        ):
            live_snapshot("owner/repo", 9)
        self.assertEqual(mock_gh.call_count, 1)

    def test_metadata_is_strict(self):
        body = "<!-- integration-stack parent=12 base=feature tested-base=abc -->"
        self.assertEqual(
            parse_stack(body), {"parent": 12, "base": "feature", "tested_base": "abc"}
        )
        with self.assertRaises(IntegrationError):
            parse_stack("<!-- integration-stack parent=12 base=feature -->")

    def test_non_default_base_requires_stack_metadata(self):
        child = snapshot()
        child["stack"] = None
        self.assertIn(
            "non-default base requires integration-stack metadata", verify(child)
        )

    def test_independent_default_branch_does_not_require_metadata(self):
        child = snapshot()
        child.update({"baseRefName": "main", "baseRefOid": "main-head", "stack": None})
        child["mergeBaseCommit"]["oid"] = "main-head"
        self.assertEqual(verify(child), [])

    def test_independent_default_branch_must_include_live_base(self):
        child = snapshot()
        child.update({"baseRefName": "main", "baseRefOid": "new-main", "stack": None})
        child["mergeBaseCommit"]["oid"] = "old-main"
        self.assertIn(
            "pull request is not based on the live default branch",
            verify(child),
        )

    @patch("tools.pr_integration.gh")
    @patch("tools.pr_integration.live_ref_oid")
    @patch("tools.pr_integration.live_snapshot")
    def test_merge_rejects_base_advance_immediately_before_mutation(
        self, mock_snapshot, mock_live_ref_oid, mock_gh
    ):
        child = snapshot(head="expected-head", base="expected-base-sha")
        child.update({"baseRefName": "main", "defaultBranch": "main", "stack": None})
        child["mergeBaseCommit"]["oid"] = "expected-base-sha"
        mock_snapshot.return_value = child
        mock_live_ref_oid.return_value = "advanced-base-sha"
        args = argparse.Namespace(
            repo="owner/repo",
            pr=9,
            expected_head="expected-head",
            expected_base="main",
            expected_base_sha="expected-base-sha",
            method="squash",
        )

        with self.assertRaisesRegex(IntegrationError, "changed before merge mutation"):
            command_merge(args)
        mock_gh.assert_not_called()


if __name__ == "__main__":
    unittest.main()

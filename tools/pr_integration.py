#!/usr/bin/env python3
"""Derive pull-request integration decisions from live GitHub state.

The commands deliberately ignore prose claims about a "current" head.  Stack
metadata is machine-readable and historical verification remains ordinary PR
body text::

    <!-- integration-stack parent=132 base=agent/compact-tab-reveal tested-base=0123abcd... -->
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from typing import Any
from urllib.parse import quote


STACK_PREFIX = "<!-- integration-stack "


class IntegrationError(RuntimeError):
    pass


def gh(*args: str, input_text: str | None = None) -> Any:
    command = ["gh", *args]
    env = dict(os.environ)
    if os.getenv("GITHUB_TOKEN") and not os.getenv("GH_TOKEN"):
        env["GH_TOKEN"] = os.environ["GITHUB_TOKEN"]
    result = subprocess.run(
        command, input=input_text, text=True, capture_output=True, env=env, check=False
    )
    if result.returncode:
        raise IntegrationError(
            f"{' '.join(command)} failed: {result.stderr.strip() or result.stdout.strip()}"
        )
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise IntegrationError(f"{' '.join(command)} returned invalid JSON") from error


def parse_stack(body: str) -> dict[str, Any] | None:
    for line in body.splitlines():
        line = line.strip()
        if not (line.startswith(STACK_PREFIX) and line.endswith(" -->")):
            continue
        fields: dict[str, str] = {}
        for item in line[len(STACK_PREFIX) : -4].split():
            if "=" not in item:
                raise IntegrationError(f"invalid stack metadata item: {item}")
            key, value = item.split("=", 1)
            fields[key] = value
        required = {"parent", "base", "tested-base"}
        if fields.keys() != required:
            raise IntegrationError(
                "stack metadata must contain exactly parent, base, and tested-base"
            )
        try:
            parent = int(fields["parent"])
        except ValueError as error:
            raise IntegrationError("stack parent must be a pull-request number") from error
        return {"parent": parent, "base": fields["base"], "tested_base": fields["tested-base"]}
    return None


QUERY = r"""
query($owner:String!, $name:String!, $number:Int!) {
  repository(owner:$owner, name:$name) {
    defaultBranchRef { name }
    pullRequest(number:$number) {
      number url state isDraft mergeable reviewDecision body
      headRefName headRefOid baseRefName baseRefOid
      baseRef {
        target { oid }
        branchProtectionRule {
          pattern requiresStatusChecks
          requiredStatusChecks { context app { databaseId slug } }
          requiresApprovingReviews requiredApprovingReviewCount
          requiresCodeOwnerReviews requireLastPushApproval
        }
      }
      reviews(first:100) {
        nodes {
          state commit { oid } author { login }
          authorCanPushToRepository
        }
        pageInfo { hasNextPage }
      }
      reviewThreads(first:100) { nodes { isResolved } pageInfo { hasNextPage } }
      statusCheckRollup { contexts(first:100) {
        nodes {
          __typename
          ... on CheckRun {
            name status conclusion detailsUrl
            checkSuite { app { databaseId slug } }
          }
          ... on StatusContext { context state targetUrl }
        }
        pageInfo { hasNextPage }
      } }
    }
  }
}
"""


REF_QUERY = r"""
query($owner:String!, $name:String!, $qualifiedName:String!) {
  repository(owner:$owner, name:$name) {
    ref(qualifiedName:$qualifiedName) { target { oid } }
  }
}
"""


def split_repo(repo: str) -> tuple[str, str]:
    try:
        owner, name = repo.split("/", 1)
    except ValueError as error:
        raise IntegrationError("repository must be OWNER/NAME") from error
    if not owner or not name:
        raise IntegrationError("repository must be OWNER/NAME")
    return owner, name


def rule_matches_ref(rule: dict[str, Any], ref: str, default_branch: str) -> bool:
    """Return whether a repository branch ruleset applies to ``ref``.

    GitHub's REST response represents its special default-branch matcher as
    ``~DEFAULT_BRANCH``.  Other glob forms cannot safely be reimplemented
    here, so an active rule with one is deliberately rejected by the caller.
    """
    conditions = rule.get("conditions") or {}
    ref_name = conditions.get("ref_name") or {}
    includes = ref_name.get("include", [])
    excludes = ref_name.get("exclude", [])
    if not includes:
        return False
    supported = {"~DEFAULT_BRANCH", ref}
    if any(pattern not in supported for pattern in includes + excludes):
        raise IntegrationError("cannot safely evaluate repository ruleset ref pattern")
    return ("~DEFAULT_BRANCH" in includes and ref == default_branch or ref in includes) and ref not in excludes


def required_policy_from_rulesets(
    rulesets: list[dict[str, Any]], ref: str, default_branch: str
) -> tuple[list[dict[str, Any]], int]:
    """Derive required check and approval policy from active matching rulesets."""
    checks: dict[tuple[str, int | None], dict[str, Any]] = {}
    approvals = 0
    for ruleset in rulesets:
        if ruleset.get("enforcement") != "active" or ruleset.get("target") != "branch":
            continue
        if "conditions" not in ruleset or "rules" not in ruleset:
            raise IntegrationError("repository ruleset detail is incomplete")
        if not rule_matches_ref(ruleset, ref, default_branch):
            continue
        for rule in ruleset.get("rules", []):
            rule_type = rule.get("type")
            parameters = rule.get("parameters") or {}
            if rule_type == "required_status_checks":
                for check in parameters.get("required_status_checks", []):
                    context = check.get("context")
                    if not isinstance(context, str) or not context:
                        raise IntegrationError("repository ruleset has an invalid required check")
                    integration_id = check.get("integration_id")
                    if integration_id is not None and (
                        not isinstance(integration_id, int) or integration_id <= 0
                    ):
                        raise IntegrationError(
                            "repository ruleset has an invalid required-check integration"
                        )
                    checks[(context, integration_id)] = {
                        "context": context,
                        "integrationId": integration_id,
                    }
            elif rule_type == "pull_request":
                if parameters.get("require_code_owner_review"):
                    raise IntegrationError(
                        "cannot verify ruleset code-owner approval identity"
                    )
                if parameters.get("require_last_push_approval"):
                    raise IntegrationError(
                        "cannot verify ruleset last-push approval identity"
                    )
                if parameters.get("required_reviewers"):
                    raise IntegrationError(
                        "cannot verify ruleset required-reviewer identity"
                    )
                count = parameters.get("required_approving_review_count", 0)
                if not isinstance(count, int) or count < 0:
                    raise IntegrationError("repository ruleset has an invalid approval count")
                approvals = max(approvals, count)
    return [checks[key] for key in sorted(checks, key=lambda item: (item[0], item[1] or 0))], approvals


def live_rulesets(repo: str) -> list[dict[str, Any]]:
    """Load complete ruleset objects rather than incomplete list summaries."""
    pages = gh(
        "api",
        "--paginate",
        "--slurp",
        f"repos/{repo}/rulesets?includes_parents=true&per_page=100",
    )
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise IntegrationError("repository ruleset list has an invalid response shape")

    details = []
    for summary in (item for page in pages for item in page):
        ruleset_id = summary.get("id") if isinstance(summary, dict) else None
        if not isinstance(ruleset_id, int) or ruleset_id <= 0:
            raise IntegrationError("repository ruleset summary has an invalid id")
        detail = gh("api", f"repos/{repo}/rulesets/{ruleset_id}")
        if not isinstance(detail, dict) or detail.get("id") != ruleset_id:
            raise IntegrationError("repository ruleset detail has an invalid response shape")
        details.append(detail)
    return details


def classic_required_checks(protection: dict[str, Any]) -> list[dict[str, Any]]:
    """Read classic protection checks and app bindings from GraphQL."""
    checks = protection.get("requiredStatusChecks")
    if not isinstance(checks, list):
        raise IntegrationError("classic required-check policy has an invalid response shape")
    policies = []
    for check in checks:
        context = check.get("context") if isinstance(check, dict) else None
        app = check.get("app") if isinstance(check, dict) else None
        app_id = app.get("databaseId") if isinstance(app, dict) else None
        if not isinstance(context, str) or not context:
            raise IntegrationError("classic required-check policy has an invalid context")
        if app_id is not None and (not isinstance(app_id, int) or app_id <= 0):
            raise IntegrationError("classic required-check policy has an invalid app id")
        policies.append({"context": context, "integrationId": app_id})
    return policies


def live_ref_oid(repo: str, ref: str) -> str:
    """Resolve a ref target for the final pre-mutation base guard."""
    owner, name = split_repo(repo)
    result = gh(
        "api",
        "graphql",
        "-f",
        f"query={REF_QUERY}",
        "-F",
        f"owner={owner}",
        "-F",
        f"name={name}",
        "-F",
        f"qualifiedName=refs/heads/{ref}",
    )
    oid = (
        result.get("data", {})
        .get("repository", {})
        .get("ref")
        or {}
    ).get("target", {}).get("oid")
    if not isinstance(oid, str) or not oid:
        raise IntegrationError(f"cannot resolve live ref target for {ref}")
    return oid


def live_snapshot(repo: str, number: int) -> dict[str, Any]:
    owner, name = split_repo(repo)
    result = gh(
        "api", "graphql", "-f", f"query={QUERY}", "-F", f"owner={owner}",
        "-F", f"name={name}", "-F", f"number={number}",
    )
    repository = result.get("data", {}).get("repository", {})
    pr = repository.get("pullRequest")
    if not pr:
        raise IntegrationError(f"pull request #{number} was not found")
    threads = pr.pop("reviewThreads")
    reviews = pr.pop("reviews")
    base_ref = pr.pop("baseRef") or {}
    protection = base_ref.get("branchProtectionRule")
    live_base_oid = (base_ref.get("target") or {}).get("oid")
    if not isinstance(live_base_oid, str) or not live_base_oid:
        raise IntegrationError("cannot resolve the live base branch target")
    pr["reportedBaseRefOid"] = pr["baseRefOid"]
    pr["baseRefOid"] = live_base_oid
    contexts = (pr.pop("statusCheckRollup") or {"contexts": {"nodes": [], "pageInfo": {}}})["contexts"]
    if (
        threads["pageInfo"]["hasNextPage"]
        or reviews["pageInfo"]["hasNextPage"]
        or contexts["pageInfo"]["hasNextPage"]
    ):
        raise IntegrationError(
            "more than 100 reviews, review threads, or checks; refusing partial state"
        )
    pr["unresolvedReviewThreads"] = sum(not node["isResolved"] for node in threads["nodes"])
    pr["reviews"] = reviews["nodes"]
    pr["defaultBranch"] = repository["defaultBranchRef"]["name"]
    pr["checks"] = contexts["nodes"]
    pr["requiredChecks"] = (
        classic_required_checks(protection)
        if protection and protection["requiresStatusChecks"]
        else []
    )
    pr["requiredApprovals"] = (
        protection.get("requiredApprovingReviewCount", 0)
        if protection and protection.get("requiresApprovingReviews") else 0
    )
    if protection and protection.get("requiresCodeOwnerReviews"):
        raise IntegrationError("cannot verify classic code-owner approval identity")
    if protection and protection.get("requireLastPushApproval"):
        raise IntegrationError("cannot verify classic last-push approval identity")
    try:
        rulesets = live_rulesets(repo)
    except IntegrationError as error:
        raise IntegrationError("cannot resolve required policy from repository rulesets") from error
    else:
        ruleset_checks, ruleset_approvals = required_policy_from_rulesets(
            rulesets, pr["baseRefName"], pr["defaultBranch"]
        )
        combined = {
            (check["context"], check["integrationId"]): check
            for check in pr["requiredChecks"] + ruleset_checks
        }
        pr["requiredChecks"] = [
            combined[key]
            for key in sorted(combined, key=lambda item: (item[0], item[1] or 0))
        ]
        pr["requiredApprovals"] = max(pr["requiredApprovals"], ruleset_approvals)
    pr["stack"] = parse_stack(pr.pop("body") or "")
    comparison = gh(
        "api",
        f"repos/{repo}/compare/{quote(pr['baseRefOid'], safe='')}...{quote(pr['headRefOid'], safe='')}",
    )
    pr["mergeBaseCommit"] = {"oid": comparison["merge_base_commit"]["sha"]}
    return pr


def load_snapshot(path: str) -> dict[str, Any]:
    if path == "-":
        return json.load(sys.stdin)
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def check_success(check: dict[str, Any]) -> bool:
    if check["__typename"] == "CheckRun":
        return check.get("status") == "COMPLETED" and check.get("conclusion") in {
            "SUCCESS", "NEUTRAL", "SKIPPED"
        }
    return check.get("state") == "SUCCESS"


def check_name(check: dict[str, Any]) -> str:
    return check.get("name", check.get("context", "unknown"))


def required_check_policy(value: str | dict[str, Any]) -> tuple[str, int | None]:
    if isinstance(value, str):
        return value, None
    context = value.get("context")
    integration_id = value.get("integrationId")
    if not isinstance(context, str) or not context:
        raise IntegrationError("required-check policy has an invalid context")
    if integration_id is not None and (
        not isinstance(integration_id, int) or integration_id <= 0
    ):
        raise IntegrationError("required-check policy has an invalid integration")
    return context, integration_id


def check_matches_policy(check: dict[str, Any], policy: str | dict[str, Any]) -> bool:
    context, integration_id = required_check_policy(policy)
    if check_name(check) != context:
        return False
    if integration_id is None:
        return True
    app = check.get("app") or (check.get("checkSuite") or {}).get("app") or {}
    return app.get("databaseId") == integration_id


def required_check_label(policy: str | dict[str, Any]) -> str:
    context, integration_id = required_check_policy(policy)
    return context if integration_id is None else f"{context} (integration {integration_id})"


def exact_head_approvals(snapshot: dict[str, Any]) -> set[str]:
    """Return distinct reviewers whose effective approval names this head."""
    head = snapshot["headRefOid"]
    approvals = set()
    for review in snapshot.get("reviews", []):
        author = (review.get("author") or {}).get("login")
        commit = (review.get("commit") or {}).get("oid")
        if (
            review.get("state") == "APPROVED"
            and commit == head
            and author
            and review.get("authorCanPushToRepository") is True
        ):
            approvals.add(author)
    return approvals


def verify(
    snapshot: dict[str, Any],
    parent: dict[str, Any] | None = None,
    *,
    check_readiness: bool = True,
    allow_draft: bool = False,
) -> list[str]:
    failures: list[str] = []
    if snapshot["state"] != "OPEN":
        failures.append("pull request is not open")
    if snapshot["isDraft"] and not allow_draft:
        failures.append("pull request is draft")
    if snapshot["mergeable"] != "MERGEABLE":
        failures.append(f"mergeability is {snapshot['mergeable']}")
    if snapshot["unresolvedReviewThreads"]:
        failures.append(f"{snapshot['unresolvedReviewThreads']} unresolved review thread(s)")
    review_decision = snapshot.get("reviewDecision")
    exact_approvals = exact_head_approvals(snapshot)
    if review_decision == "CHANGES_REQUESTED":
        failures.append("review decision has requested changes")
    if review_decision == "REVIEW_REQUIRED":
        failures.append("review decision is not ready")
    elif snapshot.get("requiredApprovals", 0) and review_decision != "APPROVED":
        failures.append("required approving review is not ready")
    needed_exact_approvals = snapshot.get("requiredApprovals", 0)
    if review_decision == "APPROVED":
        needed_exact_approvals = max(needed_exact_approvals, 1)
    if len(exact_approvals) < needed_exact_approvals:
        failures.append(
            "exact-head approving reviews are not ready: "
            f"{len(exact_approvals)}/{needed_exact_approvals}"
        )
    if check_readiness:
        required = snapshot.get("requiredChecks", [])
        failed = [
            required_check_label(policy)
            for policy in required
            if not any(
                check_matches_policy(check, policy) and check_success(check)
                for check in snapshot["checks"]
            )
        ]
        if failed:
            failures.append("required checks not successful for live head: " + ", ".join(failed))

    stack = snapshot.get("stack")
    if not stack and snapshot["baseRefName"] != snapshot.get("defaultBranch", "main"):
        failures.append("non-default base requires integration-stack metadata")
    if (
        not stack
        and snapshot["baseRefName"] == snapshot.get("defaultBranch", "main")
        and snapshot["mergeBaseCommit"]["oid"] != snapshot["baseRefOid"]
    ):
        failures.append("pull request is not based on the live default branch")
    if stack:
        if parent is None:
            failures.append("stacked pull request requires live parent state")
        else:
            if parent["number"] != stack["parent"]:
                failures.append("loaded parent does not match declared parent")
            if parent["state"] != "OPEN":
                failures.append("declared parent is no longer open")
            if snapshot["baseRefName"] != stack["base"]:
                failures.append("live base ref differs from declared stack base")
            if parent["headRefName"] != stack["base"]:
                failures.append("declared base ref differs from parent head ref")
            if snapshot["baseRefOid"] != parent["headRefOid"]:
                failures.append("live child base differs from live parent head")
            if stack["tested_base"] != snapshot["baseRefOid"]:
                failures.append("tested stack base is stale")
            if snapshot["mergeBaseCommit"]["oid"] != snapshot["baseRefOid"]:
                failures.append("child is not based on the complete live parent tree")
    return failures


def render(snapshot: dict[str, Any], failures: list[str]) -> str:
    checks = ", ".join(
        f"{check_name(c)}={c.get('conclusion', c.get('state', c.get('status')))}"
        for c in snapshot["checks"]
    ) or "none"
    lines = [
        f"PR #{snapshot['number']}",
        f"head: {snapshot['headRefName']} @ {snapshot['headRefOid']}",
        f"base: {snapshot['baseRefName']} @ {snapshot['baseRefOid']}",
        f"merge-base: {snapshot['mergeBaseCommit']['oid']}",
        f"draft/mergeable: {snapshot['isDraft']}/{snapshot['mergeable']}",
        f"unresolved review threads: {snapshot['unresolvedReviewThreads']}",
        f"review decision: {snapshot.get('reviewDecision') or 'none'}",
        f"exact-head checks: {checks}",
        "required checks: "
        + (
            ", ".join(
                required_check_label(policy)
                for policy in snapshot.get("requiredChecks", [])
            )
            or "none"
        ),
        f"integration decision: {'BLOCKED' if failures else 'READY'}",
    ]
    lines.extend(f"- {failure}" for failure in failures)
    return "\n".join(lines)


def snapshot_for(args: argparse.Namespace, number: int) -> dict[str, Any]:
    return load_snapshot(args.snapshot) if args.snapshot else live_snapshot(args.repo, number)


def command_report(args: argparse.Namespace) -> int:
    snapshot = snapshot_for(args, args.pr)
    parent = None
    if snapshot.get("stack"):
        parent = load_snapshot(args.parent_snapshot) if args.parent_snapshot else live_snapshot(
            args.repo, snapshot["stack"]["parent"]
        )
    failures = verify(
        snapshot,
        parent,
        check_readiness=not args.skip_check_readiness,
        allow_draft=args.allow_draft,
    )
    print(render(snapshot, failures))
    return 1 if failures else 0


def command_report_open(args: argparse.Namespace) -> int:
    pages = gh("api", "--paginate", "--slurp", f"repos/{args.repo}/pulls?state=open&per_page=100")
    pulls = [pull for page in pages for pull in page]
    blocked = False
    for index, pull in enumerate(pulls):
        snapshot = live_snapshot(args.repo, pull["number"])
        parent = live_snapshot(args.repo, snapshot["stack"]["parent"]) if snapshot.get("stack") else None
        failures = verify(snapshot, parent)
        if index:
            print("\n---")
        print(render(snapshot, failures))
        blocked = blocked or bool(failures)
    return 1 if blocked else 0


def command_merge(args: argparse.Namespace) -> int:
    snapshot = live_snapshot(args.repo, args.pr)
    if snapshot["headRefOid"] != args.expected_head:
        raise IntegrationError("live head does not match --expected-head")
    if snapshot["baseRefName"] != args.expected_base:
        raise IntegrationError("live base ref does not match --expected-base")
    if snapshot["baseRefOid"] != args.expected_base_sha:
        raise IntegrationError("live base SHA does not match --expected-base-sha")
    parent = live_snapshot(args.repo, snapshot["stack"]["parent"]) if snapshot.get("stack") else None
    failures = verify(snapshot, parent)
    if failures:
        raise IntegrationError("merge blocked: " + "; ".join(failures))
    # GitHub atomically guards the head only, so resolve the base again after
    # every other read and immediately before the mutation.
    final_base_oid = live_ref_oid(args.repo, args.expected_base)
    if final_base_oid != args.expected_base_sha:
        raise IntegrationError("live base SHA changed before merge mutation")
    result = gh(
        "api", "--method", "PUT", f"repos/{args.repo}/pulls/{args.pr}/merge",
        "-f", f"merge_method={args.method}", "-f", f"sha={args.expected_head}",
    )
    if not result.get("merged"):
        raise IntegrationError(f"GitHub refused merge: {result.get('message', 'unknown error')}")
    print(json.dumps(result, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--repo", default=os.getenv("GITHUB_REPOSITORY"))
    commands = result.add_subparsers(dest="command", required=True)
    report = commands.add_parser("report")
    report.add_argument("--pr", required=True, type=int)
    report.add_argument("--snapshot")
    report.add_argument("--parent-snapshot")
    report.add_argument(
        "--skip-check-readiness",
        action="store_true",
        help="report checks without gating on them (for the check currently running)",
    )
    report.add_argument(
        "--allow-draft",
        action="store_true",
        help="report draft state without failing (never used by merge)",
    )
    report.set_defaults(run=command_report)
    report_open = commands.add_parser("report-open")
    report_open.set_defaults(run=command_report_open)
    merge = commands.add_parser("merge")
    merge.add_argument("--pr", required=True, type=int)
    merge.add_argument("--expected-head", required=True)
    merge.add_argument("--expected-base", required=True)
    merge.add_argument("--expected-base-sha", required=True)
    merge.add_argument("--method", choices=("merge", "squash", "rebase"), default="squash")
    merge.set_defaults(run=command_merge)
    return result


def main() -> int:
    args = parser().parse_args()
    if not args.repo and not getattr(args, "snapshot", None):
        raise IntegrationError("--repo or GITHUB_REPOSITORY is required")
    return args.run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except IntegrationError as error:
        print(f"integration error: {error}", file=sys.stderr)
        raise SystemExit(2)

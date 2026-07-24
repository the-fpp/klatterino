# Pull-request integration

Integration decisions use live GitHub metadata. A SHA or green-check claim in
ordinary PR prose is a historical verification note, never current evidence.

## Independent pull requests

Generate the decision immediately before merging:

```sh
python3 tools/pr_integration.py --repo OWNER/REPO report --pr NUMBER
python3 tools/pr_integration.py --repo OWNER/REPO report-open
python3 tools/pr_integration.py --repo OWNER/REPO merge --pr NUMBER \
  --expected-head HEAD_SHA --expected-base main --expected-base-sha BASE_SHA
```

Copy the head, base, and base SHA from the first command. The merge command
fetches them again, blocks drafts, conflicts, unresolved review threads, and
non-successful checks, and passes the expected head to GitHub's atomic merge
guard. If any value moved, rerun review and CI rather than updating arguments.
The base SHA comes from the live base ref target, not the pull request's
potentially stale `baseRefOid` summary field.
The CI workflow uses `--skip-check-readiness` only because its own check is
still running; it still prints the exact-head checks and enforces every other
policy. It also uses `--allow-draft` so drafts can iterate while their draft
state remains visible. Never use either option for a merge decision; the merge
command always rejects drafts and incomplete required checks.

Required checks and approvals are the union of classic branch protection and
every active matching repository ruleset. The tool expands ruleset-list
summaries through each ruleset's detail endpoint and fails closed on incomplete
policy, unsupported ref patterns, or invalid values. Classic protection checks
and their app bindings come from the same permission-compatible GraphQL query.
When any required status check names a GitHub App integration, both its context
and integration ID must match the successful check run. Approvals must name the
current head commit, come from distinct reviewers, and come from reviewers who
can push to the repository; a stale or unqualified approval is not exact-head
review evidence. Code-owner, last-push, or explicit-reviewer rules fail closed
when their identity semantics cannot be proven. A `REVIEW_REQUIRED` decision is
never ready, even when the resolved approval count is zero.

Immediately before the merge request, the tool resolves the live base target a
second time. GitHub's merge API atomically guards the expected head; this final
read supplies the corresponding base guard and blocks if the branch advanced
during policy, check, or stack evaluation.

The workflow integration-state job runs only for pull requests. The aggregate
requires it to succeed for pull-request events and requires it to be skipped
for push/manual events, so the same required aggregate remains valid on
`main` without weakening pull-request enforcement.

## Stacked pull requests

Add exactly one machine-readable line to the child PR body:

```text
<!-- integration-stack parent=123 base=feature/parent tested-base=<parent-head-sha> -->
```

Every PR targeting a non-default base is treated as stacked and must carry
this metadata; prose such as “depends on #123” is not integration evidence.

The parent must be open, `base` must be both the child's live base and the
parent's live head ref, and `tested-base` must equal the live parent/base SHA.
The child's merge base must also equal that SHA. These checks accept both a
linear/rebased child and a merge commit containing the complete live parent,
while rejecting a child tested on an older or divergent parent tree.

Integrate a stack in this order:

1. Review and merge the parent using the guarded command above.
2. Fetch current `main`; retarget the child to `main` and rebase its commits.
3. Resolve conflicts locally, rerun the complete deterministic suite, and
   force-update the feature branch only with `--force-with-lease`.
4. Remove the obsolete stack metadata, review the complete `main...child`
   diff, and wait for required checks on the new exact head.
5. Generate a fresh report and merge with all three expected values.

If the parent advances before merging, rebase the child on the new parent,
replace `tested-base`, and rerun review and CI. If the parent is merged or its
branch disappears, follow steps 2–5. If rebase or merge reports a conflict,
stop integration, resolve and test the new tree, then obtain a fresh review;
an older green run never applies to the resolved head.

Run the deterministic policy fixtures with:

```sh
python3 -m unittest -v tools.tests.test_pr_integration
```

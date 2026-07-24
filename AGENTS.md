# Autonomous repository execution

The live source of truth is GitHub issue #30 plus the current issue, pull
request, head commit, review, and required-check state. Historical comments
about workers, pumps, slots, claims, leases, or timers are audit history only.

When the user asks to execute the roadmap autonomously:

1. Start immediately. Refresh `main` and live GitHub state; do not ask the user
   to select or manually advance routine work.
2. Complete existing pull requests before creating duplicates. Give parallel
   writers isolated worktrees; otherwise keep subagents read-only and use one
   integration writer.
3. Implement the full current leaf-issue contract, including deterministic
   tests and documentation. Make narrow, reversible in-scope decisions without
   pausing for approval.
4. Review the complete diff, publish the coherent branch, and inspect required
   checks for the exact latest head. Fix failures on the same branch and repeat.
5. Do not treat a pushed branch, draft pull request, stale green run, or partial
   checklist as completion. Mark ready when appropriate and merge only the
   reviewed latest head after its required checks are green and its acceptance
   criteria are satisfied.
6. After every merge, refresh `main`, issues, pull requests, and dependencies,
   then immediately continue the next ready work. Close or update issues as the
   merged result requires.
7. Ask the user only for a genuine external gate: a secret that must never be
   shared, authorization for a user-only side effect, unavailable live-service
   evidence, or a product/security choice not resolved by the issue contract.
   Continue unrelated ready coding while such a gate is open.

## Asynchronous continuation fallback

The active Work Mode session owns execution. Do not recreate recurring
pseudo-agent/slot/lease schedulers.

If an already-started CI, build, review, deployment, or merge gate is expected
to outlive the active session's wait window, create one bounded scheduled
continuation instead of stopping at the open pull request. Its instructions
must require the next session to:

- re-read live issue/PR/head/check state rather than trust a captured status;
- resume the exact pending integration;
- inspect and repair failures, or merge the reviewed exact head when green;
- refresh dependencies and continue the autonomous queue after the merge; and
- stop/delete the continuation when no asynchronous gate remains or only a
  genuine user-only blocker is left.

Use the lowest practical cadence and only one continuation for the pending
integration state. A timeout is a handoff to later autonomous execution, not a
completed task and not a reason to ask the user to merge.

## Rumble operator boundary

Issues #36 and #40 are not packet-inspection assignments. The repository owns
the packaged validators and all offline fixtures. After that tooling is merged,
the operator only runs the documented command and pastes its sanitized Markdown
report. Never ask for a Rumble session, password, 2FA code, raw URL/header/body,
HAR, packet capture, or screenshot. Interpret a pasted report autonomously,
apply the smallest required documentation/code update, pass CI, merge, and
continue the queue.

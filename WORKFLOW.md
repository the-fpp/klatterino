---
pipeline:
  intake_state: Todo
  issue:
    outputs:
    - stage-1
  max_steps: 20
  stages:
    stage-1:
      effort: low
      inputs: []
      model: gpt-5.3-codex-spark
      on:
        completed: stage-2
      role: implement
      session: stage-1
      state: In Progress
      substates:
        substate-1:
          inputs:
          - output: ci_failures
            stage: stage-5
          model: gpt-5.6-luna
          on:
            completed: stage-2.substate-1
        substate-2:
          inputs:
          - output: ci_failures
            stage: stage-5.substate-1
          model: gpt-5.6-terra
          on:
            completed: stage-2.substate-2
        substate-3:
          inputs:
          - output: ci_failures
            stage: stage-5.substate-2
          model: gpt-5.6-sol
          on:
            completed: stage-2.substate-3
      type: agent
    stage-2:
      command: cargo test
      expected: success
      on:
        infrastructure: stage-3
        matched: stage-4
        mismatched: stage-1.substate-1
      provider: github_actions
      reference: null
      state: In Progress
      substates:
        substate-1:
          on:
            matched: stage-4
            mismatched: stage-1.substate-2
        substate-2:
          on:
            matched: stage-4.substate-1
            mismatched: stage-6
        substate-3:
          on:
            matched: stage-4.substate-2
      timeout_ms: 1800000
      type: ci
      workflow: ci.yml
    stage-3:
      outcome: escalate
      reason: Error
      state: Error
      type: terminal
    stage-4:
      effort: high
      model: gpt-5.6-luna
      on:
        accepted: stage-4.substate-1
        blocked: stage-3
        rejected: stage-1.substate-1
      role: check
      session: stage-4
      state: In Progress
      substates:
        substate-1:
          model: gpt-5.6-terra
          on:
            accepted: stage-4.substate-2
            blocked: stage-3
            rejected: stage-1.substate-2
        substate-2:
          model: gpt-5.6-sol
          on:
            accepted: stage-7
            blocked: stage-3
            rejected: stage-1.substate-3
      type: agent
    stage-5:
      effort: low
      inputs:
      - stage-2
      model: gpt-5.3-codex-spark
      state: In Progress
      substates:
        substate-1:
          inputs:
          - stage-2.substate-1
          model: gpt-5.3-codex-spark
        substate-2:
          inputs:
          - stage-2.substate-2
          model: gpt-5.3-codex-spark
      target: ci_failures
      type: extractor
    stage-6:
      outcome: escalate
      state: Failure
      type: terminal
    stage-7:
      outcome: auto_merge
      state: Done
      type: terminal
---

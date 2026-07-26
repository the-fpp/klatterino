pipeline: {
  "intake_state": "Todo",
  "issue": {
    "outputs": [
      "stage-1"
    ]
  },
  "max_steps": 20,
  "stages": {
    "stage-1": {
      "type": "agent",
      "state": "In Progress",
      "session": "stage-1",
      "role": "implement",
      "model": "gpt-5.3-codex-spark",
      "effort": "low",
      "substates": {
        "substate-1": {
          "on": {
            "completed": "stage-2.substate-1"
          },
          "model": "gpt-5.6-luna",
          "inputs": [
            {
              "stage": "stage-5",
              "output": "ci_failures"
            }
          ]
        },
        "substate-2": {
          "on": {
            "completed": "stage-2.substate-2"
          },
          "model": "gpt-5.6-terra",
          "inputs": [
            {
              "stage": "stage-5.substate-1",
              "output": "ci_failures"
            }
          ]
        },
        "substate-3": {
          "model": "gpt-5.6-sol",
          "on": {
            "completed": "stage-2.substate-3"
          },
          "inputs": [
            {
              "stage": "stage-5.substate-2",
              "output": "ci_failures"
            }
          ]
        }
      },
      "on": {
        "completed": "stage-2"
      },
      "inputs": []
    },
    "stage-2": {
      "type": "ci",
      "state": "CI",
      "expected": "success",
      "provider": "command",
      "command": "cargo test",
      "timeout_ms": 1800000,
      "substates": {
        "substate-1": {
          "on": {
            "matched": "stage-4",
            "mismatched": "stage-1.substate-2"
          }
        },
        "substate-2": {
          "on": {
            "matched": "stage-4.substate-1",
            "mismatched": "stage-6"
          }
        },
        "substate-3": {
          "on": {
            "matched": "stage-4.substate-2"
          }
        }
      },
      "on": {
        "infrastructure": "stage-3",
        "matched": "stage-4",
        "mismatched": "stage-1.substate-1"
      }
    },
    "stage-3": {
      "type": "terminal",
      "state": "Error",
      "outcome": "escalate",
      "reason": "Error"
    },
    "stage-4": {
      "type": "agent",
      "state": "In Progress",
      "session": "stage-4",
      "role": "check",
      "model": "gpt-5.6-luna",
      "effort": "high",
      "substates": {
        "substate-1": {
          "model": "gpt-5.6-terra",
          "on": {
            "accepted": "stage-4.substate-2",
            "blocked": "stage-3",
            "rejected": "stage-1.substate-2"
          }
        },
        "substate-2": {
          "model": "gpt-5.6-sol",
          "on": {
            "accepted": "stage-7",
            "blocked": "stage-3",
            "rejected": "stage-1.substate-3"
          }
        }
      },
      "on": {
        "accepted": "stage-4.substate-1",
        "blocked": "stage-3",
        "rejected": "stage-1.substate-1"
      }
    },
    "stage-5": {
      "type": "extractor",
      "state": "Todo",
      "model": "gpt-5.3-codex-spark",
      "effort": "low",
      "target": "ci_failures",
      "substates": {
        "substate-1": {
          "inputs": [
            "stage-2.substate-1"
          ],
          "model": "gpt-5.3-codex-spark"
        },
        "substate-2": {
          "inputs": [
            "stage-2.substate-2"
          ],
          "model": "gpt-5.3-codex-spark"
        }
      },
      "inputs": [
        "stage-2"
      ]
    },
    "stage-6": {
      "type": "terminal",
      "state": "Failure",
      "outcome": "escalate"
    },
    "stage-7": {
      "type": "terminal",
      "state": "Done",
      "outcome": "auto_merge"
    }
  }
}

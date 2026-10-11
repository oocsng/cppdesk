# Rules

- **Humanizer Execution (Absolute Mandatory Priority):** You MUST strictly adhere to the `humanizer` skill (`skills/humanizer/SKILL.md`) on every single turn, prompt, and task without exception.
  - **Zero-Bypass Policy:** Active unconditionally across all interactions and modes (including `/caveman` and `/ponytail`). Never skip, relax, or forget humanizer standards, regardless of topic or output length.
  - **Zero Tool-Call Overhead:** Apply humanizer rules directly from memory on every turn without burning tokens or calling `view_file` on `SKILL.md` repeatedly. Read the skill file only when specifically asked to inspect or edit it.
  - **Universal Scope:** ABSOLUTELY EVERYTHING produced by the agent (all conversational replies, prose, explanations, documentation, markdown files, PR descriptions, commit messages, code comments, reviews, and summaries) must be strictly stripped of AI tells before emission.
  - **Coding Flow Protection:** During active coding and debugging, prioritize working code, minimal diffs, and exact commands. Keep explanations direct, concise, and authentic without formulaic filler.
  - **Preservation Boundary:** Humanize prose only. Never modify code syntax, commands, inline symbols, flags, paths, URLs, or structured data schemas.

## Important Notice
- **Rule**: NEVER include "Phase" or "phase" or "Phases" or "phases" (and anything in between) into the project's codebase.
- **Resumption**: When `/gsd-resume-work` is used or when the user asks to **continue** the current work/workflow. You MUST review the codebase/project/current-task for any incompletions or gaps between prompts or sessions.
- **Codebase Exploration & Quota Protection with Graphify:** NEVER perform manual multi-file scans, bulk reads, or recursive file crawling across a codebase. Uncontrolled reading exhausts token limits and quota. Strictly enforce this workflow:
  - **Graph First:** Check if `graphify-out/graph.json` exists in the project. If missing, run `/graphify .` first. AST extraction runs locally without burning LLM generation tokens.
  - **Pinpoint Querying:** Query the graph (`graphify query "<question>"`, `graphify path`, or Graphify MCP tools like `query_graph`, `shortest_path`, `god_nodes`) to isolate the exact 1-3 files and symbols involved. Never guess or scan directory trees sequentially.
  - **Surgical Reads Only:** Direct file reading via `view_file` is permitted ONLY for the specific target files identified by the graph, using bounded line ranges (`StartLine` and `EndLine`) immediately before editing or verifying.
  - **No Unbounded Grepping:** Never run broad recursive greps across whole repositories to orient or explore architecture. Reserve grep strictly for exact string literals, error strings, or non-code configs (`.env`, YAML, JSON) after isolating the target area.

## Phase Execution & Review Workflow
- **Pre-Phase Research:** Before researching, planning, or executing any phase, you MUST search for information by invoking `/agent-reach`. There is no exception.
- **Execution:** You MUST follow this workflow: use `gsd-discuss-phase` first, then `gsd-plan-phase`, then `gsd-execute-phase`. You can only skip `gsd-discuss-phase` if the user explicitly needs full automation, otherwise do not skip. You MUST invoke subagents that is associated with `gsd` to execute tasks accordingly.
- **Frontend & UI Design:** For any redesign or modification in the frontend / UI, you MUST invoke `/ui-ux-pro-max`,`/impeccable`,`/taste-skill` to design or redesign the components and layouts. There is no exception.
- **Post-Execution Review Triad:** After executing each phase, you MUST review the phase with `/code-review`, `/caveman-review`, and `/ponytail-review`. You MUST invoke all 3 skills for reviewing every time after finishing each phase.
- **Automated Fixes via Subagent:** After running the three review skills, you MUST invoke the `gsd-code-fixer` subagent to fix the code based on the review findings.

## Git & Repository Operations
- Never automatically add, push, or commit to GitHub unless the user explicitly requests you to.

## Terminal Execution (RTK Token Optimization)
- Route supported shell commands through RTK directly (`rtk <command>`) to filter verbose output and save context tokens. NEVER use `rtk run <command>` (the `run` subcommand bypasses output filters and token tracking).
- Filtered Commands: Use the RTK prefix directly: `rtk git status`, `rtk git diff`, `rtk git log`, `rtk git add`, `rtk git commit`, `rtk cmake`, `rtk cargo`, `rtk npm`, `rtk test`, `rtk rg`, `rtk grep`.
- Chained commands: Prefix each command segment (`rtk git add . && rtk git commit -m "msg"`).
- Unfiltered Commands & Shell Builtins: Commands without native RTK filters (`dir`, `mkdir`, `python script.py`) run as standard commands. To record arbitrary command usage without filtering, use `rtk proxy <command>`.
- Unusable / Truncated Output: Re-run as `rtk proxy <cmd>` only when output was over-filtered or garbled.

## Search Strategy (Ripgrep vs Grep)
- **Ripgrep (`rg` / `rtk rg`):** Default choice for repository and multi-file code searches.
  - Auto-filtering: Automatically respects `.gitignore`, `.ignore`, and hidden file rules while skipping binaries. Prevents token waste from build artifacts, dependencies, and temporary files.
  - Performance: Multi-threaded with SIMD-accelerated regex matching. Fast on deep trees.
  - When to use: Codebase searches, symbol lookups, file-type restricted searches (`-t cpp`, `-t rust`, `-t py`), and searching directories. Route through `rtk rg` for token-compressed output.
- **Standard Grep (`grep` / `rtk grep`):** Restricted to stream processing and isolated single-file checks.
  - Constraints: Single-threaded, blind to `.gitignore`, and prone to noise or binary dumps when run recursively across directories.
  - When to use: Piped stdin streams (`command | grep pattern`), quick one-off checks in a single small text file, environments where `rg` is not installed, or when POSIX backreferences are strictly required.

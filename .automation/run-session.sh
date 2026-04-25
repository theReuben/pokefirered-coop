#!/bin/bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
AUTOPILOT_DIR="/Users/reuben/IdeaProjects/claude-autopilot"
export AUTOPILOT_PROJECT_DIR="$PROJECT_DIR"
LOG_DIR="$PROJECT_DIR/.automation/logs"
MAX_TURNS=75
PERMISSION_MODE="auto"
INTER_SESSION_PAUSE=10
RATE_LIMIT_PAUSE=60
MAX_RATE_LIMIT_RETRIES=5
WINDOW_DURATION_SECONDS=19200
STALL_THRESHOLD=5
DEFAULT_MODEL="sonnet"
OPUS_STEPS="0.4 1.1 1.3 1.4 2.1 2.2 2.5 2.6 3.1 3.2 3.4 4.1 4.3 5.1 5.2 5.3 7.1 7.3 7.4 7.5"

mkdir -p "$LOG_DIR"
RUN_ID=$(date '+%Y%m%d_%H%M%S')
LOG_FILE="$LOG_DIR/window_${RUN_ID}.log"
WINDOW_START=$(date +%s)
SESSION_COUNT=0
CONSECUTIVE_SAME_STEP=0
LAST_STEP=""

log() { echo "[$(date '+%H:%M:%S')] $1" | tee -a "$LOG_FILE"; }

get_current_step() { grep "^\- \*\*Active Step:\*\*" "$PROJECT_DIR/PROGRESS.md" 2>/dev/null | sed 's/.*Active Step:\*\* //' | tr -d ' ' || echo "1.1"; }
get_current_phase() { grep "^\- \*\*Active Phase:\*\*" "$PROJECT_DIR/PROGRESS.md" 2>/dev/null | sed 's/.*Active Phase:\*\* //' | tr -d ' ' || echo "1"; }
get_next_action() { grep "^\- \*\*Next Action:\*\*" "$PROJECT_DIR/PROGRESS.md" 2>/dev/null | sed 's/.*Next Action:\*\* //' || echo "Begin"; }
is_project_complete() { grep -q "Active Step.*done\|Active Step.*complete" "$PROJECT_DIR/PROGRESS.md" 2>/dev/null && echo "true" || echo "false"; }
seconds_remaining() { echo $(( WINDOW_DURATION_SECONDS - ($(date +%s) - WINDOW_START) )); }
select_model() { local s="$1"; for o in $OPUS_STEPS; do [[ "$s" == "$o" ]] && { echo "opus"; return; }; done; echo "$DEFAULT_MODEL"; }

check_stall() {
    local step="$1"
    if [[ "$step" == "$LAST_STEP" ]]; then CONSECUTIVE_SAME_STEP=$((CONSECUTIVE_SAME_STEP + 1))
    else CONSECUTIVE_SAME_STEP=0; fi
    LAST_STEP="$step"
    [[ $CONSECUTIVE_SAME_STEP -lt $STALL_THRESHOLD ]]
}

build_prompt() {
    local step="$1" next_action="$2"
    local remaining_min=$(( $(seconds_remaining) / 60 ))
    cat <<PROMPT
You are continuing autonomous work on this project.

QUALITY NOTICE: Your code WILL be reviewed by Reuben after each phase. Do not cut corners.

INSTRUCTIONS:
1. Read PROGRESS.md FIRST — find where you left off.
2. Read FEEDBACK.md — handle every [UNREAD] item first. Change to [READ].
3. Read CLAUDE.md for conventions.
4. Read relevant docs for your current task.
5. Read docs/cookbook.md before debugging.
6. Check docs/decisions.md before architectural decisions.
7. Work autonomously — do NOT ask questions.
8. MAXIMIZE OUTPUT — complete as many substeps as possible.

CURRENT STATE:
- Active Step: ${step}
- Next Action: ${next_action}
- Window remaining: ~${remaining_min} minutes
- Session number: $((SESSION_COUNT + 1))

EXECUTION:
- Jump into coding after reading docs. Don't over-plan.
- After EACH substep is done: immediately mark it [x] in PROGRESS.md and commit.
- If you finish a step, update Active Step in PROGRESS.md and start the next immediately.
- Only mark [x] if verified (parses, correct structure, tested if possible).
- Record architectural decisions in docs/decisions.md.
- Record fixes in docs/cookbook.md.

IMPORTANT — update PROGRESS.md throughout the session, not just at the end.
You may be stopped at any turn. Commit after each substep so progress is never lost.
- git add -A && git commit -m "descriptive message"

Begin now. Go fast.
PROMPT
}

run_single_session() {
    local step=$(get_current_step) phase=$(get_current_phase)
    local next_action=$(get_next_action) model=$(select_model "$step")
    SESSION_COUNT=$((SESSION_COUNT + 1))
    log "── SESSION $SESSION_COUNT | Phase $phase Step $step | $model | ~$(( $(seconds_remaining) / 60 ))m ──"
    [[ "$(is_project_complete)" == "true" ]] && { log "✓ COMPLETE"; return 3; }
    check_stall "$step" || { log "⚠ STALLED"; return 4; }

    python3 "$AUTOPILOT_DIR/slack.py" sync-feedback 2>&1 | tee -a "$LOG_FILE" || true
    python3 "$AUTOPILOT_DIR/slack.py" session-start "$SESSION_COUNT" "$phase" "$step" "$model" "$(( $(seconds_remaining) / 60 ))" 2>/dev/null || true

    python3 "$AUTOPILOT_DIR/healthcheck.py" pre 2>&1 | tee -a "$LOG_FILE"
    if [[ $? -eq 2 ]]; then
        python3 "$AUTOPILOT_DIR/healthcheck.py" fix 2>&1 | tee -a "$LOG_FILE"
        python3 "$AUTOPILOT_DIR/healthcheck.py" pre 2>&1 | tee -a "$LOG_FILE"
        [[ $? -eq 2 ]] && { log "Pre-flight failed."; return 1; }
    fi

    mkdir -p "$PROJECT_DIR/.automation/backups"
    for f in PROGRESS.md CLAUDE.md docs/cookbook.md; do
        [[ -f "$PROJECT_DIR/$f" ]] && cp "$PROJECT_DIR/$f" "$PROJECT_DIR/.automation/backups/$(basename $f).bak"
    done

    local prompt=$(build_prompt "$step" "$next_action")
    local rate_pause=$RATE_LIMIT_PAUSE
    for attempt in $(seq 1 $MAX_RATE_LIMIT_RETRIES); do
        local exit_code=0
        cd "$PROJECT_DIR"
        claude -p "$prompt" --model "$model" --permission-mode "$PERMISSION_MODE" --max-turns "$MAX_TURNS" --output-format text 2>&1 | tee -a "$LOG_FILE" || exit_code=$?
        local max_turns_hit=false
        tail -5 "$LOG_FILE" | grep -qi "reached max turns\|max.turns" && max_turns_hit=true
        if [[ $exit_code -eq 0 ]] || [[ "$max_turns_hit" == "true" ]]; then
            [[ "$max_turns_hit" == "true" ]] && log "Max turns reached — treating as normal session end"
            local new_step=$(get_current_step)
            [[ "$new_step" != "$step" ]] && log "✓ Advanced: $step → $new_step" || log "→ Still on $step"
            cd "$PROJECT_DIR"
            [[ -n $(git status --porcelain 2>/dev/null) ]] && git add -A && git commit -m "Auto #$SESSION_COUNT: Phase $phase Step $step [$(date '+%H:%M')]" 2>/dev/null || true
            python3 "$AUTOPILOT_DIR/healthcheck.py" post 2>&1 | tee -a "$LOG_FILE" || true
            python3 "$AUTOPILOT_DIR/slack.py" session-end "$SESSION_COUNT" "$phase" "$step" "$new_step" 2>/dev/null || true
            return 0
        fi
        if tail -30 "$LOG_FILE" | grep -qi "rate.limit\|too many requests\|429\|usage.limit"; then
            [[ $attempt -lt $MAX_RATE_LIMIT_RETRIES ]] && { log "Rate limited ($attempt). Waiting ${rate_pause}s..."; sleep "$rate_pause"; rate_pause=$((rate_pause * 2)); } || { log "Window exhausted."; return 2; }
        else
            log "Error (exit $exit_code)"; return 1
        fi
    done
    return 2
}

drain_window() {
    log "╔══ AUTOPILOT — DRAINING WINDOW ══╗"
    local consecutive_errors=0
    while true; do
        [[ $(seconds_remaining) -le 0 ]] && { log "Window expired."; break; }
        local result=0
        run_single_session || result=$?
        case $result in
            0) consecutive_errors=0; sleep "$INTER_SESSION_PAUSE" ;;
            1) consecutive_errors=$((consecutive_errors + 1))
               [[ $consecutive_errors -ge 3 ]] && { log "3 errors."; python3 "$AUTOPILOT_DIR/slack.py" alert error "3 consecutive errors" 2>/dev/null || true; break; }
               sleep 60 ;;
            2) break ;;
            3) python3 "$AUTOPILOT_DIR/slack.py" alert project_complete 2>/dev/null || true; break ;;
            4) python3 "$AUTOPILOT_DIR/slack.py" alert stall "$(get_current_step)" 2>/dev/null || true; break ;;
        esac
        local current=$(get_current_step)
        if [[ "$current" != "$LAST_STEP" && -n "$LAST_STEP" ]]; then
            cd "$PROJECT_DIR"
            git tag -f "checkpoint/step-${LAST_STEP}" 2>/dev/null || true
            case "$LAST_STEP" in
        0.4)
            log "Phase 1 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 1 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 1 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 1 2>/dev/null || true
                git tag -f "phase-1-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 1 2>/dev/null || true
            fi ;;
        1.6)
            log "Phase 2 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 2 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 2 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 2 2>/dev/null || true
                git tag -f "phase-2-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 2 2>/dev/null || true
            fi ;;
        2.7)
            log "Phase 3 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 3 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 3 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 3 2>/dev/null || true
                git tag -f "phase-3-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 3 2>/dev/null || true
            fi ;;
        3.5)
            log "Phase 4 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 4 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 4 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 4 2>/dev/null || true
                git tag -f "phase-4-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 4 2>/dev/null || true
            fi ;;
        4.5)
            log "Phase 5 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 5 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 5 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 5 2>/dev/null || true
                git tag -f "phase-5-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 5 2>/dev/null || true
            fi ;;
        5.6)
            log "Phase 6 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 6 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 6 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 6 2>/dev/null || true
                git tag -f "phase-6-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 6 2>/dev/null || true
            fi ;;
        6.3)
            log "Phase 7 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 7 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 7 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 7 2>/dev/null || true
                git tag -f "phase-7-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 7 2>/dev/null || true
            fi ;;
        7.7)
            log "Phase 8 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 8 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 8 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 8 2>/dev/null || true
                git tag -f "phase-8-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 8 2>/dev/null || true
            fi ;;
        8.4)
            log "Phase 9 final step complete — running gate..."
            if python3 "$AUTOPILOT_DIR/autopilot.py" gate 9 2>&1 | tee -a "$LOG_FILE"; then
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_passed 9 2>/dev/null || true
                python3 "$AUTOPILOT_DIR/slack.py" alert phase_complete 9 2>/dev/null || true
                git tag -f "phase-9-complete" 2>/dev/null || true
            else
                python3 "$AUTOPILOT_DIR/slack.py" alert gate_failed 9 2>/dev/null || true
            fi ;;
            esac
        fi
    done
    log "Sessions: $SESSION_COUNT | Final: Phase $(get_current_phase) Step $(get_current_step) | Duration: $(( ($(date +%s) - WINDOW_START) / 60 ))m"
    python3 "$AUTOPILOT_DIR/slack.py" window-summary "$SESSION_COUNT" "$(get_current_step)" "$(( ($(date +%s) - WINDOW_START) / 60 ))" "$(get_current_phase)" 2>/dev/null || true
}

case "${1:-}" in
    --once) run_single_session ;;
    --dry-run) echo "Phase $(get_current_phase) | Step $(get_current_step) | Model: $(select_model $(get_current_step))"; echo "Next: $(get_next_action)"; echo ""; build_prompt "$(get_current_step)" "$(get_next_action)" ;;
    --status) echo "Phase $(get_current_phase) | Step $(get_current_step) | Next: $(get_next_action)" ;;
    *) drain_window ;;
esac

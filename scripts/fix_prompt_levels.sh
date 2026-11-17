#!/usr/bin/env bash
# Fixes the WAV prompts that check_prompt_levels.sh flags as too quiet.
#
# Usage:
#   ./check_prompt_levels.sh [sounds folder] [AsrOptions.xml] | ./fix_prompt_levels.sh [sounds folder] [--apply] [--resample]
#
#   sounds folder  same folder given to check_prompt_levels.sh (default C:/inetpub/SIPServer/sounds/custom)
#   --apply        make the changes; without it the script only prints what it would do
#   --resample     also convert RESAMPLED files to 48000 Hz (optional; FreeSWITCH plays them fine as they are)
#
# What it does
#   (files with a peak at or below -60 dBFS are treated as intentional silence and skipped)
#   LOW-PEAK / LOW-AVG   raises the gain so the peak sits at -6 dBFS (the level that fixed the time reply), at most +18 dB
#   CUT-RISK             not fixable in the file: prints how long the router needs to wait, for AsrOptions.xml
#   RESAMPLED            converted to 48000 Hz only with --resample
#
# Every file it changes is first copied to a backup folder inside the sounds folder. Files that FreeSWITCH is
# playing at that moment cannot be replaced on Windows; those are reported and left as they were.

SOUNDS="C:/inetpub/SIPServer/sounds/custom"
APPLY=0
RESAMPLE=0
for arg in "$@"; do
    case "$arg" in
        --apply) APPLY=1 ;;
        --resample) RESAMPLE=1 ;;
        *) SOUNDS="$arg" ;;
    esac
done

TARGET_PEAK=-6
MAX_GAIN=18
BACKUP="$SOUNDS/backup_$(date +%Y%m%d_%H%M%S)"
changed=0
failed=0
cut_notes=""

[ $APPLY -eq 0 ] && echo "Dry run: nothing will be changed. Add --apply to make the changes."

while read -r name rate len peak avg speech limit flags; do
    [ "$name" = "file" ] && continue          # header
    [ -z "$name" ] && continue
    wav="$SOUNDS/$name"
    [ -f "$wav" ] || { echo "skip  $name (not found in $SOUNDS)"; continue; }

    filters=()
    actions=()

    # files that are silent on purpose (e.g. asr_silence_48k.wav) are never touched
    if awk "BEGIN{exit !($peak <= -60)}"; then
        echo "skip  $name: silent on purpose (peak ${peak} dBFS)"
        continue
    fi

    if [[ "$flags" == *LOW-PEAK* || "$flags" == *LOW-AVG* ]]; then
        gain=$(awk "BEGIN{g=$TARGET_PEAK-($peak); if(g>$MAX_GAIN)g=$MAX_GAIN; printf \"%.1f\", g}")
        if awk "BEGIN{exit !($gain > 0.5)}"; then
            filters+=("volume=${gain}dB")
            actions+=("gain +${gain} dB (peak ${peak} -> $(awk "BEGIN{printf \"%.1f\", $peak+$gain}") dBFS)")
        else
            actions+=("peak already ${peak} dBFS, average ${avg} dBFS is low: needs compression, not changed")
        fi
    fi

    if [[ "$flags" == *RESAMPLED* && $RESAMPLE -eq 1 ]]; then
        filters+=("aresample=48000")
        actions+=("resample ${rate} -> 48000 Hz")
    fi

    if [[ "$flags" == *CUT-RISK* ]]; then
        need=$(awk "BEGIN{printf \"%d\", $speech*1000+200+0.5}")
        cut_notes+="  $name: speech ends at ${speech} s, the router waits ${limit} s; needs at least ${need} ms (PromptDurations value + AudioTailMs)\n"
    fi

    [ ${#filters[@]} -eq 0 ] && { [ ${#actions[@]} -gt 0 ] && echo "note  $name: ${actions[*]}"; continue; }

    joined=$(IFS=,; echo "${filters[*]}")
    if [ $APPLY -eq 0 ]; then
        echo "would $name: $(IFS=';'; echo "${actions[*]}")"
        continue
    fi

    mkdir -p "$BACKUP"
    cp -p "$wav" "$BACKUP/$name" || { echo "FAIL  $name: could not back up, left unchanged"; failed=$((failed+1)); continue; }

    tmp="$SOUNDS/.fix_tmp_$name"
    if ffmpeg -nostdin -v error -y -i "$wav" -af "$joined" -c:a pcm_s16le -map_metadata -1 -bitexact "$tmp" </dev/null &&
       mv -f "$tmp" "$wav"; then
        echo "done  $name: $(IFS=';'; echo "${actions[*]}")"
        changed=$((changed+1))
    else
        rm -f "$tmp"
        echo "FAIL  $name: could not convert or replace (in use?), left unchanged"
        failed=$((failed+1))
    fi
done

if [ -n "$cut_notes" ]; then
    echo
    echo "Timing (not changed, edit AsrOptions.xml):"
    printf "%b" "$cut_notes"
fi

if [ $APPLY -eq 1 ]; then
    echo
    echo "$changed file(s) changed, $failed failed."
    [ $changed -gt 0 ] && echo "Originals saved in $BACKUP"
fi

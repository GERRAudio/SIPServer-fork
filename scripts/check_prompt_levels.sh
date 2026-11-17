#!/usr/bin/env bash
# Reports level, length and speech end for every WAV prompt the ASR replies use, and flags the ones at risk.
#
# Usage: ./check_prompt_levels.sh [sounds folder] [AsrOptions.xml]
#   defaults: C:/inetpub/SIPServer/sounds/custom   C:/inetpub/wwwroot/Configuration/AsrOptions.xml
#
# Flags
#   LOW-PEAK    peak below -12 dBFS (the time reply was cut at -18 dBFS and is fine at -6 dBFS)
#   LOW-AVG     average below -30 dBFS
#   CUT-RISK    speech ends within 200 ms of (or after) the point the router hangs up for that prompt
#               (playback starts about 80 ms after the router sends it, so less than that is not safe)
#   RESAMPLED   not 48000 Hz (FreeSWITCH converts it; works, noted only)

SOUNDS="${1:-C:/inetpub/SIPServer/sounds/custom}"
OPTIONS="${2:-C:/inetpub/wwwroot/Configuration/AsrOptions.xml}"
PEAK_MIN=-12
AVG_MIN=-30
SILENCE_DB=-45

# value of <Tag>n</Tag> from AsrOptions.xml (first match), or the default given
opt() { local v; v=$(grep -o "<$1>[^<]*</$1>" "$OPTIONS" 2>/dev/null | head -1 | sed -E "s#<[^>]+>##g" | tr -d '\r '); echo "${v:-$2}"; }

TAIL=$(opt AudioTailMs 120)
declare -A WINDOW   # file name -> ms the router waits before hanging up
for p in PleaseWait NotUnderstood InvalidConference NoConference OperatorCalled JoinedConference; do
    f=$(opt "$p" ""); ms=$(sed -n "/<PromptDurations>/,/<\/PromptDurations>/p" "$OPTIONS" 2>/dev/null | grep -o "<$p>[0-9]*</$p>" | grep -o "[0-9]*")
    # the PromptWavFiles entry, not the PromptDurations one
    f=$(sed -n "/<PromptWavFiles>/,/<\/PromptWavFiles>/p" "$OPTIONS" 2>/dev/null | grep -o "<$p>[^<]*</$p>" | sed -E "s#<[^>]+>##g" | tr -d '\r ')
    [ -n "$f" ] && [ -n "$ms" ] && WINDOW["$f"]=$((ms + TAIL))
done
for p in VolumeUp VolumeDn MicUp MicDn; do
    f=$(sed -n "/<PromptWavFiles>/,/<\/PromptWavFiles>/p" "$OPTIONS" 2>/dev/null | grep -o "<$p>[^<]*</$p>" | sed -E "s#<[^>]+>##g" | tr -d '\r ')
    [ -n "$f" ] && WINDOW["$f"]=$((2000 + TAIL))        # volume replies use a fixed 2000 ms in the router
done
JOINED=$(sed -n "/<PromptDurations>/,/<\/PromptDurations>/p" "$OPTIONS" 2>/dev/null | grep -o "<JoinedConference>[0-9]*" | grep -o "[0-9]*")
JOINED=${JOINED:-2000}

printf "%-42s %6s %7s %8s %8s %9s %8s  %s\n" "file" "Hz" "len s" "peak dB" "avg dB" "speech s" "limit s" "flags"
shopt -s nullglob
for wav in "$SOUNDS"/*.wav; do
    name=$(basename "$wav")
    case "$name" in asr_time_*|debug_*) continue;; esac   # temporary time files

    printf "%s ...\r" "$name" >&2   # shows which file is being read if one stalls

    # -nostdin and </dev/null: Windows ffmpeg can otherwise sit waiting for keyboard input under mintty
    rate=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$wav" </dev/null)
    len=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$wav" </dev/null)
    vol=$(ffmpeg -nostdin -hide_banner -nostats -i "$wav" -af volumedetect -f null - 2>&1 </dev/null)
    peak=$(echo "$vol" | grep -o "max_volume: [-0-9.]*" | grep -o "[-0-9.]*$")
    avg=$(echo "$vol" | grep -o "mean_volume: [-0-9.]*" | grep -o "[-0-9.]*$")

    # speech end = start of the silence that runs to the end of the file (or the file length if none)
    sil=$(ffmpeg -nostdin -hide_banner -nostats -i "$wav" -af "silencedetect=n=${SILENCE_DB}dB:d=0.15" -f null - 2>&1 </dev/null | grep -o "silence_\(start\|end\): [-0-9.]*")
    last_start=$(echo "$sil" | grep silence_start | tail -1 | grep -o "[-0-9.]*$")
    last_end=$(echo "$sil" | grep silence_end | tail -1 | grep -o "[-0-9.]*$")
    speech=$len
    # (newer ffmpeg reports a silence_end at the end of the file, older ones leave it out)
    if [ -n "$last_start" ] && { [ -z "$last_end" ] || awk "BEGIN{exit !($last_start > $last_end || $len - $last_end < 0.05)}"; }; then speech=$last_start; fi

    win=${WINDOW[$name]:-}
    case "$name" in asr_conf_*|asr_number_*) win=$((JOINED + TAIL));; esac

    flags=""
    awk "BEGIN{exit !($peak < $PEAK_MIN)}" && flags+="LOW-PEAK "
    awk "BEGIN{exit !($avg < $AVG_MIN)}" && flags+="LOW-AVG "
    if [ -n "$win" ]; then
        awk "BEGIN{exit !($speech*1000 > $win - 200)}" && flags+="CUT-RISK "
        limit=$(awk "BEGIN{printf \"%.2f\", $win/1000}")
    else
        limit="-"
    fi
    [ "$rate" != "48000" ] && flags+="RESAMPLED "

    printf "%-42s %6s %7.2f %8s %8s %9.2f %8s  %s\n" "$name" "$rate" "$len" "$peak" "$avg" "$speech" "$limit" "${flags:-ok}"
done

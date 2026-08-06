#!/bin/zsh
#
# Compute-provisioning sweep for the split phase.
#
# Low Power Mode, the QoS clamp and thread count are not three separate levers
# -- they are one knob: how much compute you provision for a workload that
# spends its time waiting on the SSD. This sweeps that knob and reports which
# setting costs the fewest joules.
#
# All four corners are measured in this run, including the baseline. Nothing is
# carried over from a previous session, because too much that matters is not
# reproducible from a description -- screen brightness above all. Set the
# display wherever you like before starting; just do not touch it afterwards.
# Brightness is recorded at both ends and the script says so if it moved.
#
# A configuration wins if  time < baseline_energy / power.  That threshold is
# derived from the baseline measured here and printed per run, so there is
# nothing to work out by hand afterwards.
#
# Unattended, roughly 35-45 minutes. Holds a caffeinate assertion so the
# display and disk cannot sleep mid-protocol and change the power draw between
# runs. Needs sudo for `purge` and `pmset`; the credential is kept warm.
#
#   ./sweep_provisioning.sh
#
# Do not close the lid (caffeinate cannot override that) and stay on battery.
# Leaves Low Power Mode ON at the end, which is how it was found.

setopt NULL_GLOB
set -u

DATA=$HOME/mergesort/ascii
TOOL=$HOME/energy-efficiency-sort/power_log.py
OUT=$HOME/energy-efficiency-sort
STAMP=$(date +%Y%m%d-%H%M)
LOG=$OUT/sweep-$STAMP.log

IDLE=2.84          # measured idle draw, unplugged with apps closed
RECS=5e8           # 47 GB
REPS=3
DRAIN=120          # writeback settle before each purge

die() { print -u2 "ABORT: $*"; exit 1 }

[[ -f $DATA/input.txt ]] || die "$DATA/input.txt is missing -- run gensort first"
[[ -x $TOOL ]]           || die "$TOOL is not executable"
command -v taskpolicy >/dev/null || die "taskpolicy not found"

sudo -v || die "sudo is needed for purge and pmset"

# Keep the sudo credential warm, and keep the machine awake. Both die with us.
while true; do sudo -n true 2>/dev/null; sleep 50; kill -0 $$ 2>/dev/null || exit; done &
KEEPALIVE=$!
caffeinate -dim -w $$ &
CAFFEINE=$!
trap 'kill $KEEPALIVE $CAFFEINE 2>/dev/null' EXIT

typeset -a RESULTS
START=$SECONDS

brightness() {
    ioreg -c AppleARMBacklight -r 2>/dev/null |
        grep -o '"brightness"={[^}]*"value"=[0-9]*' |
        grep -o '[0-9]*$' | head -1
}

lpm() {
    sudo pmset -a lowpowermode $1
    print "\n[lowpowermode = $1]"
}

run_config() {
    local tag=$1; shift
    print "\n========== $tag =========="
    local i out e t p
    for i in $(seq 1 $REPS); do
        rm -f $DATA/run*.dat
        sync
        sleep $DRAIN
        sudo purge
        print "\n--- $tag rep $i ---"
        out=$( cd $DATA && $TOOL --records $RECS --idle-watts $IDLE \
                   --csv $OUT/split-$tag-$i.csv -- "$@" 2>&1 )
        print "$out"
        # --csv makes the tool print a second "  samples <path>" line, so the
        # time pattern has to require the " over " only the stats line has.
        e=$(print "$out" | awk '/^  energy /{print $2; exit}')
        t=$(print "$out" | awk '/^  samples .* over /{print $4; exit}' | tr -d 's')
        p=$(print "$out" | awk '/^  sys_power /{print $2; exit}')
        if [[ -n $e && -n $t && -n $p ]]; then
            RESULTS+=("$tag|$i|$e|$t|$p")
        else
            print "  (rep $i produced no parseable result)"
        fi
    done
}

BRIGHT_START=$(brightness)

{
    print "provisioning sweep  $STAMP"
    print "reps=$REPS  records=$RECS  idle=${IDLE}W  brightness=$BRIGHT_START/65536"
    df -h /System/Volumes/Data | tail -1
    smartctl -a /dev/disk0 2>/dev/null | grep -E "Temperature:|Percentage Used:"

    lpm 0
    run_config nolpm-plain  ../split_program 5000000 8
    run_config nolpm-util   taskpolicy -c utility ../split_program 5000000 8

    lpm 1
    run_config lpm-plain    ../split_program 5000000 8
    run_config lpm-util     taskpolicy -c utility ../split_program 5000000 8

    # The baseline is whatever lpm-plain measured today, at today's brightness.
    # No `local` past this point: outside a function zsh treats it as a query
    # and prints any name that is already set, which lands stray "e=443" lines
    # in the middle of the summary.
    typeset -a base_e
    for r in $RESULTS; do
        [[ ${r%%|*} == lpm-plain ]] || continue
        rest=${r#*|}; rest=${rest#*|}
        base_e+=(${rest%%|*})
    done
    BASELINE=0
    for e in $base_e; do BASELINE=$(( BASELINE + e )); done
    if (( ${#base_e} > 0 )); then
        BASELINE=$(( BASELINE / ${#base_e} ))
    else
        print "\nNo lpm-plain result -- cannot derive a baseline. Raw rows only."
        BASELINE=0
    fi

    print "\n\n========== summary =========="
    printf "baseline (LPM on, P-cores), measured here: %.0f J\n" $BASELINE
    print "a config wins if its time is under baseline/power\n"
    printf "  %-16s %8s %8s %8s %10s  %s\n" \
           config energy time power "max time" verdict
    for r in $RESULTS; do
        tag=${r%%|*};      r=${r#*|}
        i=${r%%|*};        r=${r#*|}
        e=${r%%|*};        r=${r#*|}
        t=${r%%|*};        p=${r##*|}
        if (( BASELINE > 0 )); then
            maxt=$(( BASELINE / p ))
            if (( t < maxt )); then verdict="WIN"; else verdict="lose"; fi
        else
            maxt=0; verdict="-"
        fi
        printf "  %-16s %7sJ %7ss %7sW %9.1fs  %s\n" \
               "$tag-$i" "$e" "$t" "$p" "$maxt" "$verdict"
    done

    BRIGHT_END=$(brightness)
    print ""
    if [[ $BRIGHT_START != $BRIGHT_END ]]; then
        print "WARNING: brightness moved $BRIGHT_START -> $BRIGHT_END during the run."
        print "         The backlight is watts. Treat these numbers as suspect."
    else
        print "brightness held at $BRIGHT_END/65536 throughout"
    fi
    smartctl -a /dev/disk0 2>/dev/null | grep -E "Temperature:|Percentage Used:"
    print "elapsed $(( (SECONDS - START) / 60 )) min"
    print "log: $LOG"
} 2>&1 | tee $LOG

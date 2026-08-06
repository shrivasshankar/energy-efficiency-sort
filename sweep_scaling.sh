#!/bin/zsh
#
# Scaling slope: how much records/joule is lost per doubling of data.
#
# Every projection past the largest size actually run depends on this number,
# and the only prior estimate (15%) came from a pair of rows since withdrawn.
#
# 2.5e8 and 5e8 records is exactly one doubling, and both fit on this machine
# inside the fullness range already shown not to matter. It measures the FULL
# sort, not just the split: run count goes 50 -> 100, so merge fan-in doubles
# with the data, and that is the likeliest place a penalty actually lives.
#
# The two sizes are INTERLEAVED rather than run in blocks, so thermal drift,
# disk state and anything else that moves during the session hits both equally.
#
# Known limitation, logged per run rather than corrected: a 47 GB sort needs
# ~94 GiB of working set against ~47 GiB for 25 GB, so the larger size
# inherently runs a few points fuller. That is unavoidable in any external-sort
# scaling study. It biases the slope pessimistic (larger size penalised), and
# the sweep on 2026-08-06 showed 50% vs 61% fullness moving total energy by
# only 0.5%, so it should sit under the noise floor.
#
# Unattended, roughly 30 minutes. Needs sudo for `purge`.
#
#   ./sweep_scaling.sh

setopt NULL_GLOB
set -u

DATA=$HOME/mergesort/ascii
TOOL=$HOME/energy-efficiency-sort/power_log.py
GEN=$HOME/Downloads/gensort-1.5/gensort
OUT=$HOME/energy-efficiency-sort
STAMP=$(date +%Y%m%d-%H%M)
LOG=$OUT/scaling-$STAMP.log

IDLE=2.84
CYCLES=3
DRAIN=120

SMALL=250000000     # 25 GB, 50 runs
LARGE=500000000     # 47 GB, 100 runs

die() { print -u2 "ABORT: $*"; exit 1 }

[[ -x $TOOL ]] || die "$TOOL is not executable"
[[ -x $GEN ]]  || die "gensort not found at $GEN"
[[ -f $DATA/input.txt || -f $DATA/input47.txt ]] || die "no 47 GB input present"

sudo -v || die "sudo is needed for purge"
while true; do sudo -n true 2>/dev/null; sleep 50; kill -0 $$ 2>/dev/null || exit; done &
KEEPALIVE=$!
caffeinate -dim -w $$ &
CAFFEINE=$!

# Whatever happens, put the inputs back where they belong.
restore() {
    [[ -f $DATA/input.txt && ! -f $DATA/input47.txt && $CUR == 47 ]] && \
        mv $DATA/input.txt $DATA/input47.txt
    [[ -f $DATA/input.txt && ! -f $DATA/input25.txt && $CUR == 25 ]] && \
        mv $DATA/input.txt $DATA/input25.txt
    kill $KEEPALIVE $CAFFEINE 2>/dev/null
}
CUR=none
trap restore EXIT

typeset -a RESULTS
START=$SECONDS

full() { df -k /System/Volumes/Data | tail -1 | awk '{printf "%d%%", $5}' }

# tag, records, phase, command...
timed() {
    local tag=$1 recs=$2 phase=$3; shift 3
    sync; sleep $DRAIN; sudo purge
    print "\n--- $tag $phase  (disk $(full)) ---"
    local out
    out=$( cd $DATA && $TOOL --records $recs --idle-watts $IDLE \
               --csv $OUT/scale-$tag-$phase.csv -- "$@" 2>&1 )
    print "$out"
    local e t p
    e=$(print "$out" | awk '/^  energy /{print $2; exit}')
    t=$(print "$out" | awk '/^  samples .* over /{print $4; exit}' | tr -d 's')
    p=$(print "$out" | awk '/^  sys_power /{print $2; exit}')
    if [[ -n $e && -n $t && -n $p ]]; then
        RESULTS+=("$tag|$phase|$e|$t|$p")
    else
        print "  ($tag $phase produced no parseable result)"
    fi
}

# size (25|47), cycle number
one_sort() {
    local size=$1 cyc=$2 recs
    [[ $size == 25 ]] && recs=$SMALL || recs=$LARGE
    CUR=$size
    mv $DATA/input$size.txt $DATA/input.txt
    print "\n========== ${size}GB cycle $cyc =========="
    rm -f $DATA/run*.dat $DATA/output.dat
    timed "${size}-c$cyc" $recs split ../split_program 5000000 8
    rm -f $DATA/output.dat
    timed "${size}-c$cyc" $recs merge ../merge_program 10
    mv $DATA/input.txt $DATA/input$size.txt
    CUR=none
    rm -f $DATA/run*.dat $DATA/output.dat
}

{
    print "scaling slope  $STAMP"
    print "sizes: ${SMALL} and ${LARGE} records, ${CYCLES} interleaved cycles"
    print "idle=${IDLE}W"

    # Stage the two inputs.
    [[ -f $DATA/input.txt ]] && mv $DATA/input.txt $DATA/input47.txt
    rm -f $DATA/run*.dat $DATA/output.dat
    if [[ ! -f $DATA/input25.txt ]]; then
        print "\ngenerating the 25 GB input (~30s)"
        $GEN -a $SMALL $DATA/input25.txt || die "gensort failed"
    fi
    print "disk after staging: $(full)"

    for c in $(seq 1 $CYCLES); do
        one_sort 25 $c
        one_sort 47 $c
    done

    print "\n\n========== summary =========="
    # Full-sort energy per size per cycle, then the slope from the pair.
    # Every division below is forced to float: zsh does integer division on
    # $(( a / b )) when both sides are integers, and rj47/rj25 would truncate
    # to 0, reporting a 100% slope on any input.
    typeset -a slopes
    printf "  %-8s %10s %10s %12s %12s %8s\n" \
           cycle "25GB J" "47GB J" "25GB rec/J" "47GB rec/J" slope
    rj47sum=0.0; n47=0
    for c in $(seq 1 $CYCLES); do
        s25=0; s47=0
        for r in $RESULTS; do
            tag=${r%%|*}; rest=${r#*|}
            ph=${rest%%|*}; rest=${rest#*|}
            e=${rest%%|*}
            [[ $tag == "25-c$c" ]] && s25=$(( s25 + e ))
            [[ $tag == "47-c$c" ]] && s47=$(( s47 + e ))
        done
        if (( s25 > 0 && s47 > 0 )); then
            rj25=$(( SMALL * 1.0 / s25 ))
            rj47=$(( LARGE * 1.0 / s47 ))
            d=$(( 100.0 * (1.0 - rj47 / rj25) ))
            slopes+=($d)
            rj47sum=$(( rj47sum + rj47 )); n47=$(( n47 + 1 ))
            printf "  %-8s %10.0f %10.0f %12.0f %12.0f %7.1f%%\n" \
                   "$c" $s25 $s47 $rj25 $rj47 $d
        fi
    done

    if (( ${#slopes} > 0 )); then
        tot=0.0
        for d in $slopes; do tot=$(( tot + d )); done
        mean=$(( tot / ${#slopes} ))
        base=$(( rj47sum / n47 ))
        # 5e8 -> 1e10 is log2(20) = 4.3219 doublings. x1.1 for charger loss,
        # which sys_power excludes by construction.
        proj_rj=$(( base * ((1.0 - mean / 100.0) ** 4.3219) ))
        proj_kj=$(( 1e10 / proj_rj / 1000.0 * 1.1 ))
        print ""
        printf "  mean slope        %6.1f%% per doubling\n" $mean
        printf "  47GB baseline     %6.0f records/joule\n" $base
        printf "  projected 1e10    %6.0f records/joule\n" $proj_rj
        printf "                    %6.1f kJ wall-side (x1.1 for charger loss)\n" $proj_kj
        print "\n  One doubling measured at the small end, extrapolated across"
        print "  4.32 more. It assumes the loss stays constant per doubling,"
        print "  which is a model rather than a measurement."
    fi

    print "\nelapsed $(( (SECONDS - START) / 60 )) min"
    print "log: $LOG"
} 2>&1 | tee $LOG

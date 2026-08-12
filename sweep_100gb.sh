#!/bin/zsh
#
# 100 GB point, to settle the scaling slope.
#
# Two estimates currently disagree. Measuring 25 vs 47 GB gave 4.2% loss per
# doubling. The withdrawn 100 GB figure, rescaled by the same 1.42x measurement
# correction found at 47 GB, implies about 13.5%. Both are shaky -- one is a
# single doubling at the small end, the other is a discarded measurement crudely
# adjusted -- but they disagree in the pessimistic direction, so the question is
# whether the slope steepens with size.
#
# 5e8 -> 1e9 records is exactly one doubling from the existing 47 GB result.
#
# KNOWN CONFOUND, and it only points one way. A 100 GB sort needs ~186 GiB of
# working set against ~93 GiB for 47 GB, so it runs at ~66% full where the 47 GB
# slope was measured at 54-59%. Higher fullness penalises the LARGER run, so the
# measured slope comes out at least as steep as the truth:
#
#   result near 4.2%   -> conclusive, the true slope is flatter still
#   result near 13.5%  -> ambiguous, could be real steepening or the fullness
#
# Matching fullness would need the 47 GB run ballasted, and holding both inputs
# plus ballast does not fit on a 1 TB drive. So the bias is stated, not removed.
#
# Bonus data point: 1e9 records at 5M per run is a 200-way merge, twice the
# fan-in of the 47 GB case. Not the 2000-way that 1e10 would need, but a step.
#
# Unattended, roughly 20 minutes, ~700 GB written. Needs sudo for purge/pmset.
#
#   ./sweep_100gb.sh
#
# Do not close the lid, stay on battery, set brightness once and leave it.

setopt NULL_GLOB
set -u

DATA=$HOME/mergesort/ascii
TOOL=$HOME/energy-efficiency-sort/power_log.py
GEN=$HOME/Downloads/gensort-1.5/gensort
OUT=$HOME/energy-efficiency-sort
STAMP=$(date +%Y%m%d-%H%M)
LOG=$OUT/100gb-$STAMP.log

IDLE=2.84
RECS=1000000000        # 1e9 records, 100 GB, 200 runs
REPS=3
DRAIN=120
NEED_GIB=200           # working-set headroom required before we start

# 47 GB reference, from the three interleaved scaling cycles:
# (582072 + 567537 + 578704) / 3. Same protocol, same machine, LPM on.
REF47=576104

die() { print -u2 "ABORT: $*"; exit 1 }

[[ -x $TOOL ]] || die "$TOOL is not executable"
[[ -x $GEN ]]  || die "gensort not found at $GEN"

avail=$(df -g /System/Volumes/Data | tail -1 | awk '{print $4}')
(( avail >= NEED_GIB )) || die "only ${avail} GiB free, need ${NEED_GIB}. Delete input25/input47 first."

sudo -v || die "sudo is needed for purge and pmset"
while true; do sudo -n true 2>/dev/null; sleep 50; kill -0 $$ 2>/dev/null || exit; done &
KEEPALIVE=$!
caffeinate -dim -w $$ &
CAFFEINE=$!
trap 'kill $KEEPALIVE $CAFFEINE 2>/dev/null' EXIT

typeset -a RESULTS
START=$SECONDS

full() { df -k /System/Volumes/Data | tail -1 | awk '{printf "%d%%", $5}' }

brightness() {
    ioreg -c AppleARMBacklight -r 2>/dev/null |
        grep -o '"brightness"={[^}]*"value"=[0-9]*' |
        grep -o '[0-9]*$' | head -1
}

# phase, then the command
timed() {
    local phase=$1; shift
    local i out e t p
    for i in $(seq 1 $REPS); do
        [[ $phase == split ]] && rm -f $DATA/run*.dat
        [[ $phase == merge ]] && rm -f $DATA/output.dat
        sync
        sleep $DRAIN
        sudo purge
        print "\n--- $phase rep $i   (disk $(full)) ---"
        out=$( cd $DATA && $TOOL --records $RECS --idle-watts $IDLE \
                   --csv $OUT/100gb-$phase-$i.csv -- "$@" 2>&1 )
        print "$out"
        e=$(print "$out" | awk '/^  energy /{print $2; exit}')
        t=$(print "$out" | awk '/^  samples .* over /{print $4; exit}' | tr -d 's')
        p=$(print "$out" | awk '/^  sys_power /{print $2; exit}')
        if [[ -n $e && -n $t && -n $p ]]; then
            RESULTS+=("$phase|$i|$e|$t|$p")
        else
            print "  ($phase rep $i produced no parseable result)"
        fi
    done
}

BRIGHT_START=$(brightness)

{
    print "100 GB scaling point  $STAMP"
    print "records=$RECS  reps=$REPS  idle=${IDLE}W  brightness=$BRIGHT_START/65536"
    print "47 GB reference: $REF47 records/joule"

    # The baseline was measured at powermode 1. Anything else invalidates the
    # comparison -- powermode 2 is High Power Mode on this machine, never
    # characterised, and 0 is off, which measured 31% worse.
    sudo pmset -a lowpowermode 1
    print "[lowpowermode forced to 1]"
    pmset -g | grep powermode

    print "\nclearing old inputs and generating 100 GB (~2 min)"
    rm -f $DATA/input25.txt $DATA/input47.txt $DATA/run*.dat $DATA/output.dat
    rm -f $DATA/input.txt
    $GEN -a $RECS $DATA/input.txt || die "gensort failed"
    print "disk after staging: $(full)"
    df -h /System/Volumes/Data | tail -1
    smartctl -a /dev/disk0 2>/dev/null | grep -E "Temperature:|Percentage Used:"

    print "\n========== split (input present) =========="
    timed split ../split_program 5000000 8

    print "\nrun files: $(ls $DATA/run*.dat | wc -l | tr -d ' ') -- deleting input for the merge phase"
    rm -f $DATA/input.txt

    print "\n========== merge (input deleted) =========="
    timed merge ../merge_program 10

    print "\n\n========== summary =========="
    # Every division forced to float: zsh does integer division on $(( a / b )).
    sp=0.0; ns=0; mg=0.0; nm=0
    for r in $RESULTS; do
        ph=${r%%|*}; rest=${r#*|}; rest=${rest#*|}; e=${rest%%|*}
        [[ $ph == split ]] && { sp=$(( sp + e )); ns=$(( ns + 1 )) }
        [[ $ph == merge ]] && { mg=$(( mg + e )); nm=$(( nm + 1 )) }
    done

    if (( ns > 0 && nm > 0 )); then
        smean=$(( sp / ns )); mmean=$(( mg / nm ))
        total=$(( smean + mmean ))
        rj100=$(( RECS * 1.0 / total ))
        slope=$(( 100.0 * (1.0 - rj100 / REF47) ))
        printf "  split mean       %8.1f J\n" $smean
        printf "  merge mean       %8.1f J\n" $mmean
        printf "  100 GB total     %8.1f J\n" $total
        printf "  100 GB rec/J     %8.0f   (47 GB was %d)\n" $rj100 $REF47
        printf "  slope            %8.1f%% per doubling\n" $slope
        # 1e9 -> 1e10 is log2(10) = 3.3219 doublings.
        proj=$(( rj100 * ((1.0 - slope / 100.0) ** 3.3219) ))
        printf "  projected 1e10   %8.0f rec/J\n" $proj
        printf "                   %8.1f kJ wall-side (x1.1 charger)\n" \
               $(( 1e10 / proj / 1000.0 * 1.1 ))
        print ""
        print "  Read against the two prior estimates: 4.2% from 25-vs-47 GB,"
        print "  ~13.5% implied by the rescaled withdrawn 100 GB figure."
        print "  Fullness bias makes this AT LEAST as steep as the truth, so a"
        print "  low number here is conclusive and a high one is ambiguous."
    else
        print "  incomplete -- missing a phase, raw rows only"
    fi

    BRIGHT_END=$(brightness)
    print ""
    if [[ $BRIGHT_START != $BRIGHT_END ]]; then
        print "WARNING: brightness moved $BRIGHT_START -> $BRIGHT_END. Numbers suspect."
    else
        print "brightness held at $BRIGHT_END/65536 throughout"
    fi
    pmset -g | grep powermode
    smartctl -a /dev/disk0 2>/dev/null | grep -E "Temperature:|Percentage Used:"
    print "elapsed $(( (SECONDS - START) / 60 )) min"
    print "log: $LOG"
} 2>&1 | tee $LOG

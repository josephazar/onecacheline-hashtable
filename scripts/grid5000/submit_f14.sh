#!/bin/bash
# Soumet la campagne F14 en queue normale (default, non-préemptible) sur tous les
# clusters. x86 -> g5k_run.sh direct ; ARM -> g5k_arm.sh (exotic, setup+run).
# Écrit la table TAG:site:jobid:path dans jobs_f14.conf pour le watcher unifié.
OUT=jobs_f14.conf; : > $OUT

# site cluster tag arch
JOBS=(
  "lille    chiclet  chiclet_zen1              x86"
  "lille    chuc     chuc_zen3                 x86"
  "lille    chirop   lille_chirop_icelake      x86"
  "nancy    grele    nancy_grele_broadwell     x86"
  "nancy    gros     nancy_gros_cascadelake    x86"
  "nancy    grvingt  nancy_grvingt_skylake     x86"
  "nancy    grat     nancy_grat_zen3           x86"
  "nancy    gres     nancy_gres_zen4           x86"
  "nancy    gruss    nancy_gruss_zen2          x86"
  "grenoble vercors14 grenoble_spr             x86"
  "lyon     pyxis    lyon_pyxis_thunderx2      arm"
)

for row in "${JOBS[@]}"; do
  read -r site cluster tag arch <<< "$row"
  if [ "$arch" = arm ]; then
    cmd="oarsub -q default -t exotic -p \"cluster='$cluster'\" -l host=1,walltime=2:0:0 -O ~/oar.%jobid%.out -E ~/oar.%jobid%.err 'bash ~/g5k_arm.sh $tag'"
  else
    cmd="oarsub -q default -p \"cluster='$cluster'\" -l host=1,walltime=1:30:0 -O ~/oar.%jobid%.out -E ~/oar.%jobid%.err 'bash ~/g5k_run.sh $tag'"
  fi
  resp=$(timeout 40 ssh ${site}.g5k "$cmd" 2>&1)
  jid=$(echo "$resp" | grep -oE 'OAR_JOB_ID=[0-9]+' | cut -d= -f2)
  if [ -n "$jid" ]; then
    echo "${tag}:${site}:${jid}:~/htbench_work/results_${tag}" >> $OUT
    printf "OK   %-26s %s/%s job %s\n" "$tag" "$site" "$cluster" "$jid"
  else
    printf "FAIL %-26s %s/%s : %s\n" "$tag" "$site" "$cluster" "$(echo "$resp" | grep -iE 'error|not enough' | head -1)"
  fi
done
echo "--- jobs_f14.conf ---"; cat $OUT

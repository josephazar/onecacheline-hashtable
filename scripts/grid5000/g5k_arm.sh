#!/usr/bin/env bash
# Wrapper ARM : construit l'env conda aarch64 sur le noeud (premiere fois) puis
# lance la campagne. Un seul argument = TAG. (oarsub n'aime pas le '&&' inline.)
set -uo pipefail
bash ~/g5k_setup.sh && bash ~/g5k_run.sh "${1:-arm_node}"

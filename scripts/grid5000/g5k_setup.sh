#!/usr/bin/env bash
set -e
cd ~
# Prefixe conda specifique a l'archi : les frontends G5K sont x86 mais partagent le
# home NFS avec les noeuds ARM -> un env x86 ne tourne pas sur un noeud aarch64.
CH=~/miniconda3
case "$(uname -m)" in
  aarch64|arm64) CH=~/miniconda3-aarch64; MC=Miniconda3-latest-Linux-aarch64.sh ;;
  *)             MC=Miniconda3-latest-Linux-x86_64.sh ;;
esac
if [ ! -d "$CH" ]; then
  echo "[setup] miniconda ($(uname -m)) -> $CH ..."
  curl -sSL -o /tmp/mc.sh https://repo.anaconda.com/miniconda/$MC
  bash /tmp/mc.sh -b -p "$CH"
fi
source "$CH"/etc/profile.d/conda.sh
if ! conda env list | grep -q htbench; then
  echo "[setup] env htbench (conda-forge)..."
  conda create -y -n htbench --override-channels -c conda-forge \
    gxx=13 abseil-cpp libboost-headers numactl pkg-config folly > ~/htbench_setup_$(uname -m).log 2>&1
fi
conda activate htbench
# folly (F14ValueMap) peut manquer si l'env date d'avant l'ajout de F14 -> l'installer.
if [ ! -f "$CONDA_PREFIX/include/folly/container/F14Map.h" ]; then
  echo "[setup] ajout folly (F14) a l'env existant..."
  conda install -y -n htbench --override-channels -c conda-forge folly >> ~/htbench_setup_$(uname -m).log 2>&1
fi
echo "g++ = $(g++ --version|head -1)"
ls $CONDA_PREFIX/include/absl/container/flat_hash_map.h >/dev/null 2>&1 && echo "abseil OK" || echo "abseil MANQUE"
ls $CONDA_PREFIX/include/boost/unordered/unordered_flat_map.hpp >/dev/null 2>&1 && echo "boost OK" || echo "boost MANQUE"
ls $CONDA_PREFIX/include/folly/container/F14Map.h >/dev/null 2>&1 && echo "folly/F14 OK" || echo "folly/F14 MANQUE"
echo SETUP_DONE

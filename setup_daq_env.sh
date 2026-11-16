#!/usr/bin/env bash
# Окружение DANSS DAQ / Slow Control на Linux x86_64 — целиком в $HOME, система не меняется.
#
#   bash scripts/setup_daq_env.sh [--no-midas] [--no-experiment] [--no-build] [--repo <путь к danss_daq>]
#
# Что делает (каждый шаг пропускается, если уже сделан — скрипт можно запускать повторно):
#   1. Miniforge в $CONDA_DIR (по умолчанию ~/miniforge3); `conda init` НЕ запускается, .bashrc не трогается.
#   2. Окружение conda `daq`: компиляторы GCC 13, CMake, Boost, nlohmann_json, zlib,
#      ROOT 6.40.04 (C++20), Python. Системные gcc/g++ и пакеты не меняются.
#   3. MIDAS c69c085 (тот же коммит, что на всех машинах) в $DANSS_BASE/packages/midas,
#      сборка компиляторами окружения, с ROOT; без MySQL/PostgreSQL/ODBC.
#   4. Тестовый эксперимент MIDAS `danss` в $DANSS_BASE/online (exptab, ODB 32 МБ).
#   5. Сборка danss_daq (если указан --repo или скрипт лежит в репозитории) и ctest.
#   6. Файл $DANSS_BASE/danss_env.sh — подключать в каждом новом терминале:
#        source ~/daq/danss_env.sh
#
# Переменные (необязательно): DANSS_BASE (~/daq), CONDA_DIR (~/miniforge3), ENV_NAME (daq),
# MIDAS_URL (https://gitlab.triumf.ca/midas/midas.git).
#
# Удалить всё: rm -rf ~/miniforge3 ~/.conda ~/daq (и строку source из .bashrc, если добавляли).

set -euo pipefail

DANSS_BASE=${DANSS_BASE:-$HOME/daq}
CONDA_DIR=${CONDA_DIR:-$HOME/miniforge3}
ENV_NAME=${ENV_NAME:-daq}
MIDAS_URL=${MIDAS_URL:-https://gitlab.triumf.ca/midas/midas.git}
MIDAS_COMMIT=${MIDAS_COMMIT:-c69c0853930b226e96090725a0ba5cb2b4293a14}  # один коммит на всех машинах — не менять
ROOT_VERSION=6.40.04
EXPT_NAME=danss

DO_MIDAS=1; DO_EXPT=1; DO_BUILD=1; REPO=""
while [ $# -gt 0 ]; do
    case "$1" in
        --no-midas) DO_MIDAS=0 ;;
        --no-experiment) DO_EXPT=0 ;;
        --no-build) DO_BUILD=0 ;;
        --repo) REPO="$2"; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
if [ -z "$REPO" ]; then
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    [ -f "$here/CMakeLists.txt" ] && [ -d "$here/src/frontend" ] && REPO="$here"
fi

step() { printf '\n\033[1m=== %s\033[0m\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || die "этот скрипт — для Linux (на Mac: Homebrew, см. doc/BUILD.md)"
[ "$(uname -m)" = x86_64 ] || die "нужен x86_64 (сейчас $(uname -m))"
grep -q '^PRETTY_NAME' /etc/os-release 2>/dev/null && echo "OS: $(. /etc/os-release; echo "$PRETTY_NAME")"
echo "DANSS_BASE=$DANSS_BASE  CONDA_DIR=$CONDA_DIR  ENV_NAME=$ENV_NAME  REPO=${REPO:-<нет>}"

# ---- 1. Miniforge -----------------------------------------------------------------------
step "1. Miniforge в $CONDA_DIR"
if [ -x "$CONDA_DIR/bin/conda" ]; then
    echo "уже установлен: $("$CONDA_DIR/bin/conda" --version)"
else
    inst=$(mktemp -d)/Miniforge3.sh
    curl -fsSL -o "$inst" https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh
    bash "$inst" -b -p "$CONDA_DIR"
    rm -f "$inst"
fi
export CONDA_PKGS_DIRS=${CONDA_PKGS_DIRS:-$HOME/.conda/pkgs}

# ---- 2. Окружение conda -----------------------------------------------------------------
step "2. Окружение conda '$ENV_NAME' (GCC 13, CMake, Boost, json, ROOT $ROOT_VERSION C++20)"
PKGS=(gcc_linux-64=13 gxx_linux-64=13 cmake make git pkg-config
      libboost-devel nlohmann_json zlib python
      "root=$ROOT_VERSION" root_cxx_standard=20)
if [ -d "$CONDA_DIR/envs/$ENV_NAME" ]; then
    echo "уже есть; доустанавливаю недостающее (если всё на месте — ничего не меняется)"
    "$CONDA_DIR/bin/conda" install -y -n "$ENV_NAME" -c conda-forge "${PKGS[@]}"
else
    "$CONDA_DIR/bin/conda" create -y -n "$ENV_NAME" -c conda-forge "${PKGS[@]}"
fi

# Активация только в этом скрипте: задаёт CC/CXX/CMAKE_ARGS окружения (sysroot conda,
# поиск библиотек внутри окружения), PATH к root-config
set +u
# shellcheck disable=SC1091
source "$CONDA_DIR/bin/activate" "$ENV_NAME"
set -u
echo "compiler: $($CXX --version | head -1)"
echo "cmake:    $(cmake --version | head -1)"
echo "ROOT:     $(root-config --version) (C++ $(root-config --cflags | grep -o 'std=c++[0-9a-z]*' | head -1))"

# ---- 6 (раньше остальных: нужен шагам 3–5). Файл окружения ----------------------------
ENVFILE="$DANSS_BASE/danss_env.sh"
mkdir -p "$DANSS_BASE/packages" "$DANSS_BASE/online"
cat > "$ENVFILE" <<EOF
# DANSS DAQ: окружение для терминала. Подключать: source $ENVFILE
# Создан scripts/setup_daq_env.sh $(date '+%Y-%m-%d'); правки — здесь.
source "$CONDA_DIR/bin/activate" "$ENV_NAME"
export CONDA_PKGS_DIRS=$CONDA_PKGS_DIRS
export MIDASSYS=$DANSS_BASE/packages/midas
export MIDAS_EXPTAB=$DANSS_BASE/online/exptab
export MIDAS_EXPT_NAME=$EXPT_NAME
export PATH=\$MIDASSYS/bin:\$PATH
export ROOTSYS=\$CONDA_PREFIX
EOF
echo "файл окружения: $ENVFILE"
export MIDASSYS=$DANSS_BASE/packages/midas
export MIDAS_EXPTAB=$DANSS_BASE/online/exptab
export MIDAS_EXPT_NAME=$EXPT_NAME
export PATH=$MIDASSYS/bin:$PATH

# ---- 3. MIDAS ---------------------------------------------------------------------------
if [ "$DO_MIDAS" = 1 ]; then
    step "3. MIDAS $MIDAS_COMMIT в $MIDASSYS"
    if [ ! -e "$MIDASSYS" ]; then
        git clone "$MIDAS_URL" "$MIDASSYS"
    fi
    if [ -d "$MIDASSYS/.git" ]; then
        cur=$(git -C "$MIDASSYS" rev-parse HEAD)
        if [ "$cur" != "$MIDAS_COMMIT" ]; then
            git -C "$MIDASSYS" fetch origin
            git -C "$MIDASSYS" checkout "$MIDAS_COMMIT"
        fi
        # Подмодули — ровно те, что записаны в этом коммите (без --remote)
        git -C "$MIDASSYS" submodule sync --recursive
        git -C "$MIDASSYS" submodule update --init --recursive
    else
        echo "WARNING: $MIDASSYS не git-репозиторий — собираю как есть (коммит проверить нельзя)"
    fi
    if [ -x "$MIDASSYS/bin/mhttpd" ] && [ -f "$MIDASSYS/build/.danss_built_$MIDAS_COMMIT" ]; then
        echo "уже собран"
    else
        mkdir -p "$MIDASSYS/build"
        (
            cd "$MIDASSYS/build"
            # shellcheck disable=SC2086
            cmake $CMAKE_ARGS -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                  -DNO_MYSQL=ON -DNO_PGSQL=ON -DNO_ODBC=ON -DNO_SQLITE=ON -DNO_CURL=ON -DNO_NVIDIA=ON ..
            make -j"$(nproc)"
            make install
            touch ".danss_built_$MIDAS_COMMIT"
        )
    fi
    ls "$MIDASSYS/bin" | grep -qx mhttpd || die "MIDAS: mhttpd не собран"
    if ldd "$MIDASSYS/bin/manalyzer_example_root.exe" 2>/dev/null | grep -q libCore; then echo "MIDAS собран с ROOT (manalyzer)"
    else echo "WARNING: MIDAS собран без ROOT"; fi
fi

# ---- 4. Эксперимент ---------------------------------------------------------------------
if [ "$DO_MIDAS" = 1 ] && [ "$DO_EXPT" = 1 ]; then
    step "4. Эксперимент '$EXPT_NAME' в $DANSS_BASE/online"
    if [ ! -f "$MIDAS_EXPTAB" ]; then
        echo "$EXPT_NAME $DANSS_BASE/online $(id -un)" > "$MIDAS_EXPTAB"
    fi
    cat "$MIDAS_EXPTAB"
    if [ -e "$DANSS_BASE/online/.ODB.SHM" ]; then
        echo "ODB уже есть"
    else
        (cd "$DANSS_BASE/online" && "$MIDASSYS/bin/odbinit" -s 32MB)
    fi
    # Сменилось имя компьютера — MIDAS откажется подключаться (.SHM_HOST.TXT); файл пересоздаётся сам
    h="$DANSS_BASE/online/.SHM_HOST.TXT"
    if [ -f "$h" ] && [ "$(cat "$h")" != "$(hostname)" ]; then
        echo "имя компьютера сменилось ($(cat "$h") -> $(hostname)): удаляю .SHM_HOST.TXT"
        rm -f "$h"
    fi
fi

# ---- 5. danss_daq -----------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ] && [ -n "$REPO" ]; then
    step "5. Сборка danss_daq в $REPO/build"
    WM=ON; [ "$DO_MIDAS" = 1 ] || WM=OFF
    mkdir -p "$REPO/build"
    (
        cd "$REPO/build"
        # shellcheck disable=SC2086
        cmake $CMAKE_ARGS -DWITH_MIDAS=$WM -DBUILD_TESTS=ON ..
        make -j"$(nproc)"
        ctest --output-on-failure
    )
    ls "$REPO/build/bin"
elif [ "$DO_BUILD" = 1 ]; then
    echo; echo "danss_daq не собирался: укажите --repo <путь к клону> или запустите скрипт из репозитория"
fi

step "Готово"
cat <<EOF
В каждом новом терминале:   source $ENVFILE
(или один раз:  echo 'source $ENVFILE' >> ~/.bashrc)

Запуск MIDAS:   mhttpd -D; mlogger -D      → http://localhost:8080
Платы:          ${REPO:-<repo>}/build/bin/danss_sc_fe -- --config ${REPO:-<repo>}/config/board_map_stand.json
EOF

#!/usr/bin/env bash
#
# build.sh - compileaza plugin-ul WorldSvr (.so) pe Linux si, optional, il
# deployeaza in containerul Docker al serverului.
#
# Utilizare:
#   ./build.sh              # doar compileaza -> WorldSvr/WorldSvr.so
#   ./build.sh deploy       # compileaza + copiaza ca lib.so in container + restart
#
# Config prin variabile de mediu (au valori implicite):
#   CXX        compilatorul (default: clang++, altfel g++)
#   CONTAINER  numele containerului           (default: cabal_main)
#   CORE_DIR   directorul de lucru WorldSvr   (default: /etc/cabal_etc/core)
#   PROGRAM    programul supervisor de restart(default: WorldSvr_01_04)
#
# Exemplu: CONTAINER=cabal_test ./build.sh deploy
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/WorldSvr"
OUT="$SRC_DIR/WorldSvr.so"

# --- alege compilatorul ---
CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
    if command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    elif command -v g++ >/dev/null 2>&1; then
        CXX=g++
    else
        echo "EROARE: nu am gasit clang++ sau g++. Instaleaza: sudo apt install -y clang" >&2
        exit 1
    fi
fi

# --- aduna toate sursele (exclude directoarele de build) ---
mapfile -t SRCS < <(cd "$SRC_DIR" && find . -name '*.cpp' -not -path './obj/*' -not -path './bin/*' | sort)
if [[ ${#SRCS[@]} -eq 0 ]]; then
    echo "EROARE: nu am gasit fisiere .cpp in $SRC_DIR" >&2
    exit 1
fi

# --- flag-uri (identice cu configuratia Release|x64 din .vcxproj) ---
# -Wno-*: doar zgomot inofensiv (pattern-ul extern-cu-initializer si un typedef).
CXXFLAGS=(-std=c++17 -fpermissive -pthread -O2 -I.
          -Wno-non-c-typedef-for-linkage -Wno-extern-initializer)
# Plugin-ul e NON-PIC si trebuie sa se incarce la 0x05000000 (hook-urile CALL
# rel32 depind de adresa joasa fixa). Linker-ele moderne (Ubuntu 24.04) refuza
# implicit relocarile din .text: le reactivam pastrand non-PIC.
#   -Bsymbolic : leaga referintele interne local -> elimina eroarea PC32
#   -z notext  : permite relocarile ramase in .text
LDFLAGS=(-shared -Wl,-Ttext-segment=0x05000000 -Wl,-Bsymbolic -Wl,-z,notext -ldl)

echo "==> Compilez ${#SRCS[@]} fisiere cu $CXX ..."
printf '      %s\n' "${SRCS[@]}"
( cd "$SRC_DIR" && "$CXX" "${CXXFLAGS[@]}" "${LDFLAGS[@]}" -o WorldSvr.so "${SRCS[@]}" )

echo "==> Build OK: $OUT"
command -v file >/dev/null 2>&1 && file "$OUT" || true

# --- deploy optional ---
if [[ "${1:-}" == "deploy" ]]; then
    CONTAINER="${CONTAINER:-cabal_main}"
    CORE_DIR="${CORE_DIR:-/etc/cabal_etc/core}"
    PROGRAM="${PROGRAM:-WorldSvr_01_04}"

    if ! command -v docker >/dev/null 2>&1; then
        echo "EROARE: docker nu e disponibil pentru deploy." >&2
        exit 1
    fi

    echo "==> Copiez in $CONTAINER:$CORE_DIR/lib.so ..."
    docker cp "$OUT" "$CONTAINER:$CORE_DIR/lib.so"

    echo "==> Restart $PROGRAM prin supervisor ..."
    docker exec "$CONTAINER" supervisorctl restart "$PROGRAM"

    echo "==> Gata. Vezi log-ul cu:"
    echo "      docker exec -it $CONTAINER tail -f $CORE_DIR/AutoPlay.log"
fi

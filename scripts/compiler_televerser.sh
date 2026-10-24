#!/usr/bin/env bash
# ============================================================================
#  Station AURA : compilation et téléversement sur la NUCLEO-L476RG
#
#  Usage :  ./scripts/compiler_televerser.sh [debug|release] [--sans-flash] [--moniteur]
#    debug        compilation avec symboles de débogage, sans optimisation (par défaut)
#    release      compilation optimisée
#    --sans-flash compile seulement, sans téléverser
#    --moniteur   ouvre le moniteur série à 115200 bauds après le téléversement
#
#  Étapes : 1. numéro de build incrémenté  2. configuration CMake  3. compilation
#           4. téléversement par ST-LINK (STM32_Programmer_CLI)  5. moniteur série
# ============================================================================
set -euo pipefail

MODE="debug"
FLASH=1
MONITEUR=0
for arg in "$@"; do
  case "$arg" in
    debug|release) MODE="$arg" ;;
    --sans-flash)  FLASH=0 ;;
    --moniteur)    MONITEUR=1 ;;
    -h|--help)     sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "Option inconnue : $arg (utiliser --help)"; exit 1 ;;
  esac
done

# Le script fonctionne depuis n'importe quel dossier : on se place à la racine du projet
cd "$(dirname "$0")/.."
PRESET=$([ "$MODE" = "release" ] && echo "Release" || echo "Debug")
ELF="build/$PRESET/Aura.elf"

# ---- 1. Numéro de build ----
FICHIER_BUILD="scripts/numero_build.txt"
NUM=$(( $(cat "$FICHIER_BUILD" 2>/dev/null || echo 0) + 1 ))
echo "$NUM" > "$FICHIER_BUILD"
cat > Core/Inc/version_build.h <<EOF
/* Fichier généré par scripts/compiler_televerser.sh : ne pas modifier à la main */
#define BUILD_NUMERO "$NUM"
#define BUILD_MODE   "$MODE"
EOF
echo "==> Build n°$NUM en mode $MODE"

# ---- 2. Configuration CMake (une seule fois par preset) ----
if [ ! -f "build/$PRESET/build.ninja" ]; then
  echo "==> Configuration CMake ($PRESET)"
  cmake --preset "$PRESET"
fi

# ---- 3. Compilation ----
echo "==> Compilation"
cmake --build "build/$PRESET"

# ---- 4. Téléversement ----
if [ "$FLASH" -eq 1 ]; then
  CLI=$(command -v STM32_Programmer_CLI || true)
  for p in "/opt/st/stm32cubeprogrammer/bin/STM32_Programmer_CLI" \
           "$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI" \
           "/c/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"; do
    [ -z "$CLI" ] && [ -x "$p" ] && CLI="$p"
  done
  if [ -z "$CLI" ]; then
    echo "ERREUR : STM32_Programmer_CLI introuvable (installer STM32CubeProgrammer)"; exit 2
  fi
  echo "==> Téléversement de $ELF par ST-LINK"
  "$CLI" -c port=SWD mode=UR reset=HWrst -w "$ELF" -v -rst
fi

# ---- 5. Moniteur série ----
if [ "$MONITEUR" -eq 1 ]; then
  PORT=$(ls /dev/ttyACM* 2>/dev/null | head -n 1 || true)
  if [ -z "$PORT" ]; then echo "Aucun port /dev/ttyACM* : carte branchée ?"; exit 3; fi
  echo "==> Moniteur série sur $PORT à 115200 bauds (quitter : Ctrl+A puis Ctrl+X)"
  picocom -b 115200 "$PORT"
fi

echo "==> Terminé : build n°$NUM ($MODE)"

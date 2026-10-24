#!/usr/bin/env bash
cd "$(dirname "$0")/.." || exit 1
if cmake --preset Debug && cmake --build build/Debug; then
  STM32_Programmer_CLI -c port=SWD mode=UR reset=HWrst -w build/Debug/Aura.elf -v -rst
else echo "Échec de compilation : téléversement annulé" >&2; exit 1; fi

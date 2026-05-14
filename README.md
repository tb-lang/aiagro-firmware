# aiagro-firmware

Firmware das estações e receptoras AiAgro (ESP32 + RA-02 LoRa / WiFi), com **OTA via GitHub**.

## Estrutura

```
aiagro-firmware/
├── receptora_lora/      Receptoras LoRa→WiFi→Supabase (TODAS as fazendas)
├── estacao_pivot/       Estações de pivô (2× sensor 7x1, sem pluviômetro/bateria)
├── estacao_lora_solo/   Estações LoRa fora do pivô (1× 7x1 + pluviômetro + bateria)
└── estacao_wifi/        Estações WiFi diretas (1× 7x1 + pluviômetro + bateria)
```

Cada pasta tem:
- `VERSION` — versão atual do firmware (texto puro, ex: `1`)
- `src/` — código-fonte (genérico, 1 arquivo serve N dispositivos via build_flags)
- `platformio.ini` — um `[env:CODIGO]` por dispositivo, com build_flags específicos
- `builds/` — binários compilados, um `.bin` por dispositivo

## Como funciona o OTA

Dispositivos com WiFi (receptoras + estação WiFi) checam atualização:
1. Lê `VERSION` do GitHub (raw)
2. Compara com a versão compilada no firmware
3. Se diferente, baixa `builds/<CODIGO>.bin` e auto-flasha
4. Reboota na versão nova

**Estações LoRa e Pivot não têm OTA** (ficam longe do roteador, sem WiFi). Atualização só por flash físico.

## Como atualizar um firmware (release)

1. Editar o código em `<tipo>/src/`
2. Incrementar `VERSAO_FW` no código E no arquivo `<tipo>/VERSION` (manter sincronizados)
3. Compilar todos os devices: `cd <tipo> && pio run`
4. Copiar binários: `cp .pio/build/<CODIGO>/firmware.bin builds/<CODIGO>.bin`
5. `git add . && git commit && git push`
6. Dispositivos pegam a atualização no próximo check de OTA

## URLs OTA (padrão)

- Versão: `https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/<tipo>/VERSION`
- Binário: `https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/<tipo>/builds/<CODIGO>.bin`

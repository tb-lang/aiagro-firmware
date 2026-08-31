# Estação AiAgro — firmware do cliente (teste de campo)

Versão `e3` · 31/ago/2026 · base: `laranja_wifi l3`

> **e3 tira o OLED.** Nenhuma placa AiAgro leva display — ele come bateria numa
> estação que precisa durar meses no campo. Saíram as libs `Adafruit_SSD1306` e
> `Adafruit_GFX` e o barramento I2C; o que era tela virou log no serial e, se
> alguém soldar um LED, sinal luminoso (`-DLED_STATUS=21`; os pinos 21 e 22
> ficaram livres com a saída do I2C).
>
> **Consequência de campo:** a estação pedindo WiFi não tem como avisar sozinha.
> Quem avisa é a **etiqueta**, que traz o nome da rede (`AiAgro-XXXX`, os 4
> últimos do MAC) e a senha. Sem ela o produtor não sabe onde conectar — a
> etiqueta deixou de ser conveniência e virou parte do produto.

## O que mudou

Hoje cada estação sai com o WiFi da fazenda **compilado dentro do binário**
(`build_flags`). Isso só funciona porque conhecemos as 5 fazendas. No lote de
pré-venda o produtor põe a internet dele — e não dá pra compilar 500 firmwares.

Neste firmware **não existe SSID nem senha no código**. A estação sobe um WiFi
próprio, o produtor conecta o celular nele e a telinha de configuração abre
sozinha, igual portal de hotel. O mesmo `.bin` serve pra qualquer cliente.

Todo o resto — pinagem, leitura do 7x1 lote velho, fórmula do pH, pluviômetro,
POST no Supabase, OTA — é **igual ao que já roda em campo**. A única coisa a
testar aqui é o WiFi.

## Como gravar

**Caminho curto (sem instalar PlatformIO).** Use
`builds/AiAgro-estacao-e3-COMPLETO.bin` — é o firmware inteiro, já com
bootloader e tabela de partição, gravado num offset só:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 builds/AiAgro-estacao-e3-COMPLETO.bin
```

**No lote, quem grava é o `bancada.sh`** (fora deste repo, em
`~/Desktop/AiAgro/lote20/`): ele lê o MAC, gera a partição NVS daquela unidade
— UUID e, na bancada, a rede de teste — e grava firmware + NVS num comando só.
É assim que a identidade entra sem depender do console serial, que não funciona
no Mac do Tório (driver CH340 travado).

**Caminho do projeto** (dá pra mexer no código):

```bash
pio run -e ESTACAO_CLIENTE -t upload --upload-port /dev/ttyUSB0
```

Depois, monitor serial a 115200 — todo o roteiro abaixo aparece lá.

> ⚠️ A tabela de partição mudou (`min_spiffs`, 1,9 MB por slot de OTA em vez de
> 1,2 MB). Placa que já rodou firmware antigo **precisa deste primeiro flash por
> USB**; partição não muda por OTA. Do lote novo em diante já sai assim.

## O que o firmware de teste faz

- 10 leituras seguidas, uma a cada 15 s, depois dorme 10 min e repete
  (produção é 3 pacotes às 07:30 e dorme o dia).
- Manda pro dispositivo **TESTE_PROVISAO_01**
  (`d15c0a11-7e57-4a1b-9c2e-000000000001`), criado só pra isso — não suja
  os dados de nenhuma estação de produção.

## Roteiro de teste

Marque ✅/❌ em cada item e devolva com o print do serial quando der ❌.

### 1. Primeira configuração
1. Grave e ligue a placa. Sem OLED, o sinal de que o portal subiu é a **rede
   `AiAgro-XXXX` aparecer na lista de WiFi do celular** (senha `aiagro123`) —
   e a linha `[portal] no ar` no serial, se você estiver com ele aberto.
2. No celular, entre nessa rede. **A telinha verde deve abrir sozinha.**
   - iPhone: abre em 1–3 s.
   - Android: às vezes só aparece a notificação "Entrar na rede" — tocar nela.
   - Se não abrir de jeito nenhum: abrir o navegador em `http://192.168.4.1`
     (anotar que não abriu sozinho — isso é resultado de teste, não erro de uso).
   - Aconteceu na rodada 1, num Samsung. Antes de repetir, desligue o **Private
     DNS** (Config › Conexões › Mais › DNS privado › Desativado) e o
     **Intelligent Wi-Fi / trocar para dados móveis** — os dois impedem a
     detecção de portal. Se der, repita também num iPhone.
3. A lista de redes deve aparecer com o sinal de cada uma.
4. Escolha a rede, digite a senha, **Conectar**.
5. Deve aparecer "Conectado!" e a placa reinicia sozinha.

**Esperado:** no serial, `[provisao] OK, salvo`, depois o ciclo normal com
`[Supabase] HTTP 201`.

> A telinha pode congelar por alguns segundos durante o teste — é o rádio do
> ESP32 mudando de canal. Ela volta sozinha. Isso é esperado; só reporte se
> **não** voltar depois de ~20 s.

### 2. Senha errada (o teste mais importante)
Repita o passo 1 digitando uma senha errada de propósito.
**Esperado:** a tela diz *"A rede respondeu mas recusou a senha"* — e **não**
salva. Nada de "salvou e não funciona".

### 3. Rede que não alcança
Escolha "Minha rede não aparece na lista" e digite um nome inventado.
**Esperado:** *"Não achei essa rede daqui"*.

### 4. Roteador sem internet
Se der: desligue o cabo do provedor (deixando o roteador ligado) e configure.
**Esperado:** *"Conectou no roteador mas não chegou na internet"*.

### 5. Sinal fraco
Leve a placa pro limite do alcance (ou ligue num roteador longe).
**Esperado:** abaixo de −80 dBm, *"o sinal aqui é fraco demais"* e **não salva**.
Anotar o dBm que aparecia na lista antes de tentar.

### 6. Reabrir o portal em campo — liga/desliga 3×
Com a estação já configurada e funcionando: desligue e ligue **3 vezes
seguidas**, deixando ligada uns 5 s de cada vez.
**Esperado:** na 3ª, a rede `AiAgro-XXXX` volta a aparecer no celular.
No serial: `power-cycles seguidos: 3/3`.

**Contraprova (importante):** desligue e ligue **uma vez só**, espere o ciclo
inteiro rodar, desligue e ligue de novo. A estação **não** pode abrir o portal —
se abrir, o contador não está zerando e ela vira um pesadelo em campo.

### 7. Sobrevive ao reboot
Tire e recoloque a energia. Ela deve conectar sozinha na rede salva, sem portal.

### 8. A rede sumiu (o caso da Laranja)
Configure numa rede, depois **desligue esse roteador** e deixe a estação rodando.
**Esperado:** ela tenta 3 ciclos, e no 3º abre o portal sozinha em vez de ficar
muda pra sempre. (Com o firmware de teste, ~10 min por ciclo → cerca de 30 min.)

### 9. Duas redes
Configure na rede A, depois na rede B. Desligue B: ela deve cair na A sozinha.

## Conferir se está chegando

```bash
curl -s "https://bwtotmprzmldczafjhrg.supabase.co/rest/v1/leituras?dispositivo_id=eq.d15c0a11-7e57-4a1b-9c2e-000000000001&select=recebido_em,versao_fw,ciclo,pacote,umid_solo,temp_ar,sinal_wifi_pct&order=recebido_em.desc&limit=10" -H "apikey: SUA_ANON_KEY" -H "Authorization: Bearer SUA_ANON_KEY"
```

A anon key é a mesma que está no `platformio.ini` do firmware.

> **Se isso voltar `[]` mas o serial mostrar `HTTP 201`, os dados chegaram.**
> A anon key do firmware tem permissão de INSERT e nada mais — é de propósito:
> a key viaja dentro do binário, e RLS bloqueando SELECT é o que impede que
> quem extrair a key leia os dados das fazendas. Em 25/ago foi liberada uma
> policy de leitura **só para o dispositivo de teste** (`TESTE_PROVISAO_01`),
> justamente pra este comando funcionar. Qualquer outro `dispositivo_id`
> continua devolvendo `[]` com a anon key, e isso é o comportamento correto.

## Comandos de bancada (monitor serial)

Só no firmware de teste, nos 5 s depois de conectar:

| comando | o que faz |
|---|---|
| `id=<uuid>` | grava o UUID da unidade (é o que vai no QR da etiqueta) |
| `wifi?` | lista as redes salvas e o UUID gravado |
| `reset` | apaga as redes salvas |

Na produção o `id=` é o passo de bancada antes de embalar: **uma unidade, um
UUID**, o mesmo impresso na etiqueta.

## Botão CONFIG — leia antes de contar com ele

O gatilho de campo para reabrir o portal é **ligar/desligar 3 vezes** (teste 6).
É ele que vale pro produtor: não exige hardware nenhum e funciona com a estação
dormindo.

O botão físico (GPIO33 → GND, segurado 5 s) existe no código e passou a ser
chamado no `e2`, mas **só responde enquanto a placa está acordada** — ou seja,
durante o ciclo de envio. A estação passa quase o dia inteiro em deep sleep, e
aí o botão não faz nada: o `setup()` termina dormindo e o `loop()` nunca roda.

Pra o botão funcionar com a estação dormindo seria preciso torná-lo fonte de
wake por EXT1 — mudança que mexe no mesmo subsistema do wake do pluviômetro
(EXT0) e que ninguém validou em hardware ainda. **Decisão pendente do Tório;
não implemente por conta própria.** Enquanto isso, o botão serve pra bancada.

Não usar GPIO0 pra isso: GPIO0 em nível baixo no boot joga o ESP32 em modo de
gravação e a placa trava.

## O que ainda não está resolvido (não é bug do teste)

1. **Vínculo com a conta do produtor.** O portal resolve a internet, não a
   identidade. A metade de bancada está resolvida: o UUID entra pela partição
   NVS gravada junto com o firmware, uma por unidade. Falta a outra metade — a
   página onde o produtor informa esse número e a estação passa a ser dele.
2. **A anon key está dentro do binário — e o binário está neste repositório
   público.** Não é mais "quem tiver a placa na mão": a chave está no GitHub,
   em texto, neste e em outros oito firmwares. A policy de INSERT de `leituras`
   aceita qualquer `dispositivo_id` (`with_check: true`), então quem achar a
   chave escreve leitura em nome de qualquer estação. Com 5 unidades nossas é
   teórico; com 500 na mão de terceiros é o vetor real. O caminho é ingestão
   por Edge Function com um token por dispositivo, e rotacionar a anon key
   depois — antes de vender, não depois.

## Arquivos

```
estacao_cliente/
├── platformio.ini              3 ambientes: produção, bancada e teste
├── VERSION                     e3 (OTA)
├── src/estacao_cliente.ino     firmware (base laranja_wifi l3)
├── src/provisao.h/.cpp         o portal — sem lib externa, só core ESP32
├── builds/ESTACAO_CLIENTE.bin  binário de produção (é o que o OTA baixa)
├── builds/AiAgro-estacao-e3-COMPLETO.bin   tudo em um, gravável em 0x0
└── preview-portal.html         as telas do portal, pra ver no navegador
```

---

## Status dos testes

**Rodada 1 — 24 e 25/ago/2026**, placa de bancada sem OLED e sem sensores,
rede "Cesio".

| # | Teste | Status |
|---|---|---|
| 1 | Primeira configuração | ✅ com ressalva (a telinha não abriu sozinha no Android — ver abaixo) |
| 2 | Senha errada | pendente |
| 3 | Rede que não alcança | pendente |
| 4 | Roteador sem internet | pendente |
| 5 | Sinal fraco | pendente |
| 6 | Liga/desliga 3× + contraprova | pendente |
| 7 | Sobrevive ao reboot | ✅ parcial — 4 ciclos seguidos de deep sleep reconectando sozinho pela rede salva, 40/40 pacotes |
| 8 | A rede sumiu | pendente |
| 9 | Duas redes | pendente |

Evidência no banco: 49 leituras do `TESTE_PROVISAO_01`, `versao_fw = e1`, entre
24/ago 13:01 e 25/ago 10:44 (BR). Os ciclos de teste saíram a cada ~14 min —
bate com 10 envios × 15 s + 10 min de sono. Num dos ciclos, com sinal oscilando
entre 15% e 70%, 9 dos 10 pacotes foram: o pacote que pegou o vale de sinal foi
pulado sem travar o ciclo, que é o comportamento desenhado.

**A telinha não abriu sozinha (Android Samsung).** Duas causas prováveis, as
duas fora do firmware: o *Intelligent Wi-Fi* da Samsung, que volta pro 4G
quando o WiFi não tem internet e engole o aviso de portal; e o *Private DNS*
(DNS-over-TLS), ligado por padrão em Android recente, que faz o celular
resolver nomes por fora do AP e nunca cair no nosso redirect. Vale repetir o
teste com Private DNS desligado, e num iPhone, pra separar as duas.

De qualquer forma, **auto-abertura é frágil por natureza e o produto não pode
depender dela** — por isso o `e2` passou a mostrar `192.168.4.1` na própria
tela do OLED. O mesmo vai na etiqueta e no manual.

## O que mudou no e2

Tudo veio da rodada 1 de testes.

1. **OLED mostra `192.168.4.1`** na tela do portal — o que fazer quando a
   telinha não abre sozinha, que foi exatamente o que aconteceu no teste.
2. **`checarBotao()` passou a ser chamado.** No `e1` a função existia e nunca
   rodava. Ela agora é chamada no `setup()` — não no `loop()`, que nunca
   executa porque o setup termina em deep sleep. Leia a seção do botão acima:
   o recurso continua limitado por design.
3. **Apagar as redes zera o contador de power-cycle.** O `reset` do console e o
   botão apagavam as redes e deixavam contagem residual, que podia reabrir o
   portal logo depois de configurar.
4. **`/salvar` valida tamanho**: SSID até 32 e senha até 63 bytes, os limites do
   próprio padrão WiFi. Antes qualquer coisa ia parar na NVS.

Não mudou nada em sensor, pH, Modbus ou formato do POST.

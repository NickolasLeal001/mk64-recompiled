# Port nativo do Mario Kart 64 (US) — plano de ponta a ponta

**ROM alvo (única aceita):** `carrinho.z64` — Mario Kart 64 US, código `NKTE`, 12 MB, big-endian (.z64)
SHA-1 `579c48e211ae952530ffc8738709f078d5dd215e` · MD5 `3a67d9986f54eb282924fca4cd5f6dff`

**Objetivo:** um executável nativo para Windows x64.
- Executa o código do jogo traduzido de MIPS para C.
- Lê gráficos, sons e pistas da ROM do usuário em tempo de execução.
- Não depende de emulador.

**Regra fixa:** o repositório nunca contém bytes da ROM nem assets extraídos. O programa exige a ROM do usuário e confere o SHA-1 acima antes de rodar.

---

## Estratégia

Há dois caminhos possíveis para "port nativo do zero":

| Caminho | Como | Tempo até rodar | Risco |
|---|---|---|---|
| A. Decompilação manual | Reescrever ~2.800 funções em C legível, uma a uma | Anos | Altíssimo |
| **B. Recompilação estática + runtime nativo** | Traduzir cada instrução MIPS para C automaticamente e reimplementar o hardware do N64 (vídeo, som, controle, SO) em código nativo | Meses | Médio |

**Escolha: B, depois A aos poucos.**
1. Primeiro o jogo inteiro roda via tradução automática.
2. Depois os sistemas já entendidos pela análise viram C legível, um por vez, sempre conferidos contra a versão traduzida. Exemplos: controle, física, drift, aceleração (ver `../mk64-analise/NOTAS.md`).

O REA entra como ferramenta de evidência em todas as fases:
- **Ghidra** (headless) fornece o pseudocódigo para leitura.
- **REA + Rizin** fornecem limites de função, referências cruzadas e chamadas. Cada conclusão sai com um ID de evidência (`ev_…`).

---

## O que já sabemos desta ROM

| Fato | Valor | Fonte |
|---|---|---|
| Entrada | `0x80000400` (zera BSS `0x800F6910`, tamanho `0xA0FC0`) | [obs] Rizin `ev_1e55e6d2…` |
| Segmento principal | ROM `0x1000–0xF7510` → `0x80000400` | [obs] |
| Segmento de corrida | ROM `0xF7510`, `0x2C130` bytes → `0x8028DF00` (memória `0x2C470`) | [obs] `FUN_80001158` |
| Terceiro segmento de código | ROM `0x123640`, `0x74A0` bytes → `0x80280000` | [obs] `FUN_80001158`; [?] finalidade (provavelmente pódio/encerramento) |
| Cópia ROM→RAM | `FUN_80001158(ram, rom, tamanho)` | [obs] |
| Microcódigos gráficos | F3DEX 0.95 e F3DLX 0.95 | [obs] strings na ROM |
| Compressão de assets | ~3.354 blocos MIO0 entre ROM `0x132B50` e `0x963EF0` | [obs] varredura de cabeçalhos; [?] falsos positivos |
| Funções | 2.662 (Rizin) / 2.799 (Ghidra) em `.main` + `.racing` | [obs] |
| Controle | 4 structs de 0x10 bytes em `0x800F6910`; leitura crua em `0x8014F0F0` | [obs] `NOTAS.md` |
| Jogadores | 8 × 0xDD8 bytes em `0x800F6990` | [obs] |
| Salvamento | [?] provavelmente EEPROM 4 Kbit, a confirmar na fase 1 | — |

---

## Fase 0 — Ambiente e projeto (1–2 dias) — ✅ concluída em 2026-10-28

> `build.bat` compila com MSVC 14.51 + CMake/Ninja do VS 2026, sem avisos em /W4, e baixa o SDL3 `release-3.4.16` na primeira vez.
> `test_rom` passa: vetores SHA-1 e ROM real aceita. Testes manuais: .v64 aceita, 1 byte alterado recusado, tamanho errado recusado.
> `mk64.exe` abre a janela com a ROM (`fase0-janela.png`).

**Ferramentas:**
- **Já instalado:** Visual Studio (pasta `18`), Git, Ghidra 12.1.4 + N64LoaderWV, JDK 21, Rizin 0.9.1 ligado ao REA.
- **A confirmar ou instalar:** CMake e Ninja (componente do VS), SDL3 (via vcpkg ou submódulo), Python 3 para as ferramentas.
- **Opcional:** REA dentro do WSL Ubuntu, para ter o provider Ghidra MIPS completo dentro do REA. No Windows ele só aceita PE/6502.

**Estrutura do repositório `mk64-port/`:**
```
tools/         recompilador, extrator de segmentos, verificador de ROM (Python)
analysis/      mapa da ROM (YAML), lista de funções, evidências REA exportadas
runtime/       "libultra nativa": threads, DMA, controle, VI, timers, save
gfx/           interpretador F3DEX/RDP → GPU
audio/         mixer HLE do microcódigo de áudio
generated/     C gerado pelo recompilador (fora do Git, regenerável)
src/           main, carregamento da ROM, janela, config
```

**Critério de saída:**
- `cmake --build` gera um `mk64.exe` vazio que abre uma janela.
- O executável recusa ROM com SHA-1 diferente.

---

## Fase 1 — Mapa completo da ROM (1–2 semanas) — ✅ concluída em 2026-10-28 (com resíduos registrados)

> **Fechamento:**
> - Save: **EEPROM 4 Kbit** (512 bytes, blocos de 8). Escrita `FUN_800d0050` (cmd PIF `0A 01 05`), leitura `FUN_800d0190` (cmd `02 08 04`).
> - Banco `0x7fa3c0`: as **63 imagens TKMK00** da ROM (segundo formato de compressão, decodificado por `FUN_800405d0`) + 2 MIO0. O port precisa de um decodificador TKMK00.
> - **Resíduos aceitos**, a resolver quando o código que os usa for recompilado (Fase 2/3):
>   - 17 blocos MIO0 do banco `0x641f70` sem referência achada;
>   - parte da área crua `0x6a0aba–0x712dc0`;
>   - o conteúdo cru dos 8 blocos por personagem além da paleta.

> **Feito (2026-10-28):**
> - `tools/build_rom_map.py` gera `analysis/rom_map.json` a partir da ROM: 81 regiões, 100% da ROM, sem sobreposição. O mapa é em JSON, sem dependências externas.
> - Código: 3 segmentos. `main`, `racing` e `seg3` (ROM `0x123640` → `0x80280000`).
> - Leituras da ROM: 36 chamadas diretas ao DMA `0x800cdc30` + o wrapper `0x80001158` (REA `ev_eebc979b…`, `ev_9a3a7a20…`).
> - Tabela de pistas `0x802B8D80`: 20 entradas × 0x30, cada uma com 3 faixas de ROM.
> - Áudio: tabelas tipo 1/2/3 (bancos/samples/sequências) e o mapa sequência→banco, com tamanhos tirados das próprias tabelas.
> - `tools/mio0.py` descomprime e valida: os 3.354 blocos MIO0 decodificam sem erro (zero falsos positivos).
>
> - Bancos: cada fim agora é limitado pelo início literal da região seguinte. Tabelas de índice achadas:
>
> | Banco | Índice | Cobertura |
> |---|---|---|
> | `0x145470` (karts) | `0x800DDEC0` (3.959 ptrs) + `0x800E1F20` (256) | 2.567/2.568 MIO0 |
> | | Paletas por personagem `0x800E2340` | uma em cada um dos 8 blocos crus (~0x24389 B) |
> | `0x641f70` | `0x800E54D0` (20, uma por pista) | 20/418 MIO0 |
> | `0x729a30` (menus/HUD) | descritores de 0x28 B em `common_data` (ROM `0x12aae4`) | 324/325 MIO0 |
>
> - Pistas: as 3 faixas de cada entrada identificadas pelo carregador `FUN_802aa918` e gravadas na tabela de segmentos `0x80150258` (`FUN_802a7b94`):
>
> | Faixa | Segmento | Conteúdo |
> |---|---|---|
> | `data` | 6 | MIO0 com os dados da pista |
> | `vtx_dl` | F | vértices MIO0 (expandidos para o seg 4) + display lists empacotadas (interpretadas para o seg 7) |
> | `textures` | 9 | lista de texturas (para o seg 5) |
>
>   Conferência: o bloco MIO0 de vértices termina exatamente no offset das display lists (ex.: `0x96f4`).
> - Banco `0x641f70`: 401/418 blocos referenciados.
>   - 280 texturas distintas nas listas das pistas, todas batendo com blocos MIO0 e tamanhos.
>   - 20 pela tabela `0x800E54D0`.
>   - O resto vem de 123 chamadas literais de `FUN_802a84f4` e de `FUN_8006ed94`.
>   - 7 constantes do código apontam para a área crua.
>
> **Falta:**
> - 17 blocos MIO0 do banco `0x641f70` sem referência achada, e o resto da área crua `0x6a0aba–0x712dc0`.
> - Banco `0x7fa3c0`: conteúdo não identificado. Só 2 blocos MIO0 nos seus 0x2E110 bytes.
> - Tipo de save (item 4).

Meta: cada byte da ROM classificado como código, dados, asset comprimido, áudio ou tabela.

1. **Achar os segmentos.**
   - Listar todos os chamadores de `FUN_80001158`, a cópia ROM→RAM, pelo REA (`axt`), e resolver os argumentos que hoje são variáveis.
   - Procurar também cópias por outras rotinas, como o equivalente a `osPiStartDma`.
2. **Tabelas de assets.**
   - Localizar as tabelas que apontam para os blocos MIO0: pistas, texturas, modelos dos karts.
   - Separar falsos positivos.
   - Escrever um descompressor MIO0 e validar contra os tamanhos dos cabeçalhos.
3. **Áudio.** Achar bancos de instrumentos, tabelas de samples e sequências. As strings do driver ("Audio:Memory", "WAVE CACHE", "LOAD Rom") indicam onde ficam as funções.
4. **Salvamento.** Confirmar o tipo (chamadas de EEPROM via SI) e o tamanho.
5. **Saída:** `analysis/rom_map.yaml`, com faixa, tipo, destino na RAM, compressão e evidência para cada região.

**Critério de saída:** 100% da ROM coberta pelo mapa, sem sobreposições.

---

## Fase 2 — Inventário de código (2–3 semanas) — ✅ concluída em 2026-10-28

> - `tools/recomp/mips.py`: decodificador R4300 próprio. Comparado instrução a instrução com o Rizin (via REA, `ev_b94e4b28…`) em **253.318 instruções dentro de funções: zero divergências**.
> - `tools/recomp/analyze.py` gera `analysis/functions.json`: **3.098 funções sem sobreposição**, cobrindo 100% do texto dos 3 segmentos.
>   - Origem: 2.660 por `jal`, 45 por ponteiro (`lui`/`addiu`), 6 sub-rotinas `bal`, 1 tail call, 3 entradas de segmento e 383 sem referência (código morto ou alcançado por tabelas não lidas).
>   - Contém todos os inícios do Ghidra (2.913) e do Rizin (2.741). Os 71 extras do Rizin são casos de `switch`.
> - **251 tabelas de salto** resolvidas, com tamanho tirado do `sltiu` do `switch`. Restam 2 `jr` indiretos, ambos explicados:
>   - `0x80040c8c`: retorno de sub-rotina `bal` do decodificador TKMK00 via registro salvo;
>   - `0x800d1790`: handler de exceção da libultra.
> - Fim do texto: main `0x800d8f70`, racing `0x802b8790`, seg3 `0x80284e40`. Logo após o texto do main há **código do RSP** (saltos para `0x84001xxx`), que não é recompilado.
> - **libultra:** bloco `0x800cbf70–0x800d8f70` (190 funções), definido pelo grafo de chamadas (nada nele chama código abaixo). 14 funções identificadas com evidência em `analysis/libultra.json` (osPiStartDma, osRecvMesg, osEeprom*, __osException…).
> - `tools/recomp/test_analysis.py`: testes de regressão.

1. **ELF com os 3 segmentos de código**, cada um no seu endereço. Hoje o `mk64_us.elf` tem 2; acrescentar `0x80280000`.
2. **Limites de função.**
   - Cruzar Rizin (`aflj`, via REA) com Ghidra (`functions.txt`).
   - Toda divergência vira item a resolver, registrado com `record_unknown` no REA.
3. **Saltos indiretos.**
   - Para cada `jr` que não seja `jr ra`, achar a tabela de saltos (switch) e os ponteiros de função (por exemplo, `DAT_8015f72c = FUN_8028df00`).
   - Gerar `analysis/indirect_targets.yaml`.
   - **Este é o ponto que mais quebra recompiladores.**
4. **Identificar a libultra.**
   - Assinar as funções do SO do N64: threads, filas de mensagem, PI/SI/VI, `osContGetReadData`, timers, matemática (`sqrtf`, `guMtx*`).
   - Usar o `example_signatures.txt` do N64LoaderWV como base e confirmar cada uma pelo comportamento.
   - Elas **não** serão recompiladas: serão substituídas pelo runtime nativo (fase 4).
5. **Saída:** `analysis/functions.yaml` com endereço, tamanho, segmento, "jogo" ou "libultra (substituir)" e alvos indiretos.

**Critério de saída:**
- Toda instrução de código pertence a exatamente uma função.
- Todo salto indireto tem a lista de alvos ou está marcado como dinâmico, com fallback.

---

## Fase 3 — Recompilador estático MIPS → C (4–8 semanas) — ✅ concluída em 2026-10-28

> - `tools/recomp/recomp.py` gera C a partir de `functions.json`: 3.098 funções em 21 arquivos (~23 MB).
>   - Registradores em `Ctx` (GPR de 64 bits, FPU FR=0).
>   - RDRAM em palavras nativas (byte XOR 3, half XOR 2).
>   - Delay slots, inclusive branches "likely" e alvos que caem dentro de um delay slot.
>   - `switch` pelas tabelas de salto, `jal` como chamada direta, `jalr` pela tabela endereço→função.
> - Semântica conferida contra o R4300:
>   - link (`ra`) escrito antes do delay slot;
>   - divisão por zero com o resultado do hardware;
>   - `cvt` honrando o modo de arredondamento do `fcr31`;
>   - NaN padrão do MIPS (`0x7FBFFFFF` / `0x7FF7FFFFFFFFFFFF`) em vez do NaN do x86.
> - `runtime/recomp.h` e `runtime/recomp_ops.c`: semântica compartilhada. `build.bat` regenera tudo com `MK64_ROM` definido. MSVC compila tudo sem avisos.
> - **Validação:** `tests/interp.c` é um interpretador MIPS independente. `tests/recomp_diff.c` roda cada função do jogo nos dois a partir de estados aleatórios e compara registradores + 8 MB de RDRAM.
>   - Resultado: **2.500/2.500 funções idênticas em 19.855 execuções**.
>   - 408 funções não são testáveis com estado aleatório (sempre saem da RDRAM, entram em laço ou dependem da libultra); ficam para os replays da fase 7.
>   - Três problemas reais achados e corrigidos pelo teste: ordem do link, NaN e (no próprio teste) código sobrescrito pelos dados aleatórios.
>   - Uso: `build\recomp_diff.exe <rom> [tentativas] [endereco] [h=hibrido o=so-essa]`.

Ferramenta própria em `tools/recomp/` (Python). Entrada: ROM, `rom_map.yaml` e `functions.yaml`. Saída: arquivos `.c`.

**Modelo:**
- Estado da CPU num `struct Ctx`: 32 registros de 64 bits, `hi`/`lo`, 32 registros FPU, `fcr31`.
- RAM do N64 como um vetor de 8 MB, endereçado com conversão `0x80xxxxxx` → índice e troca de endianness nos acessos.

**Tradução por instrução, cobrindo:**
- **Delay slots:** a instrução após um branch/jump executa antes do salto. Os branches "likely" anulam o slot.
- **Aritmética:** 32 bits com extensão de sinal para 64. Operações 64 bits (`dadd`, `dsll`…), já que o R4300 é 64 bits e a ABI é o32.
- **Memória:** `lwl`/`lwr`/`swl`/`swr` (acessos desalinhados) e `ll`/`sc`.
- **FPU:** simples e dupla precisão, modos de arredondamento, conversões `cvt.*`, comparações e `bc1t`/`bc1f`.
- **Chamadas:**
  - `jal` vira uma chamada C direta;
  - `jalr`/`jr` indireto passa por uma tabela endereço → função;
  - endereço desconhecido aborta com log.
- **Overlays:** prefixo por segmento (`main_`, `race_`, `seg3_`). Se dois segmentos dividirem o mesmo endereço, a tabela indireta consulta qual segmento está carregado.
- **Funções da libultra:** a chamada é redirecionada para a versão nativa (fase 4).

**Validação:**
- Teste unitário por classe de instrução, comparando com um interpretador MIPS de referência simples escrito na própria ferramenta.
- Teste diferencial: rodar funções puras (matemática, `FUN_800388b0` da zona morta, tabela de aceleração) no C gerado e no interpretador, com as mesmas entradas.

**Critério de saída:** todo o código compila com MSVC/clang sem avisos de função faltando, e os testes diferenciais passam.

---

## Fase 4 — Runtime nativo, a "libultra" do PC (4–6 semanas) — ✅ concluída em 2026-10-28

> - **API usada:** o jogo chama 61 funções da libultra. 46 são substituídas nativamente (`analysis/replaced.json`, cada uma com a evidência da identificação). As puras (gu*, sinf/cosf, sprintf, bcopy, aritmética 64 bits) e as que só preenchem estruturas na RAM (`osCreateMesgQueue`, `osSetEventMesg`) continuam recompiladas.
> - **`runtime/ultra.c`:**
>   - cada thread do N64 é uma fibra do Windows; o escalonador por prioridade (FIFO entre iguais) nunca retoma a thread ociosa (prioridade 0);
>   - filas de mensagem lidas e escritas na própria estrutura da RAM;
>   - eventos via `__osEventStateTab` (`0x80196440`), VI a 59,94 Hz;
>   - DMA da ROM, tarefas do RSP (por enquanto concluídas na hora, com SP e DP done);
>   - relógio a 46,875 MHz, EEPROM em `mk64.eep`;
>   - controles por teclado/SDL gamepad (`src/input.c`); Controller Pak relatado como ausente.
> - **Correção achada no boot:** uma palavra de dados após o texto (`0x800e5530`) era lida como `jal 0x80002000` e partia uma função ao meio. Os `jal` agora só contam se estiverem dentro do texto.
> - **Resultado:** o jogo cria suas 5 threads (ociosa, jogo, escalonador, áudio, controles), configura o áudio para **26.800 Hz** e entra no laço principal.
>   - Em ~12 s: 699 retraces (60 Hz), **323 tarefas gráficas (~30/s)**, 683 tarefas de áudio, 1.289 DMAs (1,1 MB), framebuffer trocando.
>   - Nenhum acesso de IO sem tratamento. Log em `mk64_runtime.log`; tela de status em `fase4-status.png`.
> - Teste diferencial depois das mudanças: 2.469/2.469 funções idênticas.

Substitui o sistema operacional e o hardware do N64:

| N64 | Implementação nativa |
|---|---|
| Threads + filas de mensagem (`osCreateThread`, `osSendMesg`, `osRecvMesg`) | Agendador cooperativo com prioridades, fibras do Windows ou corrotinas |
| PI DMA (ROM → RAM) | `memcpy` a partir do arquivo da ROM, mais a mensagem de "DMA concluído" |
| SI / controles (`osContGetReadData`) | SDL3 gamepad → preencher `OSContPad` em `0x8014F0F0` (botões u16, stick ±80) |
| VI (retraço vertical, 60 Hz) | Evento de quadro no relógio do PC; troca de framebuffer |
| Timers / `osGetTime` | Relógio de alta resolução convertido para ticks do N64 |
| EEPROM | Arquivo `save.eep` |
| Tarefas do RSP (`osSpTaskStart`) | Interceptar: tarefa gráfica → fase 5; tarefa de áudio → fase 6 |

**Critério de saída:** o jogo passa da inicialização e entra no loop principal, com logs mostrando as tarefas gráficas sendo enviadas.

---

## Fase 5 — Gráficos: F3DEX/F3DLX 0.95 + RDP (6–10 semanas) — ✅ concluída em 2026-10-28

> **Decisão:** renderizador escrito do zero, em software, na resolução nativa (320×240), desenhando no framebuffer RGBA16 da própria RDRAM. Dá controle exato da semântica do RDP e permite conferir cada quadro em imagem. A GPU fica para a fase 10.
> - `gfx/rsp.c`:
>   - display lists GBI1 (F3DEX detectado pelo `G_VTX`), segmentos, pilha de matrizes;
>   - luz direcional e ambiente, environment mapping, neblina;
>   - recorte no near plane, culling;
>   - `SETTIMG/CIMG/ZIMG` com segmento resolvido.
> - `gfx/rdp.c`:
>   - TMEM com `LOADBLOCK`/`LOADTILE`/`LOADTLUT`;
>   - formatos RGBA16/32, CI4/8, IA4/8/16, I4/8; clamp/mirror/mask;
>   - combiner 2 ciclos, blender, Z, texrect, fill.
> - **Bugs achados olhando os quadros:**
>   - textura de pista como ruído: segmento não resolvido no `SETTIMG`;
>   - logo do título embaralhado: passo de linha errado em RGBA32.
> - **Resultado:** logo da Nintendo, tela de título idêntica à original, menus completos (GAME/PLAYER/MAP SELECT), apresentação da pista e corrida no Luigi Raceway com HUD. **30 quadros/s na corrida**, a taxa do jogo.
> - **Não feito:** comparação lado a lado com um emulador de referência (nenhum instalado). As capturas foram conferidas visualmente.
> - **Ferramentas:**
>   - `MK64_INPUT="de-ate:BOTOES;..."` (entrada roteirizada);
>   - `MK64_SHOTS=n[:de[:ate]]` (capturas BMP);
>   - `MK64_TRACE=n` (display list em `dl_trace.txt`).

1. Interpretar as display lists F3DEX:
   - matrizes, vértices, triângulos, geometria (luz, névoa, culling);
   - `G_SETTIMG`, `G_LOADBLOCK` e `G_LOADTILE` (texturas);
   - combiner, othermode, `G_TEXRECT` (HUD e sprites).
2. Converter texturas: formatos RGBA16/32, CI4/8 com paleta, IA, I.
3. Traduzir o color combiner e o blender do RDP para shaders (OpenGL 3.3 ou D3D11 via SDL3).
4. Tratar os acessos do jogo ao framebuffer, se houver: efeitos, captura.

**Decisão:** escrever o renderizador do zero, como o objetivo pede, ou acelerar integrando um renderizador de licença permissiva e trocá-lo depois. Registrar a escolha aqui.

**Critério de saída:**
- O logo e a tela de título aparecem corretos.
- O Luigi Raceway renderiza, conferido por captura contra um emulador de referência.

---

## Fase 6 — Áudio (3–5 semanas) — ✅ concluída em 2026-10-28

> - **ABI real:** o jogo **não** usa a ABI 1 da libultra. As listas têm opcodes até `0x16`; a codificação bate com a ABI de áudio mais nova (`ENVSETUP1/2`, `ENVMIXER` em `0x13`, `LOADBUFF/SAVEBUFF` em `0x14/0x15` com tamanho `>> 4`) e endereços de DMEM absolutos. Identificado capturando uma lista real (`MK64_DUMP_AL`).
> - `audio/abi.c`: ADPCM (VADPCM 4 bits), RESAMPLE, ENVMIXER (barramentos seco/reverb), MIXER, INTERLEAVE, INTERL, DMEMMOVE, LOADADPCM, SETLOOP, LOAD/SAVEBUFF. Opcodes desconhecidos são logados; nenhum apareceu.
>   - A tabela do filtro de reamostragem é lida da ROM do usuário (`0xF5A00`, conferida pela soma das linhas ≈ 32768), nunca embutida no código.
> - **Saída:** `osAiSetNextBuffer` → SDL3 a 26.807 Hz; `MK64_WAV=arquivo` grava tudo.
> - **Validação objetiva** (sem ouvir):
>   - planura espectral 0,04–0,16 contra 0,55 do ruído branco;
>   - na música do título, picos em 209/393/589/772/956 Hz, série harmônica de ~196 Hz;
>   - clipping ≤ 0,05%;
>   - fila do dispositivo estável em ~13–16 ms (sem atraso crescente).
> - **Não feito:** escuta humana e comparação com emulador. Ficam para a validação da fase 8.

- Interpretar as listas de comandos do microcódigo de áudio (ABI): `ADPCM`, `RESAMPLE`, `ENVMIXER`, `LOADBUFF`, `SAVEBUFF`, `INTERLEAVE`…
- Gerar PCM e entregar ao SDL3 na taxa que o jogo pede ("SynoutRate" aparece nas strings).

**Critério de saída:** música e efeitos sem estalos e no tempo certo durante uma corrida completa.

---

## Fase 7 — Primeiro "jogo de verdade" (2–4 semanas)

Marcos, nesta ordem:
1. Logo da Nintendo.
2. Tela de título.
3. Menus.
4. 1 jogador, Grand Prix 50cc, Luigi Raceway até a linha de chegada.
5. Todas as pistas.
6. Time Trials com fantasma.
7. VS 2–4 jogadores (tela dividida).
8. Battle.
9. Pódio (terceiro segmento).
10. Salvar e carregar recordes.

Cada marco vira um teste de replay: uma sequência de entradas gravada é rodada e confere checksum da RAM e capturas de tela em pontos fixos.

---

## Fase 8 — Validação comportamental com o REA (contínua) — 🔶 em andamento

> `tests/behavior_test.c` chama as funções **recompiladas** com a RAM carregada da ROM e confere 6 achados do `NOTAS.md`: zona morta ±12 nos dois eixos, limite ±53, 40 → 28, C-baixo liga Z, e a tabela de aceleração por classe de peso. **Todos passam.**

Os achados da análise viram testes automáticos contra o port:

| Achado (NOTAS.md) | Teste |
|---|---|
| Zona morta ±12 e limite ±53 (`FUN_80038534` / `FUN_800388b0`) | Varrer o stick de −80 a +80 e comparar a saída |
| C-baixo liga o bit Z (`FUN_80000934`) | Apertar C-baixo e conferir o bit 0x2000 no struct do controle |
| Drift preso a −53..−40 / +40..+53 | Replay de drift e leitura de +0x7C do jogador |
| Tabela de aceleração por personagem (`PTR_DAT_800e3010`) | Cronometrar 0→máxima por personagem e comparar com o emulador |
| Física simplificada fora da tela (`FUN_8002f35c`) | Garantir o mesmo resultado de corrida em 1P e 4P |

Cada teste aponta para o ID de evidência REA que o justifica. Quando o port diverge do emulador, a evidência mostra se o erro está na tradução ou no runtime.

---

## Fase 9 — Substituir tradução por C legível (opcional, contínuo) — 🔶 iniciada

> - **Mecanismo:** `analysis/rewritten.json` lista as funções reescritas à mão em `native/`. O gerador não emite corpo para elas, e `tests/recomp_diff.c` compara cada uma com o interpretador MIPS só no estado visível pela ABI o32 (v0, s0–s7/fp, sp, ra, f0/f2, f20–f31 e toda a RAM).
> - **Primeiras reescritas:** leitura do analógico (`native/controls.c`: zona morta quadrada de ±12 subtraída na direção do stick e limite de ±53), substituindo `FUN_80038534` e `FUN_800388b0`.
>   - Validação: 1.352 execuções idênticas cada contra o interpretador, testes de comportamento e replays inalterados.

Para cada sistema já entendido (controle → esterço → aceleração → drift → itens → IA):
1. Escrever a função em C legível, com nomes reais e structs (`Player`, `Controller`).
2. Ligar uma chave de compilação que alterna entre a versão traduzida e a nova.
3. Rodar todos os replays: a RAM e as capturas precisam bater bit a bit.
4. Só então remover a versão traduzida daquela função.

---

## Fase 10 — Melhorias nativas (depois que tudo bate) — 🔶 em andamento

> - **Feito:**
>   - `mk64.ini` ao lado do executável (criado com o padrão): escala da janela, tela cheia (F11 alterna), filtro nítido/suave, volume e mapa completo do teclado (nomes de tecla do SDL).
>   - Renderizador 2,3× mais rápido sem mudar nenhum pixel (pior caso 13,9 ms por quadro).
>   - Rasterização em faixas paralelas: cada primitiva é gravada com um retrato do estado do RDP (copiado só quando muda) e as faixas de linhas são desenhadas em threads. Pior caso 2,8 ms por quadro (Wario Stadium), os 20 replays continuam idênticos e a suíte caiu de 445 s para 78 s. `MK64_RENDER_THREADS` escolhe o número de threads e `MK64_RENDER_IMMEDIATE` volta ao desenho imediato.
> - **Falta:** resolução interna maior e widescreen, que exigem desenhar num buffer próprio em vez do framebuffer de 320×240 na RAM. Também falta 60 fps com interpolação.

- Resolução livre e widescreen: corrigir o FOV, que hoje é ~58°/~68°/~45° conforme o número de telas.
- 60 fps com interpolação de matrizes.
- Remapeamento de controle e suporte a teclado.
- Opções de áudio.

---

## Riscos principais

| Risco | Mitigação |
|---|---|
| Saltos indiretos não resolvidos | Tabela + fallback com log; fase 2 só termina com todos listados |
| Diferenças de ponto flutuante MIPS × x86 | Forçar precisão simples (`/fp:strict`), testes diferenciais |
| Temporização (o jogo depende do VI e do ritmo das tarefas do RSP) | Agendador determinístico com o relógio do PC só no VI |
| Precisão do RDP (combiner e blender) | Comparar capturas com emulador de referência por pista |
| REA sem provider MIPS completo no Windows | Rizin via REA para estrutura; Ghidra headless para pseudocódigo; REA no WSL se precisar |
| Escopo enorme | Marcos pequenos com replay; nunca avançar de fase sem o critério de saída |

## Estimativa total

**Primeira corrida jogável:** cerca de 6 a 9 meses de trabalho, uma pessoa em meio período, com o recompilador e o runtime escritos do zero. A fase 9 é aberta.

## Próximo passo concreto

Fase 0: criar o esqueleto CMake e o verificador de SHA-1. Fase 1, item 1: listar pelo REA todos os chamadores de `FUN_80001158` e montar o `rom_map.yaml` inicial com os 3 segmentos de código já conhecidos.

## Fase 7 — andamento (2026-10-28)

> - **Modo de teste:** `MK64_HEADLESS=1` (tempo virtual, sem janela/áudio, EEPROM em branco na pasta da execução) + `MK64_EXIT_AT`. `MK64_INPUT2` é um 2º controle roteirizado.
> - `tools/replay.py`: roda e compara hashes de quadros. `--record` grava a referência; os BMPs ficam em `build/replays/<nome>/`. `tools/explore.ps1` monta folhas de contato para explorar menus.
> - **20 replays em `tests/replays/`, todos passando duas vezes seguidas (~7,5 min):**
>   - título → menus → largada no Luigi Raceway;
>   - Time Trials com aviso de Controller Pak e recordes da EEPROM;
>   - largada em **cada uma das 16 pistas**;
>   - **VS 2 jogadores** em tela dividida;
>   - **Battle 2 jogadores** na Big Donut.
> - **EEPROM:** no primeiro boot o jogo grava os recordes padrão (512 bytes com checksum) em `mk64.eep`.
> - **Correção:** neblina calculada depois do recorte no near plane (vértices atrás da câmera davam z/w sem sentido).
> - **Piloto automático para testes:** `MK64_AUTOPILOT=n` dá ao jogador 1 as flags de kart da IA (`0x9010` em vez de `0xc010`, achadas num dump da RAM com `MK64_DUMPRAM`). A IA do próprio jogo pilota o Mario: ele completou as 3 voltas do Luigi Raceway (7º lugar). Ao terminar, o jogador volta a ser humano (bit `0x0800` = corrida terminada).
> - **Fim de corrida resolvido:** o jogo só avança a tela de resultado quando um jogador **humano** cruza a linha. O piloto automático agora devolve o controle quando o progresso do jogador 1 chega a 2,97 voltas, com A segurado.
>   - Endereços achados comparando dumps: contador de voltas `0x80164390` (s32 × 8, o mesmo que o código de debug da pausa altera) e progresso acumulado `0x801644d0` (float × 8, voltas + fração).
>   - Replay `gp_corrida_completa`: 3 voltas, ranking, RETRY e nova largada; 82 quadros idênticos em duas execuções.
> - **Aberto:**
>   - Pódio: exige terminar entre os 4 primeiros, e a IA pilotando o Mario terminou em 7º em todas as tentativas.
>   - Choco Mountain aparece com muita neblina branca. A fórmula segue a referência; falta comparar com o original (fase 8).
>   - Pódio e gravação de um recorde novo: exigem completar corridas, ou seja, pilotar 3 voltas por roteiro ou com piloto automático.
>   - VS/Battle com 3–4 jogadores: roteiros dos controles 3 e 4.

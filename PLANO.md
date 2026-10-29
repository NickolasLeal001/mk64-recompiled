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
>   - controles por teclado/SDL gamepad (`src/input.c`); Controller Pak relatado como ausente (emulado desde 2026-10-29, ver fase 10).
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
> - **Correção (2026-10-29, relatada jogando: "o chão não está sendo renderizado"):** no Toad's Turnpike, no Choco Mountain e no Moo Moo Farm, a pista sumia.
>   - Causa: o modo neblina + superfície opaca (`G_RM_FOG_SHADE_A` + `G_RM_AA_ZB_OPA_SURF2`) liga só o `ALPHA_CVG_SEL`. O RDP descartava pixels com alfa abaixo de 0,5 como se fosse `CVG_X_ALPHA`, e com neblina o alfa do sombreamento é o fator de neblina, perto de 0 junto à câmera. Um quadro do Toad's Turnpike perdia 113 mil pixels assim.
>   - Agora só o `CVG_X_ALPHA` (bordas recortadas de árvores e placas) descarta por alfa. O reflexo do kart no gelo do Sherbet Land também passou a aparecer.
> - **Correção (2026-10-29, relatada jogando: "cortes" nos menus e na seleção de personagens):** retratos e caixas de menu desenhados em faixas (por exemplo, 64×16, com tile em wrap) mostravam linhas finas nas emendas, em 1× e em 2×.
>   - Causa: o filtro bilinear amostrava meio texel antes (`s − 0,5`), como um filtro centrado. Na primeira linha de cada faixa, isso misturava a linha −1, que pela repetição é a última da própria faixa. O RDP não faz esse deslocamento: em s = 16,0 ele lê só o texel 16.
>   - Agora a amostragem segue o hardware. Na passada ampliada, os retângulos de textura amostram a partir do canto do pixel, como em 1×, e ficam presos aos pixels que cobrem em 1×. Os 26 replays foram regravados.
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
> - **Escuta humana:** aprovada em 2026-10-29, depois das duas correções abaixo. A comparação com emulador fica para a fase 8.
> - **Correção (2026-10-29, depois da primeira escuta: "só ruído"):** no microcódigo do MK64, o INTERLEAVE (`0d000000 llllrrrr`) usa o destino e o tamanho do SETBUFF anterior. Antes, os campos zerados da 1ª palavra eram lidos como destino e tamanho, nada era intercalado e o SAVEBUFF mandava lixo da DMEM para a saída. O ENVMIXER agora alinha a contagem a 8 amostras, e o reverb vai para o mesmo lado, ou para o lado oposto só com o bit de troca. A variação entre amostras vizinhas caiu de 0,57 para 0,25–0,35 da amplitude. `MK64_WAV` passou a gravar também no modo sem janela.
> - **Correção 2 (2026-10-29, "ainda tem problema"):**
>   - O HLE foi alinhado à variante do MK64 no HLE do Mupen64Plus (`nead_mk`): o ENVSETUP1 não tem rampa de reverb (antes o volume do reverb dava voltas a cada 8 amostras), o ENVMIXER inverte o sinal pelos bits 1/0 e não troca os canais do reverb, e LOADBUFF/SAVEBUFF alinham os endereços como DMA (RAM a 8 bytes, DMEM a 4) com contagem de 12 bits. Também entraram o ADPCM de 2 bits e as contagens de CLEARBUFF, DMEMMOVE e INTERL.
>   - Saída em tempo real: o `osAiGetLength` respondia com tudo o que estava na fila do SDL, então o jogo gerava pouco som e a fila vivia vazia (0–3 ms), o que soa como estalos. Agora ele vem de uma fila de DMA do AI emulada (2 buffers, relógio real), como no console. O som só começa com 60 ms de reserva; a velocidade é ajustada em até 0,5% e, com a reserva baixa, o jogo é levado a gerar buffers maiores. Medido em janela: 10 s de som a cada 10 s, reserva de 24–47 ms, nenhuma vez vazia, também em 2× com interpolação.
>   - Desenhar as tarefas gráficas numa thread própria foi testado e descartado: o jogo passa à frente do desenho e, nas trocas de cena, carrega dados por cima do que o quadro ainda lê.

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
>
> - **Drift (2026-10-29):** replay `drift_luigi` com verificações de RAM no jogador 1 (`ram_checks` no `tools/replay.py`, sobre dumps em retraces escolhidos).
>   - R + direção em velocidade liga a flag 0x10 em +0xBC.
>   - Com o direcional cheio, o esterço em +0x7C fica entre +40 e +53.
>   - O contra-esterço liga a carga do mini-turbo (0x20000000) só além de 39: a −37 continua desligada, a −41 já ligou.
>   - Correção do `NOTAS.md`: +0x7C segue o direcional cru e vai até −49 no contra-esterço. O remapeamento para 40..53 não fica nesse campo.
> - **Física simplificada fora da tela:** medida com o perfil de chamadas (`-DMK64_PROFILE=ON`).
>   - Grand Prix de 1 jogador, 300 quadros: `FUN_80028864` roda 16 vezes por quadro (8 karts × 2). Desse total, 2.667 chamadas vão para a física simplificada `FUN_8002f35c` (55%), 1.813 para a normal `FUN_8002d268` e 320 para a "com efeito" `FUN_8002e594`.
>   - Time Trials e VS de 4 jogadores: sem karts da CPU, a simplificada não roda nenhuma vez, e a atualização usa outra rotina.
> - **Falta:** comparar com um emulador de referência, o que depende de baixar um emulador. Isso inclui as capturas da fase 5, a neblina do Choco Mountain e o tempo de 0 à velocidade máxima por personagem.

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

## Fase 9 — Substituir tradução por C legível (opcional, contínuo) — ⏸️ pausada em 2026-10-29 (5 funções reescritas)

> - **Mecanismo:** `analysis/rewritten.json` lista as funções reescritas à mão em `native/`. O gerador não emite corpo para elas, e `tests/recomp_diff.c` compara cada uma com o interpretador MIPS só no estado visível pela ABI o32 (v0, s0–s7/fp, sp, ra, f0/f2, f20–f31 e toda a RAM).
> - **Primeiras reescritas:** leitura do analógico (`native/controls.c`: zona morta quadrada de ±12 subtraída na direção do stick e limite de ±53), substituindo `FUN_80038534` e `FUN_800388b0`.
>   - Validação: 1.352 execuções idênticas cada contra o interpretador, testes de comportamento e replays inalterados.
> - **Rodada 2 (2026-10-29), `native/kart.c`:**
>   - **Aceleração** (`FUN_80030fc8`): a velocidade-alvo (+0x9C) sobe em 10 faixas de um décimo da velocidade máxima. Cada faixa soma o valor da tabela do personagem mais 0,05 por grau de inclinação; com impulso, multiplica pelo fator do personagem (2, 2, 3, 3, 1,5, 1,5, 3, 3). As faixas são conferidas em ordem, então um passo que cruza para a faixa seguinte soma de novo no mesmo quadro. Depois o alvo fica entre 0 e a máxima, e +0x8C recebe alvo²/25.
>   - **Passos do esterço** (`FUN_80033850`, `FUN_80033884`, `FUN_80033a40`): o esterço anda um passo por chamada em direção ao alvo. O passo (+0x200) encolhe 2048 por chamada até um mínimo, nada se move dentro de ±limiar, e o arrasto (+0x90) fica em zero ou abaixo.
>   - **Validação:** iguais ao interpretador em 1.189 a 3.359 execuções cada, e os 26 replays continuam idênticos. Esses replays rodam milhares de quadros de corrida, então qualquer diferença de arredondamento apareceria.
>   - **Ferramentas:** o `rewritten.json` ganhou `"returns"`; funções `void` não comparam v0 nem f0–f3, que são lixo pela ABI. As reescritas reproduzem o quadro de pilha e o endereço de retorno das chamadas, que ficam na RAM comparada, e convertem NaN no NaN padrão do R4300 (`fnan_s`).
>   - **Falta:** a função principal do esterço, `FUN_80033ae0`. Ela tem ~2.900 instruções e um quadro de pilha de 0x2A4 bytes com tabelas copiadas, e cobre drift, contra-esterço, ré e mini-turbo. Depois dela vêm o drift, os itens e a IA.

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
>   - Resolução interna de 1× a 4× (`resolucao_interna` no `mk64.ini`, padrão 2; `MK64_RES_SCALE` sobrepõe). Cada lote de primitivas é desenhado duas vezes: em 1× na RDRAM, que é o que o jogo e os replays veem, e ampliado numa imagem própria por endereço de framebuffer, usada só na tela. Uma cópia da RDRAM após cada lote detecta pixels escritos pela CPU, que entram ampliados. Os 20 replays dão os mesmos hashes com 1× e com 2×; em 2× o pior caso é ~12 ms por quadro.
>   - Tela larga 16:9 (`tela_larga = 1` no `mk64.ini`; `MK64_WIDESCREEN` sobrepõe), também só na imagem de exibição. Framebuffers de 320 pixels ganham uma margem de 53 pixels (em 1×) de cada lado. A cena 3D é deslocada pela margem e revela mais pista, porque o RSP só recorta no plano próximo e não descarta nada por G_CULLDL. Scissor e retângulos de largura total vão até as novas bordas. Triângulos ortográficos de largura total (o gradiente do céu) são esticados. Imagens 2D de largura total (título, menus) ganham barras pretas, e o HUD fica no centro. Tela dividida em 2 jogadores também se alarga; quadrantes de 3–4 jogadores ficam centralizados. Os replays não mudam; o pior caso em 16:9 com 2× é 17 ms por quadro.
>   - 60 fps com interpolação de matrizes (`interpolacao = 1` no `mk64.ini`, padrão ligado; `MK64_INTERP` sobrepõe). Depois de cada lista de exibição, ela roda de novo só para a tela, do mesmo estado inicial, com cada matriz no meio do caminho entre a do quadro anterior e a atual. As matrizes são casadas pelo que desenham em seguida (a próxima lista ou o próximo bloco de vértices) e pelas flags do G_MTX: em ordem, quando a quantidade é a mesma nos dois quadros; senão, só com uma vizinha claramente mais próxima, o que evita casar partículas de karts diferentes. Saltos de mais de 150 unidades ou rotações grandes (corte de câmera, renascimento) ficam sem interpolar. Um quadro novo que substitui outro exibido por 2 ou mais retraces aparece primeiro como imagem intermediária e, no retrace seguinte, como ele mesmo, com 1 retrace a mais de atraso. O HUD e objetos com vértices montados pela CPU ficam nos 30 fps do jogo. Os replays não mudam. Em 2× (com ou sem tela larga), a média por quadro em corrida fica em ~18–23 ms, dentro dos 33 ms de um quadro a 30 fps. Quando o quadro real somado ao último intermediário passa de 25 ms (a intro em 2×), aquele quadro sai sem intermediário: enquanto o quadro é desenhado, as threads do jogo esperam, e a de som ficava sem tempo.
>   - **Controller Pak (2026-10-29):** 32 KB no controle 1, gravados em `mk64.mpk` ao lado do executável.
>     - O arquivo é criado já formatado como um pak novo: rótulo, bloco de ID com checksums e suas 3 cópias, tabela de inodes com 123 páginas livres.
>     - O código Pfs do próprio jogo roda. O runtime substitui só `__osSiRawStartDma` (`0x800D3AA0`), que interpreta a RAM do PIF (status com o bit de pak, botões, leitura e escrita do pak com o CRC de dados 0x85) e termina com o evento do SI, e `__osSiDeviceBusy`.
>     - O `osContInit` substituído passou a preencher `__osMaxControllers` (`0x80196541`). Sem isso, o Pfs montava comandos para nenhum controle.
>     - No Time Trials sumiu o aviso "NO N64 CONTROLLER PAK". "SAVE GHOST" (confirmado com START) cria a nota do MK64, grava o fantasma no arquivo 1 e depois avisa "THIS GHOST IS ALREADY SAVED". Numa sessão nova, a seleção de pista mostra a etiqueta GHOST lida do pak.
>     - O jogo recusa salvar fantasmas pilotados pela IA (`0x80162dd4` = 1). O replay `tt_fantasma_pak` zera esse aviso com `MK64_POKE` e confere o SHA-1 do pak (`check_pak`).
>   - **Proporção livre (2026-10-29):** `proporcao = a:b` no `mk64.ini` (4:3, 16:9, 16:10, 21:9, 32:9 ou outra, de 4:3 até 4:1; `MK64_ASPECT` sobrepõe). `tela_larga = 1` continua valendo 16:9. A margem de cada lado é `(240 × proporção − 320) / 2` pixels em 1×; em 16:9 ela dá os mesmos 53 de antes. O buffer de exibição passou a ser alocado pela largura lógica, porque em 32:9 a imagem tem 853 pixels em 1×. Conferido em 21:9 e 32:9: corrida, tela dividida de 2 jogadores e menus com barras pretas.
>   - **Desenho na GPU (Vulkan via SDL_gpu; plano em `PLANO_GPU.md`):**
>     - **M0 — base (2026-10-29):** `renderizador = gpu | cpu` no `mk64.ini` (padrão `gpu`; `MK64_RENDERER` sobrepõe; sem janela o padrão é `cpu`). O device é criado com `SDL_CreateGPUDevice(SPIRV, debug, "vulkan")` (`MK64_GPU_DEBUG` liga as validation layers) e reivindica a janela. A imagem passa a ser apresentada por blit para a swapchain, com letterbox e filtro nítido/suave; o status do F1 vai para a barra de título. Os shaders GLSL (`gfx/shaders/`) viram SPIR-V no build pelo `glslc` do Vulkan SDK (1.4.363) e entram no executável; sem o `glslc`, o build sai só com o software. Alvo `vulkan1.0`, porque é a versão da instância que o SDL cria. Se o device ou a janela falham, o jogo registra o motivo no log e volta para o `SDL_Renderer`. Nesta etapa a imagem ampliada ainda vem do software (enviada como textura); `MK64_GPU_TEST` mostra um render target limpo com cor fixa. Conferido numa RTX 4070 SUPER, sem avisos da validação; 26 replays idênticos.
>     - **M1 — desenho na GPU (2026-10-29):** a passada ampliada (`draw_pass(g_scale, 1|2)`) vira `gpu_draw_ops` quando a GPU está ativa; a passada 1× continua igual. Cada lote vira vértices em pixels do alvo (`GpuVertex`: posição, z, 1/w, sombreamento, s/t, índice do retrato, tipo e parâmetros), e os retratos (96 palavras: othermode, combinador, cores, scissor já em pixels do alvo, 8 tiles) e as TMEM (4 KB, sem repetir, por hash) vão em storage buffers. O ubershader `gfx/shaders/rdp.frag` refaz em GLSL o que o software faz por pixel: busca de texel em todos os formatos e TLUTs, wrap/mirror/clamp/máscara, filtro sem meio texel, combinador de 1 e 2 ciclos, alpha compare, coverage × alfa, Z_SRC_PRIM e decal. O blender é tratado como linear na cor da memória: o shader devolve a parte sem memória em RGB e o peso da memória em alfa, e o blend fixo `src + dst × src_alpha` completa a conta. Isso cobre neblina, transparência e os modos de 2 ciclos sem ler o alvo; o alfa da memória (G_BL_A_MEM) é aproximado por 1 e registrado no log. Z: `D32_FLOAT` por tamanho de alvo, teste `LESS_OR_EQUAL`, quatro pipelines (teste × escrita) e um só de profundidade para limpar o Z quando o jogo preenche a imagem de Z. Fill com a paridade de 16 bits, retângulos de textura com a amostragem presa aos pixels 1×, modo cópia sem filtro. Escritas da CPU: o `hi_sync` acha os pixels mudados e manda o retângulo que os contém, com uma marca por pixel, desenhado ampliado antes do lote. A imagem intermediária começa como cópia do alvo (`SDL_CopyGPUTextureToTexture`). O `main.c` recebe a textura (`rdp_gpu_frame`) e a apresenta sem passar pela CPU; as capturas (`MK64_SHOTS`) leem a textura de volta.
>       - `tools/gpu_compare.py` roda cada replay com `MK64_RENDERER=cpu` e `=gpu` na mesma escala e compara os `frame_*_hi.bmp`. Limiar: média ≤ 2/255 e no máximo 1% dos pixels acima de 8/255. Ele gera `build/gpu_compare/mosaico.png` (software | GPU | diferença × 4) e as imagens de diferença, e também confere que os quadros 1× têm o mesmo hash. Python puro (lê BMP, escreve PNG), sem dependências.
>       - A imagem do software é RGBA16 (5 bits por canal) e a da GPU tem 8 bits. Só isso já dava média de ~1–2/255. Por isso o GPU é posto na mesma grade antes de comparar ("acima" passa a ser mais de um degrau RGBA16, 9/255), e a média sem a grade continua no relatório.
>       - Profundidade guardada como z × 0,5 (limpa em 0,5): o software compara floats crus contra o Z limpo em 1,0, e z acima de 1 precisa continuar ordenado.
>       - Resultado em 2× (26 replays): média de 0,16 a 0,78/255 na grade (0,9 a 1,65 sem ela). Todos passam, exceto 5 quadros do `vs4p_luigi`, registrados como diferença conhecida em `KNOWN`. Neles, a parede colada na câmera do jogador 1 deixa, no software (1× e ampliado), a areia de trás vazar em pontos soltos, por ruído das baricêntricas em float num triângulo enorme; a GPU desenha a parede inteira. Os quadros 1× têm o mesmo hash nos dois modos, e os 26 replays continuam idênticos.
>     - **M2 — triângulos e Z (2026-10-29):** o ubershader entrou inteiro na M1, então as etapas M2–M4 foram de conferência. Triângulos com sombreamento e o Z da GPU (teste `LESS_OR_EQUAL`, escrita por Z_UPD, limpeza pelo fill da imagem de Z) dão a mesma geometria do software em todas as pistas dos replays, inclusive na tela dividida de 2 e 4 jogadores. A única diferença é a do `vs4p_luigi` descrita acima, em que o software erra.
>     - **M3 — TMEM e texturas (2026-10-29):** TMEM por retrato num storage buffer, sem repetir conteúdo (hash + comparação). Conferidos: logo da Nintendo, título, GAME SELECT, MAP SELECT (retratos das pistas e caixas sem cortes, pela amostragem presa aos pixels 1×), HUD e as texturas das 16 pistas, com média ≤ 0,35/255 na grade RGBA16.
>     - **M4 — combinador, blender, neblina, alpha compare, coverage e decal (2026-10-29):** conferidos o chão com neblina de Toad's Turnpike, Choco Mountain e Moo Moo Farm, o gelo do Sherbet Land, Rainbow Road, os escurecimentos dos menus (blend com a memória) e a fumaça e as sombras dos karts. Nenhum dos 26 replays usa o alfa da memória como peso (o log `GPU: blender com alfa da memoria` não aparece), então o blend fixo cobre todos os modos que o jogo usa nesses trechos.
>     - **M5 — tela larga, céu, barras, escritas da CPU, 60 fps e escala até 8 (2026-10-29):** margem lateral, scissor e retângulos de largura total alargados, céu (triângulos ortográficos de largura total) esticado e barras pretas nas imagens 2D de largura total seguem as regras do software (`span_cols`, `draw_tri`, `draw_texrect` em `gpu.c`). As escritas da CPU entram pelo mesmo `hi_sync` (retângulo dos pixels mudados → `gpu_patch`); é também por ele que um alvo novo recebe a imagem 1× ampliada. A imagem intermediária é uma segunda textura por alvo. `resolucao_interna` vai até 8 com a GPU (até 4 no software). Conferido:
>       - `gpu_compare.py --scale 3 --aspect 21:9 --interp` em menu, Luigi Raceway, tela dividida de 2 jogadores, Rainbow Road, Sherbet Land e Toad's Turnpike: todos dentro dos limiares (média ≤ 0,14/255 na grade);
>       - `--scale 4 --aspect 32:9 --interp` no Luigi Raceway: ok;
>       - 8× em 32:9 (6826×1920) sem janela, conferido a olho;
>       - na janela, 16:9 a 4× com interpolação: 5,6–6,2 ms por quadro;
>       - com as validation layers, só o aviso esperado de saída sem alvo no pipeline que limpa o Z;
>       - queda para o software sem driver Vulkan (`VK_LOADER_DRIVERS_SELECT` sem correspondência), com e sem janela, com o motivo no log.
> - **Conferido (2026-10-29):** a tela larga não mostra objetos surgindo tarde nas laterais. Foram 34 quadros em 16:9, ao longo de uma volta do Luigi Raceway e do começo do Koopa Troopa Beach e do Bowser's Castle, com pista e cenário completos até as bordas. O teste de visão dos karts (`FUN_8001fb0c`) usa meio-ângulo de ±58° com 1 jogador, mais largo que a imagem 16:9.
> - **Falta:** na interpolação, partículas que nascem ou somem no quadro não são interpoladas.

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
> - **24 replays em `tests/replays/`, todos passando:**
>   - título → menus → largada no Luigi Raceway;
>   - Time Trials com aviso de Controller Pak e recordes da EEPROM;
>   - largada em **cada uma das 16 pistas**;
>   - **VS 2 jogadores** em tela dividida;
>   - **Battle 2 jogadores** na Big Donut.
>   - **VS 4 jogadores** no Luigi Raceway (tela em quatro) e **Battle 3 jogadores** na Big Donut (o 4º quadrante mostra o mapa). Com 3–4 controles conectados a abertura demora mais, então os menus vêm ~250 retraces depois; os controles 2–4 apertam B no retrace 1, sem o que a sequência muda.
>   - **Recorde novo no Time Trials:** o piloto automático completa as 3 voltas do Luigi Raceway em 2'02"19, e a tela mostra "BEST RECORDS 1st 02'02"19 MARIO". O replay `tt_recorde_luigi` confere também o SHA-1 da EEPROM gravada (`check_eeprom` no `tools/replay.py`): o tempo (centésimos, 3 bytes) entra na tabela e na cópia de reserva.
> - **EEPROM:** no primeiro boot o jogo grava os recordes padrão (512 bytes com checksum) em `mk64.eep`.
> - **Correção:** neblina calculada depois do recorte no near plane (vértices atrás da câmera davam z/w sem sentido).
> - **Piloto automático para testes:** `MK64_AUTOPILOT=n` dá ao jogador 1 as flags de kart da IA (`0x9010` em vez de `0xc010`, achadas num dump da RAM com `MK64_DUMPRAM`). A IA do próprio jogo pilota o Mario: ele completou as 3 voltas do Luigi Raceway (7º lugar). Ao terminar, o jogador volta a ser humano (bit `0x0800` = corrida terminada).
> - **Fim de corrida resolvido:** o jogo só avança a tela de resultado quando um jogador **humano** cruza a linha. O piloto automático agora devolve o controle quando o progresso do jogador 1 chega a 2,97 voltas, com A segurado.
>   - Endereços achados comparando dumps: contador de voltas `0x80164390` (s32 × 8, o mesmo que o código de debug da pausa altera) e progresso acumulado `0x801644d0` (float × 8, voltas + fração).
>   - Replay `gp_corrida_completa`: 3 voltas, ranking, RETRY e nova largada; 82 quadros idênticos em duas execuções.
> - **Aberto:**
>   - Pódio: exige terminar entre os 4 primeiros nas 4 corridas da copa. Com a IA do jogo pilotando o Mario, ~24 corridas seguidas (400 mil retraces, já sem travar) terminaram sempre entre 5º e 8º. Aumentar a velocidade máxima do Mario (+0x214, de 290 para 340, via `MK64_POKE`) não muda nada: no GP, o Mario pilotado pela IA leva ~3'55" nas 3 voltas, enquanto no Time Trials a mesma IA faz 2'03". A IA do GP regula a velocidade das CPUs em relação ao humano, e sem humano trava o próprio Mario. Para o pódio falta um piloto de teste próprio, com o kart ainda humano, seguindo os pontos de caminho da pista.
> - **Corrigido (2026-10-29): a 5ª largada da sessão travava.** Depois de 4 corridas (por RETRY, DRIVER CHANGE ou fim de corrida), a largada seguinte ficava parada, sem Lakitu nem contagem, também em janela. A causa: `0x800CDA60` era tratada como `osInvalICache` e virava "não fazer nada", mas é o `bzero` (o corpo grava zeros). O jogo a usa para esvaziar a lista de objetos a cada largada (`0x80165c18`, 550 entradas de 0xE0 bytes). Sem isso, cada largada somava ~174 objetos, e na 5ª não havia vaga para o Lakitu. Achado com perfis de chamadas por função (`-DMK64_PROFILE=ON`, `MK64_PROFILE=a,b`), dumps de RAM em vários retraces (`MK64_DUMPRAM=a,b,...`) e estado das threads (`MK64_THREADDUMP=n`). Agora 8 largadas seguidas funcionam, e a lista fica em 174 objetos. Os replays foram regravados: o desenho só muda em detalhes de fumaça e pose, porque os objetos passam a começar do zero.
>   - ~~Choco Mountain aparece com muita neblina branca.~~ Corrigido em 2026-10-29: era o chão faltando, não neblina. Ver a correção do `ALPHA_CVG_SEL` na fase 5.

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

## Fase 1 — Mapa completo da ROM (1–2 semanas) — 🔶 em andamento

> **Feito (2026-10-28):**
> - `tools/build_rom_map.py` gera `analysis/rom_map.json` a partir da ROM: 81 regiões, 100% da ROM, sem sobreposição. O mapa é em JSON, sem dependências externas.
> - Código: 3 segmentos. `main`, `racing` e `seg3` (ROM `0x123640` → `0x80280000`).
> - Leituras da ROM: 36 chamadas diretas ao DMA `0x800cdc30` + o wrapper `0x80001158` (REA `ev_eebc979b…`, `ev_9a3a7a20…`).
> - Tabela de pistas `0x802B8D80`: 20 entradas × 0x30, cada uma com 3 faixas de ROM.
> - Áudio: tabelas tipo 1/2/3 (bancos/samples/sequências) e o mapa sequência→banco, com tamanhos tirados das próprias tabelas.
> - `tools/mio0.py` descomprime e valida: os 3.354 blocos MIO0 decodificam sem erro (zero falsos positivos).
>
> **Falta:**
> - O fim dos 4 "bancos" (`0x145470`, `0x641f70`, `0x729a30`, `0x7fa3c0`, 57% da ROM) é inferido: cada um vai até o próximo início conhecido. Falta achar as tabelas que listam cada asset dentro deles.
> - Separar o que é cada faixa das pistas. Os nomes `geo`/`packed`/`seg9` são provisórios.
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

## Fase 2 — Inventário de código (2–3 semanas)

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

## Fase 3 — Recompilador estático MIPS → C (4–8 semanas)

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

## Fase 4 — Runtime nativo, a "libultra" do PC (4–6 semanas)

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

## Fase 5 — Gráficos: F3DEX/F3DLX 0.95 + RDP (6–10 semanas)

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

## Fase 6 — Áudio (3–5 semanas)

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

## Fase 8 — Validação comportamental com o REA (contínua)

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

## Fase 9 — Substituir tradução por C legível (opcional, contínuo)

Para cada sistema já entendido (controle → esterço → aceleração → drift → itens → IA):
1. Escrever a função em C legível, com nomes reais e structs (`Player`, `Controller`).
2. Ligar uma chave de compilação que alterna entre a versão traduzida e a nova.
3. Rodar todos os replays: a RAM e as capturas precisam bater bit a bit.
4. Só então remover a versão traduzida daquela função.

---

## Fase 10 — Melhorias nativas (depois que tudo bate)

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

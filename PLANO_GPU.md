# Plano: desenho na GPU (imagem de exibição)

> **Estado (2026-10-29): executado, etapas M0 a M6.** O registro de cada etapa, com resultados e pendências, está no `PLANO.md` (fase 10, "Desenho na GPU").

Plano para executar em outra sessão. Ele parte do estado do repositório em 2026-10-29: todas as fases do `PLANO.md` concluídas ou pausadas, 26 replays passando.

## Objetivo

A imagem que aparece na tela passa a ser desenhada pela GPU, em vez do RDP em software. Isso permite resolução interna alta (4×, 8× ou a resolução nativa da janela) sem custo de CPU.

O desenho em 1× continua em software e inalterado. Ele escreve na RDRAM o que o jogo lê, e é o que os replays conferem (hash por quadro). Por isso, nenhum replay pode mudar.

## Como é hoje (o que a GPU vai substituir)

- `gfx/rsp.c` (HLE do F3DEX) transforma vértices, recorta no plano próximo e entrega à `gfx/rdp.c`:
  - triângulos `RdpVertex` (x, y em pixels 1×; z 0..1; invw; rgba de sombreamento; s, t em texels);
  - retângulos de textura (`rdp_texrect`);
  - comandos do RDP (`rdp_command`).
- `gfx/rdp.c` grava cada primitiva como um `Op` (`OP_TRI`, `OP_TEXRECT`, `OP_FILL`, `OP_RECT`) com um retrato (`RdpState`) do estado do RDP. O retrato só é copiado quando o estado muda e tem cerca de 4,3 KB, porque inclui a TMEM inteira. O `rdp_flush()` então desenha tudo duas vezes:
  - `draw_pass(1, 0)`: em 1× na RDRAM. **Isso fica.**
  - `draw_pass(g_scale, 1)`: a "passada ampliada" em imagens privadas (`HiTarget`, uma por endereço de framebuffer, RGBA16). **É isso que vai para a GPU.**
  - Com interpolação há uma terceira passada, `draw_pass(g_scale, 2)`, que desenha a imagem intermediária (`icolor`). Ela vem de uma segunda execução da lista de exibição (`rsp_run_display_list_interp`). **Também vai para a GPU.**
- A passada ampliada tem regras próprias que precisam ser mantidas:
  - **Escala e margem:** coordenadas × `g_s` mais a margem lateral `g_ox` da tela larga (`hi_ox`, `span_cols`, `clip_cols`).
  - **Céu esticado:** triângulos ortográficos (invw = 1) de largura total são esticados até as bordas (`draw_triangle`).
  - **Barras pretas:** retângulos de textura de largura total ganham barras pretas nas margens (`draw_texrect`).
  - **Amostragem dos retângulos de textura:** começa no canto do pixel e fica presa aos pixels que o retângulo cobre em 1× (`draw_texrect`). Sem isso, voltam os cortes nos menus.
  - **Escritas da CPU:** pixels que a CPU escreveu na RDRAM entre dois lotes entram ampliados (`hi_sync` compara com a cópia `shadow`).
  - **Profundidade:** a passada ampliada tem um Z buffer próprio (`g_zhi`).
- `src/main.c` lê a imagem pronta com `rdp_read_framebuffer_hires` / `rdp_read_framebuffer_interp` e a envia por `SDL_UpdateTexture`. A apresentação é feita pelo `SDL_Renderer` (SDL 3.4.16), com letterbox e filtro nítido ou suave.
- **Comportamento do RDP que o software já acerta e a GPU tem que manter:**
  - **Filtro bilinear:** sem deslocamento de meio texel.
  - **Descarte por alfa:** só com `CVG_X_ALPHA`; o `ALPHA_CVG_SEL` sozinho não descarta.
  - **Alpha compare:** contra o alfa da cor de blend.
  - **Blender:** só mistura com `FORCE_BL`; sem ele, o ciclo final passa P adiante.
  - **Neblina:** no ciclo 0 do blender, com o alfa do sombreamento.
  - **Z_MODE decal:** com a tolerância de 0,002.
  - **Z_SRC_PRIM:** usa `prim_depth`.

## Backend: Vulkan (escolha do usuário)

- **Caminho recomendado: a API de GPU do SDL3 (`SDL_gpu.h`, SDL 3.4.16), forçando o driver Vulkan.** O backend é Vulkan de verdade (`SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, "vulkan")`). O SDL cuida de memória, sincronização, filas e swapchain, o que tira a maior parte do código repetitivo do Vulkan e mantém um único lugar de inicialização.
  - **Apresentação:** passa do `SDL_Renderer` para o próprio `SDL_gpu`. A janela é reivindicada com `SDL_ClaimWindowForGPUDevice`. Cada quadro termina num passe de cópia/blit do render target para a imagem da swapchain, com letterbox e filtro nítido ou suave reimplementados ali, que são uma cópia escalada simples. O texto de status (F1) usa hoje `SDL_RenderDebugText` e precisa de um substituto simples; pode até sumir nesse modo.
  - **Modo sem janela:** o `SDL_gpu` cria o device sem janela. Isso serve para a comparação automática de imagens (`tools/gpu_compare.py`).
  - **Shaders:** em GLSL (`#version 450`), compilados para SPIR-V no build e embutidos no executável (gerar um `.h` com o SPIR-V). O `SDL_gpu` recebe SPIR-V direto (`SDL_GPU_SHADERFORMAT_SPIRV`). Os recursos seguem a convenção de sets/bindings do `SDL_gpu`: em fragment shaders, set 2 para texturas somente leitura e storage buffers, set 3 para uniform buffers. Verificar a convenção na documentação de `SDL_CreateGPUShader`.
  - **Compilador de shader (decisão a confirmar com o usuário antes de baixar):**
    1. **Recomendado:** `glslangValidator` ou `glslc` do Vulkan SDK (LunarG), instalado pelo usuário. O CMake o acha por `find_program` e gera o SPIR-V como etapa de build.
    2. **Alternativa sem instalar SDK:** glslang via `FetchContent` (compila a partir do código-fonte, como já é feito com o SDL). Deixa o primeiro build mais lento.
  - **Requisito em tempo de execução:** um driver de vídeo com Vulkan 1.1 ou mais novo (NVIDIA, AMD e Intel atuais trazem `vulkan-1.dll`). Se a criação do device falhar, o jogo volta para o desenho em software sem fechar.
- **Alternativa: Vulkan "puro"** (vulkan-headers via `FetchContent`, loader `vulkan-1.dll` do driver, funções obtidas por `SDL_Vulkan_GetVkGetInstanceProcAddr`). Dá controle total (por exemplo, `VK_EXT_fragment_shader_interlock` para blend programável), mas são milhares de linhas a mais. Só vale se o `SDL_gpu` limitar algo de que o renderizador precise; nesse caso, registrar o motivo no plano antes de trocar.
- **Validação:** em builds de depuração, ligar as validation layers (`debug = true` no `SDL_CreateGPUDevice`). Elas vêm com o Vulkan SDK. Sem o SDK, rodar sem elas.

## Arquitetura proposta

1. **Gravação:** sem mudanças. `Op` + `RdpState` continuam sendo a entrada; a GPU consome a mesma lista que a passada ampliada consome hoje. Criar `gfx/gpu.c` / `gfx/gpu.h`. No `rdp_flush`, quando a GPU estiver ativa, `draw_pass(g_scale, 1|2)` vira `gpu_draw_ops(ops, count, interp)`.
2. **Alvos:** um render target RGBA8 + depth (D32F) por `HiTarget`, com o mesmo endereço e a mesma largura. A imagem intermediária usa um render target separado. A margem e o tamanho vêm de `hi_ox` / `hi_stride`, e a escala pode passar de 4.
3. **TMEM:** 4 KB por retrato, enviados como storage buffer somente leitura (`SDL_GPUBuffer` com `SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ`; no GLSL, `readonly buffer` de `uint`). Os bytes são lidos com deslocamento e máscara. Guardar em cache por hash do conteúdo, porque a TMEM muda pouco entre retratos. O fragment shader faz em GLSL o mesmo que `fetch_texel` / `wrap_coord` / `shift_coord` / `tlut_color`:
   - formatos RGBA16/32, CI4/8, IA4/8/16 e I4/8;
   - TLUT RGBA16 e IA16;
   - linhas ímpares com os bytes trocados (^4);
   - RGBA32 dividido entre as metades da TMEM.

   Começar com um "ubershader" que lê o retrato inteiro de um constant buffer ou structured buffer. Só especializar se a medição mostrar necessidade.
4. **Combinador:** o ubershader replica `combine_cycle` (1 e 2 ciclos) com os seletores vindos do retrato. Os mesmos atalhos de `need_tex0` / `need_tex1` / `need_mem` podem virar ramos.
5. **Blender e memória:** o fragment shader não lê o render target em que está escrevendo (o `SDL_gpu` não expõe input attachments nem interlock). Plano:
   - **Fixed-function:** os modos usados pelo MK64 viram estados de blend. `P*A + M*(1−A)` vira alpha blend comum, e com `FORCE_BL` desligado o blend fica desligado. A neblina (ciclo 0 com a cor de neblina e o alfa do sombreamento) é calculada no shader, porque não depende da memória.
   - **Log de modos:** registrar os modos encontrados que não se encaixam (em debug, contar combinações `om_lo` / `combine` distintas, como foi feito ao investigar o chão sumindo). Se algum precisar da cor da memória de outro jeito, copiar o alvo para uma textura de leitura (`SDL_CopyGPUTextureToTexture`, num passe de cópia) antes daquele lote. Se isso ficar frequente demais, reavaliar o Vulkan puro com `VK_EXT_fragment_shader_interlock`.
6. **Profundidade:** depth test/write nativos.
   - Comparação `LESS_EQUAL`, como o software: `z > cur` rejeita.
   - Decal (`Z_MODE == 3`): `LESS_EQUAL` com tolerância de 0,002, via depth bias do pipeline ou ajuste do z no shader.
   - `Z_SRC_PRIM`: o shader escreve `prim_depth` em `gl_FragDepth`.
   - Cada combinação de estados fixos (blend, depth, cull desligado) vira um `SDL_GPUGraphicsPipeline`, guardado em cache por chave.
   - Limpar o Z quando o jogo preenche a imagem de Z (`OP_FILL` com `cimg == zimg`).
7. **Primitivas:**
   - **Triângulo:** vértices em pixels do alvo (x × escala + margem), com o céu esticado igual ao software. Perspectiva por `invw`: s, t e cores interpoladas com correção de perspectiva só quando `TEX_PERSP` estiver ligado.
   - **Retângulo de textura:** quad com s, t calculados como em `draw_texrect`, incluindo o clamp à área em 1×. O modo cópia (`CYC_COPY`) amostra sem filtro e só faz alpha test.
   - **Fill e rect:** quads, com a paridade de 16 bits do fill.
   - **Barras pretas:** mantidas.
8. **Escritas da CPU:** o `hi_sync` continua na CPU para achar os pixels mudados. Os retângulos mudados sobem como textura e são desenhados ampliados no alvo antes do lote, ou num caminho simples: subir a imagem 1× inteira e copiar só onde mudou.
9. **Saída:** `rdp_read_framebuffer_hires` / `rdp_read_framebuffer_interp` ganham uma variante que devolve a `SDL_GPUTexture` do alvo (sem cópia para a CPU). O `main.c` apresenta a imagem com um blit para a swapchain (`SDL_BlitGPUTexture`, com filtro nearest ou linear e o retângulo do letterbox). As capturas (`MK64_SHOTS`, `frame_XXXXX_hi.bmp`) leem de volta por transfer buffer (`SDL_DownloadFromGPUTexture`), só quando pedidas.
10. **Configuração:** no `mk64.ini`, `renderizador = gpu | cpu`, com padrão `gpu` se a inicialização der certo e queda automática para `cpu`. `MK64_RENDERER` sobrepõe. `resolucao_interna` passa a aceitar até 8 no modo GPU. No modo sem janela (headless), o padrão continua `cpu`.

## Etapas (cada uma com commit e entrada no `PLANO.md`)

1. **M0 — Base:** device do `SDL_gpu` com Vulkan; janela reivindicada; render target limpo com uma cor fixa e apresentado por blit no lugar do `screen_hi`; shaders GLSL → SPIR-V no build (depois de o usuário confirmar o compilador); queda para o software com mensagem clara quando o Vulkan não existir; opção `renderizador`. Sem o modo GPU ligado, a apresentação atual pelo `SDL_Renderer` continua igual.
2. **M1 — Fills, rects e retângulos de textura em modo cópia:** a abertura, o título e os menus aparecem certos.
3. **M2 — Triângulos só com sombreamento** (sem textura), com Z: a geometria das pistas aparece.
4. **M3 — TMEM e texturas:** todos os formatos, wrap/mirror/clamp, máscaras e filtro. As pistas aparecem com textura.
5. **M4 — Combinador de 2 ciclos, blender, neblina, alpha compare, coverage e decal.**
6. **M5 — Tela larga, céu esticado, barras pretas, escritas da CPU, imagem intermediária (60 fps) e escalas acima de 4.**
7. **M6 — Desempenho e acabamento:** medir ms por quadro (CPU e GPU) em 2×, 4× e 8×, ajustar o cache de TMEM e de estados, queda para CPU, documentação.

## Verificação

- **Replays:** `python -I tools/replay.py <rom>` precisa passar idêntico em todas as etapas. A passada 1× não muda, e os replays rodam sem janela, em CPU.
- **Comparação de imagem GPU × software:** criar `tools/gpu_compare.py`. Ele roda cada replay duas vezes, com `MK64_RENDERER=cpu` e `=gpu`, com a mesma escala, e compara os `frame_XXXXX_hi.bmp`:
  - **Métrica:** diferença média por canal e porcentagem de pixels com diferença maior que 8/255.
  - **Limiar inicial sugerido:** média ≤ 2/255 e no máximo 1% dos pixels acima de 8/255. A diferença esperada vem de arredondamento e da precisão da interpolação.
  - **Relatório:** gera um mosaico com as piores diferenças, para olhar, e uma imagem de diferença.

  Requisito: o modo sem janela cria o device do `SDL_gpu` sem janela quando `MK64_RENDERER=gpu`.
- **Inspeção visual nas cenas que já deram problema:**
  - chão das pistas com neblina: Toad's Turnpike, Choco Mountain, Moo Moo Farm;
  - cortes nos retratos da seleção de personagens e nas caixas de menu;
  - reflexo no gelo do Sherbet Land;
  - Rainbow Road;
  - tela dividida de 2 e 4 jogadores;
  - tela larga 21:9 e 32:9.
- **Tempo:** registrar no log (`render: ... ms`) o tempo de CPU por quadro antes e depois, em 2× e 4×, com interpolação.

## Riscos

- **Blender com cor da memória** em modos fora do padrão: a solução é a cópia do alvo antes do lote, mais lenta. Registrar quais modos aparecem.
- **Precisão:** z, perspectiva e coordenadas de textura em float na GPU podem diferir em fração de texel do software. Compensar no limiar da comparação e olhar as bordas.
- **Muitos retratos por quadro:** pode haver centenas de mudanças de estado. Agrupar draws com o mesmo retrato e usar cache de TMEM e de constant buffers.
- **Fallback:** se não houver Vulkan ou a criação do device falhar, voltar para o software, e para o `SDL_Renderer` atual, sem fechar o jogo.
- **Duas formas de apresentar:** o `SDL_gpu` e o `SDL_Renderer` não dividem a mesma janela. Escolher uma no início, conforme o modo, e manter as duas funcionando.

## Regras do projeto (valem para a sessão que executar)

- **Nunca commitar bytes da ROM nem imagens do jogo.** Os BMPs ficam em `build/`, que é ignorado; prints não vão para o repositório.
- **Build:** pelo PowerShell, `cmd /c "C:\Users\nicko\Desktop\projects\mk64-port\build.bat"`. Para regenerar o código recompilado, definir antes `$env:MK64_ROM = "C:\Users\nicko\Downloads\carrinho2\carrinho.z64"`.
- **Git:** identidade local `nicko <nickolasvictor@gmail.com>`. Mensagens de commit em português, terminando com `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Um commit por etapa, com o `PLANO.md` atualizado (seção da fase 10).
- **Downloads e instalações:** pedir confirmação ao usuário antes.
- **Escrita de scripts:** heredocs no bash convertem `\n` dentro de strings C/Python. Para editar código com escapes, usar arquivos de script no scratchpad ou a ferramenta de edição.

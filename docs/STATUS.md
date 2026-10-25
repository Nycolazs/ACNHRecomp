# Estado atual

Medido no dump 3.0.2, Linux x86-64 (Ubuntu 26.04, clang 21, Intel Iris Xe / Mesa). Atualizado a cada avanço.

## Pipeline

| Etapa | Estado | Detalhes |
|---|---|---|
| Importação NSP base + update | ✅ | BKTR aplicado; 103.400 arquivos, 10,5 GB, ~5 min |
| Recompilação `main` | ✅ | 178k funções, 17,2 M instruções, 0 não tratadas |
| Recompilação `sdk` / `subsdk0` | ✅ | 1,7 M / 0,9 M instruções, 0 não tratadas |
| Instruções novas (SDK 20) | ✅ | 15 mnemônicos, validados contra o Unicorn |
| Relocações RELR (SDK 20) | ✅ | `main`: 7,5 mil RELA → 380 mil relocações no total |
| Shaders → SPIR-V | ✅ | 6.200 blocos, 0 inválidos, 0 exceções |
| Build Linux | ✅ | ~1h45 na primeira vez (12 threads) |
| Build macOS | ⏳ | não testado ainda (sem Mac aqui); CMake e scripts prontos |

## Execução

| Marco | Estado |
|---|---|
| Carrega e liga os 3 módulos | ✅ |
| `nn::init::Start`, `nnosInitialize`, construtores | ✅ |
| `sm:` e serviços (am, acc, aoc, pctl, time, hid, fs, audio, pl, nfp) | ✅ |
| Dispositivo NVN, janela, pools de memória | ✅ |
| AudioRenderer (o do próprio SDK, em processo) | ✅ renderizando a 48 kHz, saída `MainAudioOut` |
| Carregamento de assets (fontes, mensagens, layouts, efeitos, sons) | ✅ |
| Loop principal: leitura de controle, 30 fps de submits Vulkan | ✅ |
| Shaders: todos os desenhos com programa (0 sem shader) | ✅ |
| UI renderizada corretamente (elementos de layout conferidos por dump) | ✅ |
| Relógio local (data/hora do computador, fuso do sistema) | ✅ ("Good afternoon!" à tarde) |
| Save do aparelho (`MountDeviceSaveData`) em `private/acnh/user/device_save` | ✅ monta; gravação a confirmar |
| BCAT (conteúdo entregue pela Nintendo): vazio, como console nunca conectado | ✅ |
| **UI na tela**: tela de carregamento, diálogos do Timmy | ✅ |
| **Criação de personagem** (registro Nook Inc.) com o teclado do próprio jogo | ✅ digitação funciona |
| Renderização 3D (cena do oceano ao entardecer, terreno em alvos menores) | ✅ conferida por dump |
| Cena 3D da abertura (Timmy e Tommy no balcão da Nook Inc.) | ✅ |
| Registro completo: nome (`+` confirma), aniversário | ✅ (o jogo recusa nomes com mais de 5 algarismos, como no console) |
| DLC (`aoc:u` + `MountAddOnContent`): os dois pacotes do dump montados | ✅ |
| Escolhas da introdução: estilo, pele, hemisfério, mapa da ilha, avião | ✅ |
| Filme de abertura (`OpeningMovie.webm`, VP9 + Vorbis, 65 s) inteiro, em cores, com som | ✅ |
| Chegada à ilha ("We're here!"), diálogos do Timmy no píer | ✅ |
| Ilha em 3D: píer da Dodo Airlines, personagens, água, árvores, tela de título com a ilha | ✅ |
| Controle livre do personagem na ilha (andar, câmera acompanha) | ✅ |
| Alguns moradores (pássaros) com parte da cabeça preta | 🔧 os moradores iniciais são sorteados; não reproduz sempre |

## Problemas encontrados e resolvidos

1. `hactool` não lê o BKTR das NCAs do SDK 20 ("Invalid BKTR layout"): usamos o importer próprio.
2. `prod.keys` com entradas de tamanho ímpar quebrava o hactool: o setup filtra as linhas malformadas.
3. Tabelas de salto em funções com cauda compartilhada apontando para antes da entrada: viram entradas.
4. libdecor/GTK aborta o processo em alguns GNOME (schema sem `antialiasing`): preferimos X11/XWayland.
5. `svcGetInfo` tipos 24-28 (firmware 18+, AliasRegionExtraSize) adicionados.
6. **DT_RELR** não era lido: ponteiros ficavam sem a base (crash em `nn::lm`). Agora é decodificado
   (`main`: 7,5 mil → 380 mil relocações; a descoberta de funções também ganhou 0,8 M instruções).
7. `DT_INIT` não fica mais em +0x30 no SDK 20: lido do `.dynamic`.
8. Tabela de formatos do driver NVN em offset fixo do SDK 16.2: agora achada pelo conteúdo (`sdk+b7f7d8`).
9. `hid`: touch screen, cores do controle, LED, estilos suportados e modos de Joy-Con (HLE novos).
10. `IApplicationFunctions` 210: evento que o `nn::oe::GetOperationMode` espera; sem ele, laço infinito.
11. Áudio: offsets do AudioRenderSystem e id do comando DeviceSink mudaram no SDK 20; agora lidos do código.
12. `nn::pl::GetSharedFont` / `RequestSharedFontLoad`, `nn::nfp` (amiibo: sem leitor) implementados.
13. Orçamento de memória `cSys` do motor estourava (heaps de tarefa criados com "o resto" e aparados
    depois, em outra ordem que no console): +64 MiB vindos de `cOthers` (`games/acnh/game.cpp`).
14. Shaders das bibliotecas de amostra do SDK (`System/Font`, `System/PrimitiveRenderer`, `System/Agl`): novo
    formato no extrator; sem eles o passe final de tela não tinha vertex shader (9.366 blocos agora).

15. `nn::time` em nível de API (HLE): relógio do host e conversões com o fuso local. Sem isso o jogo mostrava
    "The system is set to a date outside the supported range." (`runtime/src/hle_time.cpp`).
16. Save do aparelho: `MountDeviceSaveData`, `Get/ExtendDeviceSaveDataSize`, `Commit`, `RenameDirectory`,
    `CleanDirectoryRecursively` (`runtime/src/hle_fs.cpp`).
17. `nn::bcat` (`runtime/src/hle_bcat.cpp`): armazenamento de entrega vazio.
18. Vulkan: recursos 1.1/1.2 (`shaderDrawParameters`) e `VK_EXT_fragment_shader_interlock` habilitados; sem eles
    as camadas de validação acusavam os shaders traduzidos.
19. Tela preta: o "clear on bind" do motor (`main+37fb390`) limpava o alvo HDR a cada troca de alvo da UI, e a
    composição final lia o alvo limpo. No console esses clears são controlados por estado que a GPU mantém; por
    enquanto os clears chamados de `main+37fb4fc` são ignorados (`games/acnh/game.cpp`, `NX_SKIP_CLEAR_AT=0` desliga).

20. **DLC**: o importer extrai a RomFS dos NCAs `PublicData` de cada add-on para `private/acnh/aoc/<índice>/`
    (`setup_acnh.sh --dlc <nsp>`); `nn::aoc::Count/ListAddOnContent` e `nn::fs::MountAddOnContent` respondem com o que
    está lá. Os dois DLCs do dump (300 Pocket Camp, 301 Nook Inc. Silk Rug) são vistos e montados pelo jogo.
21. `AudioOut` sem evento, formato/taxa/canais/volume (HLE): o filme de abertura toca o som por ele. Enquanto o
    AudioRenderer roda, o som do AudioOut é somado à mixagem final dele em vez de entrar na fila do dispositivo.
22. **Filme de abertura**: o `movie::Decoder` do SDK é o stagefright do Android sobre o NVDEC do console (aborta ao
    abrir o driver). O construtor é substituído para vídeo (VP8/VP9): o objeto recebe uma vtable nossa com o mesmo
    contrato (índices de buffers de entrada/saída, quatro eventos), decodificada com libvpx em NV12
    (`runtime/src/hle_movie.cpp`). O áudio (Vorbis) continua no decodificador do SDK, que é software.

23. **Kernel (compartilhado)**: `svcWaitSynchronization` descartava um `CancelSynchronization` que chegava antes da
    espera. No Horizon ele fica pendente até uma espera consumi-lo; é assim que `nn::os::MultiWait` acorda quem espera
    eventos. Sem isso, acordadas se perdiam ao acaso (o player do filme travava num quadro aleatório).
24. **NVN**: `nvnCommandBufferCopyBufferToTexture` não existia no runtime (caía num stub neutro): agora é gravado e
    reproduzido como `vkCmdCopyBufferToImage` a partir dos bytes do jogo. O filme copia seus planos NV12 assim.
25. `nn::friends` (offline: sem amigos, sem bloqueados, sem conta de rede): `GetBlockedUserList` lia um ponteiro nulo
    ao chegar na ilha.
26. Decodificador do filme: depois da entrada de fim de stream não oferece mais buffers (como o MediaCodec); senão a
    fila de mensagens do jogo enchia e o encerramento do player esperava para sempre.

27. **Cena 3D preta / parede por cima dos personagens**: o "clear on bind" do motor (`main+37fb390`) limpa também a
    profundidade (`main+37fb744`) a cada vez que um passe liga o alvo. O passe de cor religa o alvo da cena depois do
    pré-passe de profundidade, então a profundidade era apagada e o passe de cor (teste EQUAL) desenhava tudo em
    ordem: a parede do fundo cobria Timmy e Tommy, e o resultado ficava preto. Agora só a primeira limpeza de
    profundidade de cada alvo por quadro é mantida (`NX_CLEAR_ONCE_AT`, em `nvn_cmd.cpp`); as limpezas de cor dali
    continuam descartadas. Imagens de alvos de render ficam guardadas por memória (o jogo reinicializa os mesmos
    `NVNtexture` várias vezes por quadro).

28. **`setjmp` do SDK**: o `sdk` define o próprio `setjmp` e o chama pela PLT dele; o recompilador só reconhecia
    importados, então a libjpeg/libpng do SDK (fotos) fazia `longjmp` sem destino e o processo abortava. Agora chamadas
    a um `setjmp` definido no próprio módulo também ganham o `try` (`recomp/analysis.py`, `recomp/emit.py`).
29. `nvnCommandBufferCopyTextureToBuffer`: reproduzido como `vkCmdCopyImageToBuffer` e copiado para a memória do jogo
    depois do submit (o jogo lê de volta, por exemplo, a foto 500x500 do passaporte).

30. **Recompilador: cache de registradores** (`recomp/regcache.py`): dentro de cada função os registradores gerais,
    vetoriais e as flags NZCV do convidado viram variáveis locais em C++, sincronizadas com o `nx::CPU` só em chamadas,
    `svc`, `setjmp`/`longjmp` e retornos, e só os que podem ter mudado (fluxo de dados "talvez sujo" sobre os `goto`).
    Antes o clang recarregava `c.x[31]` e companhia depois de cada store do jogo (o ponteiro do convidado vem de um
    inteiro e pode, para ele, apontar para o contexto). Reverb I3DL2 (`NX_PROFILE_REVERB=1`, mediana de 3000
    repetições): 9,7 us por chamada antes; 7,1 só com as flags; 4,1-4,7 com tudo. Binário 6 MB menor e compilação
    completa em 28 min. Validado na introdução inteira até a ilha. `NX_REGCACHE=0` no recompilador desliga.
31. **Inicializador** (`launcher/`, `acnhrecomp_launcher`): programa sem código do jogo que pede chaves, jogo,
    atualização e DLC, confere os arquivos e roda a recompilação no próprio computador (`setup_acnh.sh --out`), ou
    abre uma versão já recompilada. Mesmo visual e idiomas da tela de configurações.
32. **Mar desenhado por cima do píer, do jogador e da praça** (quadros isolados na ilha): o "clear on bind" mantido
    uma vez por quadro (item 27) decidia o quadro pelo `g_frame` da thread que apresenta, mas o jogo grava os passes
    nas threads de render. Com rastreio, ~260 limpezas de profundidade "primeiras do quadro" vinham 0,2-9 ms depois de
    outra do mesmo alvo: o re-bind do passe da água caía logo depois da apresentação, apagava a profundidade e o mar
    (translúcido) cobria tudo. Agora a limpeza é gravada marcada (`kClearOnce`) e decidida no replay, onde o quadro é
    o número de apresentações na ordem da fila (`nvn_cmd.cpp`, `nvn_clears.cpp`). A cena da Nook Inc. continua certa.
33. **Atualização 3.0.3** (24/10/2026): suportada junto com a 3.0.2. A maior parte do código entre main+280c000 e
    main+3e02000 andou 0x10; os dados não andaram ([ACNH.md](ACNH.md), "Updates"). Ganchos em
    `games/acnh/hooks_303.txt` (o `setup_acnh.sh` escolhe a lista pelo build id do `main`), locais do "clear on bind"
    e o SetFocus do `pointer.cpp` escolhidos em tempo de execução (`acnh::is_303()`). Com os locais de limpeza da
    3.0.2 o 3.0.3 apresentava só preto. Medido da Nook Inc. até a chegada na ilha: AMD Radeon 780M (RADV) e NVIDIA
    RTX 3080 Ti a 30 e a 60 fps; Steam Deck (RADV VanGogh) a 60 fps, com uma versão anterior das mesmas mudanças.
34. **SpeedFix: fim de função que passa a cair adiante** (`recomp/analysis.py`): em main+162d084 e main+2ead6cc o patch
    leva o desvio final da função uma palavra para dentro do preenchimento depois do fim do FDE. O corpo da variante
    parava antes dele e o jogo abortava com "call into the middle of a function" (main+162d2fc), na abertura a 60
    fps. Na análise da variante, uma função cuja última instrução deixa de desviar é estendida sobre as palavras do
    patch que vêm depois; rotinas do patch no preenchimento depois de uma função que ainda termina em desvio seguem
    separadas. Os mesmos dois locais e palavras existem no patch da 3.0.2.
35. **Fontes no Arch / SteamOS** (`runtime/src/hle_pl.cpp`): as fontes do sistema ficam em `/usr/share/fonts/TTF`,
    `/noto` e `/liberation`, que não estavam na lista. Sem fonte nenhuma o jogo lia uma vazia e falhava (dado nulo em
    main+252450, ao carregar as fontes compartilhadas) no Steam Deck.

## Teste de jogo roteirizado (23/10/2026)

Roteiros em `private/play/` (`gen.py` gera o `NX_INPUT` de uma linguagem simples: `include`, `press`, `walk` com
analógico e corrida, `wait`; `run.sh` roda numa raiz de dados isolada, com despejo de quadros e resumo de fps). O
`s5_island_menus.txt` faz a introdução inteira, os diálogos da chegada, anda até a praça, abre menus (`X`, `ZL`,
`Minus`), corre pela ilha (rio, praia, penhascos). `NX_FAST_UNTIL=<quadro>` apresenta a 60 fps até aquele quadro (a
lógica anda por quadro: a introdução leva metade do tempo; sem limite, as cargas que seguem o relógio levavam
milhares de quadros e o roteiro se perdia); `NX_INPUT` aceita agora `ZL`/`ZR`.

| Cenário | Resultado |
|---|---|
| Introdução (logos, registro, criação do personagem, Nook Inc., filme, voo) | ✅ sem falhas visuais |
| Chegada no píer, diálogos, moradores (pássaro azul, sapo, coelha...) | ✅ cabeças normais em todos os sorteados que vimos (nenhuma cabeça preta) |
| Caminhada/corrida: aeroporto, praça (tendas, quadro de avisos), rio, praia, penhascos | ✅ (mar sobre a cena: item 32) |
| `Minus` no 1º dia | ✅ "You can't save right now." (como no console); `ZL`/`X` sem efeito antes da cerimônia |
| Crashes | 1 em ~12 execuções, no carregamento da cena da Nook Inc. (abaixo) |
| Shaders sem `.spv`, hitches (`NX_HITCH_LINES`) | 0 em todas as execuções |

fps: a máquina estava dividida com outros processos do jogo e compilações (load 10-19) em todas as execuções, então
os números absolutos medem a disputa: ilha 8-30 fps, Nook Inc. 13-30 (com a GPU livre, `rc_island.log`: 30 fps
estáveis em tudo, replay da ilha 15-20 ms por quadro, a maior parte dos 33 ms).

Em aberto:
- ~~Crash intermitente (sinal 11, endereço 8) em `main+6d6514`~~ **corrigido**: os workers de jobs (`main+6d6514`,
  calc_ da thread feita pela fábrica `main+6c5b20`) registram-se no Havok se o singleton Phive (`main+5a05378`)
  existe, mas a thread principal publica o singleton (`main+3dfc120`) antes de montar o sistema em `g+8`
  (`main+3dfc2a8`, ~3 ms: PhiveSystemHeap de 32 MB). Um worker acordado tarde (wakeup do job queue em
  `main+3e42c08` atendido com atraso pelo host carregado) caía nessa janela. Gancho em `main+6d659c`
  (`games/acnh/game.cpp`): singleton pela metade conta como ausente, o registro fica para a próxima mensagem.
  Reproduzido de forma determinística segurando a thread principal 300 ms no `g->init` (gdb): sem o gancho, crash;
  com ele, a linha `acnh: a job worker woke while the Phive singleton is half built` e o jogo segue.
- Câmera de diálogo no píer (Tank, quadro 12420 a 60 fps): uma lâmina preta curva perto da câmera, provavelmente a
  porta/bandeira do aeroporto vista de trás. A confirmar com `NX_TRACE_DRAWS`.
- Um quadro com o chão marrom sem textura e só os contornos da grama (12720, ao sair da praça): transição de câmera
  ou textura ainda não carregada.
- Não chegamos à tenda, ao Resident Services nem à noite do 1º dia: o jogo só grava o save ao dormir, então cada
  teste refaz a introdução (~6 min com a GPU livre) e o limite de 8 min por execução acaba antes.

## Proporções largas: faixas nas cenas externas (24/10/2026, aberto)

Em 21:9 (`video.aspect`), dentro da Nook Inc. a imagem fica certa (com o `NX_UW_ORTHO` desligado, o padrão desde
`dcb9289`). Na ilha, porém, o último quarto da tela à direita sai borrado em faixas horizontais, a partir de 1/k da
largura (76% em 21:9), tanto em 1080 quanto em 1440. Em 16:9 a 1440 fica limpo. O que já se sabe:
- todos os alvos de tela são alargados (um trace com o arquivo `uw_trace` na raiz mostra `wide 1`); o array
  960x540x2 fmt 0x11 da ilha passou a ser alargado em `dcb9289`, mas não era a causa;
- descartar os passes de pós-processamento (`NX_SKIP_FS=fb8e3d25fa70ccb5,3f42705b617f3e52,92b733b47648169c,0ae22bc3aa4fedc9`)
  ou os que desenham no array (`2f9adfd53032cfd8,bfc1f66f7ee90900`) não tira as faixas;
- o reescalonamento do Ryujinx (`render_scale`, `nvn_sets.cpp`) é escalar (`min(sx, sy)`): ele não corrige um shader
  que monte uma coordenada de tela como `gl_FragCoord.x * (1/1920)` com a constante do próprio jogo. Esse é o
  principal suspeito; o próximo passo é achar o shader (bisseção com `NX_SKIP_FS` nos fragment shaders de
  `c0aa42f5f330ef6d`/`f9332e7274cde7c9`/`911007845f398fde`/`41ca6871be88f0b5`, que desenham a cena externa) e dar a ele
  uma escala por eixo.

## Próximos passos (pedidos)

- **60 fps**: medido: a lógica do ACNH avança **por quadro** (uma fala é digitada em ~115 quadros tanto a 30 quanto
  a 60 apresentações por segundo, `NX_PRESENT_INTERVAL=1`), então só apresentar a 60 deixa o jogo 2x mais rápido. Um
  60 fps de verdade exige achar e reduzir à metade o passo de cada sistema que conta quadros (não há uma constante
  única: o `cFrameRate` do binário é só o fim da string `MusicBeatSyncFrameRate`).
  Investigado de novo (3.0.2): o jogo nunca muda o intervalo de apresentação (fica o 2 do
  `NVNwindowBuilder`; nenhum log registra troca), então não há um "passo" do framework para seguir, como o `fw[0x260]` do Wonder. O tempo
  em segundos aparece só como constantes 1/30 embutidas no código (`mov`/`movk 0x3d088889`: 21 lugares, por exemplo
  `main+16548e4`, `main+14b9d5c`, `main+2ef7fd0`; 1/60 em 7), e os parâmetros de dados contam quadros
  (`ActionFrame`, `DelayMaxFrame`, `EmoticonActionAutoEndFrame`...). Os códigos de 60 fps da comunidade só trocam o
  intervalo e deixam o jogo 2x mais rápido; o FPSLocker-Warehouse marca o 3.0.3 como "sem patch" (velocidade,
  física e UI presas ao fps). Medição (mesma entrada, quadros despejados a cada 150): o carregamento segue o relógio
  (a cena da Nook Inc. começa ~900 quadros depois a 60), mas dali em diante cada imagem a 30 fps reaparece a 60 fps
  com o mesmo deslocamento fixo de quadros (1200→1350, 1500→1650, 1800→1950, 2100→2250, 2400→2550): a lógica é
  por quadro (só a hora do dia segue o relógio, pelo `nn::time`). Caminho viável para 60 na tela: geração de quadros (`video.frame_gen`).
  O caminho de verdade (lógica a 60 Hz com metade do passo por sistema, `video.frame_rate = 60native`) está em
  [NATIVE60.md](NATIVE60.md): UI, efeitos, física e animação dos personagens já andam certo; texto e movimento ainda não.
  **Feito: geração de quadros embutida** (`video.frame_gen = builtin`, `NX_FRAMEGEN=1`; `nvn_framegen.cpp`, shaders em
  `tools/shaders/framegen`). O jogo segue a 30 (intervalo 2, velocidade normal) e, quando o quadro real N chega, um
  quadro intermediário entre N-1 e N é feito na GPU e apresentado na hora; N vai meio quadro depois (16,7 ms; o
  replay espera sem a trava da GPU). Movimento por casamento simétrico de blocos (o vetor do próprio quadro do meio,
  sem buracos) sobre a luma em 1/32, 1/8 e 1/4, um vetor por 16x16 px, mediana 3x3; por pixel, o melhor de alguns
  vetores vizinhos, e pixels que não mudaram (HUD) são copiados como estão. Custo medido com timestamps no Intel
  Iris Xe, 1080p: ~1,2-1,8 ms por quadro gerado sem outra carga na GPU (luma 0,15-0,25, movimento 0,5, quadro 0,6-0,9;
  o máximo com a tela toda em movimento).
  Janela: 86% das apresentações entre 14 e 19 ms com o jogo a 30. Sem geração (mostra só os quadros reais) abaixo de
  20 fps, com `NX_PRESENT_INTERVAL=1`, ou se a textura da janela anterior foi reaproveitada.
- **Janela adaptável / resolução nativa** (feito, `nvn_ultrawide.cpp`): os alvos de render do tamanho da tela
  (cadeias 1920x1080 e 1280x720) são refeitos na resolução interna (`video.resolution`, 540p a 2160p ou a da
  janela) e na proporção (`video.aspect`, ou a da janela), ao vivo. As câmeras são as `sead::PerspectiveProjection`
  do jogo (vtable `main+0x4a65608`), ajustadas por um gancho de instrução no `doUpdateMatrix` (`main+37e5958`):
  aspecto mais largo (Hor+) ou campo vertical maior (Vert+). A UI fica em 16:9 no
  meio; telas de menu de fundo cheio ganham faixas laterais (ou em cima e embaixo). Falta: ancorar a HUD nas bordas
  (a raiz do layout `nn::ui2d`).
- **Hardware fraco**: perfil (perf) da cena 3D: o reverb I3DL2 do áudio, renderizado na CPU (no console é o DSP),
  era a função mais cara do processo (~20% das amostras, um núcleo inteiro). `NX_AUDIO_REVERB=0` (presets `intel`,
  `lightest`, `performance`) o desliga: a thread de áudio cai de 26% para 11%. Próximos ganhos: o despacho de
  chamadas indiretas (`nx::call`, 2%), reverb nativo em vez do recompilado, cache de registradores no recompilador.

## Ferramentas de diagnóstico

| Variável | O que faz |
|---|---|
| `NX_ALLOC_TRACE=1` | tamanhos dos alocadores e falhas de alocação, com backtrace (`games/acnh/diag.cpp`) |
| `NX_DUMP_FRAMES=N` | grava a cada N quadros o que foi apresentado em `private/acnh/frames/` |
| `NX_DUMP_TEXTURES=<hex>,...` | junto, grava essas texturas (alvos de render) |
| `NX_DEBUG_TEXTURES=<hex>,...` | descreve essas texturas no log (formato, memória, pool) |
| `NX_TRACE_DRAWS=<n>` | 80 desenhos a partir do n-ésimo: shaders, alvos, texturas |
| `NX_TRACE_TEXTURE_SIZES=1` | tamanhos de armazenamento de texturas ≥ 1 MB |
| `NX_DUMP_SHADER_MISSES=1` | grava shaders que não foram traduzidos (`private/acnh/shader_misses/`) |
| arquivo `private/acnh/dump_threads` | despeja o estado de todas as threads no log |
| `NX_TRACE_SUBMITS=<quadro>` | esboço das listas de comandos submetidas (alvos, clears, scissor, desenhos) |
| `NX_TRACE_RECORD=<quadro>` | `SetRenderTargets`/`ClearColor`/`SetScissor` como o jogo grava, com o chamador |
| `NX_SKIP_CLEAR_AT=<offset do main>` | ignora os clears de cor chamados daquele ponto |
| `NX_SKIP_DEPTH_CLEAR_AT=<offset>` / `NX_CLEAR_ONCE_AT=<offsets>` | ignora os clears de profundidade dali / mantém só o primeiro por alvo e quadro |
| `NX_TRACE_FRAME=<quadro>` | com `NX_TRACE_DRAWS`, só desenhos a partir daquele quadro |
| `NX_PRESENT_INTERVAL=<n>` | força o intervalo de apresentação (1 = 60 fps) |
| `NX_NATIVE60=1` / `NX_NATIVE60_TRACE=1` | lógica a 60 Hz nos sistemas já achados ([NATIVE60.md](NATIVE60.md)) / o que cada gancho vê |
| `NX_FRAMEGEN=1` | geração de quadros embutida (`nvn_framegen.cpp`); com `NX_DUMP_FRAMES`, grava também o quadro gerado (`NNNNN_gen`) e o real anterior (`NNNNN_prev`) |
| `NX_SNAP_FOLLOW=<hash do fs>@<desenho>` | captura o alvo depois de cada desenho seguinte |
| `NX_INPUT=quadro:Botão,...` | aperta botões (`A`...`ZL`/`ZR`, ou `LUp`/`LDown`/`LLeft`/`LRight`: analógico) em quadros definidos |
| `NX_FAST_UNTIL=<quadro>` | 60 fps até aquele quadro, o ritmo do jogo depois (introdução de testes em metade do tempo) |
| `NX_TIME_OFFSET=<segundos>` | desloca o relógio do jogo (viagem no tempo) |
| `NX_USER_NAME=<nome>` | nome do usuário do sistema (padrão: o do computador) |
| `NX_MOVIE_TRACE=1` / `NX_MOVIE_STATS=1` | cada chamada ao decodificador do filme / estado dos buffers a cada 2 s |
| `NX_AUDIO_OUT_STATS=1` | buffers do `AudioOut` (anexados, tocados, devolvidos) a cada 2 s |

# 60 Hz nativo (lógica do jogo a 60 por segundo)

## Caminho atual: SpeedFix (24/10/2026)

`tools/setup_60fps.sh` instala o **ACNH 60 FPS + SpeedFix** de tmouh (https://github.com/tmouh/acnh-60fps,
CC BY-NC 4.0, baixado para `private/`, nunca incluído no repositório): 6.417 palavras de código e dados para o `main`
3.0.2, 1.437 valores do `romfs/Pack/StaticParam.pack` e três tabelas dos moradores, cada um medido contra o jogo a
30 fps. O recompilador aplica o patch como segunda versão de cada função que ele muda (`recomp --variant`): 974
funções ganham dois corpos e um despachante (`nx_variant_on`, ligado por `NX_SPEEDFIX`), e 866 rotinas que o patch
põe em espaço livre viram funções próprias; o carregador escreve as palavras na imagem do módulo quando ligado. Um
só binário roda a 30 ou a 60 (`video.frame_rate = 60`; sem o SpeedFix instalado, "60" fica em 30). Os ganchos de
`NX_NATIVE60` (abaixo) não são usados nesse modo.

Medido no M1 a partir de um save do dia 2: andar 3 s para a esquerda (90 quadros a 30, 180 a 60) leva o personagem
ao mesmo ponto nos dois modos. Velocidade do jogo = taxa de quadros: perto da barraca 60 fps; na beira do rio
~44 fps (o replay do NVN leva 14 ms por quadro ali, com ou sem o SpeedFix), e o jogo fica mais lento junto. Próximo
passo: baratear o replay.

Notas da integração: no `.pchtxt` os deslocamentos já são do módulo (`offset_shift` é o que se soma para o IPS); e
as rotinas do patch se encadeiam (uma passada da análise por elo: 51 passadas), daí o limite de 400.

## Caminho anterior: ganchos (NX_NATIVE60)

Objetivo: apresentar a 60 fps com o jogo andando na velocidade certa, fazendo a lógica rodar 60 vezes por segundo com
metade do passo por quadro (e não inventando quadros intermediários, como o `video.frame_gen`). **Experimental e
incompleto**: `video.frame_rate = 60native` no `config.ini` (liga `NX_NATIVE60=1` e o intervalo de apresentação 1;
não aparece nas telas de configuração ainda). Sem esse valor nada muda: os ganchos só agem com `NX_NATIVE60=1`, e o
modo de 30 fps segue igual.

Endereços do `main` 3.0.2; no 3.0.3 a maioria está 0x10 adiante (`games/acnh/hooks_303.txt`, [ACNH.md](ACNH.md),
"Updates"). Contexto em [STATUS.md](STATUS.md) ("60 fps").

## Estado

| Sistema | Estado | Evidência |
|---|---|---|
| Animações de layout da UI (balão de fala, seta "continuar", teclado, telas) | ✅ metade por quadro | seta do balão: período de 20 quadros a 30 fps, 40 a 60native (mesmo tempo real); oscilação do balão: 32 → 64 |
| Efeitos (`nn::vfx`) | ✅ metade por quadro | taxa por grupo vista = 1 por quadro; não medido em imagem |
| Física Havok | ✅ passo de 1/60 s | passo visto = 1/30 s por quadro; não medido em imagem |
| Animação de modelos pelo grupo do gsys (`main+38b2b00`) | ✅ metade, mas poucos modelos usam | poucos milhares de chamadas na abertura inteira |
| Animação esquelética dos personagens (Timmy, Tommy, jogador, NPCs) | ✅ metade por quadro | o `AnimFrameCtrl` dos personagens passou de +1 para +0,5 por quadro; período do movimento parado do Timmy: 32 quadros a 30 fps, 64 a 60native |
| Digitação das falas | ❌ por quadro | "Let us be the first...": ~1,4 caractere por quadro tanto a 30 quanto a 60native |
| Movimento do jogador | ❌ por quadro | na ilha (save `private/play/saves/day2_morning`), andar para baixo 192 quadros a 60native leva o personagem da barraca até a praia; 96 quadros a 30 fps só até a pedra ao lado: a distância por quadro é a mesma |
| Câmera, temporizadores em quadros, parâmetros de dados | ❌ | não medidos (a câmera segue o personagem, que já anda 2x) |

Resultado prático hoje: a 60native a UI, os efeitos, a física e as animações dos personagens andam na velocidade
certa, mas a digitação, o deslocamento dos personagens e a lógica de jogo (temporizadores, IA) ainda andam 2x mais
rápido. Não serve para jogar ainda.

## Como o jogo anda um quadro

O ACNH é feito sobre sead + agl + gsys (o mesmo conjunto de Splatoon 2), com o `sead::GameFrameworkNx`:

| Endereço | Papel |
|---|---|
| `main+37b8bf0` | laço principal da thread `sead::MainThread`: `for (;;) this->procFrame_()` (vtable+0xe0) |
| `main+37b8c14` | `procFrame_`: `procDraw_` (vtable+0xe8), `procCalc_` (vtable+0xf0), espera/apresentação (vtable+0x70), tempos (`fw+0xf0`, `+0xf8`, `+0x130`) |
| `main+37b8d1c` | `procDraw_`: grava os comandos do quadro |
| `main+37b90e4` | `procCalc_`: `ControllerMgr` (`main+37bf0f0`) e depois a árvore de métodos inteira (`sead::MethodTreeNode::call`, `main+37bbee0`, recursiva): **toda a lógica é uma chamada por quadro** |
| `main+37e3300` | laço de mensagens de `sead::Thread` (várias threads de cálculo: UI em `main+3c5bb1c`, tarefas); a de apresentação apresenta em `main+37b91a4` |
| `main+3a09200` | nó `VfxMgr::calc` da árvore |

Não existe um "delta" central: a árvore de cálculo não recebe tempo. Cada sistema guarda o seu próprio passo por
quadro, quase sempre a constante 1 (quadros) ou 1/30 (segundos). Os que têm o passo num lugar só estão abaixo; o
resto conta quadros espalhado pelo código e pelos dados.

## Ganchos de instrução

Para mexer num registrador no meio de uma função gerada, o recompilador ganhou `--hooks <arquivo>`: antes de cada
instrução listada o código gerado chama `nx_hook(c, pc)` com os registradores em dia no contexto (o cache de
registradores trata a chamada como qualquer outra), e a função registrada com `NX_HOOK(offset, fn)` (`runtime.h`)
pode ler e trocar qualquer um. A lista do ACNH é `games/acnh/hooks.txt`; os tratadores ficam em
`games/acnh/native60.cpp`. Depois de mudar a lista, regenerar só as funções que contêm as instruções (`--only
<função>` num diretório à parte) e trocar o texto delas nos `code_*.cpp`. **Uma regeneração completa sem `--hooks`
apaga os ganchos** (o modo 60native deixa de fazer efeito, sem erro). `NX_NATIVE60_TRACE=1` registra no log o que
cada gancho vê (os primeiros acertos e um a cada 4096).

| Gancho | Função | O que é | Passo visto a 30 fps |
|---|---|---|---|
| `main+3a1d614` | `main+3a1d5e0` | atualização das animações de um layout: passo = `this->vtable[0x128]()`, depois `Animate()` e `UpdateFrame(passo)` (vtable+0x18) de cada animador da lista `this+0x70` | 1 |
| `main+38b2b34` | `main+38b2b00` | animações de um grupo de modelo do gsys sobre `nn::g3d`: cada slot recebe passo = velocidade (`grupo+0x30`) × passo da cena (`cena+0x55bc`, lido em `main+38d6ee0`) e avança o `AnimFrameCtrl` (quadro +0, início +4, fim +8, passo +0xc, política +0x10, dados +0x18) | 1 |
| `main+3a09404` | `main+3a09200` | `nn::vfx::System::Calculate` (`main+23de30`) por grupo (64) com a taxa do grupo (`VfxMgr+0x3f4 + 4·grupo`, 0 em pausa) | 1 |
| `main+3e01f08` | `main+3e01a40` | passo dos mundos Havok (`main+4127c0`, marcador "TtPreStepSignal"), em segundos | 1/30 |
| `main+3dc3298` | `main+3dc3210` | grafo de animação dos personagens: cada nó raiz é avaliado com o passo (`main+3dbf770`), o que avança os quadros dos nós, e as entradas (quadro, peso) resultantes vão para os slots do modelo (`main+3dc10e0` → tocar slot `main+3751af8` / quadro do grupo `main+38afe40`) | 1 |

## Pistas para o que falta

- **Personagens** (resolvido; como foi achado): o `AnimFrameCtrl` deles é o embutido no `AnimObj` do `nn::g3d`
  (`AnimObj+0x10`, política Loop), avançado 1 por quadro sem passar pelas políticas no avanço; os únicos que
  escrevem os quadros por quadro são `main+3751af8` (tocar slot) e `main+38afe40` (quadro do grupo), chamados só de
  `main+3dc13a4`/`main+3dc17c4` (aplicação das entradas do grafo, `main+3dc10e0`), que vem de `main+3dc3210`, que
  recebe o passo. `main+3b26e80` (camadas em entradas de 0x14 bytes) não roda na abertura.
- **Texto**: a "LetterAnimControl" (`main+3a102c0`: `this+0x50` pausa, `this+0x54` posição, `this+0x40`
  velocidade, `s0` passo) não é a digitação das falas (não roda na abertura). Os balões são `TextBoxEx` (classe do
  jogo sobre `nn::ui2d::TextBox`, vtable em `main+474d7c0`). A digitação não chama `SetString` por quadro
  (contagem dos chamadores de `main+28a300`/`main+28a714`: só textos de relógio e de interface), nem passa pelos
  animadores de layout. A taxa medida (~1,4 caractere por quadro = 42 por segundo a 30 fps) sugere uma conta em
  segundos (velocidade × 1/30); próximo passo: os lugares de 1/30 abaixo, ou o `PerCharacterTransform` do
  `TextBox` (as letras surgem com animação).
- **Diagnósticos usados** (ganchos só de rastreio, tirados da lista final): `nn::ui2d::AnimTransformBasic::Animate`
  (`main+25e7b8`, depois do teste de habilitado) guardando o quadro anterior de cada transformação e mostrando a
  cadeia das que avançam 1 inteiro (achou `main+3a1d5e0`); o mesmo no `SkeletalAnimObj::Calculate` (entrada,
  `main+2a5454`), com os campos do controle de quadro; contagem dos chamadores (lr) das políticas do `nn::g3d`, de
  `main+3751af8`/`main+38afe40` e dos métodos do `TextBox`; uma cadeia (backtrace) na entrada de `main+3dc10e0`.
- **Uma regra útil**: as constantes 1/30 que são "segundos por quadro" (passo de integração, ou quadros contados
  pelo jogo → segundos) ficam certas com 1/60 a 60 Hz; as que convertem tempos de dados (feitos a 30) não. Cada
  lugar precisa ser olhado antes de trocar; ex.: `main+16548e0` integra posições de uma corrente com vento usando
  1/30 como passo (`main+16548ec`, `w22` em `main+1654948`).
- **Movimento do jogador e dos NPCs**: parâmetros por quadro (`mWalkSpeedMax`, `mRunSpeedMax`,
  `mWalkRunSpeedChaseStep`, `mDashSpeedChaseStep`, `mWalkRunAngleYSpeedMax`...). O objeto de parâmetros dessas
  (criado em `main+225e000`) tem nome em `+0x8`, valor em `+0x28` e é registrado pela vtable+0x140 do dono.
  Velocidades por quadro precisam de metade, acelerações de um quarto, contadores do dobro.
- **Contadores em quadros nos dados**: `ActionFrame`, `DelayMaxFrame`, `WaitFrame`, `EmoticonActionAutoEndFrame`...
- **Constantes de tempo no código**: 21 lugares com 1/30 (`mov`/`movk 0x3d088889`), 7 com 1/60 e 202 com
  `fmov #30.0` (segundos ↔ quadros). Os de 1/30: `main+2b7b2c` (tabela por tempo), `main+604800` (taxa 0,1 ou 1/30),
  `main+370eab8` (quadros → segundos), `main+620300` (passo de um objeto em segundos), `main+6ba008` (passo da rede,
  `main+3e26900`), `main+3e02b5c` (assentamento inicial do Havok), `main+3c88cc0`, `main+2179660`, `main+14b9d5c`
  (velocidade × 1/30), `main+16548e4`/`main+16547e0`, `main+1654ab8`/`main+1654c58`, `main+28aea48` e `main+28c3258`
  (contador → segundos), `main+2f0a410`/`2f0a420`, `main+28eb8e0`/`28eba44`, `main+2ef7fd0`/`2ef7fe4` (arredonda
  tempo para quadros de 1/30).
- **Câmera**: suavização por quadro (não medida isolada: segue o personagem, que ainda anda 2x).
- **Teste na ilha**: `private/acnh/n60/live.sh` sobe o jogo com o save `day2_morning` e entrada ao vivo
  (`input_live`); A na tela de título e ~2.700 quadros depois o personagem está controlável na frente da barraca.

Estimativa do que falta para jogar a 60native: a digitação é um achado do mesmo tipo dos de cima (dias de
trabalho, com os diagnósticos prontos). Movimento, câmera, temporizadores e parâmetros de
dados são o grosso: centenas de lugares, cada um a classificar (velocidade, aceleração, contador) e testar na ilha,
que exige um save pronto (a abertura não chega lá dentro de uma execução de teste). Semanas a meses; a geração de
quadros (`video.frame_gen`) continua sendo o caminho para 60 na tela hoje.

## Medição

Mesma entrada com os números de quadro dobrados a 60 (`NX_INPUT`); o arquivo `dump_frames` (`"150 2"` a 30 fps,
`"150 4"` a 60native) despeja um trecho denso de quadros; a periodicidade de um recorte da imagem (distância média
entre recortes k despejos afastados) dá o período de cada movimento em quadros: o que estiver certo dobra a
60native. O carregamento segue o relógio, então imagens de mesmo número de quadro não se correspondem; e nesta
máquina (compartilhada, carregada) o jogo não sustenta 60 fps (20 a 35), por isso tudo é medido em quadros, não em
tempo de relógio.

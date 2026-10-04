# Triagem dos feedbacks externos (GPT + OPUS) — o que muda no plano

Data: 2026-10-04. Fontes: `GPT_FEEDBACK.md` e `OPUS_FEEDBACK.md` na raiz,
ambos revisando o commit `75ea3a1` com a árvore limpa. Nenhum dos dois
alterou código de produção, testes, manifests ou histórico; o GPT rodou a
suíte e probes sintéticos em `private/gpt-feedback-validation/` (ignored).
Verifiquei que esse diretório existe e tem os artefatos citados
(`run-probes.ps1`, `generate_repro.cpp`, `generated_repro.hpp`,
`run_repro.cpp`, `runtime_contract_probe.cpp`, logs de CTest/Python).

## O que eu verifiquei por conta própria (não só acreditei)

Lidos no código atual, antes de planejar:

- `tools/gt4boot/main.cpp:59-63`: VIF0/VIF1/GIF completam via INTC 4/5/9.
  Confirmed — o roteamento que os dois feedbacks apontam está lá.
- `src/ee/kernel.cpp:1241-1276` (idle) e `:1279+` (por serviço): COUNT/COMP
  de 32 bits, overflow em `0xFFFFFFFF`, EQUF levantada todo frame idle sem
  olhar COMP. Confirmed — o contrato de timer que o GPT descreve está lá.
- `tools/gt4translate/main.cpp:818-827`: o `jr ra` emite o delay slot antes
  de ler `ra`. Confirmed.
- `src/ee/driver.cpp:52-61`: `PC == RA` significa retorno, com o próprio
  comentário admitindo a ambiguidade. Confirmed.
- `src/ee/kernel.cpp:1361-1376`: o frame do handler preenche `a0`, `gp`,
  `sp`, `ra` e nunca `a1` (argumento de registro) nem `a2`. Confirmed — o
  bug de argumento do OPUS está lá.
- `src/ee/kernel.cpp:80-98` (`dispatch`) + `:123-136` (`block_current`):
  quando ninguém está pronto, retorna `false` sem zerar
  `current_thread_id_`. Confirmed — o stale-id do OPUS está lá.
- `src/ee/kernel.cpp:2390`: "Anything else answers an empty result."
  Confirmed — o fallback silencioso que o OPUS cita viola a regra do
  `AGENTS.md` ("no silent fallbacks").
- Desmontei `0x004ab6d8` e `0x005aeb68` (12 palavras cada): as primeiras
  instruções batem literalmente com o OPUS (`daddu a3,a0,zero` + base
  `0x70002000` + tabela `0x00639dc8`; `syscall -0x2f`, `daddu s0,v0,zero`,
  `beq s0,a0`). High confidence no restante do disassembly citado (não
  re-desmontei as ~150 instruções de cada).
- Portas rodadas por mim: CTest 50/50, Python 73 (67 rodam, 6 pulam).

Não verifiquei de forma independente: os offsets IRX/SID um a um, os bytes
do checkpoint (`0x70002079` etc.), os 10 call sites de `0x005aeb68`, o
ambiente PCSX2/PINE local, nem o contrato de 16 bits contra hardware real
(as referências citadas — PCSX2, PS2tek, PS2SDK, ps2autotests — não foram
re-auditadas aqui). Esses itens entram abaixo como High confidence ou
Hypothesis, não Confirmed.

## Achados convergentes (os dois dizem, eu confirmei o código)

| # | Achado | Meu veredito |
|---|---|---|
| 1 | Timer COUNT/COMP 32 bits + overflow 32 bits; EQUF todo frame; sem W1C; idle e serviço em máquinas separadas | Bug de contrato Confirmed; efeito no stall Hypothesis |
| 2 | DMA VIF0/VIF1/GIF via INTC 4/5/9 com TIE como enable geral; INTC 9 é Timer0, GIF não tem causa INTC | Bug de roteamento Confirmed; efeito no stall Hypothesis |
| 3 | `jr ra` lê `ra` depois do slot (GPT provou por execução via emitter real) | Bug Confirmed (execução sintética); reachability no GT4 Unknown |
| 4 | Saída do módulo por heurística `PC == RA`; ERET aplicado vira `InstructionStop`; BREAK com PC==RA vira loop (GPT provou por execução) | Bug de interface Confirmed; reachability Unknown |
| 5 | Replies RPC zerados para `(SID, função)` sem contrato | Risco Confirmed (viola regra própria); qual reply travou o boot Unknown |
| 6 | Comparador cobre contexto + RAM principal, não scratchpad/kernel/dispositivos; checkpoint sem identidade semântica | Lacuna Confirmed |
| 7 | Relógio de 1 ms por serviço distorce tempo guest (243M serviços ≈ 67 h virtuais) | Cálculo Confirmed; quanto distorce o gate atual Unknown |

## Achados só do OPUS (específicos e testáveis)

| # | Achado | Meu veredito |
|---|---|---|
| 8 | Handler `0x004ab6d8` (DMAC 0/1/2) nunca executou; Thread 3 presa na wait-list VIF1 (`0x7000207c`), busy bytes `0x70002079`/`0x70002085` em 1 | High confidence (disasm inicial bate; dump do checkpoint não re-lido por mim) |
| 9 | Produtor do anel da Thread 2 é `0x005aeb68` (wrapper `iWakeupThread` seguro: só usa o anel quando `self == alvo`); 10 call sites mapeados | High confidence (cabeça do disasm bate; call sites não re-varridos) |
| 10 | Mapa dos 24 SIDs: `0x80001300/1c/1e/1f` = libpad2 já bound (M35 esperou PADMAN errado); `0x80000400` = MCSERV/libmc; `0x046d046d` = volante Logitech (responder `0x046DC298` finge um Driving Force Pro plugado); `MPG1/MPG2/PBGM/VOIC/SPUP` zerados | High confidence como mapa a verificar item a item; as correções de identidade são a parte mais barata de confirmar (strings + offsets nos IRX do ISO pinado) |
| 11 | `a1` do handler perdido + `current_thread_id_` stale no idle | Bug Confirmed (lidos no código) |

## O que isso muda no planejamento

1. **A hipótese "falta um evento originador desconhecido" deixa de ser a
   única explicação.** Existem bugs no modelo com endereço e linha que
   podem, sozinhos, produzir o estacionamento — sobretudo o roteamento
   DMA (Thread 3 presa com bytes ocupados) e o tempo (delays e compares
   calculados sobre outro relógio). Repetir o mesmo estado por mais tempo
   não vai descobrir isso. Confirmed (bugs) / Hypothesis (qual deles
   prende a main).
2. **Ordem nova: contratos primeiro, subsistemas depois.** Timer → DMA +
   interrupt → `jr`/`ERET` + saída explícita → comparador + identidade de
   checkpoint → prefixo novo desde a entrada → só então caçar o produtor
   da main com âncora independente (PCSX2), e só então escolher o próximo
   serviço (MCSERV, LGDEV, DBCMAN, PDISTR) pelo disassembly dos IRX.
3. **Checkpoints antigos viram forense.** Qualquer mudança semântica de
   tempo invalida a retomada das marchas antigas como evidência de
   comportamento; a referência nova nasce de um prefixo fresco com
   checkpoints identificados.
4. **Nada de tráfego fabricado.** Resposta RPC desconhecida passa a parar
   com contexto (telemetria/histograma + modo estrito), nunca zero
   silencioso — alinhado à regra que já tínhamos.
5. **Cada correção leva sua regressão permanente.** O verde atual
   (50/50 + 73) coexiste com os defeitos porque a suíte não cobre esses
   contratos; um teste que só fixa o comportamento antigo (ex.: DMA sem
   TIE em silêncio) é atualizado com a evidência externa.
6. Propostas de processo dos revisores (pausar lições até M31; compactar
   `STATUS.md` para <200 linhas com arquivo do histórico): aceito a
   direção, deixo a decisão final para o dono — registro aqui para não se
   perder.

## Próximas fatias propostas (ordem)

- 63 (esta): triagem + arquivar os dois feedbacks no git. Docs-only.
- 64: DMA 0/1/2 + `a1`/`a2` no frame + `current_thread_id_` no idle vazio;
  testes unitários; aceite: `0x004ab6d8` executa e limpa os bytes ocupados.
- 65: contrato do timer (16 bits, W1C, compare/overflow, idle e serviço na
  mesma máquina) + revisar assertions antigas; prefixo novo, checkpoints
  antigos marcados incompatíveis.
- 66: `jr ra` + motivo de saída explícito + ERET, com fixtures
  emitter → módulo → driver.
- 67: telemetria RPC (histograma por `(SID, função)` + modo estrito) +
  comparador ampliado + identidade de checkpoint.
- 68+: caçada causal ao predicado da main num prefixo fresco com âncora
  PCSX2; depois o primeiro serviço guiado pelo disassembly dos IRX
  (candidatos: MCSERV `mcGetInfo`, LGDEV "sem volante", DBCMAN/libpad2,
  PDISTR/PDISPU2).

Nada acima promove diferença de modelo a causa do stall sem o experimento
que a ligue ao predicado da main — a mesma disciplina que os dois
revisores pedem.

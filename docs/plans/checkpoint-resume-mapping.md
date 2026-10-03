# Checkpoint/retomada do `gt4boot` — mapeamento (planejamento, não implementado)

Data: 2026-10-03. Origem: levantamento somente-leitura por subagente de
exploração (nada implementado, nenhuma mudança no comportamento).
Motivação: cada experimento hoje paga o prefixo inteiro (corridas de
13–40 min); checkpointar numa fronteira de serviços e retomar dali
aceleraria as iterações em ~20×. Registrado aqui para não se perder;
a implementação, se aprovada, segue em 3–5 fatias próprias com testes.

Premissa do desenho: snapshot capturado **numa fronteira de serviço**
(após `handle_syscall`/`advance_time`, com
`Interpreter::pending_transfer() == false`), só captura/restauração de
estado, sem mudar semântica do modelo.

## 1. Inventário do estado a salvar

- `GuestState`: o par `save_registers()`/`restore_registers()` já move
  todo o contexto (`RegisterContext`: GPRs baixos/altos, FPRs, HI/LO
  0/1, acumulador/FCR31, shift cache, CP0, VU0 completo, pc) — trivial.
  Sem RNG do host no modelo (tempo = `advance_service_time`), o que
  torna o checkpoint bit a bit plausível.
- `GuestMemory`: RAM 32 MiB + scratchpad 16 KiB + bloco GS (~32 MB por
  checkpoint); geometria trivial. **Problema**: janelas MMIO guardam
  `std::function` capturando `this` (não serializáveis) — reconstruir a
  topologia (`BootDevices` + remapeamentos, igual ao `make_boot_state`)
  e serializar só os **conteúdos** dos bancos.
- `Kernel`: tudo serializável exceto dois ponteiros — threads (+
  contextos), semáforos, handlers INTC/DMAC, deferred calls, fila de
  interrupções (obrigatória), tabela de syscalls, OSD, regs SIF, tabela
  de servidores RPC, `sif_reboot_pending_`, relógio de serviço
  (`service_ticks_`, `service_timer_remainders_[4]`, `idle_interrupts_`),
  handles de arquivo, cache PRTS **com cursor**, contadores
  (`next_*`). Ponteiros de disco e `service_table_`/`Module`: não
  serializar — religar/reconstruir no load (discos reabertos com hash
  verificado).
- `BootDevices`/bancos: conteúdos via `register_value`; callbacks
  reconstruídos na mesma ordem. Caches lazy (`Iso9660Image`,
  `Gt4Volume`, `DiscSectors`): reabrir, não reaproveitar.
- Driver/interpretador: `transfer_pending_`/`transfer_target_` (só com
  `pending == false`), contadores `DriverStats` (continuam de N),
  limites. `Module`/`ServiceTable`: reconstruídos (stateless).

## 2. Cinco maiores riscos (e verificação de cada)

1. Topologia MMIO reconstruída errada → varrer bases conhecidas
   (a lista do `--threads`) + digest FNV da RAM contra corrida íntegra.
2. Snapshot em estado transitório → save só pós-`advance_time` com
   assert; prova = diferencial `states_match` retomada vs. do zero.
3. Relógio de serviço/restos fracionários → teste com COMP programado.
4. Cursor PRTS/handles reemitidos → teste no meio da carga da fonte
   (3 leituras + 7 copy-outs) exigindo bytes idênticos.
5. Identidade das entradas (ISO/CORE/binário) → header com hashes
   pinados, recusa em mismatch; `--compare-interpreter` a partir do
   checkpoint.

## 3. Ganchos (esboço)

Serializadores por dono; save em `main.cpp` ao atingir N serviços
(novo código de saída); load via `--resume <arquivo>` (+ `--disc`
obrigatório) reconstruindo tudo e aplicando o snapshot por cima;
verificação `--verify-checkpoint N M` (limite N+M do zero vs. N+M via
retomada, exige boundary + `states_match` iguais). Formato: diretório
por checkpoint sob `private/checkpoints/` ou `generated/checkpoints/`
(ignorados; RAM tem bytes do jogo — nunca no Git).

## 4. Esforço e ganho

3–5 fatias (contexto+memória; Kernel; bancos+flags; verificação ponta a
ponta com decisão numerada). Ganho por iteração ~(N+M)/M (~20× no caso
atual: checkpoint nos 41,9 M e iterações de 1–2 M). Pré-requisitos:
pontos N canônicos, espaço ignorado, header com hashes, `main` verde
(sem tocar semântica — o diferencial existente continua passando).

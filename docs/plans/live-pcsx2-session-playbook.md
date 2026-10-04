# Playbook da sessão live PCSX2 (conjunta: dono + agente)

Data: 2026-10-04. Objetivo: executar a lista de compras final da
slice 87 (S0 calibração → S1 estágios → S2 watchpoint → S3 invocador)
para destravar H1/H2′ e o candidato F. Fonte técnica:
`docs/reverse-engineering/slice87-e2-livelist.md` §2 (lê-la antes).
Nada aqui muda o modelo; é só observação.

## Regras da sessão (valem do início ao fim)

- Controle positivo PRIMEIRO em cada etapa; sem ele, nenhuma leitura
  causal vale. Se S0 falhar, a sessão para ali.
- Sem instalações permanentes sem necessidade documentada.
- Nada de payload do jogo no git: capturas, logs e dumps ficam em
  diretório temporário aprovado, nunca commitados.
- Ao final: fechar o emulador e conferir `git status` limpo.
- Tripwires armados: `0x0086CBBC`, B+`0x00`, tabela de gerações
  (qualquer carimbo `0x23x` ou `+0x3C != 0` falsifica o endpoint parado).

## FASE 0 — preparação (agente, sozinho, antes de chamar o dono)

- [ ] Confirmar ISO `Gran Turismo 4 (USA) (v2.00).iso` na raiz
  (5.314.478.080 bytes) + `private/fingerprint-check/CORE.GT4`.
- [ ] Confirmar `private/pcsx2/pcsx2-v2.9.94/pcsx2-qt.exe` + BIOS
  `private/pcsx2/bios` (linha 90001-v18, v02.30).
- [ ] Fazer backup de `private/pcsx2/pcsx2-config/PCSX2.ini`.
- [ ] Confirmar que a porta PINE 28011 está livre.

## FASE 1 — setup (juntos, ~10 min)

1. Dono: abrir `PCSX2.ini`, trocar o caminho de BIOS
   (`F:\Games\PS2\BIOS`, inexistente) para `private/pcsx2/bios` e
   salvar. Agente: confere o diff do ini.
2. Dono: abrir `pcsx2-qt.exe`, selecionar a BIOS 90001-v18 e o ISO do
   GT4. Agente: registra versão/hashes visíveis.
3. Dono: testar se os savestates v2.9.93 carregam no emu v2.9.94
   (B3: skew testado). Anotar o resultado — se não carregar, a sessão
   vira boot-do-zero (mais longa, mesmo roteiro).
4. Dono: checar PINE (porta 28011 responde?). Agente: testa leitura
   de registradores via PINE.

## S0 — calibração, controle positivo OBRIGATÓRIO (juntos, ~15–30 min)

1. Dono: dar boot no ISO e jogar/esperar até o menu (fase do slot 9).
2. Dono: extrair RAM do EE + registradores (PINE ou savestate).
3. Agente: confere o PASS, tudo junto:
   - slot A = `0x11E`, B+`0x00`/`+0x34`/args = `0x10D`/`1`/quatro vivos;
   - tabela de gerações com 22×1 e 63/71/75 = 0;
   - registradores offline == captura PINE + janela de texto
     hasheando o payload pinado.
4. **Se qualquer item falhar: STOP.** A sessão está cega; nada do
   resto vale. Voltamos outro dia.

## S1 — savestates estagiados (juntos, ~20 min)

Salvar (e em cada um ler A, B+`0x00/0x34/0x38/0x3C/0x40/0x80s`, C,
gerações, flag, gate `0x00616F24`):

- (a) init `0x00101938`;
- (b) passada BUILD `0x005BC4C8` / flag `0x0088D7C8` — controle: tem
  que mostrar o censo dos 370 BUILDs, senão o tracer está cego;
- (c) gate parado `0x00548660` (W_B dentro do WaitSema);
- (d) menu `0x00568B94`.

Agente: monta a tabela estágio × valores na hora. Vereditos na hora:
qualquer estágio com ids frescos + waiter antigo parado = ordem
teardown-vs-build (braço H1); hit de TEARDOWN em qualquer estágio =
invocador achado (G1, sessão vence cedo).

## S2 — watchpoint do delete (só após S0 verde, ~15 min)

1. Dono: armar break no topo do loop do worker `0x80004B00` com
   espera > 0 (ou watch nos writes do TCB do waiter parado).
2. Primeiro registrar um wake natural do ciclo de delay (caminho
   `0x005AEF58`/iSignalSema) como controle do caminho de wake.
3. Depois ler o v0 de resume do wake-por-delete. Esperado: `-2`
   (aposta explícita; um `-1` REABRE a E2 na hora).

## S3 — hunt do invocador (se S1/S2 não fecharem G1, ~20 min)

Break/log nas entradas `0x005485E0` (publisher), `0x00548A00`
(TEARDOWN), clones tardios `0x0060FF18/0x0060FFD0/0x00610000/0x00610050`,
logando ra do chamador; controle = os 370 BUILDs do boot aparecendo.
Bound até o menu: miss é resultado (D6 segue com bounds maiores),
não falha.

## Encerramento (juntos, 5 min)

1. Dono: fechar o emulador.
2. Agente: conferir `git status` limpo, arquivar a tabela de
   resultados na evidência da fatia correspondente e atualizar STATUS.
3. Decisão na hora, com os números na mesa: H1/H2′ fechado ou próximo
   experimento; DRAFT 0037/0038 promovido ou não.

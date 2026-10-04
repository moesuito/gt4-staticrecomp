# GPT_FEEDBACK — revisão técnica de GT4Recomp

**Data da revisão:** 2026-10-04.  
**Workspace:** `C:\Antigravity\gt4-staticrecomp`.  
**Commit analisado:** `75ea3a1b523d2c070ed56b7a605b6b9de1a3c270`, branch `main`.  
**Destinatário:** agente engenheiro que mantém o projeto.  
**Escopo autorizado:** inicialmente leitura e criação deste arquivo; posteriormente o dono autorizou testes para enriquecer o feedback. Foram executados builds incrementais, a suíte existente e probes sintéticos em diretório ignored. Não houve commit, push, alteração de código de produção, atualização de manifests ou instalação de ferramentas. A única mudança fora das áreas ignored continua sendo este documento.

Este documento é uma revisão independente de arquitetura e de evidências, com inspeção estática e validação experimental de caminhos específicos. Não é uma certificação de correção do projeto inteiro. Resultados históricos são identificados como registros do worker; a **seção 29** contém os testes executados nesta revisão e promove alguns achados estáticos a resultados reproduzidos.

**Atualização experimental:** CTest 50/50 e Python 73 casos, 6 skips, reproduzidos; inputs revalidados. O emitter real confirmou a divergência de `jr ra`, o driver confirmou a parada indevida depois de ERET e a confusão de BREAK quando PC==RA. Probes pela API real confirmaram largura/flags/wrap do timer e a dependência incorreta de completion DMA em TIE. Esses resultados reforçam a prioridade dos contratos; a causa do bloqueio do GT4 continua não demonstrada.

## 1. Recomendação principal

Eu manteria a direção de **EE recompilada estaticamente, estado guest explícito e runtime próprio**, mas mudaria a prioridade imediata da investigação. Antes de ampliar o modelo de IOP, adicionar input ou implementar gráficos, eu fecharia contratos pequenos de hardware e de execução que a revisão encontrou inconsistentes.

O achado mais relevante é o timer: o modelo conserva `COUNT` e `COMP` como valores de 32 bits e reconhece overflow no limite de 32 bits, enquanto as referências independentes consultadas modelam os contadores EE com largura lógica de 16 bits. A biblioteca de tempo do próprio jogo, já reconstruída pelos slices M32, combina um contador de overflows com os 16 bits baixos de `COUNT`. A incompatibilidade torna necessário reexaminar as conclusões sobre esperas que atravessam wrap, disparos raros e timers considerados saudáveis.

Também há uma diferença concreta no caminho DMA: VIF0/VIF1/GIF estão conectados ao INTC com causas 4/5/9 e a completion depende de `CHCR.TIE`. Essas completions pertencem ao DMAC, canais 0/1/2; a causa INTC 9 é Timer0. Existem ainda uma leitura tardia de `ra` no `jr ra` traduzido e uma interface módulo/driver que não transporta explicitamente o motivo da parada.

**Essas diferenças não provam a causa do bloqueio atual.** Provam que há contratos verificáveis que precisam ser corrigidos ou explicitamente delimitados antes de atribuir o bloqueio apenas a tráfego IOP ausente. Uma execução longa que estaciona deterministicamente pode estar repetindo, com muita fidelidade, um estado produzido por um modelo incorreto.

Minha sequência seria:

1. Confirmar e testar o contrato do timer, incluindo largura, flags, compare, acknowledge e avanço em múltiplos eventos.
2. Corrigir a semântica de retorno e tornar explícito o motivo de saída dos módulos; testar também `ERET`.
3. Separar completion DMA, status pendente, máscaras e entrega de handlers; corrigir o roteamento dos canais usados.
4. Ampliar a comparação de estados e identificar a versão semântica dos checkpoints.
5. Executar um prefixo novo desde a entrada, sem herdar o tempo ou as filas do checkpoint antigo.
6. Comparar âncoras semânticas com PCSX2 e localizar a primeira diferença causal até a condição em que a main espera.
7. Escolher a próxima peça de IOP, input, VIF/VU ou GS a partir dessa diferença.

Esse plano preserva o investimento existente. Ele procura reduzir a quantidade de suposições acumuladas antes de investir em subsistemas maiores.

## 2. Vocabulário de confiança usado aqui

| Rótulo | Significado nesta revisão |
|---|---|
| **Confirmed — código** | O comportamento descrito está diretamente no código do checkout analisado. Não implica execução nova. |
| **Confirmed — registro** | O resultado foi documentado no histórico/evidência do projeto; não foi reproduzido nesta sessão. |
| **Confirmed — execução** | Uma fixture/probe ou suíte foi efetivamente executada nesta revisão, com método e resultado na seção 29. |
| **Confirmed — referência** | Uma fonte primária externa consultada contém o contrato ou implementação descritos. |
| **High confidence** | A combinação de código, cálculo e referências sustenta fortemente a conclusão, mas falta o experimento específico. |
| **Hypothesis** | Explicação causal plausível que ainda precisa de teste discriminante. |
| **Unknown** | Não há evidência suficiente para estabelecer comportamento ou causalidade. |

Importante: uma diferença confirmada no modelo e uma hipótese sobre o seu efeito no boot são afirmações distintas. Um teste que passa depois de uma alteração também não estabelece, sozinho, equivalência com o console.

## 3. Snapshot atual: o que os slices novos mudaram

### 3.1 Histórico recente

O estado foi relido depois do pedido do dono. O `git status --short` estava vazio antes da criação deste documento. A comparação com `origin/main` se refere ao ref local existente; não houve `fetch` nem verificação do servidor remoto nesta revisão.

| Commit | Conteúdo relevante |
|---|---|
| `75ea3a1` | Slice 62: tripwire re-check 01 com resultado SAME; trabalho interrompido para revisão externa; 50/50 CTest registrados. |
| `7802905` | Slice 61: notas M0/M1; currículo documentado em 22 lessons; 50/50 registrados. |
| `e72548a` | Atualização de números e índice de 21 lessons. |
| `cb0ea99` | Aula sobre autosave, da especificação à verificação 50/50. |
| `de62ab1` | Slice 58: auditoria por endereço corrige a distribuição para 467 palavras de tabela + 30 palavras reais. |

O cabeçalho de [STATUS](C:/Antigravity/gt4-staticrecomp/docs/STATUS.md:3) informa que a parada para revisão externa foi solicitada pelo dono. Este feedback não executa a retomada nem despacha slices de implementação.

O [journal de 2026-10-03](C:/Antigravity/gt4-staticrecomp/docs/journal/2026-10-03.md:3) contém as entradas dos slices 61 e 62; a data do cabeçalho do STATUS foi atualizada para 2026-10-04. Não inferir que a localização da entrada no journal corresponde necessariamente à data de cada ação.

### 3.2 O que está efetivamente estabelecido

**Confirmed — registro:** há tradução AOT de um conjunto muito grande do código EE, geração do módulo completo, execução integrada com serviços e vários marcos reais de boot. Os episódios de PRTS e do cursor de copy-out são exemplos especialmente bons de descobrir um contrato ausente a partir do consumidor guest e demonstrar o efeito da correção.

| Dimensão | Estado documentado | Limite da interpretação |
|---|---|---|
| Decoder | 349 operações | Reconhecer uma operação não estabelece todos os seus casos extremos. |
| Varredura do texto | 497 palavras unsupported em 1.334.917 | 467 ficam na tabela; 30 são código real. |
| Unsupported reais | 26 formas COP2 função `0x38`, 2 BC0F, 2 encodings não atribuídos no handler de exceção | Priorizar por execução e dependência; não concluir que todas bloqueiam o boot atual. |
| Tabela final | 700 palavras em `0x00616F28..0x00617A14` | Dados na seção text não são automaticamente instruções. |
| Survey de chamadas diretas | 14.991 de 15.067 entradas, cerca de 99,5% | É cobertura desse universo de entradas, não percentual de conclusão do jogo. |
| Módulo `--all` | 15.068 funções, 924.991 instruções, aproximadamente 146 MB de C++ | Sintaxe aceita pelo MSVC não prova execução correta de todo o módulo. |
| Boot e serviços | Inicialização, threads, SIF/RPC, disco, arquivos, som e fonte ultrapassados em marcos documentados | BIOS/IOP/dispositivos continuam sendo modelos parciais. |
| Testes atuais | 50/50 CTest e suíte Python com 73 casos, 6 skips, registrados pelo worker e agora reproduzidos | Skips e escopo de cada fixture continuam relevantes; os probes novos revelam contratos que a suíte atual não exige. |
| Fronteira atual | Tripwire SAME; 17 threads preservadas; estímulo único sem progresso observado | Estacionar não valida retrospectivamente os contratos usados para chegar lá. |

As medidas vêm de [STATUS](C:/Antigravity/gt4-staticrecomp/docs/STATUS.md:20), das evidências dos milestones e de [tripwire-recheck-01](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/tripwire-recheck-01.md:1). Não recomputei os números de decoder, survey ou geração.

### 3.3 Identidade dos inputs

Os valores abaixo foram lidos dos manifests. Na etapa experimental, **ISO e CORE extraído tiveram tamanho e SHA-256 recalculados**, e o verificador de disco existente confirmou os fingerprints dos três arquivos internos. Os hashes dos ELFs reconstruídos e do payload text permanecem metadados do manifesto, sem nova recomputação nesta revisão. O documento registra identidade, não distribui bytes do jogo.

| Input | Tamanho em bytes | SHA-256 do manifesto |
|---|---:|---|
| ISO USA v2.00, SCUS-97328 | 5.314.478.080 | `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f` |
| `CORE.GT4` | 2.020.861 | `85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9` |
| Loader `SCUS_973.28` | 273.020 | `f8f10823160e2b5cef5c7032628134632b291b1df87a9aee3c144794dd8019fa` |
| Payload text | 5.339.668 | `5a9a9107b146b7d533a2a2421cdf900a913d5ced4e97892798cfa810b3dd2d34` |
| ELF reconstruído nativo | 6.123.004 | `10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c` |
| ELF de referência | 6.127.896 | `94aada8984999736f14a121ea1bd543e72d9d59b801e513716a32d8991746222` |

Referências locais: [usa-v2.00.json](C:/Antigravity/gt4-staticrecomp/docs/inputs/usa-v2.00.json), [usa-v2.00-native.json](C:/Antigravity/gt4-staticrecomp/docs/inputs/usa-v2.00-native.json) e [usa-v2.00-reference.json](C:/Antigravity/gt4-staticrecomp/docs/inputs/usa-v2.00-reference.json). Entry `0x00100008`; texto inicia em `0x00100000`. Os dois ELFs têm layouts e hashes distintos de propósito. Comparar hash do container ELF não substitui a comparação dos segmentos ou do texto carregado.

### 3.4 Correções editoriais que ajudariam a próxima sessão

O cabeçalho atual do STATUS é claro, mas o documento acumula resumos de fases antigas. Há uma seção de ambiente com `32/32`, uma seção Next actions antiga e uma indicação de slice 62 como próximo passo depois de o cabeçalho registrá-lo concluído. O README conserva algumas descrições anteriores à auditoria 467+30. A ADR de autosave nasceu como specification-only e deve ser lida junto da implementação posterior.

Eu acrescentaria, em trabalho futuro, um quadro curto no topo com: HEAD, fronteira operacional, testes atuais, contracts conhecidos incompletos, próxima pergunta e links às evidências. Históricos devem permanecer históricos; marcar uma conclusão superada é preferível a apagar a sequência de aprendizado. **Não alterei esses documentos nesta sessão.**

## 4. Arquitetura: manter a separação que já funciona

O projeto tem quatro níveis com responsabilidades diferentes:

1. **Imagem e descoberta:** inputs pinados, reconstrução, decoder, disassembly, survey e identificação de funções.
2. **Semântica EE:** registradores, memória guest, wrapping definido, interpreter, operações FPU/MMI/VU0 e controle de fluxo.
3. **Execução AOT:** geração de C++ por função, despacho de entradas e bridge interpretada para fronteiras que o módulo não atravessa.
4. **Ambiente PS2:** kernel HLE, threads/semas, timers, IRQ, SIF, RPC, serviços IOP modelados, disco e janelas de dispositivos.

A divergência pode nascer em qualquer nível. O fato de o interpreter e o tradutor compartilharem o nível 4 é útil para isolar bugs de tradução, mas também permite que os dois aceitem o mesmo comportamento incorreto de timer, RPC ou interrupt.

**Eu não reescreveria o recompilador nem substituiria todo o runtime agora.** A direção AOT é coerente com a missão. O problema imediato é fechar contratos e criar uma terceira fonte de evidência nos pontos em que os dois motores usam a mesma implementação.

O bridge interpretado é uma ferramenta prática de progresso, desde que cada passagem tenha motivo observável. Vale medir quais PCs, opcodes e destinos consomem a execução interpretada. Cobertura estática de funções e quantidade de instruções realmente executadas nativamente são medidas diferentes.

## 5. F01 — Timer: largura lógica incorreta, com impacto direto na interpretação dos slices M32

**Prioridade: P0 de investigação/correção de contrato.**  
**Confirmed — código:** COUNT/COMP atravessam um banco genérico de 32 bits; overflow é calculado no limite `0xFFFFFFFF`.  
**High confidence:** isso é incompatível com o contrato do timer EE usado pela biblioteca de tempo investigada.  
**Hypothesis:** pode explicar parte das esperas e disparos anormais.  
**Unknown:** se corrigir o contrato fará a main avançar até a próxima fase.

### 5.1 Cadeia de evidência

Em [timer.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/timer.cpp:9), `TimerUnit::read_register` e `write_register` delegam integralmente a `RegisterBank`. Em [device.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/device.cpp:43), o banco exige acesso de quatro bytes e conserva o valor inteiro em um `uint32_t`.

Em [kernel.cpp, avanço idle](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1241), o próximo COUNT é calculado como soma de 64 bits, convertido para 32 bits, e OVFF só é levantado acima de `0xFFFFFFFF`. Em [avanço por serviço](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1279), a mesma largura aparece.

A largura do acesso MMIO e a largura lógica do contador não são a mesma coisa. Um registrador pode ser acessado com `lw/sw` de 32 bits e conter somente 16 bits significativos.

As referências independentes corroboram quatro contadores EE de 16 bits. O PCSX2 limita COUNT/TARGET a `0xFFFF`, verifica overflow nesse limite e implementa clear de flags por escrita. A documentação de engenharia reversa PS2tek descreve o mesmo tamanho lógico. [PCSX2 Counters.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Counters.cpp), [PS2tek](https://psi-rockin.github.io/ps2tek/).

O código de timer do PS2SDK constrói tempo estendido combinando uma contagem de overflows com os 16 bits baixos do contador e trata a possibilidade de overflow pendente durante a leitura. É corroborador da convenção, não prova de que essa revisão do SDK seja exatamente a usada pelo GT4. [PS2SDK timer.c](https://github.com/ps2dev/ps2sdk/blob/ac92a9f657d2e531dd8f060250b07f2a5ac6dea5/ee/kernel/src/timer.c).

### 5.2 Exemplo manual que separa os dois modelos

Condição sintética: COUNT inicial `0xFFF0`, clock BUSCLK/256, avanço de 576 ticks do contador.

```text
0xFFF0 + 576 = 0x10230

Modelo atual de 32 bits:
    COUNT = 0x00010230
    nenhum overflow de 32 bits

Contador lógico de 16 bits:
    COUNT = 0x0230
    ocorreu passagem por 0xFFFF -> 0x0000
    flag/status/entrega devem seguir seus contratos separados
```

O exemplo não depende de bytes de GT4 nem de uma captura. É uma fixture pequena capaz de detectar a diferença específica.

### 5.3 Por que esse detalhe muda a leitura do M32

O [slice 5](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m32-slice5-delay-node-fires.md) reconstrói, em essência:

```text
extended = (software_overflow_count << 16) | TIM2_COUNT
current  = extended << ((TIM2_MODE & 3) * 4)
```

O overflow de software identificado está em `0x006592F0`. Se COUNT já tem bits altos, o OR deixa de concatenar duas partes disjuntas. Isso corrompe o valor e a evolução do tempo reconstruído pelo guest; o shift continua aplicando a conversão de unidade usada pela biblioteca. A aparência de um relógio aumentando não prova que ele represente o relógio que o código espera.

Os dumps antigos com COUNT `0x89E863C0` e COMP `0xFFFFFE40` são particularmente úteis: no contrato de 16 bits, as partes significativas seriam `0x63C0` e `0xFE40`. Não recomendo reinterpretar o dump antigo mascarando os valores e declarar o problema resolvido; a quantidade de overflows já processada e os alvos de delay foram produzidos sob outra história temporal.

O [slice 17](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m32-slice17-walk-traced-live.md) e o [slice 18](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m32-slice18-worker-requests.md) fornecem observações úteis do modelo. A extrapolação de que o mecanismo é intrinsecamente uma loteria no hardware, ou de que os micro-waits ficaram presos por comportamento normal do timer, precisa ser reaberta. Uma varredura em um domínio de 32 bits não demonstra a evolução de um contador de 16 bits.

O re-check atual chama o Timer2 de saudável e mostra COUNT `0x1E9DBBC0`, MODE `0x782`, COMP `0x240`. Isso estabelece que o mesmo estado modelado foi preservado. Não estabelece validade do contrato do contador. [Tripwire re-check](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/tripwire-recheck-01.md).

### 5.4 O que eu implementaria, em fatia pequena

Um estado de timer com campos nomeados e regras de registrador, evitando que `RegisterBank` seja a semântica do dispositivo. É aceitável manter um backing store para snapshots, mas leituras/escritas e avanço precisam passar pelo contrato do timer.

Antes da alteração, documentar para cada campo usado pelo jogo: largura, reset, bits writable, flags, origem de clock, compare, zero-return e gate. Não implementar todos os modos por especulação; um modo observado mas não suportado deve ser identificável.

Critérios mínimos de aceitação:

- Escritas/leituras COUNT e COMP respeitam os bits significativos.
- A fixture `0xFFF0 + 576` observa o wrap correto.
- A rotina guest de tempo estendido vê crescimento coerente, inclusive junto de overflow pendente.
- COMP atrás do COUNT não causa uma igualdade fictícia imediata; a passagem seguinte é tratada de acordo com o modo.
- O reprogramming de COMP pelo handler participa da próxima passagem.
- O avanço idle e o avanço por serviço usam a mesma máquina de estados do timer.
- O novo estado é serializável e a retomada conserva divisores, restos, flags e eventos.

O teste precisa verificar o que o guest lê e o efeito do acknowledge, não somente um contador privado do host.

## 6. F02 — Timer: compare, acknowledge e múltiplos wraps são problemas separados

**Confirmed — código:** o avanço idle levanta EQUF a cada frame, independentemente de COMP. O avanço por serviço usa `else if` entre overflow e compare. MODE é armazenado como uma escrita ordinária. [kernel.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1264), [device.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/device.cpp:51).

A referência PCSX2 trata EQUF/OVFF com escrita de 1 para limpar, em vez de atribuição integral de uma palavra. Esse contrato deve ser distinguido da operação interna que levanta uma flag. [Counters.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Counters.cpp).

### 6.1 Armadilha de uma correção incompleta

Hoje o kernel levanta flags usando `state.memory().write_word` no mesmo caminho de acesso usado pelo guest. Se esse caminho ganhar W1C sem outra alteração, escrever uma flag para levantá-la poderá limpá-la.

Eu separaria explicitamente:

```text
guest_write_mode(value)        -> aplica controle writable e acknowledge
device_set_compare_flag()     -> levanta flag internamente
device_set_overflow_flag()    -> levanta flag internamente
device_restore(snapshot)     -> restaura estado, sem simular escrita guest
```

Os nomes são ilustrativos; não há proposta de criar um framework genérico. O importante é não confundir três operações diferentes: programação do guest, transição de hardware e restauração de snapshot.

### 6.2 Não basta trocar a máscara para 0xFFFF

Um quantum idle BUSCLK de 2.457.600 ticks atravessa 37 ou 38 wraps de um contador de 16 bits, dependendo do ponto inicial. No clock /256, um frame modelado tem 9.600 ticks. O mesmo mecanismo tem regimes muito diferentes conforme CLKS.

Se o host avança tudo de uma vez e entrega só um handler no fim, o contador de overflows de software pode perder épocas. Se entregar dezenas de callbacks artificiais sem aplicar máscaras e ack, também pode inventar comportamento. O contrato de eventos precisa dizer quando a CPU tem oportunidade de observar e limpar status e reprogramar o próximo compare.

A abordagem mais segura é avançar até o próximo acontecimento relevante, registrar a transição do dispositivo e devolver controle num ponto permitido para entrega. Quando não há interrupção elegível, a passagem de vários wraps pode coalescer no status; o modelo não deve contar cada wrap como um handler automaticamente.

Essa proposta conserva determinismo e não exige começar com um simulador ciclo a ciclo. Exige que um grande salto de tempo não atravesse acontecimentos observáveis sem decidir como eles serão representados.

### 6.3 Fixtures de alto valor

| Caso | O que verificar |
|---|---|
| Write COUNT/COMP com bits altos | Bits significativos e comportamento dos reservados. |
| MODE com flag pendente, write 0 | Flag não apagada por atribuição incorreta. |
| MODE com flag pendente, write 1 nessa flag | Acknowledge remove a flag correspondente. |
| Compare e overflow atravessados no mesmo avanço | Os dois acontecimentos não se excluem por estrutura de `else if`. |
| Compare reprogramado pelo handler | O próximo avanço usa o novo alvo. |
| COMP menor que COUNT | Aguarda a próxima passagem pertinente. |
| Vários wraps em um salto | Estado de dispositivo e oportunidades de atendimento coerentes. |
| Timer parado | COUNT não progride. |
| Snapshot/restore | Não dispara side effects de escrita guest. |
| Função de tempo do jogo | A leitura composta e os delays mantêm unidade e monotonicidade esperadas. |

Medir o time getter `0x005B8400` e o comparador de vencimento `0x005B822C` na referência permitiria validar a relação entre COUNT, MODE, COMP, overflow de software, base e target. Esses endereços vêm da investigação local; confirmar o texto live antes de usá-los como hooks.

## 7. F03 — DMA: completions conectadas à família de interrupções errada

**Prioridade: P0/P1, depois ou em paralelo de leitura com o timer.**  
**Confirmed — código:** o roteamento local é INTC 4/5/9 e depende de TIE.  
**Confirmed — referência:** completion desses canais é DMAC 0/1/2; TIE não é o enable geral da completion final.  
**Unknown:** quais transferências desse caminho ocorreram antes da fronteira atual e qual efeito uma correção terá no boot.

### 7.1 Caminho local

Em [BootDevices](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:59):

| Canal | Base MMIO | Causa passada ao callback local |
|---|---|---:|
| VIF0 DMA | `0x10008000` | 4 |
| VIF1 DMA | `0x10009000` | 5 |
| GIF DMA | `0x1000A000` | 9 |

O callback de [main.cpp](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:915) chama `Kernel::raise_interrupt`, que enfileira uma causa INTC. `DmaChannel::write_register` limpa STR imediatamente e só chama o callback quando o bit usado como `interrupt_enable`, TIE, está presente. [device.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/device.cpp:129).

A [decisão 0011](C:/Antigravity/gt4-staticrecomp/docs/decisions/0011-timer-ticks-and-dma-completions.md) registra essa aproximação. A revisão deve corrigir o contrato; não tratar a escolha antiga como validação de hardware.

### 7.2 Contrato externo e confirmação independente

Na referência PCSX2, o fim do VIF0 DMA chama `hwDmacIrq(DMAC_VIF0)` e o fim do GIF chama `hwDmacIrq(DMAC_GIF)`. O teste de TIE junto da IRQ da tag participa do término antecipado da cadeia, enquanto a completion final levanta o status DMAC. [Vif0_Dma.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Vif0_Dma.cpp), [Gif.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Gif.cpp), [Hw.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Hw.cpp).

Os canais DMAC são 0 para VIF0, 1 para VIF1 e 2 para GIF. INTC 4/5 correspondem a interrupções de VIF, que são eventos distintos de finalizar a transferência DMA; INTC 9 pertence ao Timer0. [PS2tek](https://psi-rockin.github.io/ps2tek/).

Há um corroborador baseado em hardware: o `tagintr.expected` de ps2autotests mostra status CIS após término em todas as quatro combinações de TIE e IRQ de tag. O ponto de término/tag muda em uma combinação; a existência de completion não depende de TIE. O teste usa outro canal, portanto corrobora o papel de TIE e a família DMAC, sem ser uma execução específica de GIF. [tagintr.cpp](https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/dma/dmac/tagintr.cpp), [resultado esperado](https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/dma/dmac/tagintr.expected).

### 7.3 O que mudar e como testar

Uma completion simulada ainda pode ser instantânea numa fatia inicial, mas precisa declarar que não transferiu nem processou payload. O caminho mínimo deve identificar o canal DMAC, levantar o status correto e tornar a entrega dependente das máscaras e handlers pertinentes. Interrupções geradas pelo processamento de comandos VIF devem ficar separadas.

Há outra sutileza no código atual: `start_interrupt` verifica se existe handler antes de levantar D_STAT ou INTC_STAT e descarta o pedido quando não há handler. Portanto, trocar somente o callback para a fila DMAC não basta. A causa pendente do dispositivo deve existir independentemente de haver handler registrado; escolher se/quando chamar o handler é uma etapa posterior. [kernel.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1179).

O teste local de DMA em [ee_timer_test.cpp](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_timer_test.cpp:100) exige o comportamento antigo, inclusive silêncio sem TIE. Ele é útil como teste da implementação anterior, mas não pode ser usado como prova do contrato correto. A expectativa deve ser revisada com evidência externa.

Fixtures recomendadas:

- Completion normal com TIE=0 levanta o bit DMAC do canal.
- Completion normal com TIE=1 tem a mesma família de status.
- Completion sem handler conserva o status CIS; registrar/habilitar um handler depois não fabrica uma nova completion.
- Máscara desabilitada conserva pending status e impede dispatch elegível.
- Habilitar a máscara com status pendente segue a política verificada.
- Acknowledge limpa somente as causas solicitadas.
- VIF command IRQ não é confundida com DMA completion.
- Uma completion GIF jamais entrega o handler Timer0 só por causa do número 9.
- Se chain mode ainda não é implementado, diagnosticar o modo em vez de fingir que tags foram consumidas.

Um GIF/VIF seco no estado estacionário não exonera o DMA de efeitos incorretos durante a inicialização. Capturar os primeiros starts/completions e handlers é mais discriminante que acrescentar estímulos no fim.

## 8. F04 — Interrupt: separar origem, pending status, máscara e dispatch

**Confirmed — código:** Enable/Disable INTC e DMAC retornam 0 sem efeito em [kernel.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1018). O comentário ainda diz que não há entrega de interrupt, embora ela exista em outras rotinas. `start_interrupt` verifica handler ativo e fila, mas não usa uma avaliação completa das máscaras e do estado de interrupção CP0 antes de escolher o pedido. [start_interrupt](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1160).

A implementação PCSX2 distingue levantar status de decidir se a CPU pode atender. INTC_STAT usa W1C e INTC_MASK escrita de toggle; D_STAT também distingue causas e máscaras. [Hw.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Hw.cpp), [HwWrite.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/HwWrite.cpp).

Eu escreveria um contrato pequeno para cada passagem:

```text
acontecimento do dispositivo
    -> alteração de dados/status do dispositivo
    -> causa pendente INTC ou DMAC
    -> elegibilidade por máscaras e estado da CPU/kernel
    -> entrega ao handler registrado
    -> consumo/ack pelo guest
    -> mudança de predicado que libera trabalho
```

Um callback entregue não prova que exista dado novo para consumir. Um status pendente não precisa equivaler a uma chamada de handler por item na fila. Um serviço que habilita interrupt não deve fabricar um acontecimento.

### 8.1 Ordem e coalescing

Em [queue_interrupt](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:1073), uma ocorrência duplicada é removida e recolocada ao fim. Portanto:

```text
fila inicial:       [A, B]
nova ocorrência A:  [B, A]
```

Isso muda a ordem relativa entre causas distintas. A intenção documentada de coalescing é razoável para eliminar backlog artificial, mas a afirmação de que a ordem permanece intacta precisa ser testada/reformulada. [ADR 0025](C:/Antigravity/gt4-staticrecomp/docs/decisions/0025-coalesce-pending-interrupts.md).

A escolha de uma ordem determinística de dispatch pode continuar simples. Ela deve ser descrita como política do modelo e comparada com o trecho do jogo em questão. Uma fila ilimitada de completions DMAC também merece revisão: status por bit e fila por transação transportam informações diferentes.

### 8.2 Limite de escopo

A [decisão 0013](C:/Antigravity/gt4-staticrecomp/docs/decisions/0013-handler-execution.md) mantém handlers sem nesting e sem preempção. Não recomendo abandonar a escolha inteira sem evidência. Recomendo primeiro garantir que o modelo não entregue uma causa desabilitada ou a família errada e que preserve a origem dos dados.

Se a captura independente mostrar que a falta de preempção é a primeira diferença necessária, a revisão do scheduler passa a ter justificativa. Antes disso, implementar um kernel mais amplo adiciona custo sem localizar o erro.

## 9. F05 — `jr ra`: o destino precisa ser capturado antes do delay slot

**Prioridade: P1, correção pequena com teste forte.**  
**Confirmed — código e execução:** o emitter de Return executa o delay slot antes de ler `ra`. O interpreter captura o destino antes do slot; a fixture foi compilada e reproduziu a divergência.  
**Unknown:** existe um caso executado no GT4 atual cujo slot modifica `ra`.

Em [gt4translate](C:/Antigravity/gt4-staticrecomp/tools/gt4translate/main.cpp:818), `FlowKind::Return` emite primeiro a instrução do slot e depois `state.read_gpr64(31)` para definir PC. O caminho de salto indireto genérico já captura o target antes do slot. O [interpreter](C:/Antigravity/gt4-staticrecomp/src/ee/interpreter.cpp:2617) faz a captura na ordem esperada.

Fixture original, montada manualmente:

```text
ra inicial = 0x00003000

0x03E00008    jr    ra
0x241F2000    addiu ra, zero, 0x2000    ; delay slot

Estado correto após o salto:
    PC = 0x00003000
    RA = 0x00002000

Estado produzido pela ordem atual do emitter:
    PC = 0x00002000
    RA = 0x00002000
```

A diferença foi inicialmente deduzida da ordem de leitura/escrita. Depois da autorização para testes, a fixture passou pelo **emitter real -> C++ compilado -> execução**, e confirmou exatamente esses dois estados. Método, arquivo gerado e output estão na seção 29.

Eu acrescentaria esse caso à suíte existente de tradução/controle de fluxo. Além de verificar o C++ emitido, executar o módulo gerado e o interpreter até a mesma fronteira deve produzir PC, RA e efeitos do slot iguais.

A correção não deve parar em salvar `ra` numa variável: o contrato com o driver precisa acompanhar a mudança, conforme o achado seguinte.

## 10. F06 — O motivo da saída do módulo não cabe numa heurística `PC == RA`

**Prioridade: P1, junto com F05.**  
**Confirmed — código:** o driver identifica retorno pela igualdade entre PC e RA atual e decodifica a instrução no PC para deduzir outras paradas.  
**Confirmed — execução:** a interface confundiu ERET já aplicado e BREAK com PC==RA nas fixtures geradas; um controle com ordem correta de JR também foi classificado como InstructionStop. A seção 29 distingue código gerado real do controle hipotético.

### 10.1 Retorno com RA modificado no slot

Em [classify_boundary](C:/Antigravity/gt4-staticrecomp/src/ee/driver.cpp:52), `PC == RA` significa Returned. Depois de corrigir F05, um retorno perfeitamente válido terá PC `0x3000` e RA `0x2000`. Se o destino contiver uma instrução comum, o classificador pode produzir `InstructionStop`.

O comentário do próprio código reconhece que um trap com PC igual a RA seria indistinguível de um retorno. É uma limitação de representação, não um caso resolvido por aumentar a quantidade de comparações.

### 10.2 `ERET` já aplicado

O emitter de [ERET](C:/Antigravity/gt4-staticrecomp/tools/gt4translate/main.cpp:600) aplica EPC ou ErrorEPC, limpa EXL ou ERL e retorna com PC **no destino**. O classificador do driver, por sua vez, só identifica ExceptionReturn se a instrução **nesse destino** for ERET.

Se EPC aponta para uma instrução ordinária e não coincide com RA, a saída pode ser classificada como `InstructionStop`. O mock [stop_at_eret](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_driver_test.cpp:48) simplesmente coloca PC no word ERET; não representa o comportamento efetivo do emitter. Portanto, esse teste não fecha a integração.

Não há prova nesta revisão de que o ERET traduzido seja alcançado na fronteira atual. Parte dos handlers pode passar pelo bridge. O problema de contrato continua válido para a capacidade declarada do tradutor.

### 10.3 Interface que eu escolheria

Uma indicação explícita e pequena de saída do módulo, com:

- motivo: continuação/retorno aplicado, syscall, trap, transferência ainda não aplicada, ERET aplicado ou unsupported;
- PC da instrução que produziu a saída;
- PC de continuação quando já calculado;
- informação necessária para saber se o delay slot foi aplicado.

Isso pode ser um resultado da chamada ou um campo transitório bem definido do contexto. Eu preferiria uma representação que não dependa de analisar valores de registradores para descobrir intenção. O resultado precisa se propagar nas chamadas internas do módulo sem ser sobrescrito por um caller que ainda está desempilhando.

Não é necessário introduzir exceções C++ para todo fluxo guest nem um sistema amplo de continuations. O objetivo é representar precisamente a fronteira já existente.

Fixtures de integração:

1. `jr ra` com RA alterado no slot.
2. `ERET` com destino ordinário diferente de RA, para EXL e ERL.
3. Trap em PC coincidente com RA.
4. Syscall em slot/caminho permitido pelo modelo, conservando a regra de parada.
5. Transferência indireta conhecida e desconhecida, sem repetir o delay slot.

Esses casos devem atravessar **emitter -> módulo compilado -> driver**. Mocks continuam úteis para outras regras, mas não substituem esse contrato.

## 11. F07 — A comparação atual não cobre toda a máquina

**Prioridade: P1 de evidência.**  
**Confirmed — código:** `states_match` compara muito do contexto EE, mas o digest cobre somente a RAM principal e não recebe Kernel/dispositivos.

Em [main.cpp](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:452), `memory_digest` lê de 0 até `ram_size`, usando FNV-1a. Em [states_match](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:464), há comparação de GPRs completos, FPRs, CP0, VU0, acumuladores, flags, HI/LO, shift cache e PC.

O comparador não inclui, nesse caminho:

- scratchpad em `0x70000000`;
- a região de armazenamento GS mapeada em `0x12000000`;
- contextos das threads que não estão ativos;
- semáforos, wakeup counts e filas do kernel;
- estado de timer/IRQ e bancos MMIO;
- restos e épocas do relógio de serviço;
- servidores, requests/replies, handles e cursores PRTS;
- flags one-shot que pertencem ao modelo de serviço.

O contexto ativo e a RAM principal iguais são evidência forte e valiosa. A frase “máquina completa idêntica” precisa ter o escopo ajustado até que essas partes sejam comparadas. A verificação de resume também chama esse comparador no caminho inspecionado.

### 11.1 Extensão com infraestrutura que já existe

O repositório já tem [regions_snapshot](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:233), [snapshot_banks](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:144), serialização de kernel e testes de snapshot. Eu reutilizaria esses mecanismos com representação canônica, sem criar outra infraestrutura paralela.

Uma comparação de máquina poderia receber contexto, snapshots das regiões RAM, snapshot do kernel e snapshots dos dispositivos. Precisaria explicar exclusões como ponteiros host religados, métricas puramente diagnósticas e callbacks. Se arrays/maps dependem da ordem de inserção, normalizar por identidade antes de comparar.

Para testes pequenos, comparar bytes exatamente. Para estados grandes, digest pode localizar o primeiro bloco diferente e depois comparar o bloco para diagnóstico. FNV-1a é um resumo de diagnóstico, não uma prova matemática de identidade.

**Não calcular um digest lendo MMIO indiscriminadamente.** Alguns registradores têm side effects de leitura. Snapshot de estado do dispositivo deve ser uma operação de observação definida, sem simular acessos guest.

Testes já relevantes:

- [ee_checkpoint_test](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_checkpoint_test.cpp:96): restauração de RAM e scratchpad.
- [ee_kernel_test](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_kernel_test.cpp:1191): serialização e continuação de estado/cursor.
- [ee_device_test](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_device_test.cpp:27): snapshot e restauração de bancos.

Acrescentar fixtures em que somente scratchpad, um semáforo ou um registrador do dispositivo difira. O comparador ampliado precisa detectar a mudança com contexto EE e RAM principal iguais.

## 12. F08 — Checkpoints precisam de identidade semântica, além de formato válido

**Confirmed — código:** [CheckpointFile](C:/Antigravity/gt4-staticrecomp/include/gt4recomp/ee_checkpoint.hpp:74) contém número de serviços e três seções: contexto/memória, kernel e bancos. Há versões/magias e validação estrutural. Não há no framing inspecionado uma associação completa a hash de inputs, versão semântica do modelo ou identidade do módulo gerado.

Um checkpoint pode ser estruturalmente legível e semanticamente incompatível. Isso é especialmente importante para corrigir F01/F02: as bases de delay, contagem de overflows e filas já foram produzidas com outra semântica. Truncar o COUNT restaurado não reconstrói a história correta.

Eu conservaria os checkpoints antigos como evidência forense e fixtures da versão antiga, mas marcaria sua incompatibilidade com o novo modelo de tempo. Uma execução nova desde a entrada deve gerar a referência e os checkpoints novos.

Metadados recomendados, no formato ou em sidecar validado:

| Campo | Por que importa |
|---|---|
| SHA-256 do CORE/text/native image | Evita interpretar estado de outro executável. |
| SHA-256 e geometria do disco | PRTS e handles dependem dos bytes e dos offsets servidos. |
| Revisão/model compatibility ID | Diferencia alteração semântica de alteração apenas editorial. |
| Identidade/hash do módulo gerado | O estado não deve retomar silenciosamente em outra tradução. |
| Configuração de tempo/eventos | Quantum, divisores e política de dispatch mudam comportamento. |
| Mapa/topologia de memória/dispositivos | Os bancos salvos não identificam sozinhos o significado de cada endereço. |
| Ponto de save e predicado de segurança | Conserva a garantia de não estar no meio de um delay slot/transfer transitório. |

Não usaria somente o commit do repositório como versão de compatibilidade: alterações de documentação não deveriam invalidar todos os estados. Mas o commit e o build devem continuar registrados como provenance.

O CORE é validado pelo caminho `read_verified_core`. No caminho `--disc` inspecionado, a abertura da imagem não mostrou a mesma checagem inline de SHA-256. Se a disciplina atual depende de um passo prévio de verificação, registrar o resultado desse passo junto do run/checkpoint. [main.cpp](C:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp:769).

## 13. F09 — RPC: completar transporte não valida o conteúdo nem a existência do serviço

**Prioridade: P1/P2, causalidade a selecionar depois dos contratos anteriores.**  
**Confirmed — código:** bind cria/associa servidor para qualquer SID solicitado; requests não reconhecidos podem receber buffers zerados e completion.  
**Hypothesis:** uma resposta anterior incompleta pode ter impedido a criação do produtor que falta hoje.  
**Unknown:** qual SID/função, se houver, causou esse efeito.

### 13.1 O risco concreto

O caminho de bind em [kernel.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:2184) cria/associa um servidor para qualquer SID solicitado e responde com handle não nulo; não condiciona a disponibilidade à existência de contrato implementado ou ao registro real do serviço. O caminho de resposta pode construir uma área zerada para um RPC sem contrato específico. Não interpretar todo zero como sucesso: cada protocolo pode atribuir significado diferente a ele. O problema é entregar uma resposta **não verificada** como se houvesse execução válida do servidor.

No SIF RPC do PS2SDK, a resolução de bind pesquisa um servidor registrado; a conclusão de request transporta resultado e permite callback/semaphore do cliente. A existência do endpoint e a completion do transporte são etapas distintas. [PS2SDK sifrpc.c](https://github.com/ps2dev/ps2sdk/blob/ac92a9f657d2e531dd8f060250b07f2a5ac6dea5/ee/kernel/src/sifrpc.c), [sifcmd.c](https://github.com/ps2dev/ps2sdk/blob/ac92a9f657d2e531dd8f060250b07f2a5ac6dea5/ee/kernel/src/sifcmd.c).

Uma fila de SIF vazia no fim pode resultar de um sistema corretamente aguardando algo externo. Também pode resultar de um bootstrap que recebeu um dado incorreto e jamais construiu seu produtor. A observação final não separa essas hipóteses.

### 13.2 O que eu colocaria sob contrato explícito

Para cada par `(SID, RPC number)`, registrar:

- versão/ABI observada;
- pré-condição de bind e disponibilidade após load/reset;
- tamanho e campos do request;
- área de reply e buffers adicionais;
- interpretação de erro/retorno;
- sync/async, callback e completion;
- transições de estado do serviço;
- evidência do consumidor guest e de uma fonte independente;
- campos ainda desconhecidos e próximo experimento.

Classificar endpoint como **implemented**, **observed inert**, **provisional** ou **unknown**, sem transformar uma resposta genérica em prova de implementação. Na build de investigação, unknown deveria parar no primeiro uso com SID, função, PC/caller, tamanhos e estado. Exceções provisórias necessárias para alcançar uma âncora devem ser nominadas e contabilizadas, nunca silenciosas.

### 13.3 Alvos específicos que merecem captura

O liblgdev SID `0x046D046D` tem conteúdo de replies parcialmente modelado: RPC 12 inclui o status concluído `0x010B2400` em `+4` de uma resposta maior; RPC 4 inclui assinatura `0x046DC298` em `+0x5C`; outros bytes/calls ficam genéricos. O status informado pelo modelo não deve ser confundido com completude da modelagem dos campos. Isso pode ter sido suficiente para ultrapassar um polling inicial, mas não especifica o protocolo inteiro. A [ADR 0015](C:/Antigravity/gt4-staticrecomp/docs/decisions/0015-sif-register-mirror-and-liblgdev-sync.md) deve ser confrontada com uma captura completa dos requests e replies que o boot realmente consome.

Eu capturaria o primeiro request relevante, a resposta inteira, os buffers auxiliares, o callback e as primeiras leituras guest desse resultado. Comparar somente uma constante de compatibilidade ou uma palavra de status pode esconder o campo que habilita a fase seguinte.

PRTS também tem limites a documentar: flags de read, gestão/eviction de handles, distinção entre RPC 4/7 e semântica de operações de manutenção. O cursor corrigido já demonstra a importância de estado por handle. Eu não ampliaria tudo de uma vez; escolheria os calls alcançados e qualquer resultado divergente. [ADR 0021](C:/Antigravity/gt4-staticrecomp/docs/decisions/0021-prts-block-cache.md).

### 13.4 IOP module load não é execução do módulo

Servir os tamanhos reais dos IRX e aceitar requests de loading é um marco importante do caminho de arquivos. Isso não significa que o código IRX foi executado, seus imports resolvidos, threads iniciadas ou servidores realmente registrados.

Manter os termos separados evita concluir que um subsistema IOP está operacional só porque o EE ultrapassou o loader. Esse é justamente o ponto em que uma IOP parcial com execução de IRX pode ganhar valor, mas a decisão exige escolher um produtor concreto e seus contratos.

## 14. F10 — SUB/DSUB no mínimo assinado: auditar com uma terceira referência

**Prioridade: P2; não parece o primeiro alvo do bloqueio atual.**  
**Confirmed — código:** SUB/DSUB são implementados como adição checked do negado em domínio unsigned e existe comentário sobre espelhar a referência no caso extremo.  
**Hypothesis:** o comportamento de overflow pode seguir uma peculiaridade da implementação de referência, não o contrato do hardware.

Referências locais: [helpers aritméticos](C:/Antigravity/gt4-staticrecomp/src/ee/interpreter.cpp:37), [SUB/DSUB](C:/Antigravity/gt4-staticrecomp/src/ee/interpreter.cpp:2153). A implementação PCSX2 consultada também chama helper de adição depois de negar o operando, com conversões de largura relevantes. Essa semelhança é motivo para teste independente, não uma segunda prova inteiramente independente do hardware. [R5900OpcodeImpl.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/R5900OpcodeImpl.cpp).

Exemplos matemáticos para aritmética assinada convencional de 32 bits:

```text
0 - INT32_MIN   -> +2147483648, fora do intervalo: overflow
-1 - INT32_MIN  -> +2147483647, dentro do intervalo: sem overflow
```

O cálculo de negação no domínio de 32 bits exige cuidado exatamente nesse valor. Para DSUB, repetir com 64 bits e não depender de negation signed com comportamento indefinido no host.

Próximo experimento: localizar um resultado em hardware/ps2autotests ou produzir uma fixture original para console, se o ambiente permitir. Testar também o destino zero, preservação de destino em trap, EPC/BD conforme escopo atual e os casos extremos adjacentes. Não mudar cegamente uma peculiaridade deliberada apenas porque a aritmética convencional sugere outra coisa.

## 15. Outros riscos técnicos, a atacar por evidência de uso

### 15.1 Tempo por serviço não é tempo proporcional ao trabalho guest

**Confirmed — código:** o relógio progride um quantum por serviço e um frame no idle. Esse desenho é deliberado na [ADR 0016](C:/Antigravity/gt4-staticrecomp/docs/decisions/0016-service-clock.md).

Um loop guest sem serviço pode não avançar tempo. Um trecho que chama muitos serviços pode avançar mais tempo que um trecho que processa muitos comandos. Interrupções só têm oportunidades em certas fronteiras. Isso pode ser suficiente para determinado prefixo e insuficiente para outro.

Eu primeiro corrigiria o contrato dos timers mantendo a política atual isolada. Depois mediria se a política ainda impede uma condição observada na referência. Se sim, uma evolução possível é contabilizar trabalho por bloco/quantum guest e avançar uma agenda de eventos com unidade documentada.

Um relógio determinístico pode ser aproximado sem alterar a largura, o acknowledge ou a causalidade dos dispositivos. Fidelidade temporal e fidelidade de registrador são eixos diferentes.

### 15.2 Orçamento do driver e loops nativos

Em [driver.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/driver.cpp:155), o step budget soma chamadas de módulo e passos interpretados. Uma chamada pode executar muitas instruções e loops; o budget não é uma contagem total de instruções guest.

**Hypothesis:** um loop traduzido sem saída pode reter o host e impedir polling/entrega, enquanto uma sequência equivalente no bridge gera oportunidades mais frequentes. Não há nesta revisão prova de que esse seja o motivo da parada observada.

Eu registraria backedges relevantes, maior duração de chamada, profundidade de chamadas C++ e pontos de entrega. Se aparecer starvation numa função concreta, inserir uma saída/check de quantum numa fronteira segura é mais objetivo que tornar cada instrução uma chamada ao scheduler.

Chamadas guest recursivas também podem consumir stack do host. Tail-call no C++ não deve ser pressuposto. Trampoline ou despacho iterativo é uma opção futura quando houver uso/profundidade que justifique o custo.

### 15.3 Código guest modificável e AOT

A memória guest aceita escritas e aliases; o módulo contém as instruções emitidas antes da execução. O interpreter busca os words atuais. O jogo já usa patching de stubs em alguns caminhos, mas isso não prova modificação de todas as regiões traduzidas.

**Unknown:** há uma escrita executada que altera código AOT relevante sem invalidar/evitar sua versão antiga?

Experimento barato: watch de escrita física na região de texto traduzida, atribuindo CPU/host/DMA e endereços aliases. Uma mutação detectada precisa ser ligada ao fetch/entry subsequente. Não implementar invalidation ampla ou recompilação dinâmica apenas por possibilidade abstrata.

### 15.4 Floating point e VU

Existem caminhos com operações float do host, normalização e tratamento específico do guest. A equivalência entre MSVC, configurações de floating point, flush de subnormais e semântica R5900 merece fixtures externas focadas. [interpreter.cpp](C:/Antigravity/gt4-staticrecomp/src/ee/interpreter.cpp:150).

**Unknown:** o boot atual depende de um caso de FPU/VU não coberto. Não trataria precisão gráfica futura como justificativa para reescrever FPU agora. Documentaria o contrato de compilação e os casos extremos ao alcançar uma função relevante.

Os 26 encodings COP2 reais restantes merecem classificação por função/uso. Completar decoder é útil quando a operação é alcançada, mas os gates de runtime permanecem mesmo com decode completo.

### 15.5 Janelas MMIO não equivalem a dispositivos implementados

Um `RegisterBank` que retorna zero para endereços não escritos torna uma janela acessível. Isso não estabelece os registradores existentes, reset values, bits reservados ou side effects.

Eu manteria uma tabela por dispositivo: **storage-only**, **provisional**, **contract implemented**, **unknown offset**. A transição entre categorias deve ocorrer com evidência e fixture. Acessos desconhecidos que influenciam fluxo precisam de diagnóstico útil, em linha com a missão do projeto.

## 16. O que eu reabriria nas conclusões sobre o bloqueio

| Conclusão/observação | O que continua sustentado | O que ainda não decorre dela |
|---|---|---|
| Não há ciclo entre waiters | O grafo modelado não mostra aquele deadlock | Todos os produtores foram corretamente inicializados. |
| SIF queue vazia | Não há pacote pendente no estado observado | Todos os replies anteriores estavam corretos. |
| Threads estacionam igual após resume | A versão atual reproduz a própria fronteira | A fronteira corresponde ao console. |
| Timer handler executa | Dispatch modelado funciona | COUNT/COMP/flags/tempo estendido estão corretos. |
| INTC 0/5 não libera trabalho | Esses estímulos, nesse estado, foram effect-free | VIF/DMA anteriores não contribuíram para o estado. |
| Nenhum PADMAN clássico bound | Esse endpoint não apareceu no inventário | Nenhum protocolo de input alternativo existe. |
| Job pool `0x00587xxx` não foi criado | O caminho específico permaneceu não inicializado | Ele é necessariamente o dispatcher gráfico principal que falta. |
| Decoder cobre quase tudo | Poucas palavras reais continuam unsupported | O jogo está quase concluído como produto jogável. |
| Tripwire SAME | O mesmo estímulo não produziu o evento observado | Novas repetições idênticas vão descobrir o produtor. |

### 16.1 PADMAN, DBCMAN e input

O inventário sem PADMAN não fecha a questão de input. O SID `0x80001300` já aparece no modelo e merece distinção entre os usos/protocolos atribuídos a ele. O projeto PS2Recomp atual identifica DBCMAN nesse SID; um registro histórico do autor de Play! também relaciona DBCMAN ao PAD2. Isso é uma pista de pesquisa, não prova de que input seja a causa atual. [ps2xIOP README](https://github.com/ran-j/PS2Recomp/blob/c5a9d02573410a2085a4b4b831b0b68ba3515440/ps2xIOP/README.md), [registro de desenvolvimento Play!](https://purei.org/index.php?base=316).

Eu identificaria requests, buffers EE/IOP, formatos e condições que o cliente usa antes de emitir um dispositivo virtual. “Botão neutro” e “dispositivo inexistente” são estados diferentes. Não bastaria retornar uma palavra de sucesso para simular pad conectado.

### 16.2 O job pool pode pertencer a outro subsistema

As strings e callers já mapeados incluem pistas de impressão/renderização que não bastam para promover o pool `0x00587xxx` a uma fila gráfica universal. A investigação local é útil para descrever suas próprias condições de criação. Ela precisa de um caller realmente alcançado no boot ou na referência para ser o próximo gate causal.

### 16.3 Qual é a pergunta correta agora?

Eu substituiria “qual estímulo arbitrário acorda alguém?” por:

> Qual condição necessária ao próximo trabalho da main difere entre a execução funcional de referência e o modelo, e qual produtor normalmente escreve essa condição?

Essa pergunta permite distinguir timeout, reply, flag, disponibilidade de arquivo, estado de input ou trabalho gráfico. Ela também evita tratar cada sleeper como problema independente.

## 17. Pesquisa externa: ferramentas e repositórios que realmente ajudam

### 17.1 PCSX2 como instrumento independente, sem incorporá-lo ao runtime

O maior ganho imediato é observar uma execução que executa o código original do BIOS e dos módulos IOP dentro do emulador, usando o mesmo input pinado. Isso não é uma execução em hardware físico. O PCSX2 é uma implementação independente madura, mas ainda contém aproximações; quando ele e o modelo divergem, registrar o contrato relevante e, em casos extremos, buscar resultado de hardware.

**Começar pelo debugger oficial.** Na revisão consultada, o diálogo de breakpoints tem condição, logging, limite de hits e opção de continuar após o hit. O engine avalia expressões como PC, RA e argumentos no formato de log. Não verifiquei se a instalação local contém exatamente esses recursos. [BreakpointDialog.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2-qt/Debugger/Breakpoints/BreakpointDialog.cpp), [Breakpoints.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/DebugTools/Breakpoints.cpp).

Exemplo de formato proposto, a calibrar na versão instalada:

```text
OPEN pc={pc} ra={ra} a0={a0} a1={a1} v0={v0} sp={sp}
```

Executar primeiro um logpoint de controle positivo: confirmar hit, registradores e momento em relação à instrução e ao delay slot. Limitar hits evita gerar outro log de dezenas de GB sem hipótese.

O header do debugger registra limites de memchecks em interpreter/HLE. Portanto, zero hits num watchpoint não prova ausência de writes de DMA ou host. Calibrar o caminho específico é obrigatório. [Breakpoints.h](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/DebugTools/Breakpoints.h).

### 17.2 PINE: memória e identificação, não debugger remoto completo

PINE oferece operações de memória, identificação/status e save/load. A enumeração consultada não contém pause/resume, step, breakpoint ou leitura de registradores. No Windows usa loopback TCP; o slot padrão é 28011. Save/load são despachados para o thread da CPU, e leituras agrupadas não demonstram atomicidade enquanto a VM está executando. [PINE.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/PINE.cpp), [PINE.h](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/PINE.h).

Usar PINE para dumps em paradas controladas ou depois de confirmar save concluído é um caminho compatível com os scripts existentes do projeto. Não presumir que uma resposta OK de save já representa a conclusão efetiva da operação. Não usar writes para forçar flags nesta investigação causal.

Há uma issue oficial sobre corrida em writes concorrentes em páginas protegidas no ambiente relatado. Ela não prova um problema em toda leitura nem na instalação Windows local, mas reforça a opção de observação pausada e sem writes. [PCSX2 issue #14589](https://github.com/PCSX2/pcsx2/issues/14589).

A CLI oficial documenta abertura do debugger, slowboot e carga de states. Conferir as opções da build local antes de automatizar; manter a forma de boot registrada. [CLI PCSX2](https://github.com/PCSX2/pcsx2-net-www/blob/main/docs/advanced/cli.md).

### 17.3 Build instrumentada do PCSX2: somente se os logpoints não atribuírem a origem

Os caminhos SIF0/SIF1 oferecem pontos naturais para registrar EE -> IOP, IOP -> EE, cópia, tag e completion. As direções e completions não devem ser reduzidas a um único “RPC respondeu”. [Sif0.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Sif0.cpp), [Sif1.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/Sif1.cpp).

Eu só criaria essa build quando faltasse uma atribuição específica que o debugger não consegue oferecer, por exemplo: quem gravou um buffer EE, a origem IOP do pacote ou a associação entre uma DMA e um request. Buffer de eventos limitado, pin de revisão e checksum do binário são preferíveis a logs irrestritos por instrução.

Não há nesta revisão verificação de uma API oficial remota completa de debugging. Não assumir GDB/DAP/WebSocket pronto apenas porque existe PINE.

### 17.4 ps2autotests: quebrar o circuito de validar o modelo com ele mesmo

O repositório correto é `unknownbrackets/ps2autotests`. Ele contém programas EE/IOP e resultados esperados obtidos para comparação com PS2. É uma fonte útil para casos extremos de CPU, kernel, DMA, VIF e GS. Não executei seus binários nesta sessão. [ps2autotests](https://github.com/unknownbrackets/ps2autotests/tree/97469ffbed8631277b94e28d01dabd702aa97ef3).

Além do teste de tag IRQ citado em F03, [sleep.cpp](https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/kernel/ee/thread/sleep.cpp) exercita wakeups acumulados e prioridades. Usar um caso para validar uma hipótese concreta, sem importar indiscriminadamente outra suíte ou presumir que o runner GT4 já execute qualquer ELF homebrew.

Fixtures públicas do projeto podem reproduzir o contrato com código sintético original. Resultados de jogo e payloads continuam em `private/`/`generated/`.

### 17.5 PS2SDK: contratos SIF/kernel, com atenção à versão

É valioso para estruturas, layouts, handshake, callbacks e semáforos do lado EE/IOP. Não é prova automática da revisão exata do SDK usada pelo GT4. Versão de módulo, flags e formato de packet devem ser confrontados com o disassembly e os requests do alvo pinado.

A utilidade imediata nesta revisão foi corroborar o tempo estendido e separar bind/completion. Reutilizar nomes de campos confirmados é melhor que inventar structs por posição sem nome. [PS2SDK](https://github.com/ps2dev/ps2sdk/tree/ac92a9f657d2e531dd8f060250b07f2a5ac6dea5).

### 17.6 Ghidra Emotion Engine Reloaded e Version Tracking

O projeto original de extensão EE foi arquivado. O fork Reloaded declara suporte a MMI/VU0 macro, análise STABS e importação de savestate. Seu changelog v2.1.37 declara suporte a Ghidra 12.1.3; instalação e funcionamento efetivo neste ambiente continuam não verificados. Não instalei nada. [Extensão original](https://github.com/beardypig/ghidra-emotionengine), [Reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded/tree/ae013ee1475dc970db4fdeba3ec88def6b933d43), [changelog](https://github.com/chaoticgd/ghidra-emotionengine-reloaded/blob/ae013ee1475dc970db4fdeba3ec88def6b933d43/CHANGELOG.md).

O importador de savestate carrega memórias, incluindo scratchpad/VU, mas não é um replay completo de CPU/kernel; também preserva blocos executáveis existentes. A validação de texto runtime do M14 continua necessária. [PCSX2SaveStateImporter.java](https://github.com/chaoticgd/ghidra-emotionengine-reloaded/blob/ae013ee1475dc970db4fdeba3ec88def6b933d43/ghidra_scripts/PCSX2SaveStateImporter.java).

Version Tracking oferece um processo para correlacionar funções/dados entre programas. Eu usaria matches exatos e sinais estruturais para transportar nomes candidatos de outra revisão, mantendo validação por função no alvo. [Guia oficial Ghidra](https://github.com/NationalSecurityAgency/ghidra/blob/master/GhidraDocs/GhidraClass/Intermediate/Intermediate_Ghidra_Student_Guide.html).

### 17.7 GT4Hooks, PDTools e GT4FS

**GT4Hooks** é uma referência excelente de vocabulário e estruturas, mas mira GT4 Online US, `SCUS_974.36`, e não o alvo retail `SCUS_973.28`. Endereços e layouts não devem ser copiados por nome. [README](https://github.com/Nenkai/GT4Hooks/blob/d89e76bdf3d5846d86a54d57c9f146f4ee44033c/README.md), [FileDevice.c](https://github.com/Nenkai/GT4Hooks/blob/d89e76bdf3d5846d86a54d57c9f146f4ee44033c/source/gt4/GameFunctions/FileDevice.c).

**PDTools/GT4ElfBuilder** fornece evidência da reconstrução de CORE e dos metadados ELF/reginfo; o tratamento Online tem diferenças. Reconstruir ELF não substitui verificar o estado inicial produzido pelo loader. [README ElfBuilder](https://github.com/Nenkai/PDTools/blob/master/PDTools.GT4ElfBuilderTool/README.md), [GTImageLoader.cs](https://github.com/Nenkai/PDTools/blob/master/PDTools.GT4ElfBuilderTool/GTImageLoader.cs).

**GT4FS** está em `Razer2015/GT4FS`. Distingue variantes GT4/Online e tem um leitor de volume com TOC 3.1, páginas e transformações. Pode ser um parser independente para validar páginas/arquivos internos. Não presume, sozinho, o contrato do container externo 2.2 nem os bytes entregues pelo driver CD guest. [README GT4FS](https://github.com/Razer2015/GT4FS/blob/master/README.md), [Volume.cs](https://github.com/Razer2015/GT4FS/blob/master/GT4FS.Core/Volume.cs).

Procedimento para transportar nomes de uma revisão diferente:

1. Normalizar endereços imediatos do CFG/prologue quando apropriado.
2. Confrontar chamadas, constantes, strings, vtable e acessos a campos.
3. Verificar pelo menos um caminho dinâmico no alvo.
4. Registrar `candidate-name`, revisão de origem e confiança.
5. Promover somente o campo/função sustentado; não promover a classe inteira por um match parcial.

### 17.8 Outros recompiladores: estudar arquitetura, sem supor solução pronta

O **PS2Recomp** é experimental e declara tradução R5900 para C++ com runtime ainda parcial. Não é evidência de compatibilidade GT4. Seu `ps2xIOP` atual descreve execução dos IRX originais em interpreter R3000A, kernel virtual para imports, RAM IOP separada e serviços HLE com lifecycle condicionado a loading. É um estudo útil da opção híbrida IOP, não uma dependência recomendada automaticamente. [PS2Recomp](https://github.com/ran-j/PS2Recomp/tree/c5a9d02573410a2085a4b4b831b0b68ba3515440), [ps2xIOP](https://github.com/ran-j/PS2Recomp/blob/c5a9d02573410a2085a4b4b831b0b68ba3515440/ps2xIOP/README.md).

**N64Recomp** demonstra a separação entre tradução estática, metadados de funções e runtime. **N64ModernRuntime** implementa serviços libultra e faz a ponte com código recompilado, mantendo integração gráfica distinta. São referências de organização e contratos; diferenças de RSP/VI/OS para VU/GS/IOP impedem copiar a solução como resposta PS2. [N64Recomp](https://github.com/N64Recomp/N64Recomp/tree/ffb39cdad1da5de07eaaa48bd1db4a89a7986771), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime/blob/cdf5abbd5026fef5c364c676e4667c45e42b6863/README.md).

**Play!** é outra referência primária de ambiente PS2 com HLE. Pode oferecer uma implementação independente de contratos e nomes de serviços. Não o incorporaria ao runtime nem trataria os relatos históricos de seu autor como validação do GT4 pinado. [Play!](https://github.com/jpd002/Play-).

Antes de reutilizar código externo, revisar a licença e o contrato de dependência. Para esta investigação, o ganho principal vem de ideias e evidências, sem necessidade de copiar implementação.

## 18. Plano de observação independente: curta, calibrada e causal

### 18.1 Preparar uma execução comparável

Registrar:

- ISO/CORE pinados e hashes verificados;
- PCSX2: versão, revisão quando disponível e SHA-256 do binário;
- BIOS usado e hash, sem payload público;
- slowboot/fastboot, patches e configurações que alteram timing;
- região/idioma/OSD, memory cards e estado de pad;
- renderer e opções necessárias à reprodução;
- origem e validade de cada savestate.

O M14 já estabeleceu uma ponte de texto live importante. Reutilizar esse procedimento com o ISO original reduz a chance de atribuir ao runtime uma diferença de loader introduzida pelo ELF reconstruído.

### 18.2 Calibrar os instrumentos

Todo instrumento precisa de um controle positivo:

| Instrumento | Controle mínimo |
|---|---|
| Logpoint de execução | Função conhecida dispara; PC/RA/args são coerentes. |
| Watchpoint CPU | Uma escrita conhecida produz hit no caminho usado. |
| Watch DMA/host | Transferência conhecida aparece com origem e destino corretos. |
| Hook AOT | O hook funciona quando a mesma função está traduzida, não só no interpreter. |
| Snapshot | CPU pausada/ponto de save conhecido; operação concluída antes da leitura. |
| Parser de log | Número de eventos bruto e descartado registrado; truncamento detectado. |

Os slices já encontraram registradores stale em instrumentos de código traduzido. Acrescentar o endereço da instrução real e a origem do estado evita repetir essa armadilha. Um PC de entrada de módulo não é necessariamente o PC da escrita observada.

### 18.3 Escolher âncoras semânticas

Eu começaria com cinco pontos:

1. CORE live carregado e entry/início do programa.
2. Inicialização dos handlers e primeiros binds/loads IOP.
3. Abertura de um arquivo de som já verificado.
4. Abertura/copy-out da fonte `/fonts/system.fnt`.
5. Último predicado da main antes de estacionar ou de iniciar trabalho gráfico na referência.

Adicionar um sexto ponto para o time getter e um para os primeiros starts DMA dos canais relevantes. Não rastrear cada instrução de todo o boot antes de localizar o intervalo divergente.

### 18.4 Não comparar RAM inteira entre BIOS real e kernel HLE como primeira ferramenta

Comparação exata é apropriada entre dois motores do mesmo modelo, depois de ampliar o estado. Na referência com BIOS/allocator/scheduler distintos, endereços de heap, IDs e contagens de serviços podem divergir legitimamente.

Comparar primeiro transações e predicados:

- abertura do mesmo caminho e conteúdo entregue;
- bind/call do mesmo SID/função;
- relação entre request, reply e callback;
- completion DMA do mesmo trabalho;
- alteração do mesmo campo lógico;
- wake direcionado à mesma função/papel de thread;
- chamada subsequente que demonstra consumo do resultado.

Quando um objeto é correspondido, comparar seus campos pertinentes e validar o mapeamento. Não normalizar diferenças desconhecidas até o comparador aceitar tudo.

### 18.5 Formato de trace proposto

Exemplo de **schema**, não de captura real:

```json
{
  "sequence": 120,
  "run_id": "reference_or_model",
  "engine": "EE_AOT_or_EE_interpreter_or_IOP",
  "event": "rpc_reply_consumed",
  "guest_pc": "0x00000000",
  "caller_pc": "0x00000000",
  "thread_role": "candidate_role",
  "request_id": "local_correlation_id",
  "sid": "0x00000000",
  "rpc_number": 0,
  "source_address": "0x00000000",
  "destination_address": "0x00000000",
  "size": 0,
  "payload_sha256": "hash_of_private_capture",
  "timer_count": 0,
  "timer_mode": 0,
  "timer_compare": 0,
  "result_kind": "observed_or_modeled_or_unknown"
}
```

Os números são placeholders. Payloads devem permanecer em diretórios ignored. A documentação pública pode conter hashes, offsets, tamanhos e relações causais.

Separar sequence de clock/cycle: sequence registra ordem observada; tempo explica a agenda; não são unidades intercambiáveis. SIF serializa algumas relações, mas EE/IOP podem exigir uma ordem parcial por request/completion. Não alinhar loops repetidos apenas por proximidade textual.

## 19. Reconstrução do predicado da main: investigação que eu faria após a execução limpa

O censo documentado de 17 threads e as cadeias de wait já estreitaram a busca. O próximo passo deve ser o objeto/predicado que a main espera, não apenas a syscall em que ela está estacionada.

Pistas locais incluem a cadeia de flag/callback `0x00109340 -> 0x001097F0` e helpers de condição na região `0x005767E0`. A main está estacionada no helper compartilhado, com RA `0x0057689C` e `0x00109818` no terceiro frame da cadeia; esse último endereço não é o retorno imediato da syscall. Esses nomes são descritivos; validar as instruções live do alvo antes de qualquer hook. [Slice 22](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m32-slice22-cheapest-event.md:13), [slice 24](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m33-slice24-heartbeat-trace.md:88).

Procedimento proposto:

1. Identificar a comparação/loop que decide se a main pode sair do wait.
2. Registrar endereço do objeto, campo testado, valor esperado e estado observado.
3. Enumerar writers estáticos desse campo e chamadas indiretas/vtable que podem escrevê-lo.
4. Capturar writers dinâmicos no prefixo, atribuindo AOT/interpreter/host/DMA.
5. Na referência funcional, observar a escrita que torna o predicado verdadeiro.
6. Recuar até o acontecimento que causou a escrita: reply, timeout, worker, callback, arquivo, input ou evento GS.
7. Encontrar a primeira diferença nessa cadeia no modelo.

Se não houver writer estático direto, ampliar para aliases e gravações via memcpy/descritor; o histórico PRTS mostra por que o resultado pode nascer de uma callback e não do método de busca aparente.

**Critério de sucesso da investigação:** nomear o produtor, o dado recebido/gerado, a condição de habilitação e o primeiro ponto em que as execuções diferem. “Mais uma thread acordou” é evidência intermediária; “o próprio guest produziu o próximo request ou pacote esperado” é um resultado melhor.

## 20. HLE IOP, execução de IRX ou híbrido: como decidir

| Opção | Benefício | Custo/risco | Quando eu escolheria |
|---|---|---|---|
| HLE focado por serviço | Fatias pequenas, testes simples, estado legível | Protocolos privados podem acumular suposições | Um reply/evento concreto está documentado e o serviço tem estado limitado. |
| IOP interpretada com IRX e kernel virtual | Executa lógica original e reduz reinvenção de protocolos | Imports, scheduler, DMA e hardware IOP viram novo projeto | Vários produtores privados dependem de lógica IRX que é mais difícil reimplementar que executar. |
| Híbrido | IRX para serviço complexo, HLE para contratos conhecidos | Precisa de ownership claro por SID e lifecycle | A fronteira entre os dois está explícita e não há duas respostas concorrentes ao mesmo request. |

Eu começaria com o primeiro serviço causalmente necessário, sem planejar uma IOP universal antes de saber sua dependência. Um protótipo de execução IRX precisa demonstrar:

- loader/relocações/imports necessários;
- RAM IOP separada e tradução explícita de dados no SIF;
- registro real de servidor e disponibilidade depois do load;
- reset/stop invalidando servidores/handles pertinentes;
- request/reply com equivalência verificável;
- agenda de execução determinística e snapshot de estado;
- erro útil em import/hardware desconhecido.

Pontos de falha comuns a evitar: espelhar endereço IOP diretamente em EE, bind bem-sucedido antes do módulo existir, execução do `_start` tratada como inicialização completa, syscall/import desconhecido retornando sucesso e completion sem dado válido.

Não há incompatibilidade conceitual entre EE AOT e IOP interpretada. A missão do projeto não exige que todo processador e todo serviço sejam recompilados estaticamente desde a primeira fase. A decisão deve registrar a razão e os limites do híbrido.

## 21. Gráficos: caminho futuro, com gate claro

O produto jogável exigirá a cadeia que gera e consome trabalho gráfico. Eu não começaria por uma abstração de renderer sem haver tráfego confirmado.

Sequência de marcos que eu usaria:

1. Guest chega naturalmente ao primeiro trabalho gráfico esperado.
2. DMA relevante transfere/observa um payload com destino e tamanho válidos.
3. Tags e comandos VIF/GIF são identificados e o consumo é verificado.
4. VU necessária executa ou uma substituição por rotina específica é sustentada por equivalência.
5. GS recebe writes/primitives coerentes e produz um primeiro frame verificável.
6. Frame/evento de exibição se conecta à agenda que o guest espera.
7. Só então ampliar casos, resolver qualidade visual e medir desempenho.

Antes do renderer completo, um consumidor de pacotes com validação de estrutura, contadores e hashes pode separar “EE não produz trabalho” de “trabalho produzido mas não interpretado”. Não afirmar que capturar um pacote GIF prova que o frame esteja correto.

A mesma disciplina vale para áudio/IPU e movie path: formatos, streaming, completion e eventos têm contratos próprios. A pasta `/mpeg` observada no boot não define, sozinha, quais decoders e pipelines serão necessários imediatamente.

## 22. Roadmap de slices sugerido

Os nomes R1–R8 são propostas deste feedback, não milestones já existentes e não tarefas executadas.

### R1 — Contrato dos timers e revisão das conclusões temporais

**Pergunta:** o timer observado pelo guest corresponde ao contrato de 16 bits, flags e compare usado pela sua biblioteca?

**Entrega:** nota com fonte independente, fixtures de MMIO/advance/ack e implementação mínima coerente. Reaproveitar CTest existente. Registrar a política de quantização separadamente do contrato de registrador.

**Aceitação:** exemplos de wrap/compare/W1C passam; leitura de tempo composta é coerente; snapshot não muda semântica; assertions antigas incompatíveis são revisadas com justificativa.

**Cuidado:** não usar checkpoint temporal antigo como evidência do comportamento corrigido.

### R2 — Fronteiras explícitas e `jr ra`/`ERET`

**Pergunta:** o driver distingue o que já foi executado do que ainda precisa ser tratado?

**Entrega:** saída de módulo explícita e legível, captura do target de retorno antes do slot e regressões end-to-end.

**Aceitação:** JR com RA alterado, ERET para word comum, trap com PC==RA e indirect transfer não perdem contexto nem repetem slot. Comparar com interpreter e resultado manual.

### R3 — Completion DMA e pending/ack/mask

**Pergunta:** cada transfer gera a família/canal corretos e a entrega respeita a elegibilidade definida?

**Entrega:** roteamento DMAC dos canais, semântica mínima de status/máscara/ack e distinção de VIF command IRQ. Revisar ADR0011 e teste que depende de TIE.

**Aceitação:** completion sem TIE observável; GIF não chama Timer0; mask impede dispatch sem apagar status; ack não relança flag por escrita interna.

### R4 — Comparação de máquina e provenance de checkpoint

**Pergunta:** o diferencial e o resume verificam também o estado que determina o futuro da execução?

**Entrega:** snapshots canônicos comparados, diagnóstico por componente e identidade de inputs/modelo/tradução nos checkpoints ou sidecars validados.

**Aceitação:** mudanças isoladas em scratchpad/kernel/device são detectadas; estado incompatível é recusado com motivo; contexto transitório permanece protegido.

### R5 — Prefixo novo e captura independente

**Pergunta:** qual é a primeira condição divergente depois das correções dos contratos conhecidos?

**Entrega:** run novo desde entry, checkpoints novos, âncoras semânticas no PCSX2 e trace limitado. Começar com logpoints oficiais, usar build instrumentada somente se necessário.

**Aceitação:** relatórios distinguem model-to-model de model-to-reference; inputs/configuração registrados; ausência de evento tem controle positivo; próxima diferença está localizada.

### R6 — Produtor da condição da main / primeiro serviço causal

**Pergunta:** quem escreve o campo/flag que permite à main avançar?

**Entrega:** backward slice do predicado até request/timeout/evento, com writer e consumidor observados.

**Aceitação:** implementação de serviço se justifica por resultado independente; trabalho guest seguinte aparece naturalmente. Se a hipótese falhar, registrar o falsificador e escolher a alternativa.

### R7 — Escolha HLE/IRX do produtor e primeiro tráfego gráfico

**Pergunta:** o próximo protocolo é pequeno o bastante para HLE verificável ou exige executar lógica IOP original?

**Entrega:** decisão com recorte de dependências e contrato por SID; ou, se o gate já for gráfico, captura/consumo do primeiro pacote.

**Aceitação:** lifecycle correto e evento natural; sem sucesso genérico nem wake artificial.

### R8 — Pipeline visual e performance medida

**Pergunta:** a execução produz frames e responde a eventos/input corretamente, com custo aceitável?

**Entrega:** marcos gráficos pequenos, testes de conteúdo e métricas de trabalho AOT/interpreter/runtime.

**Aceitação:** resultado visual/evolução guest demonstrados; otimização parte de perfil. Não promover “compila rápido” ou “muitos serviços por segundo” a desempenho jogável.

## 23. Matriz de hipóteses para orientar o worker

| ID | Hipótese causal | Confiança atual | Experimento que discrimina | O que a enfraquece/falsifica |
|---|---|---|---|---|
| H1 | A largura/flags do timer produzem tempo guest inválido e delays errados | Alta para diferença de contrato; causalidade aberta | Fixtures + prefixo limpo + captura time getter | Contrato corrigido reproduz os mesmos predicados e a referência confirma a mesma evolução temporal. |
| H2 | Reply IOP anterior incompleto impediu construir o produtor necessário | Plausível | Comparar requests/replies e primeira escrita do predicado | Todos os resultados pertinentes coincidem e o produtor inicializa igual. |
| H3 | DMA/IRQ anterior incorreta colocou inicialização em estado errado | Plausível após achado de roteamento | Trace dos primeiros starts, D_STAT/INTC, handlers e flags | Caminho não é executado ou seus efeitos não participam do gate; referência demonstra equivalência local. |
| H4 | A main aguarda um evento de input por DBCMAN/PAD2 | Aberta | Identificar cliente, buffers e writer na referência | Input não participa da condição ou referência avança com o mesmo estado neutro/ausente. |
| H5 | Falta preempção/oportunidade de entrega dentro de trecho AOT | Aberta | Medir trecho sem saída e ordem de eventos no ponto causal | Não há trecho relevante ou delivery nas fronteiras atuais é suficiente. |
| H6 | O job pool mapeado é o gate central gráfico | Fraca sem caller dinâmico | Mostrar criação/caller na referência antes do gate | Referência avança sem criar esse pool ou ele pertence a outro subsistema. |
| H7 | Outro erro de serviço/arquivo ainda produz objeto semanticamente errado | Aberta | Hash do arquivo/chunks consumidos e campos do objeto | Dados e consumidores relevantes coincidem. |
| H8 | Restante de COP2/BC0F é o próximo gate | Dependente de reachability | Observar PC/opcode alcançado | O caminho nunca é alcançado antes da condição divergente. |

Um run que continua parado depois de corrigir H1 não torna a correção inútil. Ele remove uma variável incorreta e torna o teste de H2–H8 mais interpretável.

## 24. Critérios de evidência e de encerramento de uma fatia

Cada slice futuro deveria terminar com uma pergunta respondida ou uma hipótese falsificada, não somente mais distância de execução.

Checklist técnico proposto:

- input e revisão pinados;
- função/endereço/offset identificado;
- instrumento calibrado;
- fato observado separado da inferência;
- contrato externo ou consumidor guest que justifica o comportamento;
- teste sintético de regressão com falsificador;
- comparação interna com escopo explícito;
- uma observação de progresso natural ou de persistência do bloqueio;
- documentação com limites e próximo experimento.

São os critérios de evidência do projeto aplicados ao recorte desta revisão, não uma exigência de aprovação adicional ao dono. Neste arquivo eles aparecem como recomendações para o trabalho futuro autorizado pelo dono.

Não aumentaria budgets sucessivamente sem uma pergunta nova. A longa marcha já mostrou uma fronteira estacionária; o próximo ganho vem de melhor observação e contratos, não da repetição do mesmo estado por mais tempo.

## 25. Métricas que eu colocaria no relatório de cada run

| Métrica | Utilidade |
|---|---|
| Run/input/model/module IDs | Reproduzir e comparar corretamente. |
| Motivo de parada explícito | Distinguir budget, wait, unsupported, trap e falha de contrato. |
| Chamadas AOT e passos interpretados | Quantificar o bridge; sem tratá-los como mesma unidade. |
| Instruções/blocos guest executados, se medidos | Trabalho real, separado de número de serviços. |
| Tempo guest por origem | Detectar clocks avançados por políticas diferentes. |
| IRQ: origem, pending, elegível, entregue, ack | Separar evento de callback e reduzir falsa causalidade. |
| RPC por SID/função/resultado modelado | Identificar sucesso provisório e protocolo não coberto. |
| Threads por papel e predicado | Encontrar quem espera por quem e por qual dado. |
| DMA starts/completions/bytes observados | Verificar atividade real da pipeline. |
| Primeiro/último writer relevante | Vincular o estado final à causa. |
| Logs truncados/descartados | Tornar ausência de evidência interpretável. |

Métricas precisam ser baratas no caminho normal. Trace pesado deve ser ativado por região, evento ou condição, com limites explícitos. Otimizar depois de encontrar um perfil repetível, preservando um modo de investigação legível.

## 26. Referências externas e revisões consultadas

Consulta realizada em 2026-10-04. Os links pinados acima são preferíveis a `master/main` para repetir esta análise. Links sem pin abaixo foram consultados como leads/documentação; pinar o trecho específico antes de usá-lo como contrato de implementação.

| Projeto/fonte | Revisão consultada quando fixada | Papel nesta análise |
|---|---|---|
| PCSX2 | `81526d4dc7cc70e4ae75abb35a789417456c6d43` | Timer, MMIO/IRQ, DMA, SIF e debugger/PINE. |
| PS2SDK | `ac92a9f657d2e531dd8f060250b07f2a5ac6dea5` | Tempo estendido e ABI/fluxo SIF RPC. |
| ps2autotests | `97469ffbed8631277b94e28d01dabd702aa97ef3` | Evidência de comportamento de hardware para testes focados. |
| PS2Recomp | `c5a9d02573410a2085a4b4b831b0b68ba3515440` | Arquitetura AOT e estudo do híbrido IOP. |
| N64Recomp | `ffb39cdad1da5de07eaaa48bd1db4a89a7986771` | Separação entre tradução e runtime. |
| N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` | Organização de serviços e ponte com código recompilado. |
| GT4Hooks | `d89e76bdf3d5846d86a54d57c9f146f4ee44033c` | Vocabulário/estruturas com diferença retail vs Online. |
| Ghidra EE Reloaded | `ae013ee1475dc970db4fdeba3ec88def6b933d43` | Disassembly/decompile e importação de memória de savestate. |
| PS2tek | Fonte `psi-rockin/ps2tek`, revisão `1c9166066c3ab9ad089a4d12088acaa9476df4b6`; página publicada consultada | Referência de RE, corroborada por implementação/testes quando disponível. |
| PDTools | Links de código/README consultados em `master` | Reconstrução e diferenças de versão. |
| Razer2015/GT4FS | Links de código/README consultados em `master` | Parser independente de inner archive. |
| Play! | Repositório e log histórico do autor | Segunda abordagem HLE e pista DBCMAN/PAD2. |

PCSX2, PS2SDK e os demais repositórios são implementações/documentação dos próprios autores. PS2tek é uma referência de engenharia reversa, não um manual oficial Sony. Não confundir essas categorias. Nenhum repositório citado foi instalado ou incorporado ao checkout.

Não baseei as recomendações em uma suposta compatibilidade comercial anunciada por projetos experimentais. O valor está nos contratos observáveis, testes e pontos de instrumentação específicos.

## 27. Instrução pronta para encaminhar ao agente operador

> Releia este feedback junto do STATUS atual. Antes de continuar o tripwire ou acrescentar estímulos, confirme o contrato EE timer: COUNT/COMP de 16 bits, wrap, flags EQUF/OVFF, W1C e compare coerente entre avanço idle e por serviço. Separe operações internas de device de writes guest e restore. Preserve checkpoints antigos como evidência; uma mudança semântica de tempo exige prefixo novo e checkpoints identificados. Em fatia separada, capture o target de `jr ra` antes do delay slot e torne o motivo de saída AOT explícito, com testes emitter/módulo/driver para JR com RA alterado, ERET já aplicado e trap com PC==RA. Audite o DMA de BootDevices: VIF0/VIF1/GIF completam pelo DMAC 0/1/2, não INTC 4/5/9, e TIE não é enable geral de completion. Amplie a comparação para scratchpad/kernel/dispositivos com os snapshots existentes. Depois compare uma execução nova com PCSX2 em âncoras semânticas e identifique o produtor do predicado que bloqueia a main. Nenhuma dessas diferenças deve ser promovida a causa do stall antes do experimento; nenhuma resposta RPC genérica ou wake artificial deve ser promovida a progresso válido. Trabalhe em fatias verificadas e documente limites/hipóteses conforme AGENTS.md quando a retomada for autorizada pelo dono.

## 28. Resumo mastigado para o dono

O projeto já fez uma parte difícil: transformar muito código do processador principal em C++ e fazer o jogo percorrer uma longa inicialização. Isso é progresso real. Ainda falta garantir que o ambiente que esse código enxerga se comporte como o PS2 nas partes necessárias.

A revisão encontrou algo mais concreto que simplesmente “falta um evento”: o relógio modelado parece ter o tamanho errado, algumas conclusões sobre esperas dependem desse relógio, as finalizações DMA usam o caminho de interrupção errado e há casos de retorno que o tradutor/driver podem confundir.

Minha recomendação é conferir e corrigir essas peças pequenas primeiro, com testes claros. Depois reiniciar a execução do zero e observar, lado a lado com PCSX2, qual informação o jogo recebe no PS2 e não recebe no projeto. Isso tende a economizar trabalho porque indica exatamente o próximo serviço ou dispositivo necessário.

**Não dá para prometer que corrigir o relógio destrava o jogo. Dá para afirmar que investigar o bloqueio com um relógio incompatível aumenta o risco de perseguir a hipótese errada.** O objetivo da próxima etapa deve ser descobrir a primeira diferença que importa para a main avançar.

Depois da autorização adicional, a suíte e testes focados foram executados. Os resultados mostram que a suíte atual passa e que os defeitos específicos também são reproduzíveis; portanto, eles precisam de novas regressões quando forem corrigidos. O código de produção continua intacto, não houve commit/push e os instrumentos temporários permanecem em diretório ignored. A seção seguinte documenta a validação para o agente operador.

## 29. Validação experimental autorizada após a revisão

**Data:** 2026-10-04; registro consolidado por volta de `05:54 UTC` / `02:54 America/Sao_Paulo`.  
**HEAD:** `75ea3a1b523d2c070ed56b7a605b6b9de1a3c270`, inalterado.  
**Autorização:** o dono permitiu rodar testes e informou que o agente operador estava parado. Isso foi usado para validação; não para corrigir o runtime ou reiniciar seus milestones.

### 29.1 Ambiente e escopo efetivamente executado

| Item | Observado |
|---|---|
| CMake / CTest | 4.3.3 |
| Build | Debug, Ninja, MSVC x64; cache usa MSVC `19.44.35228.0` e toolset `14.44.35207` |
| Developer shell | VS 2022 BuildTools 17.14.34, amd64/amd64 |
| Python | 3.14.5, venv existente `private/tooling-venv` |
| Probes C++ | `/std:c++20 /EHsc /MDd /Od /Zi`, link à biblioteca Debug existente |
| Inputs | ISO e CORE pinados, revalidados antes do uso pela suíte |
| Mudanças permanentes de source/config | Nenhuma |
| Área dos probes | `private/gpt-feedback-validation/20261004-verify/`, ignored |
| Área dos logs Python/input | `private/gpt-feedback-validation/20261004-python/`, ignored |

`cmake --build build` e `cmake --build build --target gt4boot` terminaram com exit 0 e `ninja: no work to do`. Isso estabelece que a árvore incremental estava atualizada; **não foi um clean rebuild**. O target explícito é relevante porque `gt4boot` é excluído do build default. A fixture CTest de build também passou.

Os probes foram compilados de fato, sem warnings exibidos nos logs de compilação. Eles não alteram o decoder, emitter, interpreter, driver ou kernel. O script geral de reprodução é legível e deixa todos os outputs no próprio scratch: [run-probes.ps1](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/run-probes.ps1).

### 29.2 Inputs: verificação independente do uso posterior

Foram recalculados via `Get-FileHash -Algorithm SHA256` o ISO de 5.314.478.080 bytes e o CORE extraído de 2.020.861 bytes. Ambos corresponderam aos hashes da seção 3.3.

Em seguida, o verificador já existente foi executado:

```powershell
& '.\private\tooling-venv\Scripts\python.exe' -B scripts/gt4disc.py verify '.\Gran Turismo 4 (USA) (v2.00).iso'
```

Resultado, exit 0:

```text
PASS: SCUS-97328 / VER 2.00; ISO and all three file fingerprints match
```

Isso verifica ISO e os fingerprints internos de CORE, loader e SYSTEM.CNF sem alterar o manifesto. Não foi usado para revalidar os ELFs reconstruídos nem BIOS. [Log](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-python/inputs-verify.log).

### 29.3 Suíte existente: verde, com alcance explícito

Comandos executados no ambiente de desenvolvimento:

```powershell
cmake --build build
cmake --build build --target gt4boot
ctest --test-dir build --output-on-failure -j 1 --timeout 300
& '.\private\tooling-venv\Scripts\python.exe' -B -m unittest discover -s tests/python
```

| Validação | Resultado observado |
|---|---|
| CTest | **50/50 passed**, exit 0; tempo reportado **69,08 s** |
| Python unittest | **Ran 73 tests**, **OK (skipped=6)**, exit 0; **67,604 s** |
| `gt4boot_services` | Passed, **18,01 s**, 90.000 serviços com disco e comparação interna conforme comando registrado |
| `gt4boot_originating` | Passed, **1,65 s**, fixture de 20.000 serviços |
| `gt4boot_resume_verify` | Passed, **10,73 s** |
| `gt4boot_autosave_verify` | Passed, **10,48 s** |

Os testes foram executados serialmente no CTest para respeitar dependências de fixtures e outputs compartilhados. Checkpoints/autosaves usados pela suíte estão nos seus paths de teste; as marchas históricas de 1980k/243m não foram retomadas nesta validação.

O processo Python emitiu duas `ResourceWarning` sobre sockets locais não fechados. Elas não fizeram a suíte falhar, mas o resultado não deve ser descrito como execução sem qualquer warning. Não investiguei/corrigi o leak nesta revisão porque não se relaciona aos contratos prioritários. Os seis skips também permanecem exclusões de cobertura; não representam casos aprovados.

Logs: [CTest](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/ctest.log), [Python](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-python/python-tests.log), [build default](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/build.log), [build gt4boot](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/build-gt4boot.log).

**O verde da suíte e os defeitos reproduzidos coexistem.** As expectativas atuais não cobrem esses casos, e alguns testes fixam a aproximação antiga de DMA. O resultado não justifica remover os achados; justifica acrescentar regressões quando houver correção autorizada.

### 29.4 Tradução: fixture usa o emitter real, não uma cópia manual dele

[generate_repro.cpp](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/generate_repro.cpp) inclui o source real do tradutor com `wmain` renomeado. Invoca `collect_units` e `emit_unit_body` sobre um `ImageRecord` de **words sintéticos originais**, sem carregar CORE artificial ou contornar o verificador de input.

O resultado [generated_repro.hpp](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/generated_repro.hpp) é compilado por [run_repro.cpp](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/run_repro.cpp). Esse runner usa as classes reais `Interpreter`, `Driver`, `ModuleEntry` e `ServiceTable` da biblioteca do projeto.

Isso distingue o experimento de escrever à mão uma versão supostamente igual ao emitter: os corpos JR, ERET e BREAK são produzidos pela implementação atual.

#### JR com escrita de RA no delay slot

Words em `0x1000/0x1004`: `03E00008`, `241F2000`; RA inicial `0x3000`.

```text
JR native_pc=0x2000 reference_pc=0x3000 native_ra=0x2000 reference_ra=0x2000 reproduced=1
```

**Confirmed — execução:** o controle nativo tem destino incorreto; ambos aplicam a escrita de RA do slot. A diferença prevista em F05 foi reproduzida literalmente. Isso não depende de hardware/timing/IOP nem de payload do jogo.

#### ERET em EXL e ERL

Word `42000018` em `0x1010`; RA `0x2000`; EPC ou ErrorEPC `0x3000`; no destino existe uma instrução ordinária. Emitter e interpreter produzem PC `0x3000` e limpam o nível correspondente, mas o driver classifica a saída do módulo como InstructionStop e não executa nenhum passo do bridge.

```text
EXL native_pc=0x3000 reference_pc=0x3000 InstructionStop=1 bridge_steps=0 reproduced=1
ERL native_pc=0x3000 reference_pc=0x3000 InstructionStop=1 bridge_steps=0 reproduced=1
```

**Confirmed — execução:** o efeito do ERET está correto nesses casos; a integração com o driver interrompe a continuação. Não promover isso a erro da implementação aritmética/CP0 do ERET.

#### BREAK quando PC coincide com RA

Word `0000000D` em `0x1014`, RA `0x1014`, budget 10. O emitter real retorna parado no BREAK. O driver interpreta PC==RA como Returned, chama repetidamente a mesma entrada e termina em StepLimit; o interpreter identifica a exceção BREAK.

```text
BREAK pc_equals_ra native_StepLimit=1 native_module_calls=10 interpreter_BreakException=1 reproduced=1
```

**Confirmed — execução:** a ambiguidade prevista no comentário do driver não é somente teórica; uma fixture mínima oculta o trap e produz dez chamadas de módulo. Reachability desse caso no GT4 permanece Unknown.

#### Controle hipotético de JR corrigido

Foi executada separadamente uma pequena função manual que captura o target antes do slot. **Esse controle não é uma alteração do emitter e não deve ser chamado de fix testado no produto.** Ele demonstra a interação com o driver: PC `0x3000`, RA `0x2000`, estado de retorno correto, mas classificado como InstructionStop.

```text
CONTROL corrected_order_JR native_pc=0x3000 ra=0x2000 InstructionStop=1 reproduced=1
```

Essa observação sustenta tratar a captura de target e o protocolo explícito de saída conjuntamente, em vez de corrigir somente a ordem local.

O runner retorna zero quando reproduz **todos os defeitos previstos**, incluindo o controle; zero aqui significa reprodução bem-sucedida, não contratos corretos. [Output consolidado](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/translation-repro.log).

### 29.5 Timer/DMA/IRQ: probes pela API real do projeto

[runtime_contract_probe.cpp](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/runtime_contract_probe.cpp) usa `GuestMemory`, `TimerUnit`, `Kernel`, `RegisterBank` e `DmaChannel` reais. As expectativas são os contratos externos discutidos em F01–F04; o probe não implementa outra máquina PS2 como oráculo.

Flags de timer foram semeadas via snapshot restore para evitar a circularidade de usar uma escrita guest para simular o acontecimento que levanta a flag. O avanço de tempo é realizado pelos métodos reais do kernel.

| Probe | Observado no modelo atual | Expectativa específica | Resultado |
|---|---|---|---|
| COUNT escrito `ABCD1234` | Lê `ABCD1234` | Bits significativos `1234` | Diferença |
| COMP escrito `ABCD5678` | Lê `ABCD5678` | Bits significativos `5678` | Diferença |
| MODE pendente `C80`, guest write `80` | Lê `80` | Flags pendentes preservadas: `C80` | Diferença |
| MODE pendente `C80`, guest write `C80` | Lê `C80` | Acknowledge das duas flags: `80` | Diferença |
| COUNT `FFF0`, CLKS=2, um serviço | COUNT `10230` | Wrap de 16 bits: `230` | Diferença |
| Mesmo avanço, OVFF | `0` | `800` | Diferença |
| Mesmo avanço, OVFE habilitado | Nenhum request enfileirado | Pedido correspondente ao overflow no modelo | Diferença |
| Idle, COUNT 0, COMP FFFF, CLKS=2 | COUNT `2580` | 9.600 ticks: `2580` | Controle confere |
| Mesmo idle, EQUF | `400` | `0`, pois não cruzou COMP | Diferença |
| DMA normal sem TIE | STR limpo | STR limpo | Controle confere |
| DMA normal sem TIE | Callback não chamado | Completion deve existir | Diferença |
| DMA normal com TIE | Callback chamado uma vez | Completion existe | Controle confere |
| Wiring GIF atual, sem handler | D_STAT canal 2 permanece zero | Causa DMAC 2 pendente | Diferença |
| INTC causa 4 sem handler | INTC_STAT permanece zero | Status da causa conserva bit `10` | Diferença |

São 14 observações, com 11 diferenças e 3 controles que conferem. Não são 14 novos CTests permanentes; são probes de diagnóstico. Seu exit 0 indica execução concluída e não implica aprovação dos contratos; cada linha `DIFFERENCE` é um resultado que precisa ser levado à implementação futura.

Limites importantes:

- O caso de overflow verifica a fila do modelo, não mede uma interrupção entregue por hardware físico. A ausência do wrap/OVFF é a evidência direta mais fundamental.
- O caso GIF **reconstrói o wiring inspecionado de BootDevices** (`cause=9 -> raise_interrupt`), porque BootDevices é anônimo em outro translation unit. Não executa o boot inteiro nem prova uma transferência GIF alcançada pelo GT4. A observação de roteamento mantém sua base estática mais esse teste de componentes.
- O defeito de pending status sem handler foi executado diretamente pelo ramo **INTC**. O ramo DMAC tem a mesma ordem problemática confirmada por inspeção, mas `queue_dmac_completion` é privada e não foi acessada artificialmente para essa fixture.
- Não foram simulados payload, tags de cadeia, VIF commands, processamento VU/GS ou preempção.

Output completo: [runtime-contracts.log](C:/Antigravity/gt4-staticrecomp/private/gpt-feedback-validation/20261004-verify/runtime-contracts.log). As diferenças de COUNT/flags/wrap e TIE foram reproduzidas sem depender do disco.

### 29.6 Identidade dos binários e instrumentos

SHA-256 calculado depois da execução. Estes valores identificam o que foi usado/gerado localmente; não são novos manifests autorizados de input.

| Artefato | SHA-256 |
|---|---|
| `build/gt4boot.exe` | `21d78123bf5db1e48367e93a8983fabd096b3f8515b928902b61fafe4853c424` |
| `build/gt4translate.exe` | `b1f30d1fec72f4c9f4408fe6ca90b758243c409e0e01a72d4f2425a8a9a80390` |
| `build/gt4recomp_decode.lib` | `1fbef1383851abdd4e4259dd0babfac723ea9f1b6b133a2abdd7eb3b030b2e25` |
| `generate_repro.cpp` | `a2bb7bb4fc5fe5fff281b7f48417ab27d3930755fb89cf22e301b7c321f638bf` |
| `generated_repro.hpp` | `742a699b822ad8f5ff572260db5294e4c3105252571e5ab1e7f711bbe3c619d4` |
| `run_repro.cpp` | `dfe325aa0ea813fe7660a46587599d9436cba3474f722931223ccfb72ca3dff2` |
| `runtime_contract_probe.cpp` | `60529274885ca27d793c6df9fe5f440f5656b43b1cbe07efea3a6fdecad84e67` |

Os paths de todos os instrumentos estão linkados nas subseções anteriores. Fontes e logs permanecem ignored e disponíveis para reprodução local. O header gerado nesse experimento contém somente os words sintéticos originais, não tradução extraída de GT4.

### 29.7 Como estes testes mudam a recomendação

Os testes fortalecem R1–R4: há casos executados que tornam inadequado avançar atribuindo toda a fronteira a “falta um estímulo”. Eles também deixam claro que a suíte verde não substitui regressões de contrato e que a interface módulo/driver precisa ser corrigida junto da semântica local de retorno.

**O que continua Unknown:** quais desses defeitos foram exercidos no boot longo do jogo e quais alterações mudarão o predicado da main. Não corrigi o modelo, não retomamos a marcha histórica e não foi feita uma nova captura funcional do PCSX2 ou de console físico.

Próxima experiência recomendada ao agente operador, quando sua retomada for autorizada: corrigir uma fatia de contrato com regressões permanentes, registrar a compatibilidade de checkpoint e comparar um prefixo fresco com o estado anterior e com uma âncora independente. Conservar estas fixtures para demonstrar o antes/depois, alterando a expectativa de reprodução de defeito para a semântica correta quando integrar os testes à suíte existente.

# PLAN.md — Plano de continuidade do GT4Recomp

> Atualização de orientação (2026-10-09): este plano estratégico foi escrito
> na baseline abaixo, não descreve a fronteira operacional atual. Para retomar
> após trocar de modelo, leia `docs/RETOMADA.md`, `docs/STATUS.md` e `AGENTS.md`.
> A slice100 está encerrada; o menu ainda não foi alcançado pela produção.

**Data:** 2026-10-04  
**Baseline consultada:** 9fc08c4cd3dcec06a11df4c6c6622b1087b3e8e1  
**Alvo:** Gran Turismo 4 USA v2.00, SCUS-97328, Windows x86-64, C++20.  
**Primeiro objetivo do dono:** o jogo dar boot, gerar imagem e chegar ao menu inicial.  
**Primeira entrega funcional recomendada:** menu original visível e navegável com controle.  
**Natureza deste documento:** planejamento técnico; não representa implementação nem novos testes executados.

Este plano consolida a revisão do repositório, o GPT_FEEDBACK.md, o OPUS_FEEDBACK.md, a discussão das ressalvas ao Opus e a triagem posterior. Ele cobre o caminho até um jogo utilizável, mas detalha principalmente a próxima entrega: **menu inicial com imagem real e entrada funcional**.

Depois dessa entrega, o dono e o agente devem revisar prioridades. As fases posteriores constituem uma direção técnica, não um compromisso com uma ordem imutável.

---

## 1. Direção principal

Preservar o recompilador estático EE já construído e completar, por evidência de uso, os contratos de execução e os serviços PS2 que o jogo exige.

O próximo avanço deve vir de uma sequência verificável:

~~~text
contratos básicos corretos
    → identificação da primeira divergência relevante
    → inicialização e serviços necessários
    → dados gráficos produzidos pelo jogo
    → processamento VIF/VU/GIF/GS
    → imagem apresentada numa janela
    → menu navegável
~~~

Uma execução longa sem exceção não demonstra que o jogo chegou ao menu. Uma thread acordada não demonstra que o evento recebido estava correto. Uma janela aberta não demonstra que a renderização do jogo funciona.

O plano diferencia esses resultados para evitar anunciar progresso maior do que a evidência permite.

### 1.1 Prioridades do primeiro objetivo

1. Corrigir contratos comprovadamente defeituosos.
2. Tornar desconhecidos e divergências observáveis.
3. Identificar a causa concreta que impede a inicialização de continuar.
4. Implementar o caminho mínimo correto até a geração de gráficos.
5. Apresentar os gráficos produzidos pelo jogo recompilado.
6. Entregar entrada ao protocolo guest correto.
7. Demonstrar o menu original funcionando.

### 1.2 O que pode esperar

Não são requisitos para a primeira entrega:

- resolução alta;
- ultrawide;
- desempenho máximo;
- substituição extensiva de funções por implementações host;
- suporte a outras revisões do GT4;
- compatibilidade com todos os volantes;
- cobertura de todos os carros, pistas e modos;
- arquitetura genérica capaz de executar qualquer jogo de PS2.

Desempenho suficiente para demonstrar e usar o menu importa. Otimizações amplas devem esperar medições de um caminho funcional.

---

## 2. Escopo e arquitetura que devem ser preservados

A missão continua sendo recompilar antecipadamente o código R5900 do alvo pinado para C++20, com um runtime próprio fornecendo o ambiente necessário.

### 2.1 Fronteiras arquiteturais

| Componente | Responsabilidade |
|---|---|
| Input e reconstrução | Verificar revisão, reconstruir a imagem e preservar a identidade dos bytes carregados. |
| Decoder e análise | Decodificar, identificar fluxo, funções e fronteiras com evidência. |
| Semântica EE | Representar registradores, memória, operações e exceções com comportamento definido. |
| Tradutor AOT | Emitir C++ legível e preservar a ordem dos efeitos guest. |
| Driver e bridge | Continuar execução entre entradas traduzidas, serviços e fronteiras interpretadas. |
| Kernel/runtime | Threads, semáforos, handlers, interrupções, tempo e serviços BIOS necessários. |
| Ambiente IOP | Serviços, transporte SIF, disponibilidade, dados e eventos assíncronos. |
| Dispositivos | DMA, VIF, VU, GIF, GS, IPU e áudio no recorte realmente alcançado. |
| Plataforma Windows | Janela, apresentação, controle, teclado, áudio, arquivos e configuração. |

PCSX2 deve continuar como ferramenta de observação e comparação independente. Incorporá-lo como motor de execução mudaria a missão do projeto.

Interpretar um processador auxiliar, como IOP ou VU1, pode ser compatível com a missão EE AOT. Essa escolha exige uma fronteira explícita e não implica substituir o EE recompilado por emulação integral.

### 2.2 Princípios permanentes

- Estado guest explícito.
- Inteiros de largura definida.
- Endereços guest separados de ponteiros host.
- Wrapping e exceções implementados deliberadamente.
- Código humano legível, inclusive o gerado.
- Inputs pinados e verificados.
- Nenhum sucesso genérico para serviço desconhecido.
- Nenhum evento produzido apenas para acordar a thread desejada.
- Nenhum payload de jogo, BIOS ou captura com bytes do jogo no Git.
- Comparação interna e referência independente têm papéis diferentes.

O roteiro existente está em [requirements.md](C:/Antigravity/gt4-staticrecomp/docs/requirements.md). Os números históricos M31–M37 permanecem válidos como referências; este documento usa gates G0–G7 para tornar as entregas mais claras, sem renumerar o histórico.

---

## 3. Estado atual: o que já existe

### 3.1 Base técnica estabelecida

| Área | Estado conhecido | Limite da conclusão |
|---|---|---|
| Inputs | Uma revisão USA v2.00 identificada por manifests e hashes. | Não estabelece compatibilidade com outra revisão. |
| Reconstrução | Reconstrução própria, comparação de payloads e observação do texto carregado. | Layout de análise e estado real de inicialização precisam continuar distintos. |
| Decoder | 349 operações no inventário atualizado. | Contagem de operações não mede conclusão do jogo. |
| Código não suportado | 30 words na região identificada como código real. | Sua participação no próximo bloqueio depende de alcance dinâmico. |
| Dados na seção text | As 700 palavras finais foram identificadas como tabela de dados. | Não devem ser contabilizadas indiscriminadamente como instruções faltantes. |
| Survey | Aproximadamente 99,5% dos alvos de chamada direta traduzem. | É cobertura daquele universo de entradas, não “99,5% do jogo pronto”. |
| Módulo completo | 15.068 funções e 924.991 instruções emitidas, cerca de 146 MB de C++. | Emitir e compilar não prova correção de cada caminho. |
| Executável | O módulo completo está integrado ao gt4boot. | Não é apenas um arquivo que passou por syntax check. |
| Execução | Inicialização, kernel, SIF/RPC, disco e caminhos de arquivos já foram exercitados. | Os serviços e dispositivos permanecem parciais. |
| Checkpoints | Save, resume, verificação e autosave existem. | Identidade semântica ainda precisa ser fortalecida. |
| Gráficos | Janelas e registradores são acessíveis. | Não há evidência de um renderer funcional produzindo o menu. |
| Controle | Há reconhecimento parcial do vocabulário e candidatos de protocolo. | Entrada Windows → guest → navegação ainda não está demonstrada. |
| Áudio | Alguns caminhos de inicialização e arquivos avançaram. | Não equivale a áudio sintetizado e reproduzido corretamente. |

As 30 words restantes foram classificadas como:

- 26 encodings COP2 macro de função 0x38;
- duas BC0F dependentes de condição associada ao DMA;
- dois encodings não atribuídos dentro do handler de exceção.

Essa lista é uma fronteira técnica, não uma prova de que todas bloqueiam o boot atual.

Fontes: [STATUS.md](C:/Antigravity/gt4-staticrecomp/docs/STATUS.md), [GPT_FEEDBACK.md](C:/Antigravity/gt4-staticrecomp/GPT_FEEDBACK.md) e documentação de milestones.

### 3.2 Testes e ambiente conhecidos

Na validação documentada em 2026-10-04:

- CTest: **50/50 aprovados**.
- Python: **73 casos coletados, seis skips**.
- Duas ResourceWarning sobre sockets foram registradas.
- O build incremental terminou com ninja: no work to do.
- O target gt4boot foi verificado explicitamente.
- Não houve clean rebuild nessa validação.

Ambiente registrado:

- MSVC x64;
- C++20;
- Ninja;
- CMake 4.3.3 na máquina consultada;
- Python 3.14.5 no venv existente.

Esses resultados são históricos documentados. Eles não constituem nova execução de testes deste plano.

O verde atual coexiste com defeitos comprovados porque os contratos específicos não estão suficientemente cobertos, e alguns testes preservam aproximações antigas.

### 3.3 Identidade do alvo

| Input | Tamanho | SHA-256 |
|---|---:|---|
| ISO USA v2.00 | 5.314.478.080 bytes | 67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f |
| CORE.GT4 | 2.020.861 bytes | 85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9 |
| Loader SCUS_973.28 | 273.020 bytes | f8f10823160e2b5cef5c7032628134632b291b1df87a9aee3c144794dd8019fa |
| Payload text | 5.339.668 bytes | 5a9a9107b146b7d533a2a2421cdf900a913d5ced4e97892798cfa810b3dd2d34 |
| ELF nativo reconstruído | 6.123.004 bytes | 10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c |
| ELF de referência | 6.127.896 bytes | 94aada8984999736f14a121ea1bd543e72d9d59b801e513716a32d8991746222 |

Entrada: 0x00100008.  
Início do texto: 0x00100000.

Os dois containers ELF têm layouts e hashes distintos. Comparar hashes dos containers não substitui comparar os segmentos carregados.

Manifests:

- [usa-v2.00.json](C:/Antigravity/gt4-staticrecomp/docs/inputs/usa-v2.00.json)
- [usa-v2.00-native.json](C:/Antigravity/gt4-staticrecomp/docs/inputs/usa-v2.00-native.json)
- [usa-v2.00-reference.json](C:/Antigravity/gt4-staticrecomp/docs/inputs/usa-v2.00-reference.json)

### 3.4 Fronteira operacional atual

O histórico inclui uma execução com 243.711.723 serviços, aproximadamente 241,8 milhões de chamadas de módulo e 7,1 bilhões de passos interpretados, encerrada sem fault em uma fronteira de ausência de thread executável.

As retomadas posteriores preservaram uma máquina estacionada, com 17 threads no censo recente e sem trabalho gráfico demonstrado.

**Isso não estabelece que seja o menu inicial.**

O re-check registrado como SAME mostra estabilidade do estado produzido pelo modelo antigo. Não valida retrospectivamente os contratos de timer, DMA, interrupção ou RPC utilizados para chegar ali.

Os checkpoints 60k/120k/180k de uma etapa histórica são trechos retomados após o checkpoint de 243,7 milhões de serviços. Seus nomes não representam necessariamente o tempo total desde o boot. Consultar a origem de cada checkpoint, não inferi-la do nome.

Referências:

- [tripwire-recheck-01.md](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/tripwire-recheck-01.md)
- [m32-slice6-long-legs.md](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m32-slice6-long-legs.md)

---

## 4. Conhecimento consolidado dos feedbacks

### 4.1 Vocabulário de confiança

| Grau | Significado neste plano |
|---|---|
| **Confirmed** | Código ou resultado observado sustenta diretamente a afirmação específica. |
| **High confidence** | Há evidência forte e corroborada, mas falta uma confirmação independente relevante. |
| **Hypothesis** | Explicação testável; ainda não demonstrada. |
| **Unknown** | Informação necessária que não foi recuperada ou validada. |

Um defeito confirmado pode ter relação **Unknown** com a parada atual.

### 4.2 Contratos que precisam de correção

| ID | Achado | Evidência e alcance |
|---|---|---|
| C01 | COUNT/COMP do timer tratados como 32 bits | Código confirmado; probes mostraram diferenças de largura e wrap. |
| C02 | EQUF produzida incondicionalmente no avanço idle | Código/probe confirmados; não corresponde a um compare realmente cruzado. |
| C03 | Escritas de flags sem o contrato correto de acknowledge | Probes mostraram perda e preservação incorretas de flags. |
| C04 | VIF0/VIF1/GIF completam pela família INTC | Wiring confirmado; devem usar a família DMAC correspondente. |
| C05 | TIE usado como habilitação geral de completion | Comportamento confirmado por probe; precisa ser separado de interrupção de tag. |
| C06 | Pending depende da presença de handler | Ordem problemática confirmada; a ocorrência deve existir antes da entrega. |
| C07 | Habilitação/máscaras incompletas | Serviços aceitos sem representar integralmente a elegibilidade. |
| C08 | jr ra lê destino depois do delay slot | Defeito reproduzido usando o emitter real. |
| C09 | Driver usa PC == RA para inferir retorno | BREAK e ERET expuseram ambiguidades reproduzíveis. |
| C10 | Argumento registrado do handler é descartado | a1 perdido confirmado por leitura do caminho completo. |
| C11 | Thread atual conserva ID de thread bloqueada no idle | Inconsistência confirmada; solução precisa abranger retorno do handler. |
| C12 | Respostas RPC genéricas zeradas | Fallback confirmado; não estabelece o serviço correto nem falha válida. |
| C13 | Comparação interna cobre só parte da máquina | RAM principal/contexto não cobrem todo kernel, scratchpad e dispositivos. |
| C14 | Checkpoint não identifica plenamente a semântica que o produziu | Formato decodificável não implica estado compatível com modelo corrigido. |

Referências: [GPT_FEEDBACK.md](C:/Antigravity/gt4-staticrecomp/GPT_FEEDBACK.md), [OPUS_FEEDBACK.md](C:/Antigravity/gt4-staticrecomp/OPUS_FEEDBACK.md) e [triagem](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/feedback-triage-2026-10-04.md).

### 4.3 Resultados sintéticos importantes

**JR com escrita de RA no delay slot**

~~~text
0x1000: 03E00008   jr ra
0x1004: 241F2000   addiu ra, zero, 0x2000

RA inicial: 0x3000

Esperado:
    destino = 0x3000
    RA final = 0x2000

Observado no nativo:
    destino = 0x2000
    RA final = 0x2000
~~~

O destino precisa ser capturado antes de executar o slot.

**ERET**

Nos casos sintéticos EXL/ERL, o efeito do ERET foi aplicado, mas o driver classificou a saída como InstructionStop e não continuou no destino.

O problema observado é a integração da continuação, não necessariamente a aritmética do ERET.

**BREAK com PC igual a RA**

O driver classificou o trap como retorno e repetiu chamadas até o orçamento acabar. O interpreter reconheceu BREAK.

**Timer**

~~~text
COUNT inicial: 0xFFF0
Avanço: 576 ticks do contador

Esperado:
    COUNT = 0x0230
    overflow ocorrido

Modelo antigo:
    COUNT = 0x10230
    sem overflow de 32 bits
~~~

A correção precisa incluir flags, entrega e tempo estendido guest. Apenas aplicar & 0xFFFF no valor final é insuficiente.

Os probes estão documentados na seção 29 de [GPT_FEEDBACK.md](C:/Antigravity/gt4-staticrecomp/GPT_FEEDBACK.md:1005). Devem ser convertidos em regressões permanentes ao corrigir os respectivos contratos.

### 4.4 Ressalvas obrigatórias ao feedback do Opus

1. **Não declarar a causa do stall resolvida por inspeção estática.**  
   Os defeitos encontrados são candidatos fortes. A cadeia causal ainda precisa ser demonstrada.

2. **Não forçar TAG END em uma cadeia DMA não consumida.**  
   Isso pode fazer o handler confirmar uma transferência que não ocorreu.

3. **Não usar -1 como resposta genérica de “sem memory card”.**  
   A referência distingue mudança de cartão, cartão sem formato e falha de detecção.

4. **Não atribuir a2 = interrupted.pc apenas pelo nome addr.**  
   O valor correto do terceiro argumento precisa de confirmação.

5. **Não zerar current_thread_id_ isoladamente.**  
   O retorno do handler sem thread associada atualmente restaura contexto sem executar dispatch.

6. **Não considerar SID identificado como protocolo implementado.**  
   Registro, função RPC, buffers, estado, callbacks e lifecycle precisam de evidência.

7. **Não deduzir tamanho de pacote a partir de alinhamento.**  
   Alinhamento de 64 bytes não demonstra transferência de exatamente 64 bytes.

8. **Não usar GS dump como captura completa de VIF/VU.**  
   São pontos diferentes da pipeline.

9. **Não prometer ganhos 5×–10× sem profiling.**

10. **Não assumir que executar IRX em um interpreter pequeno elimina o trabalho de kernel, hardware e sincronização.**

### 4.5 Correções específicas de memory card

Na referência PS2SDK:

- -1: mudança de cartão; no exemplo de mcGetInfo, cartão formatado recém-detectado;
- -2: cartão sem formato;
- falhas de detecção: outros códigos, incluindo -12/-13 nas definições;
- type = 0: ausência de cartão.

Os números wire 01/02/03/05/06/0D, juntamente com init FE, pertencem à tabela XMC. A variante precisa ser confirmada no alvo.

Não assumir free = 8192 para um cartão formatado, nem getdir = -4 para toda consulta em cartão vazio.

Fontes: [definições libmc](https://github.com/ps2dev/ps2sdk/blob/master/common/include/libmc-common.h), [exemplo oficial](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/memorycard/samples/mc_example.c) e [tabelas RPC](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/memorycard/src/libmc.c).

---

## 5. Gates de entrega

Os gates descrevem resultados demonstráveis. As implementações necessárias podem atravessar vários milestones históricos.

| Gate | Resultado | Aceite |
|---|---|---|
| **G0 — Base confiável** | Contratos prioritários corrigidos e observáveis | Regressões dos defeitos, snapshots compatíveis e falhas desconhecidas explícitas. |
| **G1 — Inicialização avança** | A causa relevante do bloqueio é identificada e corrigida | O guest consome dado/evento correto e produz o próximo trabalho esperado. |
| **G2 — Primeira imagem live** | Frame produzido pelo jogo recompilado | Pipeline real processada e imagem apresentada na janela. |
| **G3 — Menu funcional** | Menu inicial original navegável | Boot fresco, imagem correta e ações de controle observáveis. |
| **G4 — Serviços de jogo** | Áudio/vídeo e persistência inicial | Serviços coerentes, mídia pertinente e save/load sobrevivendo a restart. |
| **G5 — Primeira corrida** | Uma combinação carro/pista utilizável | Carga, controle, HUD, física e uma volta completa. |
| **G6 — Jogo utilizável** | Fluxos principais e compatibilidade ampliados | Menu → corrida → resultado → save → restart → load repetíveis. |
| **G7 — Entrega estável** | Instalação reproduzível e desempenho aceitável | Executável instalado em ambiente suportado, limitações documentadas e regressões preservadas. |

**O objetivo atual termina em G3.** Depois disso, revisar com o dono quais partes de G4–G7 devem vir primeiro.

Gráficos e input podem ter trabalho de laboratório paralelo antes de G1, mas esse trabalho não fecha G2 ou G3 enquanto não estiver integrado à execução live AOT.

---

## 6. Próximas fatias: antes de continuar a marcha longa

### P00 — Registrar baseline e compatibilidade

**Entrega**

- Identificar commit, binário, inputs, tradução e configuração usados.
- Criar identidade explícita de compatibilidade do modelo.
- Separar formato de checkpoint, provenance e compatibilidade semântica.
- Classificar checkpoints anteriores como forenses quando o contrato mudar.

**Aceite**

- Estado produzido por semântica incompatível é rejeitado antes do restore.
- Mudança apenas editorial não invalida desnecessariamente um checkpoint.
- Logs deixam claro se o contador é relativo ao trecho retomado ou cumulativo.
- Restore não inicia DMA, limpa flags nem altera máscaras por efeitos de escrita guest.

**Observação**

Git SHA é útil para provenance, mas não deve ser a única definição de compatibilidade.

### P01 — Semântica dos registradores de timer

**Entrega**

- COUNT/COMP com largura lógica correta.
- Bits writable e flags nomeados.
- Acknowledge correto.
- Reset e modos usados pelo jogo documentados.
- Separação entre escrita guest, alteração interna e snapshot restore.

**Aceite**

- Bits altos de COUNT/COMP não viram extensão indevida do contador.
- Escrita zero em flag W1C preserva o pending.
- Escrita um reconhece a flag pertinente.
- Restore é livre de efeitos de execução.

**Fixtures existentes**

- [ee_timer_test.cpp](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_timer_test.cpp)
- [ee_device_test.cpp](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_device_test.cpp)

### P02 — Origem, pending, máscara e dispatch de interrupções

**Entrega**

Representar separadamente:

~~~text
ocorrência
    → dados/status
    → pending
    → elegibilidade
    → handler
    → acknowledge
~~~

- Causas INTC e DMAC em domínios distintos.
- Pending independente da existência de handler.
- Enable/disable e registradores de máscara com seus contratos próprios.
- Gate CP0 pertinente ao caminho usado.
- Política explícita de coalescing e ordem.
- Eventos de dados separados de bits de interrupt.

**Aceite**

- Uma ocorrência sem handler continua representada no status.
- Habilitar depois permite atender pending conforme o contrato.
- Acknowledge de uma causa não apaga outra.
- GIF completion não chama Timer0.
- Não há entrega de causa inelegível.
- Alteração interna de pending não passa pelo caminho guest W1C/toggle.

**Fixtures**

- [ee_kernel_test.cpp](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_kernel_test.cpp)
- [ee_device_test.cpp](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_device_test.cpp)

### P03 — Uma máquina de avanço dos timers

**Dependências:** P01 e P02.

**Entrega**

- Idle e serviço usam a mesma máquina de estados.
- Compare e overflow são tratados sem perder cruzamentos.
- Remainder de clock preservado.
- Reprogramação de COMP respeitada.
- Modos de gate/zero-return usados pelo jogo tratados explicitamente.
- Saltos de tempo grandes não pulam oportunidades de atendimento relevantes.

**Aceite**

- 0xFFF0 + 576 → 0x0230 com overflow correto.
- COMP não cruzado não produz EQUF.
- Compare e overflow combinados são classificados corretamente.
- A rotina guest de tempo estendido cresce coerentemente.
- Avanço sem intervenções do guest tem propriedades de decomposição verificáveis.
- Com handlers reprogramando o timer, o scheduler oferece as oportunidades previstas entre eventos.

### P04 — Handler arguments e idle explícito

**Entrega**

- Preservar handler e argumento como uma unidade de registro.
- Passar a1 correto para as variantes pertinentes.
- Confirmar a semântica de a2/addr.
- Representar contexto interrupted-idle explicitamente.
- Definir GetThreadId nesse contexto por referência.
- Revisar dispatch no retorno de interrupção.
- Preservar contexto e política de não nesting/não preempção até existir evidência que exija revisão.

**Aceite**

- Dois handlers com argumentos diferentes recebem seus argumentos.
- Handler em idle acorda uma thread e o retorno escolhe a ready apropriada.
- Se ninguém acorda, o sistema continua idle corretamente.
- O retorno não restaura acidentalmente uma thread WAIT como RUN.
- O frame interrompido é preservado.

Não aceitar como solução completa apenas:

~~~cpp
current_thread_id_ = 0;
~~~

O ramo de [deferred_return](C:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp:826) precisa participar da correção.

### P05 — JR e resultado explícito da saída AOT

**Entrega**

- Capturar destinos antes do delay slot.
- Remover a inferência de retorno baseada apenas em PC == RA.
- Distinguir semanticamente:
  - retorno aplicado;
  - syscall pendente;
  - trap;
  - transferência pendente;
  - ERET aplicado;
  - instrução não suportada;
  - término por orçamento.
- Propagar o motivo pelas chamadas internas do módulo sem sobrescrevê-lo.
- Garantir que bridge não repita serviço, link ou delay slot já aplicado.

Os nomes finais da API devem ser escolhidos durante a implementação. O contrato importa mais que o nome do enum.

**Aceite**

- Fixture JR mantém destino antigo e RA final atualizado.
- BREAK com PC igual a RA para como BREAK.
- ERET EXL/ERL continua no destino.
- O código é emitido pelo tradutor real, compilado e executado pelo Driver.
- Comparação inclui efeito e motivo de parada.

**Fixtures**

- [ee_driver_test.cpp](C:/Antigravity/gt4-staticrecomp/tests/unit/ee_driver_test.cpp)
- Suítes existentes de interpreter e tradução.
- Probes sintéticos documentados no feedback GPT.

### P06 — DMA com dados e conclusão reais

**Entrega**

- Roteamento VIF0/VIF1/GIF para DMAC 0/1/2.
- Separação entre conclusão normal e interrupção de tag.
- Interface device → kernel com domínio/canal explícitos.
- Captura dos primeiros starts necessários ao boot.
- Transferência do subconjunto observado.
- Percurso de tags e atualização coerente de registradores.
- Integração com o consumidor VIF/GIF quando alcançado.

**Aceite**

- Payload entregue corresponde à origem e tamanho.
- QWC/MADR/TADR e estado de tag evoluem coerentemente.
- Completion ocorre quando a operação implementada conclui.
- Pending e handler corretos são observados.
- O guest limpa busy ou acorda waiter em resposta à operação real.
- Cadeia desconhecida para com contexto.

**Proibição de implementação**

Não colocar artificialmente TAG_ID = END para satisfazer o ramo de sucesso do handler.

O aceite não pode ser apenas “o handler executou e limpou o byte”. É necessário demonstrar a transferência que justifica esse resultado.

### P07 — RPC rastreável e estrito

**Entrega**

Inventariar chamadas por:

~~~text
SID + função + revisão/protocolo + estado do serviço
~~~

Registrar:

- caller e thread;
- request;
- resposta;
- buffers auxiliares;
- callback;
- completion;
- consumidor do resultado;
- classificação da implementação.

Classificações recomendadas:

- implementado e verificado;
- constante de compatibilidade documentada;
- ausência/falha válida modelada;
- provisório explícito;
- desconhecido.

**Aceite**

- Primeiro desconhecido é identificável.
- Não há resposta zerada global silenciosa.
- Uma resposta válida de erro é diferente de serviço sem implementação.
- Disponibilidade e bind respeitam um lifecycle definido.
- Reset invalida os recursos pertinentes.

Uma resposta inteira zero pode ser correta em um contrato específico. Precisa ser demonstrada e nomeada, não surgir de fallback genérico.

### P08 — Comparador ampliado

**Entrega**

Comparar snapshots canônicos de:

- registradores EE;
- RAM principal;
- scratchpad;
- regiões adicionais;
- threads e contextos;
- semáforos;
- handlers;
- deferred calls;
- SIF/RPC;
- timers;
- bancos e status;
- causas pendentes;
- demais dispositivos efetivamente implementados.

**Aceite**

- Diferença isolada em scratchpad é detectada.
- Diferença em semáforo é detectada.
- Diferença em registrador de dispositivo é detectada.
- Diagnóstico informa primeiro componente/campo divergente.
- Não incluir ponteiros host ou ordem incidental de containers no estado semântico.

Não reutilizar reads MMIO com efeitos colaterais para produzir o snapshot.

### P09 — Prefixo fresco e observação independente

**Entrega**

- Boot desde a entrada, com inputs verificados.
- Checkpoints novos depois das alterações semânticas.
- Âncoras semânticas pequenas e comparáveis.
- Instrumentos calibrados com controle positivo.
- Localização da primeira divergência relevante.

**Aceite**

- Mesma revisão e configuração registradas.
- Um trace identifica a ordem request → dado → completion → consumo.
- Evidência indica onde o modelo se afasta da referência.
- Não usar checkpoint antigo mascarado como prova da execução corrigida.

### P10 — Primeiro serviço causal

**Entrega**

Implementar o menor produtor necessário ao predicado que impede o avanço da main.

A escolha entre timer, DMA, memory card, input, streaming ou outro serviço deve vir da cadeia causal observada.

**Aceite**

- Request/condição identificado.
- Resultado ou evento correto.
- Consumo pelo código guest.
- Próximo trabalho produzido naturalmente.
- Comparação independente e regressão permanente.

### 6.1 Ordem de trabalho recomendada

Preparar P00 antes de mudanças semânticas.

Depois, trabalhar em fatias pequenas:

~~~text
P01 + P02
    → P03
    → P04
    → P05
    → P06 no recorte alcançado
    → P07 + P08
    → P09
    → P10
~~~

Essa é uma recomendação, não uma fila inflexível. P07, por exemplo, pode ser antecipada se revelar um desconhecido mais barato e decisivo. P05 pode ser corrigida independentemente das alterações de dispositivos.

Não concentrar tudo em uma alteração enorme. Uma fatia deve ter um comportamento, um teste e um efeito verificável.

---

## 7. Política de tempo

A política atual de um milissegundo por serviço é determinística entre os motores, mas é uma aproximação.

No volume histórico de 243.711.723 serviços, somente essa parcela equivale a aproximadamente 67,7 horas virtuais. Isso não representa duração realista do boot.

### 7.1 Primeiro passo

Corrigir:

- largura;
- flags;
- compare;
- overflow;
- pending;
- avanço comum idle/serviço.

Inicialmente, a política de quantum pode permanecer explicitamente aproximada para isolar as correções.

### 7.2 Evolução posterior

Se a evidência indicar que o quantum impede a sequência correta:

1. Definir unidade temporal guest.
2. Contabilizar trabalho guest de forma comparável.
3. Associar eventos a uma agenda determinística.
4. Permitir idle avançar ao próximo evento relevante.
5. Inserir oportunidades de atendimento entre eventos.
6. Registrar a política temporal no checkpoint.

Não contar uma chamada de módulo AOT como uma instrução. Um módulo pode executar loops e milhares de instruções.

Não tratar “uma instrução = um ciclo” como precisão de hardware.

Não validar somente um modo temporal diferente daquele usado no produto. Cada política suportada precisa ser identificada e testada em sua configuração real.

---

## 8. Investigação causal do bloqueio

A pergunta principal passa a ser:

> Qual condição o guest está esperando, quem a produz na execução de referência e em qual ponto nosso modelo deixa de reproduzir essa cadeia?

### 8.1 Procedimento

1. Identificar o predicado da main.
2. Registrar objeto, campo, valor atual e valor esperado.
3. Enumerar writers e aliases.
4. Observar writers dinâmicos.
5. Capturar a escrita correspondente na referência funcional.
6. Recuar até seu produtor.
7. Localizar a primeira diferença.
8. Corrigir esse contrato.
9. Demonstrar o próximo trabalho guest.

Pistas já documentadas incluem a cadeia 0x00109340 → 0x001097F0, helpers na região 0x005767E0 e a cadeia de espera que inclui 0x00109818.

Esses endereços são pontos de investigação, não autorização para alterar o estado do jogo.

### 8.2 Árvore de decisão

~~~text
main não avança
├─ continuação AOT incorreta?
│  └─ JR / ERET / motivo de saída
├─ prazo não vence corretamente?
│  └─ getter / base / target / compare / overflow
├─ completion DMA não chega?
│  └─ start / tags / payload / pending / handler / busy
├─ dado IOP é incorreto ou não chega?
│  └─ SID / função / buffers / callback / lifecycle
├─ dispositivo ou memory card impede transição?
│  └─ protocolo e estado completos
├─ entrada não chega ao consumidor?
│  └─ registro do dispositivo / buffer / sequence / leitura guest
└─ trabalho gráfico depende de outro produtor?
   └─ caller / agenda / VIF / VU / GIF / GS
~~~

### 8.3 Disciplina de experimentos

Cada experimento precisa ter:

- hipótese;
- previsão;
- controle positivo;
- observação que a refuta;
- orçamento;
- resultado;
- próxima ação.

Duas execuções equivalentes sem informação nova são motivo para mudar o experimento. Aumentar o limite só é justificável quando há uma previsão temporal ou uma transição observada que depende disso.

---

## 9. Observação com PCSX2, Ghidra e referências

### 9.1 Papel do PCSX2

Comparar fronteiras pequenas:

- leitura de tempo;
- primeira transferência;
- entrada e retorno do handler;
- primeiro RPC substancial;
- dados entregues a arquivos;
- escrita do predicado;
- primeiro pacote gráfico.

PCSX2 é referência independente, não prova de perfeição do hardware. Registrar versão, BIOS/input local, opções de boot, configurações e ponto de captura.

### 9.2 Limites do PINE

A interface consultada oferece operações de memória, identificação/status e save/load. Não deve ser tratada automaticamente como debugger remoto com step, breakpoint e registradores.

Leituras de uma VM rodando não estabelecem uma fotografia atômica da máquina.

Usar debugger/logpoints ou instrumentação apropriada quando a investigação exigir controle da execução. Fonte: [PINE.cpp](https://github.com/PCSX2/pcsx2/blob/81526d4dc7cc70e4ae75abb35a789417456c6d43/pcsx2/PINE.cpp).

### 9.3 Calibração

Antes de confiar em ausência de hits:

- demonstrar que o instrumento vê um caso conhecido;
- conferir o momento em relação à instrução/delay slot;
- distinguir AOT, interpreter, host e DMA;
- verificar aliases;
- limitar logs e registrar dropped counters.

Registradores observados num helper interno podem não representar os argumentos originais da função. O histórico já mostrou leituras stale durante execução traduzida.

### 9.4 Trace mínimo recomendado

~~~text
run_id
model_compatibility_id
input_hashes
event_sequence
guest_time
engine
pc
thread_id
source
request_id
sid
rpc_number
send_size
send_hash
receive_size
receive_hash
dma_channel
source_address
destination_address
handler
consumer
~~~

Nem todo evento exige todos os campos.

Para SIF, observar as duas direções e as cópias auxiliares pertinentes. Logar apenas envio EE não captura automaticamente notificações originadas pelo IOP nem todos os dados copiados fora do pacote principal.

### 9.5 Comparação por significado

Não iniciar tentando igualar toda a RAM de um BIOS real a um kernel HLE.

Primeiro comparar:

- dados relevantes;
- estruturas de request/reply;
- ordem causal;
- buffers;
- efeitos consumidos pelo jogo.

Diferenças irrelevantes só podem ser normalizadas mediante justificativa explícita.

---

## 10. IOP: serviços e escolha arquitetural

### 10.1 Mapa de candidatos prioritários

| SID | Identidade candidata/estabelecida | Ação |
|---|---|---|
| 0x53545250 | PRTS/cache de blocos | Preservar reader/cursor e validar lifecycle, limites e novas chamadas. |
| 0x50434456 | PCDV | Preservar leitura de disco e validar protocolo alcançado. |
| 0x50636476 | Canal Pcdv em PDICDVD, segundo o Opus | Confirmar registro, dispatcher e respostas. |
| 0x80000400 | MCSERV/libmc | Confirmar variante, tabelas wire e buffers auxiliares. |
| 0x80001300 | DBCMAN/libpad2, segundo o mapa Opus | Confirmar init, registro de socket e funções. |
| 0x8000131C/1E/1F | Canais DBC/libpad2 candidatos | Recuperar campos, destino, tamanho e frequência. |
| 0x046D046D | LGDEV/Logitech candidato | Confirmar estados presença/ausência e consumidores. |
| MPG1/MPG2/PBGM/VOIC | Serviços de streaming candidatos em PDISTR | Recuperar protocolos apenas quando alcançados ou causais. |
| SPUP/SPUT | Serviços candidatos em PDISPU2 | Confirmar init, dados, estado e completions. |

A identificação específica dos módulos e offsets apresentada pelo Opus é um mapa de alto valor, mas ainda exige reprodução item a item.

### 10.2 Registro mínimo de cada contrato

- Hash do IRX.
- Container e offset de extração.
- Seção e endereço correspondente.
- Sequência de registro do SID.
- Callback registrado.
- Função RPC.
- Request e reply.
- Buffers adicionais.
- Estados e transições.
- Erros válidos.
- Origem de notificações.
- Relação com load/reset/stop.
- Consumidor EE.
- Experimento independente.

Encontrar uma constante SID com lui/ori não demonstra sozinho que ela é argumento do registro de servidor.

### 10.3 HLE focado

Preferir quando:

- request/reply e estado são pequenos;
- o contrato foi observado;
- a dependência pode ser modelada de forma clara;
- os callbacks são conhecidos.

A implementação precisa conservar estado e comportamento assíncrono quando o serviço os exige.

### 10.4 IRX interpretada

Considerar quando:

- vários protocolos privados dependem de lógica interligada;
- reimplementar o conjunto acumula suposições;
- executar o módulo original oferece um recorte mais verificável.

O protótipo precisa demonstrar:

- carregamento e relocação;
- resolução de imports;
- RAM IOP separada;
- agenda de threads/eventos;
- transporte SIF;
- registro real e disponibilidade;
- request/resposta/dados;
- reset e invalidação;
- erro útil em import desconhecido;
- snapshot determinístico.

Não basta executar _start.

### 10.5 Híbrido

Usar ownership explícito por serviço ou endpoint.

Não permitir HLE e IRX responderem simultaneamente ao mesmo request.

Não espelhar endereços IOP como se fossem endereços EE. A transferência entre espaços precisa ser deliberada.

PS2Recomp é uma referência arquitetural experimental, não uma dependência automática nem evidência de compatibilidade GT4. [Repositório primário](https://github.com/ran-j/PS2Recomp).

### 10.6 Decisão de arquitetura

Registrar ADR depois de identificar o produtor relevante.

A decisão deve comparar:

- serviços necessários;
- complexidade dos contratos;
- imports/hardware exigidos;
- custo de verificação;
- possibilidade de snapshot;
- manutenção;
- efeito no próximo gate.

Não escolher uma IOP universal antes de saber qual serviço impede a entrega atual.

---

## 11. Disco, arquivos e assets

### 11.1 Conhecimento já aproveitável

- ISO9660 e acesso aos setores foram implementados no recorte documentado.
- Foram identificados dois volumes no disco.
- O reader externo GT4.VOL foi desenvolvido e validado parcialmente.
- O cache PRTS corrigiu uma falha de inicialização de objeto.
- O cursor de copy-out PRTS corrigiu a corrupção no carregamento da fonte.
- Formatos internos v3.1, páginas, objetos de textura e fontes têm reconhecimento parcial.
- O código original pode executar transformações e descompressão; isso deve ser preferido quando já funciona.

### 11.2 Próximas verificações

- Unidades de LBA, tamanho e destino.
- Limites de arquivo e cache.
- Cursores por handle.
- Leitura parcial e exaustão.
- Lifetime de handles.
- Ordem de completions.
- Seleção de volume.
- Dados servidos comparados a bytes do disco.
- Relação entre página compactada, transformação e objeto produzido.

### 11.3 Asset viewer

Pode ajudar a:

- validar parser;
- inspecionar fontes;
- confirmar swizzle/paleta;
- comparar textura;
- entender material e tamanho.

Não substitui a renderização original nem precisa bloquear a chegada ao menu se o código guest já consegue carregar os assets necessários.

### 11.4 Correção de uma expectativa do Opus

Não assumir que um boot curto de 85 mil serviços desbloqueia automaticamente todos os assets internos.

O slice 28 registrou hooks de página/material/upload sem execução num boot fresco de 95 mil serviços, com limites dos instrumentos examinados. Isso contraria a promessa de que os hooks necessariamente produzirão tudo no prefixo antigo.

Retomar esses hooks quando a nova execução alcançar efetivamente o caminho.

Referências:

- [asset-viewer-roadmap.md](C:/Antigravity/gt4-staticrecomp/docs/plans/asset-viewer-roadmap.md)
- [asset-page-cipher-static.md](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/asset-page-cipher-static.md)
- [m33-slice28-asset-dumps.md](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m33-slice28-asset-dumps.md)

---

## 12. Gráficos: da primeira transferência à imagem

### 12.1 Distinguir três provas

1. **Replay de referência:** processar uma captura de PCSX2.
2. **Pipeline live:** processar dados produzidos pelo EE recompilado.
3. **Menu funcional:** pipeline live, apresentação e interação.

Replay é uma ferramenta de desenvolvimento. Não fecha o gate de boot live.

### 12.2 Etapa gráfica A — DMA e transporte

Implementar o recorte alcançado de:

- DMA normal e chain;
- tags reais;
- transferências e contadores;
- controle de fluxo da cadeia;
- entrega a VIF/GIF;
- espera e conclusão;
- interrupções pertinentes.

Não reconhecer conclusão antes do efeito correspondente existir.

### 12.3 Etapa gráfica B — VIF

Recuperar e implementar os comandos usados:

- estado de unpack;
- formatos e endereçamento;
- máscaras e ciclos;
- upload de microprogramas;
- controle de execução VU;
- transfers diretas;
- flush/esperas;
- status consumido pelo guest.

Comandos desconhecidos devem indicar stream, offset e estado.

### 12.4 Etapa gráfica C — VU1

Começar por execução de referência legível do subconjunto necessário ao menu.

Tratar:

- memórias e registradores;
- microprogramas;
- flags;
- branches e slots;
- interações com VIF/GIF;
- publicação de resultados;
- peculiaridades aritméticas alcançadas.

Uma implementação inicialmente interpretada é aceitável para estabelecer comportamento. Otimizar depois do profiling.

Captura necessária:

~~~text
estado inicial
    + stream VIF
    + microprograma
    + entradas
    + eventos relevantes
    → saída GIF esperada
~~~

### 12.5 Etapa gráfica D — GIF

Implementar:

- parsing de GIFtags;
- formatos de packet usados;
- payloads e registradores;
- paths necessários;
- interação e ordenação pertinentes;
- status e handshakes consumidos pelo jogo.

A contagem de pacotes deve ser atribuída a produtores reais.

### 12.6 Etapa gráfica E — GS

Priorizar o conjunto necessário aos frames de menu observados:

- VRAM e layout;
- transferências host/local;
- formatos usados;
- paletas e swizzle;
- registradores/contextos;
- primitivas;
- scissor e viewport;
- textura;
- alpha/depth/blend pertinentes;
- framebuffers e display.

O subconjunto deve ser guiado por captura, não pelo desejo de implementar todo o GS antecipadamente.

### 12.7 Software e Vulkan

A documentação atual propõe Vulkan para apresentação/backend moderno. Manter essa direção como recomendação inicial.

Um rasterizador software/offscreen pequeno pode ajudar a estabelecer pixels de referência antes de aumentar a complexidade GPU. Não deve virar um segundo projeto genérico.

A primeira apresentação pode usar a resolução/framebuffer efetivamente configurados pelo guest, com escala de janela simples. Não forçar uma resolução fixa que contradiga o modo usado pelo jogo.

A integração Vulkan deve ter lifecycle correto de superfície, swapchain, sincronização e resize. [Documentação oficial de apresentação](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/01_Presentation/01_Swap_chain.html).

### 12.8 GS dump e VIF/VU são capturas distintas

GS dump inclui dados e estado no lado GS. Não recupera automaticamente a timeline upstream de VIF e VU.

Usar:

- GS dump para testar GS;
- captura upstream para VIF/VU;
- execução live para demonstrar o port.

[Formato oficial do GS dump](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSDump.h).

### 12.9 Aceite da primeira imagem

- Frame produzido a partir de comandos do guest recompilado.
- Uploads/draws/present correlacionados.
- Conteúdo reconhecível.
- Comparação com referência apropriada.
- Sem screenshot substituindo renderização.
- Sem erros desconhecidos escondidos.
- Mais de um frame para distinguir imagem estática de pipeline viva.

Diferenças visuais toleradas devem ser explicitamente classificadas, com origem e plano de correção.

---

## 13. Controle e teclado

### 13.1 Dois problemas separados

**Host**

- detectar dispositivo;
- ler botões/eixos;
- hotplug;
- configuração.

**Guest**

- presença e identificação;
- registro de buffers;
- formatos;
- sequence/status;
- notificações;
- leitura pelo jogo.

Input host funcionando não demonstra entrega ao guest.

### 13.2 Backend Windows recomendado

Considerar SDL3 como camada de janela, gamepad e eventos para reduzir trabalho específico por API.

Fixar versão e escopo antes da integração. Validar controles realmente usados pelo dono.

SDL oferece mapeamento por posição de botão/eixo, hotplug e consulta de capacidades opcionais. Isso não garante que qualquer dispositivo funcione sem validação. [Documentação oficial de gamepads](https://wiki.libsdl.org/SDL3/CategoryGamepad).

XInput pode ser suficiente para um recorte inicial de dispositivos compatíveis, mas não deve ser apresentado como suporte universal.

### 13.3 Protocolo guest

Reabrir a investigação de libpad2/DBCMAN.

Não esperar exclusivamente por PADMAN clássico 0x80000100/101.

Confirmar:

- servidor;
- registro;
- buffer EE;
- tamanho;
- campos;
- frequência;
- status conectado/pronto;
- formato dos botões;
- analógicos;
- pressão;
- vibração;
- destino e conclusão da DMA, se houver.

### 13.4 Estado normalizado

Manter uma interface host independente do protocolo:

~~~text
presença
botões
eixos
gatilhos
pressões disponíveis
capacidade de vibração
~~~

A conversão para o estado DS2 deve respeitar o formato confirmado.

Controle moderno sem pressão nos face buttons exige uma política de adaptação explícita. Por exemplo, mapear uma fonte analógica apropriada para aceleração/freio quando o protocolo e a configuração do jogo permitirem.

Não anunciar fidelidade de pressão se a fonte host só oferece botão digital.

### 13.5 Determinismo

- Aplicar snapshots de input em fronteiras guest definidas.
- Gravar/reproduzir sequência de ações.
- Separar evento host de tempo guest.
- Usar replay para regressões.
- Não deixar a comparação interna depender do instante de polling do sistema operacional.

### 13.6 Aceite de menu com controle

Demonstrar:

- estado neutro;
- pressionar;
- soltar;
- segurar;
- navegar;
- confirmar;
- voltar;
- entrar em submenu;
- desconectar/reconectar;
- ausência de input preso.

Teclado pode servir como alternativa inicial e ferramenta de teste. O controle físico continua sendo parte da primeira entrega funcional recomendada.

---

## 14. Critério do objetivo atual: menu inicial

### 14.1 Gate G3

A entrega deve demonstrar:

1. Execução iniciada por boot fresco.
2. Inputs verificados.
3. Inicialização percorre os caminhos necessários naturalmente.
4. Menu inicial original aparece.
5. Frames são produzidos pela pipeline live.
6. Entrada chega ao consumidor guest.
7. Navegação altera o estado do menu.
8. Seleção e retorno funcionam.
9. A execução permanece viva e sem erros escondidos.
10. Comportamento é reproduzível.

### 14.2 Proposta de cenário de aceitação

Registrar:

- três boots frescos;
- sequência fixa de ações de navegação;
- sessão de menu com animações/atualizações observáveis;
- controle neutro, press/release e reconexão;
- tempo host para boot e resposta;
- tempo guest e frames apresentados;
- screenshot/captura local;
- trace curto do fluxo;
- build/configuração exatas.

A quantidade de frames e duração final devem ser definidas no cenário implementado, considerando a velocidade inicial. O objetivo é provar continuidade e interação, não exigir antecipadamente desempenho final.

### 14.3 Áudio no primeiro gate

Som audível pode ser entregue depois de G3, desde que:

- isso esteja explícito;
- serviços de áudio necessários ao boot não respondam sucesso fictício;
- agenda, estado e completions exigidos estejam corretos;
- a ausência de reprodução host não esconda uma dependência bloqueante.

Se áudio ou intro forem a primeira condição que impede o menu, passam a integrar o caminho crítico.

### 14.4 Revisão com o dono

Depois de G3, apresentar:

- o que funciona;
- demonstração visível;
- limitações;
- velocidade medida;
- dependências conhecidas;
- opções para a próxima prioridade.

O próximo foco pode ser áudio, primeira corrida, saves ou desempenho básico, conforme o resultado.

### 14.5 Se o boot exigir save antes do menu

"Menu inicial" acima é o que o jogo mostrar naturalmente — se um boot
sem save abrir uma tela de criação de save antes do menu principal,
ESSA tela é a forma válida de G3, não um desvio. Consequência de
planejamento (nota do dono, 2026-10-08): nesse caso o memory card
(MCSERV/libmc, §16) vira caminho crítico mais cedo do que a ordem
atual sugere — a telemetria RPC dirá quais chamadas a fase faz, e a
promoção se decide com essa evidência, não por antecipação.

---

## 15. Áudio, vídeo e streaming

### 15.1 Separar as camadas

~~~text
request de serviço
    → estado do servidor
    → leitura/transporte
    → buffers
    → decode/síntese
    → avanço temporal
    → saída host
~~~

Resposta RPC e reprodução audível são entregas diferentes.

### 15.2 Áudio

Priorizar:

1. Protocolo causal para o boot.
2. Música do menu.
3. Efeitos de navegação.
4. Sons da primeira corrida.
5. Vozes e caminhos adicionais.

Implementar o recorte SPU2/serviços necessário com:

- transferências;
- estado de vozes/buffers;
- endereçamento;
- decode/mix pertinente;
- ritmo guest;
- buffering host;
- underflow/overflow observáveis.

Não permitir que o relógio da API de áudio determine arbitrariamente a ordem dos eventos guest.

### 15.3 Vídeo/intro e IPU

Se o caminho de movie for necessário:

- mapear streaming;
- confirmar dados;
- implementar o recorte IPU/decode alcançado;
- coordenar apresentação e áudio;
- respeitar controles originais.

Se existe um skip legítimo pelo próprio jogo, ele pode compor um cenário inicial observado.

Não produzir “movie complete” falso nem alterar a main para passar uma fase.

### 15.4 Aceite

- Mídia pertinente aparece/toca.
- Estado e completions coerentes.
- Sem loops de polling sustentados por respostas inventadas.
- Entrada de skip, quando aplicável, funciona pelo caminho original.
- Transição para a próxima fase ocorre naturalmente.

---

## 16. Memory card e persistência

### 16.1 Separação necessária

Checkpoint do runtime e save do jogo são coisas diferentes.

- **Checkpoint:** fotografia da execução inteira para investigação/retomada.
- **Memory card:** protocolo de arquivos e persistência que o guest utiliza.

Um não substitui o outro.

### 16.2 Primeiro recorte

Escolher por evidência:

- ausência válida de cartão; ou
- cartão virtual persistente.

A ausência válida pode permitir um boot inicial. O produto utilizável precisa suportar os saves esperados pelo dono. E se a
tela de criação de save for o primeiro gate do boot (§14.5), este
recorte inteiro sobe para o caminho crítico — decidir com a
telemetria, não por antecipação.

### 16.3 Contratos

Confirmar:

- variante MCSERV/XMCSERV;
- comandos wire;
- request/reply;
- buffers auxiliares;
- callback;
- tipo/formato;
- mudanças de cartão;
- espaço livre;
- diretórios;
- handles;
- read/write/seek/close/flush;
- limites e erros;
- reset e lifetime.

Não devolver o mesmo código para ausência, diretório vazio, cartão sem formato e arquivo inexistente.

### 16.4 Backend

Decidir entre:

- representação virtual de cartão;
- filesystem host com adaptação explícita do protocolo;
- outra representação demonstrada compatível.

Preferir uma solução simples que conserve os contratos usados pelo jogo.

### 16.5 Integridade

- Escritas com tratamento de falha.
- Preservação do save válido anterior.
- Handles e offsets consistentes.
- Estado guest e host sincronizados.
- Snapshot/resume com política explícita para efeitos externos.

Restaurar checkpoint antigo não deve sobrescrever silenciosamente persistência host mais nova. Essa interação precisa de uma política antes de checkpoint ser recurso do usuário final.

### 16.6 Aceite

O próprio jogo:

1. cria save;
2. fecha/flush;
3. encerra;
4. reinicia;
5. reconhece o cartão;
6. carrega o progresso correto.

Depois testar erros e interrupções controladas sem corromper um save previamente válido.

---

## 17. Primeira corrida

Escolher uma combinação fixa e documentada de carro/pista/modo, preferencialmente acessível pelo fluxo mais curto do menu.

### 17.1 Trabalho técnico

- Carregamento de carro e pista.
- Arquivos e assets pertinentes.
- Novos serviços IOP.
- Ampliação de VIF/VU/GS por uso.
- Física original recompilada.
- Input contínuo.
- Câmera.
- HUD.
- Tempo guest coerente.
- Áudio necessário ao cenário.
- Transições.

Não reimplementar a física em código host para contornar uma falha de execução do guest.

### 17.2 Aceite

- Corrida escolhida pelo menu.
- Carro e pista carregados.
- Imagem coerente.
- Aceleração, freio e direção respondem.
- Início e cronômetro funcionam.
- Uma volta completa.
- Resultado ou retorno ao menu.
- Fluxo repetível.

### 17.3 Novos riscos alcançados

Corrida pode exercitar:

- instruções ainda não usadas;
- novos microprogramas VU;
- comportamentos floating-point;
- padrões GS mais complexos;
- código modificado/carregado;
- profundidade de chamadas;
- novos caminhos de streaming;
- orçamento dentro de loops nativos.

Esses riscos devem ser resolvidos pelo primeiro caso reproduzível, não por implementação ampla antecipada.

---

## 18. Compatibilidade até um jogo utilizável

Após a primeira corrida, criar uma matriz por cenário.

| Área | Cenários |
|---|---|
| Boot | Primeira execução, execução com save, ausência válida de cartão. |
| Menu | Submenus, confirmar, cancelar, voltar, mudanças de opções. |
| Corrida | Carros/pistas representativos, diferentes câmeras e múltiplas voltas. |
| Progressão | Recompensas, compra/seleção e caminhos de carreira priorizados. |
| Licenças/eventos | Cenários selecionados e depois ampliados. |
| Replay | Gravar/reproduzir quando alcançado e priorizado. |
| Persistência | Save/load, restart, erros, cartões e arquivos pertinentes. |
| Controle | Dispositivos declarados suportados, hotplug, remapeamento. |
| Áudio/vídeo | Música, efeitos, transições e movies usados. |
| Estabilidade | Sessões longas, corrida → menu → outra corrida. |
| Plataforma | Builds e GPUs/configurações efetivamente testadas. |

Classificar cada cenário:

- não iniciado;
- parcial;
- aprovado;
- falha conhecida;
- fora do escopo declarado.

Não anunciar compatibilidade total a partir de uma volta.

A conclusão funcional do projeto deve ser definida para o **alvo e recursos declarados suportados**. Não significa prova exaustiva de cada estado possível do jogo.

Volantes, force feedback avançado, multiplayer e recursos adicionais podem ter fases próprias.

---

## 19. Performance e robustez

### 19.1 Medir antes de otimizar

Registrar por cenário:

- tempo host;
- tempo guest;
- frames apresentados;
- custo EE AOT;
- custo bridge;
- custo helpers;
- custo IOP;
- custo VU;
- custo GS;
- custo de serviços e instrumentação;
- memória;
- profundidade de chamadas;
- pausas e latência de input.

Uma contagem elevada de passos interpretados sugere um candidato, mas não determina o ganho possível.

### 19.2 Otimizações candidatas

Após profiling:

- entradas traduzidas após fronteiras;
- redução de bridge em PCs quentes;
- especialização de helpers quentes;
- divisão do módulo para reduzir custo de compilação;
- cache de estados gráficos;
- batching que preserve semântica;
- otimização VU;
- redução de logs;
- agenda de eventos mais eficiente.

### 19.3 Entradas de continuação

Distinguir:

- retomar checkpoint;
- retomar código traduzido depois de syscall/chamada.

Para novas entradas traduzidas:

- side effects já aplicados;
- nenhuma repetição de delay slot;
- nenhum serviço duplicado;
- contexto válido;
- mesma semântica;
- saída explícita;
- teste emitter → compilação → driver.

### 19.4 Orçamentos e loops

O orçamento atual não limita necessariamente o interior de um módulo longo.

Se observado:

- medir backedges;
- inserir pontos de orçamento coerentes;
- garantir continuidade sem efeito repetido;
- considerar trampoline quando profundidade host justificar.

Não trocar essa arquitetura preventivamente sem evidência.

### 19.5 Floating-point

Antes de comparar Debug/Release:

- documentar flags do compilador;
- explicitar normalização/clamp;
- testar casos relevantes FPU/VU;
- verificar efeitos de otimizações;
- usar referência independente para peculiaridades.

### 19.6 Alta resolução e ultrawide

Fase posterior à estabilidade:

- separar resolução interna de tamanho da janela;
- estudar coordenadas UI e projeção;
- evitar quebrar HUD;
- validar texturas, clipping, efeitos e framebuffer;
- preservar um modo de referência.

Não integrar essa fase ao aceite do primeiro menu.

---

## 20. Build, aplicação e entrega

### 20.1 Reprodutibilidade

Demonstrar:

- clean build;
- Debug;
- Release;
- target completo;
- dependências fixadas;
- geração local reproduzível;
- executável identificado;
- configuração sem dependência de arquivos ocasionais da workspace.

O gt4boot é excluído do build default; precisa ser construído explicitamente ou pela fixture pertinente.

### 20.2 Aplicação Windows

Entregar uma interface simples para:

- escolher input local;
- validar revisão;
- iniciar;
- configurar controle;
- escolher opções básicas de janela/áudio;
- visualizar erro útil;
- localizar logs.

A interface do usuário não deve exigir entendimento de guest addresses, slices ou RPCs.

### 20.3 Inputs e artefatos

- Payloads e derivados continuam locais e ignored.
- A geração deve verificar inputs antes de uso.
- Metadados distributivos separados de bytes do jogo.
- Empacotamento precisa respeitar a política de artefatos do projeto.
- Verificar quais outputs são próprios e quais derivam dos inputs antes de qualquer publicação.

### 20.4 Aceite de entrega

Em instalação limpa suportada:

- abrir;
- selecionar/verificar input;
- chegar ao menu;
- usar controle;
- executar o cenário de corrida aprovado;
- salvar;
- reiniciar;
- carregar.

Nenhuma etapa deve depender de cwd específico, DLL copiada manualmente ou venv privado do desenvolvedor sem isso estar formalmente previsto.

---

## 21. Testes e evidência

### 21.1 Camadas de verificação

| Camada | Papel |
|---|---|
| Sintética manual | Separar contratos mínimos sem payload de jogo. |
| Emitter compilado | Verificar o C++ realmente gerado. |
| Unit tests | Estado, operações, registradores e serviços. |
| Diferencial interno | Detectar divergência AOT/interpreter no modelo compartilhado. |
| Referência independente | Detectar defeitos compartilhados pelo modelo. |
| Replay de captura | Isolar dispositivos/pipeline com entrada conhecida. |
| Integração live | Demonstrar o guest recompilado avançando e gerando trabalho. |
| Aceite visível | Demonstrar a experiência que o dono pediu. |

### 21.2 Uso da suíte existente

Estender CTest e fixtures Python existentes.

Não criar um segundo sistema permanente de testes apenas para os feedbacks.

Probes temporários podem existir em diretórios ignored, mas os contratos corrigidos precisam de regressões permanentes.

### 21.3 Expectativas antigas

Depois de uma correção, testes que fixam a aproximação antiga podem falhar legitimamente.

Exemplos:

- silêncio de completion sem TIE;
- COUNT com 32 bits;
- censo obrigatoriamente inalterado;
- fronteira antiga preservada.

Investigar a mudança e atualizar a expectativa com evidência. Não restaurar um comportamento defeituoso para manter a tripwire antiga verde.

### 21.4 Comandos de referência

Executar no ambiente de desenvolvimento adequado:

~~~powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl

cmake --build build

cmake --build build --target gt4boot

ctest --test-dir build --output-on-failure -j 1 --timeout 300

& '.\private\tooling-venv\Scripts\python.exe' -B -m unittest discover -s tests/python
~~~

Verificação do input:

~~~powershell
& '.\private\tooling-venv\Scripts\python.exe' -B scripts/gt4disc.py verify '.\Gran Turismo 4 (USA) (v2.00).iso'
~~~

Esses comandos são referência para o executor. Não foram executados como parte da redação deste plano.

Flags novas, como modo RPC estrito, devem ser documentadas como propostas até serem implementadas.

---

## 22. Métricas de progresso

### 22.1 Métricas úteis

- Primeiro contrato desconhecido.
- Primeiro ponto divergente.
- Request e completion relacionados.
- Predicado que mudou.
- Trabalho novo produzido pelo guest.
- Cobertura dinâmica de serviços.
- Bytes transferidos e consumidor.
- Pacotes gráficos.
- Frames apresentados.
- Ações de input consumidas.
- Save carregado após restart.
- Corrida/volta concluída.

### 22.2 Métricas que não bastam

- Número de commits.
- Número de slices.
- Horas de execução.
- Número de interrupts.
- Número de serviços.
- Percentual de decoder.
- Quantidade de funções emitidas.
- Quantidade de servidores bound.
- Ausência de fault.
- Janela aberta.

Essas medidas ajudam a investigar. Sozinhas não fecham o objetivo funcional.

### 22.3 Registro de run

Cada run relevante deve registrar:

~~~text
data
commit
binário/hash
inputs/hashes
compatibility ID
configuração temporal
engine
boot fresco ou origem do checkpoint
contadores relativos e cumulativos
hipótese
instrumentação
orçamento
fronteira
estado observado
diferença relevante
próximo experimento
~~~

---

## 23. Riscos e respostas

| Risco | Resposta |
|---|---|
| Progresso falso por completion artificial | Exigir transferência/dado antes de aceitar completion. |
| Erro HLE compartilhado entre motores | Comparar contratos pequenos com referência independente. |
| Checkpoint antigo contaminando conclusão | Compatibilidade explícita e prefixo fresco. |
| Serviço identificado por nome apenas | Confirmar registro, dispatcher e consumidor. |
| Investigação circular | Hipótese falsificável, orçamento e mudança de experimento. |
| Ausência de hits mal interpretada | Controle positivo e cobertura de AOT/host/DMA/aliases. |
| Complexidade IOP crescendo sem limite | Decidir por produtor causal e recorte demonstrável. |
| Menu aprovado por screenshot/replay | Exigir pipeline live e interação. |
| Renderer pequeno confundido com compatibilidade de corrida | Gates separados e matriz por cenário. |
| Input host sem entrega guest | Rastrear registro, buffer, leitura e efeito no menu. |
| Save externo incompatível com resume | Política explícita de persistência e rollback. |
| Código guest modificado após tradução | Detectar writes relevantes e definir política por região. |
| Debug correto, Release divergente | Flags FP explícitas e comparação por cenário. |
| Otimização prematura | Profiling e regressão antes/depois. |
| Conhecimento perdido na conversa | Documentação curta de estado e evidências duráveis. |

### 23.1 Código modificável e novas regiões executáveis

- Observar writes ao texto físico, incluindo aliases.
- Identificar patches e código carregado.
- Comparar conteúdo com o emitido.
- Parar com contexto quando a política atual não suporta a mudança.
- Escolher AOT adicional, bridge declarado ou outra solução restrita por evidência.

Não implementar um sistema amplo de recompilação dinâmica apenas por possibilidade teórica.

### 23.2 Semântica SUB/DSUB

O caso de mínimos assinados identificado na revisão deve ser auditado com terceira referência antes de mudar.

Não tratá-lo como causa comprovada da parada atual.

---

## 24. Processo do agente executor

### 24.1 Antes de implementar

1. Ler estado atual e este plano.
2. Conferir HEAD e árvore de trabalho.
3. Confirmar que nenhuma outra execução escreve os mesmos arquivos/build.
4. Escolher uma fatia.
5. Declarar contrato e previsão.
6. Definir teste e referência.
7. Registrar impactos em checkpoint/compatibilidade.

### 24.2 Durante a fatia

- Uma mudança comportamental clara.
- Controle de fluxo legível.
- Instrumentação bounded.
- Sem retorno genérico de sucesso.
- Sem mutação manual de flags para anunciar avanço.
- Registrar falhas e limites.
- Usar subagentes para leitura/pesquisa quando útil.
- Manter um escritor por vez.

### 24.3 Ao concluir

- Teste específico.
- Testes apropriados da suíte.
- Evidência antes/depois.
- Limites de alcance.
- Estado e journal atualizados.
- ADR quando houver decisão arquitetural.
- Próximo experimento concreto.

O workflow Git segue as instruções vigentes do dono e do repositório. A elaboração deste plano não executa commits, push ou implementação.

### 24.4 Documentação

Manter:

- STATUS.md: estado atual curto no topo;
- journal: histórico append-only;
- reverse-engineering: evidência;
- decisions: decisões;
- lessons: explicações trabalhadas;
- plano: gates, dependências e prioridades.

Não apagar histórico para tornar o status curto. Marcar conclusões superadas e mover narrativa histórica quando apropriado.

Aprendizado continua sendo parte da missão. Evitar transformar a documentação em uma série de fatias sem avanço ou hipótese nova. Produzir a explicação junto do mecanismo aprendido.

---

## 25. Índice das referências principais

### 25.1 Repositório

- [Estado atual](C:/Antigravity/gt4-staticrecomp/docs/STATUS.md)
- [Requisitos e milestones](C:/Antigravity/gt4-staticrecomp/docs/requirements.md)
- [Feedback GPT](C:/Antigravity/gt4-staticrecomp/GPT_FEEDBACK.md)
- [Feedback Opus](C:/Antigravity/gt4-staticrecomp/OPUS_FEEDBACK.md)
- [Triagem dos feedbacks](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/feedback-triage-2026-10-04.md)
- [Política de tempo por serviço](C:/Antigravity/gt4-staticrecomp/docs/decisions/0016-service-clock.md)
- [Framing async IOP](C:/Antigravity/gt4-staticrecomp/docs/decisions/0023-async-iop-framing.md)
- [Evento SIF: mecanismo, sem unblock](C:/Antigravity/gt4-staticrecomp/docs/decisions/0026-first-originating-event.md)
- [Pesquisa de controle anterior](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m35-slice35-pad-groundwork.md)
- [Asset viewer](C:/Antigravity/gt4-staticrecomp/docs/plans/asset-viewer-roadmap.md)
- [Hooks de assets sem execução](C:/Antigravity/gt4-staticrecomp/docs/reverse-engineering/m33-slice28-asset-dumps.md)

As conclusões antigas sobre ausência de input, timer saudável ou necessidade exclusiva de evento IOP devem ser lidas junto dos feedbacks posteriores.

### 25.2 Referências externas

| Fonte | Utilidade | Cuidado |
|---|---|---|
| [PCSX2](https://github.com/PCSX2/pcsx2) | Dispositivos, debugger, captura e referência funcional. | Não é hardware físico nem motor a incorporar. |
| [PS2SDK](https://github.com/ps2dev/ps2sdk/tree/ac92a9f657d2e531dd8f060250b07f2a5ac6dea5) | ABI, RPC, kernel, tipos e exemplos. | Confirmar variante/revisão do alvo. |
| [ps2autotests](https://github.com/unknownbrackets/ps2autotests/tree/97469ffbed8631277b94e28d01dabd702aa97ef3) | Casos independentes de contratos PS2. | Adaptar o caso necessário, não importar sem verificar. |
| [Ghidra EE Reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) | Análise EE, símbolos e importação para estudo. | Validar suporte efetivo e encodings. |
| [PDTools](https://github.com/Nenkai/PDTools) | Reconstrução e formatos Polyphony. | Container reconstruído não prova estado runtime. |
| [GT4FS](https://github.com/Razer2015/GT4FS) | Leitura independente de formatos de volume. | Variantes e camadas precisam corresponder ao alvo. |
| [GT4Hooks](https://github.com/Nenkai/GT4Hooks) | Vocabulário e estruturas candidatas. | Online US difere do retail USA v2.00. |
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | Estudo de arquitetura. | Não presumir compatibilidade nem solução pronta. |
| [Play!](https://github.com/jpd002/Play-) | Outra implementação de contratos PS2. | Usar como referência específica. |
| [SDL3](https://wiki.libsdl.org/SDL3/CategoryGamepad) | Input e integração de plataforma. | Fixar versão e testar dispositivos declarados. |
| [Vulkan](https://docs.vulkan.org/spec/latest/index.html) | Apresentação e backend gráfico. | Evitar complexidade que não atende ao gate atual. |

Fixar commit/versão dos trechos usados como contrato de implementação. Links para branches móveis são leads de pesquisa, não pins permanentes.

---

## 26. Primeira ordem de trabalho recomendada

Ao retomar a engenharia, o agente deve:

1. Registrar a baseline e política de incompatibilidade dos checkpoints antigos.
2. Corrigir registradores de timer e pending/ack/máscaras.
3. Unificar o avanço temporal.
4. Corrigir handler argument e idle com retorno completo ao scheduler.
5. Corrigir JR e saídas explícitas do módulo.
6. Corrigir DMA sem inventar término de cadeia.
7. Tornar RPC desconhecido explícito.
8. Ampliar comparação de estado.
9. Executar um prefixo fresco.
10. Capturar a primeira divergência causal.
11. Implementar o serviço/produtor necessário.
12. Seguir até dados gráficos, primeira imagem e menu com input.

Se a próxima evidência mostrar uma dependência diferente, adaptar a ordem e registrar o motivo.

O operador não deve ficar preso aos números históricos de slices nem ao primeiro diagnóstico. Deve ficar preso à exigência de demonstrar o comportamento.

---

## 27. Resumo para o dono

Hoje já existe uma base importante: o jogo foi traduzido, compilado e executa bastante da inicialização. Ainda faltam partes do ambiente PS2, e algumas partes existentes têm defeitos que podem produzir uma parada estável.

A primeira meta é **ver o menu original e conseguir mexer nele com controle**.

Para chegar lá, o trabalho será:

~~~text
corrigir a base
    → descobrir exatamente o que impede o boot
    → completar o serviço necessário
    → fazer os gráficos chegarem à tela
    → conectar o controle ao jogo
~~~

Depois disso, escolheremos a próxima entrega com uma demonstração concreta em mãos: áudio, primeira corrida, saves, compatibilidade e desempenho.

O plano cobre esse caminho inteiro, mas a prioridade imediata é um menu real funcionando.

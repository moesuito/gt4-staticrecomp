# Slice 99: a referência também pode dar duas respostas

## A suspeita inicial

Um desvio comum executa a instrução seguinte mesmo quando não é tomado.
Nosso modelo marca uma transferência pendente só quando o desvio é tomado.
Por isso um teste consegue pedir uma interrupção antes daquela instrução
no caminho não tomado. Parecia natural simplesmente proibir esse ponto.

Mas uma correção precisa de referência independente, não apenas de intuição.
A versão exata do PCSX2 já usada nas medições testa eventos ali para certos
desvios em seu interpretador, enquanto seu motor recompilado normalmente
executa a instrução seguinte primeiro. Outras famílias também diferem.

## A consequência

"O emulador faz assim" não basta. Precisamos dizer **qual motor**, instrução,
caminho e modo do debugger. Seu botão de passo executa até um breakpoint
temporário; não escolhe automaticamente o interpretador nem significa sempre
uma única instrução. O código confirma essas chamadas, não uma interrupção
capturada ou o comportamento do console físico.

Encontrar um serviço também não é entrar na exceção da CPU. Essa exceção pode
registrar o endereço do desvio e um indicador de falha na instrução seguinte.
O BIOS escolhe a continuação. A instrução de retorno usa o endereço escolhido,
não adivinha a continuação a partir daquele indicador. Atender um serviço e
continuar em "endereço + 4" não prova esse contrato.

## O que os novos testes provaram

- Onde nosso modelo permite eventos após seis caminhos de desvios, incluindo
  execuções divididas em duas etapas.
- Três casos nos quais os motores param com efeitos e contagens iguais,
  mas discordam sobre ainda haver uma transferência pendente. Uma foto dos
  registradores não mostra todo o estado necessário para retomar esses casos.
- Um evento dentro de uma função traduzida pode devolver o controle pelo
  chamador sem executar a continuação antiga. Restaurar o contexto certo
  executa o retorno e a continuação uma só vez: sete unidades calculadas à
  mão, uma delas o serviço aceito que restaura esse contexto.

São controles do comportamento atual, não um novo relógio. O próximo
experimento deve separar os motores da referência e medir a janela
correspondente de espera de 1000 microssegundos e trabalho da thread principal.
Não mudamos temporizadores nem recuperamos o menu por uma regra inventada.

Veja decisão `0041` e `docs/reverse-engineering/slice99-branch-slot-event-audit.md`.

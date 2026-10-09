# Slice 97: ciclos da referência não são um novo custo fixo

## O que aprendemos

O modelo desconta 1 ms por chamada de serviço do PS2. Na etapa 96, isso
consumia todo o prazo de uma espera de 1 ms antes de a thread principal
executar qualquer instrução.

Na referência PCSX2, medimos outro caso: uma espera de **2 ms**. Entre a
entrada da chamada e o ponto imediatamente anterior ao retorno da BIOS,
passaram 1111 ciclos de EE: aproximadamente **3,77 microssegundos** no
relógio nominal do emulador. Ainda restavam aproximadamente **1996,53
microssegundos** no temporizador. A troca de contexto não consumiu 1 ms.

Isso mostra o problema do atalho atual, mas **não autoriza substituir 1 ms
por 3,77 microssegundos em todas as chamadas**. Não executamos o retorno
nesse intervalo, não medimos todas as classes de serviço, nem a duração
das instruções fora desses serviços. Ciclos do emulador também não provam
ciclos do console físico.

## Duas armadilhas que evitamos

- Duas esperas por semáforo na pilha da atualização pareciam ser a espera
  pelo próximo quadro. A desmontagem mostrou outra sequência: sincronização
  gráfica. A espera real pelo quadro usa `SleepThread`, não esses `WaitSema`.
- Uma versão de formato igual não garante uma estrutura interna igual.
  O leitor temporal agora exige também a versão do PCSX2 que auditamos.
  Mesmo esse nome não autentica um binário; preservamos seu hash e a versão
  exata do código-fonte usado para interpretar os campos.

Na espera real pelo quadro, capturamos a preparação do retorno da BIOS:
1630 ciclos, destino ocioso `0x00081FC0`. **Não vimos a thread principal
executar nesse ponto**, e não capturamos a espera de 1 ms correspondente
ao bloqueio do modelo. Essas lacunas continuam registradas.

## Próximo passo seguro

Contar o trabalho que cada motor realmente conclui, sem alterar ainda o
relógio. Uma chamada a um módulo C++ pode executar uma instrução ou milhares,
por isso não serve como unidade de trabalho. Precisamos contar os caminhos
executados, incluindo laços e instruções após desvios, sem contar novamente
uma instrução em que a execução apenas parou.

Só depois de provar que os dois motores contam o mesmo trabalho podemos
investigar sua conversão para tempo e a entrega precisa de interrupções.

Evidência e limites:
`docs/reverse-engineering/slice97-reference-delay-timing.md`.

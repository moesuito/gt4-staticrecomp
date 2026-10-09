# Slice 98: trabalho concluído ainda não é tempo

## A unidade que faltava

Um módulo C++ traduzido pode entrar em um laço e executar milhares de
instruções antes de devolver o controle. Contar a entrada no módulo como
uma instrução seria errado. Contar todas as instruções escritas no arquivo
também: alguns caminhos nunca são executados.

A nova observação opcional registra cada instrução que **realmente terminou**.
Inclui o desvio e, separadamente, sua instrução seguinte quando ela é
executada. Uma instrução que foi anulada não conta. Se o desvio terminou,
mas a instrução seguinte falhou, permanece uma unidade, não duas nem zero.

## Parar não significa executar

Os motores param antes de executar um serviço da BIOS. Por isso o serviço
é contado por quem o aceita, não por quem encontrou a palavra `syscall`.
Um serviço recusado ou limitado pelo orçamento não conta. Uma parada
repetida no mesmo endereço também não aumenta o total.

O contador pertence à observação do computador inteiro, não a uma thread.
Trocar ou restaurar seus registradores não desfaz o trabalho que já ocorreu.
Ele fica fora dos estados salvos: iniciar uma execução a partir de uma foto
antiga começa uma nova janela, não inventa um total histórico que não foi
salvo. Restaurar uma foto sobre um objeto já observado conserva seu contador.

## Como verificamos

Usamos programas pequenos escritos para os testes, com contagens calculadas
à mão. Por exemplo: uma inicialização, três passagens de decremento/desvio/
instrução seguinte e um retorno com sua instrução seguinte somam
`1 + 3 × 3 + 2 = 12`. Os dois motores precisam chegar exatamente a 12,
não apenas concordar entre si. Também executamos com a observação desligada
para provar que ela não altera os efeitos do programa.

A auditoria encontrou uma diferença antiga no atendimento de um serviço
dentro da instrução seguinte a um desvio especial. Registramos esse limite
explicitamente; não alegamos equivalência universal nem o corrigimos por
adivinhação nesta etapa.

## O que isso não faz

`--count-work` não converte instruções em ciclos ou microssegundos, não
adianta temporizadores e não dá uma vez extra à thread principal. Ainda
precisamos verificar a entrega de interrupções entre desvios/instruções
seguintes e obter uma janela correspondente na referência antes de escolher
uma política de tempo. O menu ainda não foi alcançado pelo modelo.

Contrato e evidência: decisão `0040` e
`docs/reverse-engineering/slice98-completed-guest-work.md`.

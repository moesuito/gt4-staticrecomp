# Slice 100 — a referência também precisa ser testada

Um emulador ajuda a investigar o PS2, mas o seu resultado não é automaticamente
o comportamento do console. Nesta etapa usamos programas pequenos e originais,
sem jogo, cartões ou entrada de controle, para separar essas coisas.

## O controle que funcionou

Sem desvio, a chamada “qual é minha thread?” entrou no endereço de exceção,
foi atendida pelo BIOS e retornou o número 1. Capturamos antes da chamada,
na entrada da exceção, antes da instrução de retorno e na continuação.
Isso é mais forte que apenas encontrar 1 no resultado final.

## O resultado surpreendente

Ao colocar a mesma chamada imediatamente depois de um desvio, o modo dinâmico
do PCSX2 marcou a exceção, mas seguiu o desvio sem atender a chamada. O código
da referência explica como: a chamada escolhe o endereço da exceção; depois,
o código do desvio escreve outro endereço antes que a execução continue.

Por isso distinguimos três fatos:

1. A instrução de chamada pode ter executado.
2. O estado pode indicar uma exceção.
3. Mesmo assim, o código do BIOS que atende a chamada pode não ter executado.

Não devemos corrigir nosso projeto para reproduzir automaticamente essa falha.
Um endereço salvo igual ao do desvio também não prova, sozinho, que o marcador
de exceção após desvio foi ativado: precisamos ler esse marcador separadamente.

## A limitação do interpretador

O outro motor do PCSX2 ignorou as paradas por endereço. O fonte mostra que a
sondagem por instrução depende das opções da compilação de desenvolvimento.
Um teste posterior registrou um aviso de exceção após desvio e ficou em código
do BIOS, sem alcançar o ponto final. Ainda não sabemos a sequência exata dessa
falha nem o retorno que o BIOS selecionou para aquela chamada.

O aviso era do PCSX2, não uma mensagem do BIOS. Registramos também essa correção:
investigação séria conserva os limites e os erros, não só os acertos.

## O que isso muda para o jogo

Melhoramos os instrumentos e evitamos uma falsa correção. **O menu ainda não foi
alcançado pelo projeto.** Falta observar o intervalo real em que o jogo espera
1000 microssegundos e a thread principal pode trabalhar. Os testes pequenos
não justificam uma conversão arbitrária de instruções para tempo.

Evidências, endereços e hashes:
`docs/reverse-engineering/slice100-live-branch-controls.md`.

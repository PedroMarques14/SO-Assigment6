# Home IoT

Projeto de Sistemas Operativos para um simulador habitacional de Internet das Coisas.

## Compilacao

O projeto requer um ambiente POSIX com GCC, `make` e suporte para pthreads, FIFOs,
filas System V e memoria partilhada.

```sh
make
```

Os comandos devem ser executados a partir da raiz deste projeto, porque os caminhos
de configuracao e os nomes dos recursos IPC sao relativos.

## Executaveis

- `home_iot`: gestor central e processos especializados.
- `sensor`: gerador autonomo de telemetria.
- `user_console`: consola interativa.

Nesta primeira versao, os executaveis fornecem o ciclo de vida base e os contratos
partilhados. A logica de telemetria, atuadores, alertas, consola e historico sera
adicionada incrementalmente.

## Testes

```sh
make test
```

Para limpar artefactos de compilacao e runtime:

```sh
make clean
```

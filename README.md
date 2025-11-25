# Trabalho-1-de-SO
Here lies the first project for Operation Systems, for Uni, 4º Period

Antiaéreo - Trabalho 1 de Sistemas Operacionais

-----------------------------------------------



Requisitos:

\- Linux (testado em Ubuntu)

\- g++ com suporte C++17

\- libncurses-dev



Instalação (Ubuntu/Debian):

sudo apt update

sudo apt install build-essential libncurses5-dev libncursesw5-dev



Compilação:

make



Execução:

./antiaereo



Controles:

\- 1,2,3: seleccionar dificuldade no menu inicial (1=Fácil, 2=Médio, 3=Difícil)

\- Teclas:

&nbsp; - Setas ou WASD para ajustar direção / mira

&nbsp; - Z -> diagonal esquerda para cima (45°)

&nbsp; - C -> diagonal direita para cima (45°)

&nbsp; - Espaço -> dispara (usa o primeiro lançador que contiver foguete)

&nbsp; - Q -> sai do jogo



Objetivo:

Abater pelo menos 50% das naves (m) para vencer. Se mais de 50% chegarem ao solo, o jogador perde.



Observações de implementação:

\- Implementado em C++ com pthreads e ncurses.

\- Threads: enemySpawner, enemyThread (por inimigo), rocketThread (por foguete), reloadThread, playerController.

\- Sincronização: mutexes para listas de inimigos/rockets, mutex para launchers, mutex para desenho, condvar para recarga.




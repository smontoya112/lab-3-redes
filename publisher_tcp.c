/*
 * ============================================================
 * publisher_tcp.c
 * ------------------------------------------------------------
 * Publisher del sistema publicación-suscripción sobre TCP.
 *
 * Rol en el sistema:
 *   Representa a un periodista deportivo que reporta eventos
 *   de un partido en vivo. Se conecta al broker por TCP en el
 *   puerto 8081 y envía una cantidad aleatoria de mensajes
 *   sobre el partido indicado por el usuario.
 *
 * Flujo de ejecución:
 *   1. Pide al usuario los dos equipos del partido (ej. A B).
 *   2. Crea un socket TCP y se conecta al broker (127.0.0.1:8081).
 *   3. Genera y envía entre 1 y 10 mensajes aleatorios.
 *   4. Cierra la conexión.
 *
 * Formato del mensaje enviado al broker:
 *   "<equipo1><equipo2>|<evento>\n"
 *   Ejemplo: "AB|Gol de A al equipo B\n"
 *
 * Compilar: gcc publisher_tcp.c -o publisher_tcp
 * Ejecutar: ./publisher_tcp
 * ============================================================
 */

#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<syscall.h>
#include<netinet/in.h>
#include<unistd.h>
#include <arpa/inet.h>

/*
 * creadorMensaje(equipo1, equipo2)
 * --------------------------------
 * Genera aleatoriamente uno de 7 tipos de eventos deportivos
 * y lo formatea como un mensaje listo para enviar al broker.
 *
 * Tipos de eventos posibles (variable x, rango 0-6):
 *   0 – Gol del equipo1 contra equipo2.
 *   1 – Gol del equipo2 contra equipo1.
 *   2 – Cambio de jugador (jugador y entra por jugador z).
 *   3 – Tarjeta amarilla para jugador y del equipo1.
 *   4 – Tarjeta roja para jugador y del equipo1.
 *   5 – Tarjeta amarilla para jugador y del equipo2.
 *   6 – Tarjeta roja para jugador y del equipo2.
 *
 * Variables aleatorias:
 *   x – Tipo de evento (0-6).
 *   y – Número de jugador principal (1-20).
 *   z – Número de jugador secundario en cambios (1-20).
 *
 * Parámetros:
 *   equipo1 – Carácter identificador del equipo local.
 *   equipo2 – Carácter identificador del equipo visitante.
 *
 * Retorna: puntero a cadena dinámica con el mensaje completo.
 *          El llamador es responsable de liberar la memoria con free().
 *
 * Formato de retorno: "<equipo1><equipo2>|<descripción del evento>\n"
 * Ejemplo: "AB|Gol de A al equipo B\n"
 */
char* creadorMensaje(char equipo1, char equipo2){
    char* mensaje = malloc(200);  /* Buffer para el mensaje completo */

    int x = rand() % 7;          /* Tipo de evento */
    int y = rand() % 20 + 1;     /* Jugador 1 (1 a 20) */
    int z = rand() % 20 + 1;     /* Jugador 2 (1 a 20), usado en cambios */

    /* Generar la descripción del evento */
    char evento[100];
    switch(x){
        case 0:
            snprintf(evento, 100, "Gol de %c al equipo %c", equipo1, equipo2);
            break;
        case 1:
            snprintf(evento, 100, "Gol de %c al equipo %c", equipo2, equipo1);
            break;
        case 2:
            snprintf(evento, 100, "Cambio: jugador %d entra por %d", y, z);
            break;
        case 3:
            snprintf(evento, 100, "Tarjeta amarilla para %d del equipo %c", y, equipo1);
            break;
        case 4:
            snprintf(evento, 100, "Tarjeta roja para %d del equipo %c", y, equipo1);
            break;
        case 5:
            snprintf(evento, 100, "Tarjeta amarilla para %d del equipo %c", y, equipo2);
            break;
        case 6:
            snprintf(evento, 100, "Tarjeta roja para %d del equipo %c", y, equipo2);
            break;
    }

    /* Construir mensaje final con el identificador del partido al inicio */
    snprintf(mensaje, 200, "%c%c|%s\n", equipo1, equipo2, evento);
    return mensaje;
}

/*
 * main()
 * ------
 * Punto de entrada del publisher TCP.
 *
 * Pasos:
 *   1. Inicializar semilla aleatoria con el tiempo actual.
 *   2. Leer los identificadores de los dos equipos del partido.
 *   3. Crear socket TCP y conectar al broker en 127.0.0.1:8081.
 *   4. Generar num_mensajes (1-10) eventos aleatorios y enviarlos.
 *   5. Cerrar el socket al terminar.
 */
int main(){
    srand(time(NULL));  /* Semilla para números aleatorios */

    /* Leer equipos por teclado */
    printf("Ingrese los equipos que se enfrentan: ");
    char equipo1, equipo2;
    int num_mensajes = rand() % 10 + 1;  /* Entre 1 y 10 mensajes */
    scanf(" %c %c", &equipo1, &equipo2);

    /* ── Crear socket TCP ── */
    int sock;
    sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    /* Configurar dirección del broker: 127.0.0.1:8081 */
    struct sockaddr_in server_addr;
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(8081);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Establecer conexión TCP con el broker (three-way handshake) */
    int conectado = syscall(SYS_connect, sock,
                            (struct sockaddr *)&server_addr,
                            sizeof(server_addr));
    if(conectado < 0){
        perror("Error al conectar");
        return 1;
    }

    printf("Conectado al broker. Enviando %d mensajes sobre el partido %c%c\n",
           num_mensajes, equipo1, equipo2);

    /* ── Enviar mensajes al broker ── */
    for(int i = 0; i < num_mensajes; i++){
        char* mensaje = creadorMensaje(equipo1, equipo2);
        /* Enviar el mensaje completo por TCP */
        syscall(SYS_write, sock, mensaje, strlen(mensaje));
        printf("Mensaje enviado %d: %s", i+1, mensaje);
        free(mensaje);  /* Liberar memoria dinámica */
    }

    /* Cerrar la conexión (envía FIN al broker) */
    syscall(SYS_close, sock);
    return 0;
}
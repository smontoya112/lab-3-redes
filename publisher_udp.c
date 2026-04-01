/*
 * ============================================================
 * publisher_udp.c
 * ------------------------------------------------------------
 * Publisher del sistema publicación-suscripción sobre UDP.
 *
 * Rol en el sistema:
 *   Genera eventos aleatorios de un partido de fútbol y los
 *   envía al broker UDP como datagramas independientes.
 *   A diferencia del publisher TCP, NO establece una conexión
 *   persistente: cada mensaje es un datagrama autónomo.
 *
 * Flujo de ejecución:
 *   1. Pide al usuario los dos equipos del partido (ej. A B).
 *   2. Crea un socket UDP sin hacer bind (el SO asigna puerto efímero).
 *   3. Genera y envía entre 10 y 20 datagramas al broker.
 *   4. Cierra el socket.
 *
 * Formato del datagrama enviado al broker:
 *   "PUB|<equipo1>|<equipo2>|<evento>\n"
 *   Ejemplo: "PUB|A|B|Gol de A al equipo B\n"
 *
 * El broker interpreta el prefijo "PUB|" para distinguir
 * publicaciones de suscripciones (prefijo "SUB|").
 *
 * Compilar: gcc publisher_udp.c -o publisher_udp
 * Ejecutar: ./publisher_udp
 * ============================================================
 */

#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<sys/syscall.h>
#include<netinet/in.h>
#include<unistd.h>
#include<arpa/inet.h>

/*
 * creadorMensaje(equipo1, equipo2)
 * --------------------------------
 * Genera aleatoriamente uno de 7 tipos de eventos deportivos
 * y retorna solo la descripción del evento (sin el prefijo PUB|).
 * El llamador en main() construye el datagrama completo.
 *
 * Tipos de eventos posibles (variable x, rango 0-6):
 *   0 – Gol del equipo1 contra equipo2.
 *   1 – Gol del equipo2 contra equipo1.
 *   2 – Cambio de jugador (y entra por z).
 *   3 – Tarjeta amarilla para jugador y del equipo1.
 *   4 – Tarjeta roja para jugador y del equipo1.
 *   5 – Tarjeta amarilla para jugador y del equipo2.
 *   6 – Tarjeta roja para jugador y del equipo2.
 *
 * Variables aleatorias:
 *   x – Tipo de evento (0-6).
 *   y – Número de jugador (0-19).
 *   z – Número de jugador secundario en cambios (0-19).
 *
 * Parámetros:
 *   equipo1 – Carácter identificador del equipo local.
 *   equipo2 – Carácter identificador del equipo visitante.
 *
 * Retorna: puntero a cadena dinámica con el texto del evento.
 *          El llamador es responsable de liberar con free().
 *          Retorna NULL si falla malloc().
 */
char* creadorMensaje(char equipo1, char equipo2){
    char* mensajeCompleto = malloc(100);
    if (mensajeCompleto == NULL){
        return NULL;  /* Error de asignación de memoria */
    }

    int x = rand() % 7;   /* Tipo de evento */
    int y = rand() % 20;  /* Jugador principal */
    int z = rand() % 20;  /* Jugador secundario (cambios) */

    switch (x){
        case 0:
            snprintf(mensajeCompleto, 100, "Gol de %c al equipo %c\n", equipo1, equipo2);
            break;
        case 1:
            snprintf(mensajeCompleto, 100, "Gol de %c al equipo %c\n", equipo2, equipo1);
            break;
        case 2:
            snprintf(mensajeCompleto, 100, "Cambio: jugador %d, entra por %d\n", y, z);
            break;
        case 3:
            snprintf(mensajeCompleto, 100, "Tarjeta amarilla para %d del equipo %c\n", y, equipo1);
            break;
        case 4:
            snprintf(mensajeCompleto, 100, "Tarjeta roja para %d del equipo %c\n", y, equipo1);
            break;
        case 5:
            snprintf(mensajeCompleto, 100, "Tarjeta amarilla para %d del equipo %c\n", y, equipo2);
            break;
        case 6:
            snprintf(mensajeCompleto, 100, "Tarjeta roja para %d del equipo %c\n", y, equipo2);
            break;
        default:
            snprintf(mensajeCompleto, 100, "Evento no identificado\n");
            break;
    }

    return mensajeCompleto;
}

/*
 * main()
 * ------
 * Punto de entrada del publisher UDP.
 *
 * Pasos:
 *   1. Inicializar semilla aleatoria.
 *   2. Leer los equipos del partido por teclado.
 *   3. Crear socket UDP (sin bind: el SO asigna un puerto efímero).
 *   4. Configurar la dirección del broker: 127.0.0.1:8080.
 *   5. Generar num_mensajes (10-20) y enviar cada uno con sendto().
 *   6. Cerrar el socket.
 *
 * Nota sobre AF_INET (valor 2):
 *   Se usa el literal "2" en lugar de AF_INET para la familia
 *   de direcciones y SOCK_DGRAM. Ambos son equivalentes en Linux.
 */
int main(){
    srand(time(NULL));  /* Semilla para números aleatorios */

    printf("Ingrese los equipos que se enfrentan: ");
    char equipo1, equipo2;
    scanf(" %c %c", &equipo1, &equipo2);

    /* Entre 10 y 20 mensajes (más que TCP para mayor cobertura) */
    int num_mensajes = rand() % 11 + 10;

    /* Crear socket UDP usando valores numéricos directos:
     * 2 = AF_INET (IPv4), 2 = SOCK_DGRAM (UDP) */
    int sock = syscall(SYS_socket, 2, 2, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    /* Configurar dirección del broker: 127.0.0.1:8080 */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = 2;  /* AF_INET */
    server_addr.sin_port        = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* ── Enviar datagramas al broker ── */
    for(int i = 0; i < num_mensajes; i++){
        /* Generar descripción del evento */
        char* mensaje = creadorMensaje(equipo1, equipo2);
        if(mensaje == NULL){
            perror("Error al crear mensaje");
            syscall(SYS_close, sock);
            return 1;
        }

        /* Construir datagrama completo con prefijo PUB| y equipos
         * Formato: "PUB|<equipo1>|<equipo2>|<evento>\n"
         * El broker extrae equipos en buffer[4] y buffer[6],
         * y el mensaje en buffer+8 */
        char datagrama[200];
        snprintf(datagrama, sizeof(datagrama),
                 "PUB|%c|%c|%s", equipo1, equipo2, mensaje);

        /* Enviar el datagrama al broker (sin conexión previa) */
        int enviado = syscall(
            SYS_sendto,
            sock,
            datagrama,
            strlen(datagrama),
            0,
            &server_addr,
            sizeof(server_addr)
        );

        if (enviado < 0){
            perror("Error al enviar mensaje");
            free(mensaje);
            syscall(SYS_close, sock);
            return 1;
        }

        printf("Mensaje enviado %d: %s\n", i+1, mensaje);
        free(mensaje);  /* Liberar memoria dinámica */
    }

    syscall(SYS_close, sock);
    return 0;
}
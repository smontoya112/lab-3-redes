/*
 * ============================================================
 * broker_udp.c
 * ------------------------------------------------------------
 * Broker del sistema publicación-suscripción sobre UDP.
 *
 * A diferencia de TCP, UDP no mantiene conexiones persistentes.
 * Todo el tráfico llega a un único socket en el puerto 8080.
 * El broker distingue el tipo de mensaje por el prefijo:
 *
 *   "SUB|E1|E2"         → Suscripción: registrar al remitente
 *                          como interesado en el partido E1 vs E2.
 *   "PUB|E1|E2|mensaje" → Publicación: reenviar el mensaje a todos
 *                          los subscribers del partido E1 vs E2.
 *
 * Identificación de subscribers:
 *   Como UDP es sin conexión, el broker identifica a cada
 *   subscriber por su dirección IP + puerto de origen (sockaddr_in).
 *
 * Compilar: gcc broker_udp.c -o broker_udp
 * Ejecutar: ./broker_udp
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * struct Subscriber
 * -----------------
 * Representa a un subscriber registrado en el broker UDP.
 *
 * Campos:
 *   equipo1 – Carácter que identifica al primer equipo del partido
 *             (ej. 'A' en el partido A vs B).
 *   equipo2 – Carácter que identifica al segundo equipo.
 *   addr    – Dirección de red (IP + puerto) del subscriber,
 *             usada para enviarle datagramas con sendto().
 *
 * Nota: los equipos se guardan como char (un solo carácter) porque
 *       el protocolo del laboratorio usa identificadores de un byte.
 */
struct Subscriber {
    char equipo1;
    char equipo2;
    struct sockaddr_in addr;
};

/*
 * mismoSubscriber(a, b)
 * ---------------------
 * Compara dos direcciones sockaddr_in para determinar si
 * corresponden al mismo proceso cliente (misma IP y mismo puerto).
 *
 * Retorna: 1 si son iguales, 0 si son distintas.
 */
int mismoSubscriber(struct sockaddr_in a, struct sockaddr_in b){
    return a.sin_addr.s_addr == b.sin_addr.s_addr &&
           a.sin_port        == b.sin_port;
}

/*
 * yaExiste(subs, num_subs, equipo1, equipo2, origen)
 * ---------------------------------------------------
 * Verifica si ya existe una suscripción idéntica: misma dirección
 * de origen Y mismo partido (equipo1, equipo2).
 *
 * Esto evita registrar duplicados si el subscriber envía el
 * mensaje SUB más de una vez.
 *
 * Parámetros:
 *   subs     – Array de subscribers registrados.
 *   num_subs – Cantidad de subscribers actualmente registrados.
 *   equipo1  – Primer equipo del partido.
 *   equipo2  – Segundo equipo del partido.
 *   origen   – Dirección del subscriber que envió la solicitud.
 *
 * Retorna: 1 si ya existe, 0 si no existe.
 */
int yaExiste(struct Subscriber subs[], int num_subs,
             char equipo1, char equipo2, struct sockaddr_in origen) {
    for (int i = 0; i < num_subs; i++){
        if (subs[i].equipo1 == equipo1 &&
            subs[i].equipo2 == equipo2 &&
            mismoSubscriber(subs[i].addr, origen)){
                return 1;
        }
    }
    return 0;
}

/*
 * main()
 * ------
 * Punto de entrada del broker UDP.
 *
 * Crea un único socket SOCK_DGRAM que recibe todos los datagramas
 * (tanto suscripciones como publicaciones) en el puerto 8080.
 *
 * El bucle principal:
 *   1. Llama a recvfrom() para recibir el siguiente datagrama
 *      junto con la dirección del remitente.
 *   2. Determina el tipo de mensaje por el prefijo (SUB| o PUB|).
 *   3. Si es SUB: registra al remitente como subscriber.
 *   4. Si es PUB: reenvía el contenido del mensaje a todos los
 *      subscribers del partido indicado usando sendto().
 */
int main(){
    /* Crear socket UDP */
    int sock = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    /* Configurar dirección local: escuchar en 127.0.0.1:8080 */
    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family      = AF_INET;
    broker_addr.sin_port        = htons(8080);
    broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Asociar el socket a la dirección local */
    int bind_result = syscall(SYS_bind, sock, &broker_addr, sizeof(broker_addr));
    if(bind_result < 0){
        perror("Error en bind");
        syscall(SYS_close, sock);
        return 1;
    }

    /* Array de subscribers registrados (máximo 100) */
    struct Subscriber subs[100];
    int num_subs = 0;

    printf("Broker UDP escuchando en 127.0.0.1:8080\n");

    /* ── Bucle principal de recepción de datagramas ── */
    while (1){
        char buffer[300];
        memset(buffer, 0, sizeof(buffer));

        /* origen almacenará la dirección del remitente del datagrama */
        struct sockaddr_in origen;
        unsigned int origen_len = sizeof(origen);

        /* Recibir el siguiente datagrama (bloqueante) */
        int recibidos = syscall(
            SYS_recvfrom,
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            &origen,
            &origen_len
        );

        if (recibidos < 0){
            perror("Error al recibir mensaje");
            syscall(SYS_close, sock);
            return 1;
        }
        buffer[recibidos] = '\0'; /* Asegurar terminación de cadena */

        /* ── Caso 1: Mensaje de suscripción ──
         * Formato: "SUB|E1|E2\n"
         * Ejemplo: "SUB|A|B\n"
         * buffer[4] = equipo1, buffer[6] = equipo2
         */
        if (strncmp(buffer, "SUB|", 4) == 0){
            char equipo1 = buffer[4];
            char equipo2 = buffer[6];

            if (!yaExiste(subs, num_subs, equipo1, equipo2, origen) && num_subs < 100){
                /* Registrar nuevo subscriber */
                subs[num_subs].equipo1 = equipo1;
                subs[num_subs].equipo2 = equipo2;
                subs[num_subs].addr    = origen;
                num_subs++;
                printf("Nuevo subscriber suscrito a %c vs %c en puerto %d\n",
                       equipo1, equipo2, ntohs(origen.sin_port));
            } else {
                printf("Subscriber ya existente para %c vs %c en puerto %d\n",
                       equipo1, equipo2, ntohs(origen.sin_port));
            }
        }
        /* ── Caso 2: Mensaje de publicación ──
         * Formato: "PUB|E1|E2|<texto del evento>\n"
         * Ejemplo: "PUB|A|B|Gol de A al equipo B\n"
         * buffer[4] = equipo1, buffer[6] = equipo2
         * buffer+8  = inicio del texto del evento
         */
        else if (strncmp(buffer, "PUB|", 4) == 0){
            char equipo1  = buffer[4];
            char equipo2  = buffer[6];
            char *mensaje = buffer + 8; /* Saltar "PUB|E1|E2|" */

            printf("Mensaje recibido para %c vs %c: %s", equipo1, equipo2, mensaje);

            /* Reenviar a todos los subscribers del partido */
            for (int i = 0; i < num_subs; i++){
                if (subs[i].equipo1 == equipo1 && subs[i].equipo2 == equipo2){
                    int enviados = syscall(
                        SYS_sendto,
                        sock,
                        mensaje,
                        strlen(mensaje),
                        0,
                        &subs[i].addr,
                        sizeof(subs[i].addr)
                    );

                    if (enviados < 0){
                        perror("Error al reenviar mensaje");
                    } else {
                        printf("Reenviado a subscriber en puerto %d\n",
                               ntohs(subs[i].addr.sin_port));
                    }
                }
            }
        }
        /* ── Caso 3: Mensaje desconocido ── */
        else {
            printf("Mensaje desconocido recibido: %s\n", buffer);
        }
    }

    syscall(SYS_close, sock);
    return 0;
}
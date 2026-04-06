/*
 * ============================================================
 * broker_tcp.c
 * ------------------------------------------------------------
 * Broker del sistema publicación-suscripción sobre TCP.
 *
 * Escucha en DOS puertos distintos:
 *   - Puerto 8080: acepta conexiones de Subscribers.
 *   - Puerto 8081: acepta conexiones de Publishers.
 *
 * Flujo general:
 *   1. El broker arranca y queda esperando conexiones.
 *   2. Los subscribers se conectan al puerto 8080 y envían
 *      líneas con formato: "<id>|<partido>\n"
 *      (ej. "1|AB\n") para registrar sus suscripciones.
 *   3. Los publishers se conectan al puerto 8081 y envían
 *      líneas con formato: "<partido>|<evento>\n"
 *      (ej. "AB|Gol de A al equipo B\n").
 *   4. Por cada mensaje de un publisher, el broker lo reenvía
 *      únicamente a los subscribers suscritos a ese partido.
 *
 * Multiplexación:
 *   Se usa select() para atender múltiples conexiones en un
 *   solo hilo, sin bloquear en ningún socket individual.
 *
 * Compilar: gcc broker_tcp.c -o broker_tcp
 * Ejecutar: ./broker_tcp
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
#include <sys/select.h>

/* Límites máximos del sistema */
#define MAX_SUBS    20          /* Máximo número de subscribers simultáneos  */
#define MAX_PUBS    20          /* Máximo número de publishers simultáneos   */
#define MAX_PARTIDOS 50         /* Máximo de partidos a los que puede suscribirse un cliente */
#define BUF_SIZE    4096        /* Tamaño del buffer de lectura por conexión */
#define PARTIDO_LEN 10          /* Longitud máxima del identificador de partido (ej. "AB") */

/*
 * struct subscriber
 * -----------------
 * Representa un subscriber conectado al broker.
 *
 * Campos:
 *   fd        – File descriptor del socket TCP del subscriber.
 *   id        – Identificador numérico enviado por el subscriber
 *               en su primer mensaje. Empieza en -1 (no asignado).
 *   subs      – Array de partidos a los que está suscrito.
 *   num_subs  – Cantidad de partidos registrados actualmente.
 *   active    – 1 si la conexión está activa, 0 si fue cerrada.
 *   read_buf  – Buffer acumulador de datos recibidos (TCP puede
 *               entregar datos parciales, por eso se acumula
 *               hasta encontrar '\n').
 *   buf_len   – Cantidad de bytes actualmente en read_buf.
 */
struct subscriber {
    int fd;
    int id;
    char subs[MAX_PARTIDOS][PARTIDO_LEN];
    int num_subs;
    int active;
    char read_buf[BUF_SIZE];
    int buf_len;
};

/*
 * crear_socket()
 * --------------
 * Crea un socket TCP (SOCK_STREAM, AF_INET) usando la syscall
 * directa SYS_socket y habilita la opción SO_REUSEADDR para
 * poder reutilizar el puerto inmediatamente tras reiniciar el broker.
 *
 * Retorna: file descriptor del socket, o -1 en caso de error.
 */
int crear_socket(){
    int sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return -1;
    }
    /* SO_REUSEADDR (nivel SOL_SOCKET=1, opción SO_REUSEADDR=2)
     * evita el error "Address already in use" al reiniciar. */
    int opt = 1;
    syscall(SYS_setsockopt, sock, 1, 2, &opt, sizeof(opt));
    return sock;
}

/*
 * bind_listen(sock, port)
 * -----------------------
 * Asocia el socket a la dirección 127.0.0.1:<port> y lo pone
 * en modo escucha con una cola de hasta MAX_SUBS conexiones
 * pendientes.
 *
 * Parámetros:
 *   sock – File descriptor del socket creado con crear_socket().
 *   port – Puerto donde escuchará (8080 para subs, 8081 para pubs).
 *
 * Retorna: 0 en éxito, -1 en error.
 */
int bind_listen(int sock, int port){
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);    /* Convertir a big-endian de red */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (syscall(SYS_bind, sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Error al hacer bind");
        return -1;
    }
    if (syscall(SYS_listen, sock, MAX_SUBS) < 0) {
        perror("Error al hacer listen");
        return -1;
    }
    printf("Broker escuchando en puerto %d\n", port);
    return 0;
}

/*
 * procesar_linea_sub(sub, linea)
 * ------------------------------
 * Procesa una línea de suscripción recibida de un subscriber.
 * El formato esperado es: "<id>|<partido>"
 * Ejemplo: "1|AB"
 *
 * - Extrae el id numérico y lo asigna al subscriber si aún
 *   no tenía uno asignado (id == -1).
 * - Registra el partido en el array sub->subs[].
 *
 * Parámetros:
 *   sub   – Puntero al subscriber que envió la línea.
 *   linea – Cadena de texto SIN el '\n' final.
 */
void procesar_linea_sub(struct subscriber *sub, char *linea){
     /* Buscar el separador '|' que divide id y partido */
    char *sep = strchr(linea, '|');
    if (sep == NULL) return;

    *sep = '\0';
    int id = atoi(linea);
    char *partido = sep + 1;

    /* Asignar id solo la primera vez */
    if (sub->id == -1) {
        sub->id = id;
    }

    /* Agregar el partido a la lista de suscripciones */
    if (sub->num_subs < MAX_PARTIDOS) {
        strcpy(sub->subs[sub->num_subs], partido);
        sub->num_subs++;
        printf("Suscriptor %d suscrito a partido: %s\n", id, partido);
    }
}

/*
 * reenviar_a_suscriptores(subs, nsubs, partido, mensaje)
 * ------------------------------------------------------
 * Recorre todos los subscribers activos y envía el mensaje
 * a aquellos que estén suscritos al partido indicado.
 *
 * Parámetros:
 *   subs    – Array de todos los subscribers registrados.
 *   nsubs   – Tamaño del array (MAX_SUBS, no la cantidad activa).
 *   partido – Identificador del partido (ej. "AB").
 *   mensaje – Cadena completa a enviar (ej. "AB|Gol de A\n").
 *
 * Nota: usa SYS_write directamente sobre el fd del subscriber.
 *       Solo envía UNA vez por subscriber aunque esté suscrito
 *       múltiples veces (el break interior lo evita).
 */
void reenviar_a_suscriptores(struct subscriber *subs, int nsubs, char *partido, char *mensaje){
    for (int i = 0; i < nsubs; i++){
        if (!subs[i].active) continue;  /* Saltar conexiones cerradas */
        for (int j = 0; j < subs[i].num_subs; j++){
            if (strcmp(subs[i].subs[j], partido) == 0){
                syscall(SYS_write, subs[i].fd, mensaje, strlen(mensaje));
                break;  /* Evitar envío duplicado al mismo subscriber */
            }
        }
    }
}

/*
 * main()
 * ------
 * Punto de entrada del broker. Inicializa los sockets de escucha,
 * luego entra en el bucle principal de eventos usando select().
 *
 * El bucle realiza cuatro tareas en cada iteración:
 *   1. Aceptar nuevos subscribers (puerto 8080).
 *   2. Aceptar nuevos publishers (puerto 8081).
 *   3. Leer y procesar mensajes de subscribers existentes
 *      (registro de suscripciones).
 *   4. Leer mensajes de publishers y reenviarlos a subscribers.
 *
 * Manejo de mensajes parciales (framing):
 *   TCP no garantiza que un write() en el emisor corresponda a
 *   un read() en el receptor. Por eso se acumulan los bytes en
 *   read_buf / pub_buf y se procesan línea a línea (delimitadas
 *   por '\n'), moviendo los bytes no procesados al inicio del buffer.
 */
int main(){
    /* Crear los dos sockets de escucha */
    int sub_listen = crear_socket();
    int pub_listen = crear_socket();
    if (sub_listen < 0 || pub_listen < 0) return 1;

    if (bind_listen(sub_listen, 8080) < 0) return 1;
    if (bind_listen(pub_listen, 8081) < 0) return 1;

    /* Inicializar array de subscribers: todos inactivos */
    struct subscriber subs[MAX_SUBS];
    int nsubs = 0;
    for (int i = 0; i < MAX_SUBS; i++) subs[i].active = 0;
    
    /* Inicializar array de publishers: fd=-1 significa slot libre */
    int pub_fds[MAX_PUBS];
    char pub_buf[MAX_PUBS][BUF_SIZE];
    int pub_buflen[MAX_PUBS];
    int npubs = 0;
    for (int i = 0; i < MAX_PUBS; i++){
        pub_fds[i] = -1;
        pub_buflen[i] = 0;
    }

    /* Bucle principal de eventos */
    while (1){
        /* Construir el fd_set con todos los sockets activos */
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = 0;

        FD_SET(sub_listen, &readfds);
        if (sub_listen > max_fd) max_fd = sub_listen;

        FD_SET(pub_listen, &readfds);
        if (pub_listen > max_fd) max_fd = pub_listen;

        /* Sockets de subscribers activos */
        for (int i = 0; i < MAX_SUBS; i++){
            if (subs[i].active){
                FD_SET(subs[i].fd, &readfds);
                if (subs[i].fd > max_fd) max_fd = subs[i].fd;
            }
        }
        for (int i = 0; i < MAX_PUBS; i++){
            if (pub_fds[i] > 0){
                FD_SET(pub_fds[i], &readfds);
                if (pub_fds[i] > max_fd) max_fd = pub_fds[i];
            }
        }

        /* Bloquear hasta que al menos un fd esté listo para leer */
        int ready = syscall(SYS_select, max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0){
            perror("Error en select");
            continue;
        }

        /* ── 1. Nuevo subscriber entrante ── */
        if (FD_ISSET(sub_listen, &readfds)){
            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            int new_fd = syscall(SYS_accept, sub_listen, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd >= 0){
                int slot = -1;
                for (int i = 0; i < MAX_SUBS; i++){
                    if (!subs[i].active){ slot = i; break; }
                }
                if (slot >= 0){
                    subs[slot].fd = new_fd;
                    subs[slot].id = -1;
                    subs[slot].num_subs = 0;
                    subs[slot].active = 1;
                    subs[slot].buf_len = 0;
                    nsubs++;
                    printf("Nuevo suscriptor conectado (fd=%d). Total: %d\n", new_fd, nsubs);
                } else {
                    syscall(SYS_close, new_fd);
                }
            }
        }

        /* ── 2. Nuevo publisher entrante ── */
        if (FD_ISSET(pub_listen, &readfds)){
            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            int new_fd = syscall(SYS_accept, pub_listen, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd >= 0){
                int slot = -1;
                for (int i = 0; i < MAX_PUBS; i++){
                    if (pub_fds[i] < 0){ slot = i; break; }
                }
                if (slot >= 0){
                    pub_fds[slot] = new_fd;
                    pub_buflen[slot] = 0;
                    npubs++;
                    printf("Nuevo publisher conectado (fd=%d). Total: %d\n", new_fd, npubs);
                } else {
                    syscall(SYS_close, new_fd);
                }
            }
        }

        /* ── 3. Datos de subscribers (registro de suscripciones) ── */
        for (int i = 0; i < MAX_SUBS; i++){
            if (!subs[i].active) continue;
            if (!FD_ISSET(subs[i].fd, &readfds)) continue;

            int n = syscall(SYS_read, subs[i].fd, subs[i].read_buf + subs[i].buf_len, BUF_SIZE - subs[i].buf_len - 1);
            if (n <= 0){
                printf("Suscriptor %d desconectado\n", subs[i].id);
                syscall(SYS_close, subs[i].fd);
                subs[i].active = 0;
                nsubs--;
                continue;
            }
            subs[i].buf_len += n;
            subs[i].read_buf[subs[i].buf_len] = '\0';

            char *pos = subs[i].read_buf;
            char *fin;
            while ((fin = strchr(pos, '\n')) != NULL){
                *fin = '\0';
                procesar_linea_sub(&subs[i], pos);
                pos = fin + 1;
            }
            int resto = subs[i].buf_len - (pos - subs[i].read_buf);
            if (resto > 0) memmove(subs[i].read_buf, pos, resto);
            subs[i].buf_len = resto;
        }

        /* ── 4. Datos de publishers (mensajes a reenviar) ── */
        for (int i = 0; i < MAX_PUBS; i++){
            if (pub_fds[i] < 0) continue;
            if (!FD_ISSET(pub_fds[i], &readfds)) continue;

            int n = syscall(SYS_read, pub_fds[i], pub_buf[i] + pub_buflen[i], BUF_SIZE - pub_buflen[i] - 1);
            if (n <= 0){
                printf("Publisher fd=%d desconectado\n", pub_fds[i]);
                syscall(SYS_close, pub_fds[i]);
                pub_fds[i] = -1;
                npubs--;
                continue;
            }
            pub_buflen[i] += n;
            pub_buf[i][pub_buflen[i]] = '\0';

            char *pos = pub_buf[i];
            char *fin;
            while ((fin = strchr(pos, '\n')) != NULL){
                *fin = '\0';
                char *sep = strchr(pos, '|');
                if (sep != NULL){
                    *sep = '\0';
                    char *partido = pos;
                    char *evento = sep + 1;
                    char mensaje[BUF_SIZE];
                    snprintf(mensaje, BUF_SIZE, "%s|%s\n", partido, evento);
                    printf("Broker reenviando: %s", mensaje);
                    reenviar_a_suscriptores(subs, MAX_SUBS, partido, mensaje);
                }
                pos = fin + 1;
            }
            int resto = pub_buflen[i] - (pos - pub_buf[i]);
            if (resto > 0) memmove(pub_buf[i], pos, resto);
            pub_buflen[i] = resto;
        }
    }

    /* Cerrar sockets de escucha (código inalcanzable en la práctica) */
    syscall(SYS_close, sub_listen);
    syscall(SYS_close, pub_listen);
    return 0;
}

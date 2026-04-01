/*
 * ============================================================
 * subscriber_tcp.c
 * ------------------------------------------------------------
 * Subscriber del sistema publicación-suscripción sobre TCP.
 *
 * Rol en el sistema:
 *   Representa a un hincha que sigue uno o varios partidos
 *   en tiempo real. Se conecta al broker por TCP en el puerto
 *   8080, envía sus suscripciones y luego escucha eventos
 *   de forma indefinida.
 *
 * Flujo de ejecución:
 *   1. Pedir al usuario su id y los partidos a seguir.
 *   2. Crear socket TCP y conectar al broker (127.0.0.1:8080).
 *   3. Enviar una línea de suscripción por cada partido.
 *   4. Entrar en bucle de recepción: imprimir cada evento que
 *      envíe el broker hasta que se cierre la conexión.
 *
 * Formato de suscripción enviada al broker:
 *   "<id>|<partido>\n"
 *   Ejemplo: "1|AB\n"
 *
 * Formato de mensajes recibidos del broker:
 *   "<partido>|<evento>\n"
 *   Ejemplo: "AB|Gol de A al equipo B\n"
 *
 * Compilar: gcc subscriber_tcp.c -o subscriber_tcp
 * Ejecutar: ./subscriber_tcp
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
 * pedir_partido()
 * ---------------
 * Lee una línea de texto de stdin y elimina el '\n' final.
 * Usada para capturar el identificador de cada partido
 * (ej. "AB") que ingresa el usuario.
 *
 * Retorna: puntero a buffer estático con la línea leída.
 *
 * Nota: al ser static, el buffer persiste entre llamadas pero
 *       se sobreescribe en cada invocación. El llamador debe
 *       copiar el valor si necesita preservarlo (ver strdup más abajo).
 */
char* pedir_partido() {
    static char header[10];
    fgets(header, sizeof(header), stdin);
    header[strcspn(header, "\n")] = '\0';  /* Eliminar '\n' del final */
    return header;
}

/*
 * main()
 * ------
 * Punto de entrada del subscriber TCP.
 *
 * Pasos:
 *   1. Leer id del subscriber y cantidad de partidos a seguir.
 *   2. Leer el identificador de cada partido y guardarlo con strdup().
 *   3. Crear socket TCP y conectar al broker en 127.0.0.1:8080.
 *   4. Enviar una línea de suscripción por cada partido.
 *   5. Entrar en bucle de lectura hasta que el broker cierre la conexión.
 *   6. Liberar memoria y cerrar socket.
 */
int main(){

    /* ── Paso 1: Leer configuración del usuario ── */
    printf("Ingrese su id de suscriptor: ");
    int id;
    scanf("%d", &id);

    printf("Ingrese el numero de partidos a los que se va a suscribir: ");
    int partidos_suscritos;
    scanf("%d", &partidos_suscritos);

    /* Consumir el '\n' que queda en stdin después del último scanf */
    getchar();
    printf("Ingrese los partidos y luego de enter (ej: AB):\n");

    /* ── Paso 2: Leer identificadores de partidos ── */
    /* VLA: array de punteros de tamaño variable (C99) */
    char* suscripciones[partidos_suscritos];

    for (int i = 0; i < partidos_suscritos; i++) {
        char* partido = pedir_partido();
        if (strlen(partido) == 0) {
            break;  /* El usuario no ingresó nada, detener */
        }
        /* strdup() copia la cadena en memoria dinámica,
         * necesario porque pedir_partido() usa buffer estático */
        suscripciones[i] = strdup(partido);
        printf("Suscrito al partido: %s\n", partido);
    }

    /* ── Paso 3: Crear socket TCP ── */
    int sock;
    sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    /* Configurar dirección del broker: 127.0.0.1:8080 */
    struct sockaddr_in server_addr;
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Establecer conexión TCP con el broker */
    int conectado = syscall(SYS_connect, sock,
                            (struct sockaddr *)&server_addr,
                            sizeof(server_addr));
    if(conectado < 0){
        perror("Error al conectar");
        return 1;
    }

    printf("Conectado al broker. Enviando suscripciones...\n");

    /* ── Paso 4: Enviar suscripciones al broker ──
     * Formato por línea: "<id>|<partido>\n"
     * Ejemplo: "1|AB\n"
     */
    for(int i = 0; i < partidos_suscritos; i++){
        char buffer[50];
        sprintf(buffer, "%d|%s\n", id, suscripciones[i]);
        syscall(SYS_write, sock, buffer, strlen(buffer));
        printf("Suscripcion enviada %d: %s", i+1, buffer);
    }

    /* Liberar memoria de las suscripciones (ya fueron enviadas) */
    for (int i = 0; i < partidos_suscritos; i++) {
        free(suscripciones[i]);
    }

    /* ── Paso 5: Bucle de recepción de eventos ──
     * Lee del socket hasta que el broker cierre la conexión
     * (read() retorna 0 = EOF) o ocurra un error (retorna < 0).
     * Nota: este read() puede recibir múltiples mensajes juntos
     * (TCP es un stream), pero para imprimirlos en consola
     * no hay problema ya que se imprimen tal cual.
     */
    printf("\nEsperando eventos de los partidos suscritos...\n");

    char recv_buf[1024];
    int bytes_leidos;
    while ((bytes_leidos = syscall(SYS_read, sock, recv_buf, sizeof(recv_buf) - 1)) > 0) {
        recv_buf[bytes_leidos] = '\0';  /* Asegurar terminación de cadena */
        printf("Evento recibido: %s", recv_buf);
    }

    /* ── Paso 6: Cerrar conexión ── */
    syscall(SYS_close, sock);
    return 0;
}
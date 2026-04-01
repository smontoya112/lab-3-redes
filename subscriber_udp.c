/*
 * ============================================================
 * subscriber_udp.c
 * ------------------------------------------------------------
 * Subscriber del sistema publicación-suscripción sobre UDP.
 *
 * Rol en el sistema:
 *   Representa a un hincha que sigue un partido en tiempo real
 *   usando UDP. A diferencia del subscriber TCP, este cliente
 *   debe hacer bind en un puerto local conocido para que el
 *   broker sepa dónde enviarle los datagramas.
 *
 * Flujo de ejecución:
 *   1. Crear socket UDP y hacer bind en un puerto local elegido
 *      por el usuario.
 *   2. Enviar un datagrama de suscripción al broker.
 *   3. Entrar en bucle de recepción: imprimir cada datagrama
 *      que llegue del broker indefinidamente.
 *
 * Por qué es necesario el bind en UDP:
 *   En UDP no hay conexión. Para que el broker pueda enviar
 *   datagramas al subscriber, necesita conocer su IP y puerto.
 *   Al hacer bind, el subscriber reserva un puerto fijo en
 *   127.0.0.1 que el broker registra cuando llega el SUB|.
 *
 * Formato del datagrama de suscripción enviado al broker:
 *   "SUB|<equipo1>|<equipo2>\n"
 *   Ejemplo: "SUB|A|B\n"
 *
 * Formato de datagramas recibidos del broker:
 *   "<texto del evento>\n"
 *   Ejemplo: "Gol de A al equipo B\n"
 *
 * Compilar: gcc subscriber_udp.c -o subscriber_udp
 * Ejecutar: ./subscriber_udp
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * main()
 * ------
 * Punto de entrada del subscriber UDP.
 *
 * Pasos:
 *   1. Crear socket UDP.
 *   2. Leer el puerto local elegido por el usuario y hacer bind.
 *   3. Leer los equipos del partido a seguir.
 *   4. Enviar datagrama de suscripción al broker (127.0.0.1:8080).
 *   5. Entrar en bucle de recepción de datagramas.
 */
int main(){
    /* ── Paso 1: Crear socket UDP ── */
    int sock = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    /* ── Paso 2: Bind en el puerto local ──
     * El usuario elige el puerto (ej. 9001, 9002) para que
     * cada subscriber use uno diferente en la misma máquina.
     */
    printf("Ingrese el puerto local del subscriber: ");
    int puerto_local;
    scanf("%d", &puerto_local);

    struct sockaddr_in subscriber_addr;
    memset(&subscriber_addr, 0, sizeof(subscriber_addr));
    subscriber_addr.sin_family      = AF_INET;
    subscriber_addr.sin_port        = htons(puerto_local);
    subscriber_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Asociar el socket al puerto local elegido */
    int bind_result = syscall(SYS_bind, sock,
                               &subscriber_addr, sizeof(subscriber_addr));
    if (bind_result < 0){
        perror("Error en bind");
        syscall(SYS_close, sock);
        return 1;
    }

    /* ── Paso 3: Leer el partido a seguir ── */
    printf("Ingrese los equipos del partido a suscribirse: ");
    char equipo1, equipo2;
    scanf(" %c %c", &equipo1, &equipo2);

    /* ── Paso 4: Enviar suscripción al broker ──
     * Dirección del broker: 127.0.0.1:8080
     * El broker registrará la IP+puerto del origen de este datagrama
     * (es decir, 127.0.0.1:puerto_local) como destino para reenvíos.
     */
    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family      = AF_INET;
    broker_addr.sin_port        = htons(8080);
    broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Construir mensaje de suscripción: "SUB|E1|E2\n" */
    char suscripcion[100];
    snprintf(suscripcion, sizeof(suscripcion),
             "SUB|%c|%c\n", equipo1, equipo2);

    int enviados = syscall(
        SYS_sendto,
        sock,
        suscripcion,
        strlen(suscripcion),
        0,
        &broker_addr,
        sizeof(broker_addr)
    );

    if (enviados < 0){
        perror("Error al enviar suscripcion");
        syscall(SYS_close, sock);
        return 1;
    }

    printf("Suscrito al partido %c vs %c\n", equipo1, equipo2);
    printf("Esperando actualizaciones...\n");

    /* ── Paso 5: Bucle de recepción de datagramas ──
     * recvfrom() bloquea hasta recibir un datagrama.
     * Cada llamada recibe exactamente UN datagrama completo
     * (a diferencia de TCP donde los datos son un stream continuo).
     * El campo 'origen' captura la dirección del remitente
     * (en este caso, siempre será el broker).
     */
    while(1) {
        char buffer[200];
        memset(buffer, 0, sizeof(buffer));

        struct sockaddr_in origen;
        unsigned int origen_len = sizeof(origen);

        int recibidos = syscall(
            SYS_recvfrom,
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            &origen,      /* Dirección del remitente (broker) */
            &origen_len
        );

        if (recibidos < 0){
            perror("Error al recibir mensaje");
            syscall(SYS_close, sock);
            return 1;
        }

        buffer[recibidos] = '\0';  /* Asegurar terminación de cadena */
        printf("Actualizacion recibida: %s", buffer);
    }

    syscall(SYS_close, sock);
    return 0;
}
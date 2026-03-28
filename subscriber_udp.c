#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(){
    int sock = syscall(SYS_socket,  AF_INET, SOCK_DGRAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    printf("Ingrese el puerto local del subscriber: ");
    int puerto_local;
    scanf("%d", &puerto_local);

    struct sockaddr_in subscriber_addr;
    memset(&subscriber_addr, 0, sizeof(subscriber_addr));
    subscriber_addr.sin_family = AF_INET;
    subscriber_addr.sin_port = htons(puerto_local);
    subscriber_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int bind_result = syscall(
        SYS_bind,
        sock,
        &subscriber_addr,
        sizeof(subscriber_addr)
    );

    if (bind_result < 0){
        perror("Error en bind");
        syscall(SYS_close,sock);
        return 1;
    }

    printf("Ingrese los equipos del partido a suscribirse: ");
    char equipo1, equipo2;
    scanf(" %c %c", &equipo1, &equipo2);

    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port = htons(8080);
    broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    char suscripcion[100];
    snprintf(suscripcion, sizeof(suscripcion), "SUB|%c|%c\n", equipo1, equipo2);

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

    while(1) {
        char buffer[200];
        memset(buffer, 0, sizeof(buffer));
        struct sockaddr_in origen;
        unsigned int origen_len = sizeof(origen);

        int recibidos = syscall(
            SYS_recvfrom, 
            sock, 
            buffer,
            sizeof(buffer)-1,
            0,
            &origen,
            &origen_len
        );

        if (recibidos < 0){
            perror("Error al recibir mensaje");
            syscall(SYS_close, sock);
            return 1;
        }

        buffer[recibidos] = '\0';
        printf("Actualizacion recibida: %s", buffer);
    }

    syscall(SYS_close, sock);
    return 0;
}
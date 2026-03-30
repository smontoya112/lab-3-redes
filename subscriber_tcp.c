#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<syscall.h>
#include<netinet/in.h>
#include<unistd.h>
#include <arpa/inet.h>

char* pedir_partido() {
    static char header[10];
    fgets(header, sizeof(header), stdin);
    header[strcspn(header, "\n")] = '\0';
    return header;
}

int main(){

    printf("Ingrese su id de suscriptor: ");
    int id;
    scanf("%d", &id);

    printf("Ingrese el numero de partidos a los que se va a suscribir: ");
    int partidos_suscritos;
    scanf("%d", &partidos_suscritos);

    getchar();
    printf("Ingrese los partidos y luego de enter (ej: AB):\n");

    char* suscripciones[partidos_suscritos];

    for (int i = 0; i < partidos_suscritos; i++) {
        char* partido = pedir_partido();
        if (strlen(partido) == 0) {
            break;
        }
        suscripciones[i] = strdup(partido);
        printf("Suscrito al partido: %s\n", partido);
    }

    int sock;
    sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int conectado = syscall(SYS_connect, sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    if(conectado < 0){
        perror("Error al conectar");
        return 1;
    }

    printf("Conectado al broker. Enviando suscripciones...\n");

    for(int i = 0; i < partidos_suscritos; i++){
        char buffer[50];
        sprintf(buffer, "%d|%s\n", id, suscripciones[i]);
        syscall(SYS_write, sock, buffer, strlen(buffer));
        printf("Suscripcion enviada %d: %s", i+1, buffer);
    }

    for (int i = 0; i < partidos_suscritos; i++) {
        free(suscripciones[i]);
    }

    printf("\nEsperando eventos de los partidos suscritos...\n");

    char recv_buf[1024];
    int bytes_leidos;
    while ((bytes_leidos = syscall(SYS_read, sock, recv_buf, sizeof(recv_buf) - 1)) > 0) {
        recv_buf[bytes_leidos] = '\0';
        printf("Evento recibido: %s", recv_buf);
    }

    syscall(SYS_close, sock);
    return 0;
}

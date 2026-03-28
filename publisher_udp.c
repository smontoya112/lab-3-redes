#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<sys/syscall.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<unistd.h>
#include<arpa/inet.h>

char* creadorMensaje(char equipo1, char equipo2){
    char* mensajeCompleto = malloc(100);
    if (mensajeCompleto == NULL){
        return NULL;
    }

    int x = rand() % 7;
    int y = rand() % 20;
    int z = rand() % 20;

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
    }

    return mensajeCompleto;

}


int main(){
    srand(time(NULL));
    printf("Ingrese los equipos que se enfrentan: ");
    char equipo1, equipo2;

    scanf(" %c %c", &equipo1, &equipo2);

    int sock;
    sock = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    for(int i = 0; i<10; i++){
        char* mensaje = creadorMensaje(equipo1, equipo2);
        if(mensaje == NULL){
            perror("Error al crear mensaje");
            syscall(SYS_close, sock);
            return 1;
        }

        int enviado = syscall (
            SYS_sendto,
            sock,
            mensaje,
            strlen(mensaje),
            0,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        );

        if (enviado < 0){
            perror("Error al enviar mensaje");
            free(mensaje);
            syscall(SYS_close, sock);
            return 1;
        }

        printf("Mensaje enviado %d: %s\n", i+1, mensaje);
        free(mensaje);
    }

    syscall(SYS_close, sock);
    return 0;
}
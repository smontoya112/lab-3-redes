#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<syscall.h>
#include<netinet/in.h>
#include<unistd.h>
#include <arpa/inet.h>

char* creadorMensaje(char equipo1, char equipo2){
    char* mensaje = malloc(200);

    int x = rand() % 7;
    int y = rand() % 20 + 1;
    int z = rand() % 20 + 1;

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

    snprintf(mensaje, 200, "%c%c|%s\n", equipo1, equipo2, evento);
    return mensaje;
}

int main(){
    srand(time(NULL));
    printf("Ingrese los equipos que se enfrentan: ");
    char equipo1, equipo2;
    int num_mensajes = rand() % 10 + 1;

    scanf(" %c %c", &equipo1, &equipo2);

    // INICIO DE ENVIO DE MENSAJES POR TCP
    int sock;
    sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8081);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int conectado = syscall(SYS_connect, sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    if(conectado < 0){
        perror("Error al conectar");
        return 1;
    }

    printf("Conectado al broker. Enviando %d mensajes sobre el partido %c%c\n", num_mensajes, equipo1, equipo2);

    for(int i = 0; i < num_mensajes; i++){
        char* mensaje = creadorMensaje(equipo1, equipo2);
        syscall(SYS_write, sock, mensaje, strlen(mensaje));
        printf("Mensaje enviado %d: %s", i+1, mensaje);
        free(mensaje);
    }
    syscall(SYS_close, sock);
    return 0;
}

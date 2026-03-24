#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<netinet/in.h>
#include<syscall.h>
#include<unistd.h>
#include <arpa/inet.h>

//Implementar socket
//Bind
//listen
//accept
//read
//connect
//send/write a cada sub
//close

int crear_socket(){
    int sock;
    sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock<0){
        perror("Error al crear socket");
        return 1;
    }
    return sock;
}

int bind_socket(int sock, struct sockaddr_in server_addr){
    if (syscall(SYS_bind, sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error al hacer bind");
        return 1;
    }
    return 0;
}

int main(){
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int sock = crear_socket();
    if (bind_socket(sock, server_addr) < 0) {
        return 1;
    }
    char[] mensajes[10][100];
    while true{
        int backlog = 10;
        int listen_result = syscall(SYS_listen, sock, backlog);
        if (listen_result < 0) {
            perror("Error al escuchar");
            return 1;
        }
        int accept_result = syscall(SYS_accept, sock, NULL, NULL);
        if (accept_result < 0) {
            perror("Error al aceptar conexión");
            return 1;
        }
        char buffer[100];
        ssize_t read_result = syscall(SYS_read, accept_result, buffer, sizeof(buffer));
        if (read_result < 0) {
            perror("Error al leer");
            return 1;
        }
    }

    
    return 0;
}
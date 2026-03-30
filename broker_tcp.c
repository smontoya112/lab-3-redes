#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<time.h>
#include<syscall.h>
#include<netinet/in.h>
#include<unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define MAX_SUBS 20
#define MAX_PUBS 20
#define MAX_PARTIDOS 50
#define BUF_SIZE 4096
#define PARTIDO_LEN 10

struct subscriber {
    int fd;
    int id;
    char subs[MAX_PARTIDOS][PARTIDO_LEN];
    int num_subs;
    int active;
    char read_buf[BUF_SIZE];
    int buf_len;
};

int crear_socket(){
    int sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return -1;
    }
    int opt = 1;
    syscall(SYS_setsockopt, sock, 1, 2, &opt, sizeof(opt));
    return sock;
}

int bind_listen(int sock, int port){
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
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

void procesar_linea_sub(struct subscriber *sub, char *linea){
    char *sep = strchr(linea, '|');
    if (sep == NULL) return;

    *sep = '\0';
    int id = atoi(linea);
    char *partido = sep + 1;

    if (sub->id == -1) {
        sub->id = id;
    }

    if (sub->num_subs < MAX_PARTIDOS) {
        strcpy(sub->subs[sub->num_subs], partido);
        sub->num_subs++;
        printf("Suscriptor %d suscrito a partido: %s\n", id, partido);
    }
}

void reenviar_a_suscriptores(struct subscriber *subs, int nsubs, char *partido, char *mensaje){
    for (int i = 0; i < nsubs; i++){
        if (!subs[i].active) continue;
        for (int j = 0; j < subs[i].num_subs; j++){
            if (strcmp(subs[i].subs[j], partido) == 0){
                syscall(SYS_write, subs[i].fd, mensaje, strlen(mensaje));
                break;
            }
        }
    }
}

int main(){
    int sub_listen = crear_socket();
    int pub_listen = crear_socket();
    if (sub_listen < 0 || pub_listen < 0) return 1;

    if (bind_listen(sub_listen, 8080) < 0) return 1;
    if (bind_listen(pub_listen, 8081) < 0) return 1;

    struct subscriber subs[MAX_SUBS];
    int nsubs = 0;
    for (int i = 0; i < MAX_SUBS; i++) subs[i].active = 0;

    int pub_fds[MAX_PUBS];
    char pub_buf[MAX_PUBS][BUF_SIZE];
    int pub_buflen[MAX_PUBS];
    int npubs = 0;
    for (int i = 0; i < MAX_PUBS; i++){
        pub_fds[i] = -1;
        pub_buflen[i] = 0;
    }

    while (1){
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = 0;

        FD_SET(sub_listen, &readfds);
        if (sub_listen > max_fd) max_fd = sub_listen;

        FD_SET(pub_listen, &readfds);
        if (pub_listen > max_fd) max_fd = pub_listen;

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

        int ready = syscall(SYS_select, max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0){
            perror("Error en select");
            continue;
        }

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

    syscall(SYS_close, sub_listen);
    syscall(SYS_close, pub_listen);
    return 0;
}

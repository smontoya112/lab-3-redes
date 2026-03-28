#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct Subscriber {
    char equipo1;
    char equipo2;
    struct sockaddr_in addr;
};

int mismoSubscriber(struct sockaddr_in a, struct sockaddr_in b){
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

int yaExiste(struct Subscriber subs[], int num_subs, char equipo1, char equipo2, struct sockaddr_in origen) {
    for (int i = 0; i < num_subs; i++){
        if (subs[i].equipo1 == equipo1 &&
            subs[i].equipo2 == equipo2 &&
            mismoSubscriber(subs[i].addr, origen)){
                return 1;
        }
    }
    return 0;
}    

int main(){
    int sock = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
    if (sock < 0){
        perror("Error al crear socket");
        return 1;
    }
    
    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port = htons(8080);
    broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int bind_result = syscall(
        SYS_bind,
        sock,
        &broker_addr,
        sizeof(broker_addr)
    );
    if(bind_result < 0){
        perror("Error en bind");
        syscall(SYS_close, sock);
        return 1;
    }

    struct Subscriber subs[100];
    int num_subs = 0;

    printf("Broker UDP escuchando en 127.0.0.1:8080\n");

    while (1){
        char buffer[300];
        memset(buffer,0, sizeof(buffer));

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

        if (strncmp(buffer, "SUB|", 4)==0){
            char equipo1 = buffer[4];
            char equipo2 = buffer[6];

            if (!yaExiste(subs, num_subs, equipo1, equipo2, origen) && num_subs < 100){
                subs[num_subs].equipo1 = equipo1;
                subs[num_subs].equipo2 = equipo2;
                subs[num_subs].addr = origen;
                num_subs++;

                printf("Nuevo subscriber suscrito a %c vs %c en puerto %d\n", equipo1, equipo2, ntohs(origen.sin_port));
            } else {
                printf("Subscriber ya existente para %c vs %c en puerto %d\n", equipo1, equipo2, ntohs(origen.sin_port));
            }
        }
        else if (strncmp(buffer, "PUB|",4)==0){
            char equipo1 = buffer[4];
            char equipo2 = buffer[6];
            char *mensaje = buffer + 8;

            printf("Mensaje recibido para %c vs %c: %s", equipo1, equipo2, mensaje);

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
                        perror("Error al reenviar menaje");
                    } else {
                        printf("Reenviado a subscriber en puerto %d\n", ntohs(subs[i].addr.sin_port));
                    }
                }
            }
        } else{
            printf("Mensaje desconocido recibido: %s\n", buffer);
        }
    }
    syscall(SYS_close, sock);
    return 0;
}
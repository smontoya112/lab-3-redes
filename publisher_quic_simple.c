/*
 * ============================================================
 * publisher_quic_simple.c
 * ============================================================
 * QUIC-simulado sin cifrado (sin OpenSSL).
 * Publisher que envía eventos sobre un partido.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE      4096
#define BROKER_PORT   8080
#define PKT_HANDSHAKE     0x01
#define PKT_HANDSHAKE_ACK 0x02
#define PKT_DATA          0x03
#define PKT_ACK           0x04
#define PKT_CLOSE         0x05
#define CONN_ID_LEN   8
#define HEADER_LEN    (CONN_ID_LEN + 4 + 1)
#define RETRANSMIT_US 500000
#define MAX_RETRIES   5

typedef struct {
    int                sock_fd;
    uint8_t            conn_id[CONN_ID_LEN];
    struct sockaddr_in broker_addr;
    uint32_t           seq;
} PubCtx;

static int serializar(const uint8_t *conn_id, uint32_t seq, uint8_t tipo,
                       const uint8_t *payload, int plen, uint8_t *buf) {
    int pos = 0;
    memcpy(buf + pos, conn_id, CONN_ID_LEN); pos += CONN_ID_LEN;
    buf[pos++] = (seq >> 24) & 0xFF;
    buf[pos++] = (seq >> 16) & 0xFF;
    buf[pos++] = (seq >>  8) & 0xFF;
    buf[pos++] = (seq      ) & 0xFF;
    buf[pos++] = tipo;
    if (payload && plen > 0) { memcpy(buf + pos, payload, plen); pos += plen; }
    return pos;
}

static int enviar_con_ack(PubCtx *ctx, uint8_t tipo,
                           const char *payload, int plen) {
    uint8_t buf[BUF_SIZE + HEADER_LEN];
    int buf_len;

    if (tipo == PKT_HANDSHAKE) {
        buf_len = serializar(ctx->conn_id, ctx->seq, tipo,
                              (const uint8_t *)payload, plen, buf);
    } else if (payload && plen > 0) {
        buf_len = serializar(ctx->conn_id, ctx->seq, tipo,
                              (const uint8_t *)payload, plen, buf);
    } else {
        buf_len = serializar(ctx->conn_id, ctx->seq, tipo,
                              NULL, 0, buf);
    }

    uint32_t seq_enviado = ctx->seq;

    for (int intento = 0; intento < MAX_RETRIES; intento++) {
        sendto(ctx->sock_fd, buf, buf_len, 0,
               (struct sockaddr *)&ctx->broker_addr,
               sizeof(ctx->broker_addr));

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = RETRANSMIT_US };

        if (select(ctx->sock_fd + 1, &readfds, NULL, NULL, &tv) <= 0) {
            printf("Timeout esperando ACK seq=%u, reintento %d/%d\n",
                   seq_enviado, intento + 1, MAX_RETRIES);
            continue;
        }

        uint8_t recv_buf[HEADER_LEN + 64];
        struct sockaddr_in origen;
        socklen_t olen = sizeof(origen);
        ssize_t n = recvfrom(ctx->sock_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&origen, &olen);
        if (n < (ssize_t)HEADER_LEN) continue;

        uint8_t  r_tipo = recv_buf[CONN_ID_LEN + 4];
        uint32_t r_seq  = ((uint32_t)recv_buf[CONN_ID_LEN    ] << 24)
                        | ((uint32_t)recv_buf[CONN_ID_LEN + 1] << 16)
                        | ((uint32_t)recv_buf[CONN_ID_LEN + 2] <<  8)
                        |  (uint32_t)recv_buf[CONN_ID_LEN + 3];

        if ((r_tipo == PKT_ACK || r_tipo == PKT_HANDSHAKE_ACK)
            && r_seq == seq_enviado) {
            printf("  ACK recibido para seq=%u\n", seq_enviado);
            ctx->seq++;
            return 0;
        }
    }

    fprintf(stderr, "No se recibió ACK para seq=%u tras %d intentos\n",
            seq_enviado, MAX_RETRIES);
    return -1;
}

static char *creadorMensaje(char e1, char e2) {
    char *msg = malloc(200);
    int x = rand() % 7, y = rand() % 20 + 1, z = rand() % 20 + 1;
    switch (x) {
        case 0: snprintf(msg, 200, "Gol de %c al equipo %c", e1, e2); break;
        case 1: snprintf(msg, 200, "Gol de %c al equipo %c", e2, e1); break;
        case 2: snprintf(msg, 200, "Cambio: jugador %d entra por %d", y, z); break;
        case 3: snprintf(msg, 200, "Tarjeta amarilla para %d del equipo %c", y, e1); break;
        case 4: snprintf(msg, 200, "Tarjeta roja para %d del equipo %c",    y, e1); break;
        case 5: snprintf(msg, 200, "Tarjeta amarilla para %d del equipo %c", y, e2); break;
        case 6: snprintf(msg, 200, "Tarjeta roja para %d del equipo %c",    y, e2); break;
    }
    return msg;
}

int main(void) {
    srand((unsigned int)time(NULL));

    printf("Ingrese los equipos que se enfrentan: ");
    char e1, e2;
    scanf(" %c %c", &e1, &e2);
    int num_mensajes = rand() % 10 + 1;

    PubCtx ctx;
    ctx.seq = 0;
    ctx.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.sock_fd < 0) { perror("socket"); return 1; }

    /* Generar conn_id aleatorio */
    for (int i = 0; i < CONN_ID_LEN; i++)
        ctx.conn_id[i] = rand() & 0xFF;

    memset(&ctx.broker_addr, 0, sizeof(ctx.broker_addr));
    ctx.broker_addr.sin_family      = AF_INET;
    ctx.broker_addr.sin_port        = htons(BROKER_PORT);
    ctx.broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    printf("Realizando handshake QUIC-sim (sin cifrado) con el broker...\n");

    if (enviar_con_ack(&ctx, PKT_HANDSHAKE, "HELLO", 5) != 0) {
        fprintf(stderr, "Handshake fallido\n");
        return 1;
    }
    printf("Handshake completado. Conexión establecida.\n");

    printf("Enviando %d mensajes sobre partido %c%c\n", num_mensajes, e1, e2);

    for (int i = 0; i < num_mensajes; i++) {
        char *evento = creadorMensaje(e1, e2);

        char linea[300];
        snprintf(linea, sizeof(linea), "PUB|%c%c|%s\n", e1, e2, evento);

        if (enviar_con_ack(&ctx, PKT_DATA, linea, strlen(linea)) == 0) {
            printf("Mensaje enviado y confirmado %d (seq=%u): %s", i + 1, ctx.seq - 1, linea);
        } else {
            printf("Mensaje %d perdido definitivamente (seq=%u): %s", i + 1, ctx.seq, linea);
        }

        free(evento);
        usleep(50000);
    }

    uint8_t buf[HEADER_LEN];
    serializar(ctx.conn_id, ctx.seq, PKT_CLOSE, NULL, 0, buf);
    sendto(ctx.sock_fd, buf, HEADER_LEN, 0,
           (struct sockaddr *)&ctx.broker_addr, sizeof(ctx.broker_addr));

    printf("Publisher QUIC-sim finalizado.\n");
    close(ctx.sock_fd);
    return 0;
}

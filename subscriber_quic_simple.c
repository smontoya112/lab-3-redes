/*
 * ============================================================
 * subscriber_quic_simple.c
 * ============================================================
 * QUIC-simulado sin cifrado (sin OpenSSL).
 * Subscriber que se suscribe a partidos y recibe eventos.
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
#define MAX_PARTIDOS  10

typedef struct {
    int                sock_fd;
    uint8_t            conn_id[CONN_ID_LEN];
    struct sockaddr_in broker_addr;
    uint32_t           seq;
    uint32_t           esperado_seq;
    char               read_buf[BUF_SIZE];
    int                buf_len;
} SubCtx;

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

static int deserializar(const uint8_t *buf, int len, 
                        uint8_t *conn_id, uint32_t *seq, uint8_t *tipo,
                        uint8_t *payload, int *payload_len) {
    if (len < HEADER_LEN) return -1;
    int pos = 0;
    memcpy(conn_id, buf + pos, CONN_ID_LEN); pos += CONN_ID_LEN;
    *seq = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16)
         | ((uint32_t)buf[pos+2] << 8) | buf[pos+3]; pos += 4;
    *tipo = buf[pos++];
    *payload_len = len - pos;
    if (*payload_len > 0)
        memcpy(payload, buf + pos, *payload_len);
    return 0;
}

static int enviar_con_ack(SubCtx *ctx, uint8_t tipo,
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

static void procesar_mensaje(SubCtx *ctx, char *texto) {
    /* Extraer partido y evento del formato "PARTIDO|EVENTO" */
    char *sep = strchr(texto, '|');
    if (!sep) {
        printf("[Evento mal formado]: %s\n", texto);
        return;
    }
    *sep = '\0';
    char *partido = texto;
    char *evento = sep + 1;
    
    printf("[EVENTO RECIBIDO] Partido: %s | Evento: %s\n", partido, evento);
}

int main(void) {
    srand((unsigned int)time(NULL));

    printf("=== SUBSCRIBER QUIC-SIMPLE ===\n");
    printf("Ingrese su ID de suscriptor: ");
    int sub_id;
    scanf("%d", &sub_id);

    printf("Ingrese los partidos que desea seguir (ej: AB CD): ");
    char partidos[MAX_PARTIDOS][10];
    int num_partidos = 0;
    
    char linea[100];
    fgetc(stdin); /* Consumir salto de línea */
    fgets(linea, sizeof(linea), stdin);
    
    char *token = strtok(linea, " \n");
    while (token && num_partidos < MAX_PARTIDOS) {
        strcpy(partidos[num_partidos], token);
        num_partidos++;
        token = strtok(NULL, " \n");
    }

    if (num_partidos == 0) {
        fprintf(stderr, "Debe ingresar al menos un partido\n");
        return 1;
    }

    SubCtx ctx;
    ctx.seq = 0;
    ctx.esperado_seq = 1;
    ctx.buf_len = 0;
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

    /* Enviar suscripciones a cada partido */
    for (int i = 0; i < num_partidos; i++) {
        char msg[100];
        snprintf(msg, sizeof(msg), "SUB|%d|%s\n", sub_id, partidos[i]);
        
        if (enviar_con_ack(&ctx, PKT_DATA, msg, strlen(msg)) == 0) {
            printf("Suscripción enviada para: %s (seq=%u)\n", partidos[i], ctx.seq - 1);
        } else {
            printf("ADVERTENCIA: Suscripción perdida para: %s\n", partidos[i]);
        }
    }

    printf("\nEsperando eventos de los partidos suscritos...\n");
    printf("(Presione Ctrl+C para salir)\n\n");

    /* Loop de recepción de eventos */
    fd_set readfds;
    struct timeval tv;
    
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(ctx.sock_fd, &readfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int ret = select(ctx.sock_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select");
            break;
        }
        if (ret == 0) {
            /* Timeout - continuar esperando */
            continue;
        }

        uint8_t recv_buf[BUF_SIZE];
        struct sockaddr_in origen;
        socklen_t olen = sizeof(origen);
        ssize_t n = recvfrom(ctx.sock_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&origen, &olen);
        
        if (n < 0) {
            perror("recvfrom");
            break;
        }
        if (n < (ssize_t)HEADER_LEN) {
            printf("Paquete muy corto (%zd bytes)\n", n);
            continue;
        }

        uint8_t  r_conn_id[CONN_ID_LEN];
        uint32_t r_seq;
        uint8_t  r_tipo;
        uint8_t  r_payload[BUF_SIZE];
        int      r_payload_len;

        if (deserializar(recv_buf, n, r_conn_id, &r_seq, &r_tipo,
                        r_payload, &r_payload_len) != 0) {
            printf("Error desserializando paquete\n");
            continue;
        }

        /* Validar que es del broker */
        if (memcmp(r_conn_id, ctx.conn_id, CONN_ID_LEN) != 0) {
            printf("Paquete de conexión desconocida, ignorando\n");
            continue;
        }

        if (r_tipo == PKT_DATA) {
            /* Validar secuencia */
            if (r_seq < ctx.esperado_seq) {
                printf("[DUPLICADO] seq=%u (esperado %u)\n", r_seq, ctx.esperado_seq);
                /* Enviar ACK igual */
                uint8_t ack_buf[HEADER_LEN];
                serializar(ctx.conn_id, r_seq, PKT_ACK, NULL, 0, ack_buf);
                sendto(ctx.sock_fd, ack_buf, HEADER_LEN, 0,
                       (struct sockaddr *)&ctx.broker_addr, sizeof(ctx.broker_addr));
                continue;
            }
            
            if (r_seq > ctx.esperado_seq) {
                printf("[FUERA DE ORDEN] seq=%u (esperado %u)\n", r_seq, ctx.esperado_seq);
            }
            
            ctx.esperado_seq = r_seq + 1;

            /* Acumular datos en buffer */
            if (ctx.buf_len + r_payload_len < BUF_SIZE - 1) {
                memcpy(ctx.read_buf + ctx.buf_len, r_payload, r_payload_len);
                ctx.buf_len += r_payload_len;
                ctx.read_buf[ctx.buf_len] = '\0';
            }

            /* Enviar ACK inmediatamente */
            uint8_t ack_buf[HEADER_LEN];
            serializar(ctx.conn_id, r_seq, PKT_ACK, NULL, 0, ack_buf);
            sendto(ctx.sock_fd, ack_buf, HEADER_LEN, 0,
                   (struct sockaddr *)&ctx.broker_addr, sizeof(ctx.broker_addr));

            /* Procesar líneas completas */
            char *pos = ctx.read_buf;
            char *fin;
            while ((fin = strchr(pos, '\n')) != NULL) {
                *fin = '\0';
                if (strlen(pos) > 0) {
                    procesar_mensaje(&ctx, pos);
                }
                pos = fin + 1;
            }

            /* Mover datos no procesados al inicio del buffer */
            int restantes = strlen(pos);
            if (restantes > 0) {
                memmove(ctx.read_buf, pos, restantes);
            }
            ctx.buf_len = restantes;
        }
        else if (r_tipo == PKT_ACK || r_tipo == PKT_HANDSHAKE_ACK) {
            /* ACK para nuestro mensaje - ignorar en este contexto */
        }
        else if (r_tipo == PKT_CLOSE) {
            printf("Broker ha cerrado la conexión\n");
            break;
        }
    }

    printf("\nSubscriber QUIC-sim finalizado.\n");
    close(ctx.sock_fd);
    return 0;
}

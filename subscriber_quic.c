#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define BUF_SIZE      4096
#define BROKER_PORT   8080
#define PKT_HANDSHAKE     0x01
#define PKT_HANDSHAKE_ACK 0x02
#define PKT_DATA          0x03
#define PKT_ACK           0x04
#define PKT_CLOSE         0x05
#define CONN_ID_LEN   8
#define IV_LEN        12
#define TAG_LEN       16
#define KEY_LEN       32
#define HEADER_LEN    (CONN_ID_LEN + 4 + 1 + IV_LEN + TAG_LEN)
#define RETRANSMIT_US 500000
#define MAX_RETRIES   5

/* ── Contexto del publisher ── */
typedef struct {
    int                sock_fd;
    uint8_t            conn_id[CONN_ID_LEN];
    uint8_t            key[KEY_LEN];
    struct sockaddr_in broker_addr;
    uint32_t           seq;          
} PubCtx;

/* ── Cifrado/descifrado (igual que broker) ── */
static int cifrar(const uint8_t *key, const uint8_t *iv,
                  const uint8_t *plain, int plen,
                  uint8_t *cipher, uint8_t *tag) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len, clen;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv);
    EVP_EncryptUpdate(ctx, cipher, &len, plain, plen);
    clen = len;
    EVP_EncryptFinal_ex(ctx, cipher + len, &len);
    clen += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag);
    EVP_CIPHER_CTX_free(ctx);
    return clen;
}

/* ── Serialización ── */
static int serializar(const uint8_t *conn_id, uint32_t seq, uint8_t tipo,
                       const uint8_t *iv, const uint8_t *tag,
                       const uint8_t *payload, int plen, uint8_t *buf) {
    int pos = 0;
    memcpy(buf + pos, conn_id, CONN_ID_LEN); pos += CONN_ID_LEN;
    buf[pos++] = (seq >> 24) & 0xFF;
    buf[pos++] = (seq >> 16) & 0xFF;
    buf[pos++] = (seq >>  8) & 0xFF;
    buf[pos++] = (seq      ) & 0xFF;
    buf[pos++] = tipo;
    if (iv)      { memcpy(buf + pos, iv,  IV_LEN);  pos += IV_LEN;  }
    else         { memset(buf + pos, 0,   IV_LEN);  pos += IV_LEN;  }
    if (tag)     { memcpy(buf + pos, tag, TAG_LEN); pos += TAG_LEN; }
    else         { memset(buf + pos, 0,   TAG_LEN); pos += TAG_LEN; }
    if (payload && plen > 0) { memcpy(buf + pos, payload, plen); pos += plen; }
    return pos;
}


static int enviar_con_ack(PubCtx *ctx, uint8_t tipo,
                           const char *payload, int plen) {
    uint8_t iv[IV_LEN], tag[TAG_LEN];
    uint8_t cipher[BUF_SIZE];
    uint8_t buf[BUF_SIZE + HEADER_LEN];
    int buf_len;

    RAND_bytes(iv, IV_LEN);

    if (payload && plen > 0) {
        int clen = cifrar(ctx->key, iv,
                          (const uint8_t *)payload, plen,
                          cipher, tag);
        buf_len = serializar(ctx->conn_id, ctx->seq, tipo,
                              iv, tag, cipher, clen, buf);
    } else {
        buf_len = serializar(ctx->conn_id, ctx->seq, tipo,
                              NULL, NULL, NULL, 0, buf);
    }

    uint32_t seq_enviado = ctx->seq;

    for (int intento = 0; intento < MAX_RETRIES; intento++) {
        /* Enviar paquete */
        sendto(ctx->sock_fd, buf, buf_len, 0,
               (struct sockaddr *)&ctx->broker_addr,
               sizeof(ctx->broker_addr));

        /* Esperar ACK con timeout de 500ms */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = RETRANSMIT_US };

        if (select(ctx->sock_fd + 1, &readfds, NULL, NULL, &tv) <= 0) {
            printf("Timeout esperando ACK seq=%u, reintento %d/%d\n",
                   seq_enviado, intento + 1, MAX_RETRIES);
            continue;  /* Retransmitir */
        }

        /* Recibir respuesta */
        uint8_t recv_buf[HEADER_LEN + 64];
        struct sockaddr_in origen;
        socklen_t olen = sizeof(origen);
        ssize_t n = recvfrom(ctx->sock_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&origen, &olen);
        if (n < (ssize_t)HEADER_LEN) continue;

        /* Extraer tipo y seq del paquete recibido */
        uint8_t  r_tipo = recv_buf[CONN_ID_LEN + 4];
        uint32_t r_seq  = ((uint32_t)recv_buf[CONN_ID_LEN    ] << 24)
                        | ((uint32_t)recv_buf[CONN_ID_LEN + 1] << 16)
                        | ((uint32_t)recv_buf[CONN_ID_LEN + 2] <<  8)
                        |  (uint32_t)recv_buf[CONN_ID_LEN + 3];

        if ((r_tipo == PKT_ACK || r_tipo == PKT_HANDSHAKE_ACK)
            && r_seq == seq_enviado) {
            ctx->seq++;  /* Avanzar número de secuencia */
            return 0;    /* ACK recibido correctamente */
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

    /* ── Inicializar contexto ── */
    PubCtx ctx;
    ctx.seq = 0;
    ctx.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.sock_fd < 0) { perror("socket"); return 1; }

    /* Generar Connection ID y clave AES aleatorios */
    RAND_bytes(ctx.conn_id, CONN_ID_LEN);
    RAND_bytes(ctx.key, KEY_LEN);

    memset(&ctx.broker_addr, 0, sizeof(ctx.broker_addr));
    ctx.broker_addr.sin_family      = AF_INET;
    ctx.broker_addr.sin_port        = htons(BROKER_PORT);
    ctx.broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");


    printf("Realizando handshake QUIC-sim con el broker...\n");
    uint8_t handshake_payload[KEY_LEN + CONN_ID_LEN];
    memcpy(handshake_payload, ctx.key, KEY_LEN);

    if (enviar_con_ack(&ctx, PKT_HANDSHAKE,
                        (char *)handshake_payload, KEY_LEN) != 0) {
        fprintf(stderr, "Handshake fallido\n");
        return 1;
    }
    printf("Handshake completado. Conexión cifrada establecida.\n");

    printf("Enviando %d mensajes sobre partido %c%c\n", num_mensajes, e1, e2);

    /* ── Enviar mensajes cifrados ── */
    for (int i = 0; i < num_mensajes; i++) {
        char *evento = creadorMensaje(e1, e2);

        /* Formato: "PUB|partido|evento\n" */
        char linea[300];
        snprintf(linea, sizeof(linea), "PUB|%c%c|%s\n", e1, e2, evento);

        if (enviar_con_ack(&ctx, PKT_DATA, linea, strlen(linea)) == 0) {
            printf("Mensaje enviado y confirmado %d: %s", i + 1, linea);
        } else {
            printf("Mensaje %d perdido definitivamente: %s", i + 1, linea);
        }

        free(evento);
        usleep(50000);  /* 50ms entre mensajes */
    }

    /* ── Cerrar conexión ── */
    uint8_t buf[HEADER_LEN];
    serializar(ctx.conn_id, ctx.seq, PKT_CLOSE,
               NULL, NULL, NULL, 0, buf);
    sendto(ctx.sock_fd, buf, HEADER_LEN, 0,
           (struct sockaddr *)&ctx.broker_addr, sizeof(ctx.broker_addr));

    printf("Publisher QUIC-sim finalizado.\n");
    close(ctx.sock_fd);
    return 0;
}
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
#define MAX_PARTIDOS  50
#define PARTIDO_LEN   10

typedef struct {
    int                sock_fd;
    uint8_t            conn_id[CONN_ID_LEN];
    uint8_t            key[KEY_LEN];
    struct sockaddr_in broker_addr;
    uint32_t           seq;
    uint32_t           esperado_seq; 
} SubCtx;


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


static int descifrar(const uint8_t *key, const uint8_t *iv,
                     const uint8_t *cipher, int clen,
                     const uint8_t *tag, uint8_t *plain) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len, plen, rv;
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv);
    EVP_DecryptUpdate(ctx, plain, &len, cipher, clen);
    plen = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, (void *)tag);
    rv = EVP_DecryptFinal_ex(ctx, plain + len, &len);
    EVP_CIPHER_CTX_free(ctx);
    if (rv <= 0) return -1;
    return plen + len;
}


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
    if (iv)  { memcpy(buf+pos, iv,  IV_LEN);  pos += IV_LEN;  }
    else     { memset(buf+pos, 0,   IV_LEN);  pos += IV_LEN;  }
    if (tag) { memcpy(buf+pos, tag, TAG_LEN); pos += TAG_LEN; }
    else     { memset(buf+pos, 0,   TAG_LEN); pos += TAG_LEN; }
    if (payload && plen > 0) { memcpy(buf+pos, payload, plen); pos += plen; }
    return pos;
}

static int enviar_con_ack(SubCtx *ctx, uint8_t tipo,
                           const char *payload, int plen) {
    uint8_t iv[IV_LEN], tag[TAG_LEN], cipher[BUF_SIZE];
    uint8_t buf[BUF_SIZE + HEADER_LEN];
    int buf_len;

    if (tipo == PKT_HANDSHAKE) {
       
        buf_len = serializar(ctx->conn_id, ctx->seq, tipo,
                              NULL, NULL,
                              (const uint8_t *)payload, plen, buf);
    } else if (payload && plen > 0) {
        RAND_bytes(iv, IV_LEN);
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
        sendto(ctx->sock_fd, buf, buf_len, 0,
               (struct sockaddr *)&ctx->broker_addr,
               sizeof(ctx->broker_addr));

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = RETRANSMIT_US };

        if (select(ctx->sock_fd + 1, &readfds, NULL, NULL, &tv) <= 0) {
            printf("Timeout ACK seq=%u, reintento %d/%d\n",
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
            ctx->seq++;
            return 0;
        }
    }
    return -1;
}


static void enviar_ack_simple(SubCtx *ctx, uint32_t seq) {
    uint8_t buf[HEADER_LEN];
    serializar(ctx->conn_id, seq, PKT_ACK,
               NULL, NULL, NULL, 0, buf);
    sendto(ctx->sock_fd, buf, HEADER_LEN, 0,
           (struct sockaddr *)&ctx->broker_addr,
           sizeof(ctx->broker_addr));
}

int main(void) {
    srand((unsigned int)time(NULL));

    printf("Ingrese su id de suscriptor: ");
    int id;
    scanf("%d", &id);

    printf("Ingrese el numero de partidos a los que se va a suscribir: ");
    int num_partidos;
    scanf("%d", &num_partidos);
    if (num_partidos > MAX_PARTIDOS) num_partidos = MAX_PARTIDOS;

    char partidos[MAX_PARTIDOS][PARTIDO_LEN];
    getchar();
    printf("Ingrese los partidos (ej: AB):\n");
    for (int i = 0; i < num_partidos; i++) {
        fgets(partidos[i], PARTIDO_LEN, stdin);
        partidos[i][strcspn(partidos[i], "\n")] = '\0';
        printf("Suscrito al partido: %s\n", partidos[i]);
    }


    SubCtx ctx;
    ctx.seq          = 0;
    ctx.esperado_seq = 1;

    ctx.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.sock_fd < 0) { perror("socket"); return 1; }

    RAND_bytes(ctx.conn_id, CONN_ID_LEN);
    RAND_bytes(ctx.key, KEY_LEN);

    memset(&ctx.broker_addr, 0, sizeof(ctx.broker_addr));
    ctx.broker_addr.sin_family      = AF_INET;
    ctx.broker_addr.sin_port        = htons(BROKER_PORT);
    ctx.broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");


    printf("Realizando handshake QUIC-sim...\n");
    if (enviar_con_ack(&ctx, PKT_HANDSHAKE,
                        (char *)ctx.key, KEY_LEN) != 0) {
        fprintf(stderr, "Handshake fallido\n");
        return 1;
    }
    printf("Handshake completado. Conexión cifrada.\n");

    printf("Enviando suscripciones...\n");
    for (int i = 0; i < num_partidos; i++) {
        char linea[100];
        snprintf(linea, sizeof(linea), "SUB|%d|%s\n", id, partidos[i]);
        if (enviar_con_ack(&ctx, PKT_DATA, linea, strlen(linea)) == 0) {
            printf("Suscripcion confirmada: %s", linea);
        }
    }


    printf("\nEsperando eventos [QUIC-sim cifrado con AES-256-GCM]...\n");

    uint8_t recv_buf[BUF_SIZE + HEADER_LEN];
    char    acum_buf[BUF_SIZE];
    int     acum_len = 0;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx.sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        if (select(ctx.sock_fd + 1, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        struct sockaddr_in origen;
        socklen_t olen = sizeof(origen);
        ssize_t n = recvfrom(ctx.sock_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&origen, &olen);
        if (n < (ssize_t)HEADER_LEN) continue;
        uint32_t r_seq  = ((uint32_t)recv_buf[CONN_ID_LEN    ] << 24)
                        | ((uint32_t)recv_buf[CONN_ID_LEN + 1] << 16)
                        | ((uint32_t)recv_buf[CONN_ID_LEN + 2] <<  8)
                        |  (uint32_t)recv_buf[CONN_ID_LEN + 3];
        uint8_t  r_tipo = recv_buf[CONN_ID_LEN + 4];
        uint8_t *r_iv   = recv_buf + CONN_ID_LEN + 4 + 1;
        uint8_t *r_tag  = r_iv + IV_LEN;
        uint8_t *r_pay  = r_tag + TAG_LEN;
        int      r_plen = (int)n - HEADER_LEN;

        if (r_tipo != PKT_DATA) continue;
        enviar_ack_simple(&ctx, r_seq);

        if (r_seq < ctx.esperado_seq) {
            printf("[Duplicado descartado seq=%u]\n", r_seq);
            continue;
        }
        if (r_seq > ctx.esperado_seq) {
            printf("[Advertencia: paquete fuera de orden seq=%u esperado=%u]\n",
                   r_seq, ctx.esperado_seq);
        }
        ctx.esperado_seq = r_seq + 1;

        uint8_t plain[BUF_SIZE];
        int plain_len = descifrar(ctx.key, r_iv, r_pay, r_plen, r_tag, plain);
        if (plain_len < 0) {
            fprintf(stderr, "[Error: autenticación AES-GCM fallida]\n");
            continue;
        }
        plain[plain_len] = '\0';

        if (acum_len + plain_len < BUF_SIZE - 1) {
            memcpy(acum_buf + acum_len, plain, plain_len);
            acum_len += plain_len;
            acum_buf[acum_len] = '\0';
        }

        char *pos = acum_buf, *fin;
        while ((fin = strchr(pos, '\n')) != NULL) {
            *fin = '\0';
            if (strlen(pos) > 0)
                printf("Evento recibido [AES-GCM]: %s\n", pos);
            pos = fin + 1;
        }
        int resto = acum_len - (pos - acum_buf);
        if (resto > 0) memmove(acum_buf, pos, resto);
        acum_len = resto;
    }

    close(ctx.sock_fd);
    return 0;
}
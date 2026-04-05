#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#define BUF_SIZE     4096
#define BROKER_PORT  8080
#define MAX_PARTIDOS 50
#define PARTIDO_LEN  10

struct SubContext {
    ngtcp2_conn *conn;
    SSL         *ssl;
    SSL_CTX     *ssl_ctx;
    int          sock_fd;
    int64_t      stream_id;
    int          handshake_ok;
    int          id;
    char         partidos[MAX_PARTIDOS][PARTIDO_LEN];
    int          num_partidos;
    char         read_buf[BUF_SIZE];
    int          buf_len;
};


static ngtcp2_tstamp timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void generar_cid(ngtcp2_cid *cid) {
    cid->datalen = 8;
    for (size_t i = 0; i < cid->datalen; i++)
        cid->data[i] = (uint8_t)(rand() & 0xFF);
}



static void quic_send_pending(struct SubContext *ctx,
                               struct sockaddr_in *broker_addr) {
    uint8_t buf[BUF_SIZE + 256];
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;

    for (;;) {
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_ssize n = ngtcp2_conn_write_pkt(
            ctx->conn, &ps.path, &pi, buf, sizeof(buf), timestamp_ns()
        );
        if (n <= 0) break;
        sendto(ctx->sock_fd, buf, n, 0,
               (struct sockaddr *)broker_addr, sizeof(*broker_addr));
    }
}


static void procesar_datos_recibidos(struct SubContext *ctx,
                                      const uint8_t *data, size_t datalen) {
    if (ctx->buf_len + (int)datalen >= BUF_SIZE - 1) return;

    memcpy(ctx->read_buf + ctx->buf_len, data, datalen);
    ctx->buf_len += datalen;
    ctx->read_buf[ctx->buf_len] = '\0';

    char *pos = ctx->read_buf;
    char *fin;
    while ((fin = strchr(pos, '\n')) != NULL) {
        *fin = '\0';
        if (strlen(pos) > 0) {
            printf("Evento recibido [QUIC]: %s\n", pos);
        }
        pos = fin + 1;
    }

    int resto = ctx->buf_len - (pos - ctx->read_buf);
    if (resto > 0) memmove(ctx->read_buf, pos, resto);
    ctx->buf_len = resto;
}


static int init_tls_cliente(struct SubContext *ctx) {
    ctx->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx->ssl_ctx) return -1;

    if (ngtcp2_crypto_ossl_configure_client_context(ctx->ssl_ctx) != 0)
        return -1;

    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);

    ctx->ssl = SSL_new(ctx->ssl_ctx);
    if (!ctx->ssl) return -1;

    SSL_set_tlsext_host_name(ctx->ssl, "localhost");

    static const uint8_t alpn[] = "\x06pubsub";
    SSL_set_alpn_protos(ctx->ssl, alpn, sizeof(alpn) - 1);

    return 0;
}


static int realizar_handshake(struct SubContext *ctx,
                               struct sockaddr_in *broker_addr) {
    uint8_t recv_buf[BUF_SIZE];
    int intentos = 0;

    printf("Realizando handshake QUIC+TLS con el broker...\n");

    while (!ngtcp2_conn_get_handshake_completed(ctx->conn)) {
        quic_send_pending(ctx, broker_addr);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx->sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };

        int ready = select(ctx->sock_fd + 1, &readfds, NULL, NULL, &tv);
        if (ready <= 0) {
            if (++intentos > 10) {
                fprintf(stderr, "Timeout en handshake QUIC\n");
                return -1;
            }
            continue;
        }

        struct sockaddr_in origen;
        socklen_t origen_len = sizeof(origen);
        ssize_t n = recvfrom(ctx->sock_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&origen, &origen_len);
        if (n < 0) { perror("recvfrom"); return -1; }

        struct sockaddr_in local_addr;
        socklen_t local_len = sizeof(local_addr);
        getsockname(ctx->sock_fd, (struct sockaddr *)&local_addr, &local_len);

        ngtcp2_path path = {
            .local  = { .addr = (struct sockaddr *)&local_addr,
                        .addrlen = local_len },
            .remote = { .addr = (struct sockaddr *)broker_addr,
                        .addrlen = sizeof(*broker_addr) }
        };

        int rv = ngtcp2_conn_read_pkt(ctx->conn, &path, NULL,
                                       recv_buf, n, timestamp_ns());
        if (rv != 0 && rv != NGTCP2_ERR_RETRY) {
            fprintf(stderr, "Error en handshake: %s\n", ngtcp2_strerror(rv));
            return -1;
        }

        intentos = 0;
    }

    printf("Handshake QUIC completado. Conexión segura establecida.\n");
    ctx->handshake_ok = 1;
    return 0;
}


static void enviar_suscripciones(struct SubContext *ctx,
                                  struct sockaddr_in *broker_addr) {
    for (int i = 0; i < ctx->num_partidos; i++) {
        char linea[100];
        snprintf(linea, sizeof(linea), "SUB|%d|%s\n",
                 ctx->id, ctx->partidos[i]);

        ngtcp2_vec datavec = {
            .base = (uint8_t *)linea,
            .len  = strlen(linea)
        };

        uint8_t send_buf[BUF_SIZE + 256];
        ngtcp2_path_storage ps;
        ngtcp2_pkt_info pi;
        ngtcp2_path_storage_zero(&ps);

        ngtcp2_ssize nwrite = ngtcp2_conn_write_stream(
            ctx->conn, &ps.path, &pi,
            send_buf, sizeof(send_buf),
            NULL, 0,
            ctx->stream_id,
            &datavec, 1,
            timestamp_ns()
        );

        if (nwrite > 0) {
            sendto(ctx->sock_fd, send_buf, nwrite, 0,
                   (struct sockaddr *)broker_addr, sizeof(*broker_addr));
        }

        printf("Suscripcion enviada [QUIC]: %s", linea);
    }
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

    struct SubContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.id           = id;
    ctx.stream_id    = -1;
    ctx.handshake_ok = 0;
    ctx.num_partidos = num_partidos;

    getchar();  
    printf("Ingrese los partidos (ej: AB):\n");
    for (int i = 0; i < num_partidos; i++) {
        char buf[PARTIDO_LEN];
        fgets(buf, sizeof(buf), stdin);
        buf[strcspn(buf, "\n")] = '\0';
        strncpy(ctx.partidos[i], buf, PARTIDO_LEN - 1);
        printf("Suscrito al partido: %s\n", buf);
    }

    if (init_tls_cliente(&ctx) != 0) {
        fprintf(stderr, "Error inicializando TLS\n");
        return 1;
    }

    ctx.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.sock_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family      = AF_INET;
    broker_addr.sin_port        = htons(BROKER_PORT);
    broker_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(ctx.sock_fd, (struct sockaddr *)&broker_addr,
                sizeof(broker_addr)) < 0) {
        perror("connect UDP"); return 1;
    }

    ngtcp2_cid scid, dcid;
    generar_cid(&scid);
    generar_cid(&dcid);

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp_ns();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi            = 10;
    params.initial_max_stream_data_bidi_local  = BUF_SIZE;
    params.initial_max_stream_data_bidi_remote = BUF_SIZE;
    params.initial_max_data                    = BUF_SIZE * 10;

    ngtcp2_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    ngtcp2_crypto_ossl_install_client_session_callbacks(&callbacks);

    callbacks.recv_stream_data = [](
        ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
        uint64_t offset, const uint8_t *data, size_t datalen,
        void *user_data, void *stream_user_data) -> int
    {
        struct SubContext *c = (struct SubContext *)user_data;
        procesar_datos_recibidos(c, data, datalen);
        return 0;
    };

    callbacks.handshake_completed = [](ngtcp2_conn *conn, void *user_data) -> int {
        printf("[QUIC] Handshake completado\n");
        return 0;
    };

    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    getsockname(ctx.sock_fd, (struct sockaddr *)&local_addr, &local_len);

    ngtcp2_path path = {
        .local  = { .addr = (struct sockaddr *)&local_addr,
                    .addrlen = local_len },
        .remote = { .addr = (struct sockaddr *)&broker_addr,
                    .addrlen = sizeof(broker_addr) }
    };

    int rv = ngtcp2_conn_client_new(
        &ctx.conn, &dcid, &scid, &path,
        NGTCP2_PROTO_VER_V1,
        &callbacks, &settings, &params,
        NULL, &ctx
    );
    if (rv != 0) {
        fprintf(stderr, "Error creando conexión QUIC: %s\n", ngtcp2_strerror(rv));
        return 1;
    }

    ngtcp2_conn_set_tls_native_handle(ctx.conn, ctx.ssl);

    if (realizar_handshake(&ctx, &broker_addr) != 0) return 1;

    rv = ngtcp2_conn_open_bidi_stream(ctx.conn, &ctx.stream_id, NULL);
    if (rv != 0) {
        fprintf(stderr, "Error abriendo stream: %s\n", ngtcp2_strerror(rv));
        return 1;
    }

    printf("Conectado al broker QUIC. Enviando suscripciones...\n");
    enviar_suscripciones(&ctx, &broker_addr);

    printf("\nEsperando eventos de los partidos suscritos [QUIC]...\n");

    uint8_t recv_buf[BUF_SIZE];

    while (1) {
        quic_send_pending(&ctx, &broker_addr);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx.sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; 

        int ready = select(ctx.sock_fd + 1, &readfds, NULL, NULL, &tv);
        if (ready <= 0) {
            ngtcp2_tstamp now    = timestamp_ns();
            ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(ctx.conn);
            if (expiry <= now) {
                ngtcp2_conn_handle_expiry(ctx.conn, now);
            }
            continue;
        }

        struct sockaddr_in origen;
        socklen_t origen_len = sizeof(origen);
        ssize_t n = recvfrom(ctx.sock_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&origen, &origen_len);
        if (n < 0) { perror("recvfrom"); break; }

        ngtcp2_path recv_path = {
            .local  = { .addr = (struct sockaddr *)&local_addr,
                        .addrlen = local_len },
            .remote = { .addr = (struct sockaddr *)&broker_addr,
                        .addrlen = sizeof(broker_addr) }
        };

        rv = ngtcp2_conn_read_pkt(ctx.conn, &recv_path, NULL,
                                   recv_buf, n, timestamp_ns());
        if (rv != 0) {
            if (rv == NGTCP2_ERR_DRAINING) {
                printf("Broker cerró la conexión QUIC.\n");
                break;
            }
            fprintf(stderr, "Error recibiendo paquete QUIC: %s\n",
                    ngtcp2_strerror(rv));
            break;
        }
    }

    ngtcp2_conn_del(ctx.conn);
    SSL_free(ctx.ssl);
    SSL_CTX_free(ctx.ssl_ctx);
    close(ctx.sock_fd);
    return 0;
}

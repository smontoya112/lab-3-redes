#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define MAX_CLIENTES    40
#define MAX_PARTIDOS    50
#define PARTIDO_LEN     10
#define BUF_SIZE        4096
#define BROKER_PORT     8080

#define PKT_HANDSHAKE     0x01
#define PKT_HANDSHAKE_ACK 0x02
#define PKT_DATA          0x03
#define PKT_ACK           0x04
#define PKT_CLOSE         0x05

#define CONN_ID_LEN   8
#define SEQ_LEN       4
#define TYPE_LEN      1
#define IV_LEN        12   
#define TAG_LEN       16   
#define KEY_LEN       32   


#define HEADER_LEN  (CONN_ID_LEN + SEQ_LEN + TYPE_LEN + IV_LEN + TAG_LEN)

#define RETRANSMIT_US  500000   
#define MAX_RETRIES    5


typedef struct {
    uint8_t  conn_id[CONN_ID_LEN]; 
    uint32_t seq;                   
    uint8_t  tipo;                 
    uint8_t  iv[IV_LEN];        
    uint8_t  tag[TAG_LEN];        
    uint8_t  payload[BUF_SIZE];  
    int      payload_len;           
} Paquete;


typedef struct {
    uint8_t            conn_id[CONN_ID_LEN];
    struct sockaddr_in addr;
    uint8_t            key[KEY_LEN];
    int                handshake_ok;
    int                tipo;          /* 1=subscriber, 2=publisher */
    int                id;
    char               partidos[MAX_PARTIDOS][PARTIDO_LEN];
    int                num_partidos;
    int                activo;
    uint32_t           esperado_seq;
    char               read_buf[BUF_SIZE];
    int                buf_len;
} Cliente;

static int     sock_fd;
static Cliente clientes[MAX_CLIENTES];


static int cifrar(const uint8_t *key, const uint8_t *iv,
                  const uint8_t *plain, int plain_len,
                  uint8_t *cipher, uint8_t *tag) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len, cipher_len;

    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv);
    EVP_EncryptUpdate(ctx, cipher, &len, plain, plain_len);
    cipher_len = len;
    EVP_EncryptFinal_ex(ctx, cipher + len, &len);
    cipher_len += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag);
    EVP_CIPHER_CTX_free(ctx);
    return cipher_len;
}


static int descifrar(const uint8_t *key, const uint8_t *iv,
                     const uint8_t *cipher, int cipher_len,
                     const uint8_t *tag, uint8_t *plain) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len, plain_len, rv;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv);
    EVP_DecryptUpdate(ctx, plain, &len, cipher, cipher_len);
    plain_len = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, (void *)tag);
    rv = EVP_DecryptFinal_ex(ctx, plain + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (rv <= 0) return -1; 
    plain_len += len;
    return plain_len;
}


static int serializar(const Paquete *pkt, uint8_t *buf) {
    int pos = 0;
    memcpy(buf + pos, pkt->conn_id, CONN_ID_LEN); pos += CONN_ID_LEN;
   
    buf[pos++] = (pkt->seq >> 24) & 0xFF;
    buf[pos++] = (pkt->seq >> 16) & 0xFF;
    buf[pos++] = (pkt->seq >>  8) & 0xFF;
    buf[pos++] = (pkt->seq      ) & 0xFF;
    buf[pos++] = pkt->tipo;
    memcpy(buf + pos, pkt->iv,      IV_LEN);  pos += IV_LEN;
    memcpy(buf + pos, pkt->tag,     TAG_LEN); pos += TAG_LEN;
    memcpy(buf + pos, pkt->payload, pkt->payload_len); pos += pkt->payload_len;
    return pos;
}


static int deserializar(const uint8_t *buf, int len, Paquete *pkt) {
    if (len < HEADER_LEN) return -1;
    int pos = 0;
    memcpy(pkt->conn_id, buf + pos, CONN_ID_LEN); pos += CONN_ID_LEN;
    pkt->seq  = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16)
              | ((uint32_t)buf[pos+2] << 8) | buf[pos+3]; pos += 4;
    pkt->tipo = buf[pos++];
    memcpy(pkt->iv,  buf + pos, IV_LEN);  pos += IV_LEN;
    memcpy(pkt->tag, buf + pos, TAG_LEN); pos += TAG_LEN;
    pkt->payload_len = len - pos;
    if (pkt->payload_len > 0)
        memcpy(pkt->payload, buf + pos, pkt->payload_len);
    return 0;
}


static void enviar_paquete(Cliente *c, uint8_t tipo, uint32_t seq,
                            const char *payload, int payload_len) {
    Paquete pkt;
    memcpy(pkt.conn_id, c->conn_id, CONN_ID_LEN);
    pkt.seq  = seq;
    pkt.tipo = tipo;

    RAND_bytes(pkt.iv, IV_LEN);

    if (payload && payload_len > 0) {
        pkt.payload_len = cifrar(c->key, pkt.iv,
                                  (const uint8_t *)payload, payload_len,
                                  pkt.payload, pkt.tag);
    } else {
        pkt.payload_len = 0;
        memset(pkt.tag, 0, TAG_LEN);
    }

    uint8_t buf[BUF_SIZE + HEADER_LEN];
    int len = serializar(&pkt, buf);
    sendto(sock_fd, buf, len, 0,
           (struct sockaddr *)&c->addr, sizeof(c->addr));
}


static void enviar_ack(struct sockaddr_in *addr,
                        uint8_t *conn_id, uint32_t seq) {
    Paquete pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.conn_id, conn_id, CONN_ID_LEN);
    pkt.seq         = seq;
    pkt.tipo        = PKT_ACK;
    pkt.payload_len = 0;

    uint8_t buf[HEADER_LEN];
    int len = serializar(&pkt, buf);
    sendto(sock_fd, buf, len, 0,
           (struct sockaddr *)addr, sizeof(*addr));
}


static int buscar_cliente_por_conn_id(const uint8_t *conn_id) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].activo &&
            memcmp(clientes[i].conn_id, conn_id, CONN_ID_LEN) == 0)
            return i;
    }
    return -1;
}


static void reenviar_a_subscribers(const char *partido, const char *mensaje) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!clientes[i].activo)        continue;
        if (clientes[i].tipo != 1)      continue;  
        if (!clientes[i].handshake_ok)  continue;

        for (int j = 0; j < clientes[i].num_partidos; j++) {
            if (strcmp(clientes[i].partidos[j], partido) == 0) {
             
                enviar_paquete(&clientes[i], PKT_DATA, 0,
                               mensaje, strlen(mensaje));
                break;
            }
        }
    }
}

static void procesar_mensaje(Cliente *c, char *texto) {
    if (strncmp(texto, "SUB|", 4) == 0) {
        c->tipo = 1;
        char *ptr = texto + 4;
        char *sep = strchr(ptr, '|');
        if (!sep) return;
        *sep = '\0';
        c->id = atoi(ptr);
        char *partido = sep + 1;
        if (c->num_partidos < MAX_PARTIDOS) {
            strncpy(c->partidos[c->num_partidos], partido, PARTIDO_LEN - 1);
            c->num_partidos++;
            printf("Subscriber %d suscrito a: %s\n", c->id, partido);
        }
    } else if (strncmp(texto, "PUB|", 4) == 0) {
        c->tipo = 2;
        char *ptr = texto + 4;
        char *sep = strchr(ptr, '|');
        if (!sep) return;
        *sep = '\0';
        char *partido = ptr;
        char *evento  = sep + 1;
        char msg[BUF_SIZE];
        snprintf(msg, BUF_SIZE, "%s|%s\n", partido, evento);
        printf("Broker reenviando [QUIC-sim]: %s", msg);
        reenviar_a_subscribers(partido, msg);
    }
}


static void procesar_handshake(struct sockaddr_in *addr, Paquete *pkt) {
    
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!clientes[i].activo) { slot = i; break; }
    }
    if (slot < 0) {
        fprintf(stderr, "Sin slots para nueva conexión\n");
        return;
    }

    Cliente *c = &clientes[slot];
    memset(c, 0, sizeof(*c));
    c->activo       = 1;
    c->addr         = *addr;
    c->id           = -1;
    c->handshake_ok = 0;
    c->esperado_seq = 1;
    memcpy(c->conn_id, pkt->conn_id, CONN_ID_LEN);

    if (pkt->payload_len >= KEY_LEN) {
        memcpy(c->key, pkt->payload, KEY_LEN);
    } else {
      
        RAND_bytes(c->key, KEY_LEN);
    }

    c->handshake_ok = 1;
    printf("Nueva sesión QUIC-sim desde puerto %d (slot %d)\n",
           ntohs(addr->sin_port), slot);


    enviar_paquete(c, PKT_HANDSHAKE_ACK, 0, "OK", 2);
}


static void procesar_paquete_data(Cliente *c, Paquete *pkt) {

    enviar_ack(&c->addr, c->conn_id, pkt->seq);

  
    if (pkt->seq < c->esperado_seq) {
        printf("Paquete duplicado seq=%u, ignorando\n", pkt->seq);
        return;
    }
    if (pkt->seq > c->esperado_seq) {
        printf("Paquete fuera de orden seq=%u (esperado %u)\n",
               pkt->seq, c->esperado_seq);
    
    }
    c->esperado_seq = pkt->seq + 1;


    uint8_t plain[BUF_SIZE];
    int plain_len = descifrar(c->key, pkt->iv,
                               pkt->payload, pkt->payload_len,
                               pkt->tag, plain);
    if (plain_len < 0) {
        fprintf(stderr, "Error de autenticación AES-GCM (paquete alterado)\n");
        return;
    }
    plain[plain_len] = '\0';

    if (c->buf_len + plain_len < BUF_SIZE - 1) {
        memcpy(c->read_buf + c->buf_len, plain, plain_len);
        c->buf_len += plain_len;
        c->read_buf[c->buf_len] = '\0';
    }

    char *pos = c->read_buf;
    char *fin;
    while ((fin = strchr(pos, '\n')) != NULL) {
        *fin = '\0';
        if (strlen(pos) > 0) procesar_mensaje(c, pos);
        pos = fin + 1;
    }
    int resto = c->buf_len - (pos - c->read_buf);
    if (resto > 0) memmove(c->read_buf, pos, resto);
    c->buf_len = resto;
}


int main(void) {
    srand((unsigned int)time(NULL));
    memset(clientes, 0, sizeof(clientes));

  
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(BROKER_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    printf("Broker QUIC-simulado escuchando en 127.0.0.1:%d\n", BROKER_PORT);
    printf("Cifrado: AES-256-GCM | Confiabilidad: ACK+retransmisión | Orden: seq numbers\n\n");

    uint8_t buf[BUF_SIZE + HEADER_LEN];

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock_fd, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };

        if (select(sock_fd + 1, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        struct sockaddr_in origen;
        socklen_t origen_len = sizeof(origen);
        ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&origen, &origen_len);
        if (n < 0) continue;

        Paquete pkt;
        if (deserializar(buf, (int)n, &pkt) < 0) continue;

        int slot = buscar_cliente_por_conn_id(pkt.conn_id);

        switch (pkt.tipo) {
            case PKT_HANDSHAKE:
                if (slot < 0) {
                
                    procesar_handshake(&origen, &pkt);
                } else {
                   
                    enviar_paquete(&clientes[slot], PKT_HANDSHAKE_ACK, 0, "OK", 2);
                }
                break;

            case PKT_DATA:
                if (slot >= 0 && clientes[slot].handshake_ok) {
                    procesar_paquete_data(&clientes[slot], &pkt);
                }
                break;

            case PKT_ACK:
    
                if (slot >= 0)
                    printf("ACK recibido de cliente %d (seq=%u)\n",
                           clientes[slot].id, pkt.seq);
                break;

            case PKT_CLOSE:
                if (slot >= 0) {
                    printf("Cliente %d cerró la conexión QUIC-sim\n",
                           clientes[slot].id);
                    clientes[slot].activo = 0;
                }
                break;

            default:
                fprintf(stderr, "Tipo de paquete desconocido: 0x%02X\n", pkt.tipo);
        }
    }

    close(sock_fd);
    return 0;
}
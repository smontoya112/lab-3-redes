#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#define MAX_CLIENTES   40          
#define MAX_PARTIDOS   50          
#define PARTIDO_LEN    10        
#define BUF_SIZE       4096   
#define BROKER_PORT    8080       
#define CERT_FILE      "broker.crt"  
#define KEY_FILE       "broker.key"  


typedef enum {
    TIPO_DESCONOCIDO = 0,
    TIPO_SUBSCRIBER,
    TIPO_PUBLISHER
} TipoCliente;


struct Cliente {
    ngtcp2_conn        *conn;
    SSL                *ssl;
    struct sockaddr_in  addr;
    int64_t             stream_id;
    TipoCliente         tipo;
    int                 id;
    char                partidos[MAX_PARTIDOS][PARTIDO_LEN];
    int                 num_partidos;
    int                 activo;
    char                read_buf[BUF_SIZE];
    int                 buf_len;
};

static int            sock_fd;            
static SSL_CTX       *ssl_ctx;         
static struct Cliente clientes[MAX_CLIENTES]; 
static int            num_clientes = 0;

static int  init_tls_server(void);
static int  init_socket(void);
static void generar_connection_id(ngtcp2_cid *cid);
static int  nueva_conexion(struct sockaddr_in *addr, uint8_t *pkt, size_t pktlen);
static void procesar_stream_data(struct Cliente *c, uint8_t *data, size_t datalen);
static void procesar_linea(struct Cliente *c, char *linea);
static void reenviar_a_subscribers(char *partido, char *mensaje);
static void enviar_quic(struct Cliente *c, const char *mensaje);
static ngtcp2_tstamp timestamp_ns(void);


static ngtcp2_tstamp timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}




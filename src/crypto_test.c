/*
 * param_sniffer.c
 *
 *  Created on: Aug 12, 2020
 *      Author: joris
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <csp/csp.h>
#include <csp/arch/csp_time.h>

/* Using un-exported header file.
 * This is allowed since we are still in libcsp */
#include <csp/arch/csp_thread.h>

#include "tweetnacl.h"

/** Example defines */
#define CSP_DECRYPTOR_PORT  20   // Address of local CSP node
#define TEST_TIMEOUT 1000

#define CRYPTO_REMOTE_KEY_COUNT 8

// Future Params
unsigned char crypto_key_public[crypto_box_PUBLICKEYBYTES] = {0};
unsigned char crypto_key_secret[crypto_box_SECRETKEYBYTES] = {0};

unsigned char crypto_remote_key[CRYPTO_REMOTE_KEY_COUNT][crypto_box_PUBLICKEYBYTES] = {
    {0}
};
unsigned char crypto_remote_beforem[CRYPTO_REMOTE_KEY_COUNT][crypto_box_BEFORENMBYTES] = {
    {0}
};
unsigned int crypto_remote_counter[CRYPTO_REMOTE_KEY_COUNT] = {0};
unsigned int crypto_remote_privilege[CRYPTO_REMOTE_KEY_COUNT] = {0};
unsigned char crypto_remote_active = 0;

unsigned int crypto_fail_auth_count = 0;
unsigned int crypto_fail_nonce_count = 0;

// Test
unsigned char crypto_key_test_public[crypto_box_PUBLICKEYBYTES] = {0};
unsigned char crypto_key_test_secret[crypto_box_SECRETKEYBYTES] = {0};
unsigned char crypto_key_test_beforem[crypto_box_BEFORENMBYTES] = {0};


// ------------------------
// Temporary Debug functions
// ------------------------
void crypto_test_print_hex(char * text, unsigned char * data, int length) {
    printf("%-25s: ", text);
    for(int i = 0; i < length; i++) {
        printf("%02hhX, ", data[i]);
    }
    printf("\n");
}
#define CRYPTO_TEST_PRINT_HEX(EXP) crypto_test_print_hex(#EXP, EXP, sizeof(EXP))

void * debug_csp_buffer_get(size_t size) {
    printf("csp_buffer_get(%d)\n", size);
    return csp_buffer_get(size);
}
#define csp_buffer_get debug_csp_buffer_get

void crypto_test_generate_keys() {
    int result;

    if(crypto_key_public[0] == 0) {
        printf("Run crypto_box_keypair\n");
        result = crypto_box_keypair(crypto_key_public, crypto_key_secret);
        if(result != 0) {
            printf("ERROR\n");
        }
    }
    else {
        printf("Using hardcoded key");
    }
    CRYPTO_TEST_PRINT_HEX(crypto_key_public);
    CRYPTO_TEST_PRINT_HEX(crypto_key_secret);

    if(crypto_key_test_public[0] == 0) {
        printf("Run crypto_box_keypair\n");
        result = crypto_box_keypair(crypto_key_test_public, crypto_key_test_secret);
        if(result != 0) {
            printf("ERROR\n");
        }
    }
    else {
        printf("Using hardcoded key");
    }
    CRYPTO_TEST_PRINT_HEX(crypto_key_test_public);
    CRYPTO_TEST_PRINT_HEX(crypto_key_test_secret);

    printf("Run crypto_box_beforenm\n");
    result = crypto_box_beforenm(crypto_key_test_beforem, crypto_key_public, crypto_key_test_secret);
    if(result != 0) {
        printf("ERROR\n");
    }

    // Assign Test key to Slot 0
    memcpy(crypto_remote_key[0], crypto_key_test_public, sizeof(crypto_key_test_public));
}

// ------------------------
// Crypto Helper Functions
// ------------------------
void randombytes(unsigned char * a, unsigned long long c) {
    // Note: Pseudo random since we are not initializing random!
    while(c > 0) {
        *a = rand() & 0xFF;
        a++;
        c--;
    }
}

void crypto_test_make_nonce(uint8_t * nonce, int counter) {
    memset(nonce, 0, crypto_box_NONCEBYTES);
    nonce[0] = counter;
    nonce[1] = 10;
    nonce[2] = 100;
}

//#define crypto_secretbox_xsalsa20poly1305_tweet_ZEROBYTES 32
//#define crypto_secretbox_xsalsa20poly1305_tweet_BOXZEROBYTES 16
/*
There is a 32-octet padding requirement on the plaintext buffer that you pass to crypto_box.
Internally, the NaCl implementation uses this space to avoid having to allocate memory or
use static memory that might involve a cache hit (see Bernstein's paper on cache timing
side-channel attacks for the juicy details).

Similarly, the crypto_box_open call requires 16 octets of zero padding before the start
of the actual ciphertext. This is used in a similar fashion. These padding octets are not
part of either the plaintext or the ciphertext, so if you are sending ciphertext across the
network, don't forget to remove them!
*/

csp_packet_t * crypto_test_csp_encrypt_packet(uint8_t * data, unsigned int size, uint8_t * key_beforem, int counter) {

    int result = 1;

    csp_packet_t * packet = NULL;
    csp_packet_t * buffer = NULL;

    printf("crypto_test_csp_encrypted_packet %d\n", size);

    // Allocate additional buffer to pre-pad input data
    buffer = csp_buffer_get(size + crypto_secretbox_ZEROBYTES);
    if (buffer == NULL)
        goto out;

    // Copy input data to temporary buffer prepended with zeros
    memset(buffer->data, 0, crypto_secretbox_ZEROBYTES);
    memcpy(buffer->data + crypto_secretbox_ZEROBYTES, data, size);

    unsigned char nonce[crypto_box_NONCEBYTES]; //24
    crypto_test_make_nonce(nonce, counter);

    /* Prepare data */
    packet = csp_buffer_get(size + crypto_secretbox_ZEROBYTES);
    if (packet == NULL)
        goto out;

    result = crypto_box_afternm(packet->data, buffer->data, size + crypto_secretbox_ZEROBYTES, nonce, key_beforem);
    if (result != 0)
        goto out;

    // Use cyphertext 0-padding for nonce to avoid additonal memcpy's
    packet->length = size + crypto_secretbox_ZEROBYTES;
    memcpy(packet->data, nonce, crypto_secretbox_BOXZEROBYTES);

    printf("Sending [%s]\n", data);
    CRYPTO_TEST_PRINT_HEX(nonce);
    crypto_test_print_hex("buffer->data", buffer->data, size + crypto_secretbox_ZEROBYTES);
    crypto_test_print_hex("packet->data", packet->data, size + crypto_secretbox_BOXZEROBYTES + 16);

out:
    if (buffer != NULL)
        csp_buffer_free(buffer);

    if (result != 0)
        csp_buffer_free(packet);

    return packet;
}

int crypto_test_csp_decrypt_packet(uint8_t * data, unsigned int size) {
    return 0;
}

void crypto_test_packet_handler(csp_packet_t * packet) {
    int result;

    printf("\n\n\n--------\ncrypto_test_packet_handler [%d]\n", packet->length);

    // Allocate an extra buffer for de decrypted data
    csp_packet_t * buffer = NULL;

    buffer = csp_buffer_get(packet->length - crypto_box_BOXZEROBYTES + crypto_box_ZEROBYTES);
    if (buffer == NULL)
        goto out;

    // Extract NONCE from packet, then set padding to zero as required
    unsigned char nonce[crypto_box_NONCEBYTES];
    memset(nonce, 0, crypto_box_NONCEBYTES);
    memcpy(nonce, packet->data, crypto_box_BOXZEROBYTES);
    memset(packet->data, 0, crypto_box_BOXZEROBYTES);

    CRYPTO_TEST_PRINT_HEX(nonce);
    crypto_test_print_hex("packet->data", packet->data, packet->length);

    printf("crypto_box_open_afternm\n");
    result = crypto_box_open_afternm(buffer->data, packet->data, packet->length, nonce, crypto_remote_beforem[0]);
    if(result != 0) {
        printf("ERROR\n");
    }

    crypto_test_print_hex("buffer->data", buffer->data, packet->length);
    printf("Decrypted packet: [%s]\n", buffer->data + crypto_secretbox_ZEROBYTES);

out:
    if (buffer != NULL)
        csp_buffer_free(buffer);
}

void crypto_test_init(void) {
    // Debug
    crypto_test_generate_keys();

    // Pre-compute for for all public keys
    for(int i = 0; i < CRYPTO_REMOTE_KEY_COUNT; i++) {
        crypto_box_beforenm(crypto_remote_beforem[i], crypto_remote_key[i], crypto_key_secret);
    }
    CRYPTO_TEST_PRINT_HEX(crypto_remote_beforem[0]);

    /* Server */
    printf("Register Crypto Packet Handler\r\n");

    csp_socket_t *sock_crypto = csp_socket(CSP_SO_NONE);
    csp_socket_set_callback(sock_crypto, crypto_test_packet_handler);
    csp_bind(sock_crypto, CSP_DECRYPTOR_PORT);
}

int crypto_test_echo(uint8_t node, uint8_t * data, unsigned int size) {

    uint32_t start, time, status = 0;

    printf("crypto_test_echo %d\n", size);

    // Counter
    start = csp_get_ms();

    // Open connection
    csp_conn_t * conn = csp_connect(CSP_PRIO_NORM, node, CSP_DECRYPTOR_PORT, TEST_TIMEOUT, CSP_O_NONE);
    if (conn == NULL)
        return -1;

    csp_packet_t * packet = NULL;
    packet = crypto_test_csp_encrypt_packet(data, size, crypto_key_test_beforem, ++crypto_remote_counter[0]);
    if (packet == NULL)
        goto out;

    // Try to send frame
    if (!csp_send(conn, packet, 0))
        goto out;

    // Read incoming frame
    packet = csp_read(conn, TEST_TIMEOUT);
    if (packet == NULL)
        goto out;

    printf("Received\n");

    status = 1;

out:
    /* Clean up */
    if (packet != NULL)
        csp_buffer_free(packet);

    csp_close(conn);

    /* We have a reply */
    time = (csp_get_ms() - start);

    if (status) {
        printf("Great Success\n");
        return time;
    } else {
        printf("Failed\n");
        return -1;
    }

}
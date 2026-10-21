/*
 * A Windows device identity, and content licences with it
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * Encrypted Store packages (EAppx, MSIXVC) are opened with content keys from licensing.mp.microsoft.com, which hands
 * them out only to a Windows *device*: its answer packs each key (AES key wrap, RFC 3394) under a key carried in that
 * device's licence. Windows gets the device licence once, from login.live.com/ppsecure/deviceaddcredential.srf, and a
 * device ticket for the licensing service from RST2.srf (WS-Trust):
 *   1. deviceaddcredential.srf: a random member name and password plus hardware components answer with an
 *      SPLicenseBlock; its ClepSignState block holds the device's RSA key.
 *   2. RST2.srf, signed with that RSA key (XML-DSig, exclusive c14n, rsa-sha256): the device STS token (an opaque
 *      EncryptedData) and its proof key (ClepHmacState: the HMAC secret).
 *   3. RST2.srf again with that token, HMAC-signed with SP800-108 derived keys, for www.microsoft.com (mbi_ssl): the
 *      compact device ticket for the Authorization header.
 *   4. licensing.mp.microsoft.com/v7.0/licenses/content with the device ticket and the user's ticket for the same
 *      service (our Microsoft account sign-in's refresh token gets that one); the answer's SPLicenseBlock has the
 *      packed keys, and the device licence's EncryptedDeviceKey block the key-encryption key.
 * The protocol was learnt from Xodus (github.com/xodus-project, GPL-3.0); this is our own code.
 *
 * Hardware components: Windows sends the real SMBIOS system entry, disk serials and so on. We send a synthetic SMBIOS
 * type 1 entry (vendor "Wine", a random serial and UUID made once per prefix), so no identifier of this machine leaves
 * it. The device is kept in xboxauth.bin (windev_*), one per prefix.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

#define MSA_UA "User-Agent: MSAWindows/55 (OS 10.0.26100.0.0 ge_release; IDK 10.0.26100.5074 ge_release; Cfg 16.000.29325.00; Test 0)"

#define NS_S     "http://www.w3.org/2003/05/soap-envelope"
#define NS_PS    "http://schemas.microsoft.com/Passport/SoapServices/PPCRL"
#define NS_WSSE  "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
#define NS_WSU   "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
#define NS_WSP   "http://schemas.xmlsoap.org/ws/2004/09/policy"
#define NS_WSA   "http://www.w3.org/2005/08/addressing"
#define NS_WSSC  "http://schemas.xmlsoap.org/ws/2005/02/sc"
#define NS_WST   "http://schemas.xmlsoap.org/ws/2005/02/trust"
#define NS_DSIG  "http://www.w3.org/2000/09/xmldsig#"
#define EXC_C14N "http://www.w3.org/2001/10/xml-exc-c14n#"

/* ---- small crypto helpers (BCrypt) ---- */

static BOOL random_bytes( void *p, size_t n )
{
    return BCRYPT_SUCCESS( BCryptGenRandom( NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG ) );
}

static BOOL hmac_sha256( const void *key, size_t key_len, const void *data, size_t len, unsigned char out[32] )
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BOOL ok;

    if (!BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG ) ))
        return FALSE;
    ok = BCRYPT_SUCCESS( BCryptHash( alg, (UCHAR *)key, key_len, (UCHAR *)data, len, out, 32 ) );
    BCryptCloseAlgorithmProvider( alg, 0 );
    return ok;
}

/* AES in ECB (iv NULL) or CBC (iv 16 bytes), no padding, in place; decrypt or encrypt. */
static BOOL aes_raw( const void *key, size_t key_len, const unsigned char *iv, BOOL decrypt, unsigned char *data, size_t len )
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE k = NULL;
    const WCHAR *mode = iv ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
    unsigned char ivc[16], *out;
    ULONG done = 0;
    NTSTATUS st;
    BOOL ok = FALSE;

    if (len % 16) return FALSE;
    if (iv) memcpy( ivc, iv, 16 );
    if (!BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &alg, BCRYPT_AES_ALGORITHM, NULL, 0 ) )) return FALSE;
    if (BCRYPT_SUCCESS( BCryptSetProperty( alg, BCRYPT_CHAINING_MODE, (UCHAR *)mode, (wcslen( mode ) + 1) * sizeof(WCHAR), 0 ) ) &&
        BCRYPT_SUCCESS( BCryptGenerateSymmetricKey( alg, &k, NULL, 0, (UCHAR *)key, key_len, 0 ) ) &&
        (out = malloc( len ? len : 1 )))
    {
        st = decrypt ? BCryptDecrypt( k, data, len, NULL, iv ? ivc : NULL, iv ? 16 : 0, out, len, &done, 0 )
                     : BCryptEncrypt( k, data, len, NULL, iv ? ivc : NULL, iv ? 16 : 0, out, len, &done, 0 );
        if (BCRYPT_SUCCESS( st ) && done == len)
        {
            memcpy( data, out, len );
            ok = TRUE;
        }
        SecureZeroMemory( out, len );
        free( out );
    }
    if (k) BCryptDestroyKey( k );
    BCryptCloseAlgorithmProvider( alg, 0 );
    return ok;
}

/* RFC 3394 key unwrap with the default IV. key has room for wrapped_len - 8 bytes. */
static BOOL aes_key_unwrap( const unsigned char kek[16], const unsigned char *wrapped, size_t wrapped_len,
                            unsigned char *key, size_t *key_len )
{
    size_t n = wrapped_len / 8 - 1, i;
    unsigned char a[8], block[16], *r;
    int j, b;

    if (wrapped_len % 8 || n < 2) return FALSE;
    if (!(r = malloc( 8 * n ))) return FALSE;
    memcpy( a, wrapped, 8 );
    memcpy( r, wrapped + 8, 8 * n );
    for (j = 5; j >= 0; j--)
        for (i = n; i >= 1; i--)
        {
            unsigned long long t = (unsigned long long)n * j + i;
            for (b = 0; b < 8; b++) a[7 - b] ^= (unsigned char)(t >> (8 * b));
            memcpy( block, a, 8 );
            memcpy( block + 8, r + 8 * (i - 1), 8 );
            if (!aes_raw( kek, 16, NULL, TRUE, block, 16 ))
            {
                free( r );
                return FALSE;
            }
            memcpy( a, block, 8 );
            memcpy( r + 8 * (i - 1), block + 8, 8 );
        }
    for (b = 0; b < 8; b++)
        if (a[b] != 0xa6)
        {
            free( r );
            return FALSE;
        }
    memcpy( key, r, 8 * n );
    *key_len = 8 * n;
    SecureZeroMemory( r, 8 * n );
    free( r );
    return TRUE;
}

/* SP800-108 counter mode, HMAC-SHA256, one 32-byte block: Windows' WS-SecureConversation key. */
static BOOL sp800108_key( const void *secret, size_t secret_len, const void *nonce, size_t nonce_len, unsigned char out[32] )
{
    static const char label[] = "WS-SecureConversationWS-SecureConversation";
    struct buf m = {0};
    BOOL ok;

    buf_addn( &m, "\0\0\0\1", 4 );
    buf_addn( &m, label, sizeof(label) );   /* with its NUL */
    buf_addn( &m, nonce, nonce_len );
    buf_addn( &m, "\0\0\1\0", 4 );          /* 256 bits */
    ok = hmac_sha256( secret, secret_len, m.p, m.n, out );
    buf_free( &m );
    return ok;
}

/* ---- the SPLicenseBlock: a header, then type/length/value blocks ---- */

struct packed_key
{
    const unsigned char *id;         /* 16 bytes */
    const unsigned char *wrapped;
    size_t wrapped_len;
};

struct splicense
{
    const unsigned char *sign_state;  /* 0x12d ClepSignState */
    size_t sign_state_len;
    const unsigned char *device_key;  /* 0x01 EncryptedDeviceKey */
    size_t device_key_len;
    struct packed_key *packed_keys;   /* 0xca: (key id, 40 wrapped bytes) */
    unsigned packed_count;
    char *package_full_name;
    BOOL ok;
};

static unsigned get_u16( const unsigned char *p ) { return p[0] | p[1] << 8; }
static unsigned get_u32( const unsigned char *p ) { return get_u16( p ) | get_u16( p + 2 ) << 16; }

/* the parse points into blob, which has to outlive it */
static void parse_splicense( const unsigned char *blob, size_t len, struct splicense *l )
{
    size_t pos = 8;

    memset( l, 0, sizeof(*l) );
    l->package_full_name = str_dup( "" );
    if (len < 8) return;
    while (pos + 8 <= len)
    {
        unsigned id = get_u32( blob + pos ), size = get_u32( blob + pos + 4 );
        const unsigned char *v;

        pos += 8;
        if (pos + size > len) return;
        v = blob + pos;
        pos += size;
        if (id == 0x12d)
        {
            l->sign_state = v;
            l->sign_state_len = size;
        }
        else if (id == 0x01)
        {
            l->device_key = v;
            l->device_key_len = size;
        }
        else if (id == 0xce)
        {
            free( l->package_full_name );
            l->package_full_name = narrow( (const WCHAR *)v, size / 2 );
        }
        else if (id == 0xca)
        {
            size_t p = 0;
            while (p + 4 <= size)
            {
                unsigned idlen = get_u16( v + p ), keylen = get_u16( v + p + 2 );
                struct packed_key *keys;
                if (idlen < 16 || p + 4 + idlen + keylen > size) break;
                if (!(keys = realloc( l->packed_keys, (l->packed_count + 1) * sizeof(*keys) ))) break;
                l->packed_keys = keys;
                keys[l->packed_count].id = v + p + 4;
                keys[l->packed_count].wrapped = v + p + 4 + idlen;
                keys[l->packed_count].wrapped_len = keylen;
                l->packed_count++;
                p += 4 + idlen + keylen;
            }
        }
    }
    l->ok = pos == len;
}

static void splicense_free( struct splicense *l )
{
    free( l->packed_keys );
    free( l->package_full_name );
    memset( l, 0, sizeof(*l) );
}

/* ClepSignState, ClepHmacState and EncryptedDeviceKey share one 4096-byte shape: AES-128-CBC, zero IV, under a key
 * read out of an expanded key schedule stored next to the data (4 words, each two schedule words and constants). */
static void clep_key( const unsigned char *state, size_t schedule, unsigned char out[16] )
{
    unsigned k[4];
#define KS(i) get_u32( state + schedule + 4 * (i) )
    k[0] = KS(46) ^ KS(56) ^ 0xE20DF371 ^ 0xCCB22FE6;
    k[1] = KS(36) ^ KS(47) ^ 0xDF080E39;
    k[2] = KS(40) ^ KS(51) ^ 0x6D09B2F5 ^ 0x2AE17AB9;
    k[3] = KS(30) ^ KS(41) ^ 0x37288CEC;
#undef KS
    memcpy( out, k, 16 );
}

static BOOL clep_decrypt( const unsigned char *state, size_t state_len, size_t version_at, size_t data_at, size_t data_len,
                          size_t schedule, unsigned char *out )
{
    static const unsigned char zero_iv[16];
    unsigned char key[16];
    BOOL ok;

    if (state_len != 4096 || get_u32( state + version_at ) != 4) return FALSE;
    memcpy( out, state + data_at, data_len );
    clep_key( state, schedule, key );
    ok = aes_raw( key, 16, zero_iv, TRUE, out, data_len );
    SecureZeroMemory( key, sizeof(key) );
    return ok;
}

static BOOL clep_sign_key( const unsigned char *state, size_t state_len, unsigned char rsa_blob[544] )
{
    return clep_decrypt( state, state_len, 0, 4, 544, 548, rsa_blob );
}

static BOOL clep_hmac_secret( const unsigned char *state, size_t state_len, unsigned char secret[32] )
{
    unsigned char plain[48];
    if (!clep_decrypt( state, state_len, 0, 4, 48, 52, plain )) return FALSE;
    memcpy( secret, plain + 12, 32 );
    SecureZeroMemory( plain, sizeof(plain) );
    return TRUE;
}

static BOOL device_key_from( const unsigned char *state, size_t state_len, unsigned char key[16] )
{
    static const unsigned char zero_iv[16];
    unsigned char k[16];
    BOOL ok;

    if (state_len != 4096 || get_u32( state + 2 ) != 4) return FALSE;
    memcpy( key, state + 518, 16 );
    clep_key( state, 6, k );
    ok = aes_raw( k, 16, zero_iv, TRUE, key, 16 ) && !memcmp( key, k, 16 );   /* the block carries its own key: a check that it decoded */
    SecureZeroMemory( k, sizeof(k) );
    return ok;
}

/* ---- the hardware challenge (components 8196/8197): a fixed, keyless Feistel obfuscation over 8-byte blocks ---- */

#define CLEP_M_HI 0x24181621u
#define CLEP_M_LO 0x41393243u
#define CLEP_M1 0x2418u
#define CLEP_M2 0x1621u
#define CLEP_M3 0x4139u
#define CLEP_M4 0x3243u

static unsigned rotr( unsigned x, int n ) { return x >> n | x << (32 - n); }
static unsigned rotl( unsigned x, int n ) { return x << n | x >> (32 - n); }

static unsigned clep_f( int r, unsigned x )
{
    switch (r)
    {
    case 1: return CLEP_M4 * rotr( x ^ CLEP_M_HI, 22 ) - rotr( x, 8 );
    case 2: return CLEP_M4 * (rotr( x, 15 ) ^ CLEP_M1);
    case 3: return (x >> 9) + CLEP_M2 * rotl( x ^ CLEP_M3, 3 );
    case 4: return rotr( x, 28 ) ^ CLEP_M3 * rotr( x ^ CLEP_M_HI, 9 );
    case 5: return rotr( x, 12 ) + CLEP_M4 * rotr( x - CLEP_M_HI, 14 );
    case 6: return rotr( x, 11 ) ^ CLEP_M1 * rotl( x ^ CLEP_M2, 2 );
    case 7: return x - CLEP_M_LO - CLEP_M2;
    default: return CLEP_M3 * rotl( x ^ CLEP_M1, 2 ) - rotr( x, 18 );
    }
}

static unsigned clep_seed( unsigned x ) { return CLEP_M4 * rotr( x - CLEP_M_HI, 18 ) - rotr( x, 9 ); }
static unsigned clep_whiten( unsigned x ) { return CLEP_M3 * rotr( x + CLEP_M_HI, 10 ) - rotr( x, 29 ); }

static unsigned clep_initial_state( void )
{
    unsigned a = 0, b = ~(CLEP_M3 * rotr( CLEP_M_HI, 10 ));
    int r;
    for (r = 1; r <= 8; r++)
    {
        unsigned t = a ^ clep_f( r, b );
        a = b;
        b = t;
    }
    return b;
}

static void clep_obfuscate( unsigned char buf[2048] )
{
    unsigned iv = buf[4] | buf[5] << 8 | buf[6] << 16 | (unsigned)buf[7] << 24;
    unsigned lo = clep_initial_state() ^ iv, hi = 0, plo = iv, phi = 0;
    int blk, r;

    memcpy( buf + 4, &lo, 4 );
    for (blk = 1; blk < 256; blk++)
    {
        unsigned char *p = buf + 8 * blk;
        unsigned blo, bhi, a, b, nlo, nhi;

        memcpy( &blo, p, 4 );
        memcpy( &bhi, p + 4, 4 );
        a = lo ^ blo;
        b = hi ^ bhi ^ clep_seed( a );
        for (r = 8; r >= 1; r--)   /* (a,b) -> (b ^ f(a), a) */
        {
            unsigned t = b ^ clep_f( r, a );
            b = a;
            a = t;
        }
        nlo = plo ^ a ^ clep_whiten( b );
        nhi = b ^ phi;
        lo = nlo;
        hi = nhi;
        plo = blo;
        phi = bhi;
        memcpy( p, &nlo, 4 );
        memcpy( p + 4, &nhi, 4 );
    }
}

/* ---- the device: credentials, a synthetic SMBIOS entry, the licence and its tokens ---- */

static struct
{
    char *member, *password, *puid;
    struct buf license;               /* the SPLicenseBlock, raw */
    struct buf smbios;                /* our synthetic SMBIOS type 1 entry, raw */
    struct buf sts_token, sts_secret; /* the device STS token (EncryptedData XML) and its proof key */
    ULONGLONG sts_expires;
} windev;
static CRITICAL_SECTION windev_cs;

static BOOL WINAPI windev_init_once( INIT_ONCE *once, void *param, void **context )
{
    InitializeCriticalSection( &windev_cs );
    return TRUE;
}

static void windev_init( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, windev_init_once, NULL, NULL );
}

static char *random_alnum( size_t n )
{
    static const char t[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[64];
    char *s;
    size_t i;

    if (n > sizeof(r)) n = sizeof(r);
    if (!(s = malloc( n + 1 ))) return NULL;
    random_bytes( r, n );
    for (i = 0; i < n; i++) s[i] = t[r[i] % 62];
    s[n] = 0;
    return s;
}

/* An SMBIOS 3.x type 1 (system information) structure with its strings. */
static void make_smbios( struct buf *s )
{
    unsigned char uuid[16], serial[8];
    char hex[17];
    int i;

    random_bytes( uuid, 16 );
    random_bytes( serial, 8 );
    for (i = 0; i < 8; i++) sprintf( hex + 2 * i, "%02X", serial[i] );
    s->n = 0;
    buf_addn( s, "\1\x1b\0\1", 4 );      /* type 1, length 27, handle 0x0100 */
    buf_addn( s, "\1\2\3\4", 4 );        /* manufacturer, product, version, serial */
    buf_addn( s, uuid, 16 );
    buf_addn( s, "\6\5\6", 3 );          /* wake-up: power switch; SKU; family */
    buf_addn( s, "Wine\0wine-uwp\0" "1.0\0", 18 );
    buf_addn( s, hex, 17 );
    buf_addn( s, "wine-uwp\0Wine\0\0", 15 );
}

static char *smbios_string( const struct buf *e, int index )
{
    size_t p;
    int i;

    if (e->n < 2) return str_dup( "" );
    p = (unsigned char)e->p[1];
    for (i = 1; p < e->n; i++)
    {
        const char *z = memchr( e->p + p, 0, e->n - p );
        if (!z || z == e->p + p) break;
        if (i == index) return str_dupn( e->p + p, z - (e->p + p) );
        p = z - e->p + 1;
    }
    return str_dup( "" );
}

static void windev_load( void )
{
    struct kv kv;

    store_read( &kv );
    str_set( &windev.member, kv_get( &kv, "windev_member" ) );
    str_set( &windev.password, kv_get( &kv, "windev_password" ) );
    str_set( &windev.puid, kv_get( &kv, "windev_puid" ) );
    windev.license.n = 0;
    unb64( kv_get( &kv, "windev_license" ), &windev.license );
    windev.smbios.n = 0;
    unb64( kv_get( &kv, "windev_smbios" ), &windev.smbios );
    windev.sts_token.n = 0;
    unb64( kv_get( &kv, "windev_sts_token" ), &windev.sts_token );
    windev.sts_secret.n = 0;
    unb64( kv_get( &kv, "windev_sts_secret" ), &windev.sts_secret );
    windev.sts_expires = _strtoui64( kv_get( &kv, "windev_sts_expires" ), NULL, 10 );
    kv_free( &kv );
}

static void windev_save( void )
{
    char *license = b64( (unsigned char *)windev.license.p, windev.license.n, FALSE );
    char *smbios = b64( (unsigned char *)windev.smbios.p, windev.smbios.n, FALSE );
    char *sts_token = b64( (unsigned char *)windev.sts_token.p, windev.sts_token.n, FALSE );
    char *sts_secret = b64( (unsigned char *)windev.sts_secret.p, windev.sts_secret.n, FALSE );
    char expires[32];

    sprintf( expires, "%I64u", windev.sts_expires );
    store_update( "windev_member", S(windev.member), "windev_password", S(windev.password), "windev_puid", S(windev.puid),
                  "windev_license", S(license), "windev_smbios", S(smbios), "windev_sts_token", S(sts_token),
                  "windev_sts_secret", S(sts_secret), "windev_sts_expires", expires, NULL );
    str_wipe_free( sts_secret );
    free( sts_token );
    free( smbios );
    free( license );
}

/* ---- minimal XML reading: the first element with a local name, at or after a position ---- */

struct xml_elem
{
    const char *start, *inner, *inner_end, *end;   /* start NULL: not found */
};

static BOOL xml_name_end( char c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>' || !c;
}

static struct xml_elem xml_find( const char *x, const char *local, const char *from )
{
    struct xml_elem e = {0};
    size_t local_len = strlen( local );
    const char *p;

    for (p = strchr( from ? from : x, '<' ); p; p = strchr( p + 1, '<' ))
    {
        const char *q, *gt, *name = p + 1, *colon, *s;
        char open[136], close[136];
        size_t name_len;
        int depth;

        if (p[1] == '/' || p[1] == '?' || p[1] == '!') continue;
        q = name;
        while (!xml_name_end( *q )) q++;
        name_len = q - name;
        colon = memchr( name, ':', name_len );
        if (colon ? (size_t)(q - colon - 1) != local_len || strncmp( colon + 1, local, local_len )
                  : name_len != local_len || strncmp( name, local, local_len )) continue;
        if (!(gt = strchr( q, '>' )) || name_len > 128) return e;
        e.start = p;
        if (gt[-1] == '/')
        {
            e.inner = e.inner_end = gt;
            e.end = gt + 1;
            return e;
        }
        e.inner = gt + 1;
        /* the matching end tag: nesting of the same name is counted */
        sprintf( open, "<%.*s", (int)name_len, name );
        sprintf( close, "</%.*s>", (int)name_len, name );
        depth = 1;
        s = e.inner;
        while (depth)
        {
            const char *o = strstr( s, open ), *c = strstr( s, close );
            if (!c)
            {
                e.start = NULL;
                return e;
            }
            if (o && o < c && xml_name_end( o[1 + name_len] ))
            {
                const char *og = strchr( o, '>' );
                if (!og)
                {
                    e.start = NULL;
                    return e;
                }
                if (og[-1] != '/') depth++;
                s = og + 1;
                continue;
            }
            depth--;
            if (!depth)
            {
                e.inner_end = c;
                e.end = c + name_len + 3;
            }
            s = c + name_len + 3;
        }
        return e;
    }
    return e;
}

/* the element's text, malloc'ed; "" when there is no such element */
static char *xml_text( const char *x, const char *local, const char *from )
{
    struct xml_elem e = xml_find( x, local, from );
    return e.start ? str_dupn( e.inner, e.inner_end - e.inner ) : str_dup( "" );
}

static char *xml_attr( const char *x, const struct xml_elem *e, const char *attr )
{
    char want[64];
    const char *tag_end, *p;
    size_t attr_len = strlen( attr );

    if (!e->start || attr_len > 60 || !(tag_end = strchr( e->start, '>' ))) return str_dup( "" );
    sprintf( want, "%s=\"", attr );
    for (p = strstr( e->start, want ); p && p < tag_end; p = strstr( p + 1, want ))
    {
        const char *v = p + attr_len + 2, *end;
        if (p > e->start && p[-1] != ' ' && p[-1] != ':') continue;
        if (!(end = strchr( v, '"' ))) break;
        return str_dupn( v, end - v );
    }
    return str_dup( "" );
}

static char *xml_unescape( const char *s )
{
    static const struct { const char *ent; char c; } ents[] =
    {
        { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&amp;", '&' }
    };
    struct buf o = {0};
    unsigned i;

    buf_addn( &o, "", 0 );
    while (*s)
    {
        for (i = 0; i < ARRAY_SIZE(ents); i++)
        {
            size_t len = strlen( ents[i].ent );
            if (!strncmp( s, ents[i].ent, len ))
            {
                buf_addc( &o, ents[i].c );
                s += len;
                break;
            }
        }
        if (i == ARRAY_SIZE(ents)) buf_addc( &o, *s++ );
    }
    return o.p;
}

static void xml_escape_into( struct buf *o, const char *s )
{
    for (; *s; s++)
    {
        if (*s == '&') buf_add( o, "&amp;" );
        else if (*s == '<') buf_add( o, "&lt;" );
        else if (*s == '>') buf_add( o, "&gt;" );
        else buf_addc( o, *s );
    }
}

/* ---- deviceaddcredential.srf ---- */

static void component( struct buf *body, int name, const void *value, size_t len )
{
    char *enc = b64( value, len, FALSE );
    buf_printf( body, "<Component name=\"%d\">%s</Component>", name, S(enc) );
    free( enc );
}

static void component_error( struct buf *body, int name )
{
    buf_printf( body, "<Component name=\"%d\" error=\"-2147024894\"></Component>", name );
}

static HRESULT windev_provision( void )
{
    unsigned char *v2, *v4;
    struct buf body = {0}, resp = {0};
    struct splicense l;
    struct xml_elem root;
    char *ok, *block, *s3, *s4, *alnum;
    DWORD status;
    HRESULT hr;

    if (!(v2 = calloc( 2, 2048 ))) return E_OUTOFMEMORY;
    v4 = v2 + 2048;
    alnum = random_alnum( 14 );
    free( windev.member );
    windev.member = strf( "02%s", S(alnum) );
    free( alnum );
    free( windev.password );
    windev.password = random_alnum( 20 );
    if (!windev.smbios.n) make_smbios( &windev.smbios );

    /* ClepV2 / ClepV4: version, the SMBIOS entry (256 bytes), the disk serial (64, ours empty), a few flags */
    v2[0] = 2;
    memcpy( v2 + 4, windev.smbios.p, min( windev.smbios.n, 256 ) );
    v2[328] = 1;                                                   /* always set */
    v4[0] = 4;
    memcpy( v4 + 4, windev.smbios.p, min( windev.smbios.n, 256 ) );
    v4[1231] = 1;                                                  /* no debugger */
    clep_obfuscate( v2 );
    clep_obfuscate( v4 );

    s3 = smbios_string( &windev.smbios, 3 );
    s4 = smbios_string( &windev.smbios, 4 );
    buf_printf( &body, "<DeviceAddRequest><ClientInfo name=\"IDCRL\" version=\"1.0\"><BinaryVersion>55</BinaryVersion></ClientInfo>"
                "<Authentication><Membername>%s</Membername><Password>%s</Password></Authentication>"
                "<DeviceInfo Id=\"DeviceInfo\">", windev.member, windev.password );
    component( &body, 4113, "", 1 );
    component( &body, 8196, v2, 2048 );
    component( &body, 8197, v4, 2048 );
    component( &body, 4100, s3, strlen( s3 ) );
    component( &body, 4101, s4, strlen( s4 ) );
    component( &body, 4102, windev.smbios.p + 8, windev.smbios.n >= 24 ? 16 : 0 );
    component( &body, 4145, "\1\0\0\0", 4 );
    component_error( &body, 4160 );
    component_error( &body, 4161 );
    buf_add( &body, "</DeviceInfo></DeviceAddRequest>" );
    free( s3 );
    free( s4 );
    free( v2 );

    status = http_request( L"POST", "https://login.live.com/ppsecure/deviceaddcredential.srf",
                           MSA_UA "\r\nContent-Type: application/soap+xml\r\n", body.p, body.n, &resp );
    buf_free( &body );
    root = xml_find( buf_str( &resp ), "DeviceAddResponse", NULL );
    ok = xml_attr( buf_str( &resp ), &root, "Success" );
    block = xml_text( buf_str( &resp ), "SPLicenseBlock", NULL );
    TRACE( "windev: deviceaddcredential HTTP %lu, Success \"%s\", licence %u chars\n", status, ok, (unsigned)strlen( block ) );
    free( ok );
    if (status != 200 || !*block)
    {
        TRACE( "windev: deviceaddcredential answered: %.400s\n", buf_str( &resp ) );
        free( block );
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    windev.license.n = 0;
    unb64( block, &windev.license );
    free( block );
    free( windev.puid );
    windev.puid = xml_text( buf_str( &resp ), "puid", NULL );
    buf_free( &resp );
    windev.sts_token.n = 0;
    windev.sts_secret.n = 0;
    windev.sts_expires = 0;
    parse_splicense( (unsigned char *)windev.license.p, windev.license.n, &l );
    TRACE( "windev: device licence %u bytes, parsed %d, sign state %u, device key %u\n", (unsigned)windev.license.n, l.ok,
           (unsigned)l.sign_state_len, (unsigned)l.device_key_len );
    windev_save();
    hr = l.sign_state_len == 4096 ? S_OK : E_FAIL;
    splicense_free( &l );
    return hr;
}

/* ---- RST2.srf ---- */

static void iso_time( ULONGLONG t, char out[32] )
{
    time_t tt = (time_t)t;
    struct tm g;

    gmtime_s( &g, &tt );
    strftime( out, 32, "%Y-%m-%dT%H:%M:%SZ", &g );
}

/* Every element that is signed is written here exactly as exclusive c14n renders it on its own: no whitespace,
 * start and end tags, its namespace declarations on it, attributes in order. So its bytes are its digest input. */
static char *rst_auth_info( const char *hosting_app, BOOL sso_restricted, BOOL key_version )
{
    return strf( "<ps:AuthInfo xmlns:ps=\"" NS_PS "\" Id=\"PPAuthInfo\">%s"
                 "<ps:HostingApp>%s</ps:HostingApp><ps:BinaryVersion>55</ps:BinaryVersion><ps:UIVersion>1</ps:UIVersion>"
                 "<ps:InlineUX>TokenBroker</ps:InlineUX><ps:IsAdmin>1</ps:IsAdmin><ps:Cookies></ps:Cookies>"
                 "<ps:RequestParams>AQAAAAIAAABsYwQAAAAxMDMz</ps:RequestParams>"
                 "<ps:WindowsClientString>b4d/QB7Zy5pjUAY9ByQ1echTyTITx6ZCErOEztuIVtw=</ps:WindowsClientString>%s"
                 "<ps:ClientCapabilities>1</ps:ClientCapabilities></ps:AuthInfo>",
                 sso_restricted ? "<ps:SSOFlags>SsoRestr</ps:SSOFlags>" : "", hosting_app,
                 key_version ? "<ps:LicenseSignatureKeyVersion>2</ps:LicenseSignatureKeyVersion>" : "" );
}

static char *rst_timestamp( ULONGLONG now )
{
    char created[32], expires[32];

    iso_time( now, created );
    iso_time( now + 300, expires );
    return strf( "<wsu:Timestamp xmlns:wsu=\"" NS_WSU "\" wsu:Id=\"Timestamp\"><wsu:Created>%s</wsu:Created>"
                 "<wsu:Expires>%s</wsu:Expires></wsu:Timestamp>", created, expires );
}

static char *rst_request( const char *scope, const char *policy )
{
    struct buf o = {0};

    buf_add( &o, "<wst:RequestSecurityToken xmlns:wst=\"" NS_WST "\" Id=\"RST0\">"
                 "<wst:RequestType>http://schemas.xmlsoap.org/ws/2005/02/trust/Issue</wst:RequestType>"
                 "<wsp:AppliesTo xmlns:wsp=\"" NS_WSP "\"><wsa:EndpointReference xmlns:wsa=\"" NS_WSA "\"><wsa:Address>" );
    xml_escape_into( &o, scope );
    buf_add( &o, "</wsa:Address></wsa:EndpointReference></wsp:AppliesTo>" );
    if (policy && *policy)
        buf_printf( &o, "<wsp:PolicyReference xmlns:wsp=\"" NS_WSP "\" URI=\"%s\"></wsp:PolicyReference>", policy );
    buf_add( &o, "</wst:RequestSecurityToken>" );
    return o.p;
}

static char *digest_b64( const char *s )
{
    unsigned char h[32];
    sha256( s, strlen( s ), h );
    return b64( h, 32, FALSE );
}

static char *signed_info( const char *method, const char *rst, const char *ts, const char *ai )
{
    const struct { const char *uri; const char *elem; } refs[] = { { "#RST0", rst }, { "#Timestamp", ts }, { "#PPAuthInfo", ai } };
    struct buf si = {0};
    unsigned i;

    buf_printf( &si, "<SignedInfo xmlns=\"" NS_DSIG "\"><CanonicalizationMethod Algorithm=\"" EXC_C14N
                "\"></CanonicalizationMethod><SignatureMethod Algorithm=\"%s\"></SignatureMethod>", method );
    for (i = 0; i < ARRAY_SIZE(refs); i++)
    {
        char *digest = digest_b64( refs[i].elem );
        buf_printf( &si, "<Reference URI=\"%s\"><Transforms><Transform Algorithm=\"" EXC_C14N "\"></Transform></Transforms>"
                    "<DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\"></DigestMethod>"
                    "<DigestValue>%s</DigestValue></Reference>", refs[i].uri, S(digest) );
        free( digest );
    }
    buf_add( &si, "</SignedInfo>" );
    return si.p;
}

/* the SignedInfo as it sits inside the Signature element: its namespace comes from the parent */
static void add_signed_info_inline( struct buf *o, const char *si )
{
    const char *gt = strchr( si, '>' );
    buf_add( o, "<SignedInfo>" );
    buf_add( o, gt ? gt + 1 : si );
}

static char *rst_envelope( const char *auth_info, const char *security, const char *rst )
{
    return strf( "<s:Envelope xmlns:s=\"" NS_S "\" xmlns:ps=\"" NS_PS "\" xmlns:wsse=\"" NS_WSSE "\" xmlns:wsu=\"" NS_WSU
                 "\" xmlns:wsp=\"" NS_WSP "\" xmlns:wsa=\"" NS_WSA "\" xmlns:wssc=\"" NS_WSSC "\" xmlns:wst=\"" NS_WST
                 "\"><s:Header>"
                 "<wsa:Action s:mustUnderstand=\"1\">http://schemas.xmlsoap.org/ws/2005/02/trust/RST/Issue</wsa:Action>"
                 "<wsa:To s:mustUnderstand=\"1\">https://login.live.com:443/RST2.srf</wsa:To>"
                 "<wsa:MessageID>%I64u</wsa:MessageID>%s<wsse:Security>%s</wsse:Security></s:Header>"
                 "<s:Body>%s</s:Body></s:Envelope>", now_s(), auth_info, security, rst );
}

static DWORD rst2_post( const char *xml, struct buf *resp )
{
    return http_request( L"POST", "https://login.live.com/RST2.srf", MSA_UA "\r\nContent-Type: application/soap+xml\r\n",
                         xml, strlen( xml ), resp );
}

static void rst2_log_fault( const char *what, DWORD status, const char *resp )
{
    char *code = xml_text( resp, "Value", NULL ), *reason = xml_text( resp, "Text", NULL ),
         *req = xml_text( resp, "reqstatus", NULL ), *err = xml_text( resp, "error", NULL );

    TRACE( "windev: %s: HTTP %lu, fault %s \"%s\", reqstatus %s, error %s\n", what, status, code, reason, req, err );
    free( code );
    free( reason );
    free( req );
    free( err );
}

static BOOL rsa_sign_sha256( const unsigned char *blob, size_t blob_len, const char *data, struct buf *sig )
{
    unsigned magic, e, n, p1, p2;
    size_t len;
    const WCHAR *type = BCRYPT_RSAPRIVATE_BLOB;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    unsigned char h[32];
    BOOL ok = FALSE;

    if (blob_len < 24) return FALSE;
    magic = get_u32( blob );
    e = get_u32( blob + 8 );
    n = get_u32( blob + 12 );
    p1 = get_u32( blob + 16 );
    p2 = get_u32( blob + 20 );
    len = 24 + e + n + p1 + p2;
    if (magic == BCRYPT_RSAFULLPRIVATE_MAGIC)
    {
        len += p1 + p2 + p1 + n;
        type = BCRYPT_RSAFULLPRIVATE_BLOB;
    }
    else if (magic != BCRYPT_RSAPRIVATE_MAGIC) return FALSE;
    if (len > blob_len) return FALSE;
    sha256( data, strlen( data ), h );
    if (!BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &alg, BCRYPT_RSA_ALGORITHM, NULL, 0 ) )) return FALSE;
    if (BCRYPT_SUCCESS( BCryptImportKeyPair( alg, NULL, type, &key, (UCHAR *)blob, len, 0 ) ))
    {
        BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
        ULONG size = 0;
        unsigned char *out = calloc( 1, n );

        if (out)
        {
            ok = BCRYPT_SUCCESS( BCryptSignHash( key, &pad, h, 32, out, n, &size, BCRYPT_PAD_PKCS1 ) ) && size == n;
            sig->n = 0;
            buf_addn( sig, out, n );
            free( out );
        }
        BCryptDestroyKey( key );
    }
    else TRACE( "windev: the device key does not import (magic %#x, %u bytes)\n", magic, (unsigned)len );
    BCryptCloseAlgorithmProvider( alg, 0 );
    return ok;
}

/* The device STS token: RST2 signed with the device's RSA key, for http://Passport.NET/tb. */
static HRESULT windev_authenticate( void )
{
    struct splicense l;
    unsigned char rsa[544];
    struct buf sig = {0}, security = {0}, resp = {0};
    char *ai, *ts, *rst, *si, *envelope, *sig_b64, *secret;
    struct xml_elem rstr, enc;
    ULONGLONG now = now_s();
    DWORD status;
    BOOL signed_ok;
    HRESULT hr;

    parse_splicense( (unsigned char *)windev.license.p, windev.license.n, &l );
    signed_ok = clep_sign_key( l.sign_state, l.sign_state_len, rsa );
    splicense_free( &l );
    if (!signed_ok)
    {
        TRACE( "windev: no usable sign state in the device licence\n" );
        return E_FAIL;
    }
    ai = rst_auth_info( "{DF60E2DF-88AD-4526-AE21-83D130EF0F68}", FALSE, TRUE );
    ts = rst_timestamp( now );
    rst = rst_request( "http://Passport.NET/tb", "" );
    si = signed_info( "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256", rst, ts, ai );
    signed_ok = rsa_sign_sha256( rsa, sizeof(rsa), si, &sig );
    SecureZeroMemory( rsa, sizeof(rsa) );
    if (!signed_ok)
    {
        TRACE( "windev: signing with the device key failed\n" );
        hr = E_FAIL;
        goto done;
    }
    sig_b64 = b64( (unsigned char *)sig.p, sig.n, FALSE );
    buf_printf( &security, "<wsse:UsernameToken wsu:Id=\"devicesoftware\"><wsse:Username>%s</wsse:Username></wsse:UsernameToken>%s"
                "<Signature xmlns=\"" NS_DSIG "\">", windev.member, ts );
    add_signed_info_inline( &security, si );
    buf_printf( &security, "<SignatureValue>%s</SignatureValue></Signature>", S(sig_b64) );
    free( sig_b64 );
    envelope = rst_envelope( ai, buf_str( &security ), rst );
    status = rst2_post( envelope, &resp );
    free( envelope );
    rstr = xml_find( buf_str( &resp ), "RequestSecurityTokenResponse", NULL );
    enc = rstr.start ? xml_find( buf_str( &resp ), "EncryptedData", rstr.inner ) : rstr;
    secret = xml_text( buf_str( &resp ), "BinarySecret", rstr.start ? rstr.inner : NULL );
    if (status != 200 || !enc.start || !*secret)
    {
        rst2_log_fault( "device authentication", status, buf_str( &resp ) );
        hr = XA_E_NETWORK;
    }
    else
    {
        char *expires = xml_text( buf_str( &resp ), "Expires", rstr.inner );

        windev.sts_token.n = 0;
        buf_addn( &windev.sts_token, enc.start, enc.end - enc.start );
        windev.sts_secret.n = 0;
        unb64( secret, &windev.sts_secret );
        windev.sts_expires = parse_iso( expires );
        free( expires );
        TRACE( "windev: device STS token %u chars, proof key %u bytes, expires in %I64d s\n", (unsigned)windev.sts_token.n,
               (unsigned)windev.sts_secret.n, (LONGLONG)(windev.sts_expires - now) );
        windev_save();
        hr = windev.sts_secret.n == 4096 ? S_OK : E_FAIL;
    }
    str_wipe_free( secret );
    buf_free( &resp );
done:
    buf_free( &security );
    buf_free( &sig );
    free( si );
    free( rst );
    free( ts );
    free( ai );
    return hr;
}

/* An RST2 request with the device STS token, HMAC-signed: a token for another scope. */
static HRESULT windev_exchange( const char *hosting_app, const char *scope, const char *policy, char **token )
{
    unsigned char secret[32], nonce[32], key[32], mac[32];
    struct buf security = {0}, resp = {0}, server_nonce = {0}, cipher = {0};
    char *ai, *ts, *rst, *si, *envelope, *nonce_b64, *mac_b64, *id = NULL, *plain = NULL, *bst = NULL;
    struct xml_elem body, enc, ref, d;
    DWORD status;
    HRESULT hr = E_FAIL;
    size_t plain_len;
    unsigned char pad;

    *token = NULL;
    if (!clep_hmac_secret( (unsigned char *)windev.sts_secret.p, windev.sts_secret.n, secret ))
    {
        TRACE( "windev: no usable proof key\n" );
        return E_FAIL;
    }
    random_bytes( nonce, sizeof(nonce) );
    sp800108_key( secret, sizeof(secret), nonce, sizeof(nonce), key );
    ai = rst_auth_info( hosting_app, TRUE, TRUE );
    ts = rst_timestamp( now_s() );
    rst = rst_request( scope, policy );
    si = signed_info( "http://www.w3.org/2001/04/xmldsig-more#hmac-sha256", rst, ts, ai );
    hmac_sha256( key, sizeof(key), si, strlen( si ), mac );
    nonce_b64 = b64( nonce, sizeof(nonce), FALSE );
    mac_b64 = b64( mac, sizeof(mac), FALSE );
    buf_addn( &security, windev.sts_token.p, windev.sts_token.n );
    buf_printf( &security,
                "<wssc:DerivedKeyToken wsu:Id=\"SignKey\" Algorithm=\"urn:liveid:SP800108_CTR_HMAC_SHA256_DOUBLEDERIVED\">"
                "<wsse:RequestedTokenReference><wsse:KeyIdentifier ValueType=\"http://docs.oasis-open.org/wss/2004/XX/oasis-2004XX-wss-saml-token-profile-1.0#SAMLAssertionID\"></wsse:KeyIdentifier>"
                "<wsse:Reference URI=\"\"></wsse:Reference></wsse:RequestedTokenReference><wssc:Nonce>%s</wssc:Nonce></wssc:DerivedKeyToken>%s"
                "<Signature xmlns=\"" NS_DSIG "\">", S(nonce_b64), ts );
    add_signed_info_inline( &security, si );
    buf_printf( &security, "<SignatureValue>%s</SignatureValue><KeyInfo><wsse:SecurityTokenReference>"
                "<wsse:Reference URI=\"#SignKey\"></wsse:Reference></wsse:SecurityTokenReference></KeyInfo></Signature>", S(mac_b64) );
    free( nonce_b64 );
    free( mac_b64 );
    envelope = rst_envelope( ai, buf_str( &security ), rst );
    status = rst2_post( envelope, &resp );
    free( envelope );

    /* the body comes back encrypted (AES-256-CBC, the IV first) under a key derived from a nonce the server picked */
    body = xml_find( buf_str( &resp ), "Body", NULL );
    enc = body.start ? xml_find( buf_str( &resp ), "EncryptedData", body.inner ) : body;
    if (status != 200 || !enc.start)
    {
        char *what = strf( "exchange for %s", scope );
        rst2_log_fault( what, status, buf_str( &resp ) );
        free( what );
        hr = XA_E_NETWORK;
        goto done;
    }
    ref = xml_find( buf_str( &resp ), "Reference", enc.inner );
    id = xml_attr( buf_str( &resp ), &ref, "URI" );
    for (d = xml_find( buf_str( &resp ), "DerivedKeyToken", NULL ); d.start; d = xml_find( buf_str( &resp ), "DerivedKeyToken", d.end ))
    {
        char *did = xml_attr( buf_str( &resp ), &d, "Id" );
        if (*id == '#' && !strcmp( id + 1, did ))
        {
            char *n = xml_text( buf_str( &resp ), "Nonce", d.inner );
            server_nonce.n = 0;
            unb64( n, &server_nonce );
            free( n );
        }
        free( did );
    }
    {
        char *c = xml_text( buf_str( &resp ), "CipherValue", enc.inner );
        unb64( c, &cipher );
        free( c );
    }
    if (!server_nonce.n || cipher.n < 32 || cipher.n % 16)
    {
        TRACE( "windev: exchange answer not understood (key %s, %u cipher bytes)\n", id, (unsigned)cipher.n );
        goto done;
    }
    plain_len = cipher.n - 16;
    if (!(plain = malloc( plain_len + 1 ))) goto done;
    memcpy( plain, cipher.p + 16, plain_len );
    sp800108_key( secret, sizeof(secret), server_nonce.p, server_nonce.n, key );
    if (!aes_raw( key, sizeof(key), (unsigned char *)cipher.p, TRUE, (unsigned char *)plain, plain_len ) ||
        (pad = (unsigned char)plain[plain_len - 1]) > 16 || pad > plain_len)
    {
        TRACE( "windev: the exchange answer does not decrypt\n" );
        goto done;
    }
    plain_len -= pad;
    plain[plain_len] = 0;
    bst = xml_text( plain, "BinarySecurityToken", NULL );
    if (!*bst)
    {
        char *what = strf( "exchange for %s (decrypted)", scope );
        rst2_log_fault( what, status, plain );
        free( what );
        goto done;
    }
    *token = xml_unescape( bst );
    TRACE( "windev: %s ticket, %u chars\n", scope, (unsigned)strlen( *token ) );
    hr = S_OK;
done:
    SecureZeroMemory( secret, sizeof(secret) );
    SecureZeroMemory( key, sizeof(key) );
    str_wipe_free( bst );
    str_wipe_free( plain );
    free( id );
    buf_free( &cipher );
    buf_free( &server_nonce );
    buf_free( &resp );
    buf_free( &security );
    free( si );
    free( rst );
    free( ts );
    free( ai );
    return hr;
}

/* Provision the device once, and keep its STS token fresh. */
HRESULT windev_ready( void )
{
    HRESULT hr = S_OK;

    windev_init();
    EnterCriticalSection( &windev_cs );
    if (!windev.license.n) windev_load();
    if (!windev.license.n && FAILED(hr = windev_provision())) goto done;
    if (!windev.sts_token.n || windev.sts_expires < now_s() + 600)
    {
        hr = windev_authenticate();
        if (FAILED(hr) && hr != XA_E_NETWORK)
        {
            /* a licence the service no longer knows: start over with a new device */
            TRACE( "windev: re-provisioning\n" );
            windev.license.n = 0;
            if (SUCCEEDED(hr = windev_provision())) hr = windev_authenticate();
        }
    }
done:
    LeaveCriticalSection( &windev_cs );
    return hr;
}

/* the compact device ticket for www.microsoft.com, as the licensing service's Authorization header takes it */
HRESULT windev_device_ticket( char **ticket )
{
    HRESULT hr;

    if (FAILED(hr = windev_ready())) return hr;
    EnterCriticalSection( &windev_cs );
    hr = windev_exchange( "{d6d5a677-0872-4ab0-9442-bb792fce85c5}", "www.microsoft.com", "mbi_ssl", ticket );
    LeaveCriticalSection( &windev_cs );
    return hr;
}

/* ---- licensing.mp.microsoft.com: the content keys of a package this account is entitled to ---- */

HRESULT content_keys( const char *content_id, const char *market, struct content_key **keys, unsigned *count,
                      char **package_full_name )
{
    static const char challenge[] = "<?xml version=\"1.0\" encoding=\"utf-8\" ?><ClientChallenge xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns=\"http://schemas.microsoft.com/onestore/security/mkms/LicReq/v1\" Version=\"2\"><LicenseProtocolVersion>5</LicenseProtocolVersion><SigningKeyVersion>1</SigningKeyVersion><ClientVersion>2</ClientVersion></ClientChallenge>";
    char *device_ticket = NULL, *user_ticket = NULL, *puid, *challenge_b64, *cid, *mkt, *ut, *pu, *body, *headers;
    struct buf resp = {0}, xml = {0}, block = {0};
    struct json *j;
    const char *value;
    struct splicense game, dev;
    unsigned char kek[16];
    BOOL have_kek;
    DWORD status;
    HRESULT hr;
    unsigned i;

    *keys = NULL;
    *count = 0;
    if (FAILED(hr = windev_device_ticket( &device_ticket ))) return hr;
    EnterCriticalSection( &account.signin_cs );
    hr = msa_refresh( "service::www.microsoft.com::MBI_SSL", &user_ticket );
    LeaveCriticalSection( &account.signin_cs );
    if (FAILED(hr))
    {
        str_wipe_free( device_ticket );
        return hr;
    }
    puid = account_field( &account.puid );
    if (!*puid)
    {
        free( puid );
        puid = store_get( "puid" );
    }

    challenge_b64 = b64( (const unsigned char *)challenge, sizeof(challenge) - 1, FALSE );
    cid = json_escape( content_id );
    mkt = json_escape( market );
    ut = json_escape( user_ticket );
    pu = json_escape( S(puid) );
    body = strf( "{\"clientChallenge\":\"%s\",\"concurrencyMode\":\"Rude\",\"contentId\":\"%s\","
                 "\"deviceContext\":{\"hardwareManufacturer\":\"Public\",\"hardwareType\":\"Public\",\"mobileOperator\":\"Public\"},"
                 "\"licenseVersion\":4,\"market\":\"%s\",\"needKey\":true,\"keyOnly\":true,"
                 "\"users\":{\"S-1-5-21-0000000000-0000000000-0000000000-1001\":[{\"identityType\":\"Msa\",\"identityValue\":\"%s\","
                 "\"localTicketReference\":\"%s\"}]}}", S(challenge_b64), S(cid), S(mkt), S(ut), S(pu) );
    headers = strf( "Content-Type: application/json\r\nfrom: XboxLicenseManager\r\nAuthorization: %s\r\n"
                    "User-Agent: XboxLm-PC/Microsoft.GamingServices_32.107.4002.0_x64__8wekyb3d8bbwe\r\n", device_ticket );
    free( challenge_b64 );
    free( cid );
    free( mkt );
    str_wipe_free( ut );
    free( pu );
    free( puid );
    status = http_request( L"POST", "https://licensing.mp.microsoft.com/v7.0/licenses/content", headers, body, strlen( body ),
                           &resp );
    str_wipe_free( headers );
    str_wipe_free( body );
    str_wipe_free( user_ticket );
    str_wipe_free( device_ticket );

    j = json_parse( resp.p, resp.n );
    value = json_str( json_get( json_at( json_get( json_get( j, "license" ), "keys" ), 0 ), "value" ) );
    if (status != 200 || !*value)
    {
        TRACE( "windev: licence for %s: HTTP %lu: %.500s\n", content_id, status, buf_str( &resp ) );
        json_free( j );
        buf_free( &resp );
        return status == 200 || status == 403 || status == 400 ? XA_E_MSA : XA_E_NETWORK;
    }
    unb64( value, &xml );
    json_free( j );
    buf_free( &resp );
    {
        char *b = xml_text( buf_str( &xml ), "SPLicenseBlock", NULL );
        unb64( b, &block );
        free( b );
    }
    parse_splicense( (unsigned char *)block.p, block.n, &game );
    EnterCriticalSection( &windev_cs );
    parse_splicense( (unsigned char *)windev.license.p, windev.license.n, &dev );
    have_kek = device_key_from( dev.device_key, dev.device_key_len, kek );
    splicense_free( &dev );
    LeaveCriticalSection( &windev_cs );
    if (!have_kek)
    {
        TRACE( "windev: no device key in the device licence\n" );
        hr = E_FAIL;
        goto done;
    }
    if (game.packed_count && !(*keys = calloc( game.packed_count, sizeof(**keys) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < game.packed_count; i++)
    {
        struct content_key *k = *keys + i;

        memcpy( k->id, game.packed_keys[i].id, 16 );
        if (game.packed_keys[i].wrapped_len > sizeof(k->key) + 8 ||
            !aes_key_unwrap( kek, game.packed_keys[i].wrapped, game.packed_keys[i].wrapped_len, k->key, &k->key_len ))
        {
            TRACE( "windev: a content key does not unwrap\n" );
            SecureZeroMemory( *keys, game.packed_count * sizeof(**keys) );
            free( *keys );
            *keys = NULL;
            hr = E_FAIL;
            goto done;
        }
    }
    *count = game.packed_count;
    if (package_full_name) *package_full_name = str_dup( game.package_full_name );
    TRACE( "windev: licence for %s: %u content keys, package %s\n", content_id, *count, game.package_full_name );
    hr = *count ? S_OK : E_FAIL;
done:
    SecureZeroMemory( kek, sizeof(kek) );
    splicense_free( &game );
    buf_free( &block );
    buf_free( &xml );
    return hr;
}

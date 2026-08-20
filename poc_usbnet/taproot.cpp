#include "taproot.h"

#include "bip340.h"  // tagged_hash
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

namespace taproot {

// ---------------------------------------------------------------------------
// bech32 / bech32m shared machinery (BIP-173 / BIP-350)
// ---------------------------------------------------------------------------
static const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static uint32_t polymod(const uint8_t* values, size_t len) {
  static const uint32_t GEN[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa,
                                  0x3d4233dd, 0x2a1462b3};
  uint32_t chk = 1;
  for (size_t i = 0; i < len; ++i) {
    uint32_t b = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ values[i];
    for (int j = 0; j < 5; ++j)
      if ((b >> j) & 1) chk ^= GEN[j];
  }
  return chk;
}

// data = 5-bit groups (already converted). spec_const: 1 = bech32, 0x2bc830a3 = bech32m.
static String encode5(const char* hrp, const uint8_t* data, size_t dlen,
                      uint32_t spec_const) {
  size_t hlen = strlen(hrp);
  // hrp-expand (2*hlen+1) + data + 6 zero padding for checksum
  size_t vlen = hlen * 2 + 1 + dlen + 6;
  uint8_t* values = (uint8_t*)malloc(vlen);
  if (!values) return "";
  size_t p = 0;
  for (size_t i = 0; i < hlen; ++i) values[p++] = hrp[i] >> 5;
  values[p++] = 0;
  for (size_t i = 0; i < hlen; ++i) values[p++] = hrp[i] & 31;
  memcpy(values + p, data, dlen);
  p += dlen;
  memset(values + p, 0, 6);

  uint32_t pm = polymod(values, vlen) ^ spec_const;
  free(values);

  String out = hrp;
  out += '1';
  for (size_t i = 0; i < dlen; ++i) out += CHARSET[data[i]];
  for (int i = 0; i < 6; ++i) out += CHARSET[(pm >> (5 * (5 - i))) & 31];
  return out;
}

// 8-bit -> 5-bit regroup with padding. Caller frees the result.
static uint8_t* convert8to5(const uint8_t* in, size_t inlen, size_t* outlen) {
  size_t cap = (inlen * 8 + 4) / 5;
  uint8_t* out = (uint8_t*)malloc(cap);
  if (!out) return nullptr;
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < inlen; ++i) {
    acc = (acc << 8) | in[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out[n++] = (acc >> bits) & 31;
    }
  }
  if (bits) out[n++] = (acc << (5 - bits)) & 31;
  *outlen = n;
  return out;
}

String bech32Encode(const char* hrp, const uint8_t* data, size_t len) {
  size_t dlen;
  uint8_t* d5 = convert8to5(data, len, &dlen);
  if (!d5) return "";
  String s = encode5(hrp, d5, dlen, 1);
  free(d5);
  return s;
}

// ---------------------------------------------------------------------------
// P2TR address
// ---------------------------------------------------------------------------
static bool lift_x(mbedtls_ecp_group* grp, const uint8_t x_bytes[32],
                   mbedtls_ecp_point* P) {
  mbedtls_mpi x, y2, y, e, seven;
  mbedtls_mpi_init(&x); mbedtls_mpi_init(&y2); mbedtls_mpi_init(&y);
  mbedtls_mpi_init(&e); mbedtls_mpi_init(&seven);

  bool ok = false;
  if (mbedtls_mpi_read_binary(&x, x_bytes, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&x, &grp->P) >= 0) goto done;

  if (mbedtls_mpi_lset(&seven, 7) != 0) goto done;
  if (mbedtls_mpi_mul_mpi(&y2, &x, &x) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&y2, &y2, &grp->P) != 0) goto done;
  if (mbedtls_mpi_mul_mpi(&y2, &y2, &x) != 0) goto done;
  if (mbedtls_mpi_add_mpi(&y2, &y2, &seven) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&y2, &y2, &grp->P) != 0) goto done;

  if (mbedtls_mpi_add_int(&e, &grp->P, 1) != 0) goto done;
  if (mbedtls_mpi_shift_r(&e, 2) != 0) goto done;
  if (mbedtls_mpi_exp_mod(&y, &y2, &e, &grp->P, nullptr) != 0) goto done;

  {
    mbedtls_mpi chk;
    mbedtls_mpi_init(&chk);
    int r = mbedtls_mpi_mul_mpi(&chk, &y, &y);
    if (r == 0) r = mbedtls_mpi_mod_mpi(&chk, &chk, &grp->P);
    bool on_curve = (r == 0) && (mbedtls_mpi_cmp_mpi(&chk, &y2) == 0);
    mbedtls_mpi_free(&chk);
    if (!on_curve) goto done;
  }

  if (mbedtls_mpi_get_bit(&y, 0) == 1) {
    if (mbedtls_mpi_sub_mpi(&y, &grp->P, &y) != 0) goto done;
  }

  if (mbedtls_mpi_copy(&P->MBEDTLS_PRIVATE(X), &x) != 0) goto done;
  if (mbedtls_mpi_copy(&P->MBEDTLS_PRIVATE(Y), &y) != 0) goto done;
  if (mbedtls_mpi_lset(&P->MBEDTLS_PRIVATE(Z), 1) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&x); mbedtls_mpi_free(&y2); mbedtls_mpi_free(&y);
  mbedtls_mpi_free(&e); mbedtls_mpi_free(&seven);
  return ok;
}

bool tweakedXOnly(const uint8_t xonly[32], uint8_t out_x[32]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point P, Q;
  mbedtls_mpi t, one;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&P);
  mbedtls_ecp_point_init(&Q);
  mbedtls_mpi_init(&t); mbedtls_mpi_init(&one);

  bool ok = false;
  uint8_t tweak[32];

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (!lift_x(&grp, xonly, &P)) goto done;
  bip340::tagged_hash("TapTweak", xonly, 32, tweak);
  if (mbedtls_mpi_read_binary(&t, tweak, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&t, &grp.N) >= 0) goto done;
  if (mbedtls_mpi_lset(&one, 1) != 0) goto done;
  if (mbedtls_ecp_muladd(&grp, &Q, &one, &P, &t, &grp.G) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&Q.MBEDTLS_PRIVATE(Z), 0) == 0) goto done;
  if (mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), out_x, 32) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&t); mbedtls_mpi_free(&one);
  mbedtls_ecp_point_free(&P);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

String addressFromProgram(int witver, const uint8_t* prog, size_t len) {
  if (witver < 0 || witver > 16 || len < 2 || len > 40) return "";
  size_t d5len;
  uint8_t* d5 = convert8to5(prog, len, &d5len);
  if (!d5) return "";
  uint8_t* full = (uint8_t*)malloc(d5len + 1);
  if (!full) { free(d5); return ""; }
  full[0] = (uint8_t)witver;
  memcpy(full + 1, d5, d5len);
  String addr = encode5("bc", full, d5len + 1, witver == 0 ? 1 : 0x2bc830a3);
  free(d5);
  free(full);
  return addr;
}

String addressFromXonly(const uint8_t xonly[32]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point P, Q;
  mbedtls_mpi t, one;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&P);
  mbedtls_ecp_point_init(&Q);
  mbedtls_mpi_init(&t); mbedtls_mpi_init(&one);

  String addr = "";
  uint8_t tweak[32], out_x[32];

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (!lift_x(&grp, xonly, &P)) goto done;

  bip340::tagged_hash("TapTweak", xonly, 32, tweak);
  if (mbedtls_mpi_read_binary(&t, tweak, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&t, &grp.N) >= 0) goto done;  // astronomically unlikely
  if (mbedtls_mpi_lset(&one, 1) != 0) goto done;

  // Q = 1*P + t*G
  if (mbedtls_ecp_muladd(&grp, &Q, &one, &P, &t, &grp.G) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&Q.MBEDTLS_PRIVATE(Z), 0) == 0) goto done;
  if (mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), out_x, 32) != 0) goto done;

  {
    // segwit v1: data = [1] + convertbits(out_x)
    size_t d5len;
    uint8_t* d5 = convert8to5(out_x, 32, &d5len);
    if (!d5) goto done;
    uint8_t* full = (uint8_t*)malloc(d5len + 1);
    if (!full) { free(d5); goto done; }
    full[0] = 1;
    memcpy(full + 1, d5, d5len);
    addr = encode5("bc", full, d5len + 1, 0x2bc830a3);
    free(d5);
    free(full);
  }

done:
  mbedtls_mpi_free(&t); mbedtls_mpi_free(&one);
  mbedtls_ecp_point_free(&P);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return addr;
}

// ---------------------------------------------------------------------------
// LNURL
// ---------------------------------------------------------------------------
String lnurlFromLud16(const String& lud16) {
  String s = lud16;
  s.trim();
  if (s.length() == 0) return "";

  String low = s;
  low.toLowerCase();
  if (low.startsWith("lnurl1")) {
    low.toUpperCase();
    return low;  // already lud06
  }

  int at = s.indexOf('@');
  if (at <= 0 || at == (int)s.length() - 1) return "";
  String user = s.substring(0, at);
  String domain = s.substring(at + 1);
  String url = "https://" + domain + "/.well-known/lnurlp/" + user;

  String enc = bech32Encode("lnurl", (const uint8_t*)url.c_str(), url.length());
  enc.toUpperCase();  // uppercase QRs use the denser alphanumeric mode
  return enc;
}

}  // namespace taproot

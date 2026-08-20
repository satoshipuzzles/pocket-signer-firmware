#include "bech32.h"

namespace nostrocrypto {

static const char kCharset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static uint32_t polymod(const uint8_t* v, size_t n) {
  uint32_t chk = 1;
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v[i];
    if (b & 0x01) chk ^= 0x3b6a57b2;
    if (b & 0x02) chk ^= 0x26508e6d;
    if (b & 0x04) chk ^= 0x1ea119fa;
    if (b & 0x08) chk ^= 0x3d4233dd;
    if (b & 0x10) chk ^= 0x2a1462b3;
  }
  return chk;
}

// Convert a 32-byte payload (256 bits) into 52 groups of 5 bits, MSB-first.
// 256 / 5 = 51.2 → 52 groups (last group padded with 4 zero bits).
static void convert_32bytes_to_5bit(const uint8_t in[32], uint8_t out[52]) {
  uint32_t acc = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < 32; ++i) {
    acc = (acc << 8) | in[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out[o++] = (acc >> bits) & 0x1f;
    }
  }
  if (bits > 0) {
    // Pad the final group with zero bits on the right.
    out[o++] = (acc << (5 - bits)) & 0x1f;
  }
  // o should equal 52 here.
}

// Reverse charset: char → 5-bit value, or -1 if not in alphabet.
static int charset_index(char c) {
  if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';   // bech32 is lowercase, but tolerate upper
  for (int i = 0; i < 32; ++i) if (kCharset[i] == c) return i;
  return -1;
}

bool bech32_decode_32(const String& s, uint8_t out[32], char hrp_out[16]) {
  if (s.length() < 8) return false;

  // The separator is the last '1' in the string, and the HRP is everything
  // before it (bech32 spec disallows more than one '1' *only* inside the HRP
  // itself; the data part may not contain '1' since '1' is not in the
  // charset — but we split on last '1' to be safe).
  int sep = -1;
  for (int i = (int)s.length() - 1; i >= 0; --i) {
    if (s[i] == '1') { sep = i; break; }
  }
  if (sep < 1) return false;

  const int hrp_len = sep;
  if (hrp_len > 8) return false;
  const int data_len = (int)s.length() - sep - 1;
  if (data_len < 6) return false;   // must have at least the 6-char checksum

  // Copy HRP
  if (hrp_out) {
    for (int i = 0; i < hrp_len; ++i) {
      char c = s[i];
      if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
      hrp_out[i] = c;
    }
    hrp_out[hrp_len] = 0;
  }

  // Build polymod input
  uint8_t buf[128];
  size_t bi = 0;
  for (int i = 0; i < hrp_len; ++i) {
    char c = s[i]; if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    buf[bi++] = (uint8_t)(c >> 5);
  }
  buf[bi++] = 0;
  for (int i = 0; i < hrp_len; ++i) {
    char c = s[i]; if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    buf[bi++] = (uint8_t)(c & 0x1f);
  }
  // Decode data chars (payload + checksum)
  for (int i = 0; i < data_len; ++i) {
    int v = charset_index(s[sep + 1 + i]);
    if (v < 0) return false;
    buf[bi++] = (uint8_t)v;
  }

  if (polymod(buf, bi) != 1) return false;

  // Convert first (data_len - 6) 5-bit groups back to 8-bit bytes.
  const int payload_5bit_len = data_len - 6;
  // For a 32-byte payload we always get 52 5-bit groups (256/5 → 52, last has 4 padding bits)
  if (payload_5bit_len != 52) return false;

  uint32_t acc = 0;
  int bits = 0;
  size_t out_i = 0;
  for (int i = 0; i < payload_5bit_len; ++i) {
    uint8_t g = buf[hrp_len + 1 + hrp_len + i];
    acc = (acc << 5) | g;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (out_i >= 32) return false;
      out[out_i++] = (uint8_t)((acc >> bits) & 0xff);
    }
  }
  if (out_i != 32) return false;
  // Padding bits must be zero for a canonical bech32 payload.
  if (bits != 0 && (acc & ((1u << bits) - 1)) != 0) return false;

  return true;
}

int parse_private_key(const String& raw_in, uint8_t out[32], char hrp_out[16]) {
  // Trim whitespace + optional quotes.
  String raw = raw_in;
  raw.trim();
  while (raw.length() && (raw[0] == '"' || raw[0] == '\'')) raw.remove(0, 1);
  while (raw.length() && (raw[raw.length()-1] == '"' || raw[raw.length()-1] == '\'')) {
    raw.remove(raw.length() - 1, 1);
  }
  if (raw.length() < 8) return -1;

  // Try bech32 first if it starts with nsec/npub prefix
  if (raw.startsWith("nsec1") || raw.startsWith("npub1") ||
      raw.startsWith("NSEC1") || raw.startsWith("NPUB1")) {
    return bech32_decode_32(raw, out, hrp_out) ? 0 : -2;
  }

  // Fallback: 64-char hex.
  if (raw.length() != 64) return -3;
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; i < 32; ++i) {
    int hi = hex_val(raw[2*i]), lo = hex_val(raw[2*i+1]);
    if (hi < 0 || lo < 0) return -3;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  if (hrp_out) { hrp_out[0] = 'h'; hrp_out[1] = 'e'; hrp_out[2] = 'x'; hrp_out[3] = 0; }
  return 0;
}

String bech32_encode_32(const char* hrp, const uint8_t payload[32]) {
  if (!hrp || !payload) return String();
  const size_t hrp_len = strlen(hrp);
  if (hrp_len == 0 || hrp_len > 8) return String();

  // Build the checksum input: hrp_expanded + data + 6 zero placeholder bytes.
  uint8_t data[52];
  convert_32bytes_to_5bit(payload, data);

  // hrp_expanded + separator + data + 6-byte checksum placeholder. Max size:
  //   2 * hrp_len (up to 16) + 1 (separator) + 52 (data) + 6 (checksum) = 75.
  // Round up for headroom.
  uint8_t buf[96];
  size_t bi = 0;
  for (size_t i = 0; i < hrp_len; ++i) buf[bi++] = (uint8_t)(hrp[i] >> 5);
  buf[bi++] = 0;
  for (size_t i = 0; i < hrp_len; ++i) buf[bi++] = (uint8_t)(hrp[i] & 0x1f);
  for (size_t i = 0; i < 52; ++i)     buf[bi++] = data[i];
  for (size_t i = 0; i < 6;  ++i)     buf[bi++] = 0;

  uint32_t chk = polymod(buf, bi) ^ 1;
  uint8_t cs[6];
  for (int i = 0; i < 6; ++i) cs[i] = (chk >> (5 * (5 - i))) & 0x1f;

  String out;
  out.reserve(hrp_len + 1 + 52 + 6);
  for (size_t i = 0; i < hrp_len; ++i) out += hrp[i];
  out += '1';
  for (size_t i = 0; i < 52; ++i) out += kCharset[data[i]];
  for (size_t i = 0; i < 6;  ++i) out += kCharset[cs[i]];
  return out;
}

} // namespace nostrocrypto

// Types for the on-device sign-request queue. Lives in a header so the
// Arduino preprocessor's auto-generated prototypes (which get hoisted above
// everything in the .ino) can see them.
#pragma once
#include <Arduino.h>

enum Screen : uint8_t { SCR_IDLE, SCR_ACCOUNTS };

enum ReqStatus : uint8_t { REQ_NONE, REQ_PENDING, REQ_APPROVED, REQ_DECLINED };

struct SignRequest {
  uint32_t  id = 0;
  ReqStatus status = REQ_NONE;
  int       kind = -1;
  String    content;
  String    raw;          // original JSON body (re-parsed at signing time)
  bool      auto_signed = false;
  bool      polled = false;      // extension has seen the final result
  uint32_t  born_ms = 0;
  uint32_t  done_ms = 0;
  // filled at approval:
  String    event_id_hex;
  String    sig_hex;
  String    pubkey_hex;
  uint32_t  created_at = 0;
};

constexpr int QLEN = 8;

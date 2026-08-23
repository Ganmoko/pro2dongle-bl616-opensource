// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRO2_VENDOR_REPLY_MAX 80u
#define PRO2_DIAGNOSTIC_QUERY_MAGIC "P2DG"
#define PRO2_DIAGNOSTIC_QUERY_MAGIC_SIZE 4u

size_t vendor_protocol_build_reply(const uint8_t *command, size_t command_len,
                                   uint8_t *reply, size_t reply_max);
bool vendor_protocol_is_diagnostic_query(const uint8_t *command,
                                         size_t command_len);

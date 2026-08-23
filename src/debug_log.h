// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdio.h>

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#define LOG_ERR(...) do { if (LOG_LEVEL >= 0) printf("[E] " __VA_ARGS__); } while (0)
#define LOG_WRN(...) do { if (LOG_LEVEL >= 1) printf("[W] " __VA_ARGS__); } while (0)
#define LOG_INF(...) do { if (LOG_LEVEL >= 2) printf("[I] " __VA_ARGS__); } while (0)
#define LOG_DBG(...) do { if (LOG_LEVEL >= 3) printf("[D] " __VA_ARGS__); } while (0)


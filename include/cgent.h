/*
 * cgent.h — Umbrella header for the cgent library
 *
 * Pure C AI agent with support for:
 *   - Anthropic, OpenAI, DeepSeek APIs
 *   - Tools / MCP / Skills / agent.md
 *   - Subagents and agent teams
 *
 * Zero external dependencies (system OpenSSL + libc only).
 */
#ifndef CGENT_H
#define CGENT_H

#include "platform.h"
#include "json.h"
#include "config.h"
#include "network.h"
#include "protocol.h"
#include "core.h"
#include "tools.h"
#include "skills.h"

/* Library version */
#define CGENT_VERSION_MAJOR 0
#define CGENT_VERSION_MINOR 1
#define CGENT_VERSION_PATCH 0
#define CGENT_VERSION "0.1.0"

/* Maximum sizes */
#define CGENT_MAX_URL        8192
#define CGENT_MAX_HEADER     4096
#define CGENT_MAX_BODY       (16 * 1024 * 1024)  /* 16 MB */
#define CGENT_MAX_LINE       65536
#define CGENT_MAX_TOOLS      256
#define CGENT_MAX_MESSAGES   1024

#endif /* CGENT_H */

/*
 * args.c — CLI argument parsing
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

cli_args_t cli_parse(int argc, char **argv) {
    cli_args_t args = {
        .provider     = NULL,
        .model        = NULL,
        .api_key      = NULL,
        .base_url     = NULL,
        .query        = NULL,
        .agent_dir    = NULL,
        .config_path  = NULL,
        .temperature  = 0.7,
        .max_tokens   = 4096,
        .stream       = true,
        .verbose      = false,
        .help         = false,
        .version      = false,
    };

    static struct option long_opts[] = {
        {"provider",    required_argument, 0, 'p'},
        {"model",       required_argument, 0, 'm'},
        {"api-key",     required_argument, 0, 'k'},
        {"base-url",    required_argument, 0, 'u'},
        {"query",       required_argument, 0, 'q'},
        {"agent",       required_argument, 0, 'a'},
        {"temperature", required_argument, 0, 't'},
        {"max-tokens",  required_argument, 0, 'M'},
        {"no-stream",   no_argument,       0, 'n'},
        {"config",      required_argument, 0, 'c'},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:m:k:u:q:a:t:M:nc:vhV",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': args.provider   = optarg; break;
        case 'm': args.model      = optarg; break;
        case 'k': args.api_key    = optarg; break;
        case 'u': args.base_url   = optarg; break;
        case 'q': args.query      = optarg; break;
        case 'a': args.agent_dir  = optarg; break;
        case 't': args.temperature = atof(optarg); break;
        case 'M': args.max_tokens  = atoi(optarg); break;
        case 'n': args.stream      = false; break;
        case 'c': args.config_path = optarg; break;
        case 'v': args.verbose     = true; break;
        case 'h': args.help        = true; break;
        case 'V': args.version     = true; break;
        default:
            fprintf(stderr, "Try --help for usage.\n");
            exit(1);
        }
    }

    return args;
}

void config_apply_cli(cgent_config_t *cfg, const cli_args_t *args) {
    if (!cfg || !args) return;
    if (args->provider)    cfg->provider    = strdup(args->provider);
    if (args->model)       cfg->model       = strdup(args->model);
    if (args->api_key)     cfg->api_key     = strdup(args->api_key);
    if (args->base_url)    cfg->base_url    = strdup(args->base_url);
    if (args->agent_dir)   cfg->agent_dir   = strdup(args->agent_dir);
    if (args->config_path) cfg->config_path = strdup(args->config_path);
    cfg->temperature = args->temperature;
    cfg->max_tokens  = args->max_tokens;
    cfg->stream      = args->stream;
    cfg->verbose     = args->verbose;
}

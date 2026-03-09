/**
 * @file cli.c
 * @brief Command-line interface implementation
 *
 * Implementation of the CLI interface defined in cli.h. This module provides
 * both command-line and interactive modes for the weather client application.
 * It handles argument parsing, command execution, JSON output formatting,
 * and provides a REPL-style interactive interface.
 *
 * See cli.h for detailed API documentation.
 */
#include "cli.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_INVALID_ARGS 1  ///< Invalid command-line arguments
#define EXIT_NETWORK_ERROR 2 ///< Network communication error
#define EXIT_SERVER_ERROR 3  ///< Server/API error

static void print_json(json_t* data);
static int  parse_double(const char* str, double* out);
static void process_command(WeatherClient* client, char* line);

void cli_print_usage(const char* prog_name) {
    printf("\n--==> Just Weather Client <==--\n\n");

    printf("==================================================\n");
    printf("Usage:\n\n");
    printf("  %s current <lat> <lon>\n", prog_name);
    printf("  %s weather <city> [country] [region]\n", prog_name);
    printf("  %s cities <query>\n", prog_name);
    printf("  %s homepage\n", prog_name);
    printf("  %s echo\n", prog_name);
    printf("  %s getplan <city> <price>    # price: SE1, SE2, SE3 or SE4\n",
           prog_name);
    printf("  %s clear-cache\n", prog_name);
    printf("  %s interactive    # Enter interactive mode\n", prog_name);
    printf("==================================================\n\n");

    printf("==================================================\n");
    printf("Examples:\n\n");
    printf("  %s current 59.33 18.07\n", prog_name);
    printf("  %s weather Stockholm SE\n", prog_name);
    printf("  %s cities Stock\n", prog_name);
    printf("  %s interactive\n", prog_name);
    printf("==================================================\n");
}

void cli_interactive_mode() {
    // Initialize WeatherClient
    char server_address[256];
    int  server_port;
    printf("\nSpecify backend\n");
    printf("Address to server:\n");
    int ref = scanf("%s", server_address);
    if (ref != 1) {
        fprintf(stderr, "invalid server address input\n");
        return;
    }
    printf("Server port:\n");
    ref = scanf("%d", &server_port);
    if (ref != 1) {
        fprintf(stderr, "invalid server port input\n");
        return;
    }
    WeatherClient* client = weather_client_create(server_address, server_port);
    if (!client) {
        fprintf(stderr, "Failed to create weather client\n");
    }
    char* error = NULL;

    // echo to see if connection is ok
    json_t* result = weather_client_echo(client, &error);
    if (!result) {
        fprintf(stderr, "Error: %s\n", error ? error : "Unknown error");
        free(error);
        return;
    }
    char line[1024];

    printf("Just Weather Interactive Client\n");
    printf("Connected to: localhost:10680\n");
    printf("Type 'help' for commands, 'quit' to exit\n\n");

    while (1) {
        printf("just-weather> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        if (strlen(line) == 0) {
            continue;
        }

        if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0 ||
            strcmp(line, "exit") == 0) {
            printf("Goodbye!\n");
            break;
        }

        if (strcmp(line, "help") == 0) {
            printf("\nAvailable commands:\n");
            printf("  current <lat> <lon>             - Get current weather by "
                   "coordinates\n");
            printf("  weather <city> [country]        - Get weather by city "
                   "name\n");
            printf("  cities <query>                  - Search for cities\n");
            printf("  homepage                        - Get API homepage\n");
            printf("  echo                            - Test echo endpoint\n");
            printf("  getplan <city> <price>         - Get energy plan "
                   "(price: SE1-SE4)\n");
            printf("  clear-cache                     - Clear client cache\n");
            printf("  help                            - Show this help\n");
            printf("  quit / exit                     - Exit interactive "
                   "mode\n\n");
            printf("Examples:\n");
            printf("  current 59.33 18.07\n");
            printf("  weather Kyiv UA\n");
            printf("  cities London\n\n");
            continue;
        }

        process_command(client, line);
    }
}

int cli_execute_command(WeatherClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        return EXIT_INVALID_ARGS;
    }

    const char* command = argv[1];
    json_t*     result  = NULL;
    char*       error   = NULL;

    if (strcmp(command, "current") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s current <lat> <lon>\n", argv[0]);
            return EXIT_INVALID_ARGS;
        }

        double lat, lon;
        if (!parse_double(argv[2], &lat) || !parse_double(argv[3], &lon)) {
            fprintf(stderr, "Invalid coordinates\n");
            return EXIT_INVALID_ARGS;
        }

        result = weather_client_get_current(client, lat, lon, &error);

    } else if (strcmp(command, "weather") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s weather <city> [country] [region]\n",
                    argv[0]);
            return EXIT_INVALID_ARGS;
        }

        const char* city    = argv[2];
        const char* country = argc > 3 ? argv[3] : NULL;
        const char* region  = argc > 4 ? argv[4] : NULL;

        result = weather_client_get_weather_by_city(client, city, country,
                                                    region, &error);

    } else if (strcmp(command, "cities") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s cities <query>\n", argv[0]);
            return EXIT_INVALID_ARGS;
        }

        const char* query = argv[2];
        result            = weather_client_search_cities(client, query, &error);

    } else if (strcmp(command, "homepage") == 0) {
        result = weather_client_get_homepage(client, &error);

    } else if (strcmp(command, "echo") == 0) {
        result = weather_client_echo(client, &error);

    } else if (strcmp(command, "getplan") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s getplan <city> <price>\n", argv[0]);
            fprintf(stderr, "Price must be SE1, SE2, SE3 or SE4\n");
            return EXIT_INVALID_ARGS;
        }
        result = weather_client_get_plan(client, argv[2], argv[3], &error);

    } else if (strcmp(command, "clear-cache") == 0) {
        weather_client_clear_cache(client);
        printf("Cache cleared\n");
        return 0;

    } else if (strcmp(command, "interactive") == 0 ||
               strcmp(command, "-i") == 0) {
        return -1;

    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return EXIT_INVALID_ARGS;
    }

    if (!result) {
        fprintf(stderr, "Error: %s\n", error ? error : "Unknown error");
        free(error);
        return EXIT_SERVER_ERROR;
    }

    print_json(result);
    json_decref(result);
    free(error);

    return 0;
}

static void print_json(json_t* data) {
    char* json_str = json_dumps(data, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    if (json_str) {
        printf("%s\n", json_str);
        free(json_str);
    }
}

static int parse_double(const char* str, double* out) {
    if (!str || !out) {
        return 0;
    }

    char*  endptr;
    double value = strtod(str, &endptr);

    if (endptr == str || *endptr != '\0') {
        return 0;
    }

    *out = value;
    return 1;
}

static void process_command(WeatherClient* client, char* line) {
    char* cmd = strtok(line, " ");
    if (!cmd) {
        return;
    }

    json_t* result = NULL;
    char*   error  = NULL;

    if (strcmp(cmd, "current") == 0) {
        char* lat_str = strtok(NULL, " ");
        char* lon_str = strtok(NULL, " ");

        if (!lat_str || !lon_str) {
            printf("Error: Usage: current <lat> <lon>\n");
            return;
        }

        double lat, lon;
        if (!parse_double(lat_str, &lat) || !parse_double(lon_str, &lon)) {
            printf("Error: Invalid coordinates\n");
            return;
        }

        result = weather_client_get_current(client, lat, lon, &error);

    } else if (strcmp(cmd, "weather") == 0) {
        char* city    = strtok(NULL, " ");
        char* country = strtok(NULL, " ");
        char* region  = strtok(NULL, " ");

        if (!city) {
            printf("Error: Usage: weather <city> [country] [region]\n");
            return;
        }

        result = weather_client_get_weather_by_city(client, city, country,
                                                    region, &error);

    } else if (strcmp(cmd, "cities") == 0) {
        char* query = strtok(NULL, "");
        if (query && *query == ' ') {
            query++;
        }

        if (!query || strlen(query) == 0) {
            printf("Error: Usage: cities <query>\n");
            return;
        }

        result = weather_client_search_cities(client, query, &error);

    } else if (strcmp(cmd, "homepage") == 0) {
        result = weather_client_get_homepage(client, &error);

    } else if (strcmp(cmd, "echo") == 0) {
        result = weather_client_echo(client, &error);

    } else if (strcmp(cmd, "getplan") == 0) {
        char* city  = strtok(NULL, " ");
        char* price = strtok(NULL, " ");
        if (!city || !price) {
            printf("Error: Usage: getplan <city> <price>  (price: SE1-SE4)\n");
            return;
        }
        result = weather_client_get_plan(client, city, price, &error);

    } else if (strcmp(cmd, "clear-cache") == 0) {
        weather_client_clear_cache(client);
        printf("Cache cleared\n");
        return;

    } else {
        printf("Error: Unknown command '%s'. Type 'help' for available "
               "commands.\n",
               cmd);
        return;
    }

    if (!result) {
        printf("Error: %s\n", error ? error : "Unknown error");
        free(error);
        return;
    }

    print_json(result);
    json_decref(result);
    free(error);
}

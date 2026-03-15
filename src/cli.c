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
static void print_plan_table(json_t* data);
static void print_weather_table(json_t* data);
static void print_cities_table(json_t* data);
static int  parse_double(const char* str, double* out);
static void process_command(WeatherClient* client, char* line);

/* Extra bytes a UTF-8 string has vs. its display width.
 * printf width counts bytes, not codepoints — add this to the desired
 * display width when calling printf("%*s", ...) with non-ASCII strings. */
static int utf8_extra_bytes(const char* s) {
    int extra = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0xF0)
            extra += 3;
        else if (c >= 0xE0)
            extra += 2;
        else if (c >= 0xC0)
            extra += 1;
        s++;
    }
    return extra;
}

typedef enum { FMT_JSON = 0, FMT_WEATHER, FMT_CITIES, FMT_PLAN } OutputFmt;

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
    {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF)
            ;
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
            printf("  cities London\n");
            printf("  getplan Stockholm SE3\n\n");
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
    OutputFmt   fmt     = FMT_JSON;

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
        fmt    = FMT_WEATHER;

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
        fmt    = FMT_WEATHER;

    } else if (strcmp(command, "cities") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s cities <query>\n", argv[0]);
            return EXIT_INVALID_ARGS;
        }

        const char* query = argv[2];
        result            = weather_client_search_cities(client, query, &error);
        fmt               = FMT_CITIES;

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
        fmt    = FMT_PLAN;

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

    switch (fmt) {
    case FMT_WEATHER:
        print_weather_table(result);
        break;
    case FMT_CITIES:
        print_cities_table(result);
        break;
    case FMT_PLAN:
        print_plan_table(result);
        break;
    default:
        print_json(result);
        break;
    }
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

static void print_plan_table(json_t* data) {
    json_t* d = data;

    json_t* city_j      = json_object_get(d, "city");
    json_t* zone_j      = json_object_get(d, "price_zone");
    json_t* lat_j       = json_object_get(d, "latitude");
    json_t* lon_j       = json_object_get(d, "longitude");
    json_t* start_j     = json_object_get(d, "start_time");
    json_t* slots_j     = json_object_get(d, "slots_total");
    json_t* summary_j   = json_object_get(d, "summary");
    json_t* decisions_j = json_object_get(d, "decisions");

    if (!city_j || !zone_j || !summary_j || !decisions_j) {
        print_json(data);
        return;
    }

    /* Header */
    printf("\nCity: %-20s  Zone: %s\n", json_string_value(city_j),
           json_string_value(zone_j));
    if (lat_j && lon_j) {
        printf("Lat: %.4f    Lon: %.4f\n", json_real_value(lat_j),
               json_real_value(lon_j));
    }
    if (start_j) {
        printf("Start: %s", json_string_value(start_j));
        if (slots_j) {
            printf("    Slots: %lld", (long long)json_integer_value(slots_j));
        }
        printf("\n");
    }

    /* Summary table */
    printf("\n=== Summary ===\n");
    printf("%-16s | %-16s | %-16s | %-16s\n", "Buy Elec (kWh)",
           "Direct Use (kWh)", "Charge Bat (kWh)", "Sell Excess (kWh)");
    printf("%-16s-+-%-16s-+-%-16s-+-%-16s\n", "----------------",
           "----------------", "----------------", "----------------");
    printf("%16.3f | %16.3f | %16.3f | %16.3f\n",
           json_real_value(json_object_get(summary_j, "buy_electricity")),
           json_real_value(json_object_get(summary_j, "direct_use")),
           json_real_value(json_object_get(summary_j, "charge_battery")),
           json_real_value(json_object_get(summary_j, "sell_excess")));

    /* Decisions table */
    size_t n = json_array_size(decisions_j);
    printf("\n=== Decisions (%zu x 15 min) ===\n", n);
    printf("%3s | %10s | %8s | %6s | %7s | %7s | %7s | %7s\n", "#",
           "Price(SEK)", "Temp(C)", "Sun", "Buy", "Direct", "Charge", "Sell");
    printf("----+------------+----------+--------+---------+---------+---------"
           "+---------\n");

    for (size_t i = 0; i < n; i++) {
        json_t* slot = json_array_get(decisions_j, i);
        json_t* inv  = json_object_get(slot, "input_variables");
        json_t* out  = json_object_get(slot, "output");
        if (!inv || !out)
            continue;

        printf(
            "%3zu | %10.3f | %8.2f | %6.2f | %7.3f | %7.3f | %7.3f | %7.3f\n",
            i + 1, json_real_value(json_object_get(inv, "elpris")),
            json_real_value(json_object_get(inv, "temperature")),
            json_real_value(json_object_get(inv, "sun_intensity")),
            json_real_value(json_object_get(out, "buy_electricity")),
            json_real_value(json_object_get(out, "direct_use")),
            json_real_value(json_object_get(out, "charge_battery")),
            json_real_value(json_object_get(out, "sell_excess")));
    }
    printf("\n");
}

static void print_weather_table(json_t* data) {
    json_t* d = json_object_get(data, "data");
    if (!d || !json_is_object(d)) {
        print_json(data);
        return;
    }

    json_t* loc = json_object_get(d, "location");
    json_t* cw  = json_object_get(d, "current_weather");
    if (!cw) {
        print_json(data);
        return;
    }

    /* Location header */
    if (loc) {
        json_t* name_j    = json_object_get(loc, "name");
        json_t* country_j = json_object_get(loc, "country");
        json_t* cc_j      = json_object_get(loc, "country_code");
        json_t* region_j  = json_object_get(loc, "region");
        json_t* lat_j     = json_object_get(loc, "latitude");
        json_t* lon_j     = json_object_get(loc, "longitude");
        json_t* pop_j     = json_object_get(loc, "population");
        json_t* tz_j      = json_object_get(loc, "timezone");

        if (name_j && json_is_string(name_j)) {
            printf("\nLocation: %s", json_string_value(name_j));
            if (country_j && json_is_string(country_j)) {
                printf(", %s", json_string_value(country_j));
            }
            if (cc_j && json_is_string(cc_j)) {
                printf(" (%s)", json_string_value(cc_j));
            }
            if (region_j && json_is_string(region_j)) {
                printf("  Region: %s", json_string_value(region_j));
            }
            printf("\n");
        }
        if (lat_j && lon_j) {
            printf("Lat: %.4f  Lon: %.4f", json_real_value(lat_j),
                   json_real_value(lon_j));
            if (pop_j && json_integer_value(pop_j) > 0) {
                printf("  Pop: %lld", (long long)json_integer_value(pop_j));
            }
            if (tz_j && json_is_string(tz_j)) {
                printf("  TZ: %s", json_string_value(tz_j));
            }
            printf("\n");
        }
    }

    json_t* time_j = json_object_get(cw, "time");
    if (time_j && json_is_string(time_j)) {
        printf("Time: %s\n", json_string_value(time_j));
    }

    /* Weather table — numeric columns avoid UTF-8 byte/char mismatch */
    json_t* temp_j     = json_object_get(cw, "temperature");
    json_t* temp_u_j   = json_object_get(cw, "temperature_unit");
    json_t* humid_j    = json_object_get(cw, "humidity");
    json_t* press_j    = json_object_get(cw, "pressure");
    json_t* wspeed_j   = json_object_get(cw, "windspeed");
    json_t* wspeed_u_j = json_object_get(cw, "windspeed_unit");
    json_t* wdir_j     = json_object_get(cw, "wind_direction_name");
    json_t* precip_j   = json_object_get(cw, "precipitation");
    json_t* precip_u_j = json_object_get(cw, "precipitation_unit");
    json_t* desc_j     = json_object_get(cw, "weather_description");

    const char* temp_unit   = temp_u_j && json_is_string(temp_u_j)
                                  ? json_string_value(temp_u_j)
                                  : "?";
    const char* speed_unit  = wspeed_u_j && json_is_string(wspeed_u_j)
                                  ? json_string_value(wspeed_u_j)
                                  : "?";
    const char* precip_unit = precip_u_j && json_is_string(precip_u_j)
                                  ? json_string_value(precip_u_j)
                                  : "?";

    /* Build dynamic header using actual units */
    char temp_hdr[24], speed_hdr[24], precip_hdr[24];
    snprintf(temp_hdr, sizeof(temp_hdr), "Temp(%s)", temp_unit);
    snprintf(speed_hdr, sizeof(speed_hdr), "Speed(%s)", speed_unit);
    snprintf(precip_hdr, sizeof(precip_hdr), "Precip(%s)", precip_unit);

    printf("\n%*s | %8s | %10s | %*s | %-16s | %*s | %s\n",
           10 + utf8_extra_bytes(temp_hdr), temp_hdr, "Humid(%)", "Press(hPa)",
           11 + utf8_extra_bytes(speed_hdr), speed_hdr, "Direction",
           10 + utf8_extra_bytes(precip_hdr), precip_hdr, "Condition");
    printf("-----------+----------+------------+-------------+-----------------"
           "-+------------+--------------------\n");

    printf("%10.1f | %8.0f | %10.0f | %11.1f | %-16s | %10.1f | %s\n",
           temp_j ? json_real_value(temp_j) : 0.0,
           humid_j ? json_real_value(humid_j) : 0.0,
           press_j ? json_real_value(press_j) : 0.0,
           wspeed_j ? json_real_value(wspeed_j) : 0.0,
           wdir_j && json_is_string(wdir_j) ? json_string_value(wdir_j) : "-",
           precip_j ? json_real_value(precip_j) : 0.0,
           desc_j && json_is_string(desc_j) ? json_string_value(desc_j) : "-");
    printf("\n");
}

static void print_cities_table(json_t* data) {
    json_t* d = json_object_get(data, "data");
    if (!d || !json_is_object(d)) {
        print_json(data);
        return;
    }

    json_t* query_j  = json_object_get(d, "query");
    json_t* count_j  = json_object_get(d, "count");
    json_t* cities_j = json_object_get(d, "cities");
    if (!cities_j || !json_is_array(cities_j)) {
        print_json(data);
        return;
    }

    printf("\nSearch: \"%s\"  Results: %lld\n\n",
           query_j && json_is_string(query_j) ? json_string_value(query_j) : "",
           count_j ? (long long)json_integer_value(count_j)
                   : (long long)json_array_size(cities_j));

    printf("%3s | %-20s | %-20s | %2s | %-18s | %8s | %8s | %9s\n", "#", "City",
           "Country", "CC", "Region", "Lat", "Lon", "Pop");
    printf("----+----------------------+----------------------+----+-----------"
           "---------"
           "+----------+----------+-----------\n");

    size_t n = json_array_size(cities_j);
    for (size_t i = 0; i < n; i++) {
        json_t* c      = json_array_get(cities_j, i);
        json_t* name_j = json_object_get(c, "name");
        json_t* ctr_j  = json_object_get(c, "country");
        json_t* cc_j   = json_object_get(c, "country_code");
        json_t* reg_j  = json_object_get(c, "region");
        json_t* lat_j  = json_object_get(c, "latitude");
        json_t* lon_j  = json_object_get(c, "longitude");
        json_t* pop_j  = json_object_get(c, "population");

        long long pop = (pop_j && json_integer_value(pop_j) > 0)
                            ? (long long)json_integer_value(pop_j)
                            : 0;

        printf("%3zu | %-20s | %-20s | %-2s | %-18s | %8.4f | %8.4f | %9lld\n",
               i + 1,
               name_j && json_is_string(name_j) ? json_string_value(name_j)
                                                : "-",
               ctr_j && json_is_string(ctr_j) ? json_string_value(ctr_j) : "-",
               cc_j && json_is_string(cc_j) ? json_string_value(cc_j) : "-",
               reg_j && json_is_string(reg_j) ? json_string_value(reg_j) : "-",
               lat_j ? json_real_value(lat_j) : 0.0,
               lon_j ? json_real_value(lon_j) : 0.0, pop);
    }
    printf("\n");
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

    json_t*   result = NULL;
    char*     error  = NULL;
    OutputFmt fmt    = FMT_JSON;

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
        fmt    = FMT_WEATHER;

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
        fmt    = FMT_WEATHER;

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
        fmt    = FMT_CITIES;

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
        fmt    = FMT_PLAN;

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

    switch (fmt) {
    case FMT_WEATHER:
        print_weather_table(result);
        break;
    case FMT_CITIES:
        print_cities_table(result);
        break;
    case FMT_PLAN:
        print_plan_table(result);
        break;
    default:
        print_json(result);
        break;
    }
    json_decref(result);
    free(error);
}

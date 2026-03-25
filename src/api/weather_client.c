/**
 * @file weather_client.c
 * @brief Weather API client implementation
 *
 * Implementation of the weather API client interface defined in
 * weather_client.h. This module handles HTTP communication with the weather API
 * server, manages response caching with different TTL values per endpoint, and
 * provides JSON parsing and validation of API responses.
 *
 * See weather_client.h for detailed API documentation.
 */

#include "weather_client.h"

#include "../network/http_client.h"
#include "../utils/client_cache.h"
#include "../utils/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct WeatherClient {
    HttpClient*  http;
    ClientCache* cache;
    char         server_host[256];
    int          server_port;
    int          timeout_ms;
};

static char*   build_cache_key(const char* endpoint, const char* params);
static json_t* make_request(WeatherClient* client, const char* url,
                            const char* cache_key, char** error);

WeatherClient* weather_client_create(const char* host, int port) {
    WeatherClient* client = malloc(sizeof(WeatherClient));
    if (!client) {
        return NULL;
    }

    strncpy(client->server_host, host ? host : "localhost", 255);
    client->server_host[255] = '\0';
    client->server_port      = port > 0 ? port : 10680;
    client->timeout_ms       = 5000;

    client->http = http_client_create(client->timeout_ms);
    if (!client->http) {
        free(client);
        return NULL;
    }

    client->cache = client_cache_create(CACHE_MAX_ENTRIES, CACHE_DEFAULT_TTL);
    if (!client->cache) {
        http_client_destroy(client->http);
        free(client);
        return NULL;
    }

    return client;
}

void weather_client_destroy(WeatherClient* client) {
    if (!client) {
        return;
    }

    if (client->http) {
        http_client_destroy(client->http);
    }

    if (client->cache) {
        client_cache_destroy(client->cache);
    }

    free(client);
}

json_t* weather_client_get_current(WeatherClient* client, double lat,
                                   double lon, char** error) {
    if (!client) {
        if (error) {
            *error = strdup("Invalid client");
        }
        return NULL;
    }

    if (!validate_latitude(lat) || !validate_longitude(lon)) {
        if (error) {
            *error = strdup("Invalid coordinates");
        }
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/v1/current?lat=%.4f&lon=%.4f",
             client->server_host, client->server_port, lat, lon);

    char params[128];
    snprintf(params, sizeof(params), "lat=%.4f:lon=%.4f", lat, lon);
    char* cache_key = build_cache_key("current", params);

    json_t* result = make_request(client, url, cache_key, error);

    free(cache_key);
    return result;
}

json_t* weather_client_get_weather_by_city(WeatherClient* client,
                                           const char*    city,
                                           const char*    country,
                                           const char* region, char** error) {
    if (!client) {
        if (error) {
            *error = strdup("Invalid client");
        }
        return NULL;
    }

    if (!validate_city_name(city)) {
        if (error) {
            *error = strdup("Invalid city name");
        }
        return NULL;
    }

    char* city_encoded = url_encode(city);
    if (!city_encoded) {
        if (error) {
            *error = strdup("Failed to encode city name");
        }
        return NULL;
    }

    char url[1024];
    int  len = snprintf(url, sizeof(url), "http://%s:%d/v1/weather?city=%s",
                        client->server_host, client->server_port, city_encoded);

    if (country && strlen(country) > 0) {
        char* country_encoded = url_encode(country);
        if (country_encoded) {
            len += snprintf(url + len, sizeof(url) - len, "&country=%s",
                            country_encoded);
            free(country_encoded);
        }
    }

    if (region && strlen(region) > 0) {
        char* region_encoded = url_encode(region);
        if (region_encoded) {
            snprintf(url + len, sizeof(url) - len, "&region=%s",
                     region_encoded);
            free(region_encoded);
        }
    }

    char normalized_city[256]    = "";
    char normalized_country[256] = "";
    char normalized_region[256]  = "";

    if (city) {
        normalize_string_for_cache(city, normalized_city,
                                   sizeof(normalized_city));
    }
    if (country) {
        normalize_string_for_cache(country, normalized_country,
                                   sizeof(normalized_country));
    }
    if (region) {
        normalize_string_for_cache(region, normalized_region,
                                   sizeof(normalized_region));
    }

    char params[1024];
    snprintf(params, sizeof(params), "city=%s:country=%s:region=%s",
             normalized_city, normalized_country, normalized_region);

    char* cache_key = build_cache_key("weather", params);

    json_t* result = make_request(client, url, cache_key, error);

    free(city_encoded);
    free(cache_key);
    return result;
}

json_t* weather_client_search_cities(WeatherClient* client, const char* query,
                                     char** error) {
    if (!client) {
        if (error) {
            *error = strdup("Invalid client");
        }
        return NULL;
    }

    if (!query || strlen(query) < 2) {
        if (error) {
            *error = strdup("Query must be at least 2 characters");
        }
        return NULL;
    }

    char* query_encoded = url_encode(query);
    if (!query_encoded) {
        if (error) {
            *error = strdup("Failed to encode query");
        }
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/v1/cities?query=%s",
             client->server_host, client->server_port, query_encoded);

    char normalized_query[256];
    normalize_string_for_cache(query, normalized_query,
                               sizeof(normalized_query));

    char params[512];
    snprintf(params, sizeof(params), "query=%s", normalized_query);
    char* cache_key = build_cache_key("cities", params);

    json_t* result = make_request(client, url, cache_key, error);

    free(query_encoded);
    free(cache_key);
    return result;
}

json_t* weather_client_get_homepage(WeatherClient* client, char** error) {
    if (!client) {
        if (error) {
            *error = strdup("Invalid client");
        }
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/", client->server_host,
             client->server_port);

    if (http_client_get(client->http, url, error) != 0) {
        return NULL;
    }

    const char* body = http_client_get_body(client->http);
    if (!body) {
        if (error) {
            *error = strdup("Empty response");
        }
        return NULL;
    }

    json_t* result = json_object();
    json_object_set_new(result, "content", json_string(body));
    return result;
}

json_t* weather_client_echo(WeatherClient* client, char** error) {
    if (!client) {
        if (error) {
            *error = strdup("Invalid client");
        }
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/echo", client->server_host,
             client->server_port);

    if (http_client_get(client->http, url, error) != 0) {
        return NULL;
    }

    const char* body = http_client_get_body(client->http);
    if (!body) {
        if (error) {
            *error = strdup("Empty response");
        }
        return NULL;
    }

    json_t* result = json_object();
    json_object_set_new(result, "echo", json_string(body));

    return result;
}

// skapa energi-plan för clienten
json_t* weather_client_get_plan(WeatherClient* client, const char* city,
                                const char* price, char** error) {
    if (!client) {
        if (error) {
            *error = strdup("Invalid client");
        }
        return NULL;
    }

    if (!city || !price) {
        if (error) {
            *error = strdup("city and price are required");
        }
        return NULL;
    }

    /* URL-encode city and price so that special characters (spaces, etc.)
     * are valid in a URL, e.g. "My City" -> "My%20City" */
    char* city_encoded  = url_encode(city);
    char* price_encoded = url_encode(price);

    /* If either encoding failed (e.g. out of memory), free both and return
     * an error. free(NULL) is safe if one of them is NULL. */
    if (!city_encoded || !price_encoded) {
        free(city_encoded);
        free(price_encoded);
        if (error) {
            *error = strdup("Failed to encode parameters");
        }
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/v1/get_plan?city=%s&price=%s",
             client->server_host, client->server_port, city_encoded,
             price_encoded);

    char params[512];
    snprintf(params, sizeof(params), "city=%s:price=%s", city, price);
    char* cache_key = build_cache_key("get_plan", params);

    json_t* result = make_request(client, url, cache_key, error);

    free(city_encoded);
    free(price_encoded);
    free(cache_key);
    return result;
}

void weather_client_clear_cache(WeatherClient* client) {
    if (client && client->cache) {
        client_cache_clear(client->cache);
    }
}

void weather_client_set_timeout(WeatherClient* client, int timeout_ms) {
    if (client) {
        client->timeout_ms = timeout_ms;
    }
}

void weather_client_set_server(WeatherClient* client, const char* host,
                               int port) {
    if (!client) {
        return;
    }
    strncpy(client->server_host, host ? host : "localhost", 255);
    client->server_host[255] = '\0';
    client->server_port      = port > 0 ? port : 10680;
    client_cache_clear(client->cache);
}

const char* weather_client_get_host(const WeatherClient* client) {
    return client ? client->server_host : "localhost";
}

int weather_client_get_port(const WeatherClient* client) {
    return client ? client->server_port : 10680;
}

static char* build_cache_key(const char* endpoint, const char* params) {
    size_t len = strlen(endpoint) + strlen(params) + 2;
    char*  key = malloc(len);
    if (!key) {
        return NULL;
    }
    snprintf(key, len, "%s:%s", endpoint, params);
    return key;
}

static json_t* make_request(WeatherClient* client, const char* url,
                            const char* cache_key, char** error) {
    char* cached = client_cache_get(client->cache, cache_key);
    if (cached) {
        json_error_t json_err;
        json_t*      result = json_loads(cached, 0, &json_err);
        free(cached);

        if (result) {
            return result;
        }
    }

    if (http_client_get(client->http, url, error) != 0) {
        return NULL;
    }

    const char* body = http_client_get_body(client->http);
    if (!body) {
        if (error) {
            *error = strdup("Empty response");
        }
        return NULL;
    }

    json_error_t json_err;
    json_t*      result = json_loads(body, 0, &json_err);

    if (!result) {
        if (error) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "JSON parse error: %s",
                     json_err.text);
            *error = strdup(err_msg);
        }
        return NULL;
    }

    json_t* success_field = json_object_get(result, "success");
    if (success_field && json_is_boolean(success_field)) {
        if (!json_boolean_value(success_field)) {
            json_t* error_obj = json_object_get(result, "error");
            if (error_obj && error) {
                json_t* msg = json_object_get(error_obj, "message");
                if (msg && json_is_string(msg)) {
                    *error = strdup(json_string_value(msg));
                }
            }
            json_decref(result);
            return NULL;
        }
    }

    client_cache_set(client->cache, cache_key, body);

    return result;
}

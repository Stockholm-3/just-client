/**
 * @file weather_client.h
 * @brief Weather API client interface
 *
 * This header provides a high-level client for interacting with the weather
 * API. It supports fetching current weather by coordinates or city name,
 * searching for cities, and includes automatic caching for improved performance
 * and reduced server load. All API responses are returned as JSON objects using
 * the Jansson library.
 *
 * Features:
 * - Current weather by coordinates
 * - Weather lookup by city name with optional country/region
 * - City search with autocomplete support
 * - Automatic response caching with configurable TTL
 * - JSON response parsing and validation
 * - Error handling with descriptive messages
 *
 * @note All functions that return json_t* transfer ownership of the JSON object
 *       to the caller. The caller must call json_decref() when done.
 */
#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#define TTL_WEATHER 300    ///< Weather data cache: 5 minutes
#define TTL_CITIES 3600    ///< Cities search cache: 1 hour
#define TTL_HOMEPAGE 86400 ///< Homepage cache: 24 hours

#include <jansson.h>
#include <stddef.h>
#include <time.h>

/**
 * @struct WeatherClient
 * @brief Weather API client structure (opaque)
 *
 * Opaque structure that holds the weather client state including HTTP client,
 * cache, and server configuration. Users should not access internal fields
 * directly.
 */

typedef struct WeatherClient WeatherClient;

/**
 * @brief Creates a new weather client instance
 *
 * Allocates and initializes a new WeatherClient with the specified server
 * configuration. The client includes an HTTP client for network communication
 * and a cache for storing responses. Default timeout is 5000ms (5 seconds).
 *
 * @param host The weather API server hostname or IP address.
 *             If NULL, defaults to "localhost".
 * @param port The weather API server port number.
 *             If <= 0, defaults to 10680.
 *
 * @return Pointer to the newly created WeatherClient structure, or NULL if
 *         memory allocation fails or initialization of HTTP client or cache
 * fails
 *
 * @see weather_client_destroy()
 *
 * @par Example:
 * @code
 * WeatherClient *client = weather_client_create("api.weather.com", 80);
 * if (!client) {
 *     fprintf(stderr, "Failed to create weather client\n");
 *     return -1;
 * }
 * @endcode
 */
WeatherClient* weather_client_create(const char* host, int port);

/**
 * @brief Destroys a weather client instance and frees all resources
 *
 * Closes all connections, destroys the cache, and frees all memory allocated
 * for the WeatherClient structure. Safe to call with NULL pointer.
 *
 * @param client Pointer to the WeatherClient structure to destroy (can be NULL)
 *
 * @see weather_client_create()
 *
 * @par Example:
 * @code
 * WeatherClient *client = weather_client_create("localhost", 10680);
 * // ... use client ...
 * weather_client_destroy(client);
 * @endcode
 */
void weather_client_destroy(WeatherClient* client);

/**
 * @brief Gets current weather by geographic coordinates
 *
 * Fetches current weather data for the specified latitude and longitude.
 * Results are automatically cached for 5 minutes (300 seconds) to reduce
 * server load and improve response time for repeated queries.
 *
 * The returned JSON object contains weather information including:
 * - Temperature, humidity, pressure
 * - Weather conditions and description
 * - Wind speed and direction
 * - Location information
 *
 * @param client Pointer to the WeatherClient structure
 * @param lat Latitude in decimal degrees (-90 to +90)
 * @param lon Longitude in decimal degrees (-180 to +180)
 * @param error Optional pointer to store error message. If not NULL and an
 *              error occurs, will be set to a dynamically allocated string.
 *              Caller must free this string.
 *
 * @return JSON object with weather data on success, or NULL on failure.
 *         The caller owns the returned JSON object and must call json_decref()
 *         when done.
 *
 * @note The function will fail if:
 *       - Invalid client pointer
 *       - Invalid coordinates (lat not in [-90, 90] or lon not in [-180, 180])
 *       - Network/HTTP error
 *       - Invalid JSON response from server
 *       - Server returned an error response
 *
 * @see weather_client_get_weather_by_city()
 *
 * @par Example:
 * @code
 * char *error = NULL;
 * json_t *weather = weather_client_get_current(client, 59.33, 18.07, &error);
 * if (weather) {
 *     json_t *data = json_object_get(weather, "data");
 *     json_t *temp = json_object_get(data, "temperature");
 *     printf("Temperature: %.1f°C\n", json_real_value(temp));
 *     json_decref(weather);
 * } else {
 *     fprintf(stderr, "Error: %s\n", error ? error : "Unknown");
 *     free(error);
 * }
 * @endcode
 */
json_t* weather_client_get_current(WeatherClient* client, double lat,
                                   double lon, char** error);

/**
 * @brief Gets weather by city name
 *
 * Fetches current weather data for a specified city with optional country
 * and region filters to disambiguate cities with the same name. Results are
 * automatically cached for 5 minutes (300 seconds).
 *
 * @param client Pointer to the WeatherClient structure
 * @param city City name (required, must be at least 1 character)
 * @param country Optional ISO 3166-1 alpha-2 country code (e.g., "SE", "US").
 *                Pass NULL or empty string to skip.
 * @param region Optional region/state name. Pass NULL or empty string to skip.
 * @param error Optional pointer to store error message. If not NULL and an
 *              error occurs, will be set to a dynamically allocated string.
 *              Caller must free this string.
 *
 * @return JSON object with weather data on success, or NULL on failure.
 *         The caller owns the returned JSON object and must call json_decref()
 *         when done.
 *
 * @note The function will fail if:
 *       - Invalid client pointer
 *       - Invalid city name (NULL, empty, or invalid characters)
 *       - Network/HTTP error
 *       - City not found
 *       - Invalid JSON response from server
 *       - Server returned an error response
 *
 * @note City, country, and region parameters are URL-encoded automatically.
 *
 * @see weather_client_get_current(), weather_client_search_cities()
 *
 * @par Example:
 * @code
 * char *error = NULL;
 * json_t *weather = weather_client_get_weather_by_city(
 *     client, "Stockholm", "SE", NULL, &error);
 * if (weather) {
 *     // Process weather data
 *     json_decref(weather);
 * } else {
 *     fprintf(stderr, "Error: %s\n", error ? error : "Unknown");
 *     free(error);
 * }
 * @endcode
 */
json_t* weather_client_get_weather_by_city(WeatherClient* client,
                                           const char*    city,
                                           const char*    country,
                                           const char* region, char** error);

/**
 * @brief Searches for cities matching a query string
 *
 * Performs a search for cities whose names contain the specified query string.
 * Useful for autocomplete functionality or finding the correct city name before
 * fetching weather. Results are cached for 1 hour (3600 seconds).
 *
 * The returned JSON contains an array of matching cities with their full names,
 * countries, regions, and coordinates.
 *
 * @param client Pointer to the WeatherClient structure
 * @param query Search query string (minimum 2 characters)
 * @param error Optional pointer to store error message. If not NULL and an
 *              error occurs, will be set to a dynamically allocated string.
 *              Caller must free this string.
 *
 * @return JSON object with search results on success, or NULL on failure.
 *         The caller owns the returned JSON object and must call json_decref()
 *         when done.
 *
 * @note The function will fail if:
 *       - Invalid client pointer
 *       - Query is NULL or less than 2 characters
 *       - Network/HTTP error
 *       - Invalid JSON response from server
 *       - Server returned an error response
 *
 * @note The query parameter is URL-encoded automatically.
 *
 * @see weather_client_get_weather_by_city()
 *
 * @par Example:
 * @code
 * char *error = NULL;
 * json_t *results = weather_client_search_cities(client, "Stock", &error);
 * if (results) {
 *     json_t *cities = json_object_get(results, "data");
 *     size_t index;
 *     json_t *city;
 *     json_array_foreach(cities, index, city) {
 *         const char *name = json_string_value(json_object_get(city, "name"));
 *         const char *country = json_string_value(json_object_get(city,
 * "country")); printf("%s, %s\n", name, country);
 *     }
 *     json_decref(results);
 * } else {
 *     fprintf(stderr, "Error: %s\n", error ? error : "Unknown");
 *     free(error);
 * }
 * @endcode
 */
json_t* weather_client_search_cities(WeatherClient* client, const char* query,
                                     char** error);

/**
 * @brief Gets the API homepage/welcome message
 *
 * Fetches the homepage/root endpoint of the API, typically containing
 * welcome message, API version, and available endpoints. Results are
 * cached for 24 hours (86400 seconds).
 *
 * @param client Pointer to the WeatherClient structure
 * @param error Optional pointer to store error message. If not NULL and an
 *              error occurs, will be set to a dynamically allocated string.
 *              Caller must free this string.
 *
 * @return JSON object with homepage data on success, or NULL on failure.
 *         The caller owns the returned JSON object and must call json_decref()
 *         when done.
 *
 * @see weather_client_echo()
 *
 * @par Example:
 * @code
 * char *error = NULL;
 * json_t *homepage = weather_client_get_homepage(client, &error);
 * if (homepage) {
 *     json_t *message = json_object_get(homepage, "message");
 *     printf("%s\n", json_string_value(message));
 *     json_decref(homepage);
 * }
 * @endcode
 */
json_t* weather_client_get_homepage(WeatherClient* client, char** error);

/**
 * @brief Tests server connectivity with echo endpoint
 *
 * Sends a request to the server's echo endpoint to verify connectivity
 * and measure response time. This is useful for health checks and debugging.
 * Results are not cached.
 *
 * @param client Pointer to the WeatherClient structure
 * @param error Optional pointer to store error message. If not NULL and an
 *              error occurs, will be set to a dynamically allocated string.
 *              Caller must free this string.
 *
 * @return JSON object containing the echo response on success, or NULL on
 * failure. The caller owns the returned JSON object and must call json_decref()
 *         when done.
 *
 * @see weather_client_get_homepage()
 *
 * @par Example:
 * @code
 * char *error = NULL;
 * json_t *echo = weather_client_echo(client, &error);
 * if (echo) {
 *     printf("Server is responding\n");
 *     json_decref(echo);
 * } else {
 *     fprintf(stderr, "Server error: %s\n", error ? error : "Unknown");
 *     free(error);
 * }
 * @endcode
 */
json_t* weather_client_echo(WeatherClient* client, char** error);

/**
 * @brief Clears all cached responses
 *
 * Removes all entries from the client's cache, forcing the next requests
 * to fetch fresh data from the server. Useful when you need to ensure
 * up-to-date information or for debugging purposes.
 *
 * @param client Pointer to the WeatherClient structure (safe to pass NULL)
 *
 * @note This operation is safe and will not affect any JSON objects that
 *       have already been returned to the caller.
 *
 * @par Example:
 * @code
 * // Clear cache to get fresh data
 * weather_client_clear_cache(client);
 * json_t *weather = weather_client_get_current(client, 59.33, 18.07, NULL);
 * @endcode
 */

void weather_client_clear_cache(WeatherClient* client);

/**
 * @brief Gets energy plan data for a city from the server
 *
 * Fetches energy plan data from the /v1/get_plan endpoint.
 * Requires a Swedish city name and a price zone (SE1–SE4).
 *
 * @param client Pointer to the WeatherClient structure
 * @param city   City name (e.g. "Stockholm")
 * @param price  Price zone: SE1, SE2, SE3 or SE4
 * @param error  Optional pointer to store error message. Caller must free.
 *
 * @return JSON object on success, NULL on failure.
 *         Caller must call json_decref() when done.
 */
json_t* weather_client_get_plan(WeatherClient* client, const char* city,
                                const char* price, char** error);
/**
 * @brief Sets the network timeout for API requests
 *
 * Configures the timeout for HTTP requests made by this client. This affects
 * all subsequent API calls. The timeout applies to both connection
 * establishment and data transfer operations.
 *
 * @param client Pointer to the WeatherClient structure (safe to pass NULL)
 * @param timeout_ms Timeout value in milliseconds. Values <= 0 are ignored.
 *                   Typical values: 5000 (5s), 10000 (10s), 30000 (30s).
 *
 * @note The default timeout is 5000ms (5 seconds) set during client creation.
 * @note This setting does not affect requests that are already in progress.
 *
 * @par Example:
 * @code
 * WeatherClient *client = weather_client_create("localhost", 10680);
 * // Set timeout to 10 seconds for slow networks
 * weather_client_set_timeout(client, 10000);
 * @endcode
 */
void weather_client_set_timeout(WeatherClient* client, int timeout_ms);

/**
 * @brief Updates the server address and port, clearing the cache.
 * @param client Pointer to WeatherClient (safe to pass NULL)
 * @param host   New hostname or IP; NULL resets to "localhost"
 * @param port   New port; <= 0 resets to 10680
 */
void        weather_client_set_server(WeatherClient* client, const char* host,
                                      int port);
const char* weather_client_get_host(const WeatherClient* client);
int         weather_client_get_port(const WeatherClient* client);

#endif

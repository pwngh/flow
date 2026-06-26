/* tests/api/mock_server.h
 *
 * Tiny localhost HTTP server for hermetic adapter tests.
 *
 * Spins up a thread that binds an ephemeral port, accepts one
 * connection, reads the HTTP request, returns a pre-canned
 * response. The test discovers the port via the returned struct,
 * configures the adapter's base_url to point at it, runs the
 * flow, and asserts on the request body the server received.
 *
 * No external dependencies. POSIX sockets only (Linux + macOS).
 * Single connection per server — for tests that need to handle N
 * calls, spin up N servers (cheap; ports are free).
 *
 * This is the canonical pattern for testing libcurl-backed
 * adapters in flowd's CI. The cassette-recorder-replayer approach
 * common in other ecosystems is overkill at this scale; hardcoded
 * canned responses per test are clearer and cheaper to maintain.
 */

#ifndef FLOWD_TESTS_MOCK_SERVER_H
#define FLOWD_TESTS_MOCK_SERVER_H

#include <stddef.h>
#include <stdint.h>


typedef struct mock_server mock_server_t;

/* Start a server. `response` is the full HTTP response (status
 * line + headers + blank line + body) that will be returned to
 * the single client that connects. The server runs on its own
 * thread; the call returns once the server is bound and listening.
 *
 * Returns NULL on bind/listen failure. */
mock_server_t *mock_server_start(const char *response);

/* The bound port. Pass `http://127.0.0.1:<port>` to the adapter as
 * its base_url. */
uint16_t mock_server_port(const mock_server_t *m);

/* Capture: after the test, fetch what the server received. The
 * full request (request line + headers + body) is in r->request,
 * the parsed Content-Length-bounded body is in r->body. The
 * returned struct is owned by the server; valid until
 * mock_server_stop. */
typedef struct {
    char  *request;
    size_t request_len;
    char  *body;
    size_t body_len;
} mock_capture_t;

const mock_capture_t *mock_server_captured(mock_server_t *m);

/* Stop and free. Joins the server thread; safe to call from any
 * thread but only once. */
void mock_server_stop(mock_server_t *m);


#endif /* FLOWD_TESTS_MOCK_SERVER_H */

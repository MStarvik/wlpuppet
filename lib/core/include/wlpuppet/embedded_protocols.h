#ifndef WLPUPPET_EMBEDDED_PROTOCOLS_H
#define WLPUPPET_EMBEDDED_PROTOCOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A protocol XML file embedded at build time (see cmake/WlpuppetProtocols.cmake).
struct wlp_embedded_protocol {
    const char *name;
    const unsigned char *data;
    size_t size;
    /// Lower wins when two files define the same interface.
    int priority;
};

extern const struct wlp_embedded_protocol wlp_embedded_protocols[];
extern const size_t wlp_embedded_protocol_count;

#ifdef __cplusplus
}
#endif

#endif

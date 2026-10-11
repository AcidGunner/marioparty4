#ifndef PORT_LOG_H
#define PORT_LOG_H

typedef struct PortStubSite {
    const char *name;
    unsigned long count;
    struct PortStubSite *next;
} PortStubSite;

void port_stub_hit(PortStubSite *s);

/* Put at the top of a stub body. */
#define PORT_STUB(NAME)                                          \
    do {                                                         \
        static PortStubSite site_ = { NAME, 0, 0 };              \
        port_stub_hit(&site_);                                   \
    } while (0)

#endif

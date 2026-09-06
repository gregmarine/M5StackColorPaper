# dns_server

Vendored verbatim from ESP-IDF's
`examples/protocols/http_server/captive_portal/components/dns_server`
(ESP-IDF v5.5.1), which is published under `Unlicense OR CC0-1.0`. The SPDX
headers in the sources are unchanged.

It answers every A query with the soft AP's address. The frame deliberately
does *not* run a captive portal (see `docs/webapp.md` for why), but it still
needs to answer the phone's connectivity probes to pass them, and those are
requests to real hostnames — so they have to resolve here first. ESP-IDF has
no DNS server component, so the alternative was writing the same ~290 lines of
UDP socket handling by hand.

Used by `main/net/web_server.cpp`; see `docs/webapp.md`.

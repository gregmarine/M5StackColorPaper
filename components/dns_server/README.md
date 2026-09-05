# dns_server

Vendored verbatim from ESP-IDF's
`examples/protocols/http_server/captive_portal/components/dns_server`
(ESP-IDF v5.5.1), which is published under `Unlicense OR CC0-1.0`. The SPDX
headers in the sources are unchanged.

It answers every A query with the soft AP's address, which is what makes the
photo manager pop up by itself when a phone joins the frame's hotspot. ESP-IDF
has no DNS server component, so the alternative was writing the same ~290
lines of UDP socket handling by hand.

Used by `main/net/web_server.cpp`; see `docs/webapp.md`.

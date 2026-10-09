# CA trust anchors

`ca_anchors.h` is the list of root certificates botcore's TLS client trusts. It is compiled in:
botcore never reads certificate files from the host.

Source: Mozilla's CA store as extracted by curl (https://curl.se/docs/caextract.html).
The certificate data is licensed under the Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/).

Regenerate (needs network once, and BearSSL's `brssl` tool):

    curl -sSfLO https://curl.se/ca/cacert.pem
    git clone https://www.bearssl.org/git/BearSSL && make -C BearSSL
    BearSSL/build/brssl ta cacert.pem > ta.c
    # prepend the provenance comment (see the top of ca_anchors.h) and save as ca_anchors.h

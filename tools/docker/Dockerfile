FROM debian@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a

ARG PACKAGE_VERSION=0.1.0~1135-g229d64a7-1~bpo13+1
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && curl -fsSL https://pkg.noctalia.dev/deb/nickh-archive-keyring.deb -o /tmp/keyring.deb \
    && echo '2f58e934a5983b41dbd0d9e30fbf246f7eaef1d0c02132cf1d6f69cb87c1074f  /tmp/keyring.deb' | sha256sum -c - \
    && dpkg -i /tmp/keyring.deb \
    && printf '%s\n' 'Types: deb deb-src' 'URIs: https://pkg.noctalia.dev/deb/' \
       'Suites: trixie/ trixie-backports/' 'Architectures: amd64' \
       'Signed-By: /usr/share/keyrings/nickh-archive-keyring.gpg' > /etc/apt/sources.list.d/noctalia.sources \
    && apt-get update \
    && apt-get install -y -t trixie-backports --no-install-recommends build-essential fakeroot git python3 \
       libwlroots-0.20-dev=0.20.1-1~bpo13+1 libwayland-dev=1.25.0-2~bpo13+1 \
       libdrm-dev=2.4.134-3~bpo13+1 libinput-dev=1.28.1-1+deb13u1 \
       wayland-protocols=1.49-1~bpo13+2 \
    && apt-get build-dep -y -t trixie-backports --no-install-recommends umbriel=${PACKAGE_VERSION} \
    && mkdir /opt/package-source /opt/stock \
    && cd /opt/package-source && apt-get source umbriel=${PACKAGE_VERSION} \
    && cp -a umbriel-*/debian /opt/umbriel-debian \
    && cd /opt/stock && apt-get download umbriel=${PACKAGE_VERSION} \
    && chmod -R a+rX /opt \
    && dpkg-query -W -f='${binary:Package}\t${Version}\n' > /opt/build-packages.txt \
    && rm -rf /opt/package-source /var/lib/apt/lists/* /tmp/keyring.deb

COPY build.py /opt/build.py
ENTRYPOINT ["python3", "/opt/build.py"]

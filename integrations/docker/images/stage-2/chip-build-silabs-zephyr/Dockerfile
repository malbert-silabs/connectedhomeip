ARG VERSION=1
FROM ghcr.io/project-chip/chip-build:${VERSION} AS build
LABEL org.opencontainers.image.source=https://github.com/project-chip/connectedhomeip

WORKDIR /opt/silabs/

RUN set -x \
    && ZEPHYR_SDK_VERSION=0.17.2 \
    && ZEPHYR_SILABS_HASH=c89815d6d6ba24ce5e4fdf3956c9523d995131ab \
    && python3 -m venv venv \
    && . venv/bin/activate \
    && pip3 install west requests \
    && git clone https://github.com/SiliconLabsSoftware/zephyr-silabs.git \
    && cd zephyr-silabs \
    && git checkout ${ZEPHYR_SILABS_HASH} \
    # Initialize west with the specific commit of the zephyr-silabs repository
    # This commit corresponds to the version of the Zephyr SDK used in the project
    && cd ../ \
    && west init -l ./zephyr-silabs  \
    # 
    # && cd zephyr \
    # && git checkout b333d1f4cc6b96d053b425e07148661cf25778d8 \
    # && cd ../ \
    #
    && west update \
    && west blobs fetch \
    && mkdir -p /opt/silabs/sdk \
    && cd /opt/silabs/sdk \
    && wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz \
    && tar xvf zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz \
    && rm zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz \
    && cd zephyr-sdk-${ZEPHYR_SDK_VERSION} \
    && ./setup.sh -t arm-zephyr-eabi \
    && export ZEPHYR_SDK_INSTALL_DIR=/opt/silabs/sdk \
    && west zephyr-export \
    && : # last line

FROM ghcr.io/project-chip/chip-build:${VERSION}

COPY --from=build /opt/silabs/ /opt/silabs/

ENV SILABS_ZEPHYR_SDK_PATH=/opt/silabs
ENV ZEPHYR_BASE=/opt/silabs/zephyr
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/silabs/sdk

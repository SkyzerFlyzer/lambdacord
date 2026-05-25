FROM amazonlinux:2023

RUN dnf install -y \
    clang20 \
    clang20-tools-extra \
    gcc-c++ \
    glibc-static \
    cmake \
    cppcheck \
    make \
    git \
    perl-core \
    python3 \
    pkgconf-pkg-config \
    zip \
    unzip \
    llvm20 \
    valgrind \
    findutils \
    tar \
    gzip \
    which \
    libstdc++-static \
    libsodium-devel \
    libsodium-static \
    zlib-devel \
    zlib-static \
    && dnf clean all

ENV CXX=clang++-20
ENV OPENSSL_VERSION=3.5.5
ENV CURL_VERSION=8.17.0
ENV AWS_LAMBDA_RIE_VERSION=1.27
ENV PKG_CONFIG_PATH=/opt/lambda-static/lib64/pkgconfig:/opt/lambda-static/lib/pkgconfig:/usr/lib64/pkgconfig

RUN curl -fsSL \
    "https://github.com/aws/aws-lambda-runtime-interface-emulator/releases/download/v${AWS_LAMBDA_RIE_VERSION}/aws-lambda-rie-$(case "$(uname -m)" in aarch64) echo arm64 ;; x86_64) echo x86_64 ;; *) echo unsupported; exit 1 ;; esac)" \
    -o /usr/local/bin/aws-lambda-rie \
    && chmod 755 /usr/local/bin/aws-lambda-rie

RUN curl -fsSL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" -o /tmp/openssl.tar.gz \
    && mkdir -p /tmp/openssl \
    && tar -xzf /tmp/openssl.tar.gz -C /tmp/openssl --strip-components 1 \
    && cd /tmp/openssl \
    && OPENSSL_TARGET="$(case "$(uname -m)" in aarch64) echo linux-aarch64 ;; x86_64) echo linux-x86_64 ;; *) echo unsupported; exit 1 ;; esac)" \
    && ./Configure "${OPENSSL_TARGET}" no-shared no-tests --prefix=/opt/lambda-static --openssldir=/opt/lambda-static/ssl \
    && make -j"$(nproc)" \
    && make install_sw \
    && rm -rf /tmp/openssl /tmp/openssl.tar.gz

RUN curl -fsSL "https://curl.se/download/curl-${CURL_VERSION}.tar.gz" -o /tmp/curl.tar.gz \
    && mkdir -p /tmp/curl \
    && tar -xzf /tmp/curl.tar.gz -C /tmp/curl --strip-components 1 \
    && cd /tmp/curl \
    && ./configure \
        --prefix=/opt/lambda-static \
        --disable-shared \
        --enable-static \
        --with-openssl=/opt/lambda-static \
        --with-zlib=/usr \
        --with-ca-bundle=/etc/pki/tls/certs/ca-bundle.crt \
        --without-brotli \
        --without-gssapi \
        --without-libidn2 \
        --without-libpsl \
        --without-libssh2 \
        --without-librtmp \
        --without-nghttp2 \
        --without-nghttp3 \
        --without-ngtcp2 \
        --without-zstd \
        --disable-dict \
        --disable-file \
        --disable-ftp \
        --disable-gopher \
        --disable-imap \
        --disable-ldap \
        --disable-ldaps \
        --disable-mqtt \
        --disable-pop3 \
        --disable-rtsp \
        --disable-smb \
        --disable-smtp \
        --disable-telnet \
        --disable-tftp \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /tmp/curl /tmp/curl.tar.gz

RUN git clone --depth 1 https://github.com/awslabs/aws-lambda-cpp.git /tmp/aws-lambda-cpp \
    && cmake -S /tmp/aws-lambda-cpp -B /tmp/aws-lambda-cpp/build \
        -DCMAKE_CXX_COMPILER=clang++-20 \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_PREFIX_PATH=/opt/lambda-static \
        -DCMAKE_FIND_LIBRARY_SUFFIXES=.a \
        -DCURL_USE_STATIC_LIBS=ON \
        -DOPENSSL_ROOT_DIR=/opt/lambda-static \
        -DCMAKE_INSTALL_PREFIX=/opt/lambda-install \
    && cmake --build /tmp/aws-lambda-cpp/build --parallel \
    && cmake --install /tmp/aws-lambda-cpp/build \
    && rm -rf /tmp/aws-lambda-cpp

RUN git clone --depth 1 --recurse-submodules https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp \
    && cmake -S /tmp/aws-sdk-cpp -B /tmp/aws-sdk-cpp/build \
        -DCMAKE_CXX_COMPILER=clang++-20 \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_ONLY="lambda;dynamodb;kms" \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_UNITY_BUILD=ON \
        -DENABLE_TESTING=OFF \
        -DCMAKE_PREFIX_PATH=/opt/lambda-static \
        -DCMAKE_FIND_LIBRARY_SUFFIXES=.a \
        -DCURL_USE_STATIC_LIBS=ON \
        -DOPENSSL_ROOT_DIR=/opt/lambda-static \
        -DCMAKE_INSTALL_PREFIX=/opt/lambda-install \
    && cmake --build /tmp/aws-sdk-cpp/build --parallel \
    && cmake --install /tmp/aws-sdk-cpp/build \
    && rm -rf /tmp/aws-sdk-cpp

COPY scripts/build-lambda-in-docker.sh /opt/lambda-builder/build-lambda-in-docker.sh
COPY scripts/mock-lambda-runtime-api.py /opt/lambda-builder/mock-lambda-runtime-api.py
RUN chmod 755 /opt/lambda-builder/build-lambda-in-docker.sh /opt/lambda-builder/mock-lambda-runtime-api.py

WORKDIR /workspace

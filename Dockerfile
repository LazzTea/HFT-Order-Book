FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    gdb \
    openssh-server \
    libxml2-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 \
        https://github.com/quickfix/quickfix.git /tmp/quickfix \
    && cmake -B /tmp/quickfix/build /tmp/quickfix \
        -DCMAKE_BUILD_TYPE=Release \
        -DHAVE_SSL=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_CXX_STANDARD=17 \
    && cmake --build /tmp/quickfix/build -j$(nproc) \
    && cmake --install /tmp/quickfix/build \
    && rm -rf /tmp/quickfix

RUN mkdir /var/run/sshd
RUN echo 'root:password' | chpasswd
RUN sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config
RUN useradd -m user && echo 'user:password' | chpasswd
RUN usermod -aG sudo user

EXPOSE 22
CMD ["/usr/sbin/sshd", "-D"]
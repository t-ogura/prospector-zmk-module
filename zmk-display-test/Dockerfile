# GitHub Actions build environment for ZMK
FROM zmkfirmware/zmk-build-arm:stable

# Install dependencies
RUN apt-get update && apt-get install -y \
    git \
    wget \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Install west
RUN pip3 install west

# Set working directory
WORKDIR /workspace
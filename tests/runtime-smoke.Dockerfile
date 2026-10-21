FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install --no-install-recommends -y libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

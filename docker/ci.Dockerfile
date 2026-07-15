FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers

RUN apt-get update -qq \
    && apt-get install -y --no-install-recommends \
        build-essential clang clang-format clang-tidy cmake ninja-build \
        doxygen graphviz libssl-dev python3 xxd git ca-certificates curl bash file \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

# Bake Playwright browsers pinned by the repo lockfile.
COPY tests/package.json tests/package-lock.json /tmp/pw/
RUN cd /tmp/pw && npm ci \
    && npx playwright install --with-deps chromium firefox \
    && rm -rf /tmp/pw /root/.npm

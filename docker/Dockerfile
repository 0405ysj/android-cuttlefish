# Docker image for running CF instances in the server.
# Docker image includes HO(Host Orchestrator) inside,
# so it could execute CF instance with API in HO.

FROM debian:12 AS host-package-builder

USER root
WORKDIR /root
RUN set -x
RUN apt update
RUN apt install -y --no-install-recommends \
    apt-utils \
    curl \
    devscripts \
    equivs \
    sudo

# Build CF debian packages
COPY . /root/android-cuttlefish
RUN ["/bin/bash", "-c", "/root/android-cuttlefish/tools/buildutils/build_packages.sh"]


FROM debian:12 AS runner-base

# Expose Operator Port (HTTP:1080, HTTPS:1443)
EXPOSE 1080 1443
# Expose HO(Host Orchestrator) Port (HTTP:2080, HTTPS:2443)
EXPOSE 2080 2443
# Expose WebRTC Port
EXPOSE 15550-15560
# Expose ADB Port
# Corresponding ADB port for CF instance is, 6520+instance_num-1.
EXPOSE 6520-6620
# to make sure this file always exist
RUN touch /.dockerenv

USER root
WORKDIR /root
RUN set -x
RUN apt update
RUN apt install -y --no-install-recommends \
    ca-certificates \
    curl \
    nginx \
    sudo
RUN update-ca-certificates

COPY ./docker/guest/run_services.sh /root/
RUN chmod +x /root/run_services.sh


FROM runner-base AS runner-with-local-source

# Install CF debian packages.
COPY --from=host-package-builder /root/android-cuttlefish/cuttlefish-*.deb /root/debian/
RUN apt install -y --no-install-recommends -f \
    /root/debian/cuttlefish-base_*.deb \
    /root/debian/cuttlefish-user_*.deb \
    /root/debian/cuttlefish-orchestration_*.deb

RUN echo "num_cvd_accounts=100" >> /etc/default/cuttlefish-host-resources
RUN usermod -aG kvm root
RUN usermod -aG cvdnetwork root
ENTRYPOINT ["/root/run_services.sh"]


FROM runner-base AS runner-with-apt-install

RUN apt install -y --no-install-recommends \
    wget
RUN wget -qO- https://artifacts.codelinaro.org/artifactory/linaro-372-googlelt-gigabyte-ampere-cuttlefish-installer/gigabyte-ampere-cuttlefish-installer/latest/debian/linaro-glt-gig-archive-bookworm.asc \
    | tee /etc/apt/trusted.gpg.d/linaro-glt-gig-archive-bookworm.asc
RUN echo "deb https://artifacts.codelinaro.org/linaro-372-googlelt-gigabyte-ampere-cuttlefish-installer/gigabyte-ampere-cuttlefish-installer/latest/debian bookworm main" \
    | tee /etc/apt/sources.list.d/linaro-glt-gig-archive-bookworm.list
RUN apt update
RUN apt install -y --no-install-recommends \
    cuttlefish-base cuttlefish-user cuttlefish-orchestration

RUN echo "num_cvd_accounts=100" >> /etc/default/cuttlefish-host-resources
RUN usermod -aG kvm root
RUN usermod -aG cvdnetwork root
ENTRYPOINT ["/root/run_services.sh"]

#
# Toolchain base image: clang/libc++, Boost, spdlog and friends, prebuilt and pushed to the HUB so
# that codespace creation stays fast. It happens to be the dependency image of the 'anyhttp'
# project, which is where this devcontainer comes from -- nothing of that project itself is in it.
#
FROM docker.io/psedoc/anyhttp:0.31

# ==================================================================================================

#
# install some more interactive utils in the devcontainer
#
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive && \
    apt-get -y install iputils-ping tmux btop valgrind && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# ==================================================================================================

#
# MrDocs, which makes the reference documentation (cmake --build build --target docs). It is a
# release of prebuilt binaries, pinned by what its archive hashes to. The docs workflow in
# .github/workflows installs the same one.
#
ARG MRDOCS_RELEASE=2026.9.29
ARG MRDOCS_SHA256=9d96e42f303046fb0b221aa9071eabccc40b9683e0b7af2b02a929d15476ca6a
RUN curl -sSfL -o /tmp/mrdocs.tar.xz \
        "https://github.com/cppalliance/mrdocs/releases/download/${MRDOCS_RELEASE}/MrDocs-${MRDOCS_RELEASE}-Linux.tar.xz" && \
    echo "${MRDOCS_SHA256}  /tmp/mrdocs.tar.xz" | sha256sum --check && \
    mkdir -p /opt/mrdocs && \
    tar -xJf /tmp/mrdocs.tar.xz -C /opt/mrdocs --strip-components=1 && \
    ln -s /opt/mrdocs/bin/mrdocs /usr/local/bin/mrdocs && \
    rm /tmp/mrdocs.tar.xz

# ==================================================================================================

#
# enable persistent bash history in container
#
# https://code.visualstudio.com/remote/advancedcontainers/persist-bash-history
#
# To actually make it persistent across re-creation of the container, you need to
# mount a volume to /commandhistory. You can do this in devcontainer.json like this:
#
# 	"mounts": [
# 		"source=cpp-devcontainer-bashhistory,target=/commandhistory,type=volume"
#   ]
#
ARG USERNAME=vscode
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
    && mkdir /commandhistory \
    && touch /commandhistory/.bash_history \
    && chown -R $USERNAME /commandhistory \
    && echo "$SNIPPET" >> "/home/$USERNAME/.bashrc"

RUN echo "export LESS=RS" >/etc/profile.d/less.sh

#
# Copy .bash_aliases from .devcontainer to user's home
#
ARG USERNAME=vscode
COPY .bash_aliases /home/${USERNAME}/.bash_aliases
RUN chown $USERNAME:$USERNAME /home/${USERNAME}/.bash_aliases

# --------------------------------------------------------------------------------------------------

SHELL ["/bin/bash", "-c"]

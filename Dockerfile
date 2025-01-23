FROM ubuntu:22.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ninja-build \ 
        gcc-multilib \ 
        cmake \
        git \
        # Needed for gcovr
        python3 \
        python3-pip

RUN pip3 install gcovr

COPY . /opt/code/
WORKDIR /opt/code/scripts
CMD ./build_test_web.sh
# RUN (./build_web.sh)
# RUN (./test_web.sh)


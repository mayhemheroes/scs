FROM --platform=linux/amd64 ubuntu:18.04 as builder

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake liblas-dev

COPY . /repo
WORKDIR /repo
RUN make -j8
RUN make test

FROM ubuntu:18.04 as package
RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y liblas-dev
COPY --from=builder /repo/out/run_from_file_direct /

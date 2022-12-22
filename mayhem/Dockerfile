FROM --platform=linux/amd64 ubuntu:18.04 as builder

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake liblas-dev clang-10

COPY . /repo
WORKDIR /repo
RUN make -j8
RUN make test
RUN clang++-10 /repo/mayhem/fuzz_scs_norm_sq.cpp -fsanitize=fuzzer,address -I /repo/include/ -o /fuzz -L /repo/out/ -lscsdir

FROM ubuntu:18.04 as package
RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y liblas-dev
COPY --from=builder /repo/out/* /repo/out/
COPY --from=builder /fuzz /fuzz_scs_norm_sq
ENV LD_LIBRARY_PATH=/repo/out/

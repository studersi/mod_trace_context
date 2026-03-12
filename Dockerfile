FROM httpd:2.4-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        libapr1-dev \
        libaprutil1-dev

COPY . /usr/src/mod_trace_context

WORKDIR /usr/src/mod_trace_context

RUN make
RUN make install

WORKDIR /usr/local/apache2/conf/

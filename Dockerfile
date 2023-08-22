
FROM ubuntu as system_setup

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC
RUN apt-get update
RUN apt-get install build-essential -y
RUN apt-get install cmake -y
RUN apt-get install git -y 
RUN apt-get install ca-certificates -y
RUN apt-get install gperf -y
RUN apt-get install make git zlib1g-dev libssl-dev gperf php-cli cmake g++ -y --no-install-recommends
RUN git clone https://github.com/tdlib/td.git /app/lib/td

FROM system_setup as build
WORKDIR /app
COPY . /app
RUN mkdir /app/build
RUN cd /app/build && cmake ..
RUN cd /app/build && make


FROM build as app
RUN mkdir /output
RUN cd /output
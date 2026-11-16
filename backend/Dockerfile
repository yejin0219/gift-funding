FROM drogonframework/drogon
WORKDIR /app
COPY . .
RUN mkdir build && cd build && cmake .. && make -j2
CMD ["./build/gift_server"]
# Build the C++ engine and package only the executable plus the web UI.
FROM gcc:14-bookworm AS build
WORKDIR /src
COPY main.cpp .
RUN g++ -std=c++17 -O2 -pthread main.cpp -o /langtons_ant_server

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /langtons_ant_server ./langtons_ant_server
COPY index.html style.css app.js ./
ENV PORT=10000
ENV HOST=0.0.0.0
EXPOSE 10000
CMD ["./langtons_ant_server"]

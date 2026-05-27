# 2kewld

The 2Kewl Onion Daemon — runs a board node over Tor.

## Components

- `2kewld`     — C daemon, HTTP API over Tor hidden service
- `2kewld-cli` — Go CLI to configure and manage 2kewld

## Dependencies

- tor
- libsqlite3-dev
- libevent-dev
- libssl-dev
- Go 1.21+

## Build

```sh
make            # builds both 2kewld and 2kewld-cli
make daemon     # builds only 2kewld
make cli        # builds only 2kewld-cli
make clean
```

## Quick start

```sh
# Configure and launch a board
./2kewld-cli init --tag tech --title "Technology" --desc "Tech discussion"
./2kewld-cli start

# Or run the daemon directly
./2kewld --tag tech --title "Technology" --data-dir ~/.2kewld/tech
```

## API Endpoints (onion)

| Method | Path                        | Description              |
|--------|-----------------------------|--------------------------|
| GET    | /api/boards                 | Board info               |
| GET    | /api/[tag]/threads          | List threads (paginated) |
| GET    | /api/[tag]/thread/[id]      | Thread + replies         |
| POST   | /api/[tag]/post             | New thread               |
| POST   | /api/[tag]/thread/[id]/post | Reply to thread          |
| GET    | /api/[tag]/image/[hash]     | Serve image by sha256    |
| GET    | /health                     | Daemon health check      |

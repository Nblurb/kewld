# kewld

The Kewl Onion Daemon — runs a board node over Tor.

## Components

- `kewld`     — C daemon, HTTP API over Tor hidden service
- `kewld-cli` — Go CLI to configure and manage kewld

## Dependencies

- tor
- libsqlite3-dev
- libevent-dev
- libssl-dev
- Go 1.21+

## Build

```sh
make            # builds both kewld and kewld-cli
make daemon     # builds only kewld
make cli        # builds only kewld-cli
make clean
```

## Quick start

```sh
# Configure and launch a board
./kewld-cli init --tag tech --title "Technology" --desc "Tech discussion"
./kewld-cli start

# Or run the daemon directly
./kewld --tag tech --title "Technology" --data-dir ~/.kewld/tech
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

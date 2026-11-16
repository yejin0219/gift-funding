# gift-funding

A backend for **group-buying birthday gifts** with friends.
Create a funding room, let several people chip in, and let the server guarantee the total **never exceeds the target amount**.

Built with the **Drogon** C++ web framework and **PostgreSQL**, and run entirely with **Docker Compose**.
This is a personal project I am extending step by step as a networking / server / cloud portfolio.

> KakaoTalk Gift's ordering API is not open to general developers, so this service handles **fundraising and settlement records**. The organizer buys the actual gift manually.

## Features (implemented so far)

- Server health check (`/health`) and DB connectivity check (`/health/db`)
- Create a funding room
- Contribute to a room (over-target prevention, contribution records)
- Verified the over-target prevention with a 20-client concurrent request experiment

## Architecture

```
 Client (curl / browser)
        │  HTTP :8080
        ▼
┌───────────────────┐        ┌─────────────────────┐
│  server (C++)     │  SQL   │  db (PostgreSQL 16) │
│  Drogon           │ ─────▶ │  rooms              │
│                   │  :5432 │  contributions      │
└───────────────────┘        └─────────────────────┘
        └──── Docker Compose network (reached by the service name "db") ────┘
```

## Tech stack

| Role | Technology |
|---|---|
| Backend API | C++17, Drogon |
| Database | PostgreSQL 16 |
| Runtime | Docker, Docker Compose |
| Build | CMake |

## Getting started

### Prerequisites

- Docker Desktop (must be running)
- Git

### 1. Clone the repository

```bash
git clone https://github.com/yejin0219/gift-funding.git
cd gift-funding
```

### 2. Create the environment file

Copy `.env.example` to `.env` and fill in the values. `.env` is listed in `.gitignore`, so it is never pushed to GitHub.

```bash
cp .env.example .env        # Windows PowerShell: copy .env.example .env
```

```
POSTGRES_USER=your_user
POSTGRES_PASSWORD=your_password
POSTGRES_DB=giftfunding
```

### 3. Start the server and the database

```bash
docker compose up -d --build
```

The first run downloads the Drogon image and compiles the C++ code, which can take a few minutes.

### 4. Create the tables

Use the user name and DB name from your `.env` for `-U` and `-d`.

```bash
docker compose cp db/init.sql db:/tmp/init.sql
docker compose exec db psql -U <POSTGRES_USER> -d <POSTGRES_DB> -f /tmp/init.sql
```

Seeing `CREATE TABLE` twice means it worked.

### 5. Verify

```bash
curl http://localhost:8080/health       # ok
curl http://localhost:8080/health/db    # db ok
```

## API

| Method | Path | Description |
|---|---|---|
| GET | `/health` | Check that the server is alive |
| GET | `/health/db` | Check that the server can talk to the DB (503 on failure) |
| POST | `/rooms` | Create a funding room |
| POST | `/rooms/{id}/contributions` | Contribute to a room |

### Create a room

```bash
curl -X POST http://localhost:8080/rooms \
  -H "Content-Type: application/json" \
  -d '{"title":"Minsu birthday","target_amount":100000}'
```

```json
{ "id": 1 }
```

### Contribute

```bash
curl -X POST http://localhost:8080/rooms/1/contributions \
  -H "Content-Type: application/json" \
  -d '{"nickname":"chulsu","amount":60000}'
```

| Status | Meaning | Example response |
|---|---|---|
| 201 | Contribution accepted | `{"collected":60000,"target":100000}` |
| 400 | Invalid request body | `{"error":"..."}` |
| 404 | Room not found | `{"error":"..."}` |
| 409 | Would exceed the target | `{"error":"...","remaining":40000}` |
| 503 | Database error | `db error` |

When a contribution is rejected for exceeding the target, the response includes the **remaining amount** so the user can immediately retry with a smaller number.

## Design highlight: never exceed the target, even under concurrency

### The problem

Say the target is 100,000 and 90,000 has been collected. If two people each try to contribute 50,000 **at the same time**, a three-step flow of "read the current total → check it fits → add" lets both requests read 90,000 and pass the check, ending at **190,000**. This is a race condition (check-then-act): a gap exists between the check and the write.

### The solution

I merged the check and the write into **a single SQL statement**. Because the guarantee lives in the database rather than in a server-side mutex, it still holds when the server is scaled to multiple instances.

```sql
WITH upd AS (
    UPDATE rooms
    SET collected_amount = collected_amount + $2
    WHERE id = $1
      AND collected_amount + $2 <= target_amount   -- add only if it still fits
    RETURNING id, collected_amount, target_amount
),
ins AS (
    INSERT INTO contributions (room_id, nickname, amount)
    SELECT id, $3, $2 FROM upd                     -- record only if the add happened
)
SELECT collected_amount, target_amount FROM upd;
```

- If the condition fails, `upd` returns zero rows and no contribution record (`ins`) is created.
- The total and the record change inside one statement, so there is never a state where the total went up but no record exists.
- Values are passed separately as `$1`, `$2`, `$3` placeholders, which prevents SQL injection.

### A last line of defense

The table also has `CHECK (collected_amount <= target_amount)`. Even if the application code has a bug, the database still refuses to store an over-target total.

## Concurrency experiment

**Setup**: 20 clients send a 10,000 contribution **at almost the same time** to a room with a 100,000 target (200,000 attempted in total).

**Expectation**: exactly 10 succeed and 10 are rejected.

**Result**

| Item | Result |
|---|---|
| Successful responses | 10 |
| Rejected responses | 10 |
| DB `collected_amount` | 100000 (target 100000) |
| DB contribution records / sum | 10 / 100000 |

The number of responses seen by the clients matched the records actually stored in the database.

**Limitation**: I fired the requests with PowerShell `Start-Job`, so the 20 requests do not arrive at exactly the same instant. A stricter load test with a tool such as k6 is planned.

## Database

```
rooms                          contributions
─────────────────────          ─────────────────────
id (PK)                        id (PK)
title                          room_id  ──▶ rooms.id
target_amount (> 0)            nickname
collected_amount (<= target)   amount (> 0)
created_at                     created_at
```

The schema lives in [`db/init.sql`](db/init.sql).

## Project structure

```
gift-funding/
├── backend/            # C++ Drogon server (main.cpp, CMakeLists.txt, Dockerfile)
├── db/                 # DB schema (init.sql)
├── frontend/           # (planned)
├── docs/               # (planned)
├── docker-compose.yml  # runs server + db
├── .env.example        # environment variable template (real values go in .env, git-ignored)
└── README.md
```

## Roadmap

- [ ] Kakao login and KakaoTalk share links to invite friends
- [ ] Real-time funding status via WebSocket
- [ ] Toss Payments test-mode payments and webhooks (idempotent, so a webhook delivered twice is processed once)
- [ ] Apply `init.sql` automatically from `docker-compose.yml`
- [ ] AWS deployment (HTTPS, nginx) and GitHub Actions CI/CD
- [ ] k6 load testing and a write-up of the results
- [ ] Prometheus / Grafana monitoring

## License

MIT License
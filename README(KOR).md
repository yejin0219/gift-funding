# gift-funding

친구들과 생일 선물을 **공동구매(공구)** 하기 위한 모금 서비스의 백엔드입니다.
모금방을 만들고, 여러 명이 금액을 나눠 내고, 목표 금액을 **절대 넘지 않도록** 서버가 보장합니다.

C++ 웹 프레임워크 **Drogon**과 **PostgreSQL**로 만들었고, 전체를 **Docker Compose**로 실행합니다.
네트워크 / 서버 / 클라우드 포트폴리오를 목적으로 단계적으로 확장하고 있는 개인 프로젝트입니다.

> 카카오톡 선물하기의 주문 API는 일반 개발자에게 공개되어 있지 않아서, 이 서비스는 **모금과 정산 기록**을 담당하고 실제 선물 구매는 총무가 직접 합니다.

## 현재 구현된 기능

- 서버 상태 확인 (`/health`) 및 DB 연결 확인 (`/health/db`)
- 모금방 생성
- 모금 참여 (목표 금액 초과 방지, 참여 기록 저장)
- 20명 동시 요청 실험으로 초과 방지 검증

## 아키텍처

```
 Client (curl / 브라우저)
        │  HTTP :8080
        ▼
┌───────────────────┐        ┌─────────────────────┐
│  server (C++)     │  SQL   │  db (PostgreSQL 16) │
│  Drogon           │ ─────▶ │  rooms              │
│                   │  :5432 │  contributions      │
└───────────────────┘        └─────────────────────┘
        └──────── Docker Compose 네트워크 (서비스 이름 "db"로 접속) ────────┘
```

## 기술 스택

| 역할 | 사용 기술 |
|---|---|
| 백엔드 API | C++17, Drogon |
| 데이터베이스 | PostgreSQL 16 |
| 실행 환경 | Docker, Docker Compose |
| 빌드 | CMake |

## 실행 방법

### 준비물

- Docker Desktop (실행 중이어야 합니다)
- Git

### 1. 저장소 받기

```bash
git clone https://github.com/yejin0219/gift-funding.git
cd gift-funding
```

### 2. 환경 변수 파일 만들기

`.env.example`을 복사해서 `.env`를 만들고 값을 채웁니다. `.env`는 `.gitignore`에 들어 있어 GitHub에 올라가지 않습니다.

```bash
cp .env.example .env        # Windows PowerShell: copy .env.example .env
```

```
POSTGRES_USER=원하는_아이디
POSTGRES_PASSWORD=원하는_비밀번호
POSTGRES_DB=giftfunding
```

### 3. 서버와 DB 실행

```bash
docker compose up -d --build
```

처음 실행하면 Drogon 이미지를 받고 C++ 코드를 컴파일하느라 몇 분 걸립니다.

### 4. 테이블 만들기

`.env`에 적은 아이디와 DB 이름을 `-U`, `-d` 뒤에 넣습니다.

```bash
docker compose cp db/init.sql db:/tmp/init.sql
docker compose exec db psql -U <POSTGRES_USER> -d <POSTGRES_DB> -f /tmp/init.sql
```

`CREATE TABLE`이 두 번 출력되면 성공입니다.

### 5. 동작 확인

```bash
curl http://localhost:8080/health       # ok
curl http://localhost:8080/health/db    # db ok
```

## API

| 메서드 | 주소 | 설명 |
|---|---|---|
| GET | `/health` | 서버가 살아 있는지 확인 |
| GET | `/health/db` | 서버가 DB와 통신 가능한지 확인 (실패 시 503) |
| POST | `/rooms` | 모금방 생성 |
| POST | `/rooms/{id}/contributions` | 모금 참여 |

### 모금방 만들기

```bash
curl -X POST http://localhost:8080/rooms \
  -H "Content-Type: application/json" \
  -d '{"title":"Minsu birthday","target_amount":100000}'
```

```json
{ "id": 1 }
```

### 모금 참여하기

```bash
curl -X POST http://localhost:8080/rooms/1/contributions \
  -H "Content-Type: application/json" \
  -d '{"nickname":"chulsu","amount":60000}'
```

| 상태 | 의미 | 응답 예시 |
|---|---|---|
| 201 | 참여 성공 | `{"collected":60000,"target":100000}` |
| 400 | 요청 형식 오류 | `{"error":"..."}` |
| 404 | 없는 모금방 | `{"error":"..."}` |
| 409 | 목표 금액 초과 | `{"error":"...","remaining":40000}` |
| 503 | DB 오류 | `db error` |

목표를 넘어서 거절될 때는 **남은 금액(`remaining`)** 을 함께 돌려줍니다. 사용자가 "그럼 남은 만큼만 낼게요"라고 바로 다시 시도할 수 있게 하기 위해서입니다.

## 설계 포인트: 동시에 요청이 와도 목표 금액을 넘지 않게

### 문제

목표가 10만 원이고 9만 원이 모인 상태에서 두 명이 **동시에** 5만 원을 내면 어떻게 될까요?
"현재 금액을 읽고 → 넘지 않는지 확인하고 → 더하는" 세 단계로 처리하면, 두 요청이 모두 9만 원을 읽고 확인을 통과해서 **총 19만 원**이 되어버립니다. 확인과 쓰기 사이의 틈 때문에 생기는 레이스 컨디션(check-then-act)입니다.

### 해결

확인과 쓰기를 **SQL 한 문장**으로 합쳤습니다. 서버 코드에서 mutex로 막지 않고 DB에서 막기 때문에, 서버를 여러 대로 늘려도 안전합니다.

```sql
WITH upd AS (
    UPDATE rooms
    SET collected_amount = collected_amount + $2
    WHERE id = $1
      AND collected_amount + $2 <= target_amount   -- 넘지 않을 때만 더함
    RETURNING id, collected_amount, target_amount
),
ins AS (
    INSERT INTO contributions (room_id, nickname, amount)
    SELECT id, $3, $2 FROM upd                     -- 더해진 경우에만 영수증 저장
)
SELECT collected_amount, target_amount FROM upd;
```

- 조건을 만족하지 않으면 `upd`가 0줄을 반환하고, 영수증(`ins`)도 만들어지지 않습니다.
- 금액 증가와 영수증 기록이 한 문장 안에서 처리되어, "금액은 올랐는데 영수증이 없는" 어정쩡한 상태가 생기지 않습니다.
- 값은 `$1`, `$2`, `$3` 자리표시자로 따로 전달해서 SQL 인젝션을 막습니다.

### 마지막 안전장치

DB 표에도 `CHECK (collected_amount <= target_amount)` 제약을 걸어두었습니다. 애플리케이션 코드에 버그가 생겨도 DB가 마지막으로 초과 저장을 거부합니다.

## 동시성 실험

**조건**: 목표 10만 원인 방에 20명이 **거의 동시에** 1만 원씩 요청 (총 20만 원 시도)

**기대**: 정확히 10명만 성공, 나머지 10명은 거절

**결과**

| 항목 | 결과 |
|---|---|
| 성공 응답 | 10 |
| 거절 응답 | 10 |
| DB `collected_amount` | 100000 (목표 100000) |
| DB 영수증 수 / 합계 | 10장 / 100000 |

화면에 찍힌 응답 수와 DB에 실제로 남은 기록이 일치함을 확인했습니다.

**한계**: PowerShell의 `Start-Job`으로 요청을 쏘았기 때문에 20개가 정확히 같은 순간에 도착하지는 않습니다. 더 엄밀한 부하 테스트는 k6 같은 도구로 진행할 예정입니다.

## 데이터베이스

```
rooms                          contributions
─────────────────────          ─────────────────────
id (PK)                        id (PK)
title                          room_id  ──▶ rooms.id
target_amount (> 0)            nickname
collected_amount (<= target)   amount (> 0)
created_at                     created_at
```

스키마는 [`db/init.sql`](db/init.sql)에 있습니다.

## 프로젝트 구조

```
gift-funding/
├── backend/            # C++ Drogon 서버 (main.cpp, CMakeLists.txt, Dockerfile)
├── db/                 # DB 스키마 (init.sql)
├── frontend/           # (예정)
├── docs/               # (예정)
├── docker-compose.yml  # server + db 실행 설정
├── .env.example        # 환경 변수 양식 (실제 값은 .env, Git 제외)
└── README.md
```

## 로드맵

- [ ] 카카오 로그인, 카카오톡 공유하기로 친구에게 링크 전달
- [ ] WebSocket으로 모금 현황 실시간 반영
- [ ] 토스페이먼츠 테스트 결제와 웹훅 (같은 웹훅이 두 번 와도 한 번만 처리하는 멱등성 포함)
- [ ] `docker-compose.yml`에서 `init.sql` 자동 적용
- [ ] AWS 배포 (HTTPS, nginx), GitHub Actions 자동 배포
- [ ] k6 부하 테스트와 결과 정리
- [ ] Prometheus / Grafana 모니터링

## 라이선스

MIT License
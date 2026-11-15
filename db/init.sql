-- 모금방 표: 모금함 하나당 한 줄
CREATE TABLE rooms (
    id               SERIAL PRIMARY KEY,                    -- 방 번호 (자동으로 1, 2, 3...)
    title            TEXT NOT NULL,                         -- 모금방 제목
    target_amount    INTEGER NOT NULL CHECK (target_amount > 0),  -- 목표 금액 (0원보다 커야 함)
    collected_amount INTEGER NOT NULL DEFAULT 0,            -- 지금까지 모인 금액 (처음엔 0)
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),    -- 만든 시간 (자동 기록)
    CHECK (collected_amount <= target_amount)               -- 안전장치: 목표를 넘으면 저장 거부
);

-- 참여 기록 표: 누가 얼마 냈는지 영수증 한 장당 한 줄
CREATE TABLE contributions (
    id         SERIAL PRIMARY KEY,                          -- 영수증 번호
    room_id    INTEGER NOT NULL REFERENCES rooms(id),       -- 어느 모금방에 낸 건지
    nickname   TEXT NOT NULL,                               -- 낸 사람 이름
    amount     INTEGER NOT NULL CHECK (amount > 0),         -- 낸 금액 (0원보다 커야 함)
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()           -- 낸 시간 (자동 기록)
);
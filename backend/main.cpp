#include <drogon/drogon.h>
#include <cstdlib>
#include <string>

// 환경변수(.env에서 온 값)를 읽어오는 도우미 함수
static std::string getEnv(const char *key) {
    const char *v = std::getenv(key);
    return v ? v : "";
}

int main() {
    // 서버가 살아 있는지만 확인 (기존 그대로)
    drogon::app().registerHandler(
        "/health",
        [](const drogon::HttpRequestPtr &,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setBody("ok");
            callback(resp);
        },
        {drogon::Get});

    // DB 접속 정보 등록 (호스트 이름은 "db")
    drogon::app().createDbClient(
        "postgresql", "db", 5432,
        getEnv("POSTGRES_DB"),
        getEnv("POSTGRES_USER"),
        getEnv("POSTGRES_PASSWORD"),
        10);

    // DB까지 살아 있는지 확인 (새로 추가)
    drogon::app().registerHandler(
        "/health/db",
        [](const drogon::HttpRequestPtr &,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto db = drogon::app().getDbClient();
            db->execSqlAsync(
                "SELECT 1",
                [callback](const drogon::orm::Result &) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setBody("db ok");
                    callback(resp);
                },
                [callback](const drogon::orm::DrogonDbException &) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k503ServiceUnavailable);
                    resp->setBody("db error");
                    callback(resp);
                });
        },
        {drogon::Get});

    // 모금방 만들기: POST /rooms
    drogon::app().registerHandler(
        "/rooms",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            // 1) 받은 JSON 검사
            auto json = req->getJsonObject();
            if (!json || !(*json)["title"].isString() ||
                (*json)["title"].asString().empty() ||
                !(*json)["target_amount"].isInt() ||
                (*json)["target_amount"].asInt() <= 0) {
                Json::Value err;
                err["error"] = "title(글자)와 target_amount(0보다 큰 정수)가 필요해요";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
                resp->setStatusCode(drogon::k400BadRequest);
                callback(resp);
                return;
            }

            std::string title = (*json)["title"].asString();
            int target = (*json)["target_amount"].asInt();

            // 2) DB에 저장하고, 새 방 번호를 돌려받기
            auto db = drogon::app().getDbClient();
            db->execSqlAsync(
                "INSERT INTO rooms (title, target_amount) VALUES ($1, $2) RETURNING id",
                [callback](const drogon::orm::Result &r) {
                    Json::Value out;
                    out["id"] = r[0]["id"].as<int>();
                    auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                    resp->setStatusCode(drogon::k201Created);
                    callback(resp);
                },
                [callback](const drogon::orm::DrogonDbException &) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k503ServiceUnavailable);
                    resp->setBody("db error");
                    callback(resp);
                },
                title, target);
        },
        {drogon::Post});

        // 모금 참여: POST /rooms/{방번호}/contributions
    drogon::app().registerHandler(
        "/rooms/{1}/contributions",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback,
           int roomId) {
            // 1) 받은 JSON 검사
            auto json = req->getJsonObject();
            if (!json || !(*json)["nickname"].isString() ||
                (*json)["nickname"].asString().empty() ||
                !(*json)["amount"].isInt() ||
                (*json)["amount"].asInt() <= 0) {
                Json::Value err;
                err["error"] = "nickname(글자)와 amount(0보다 큰 정수)가 필요해요";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
                resp->setStatusCode(drogon::k400BadRequest);
                callback(resp);
                return;
            }

            std::string nickname = (*json)["nickname"].asString();
            int amount = (*json)["amount"].asInt();

            // 2) "목표를 안 넘을 때만 더하고, 영수증도 같이 저장" (SQL 한 문장)
            auto db = drogon::app().getDbClient();
            db->execSqlAsync(
                R"(WITH upd AS (
                       UPDATE rooms
                       SET collected_amount = collected_amount + $2::int
                       WHERE id = $1::int
                         AND collected_amount + $2::int <= target_amount
                       RETURNING id, collected_amount, target_amount
                   ),
                   ins AS (
                       INSERT INTO contributions (room_id, nickname, amount)
                       SELECT id, $3, $2::int FROM upd
                   )
                   SELECT collected_amount, target_amount FROM upd)",
                [callback, db, roomId](const drogon::orm::Result &r) {
                    if (!r.empty()) {
                        // 성공: 현재 모금 현황을 돌려줌
                        Json::Value out;
                        out["collected"] = r[0]["collected_amount"].as<int>();
                        out["target"] = r[0]["target_amount"].as<int>();
                        auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                        resp->setStatusCode(drogon::k201Created);
                        callback(resp);
                        return;
                    }
                    // 실패: 방이 없는 건지, 금액이 넘치는 건지 확인
                    db->execSqlAsync(
                        "SELECT target_amount - collected_amount AS remaining "
                        "FROM rooms WHERE id = $1::int",
                        [callback](const drogon::orm::Result &r2) {
                            Json::Value out;
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            if (r2.empty()) {
                                out["error"] = "없는 모금방이에요";
                                resp = drogon::HttpResponse::newHttpJsonResponse(out);
                                resp->setStatusCode(drogon::k404NotFound);
                            } else {
                                out["error"] = "목표 금액을 넘어요";
                                out["remaining"] = r2[0]["remaining"].as<int>();
                                resp = drogon::HttpResponse::newHttpJsonResponse(out);
                                resp->setStatusCode(drogon::k409Conflict);
                            }
                            callback(resp);
                        },
                        [callback](const drogon::orm::DrogonDbException &) {
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            resp->setStatusCode(drogon::k503ServiceUnavailable);
                            resp->setBody("db error");
                            callback(resp);
                        },
                        roomId);
                },
                [callback](const drogon::orm::DrogonDbException &) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k503ServiceUnavailable);
                    resp->setBody("db error");
                    callback(resp);
                },
                roomId, amount, nickname);
        },
        {drogon::Post});

    drogon::app().addListener("0.0.0.0", 8080).run();
    return 0;
}
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
        1);

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

    drogon::app().addListener("0.0.0.0", 8080).run();
    return 0;
}
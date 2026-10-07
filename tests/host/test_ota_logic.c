/* Host tests for src/cloud/ota_logic.c — parsing OTA commands, the status
 * message, the digest check and the application-update sequence (with fake
 * download / verify / rename ops). */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ota_logic.h"
#include "unity.h"

#define APP_T "devices/GW-1/ota/app"
#define SYS_T "devices/GW-1/ota/system"
#define SHA "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------- parsing */

static void test_app_request_parsed(void)
{
    ota_request_t r;
    TEST_ASSERT_EQUAL_INT(OTA_REQ_OK, ota_parse_request(APP_T,
        "{\"url\":\"https://s3.example/main?X-Amz=1\",\"version\":\"1.2.0\",\"sha256\":\"" SHA "\"}",
        APP_T, SYS_T, &r));
    TEST_ASSERT_EQUAL_INT(OTA_TYPE_APP, r.type);
    TEST_ASSERT_EQUAL_STRING("https://s3.example/main?X-Amz=1", r.url);
    TEST_ASSERT_EQUAL_STRING("1.2.0", r.version);
    TEST_ASSERT_EQUAL_STRING(SHA, r.sha256);
    TEST_ASSERT_EQUAL_INT(0, r.pending);
}

static void test_system_request_ignores_sha256(void)
{
    ota_request_t r;
    TEST_ASSERT_EQUAL_INT(OTA_REQ_OK, ota_parse_request(SYS_T,
        "{\"url\":\"https://x/update.swu\",\"version\":\"5\",\"sha256\":\"ab\"}", APP_T, SYS_T, &r));
    TEST_ASSERT_EQUAL_INT(OTA_TYPE_SYSTEM, r.type);
    TEST_ASSERT_EQUAL_STRING("", r.sha256);
}

static void test_other_topic_or_empty_payload_ignored(void)
{
    ota_request_t r;
    TEST_ASSERT_EQUAL_INT(OTA_REQ_NOT_OTA, ota_parse_request("devices/GW-2/ota/app", "{\"url\":\"x\"}", APP_T, SYS_T, &r));
    TEST_ASSERT_EQUAL_INT(OTA_REQ_NOT_OTA, ota_parse_request(APP_T, "", APP_T, SYS_T, &r));
    TEST_ASSERT_EQUAL_INT(OTA_REQ_NOT_OTA, ota_parse_request(NULL, "{}", APP_T, SYS_T, &r));
}

static void test_missing_url(void)
{
    ota_request_t r;
    TEST_ASSERT_EQUAL_INT(OTA_REQ_MISSING_URL, ota_parse_request(APP_T, "{\"version\":\"1\"}", APP_T, SYS_T, &r));
}

static void test_non_https_url_rejected(void)
{
    /* Regression: any scheme was accepted, and libcurl also downloads file://, http://, ftp://. */
    ota_request_t r;
    const char *bad[] = { "file:///etc/shadow", "http://x/main", "ftp://x/main", "//x/main" };
    for (int i = 0; i < 4; i++) {
        char payload[200];
        snprintf(payload, sizeof(payload), "{\"url\":\"%s\",\"sha256\":\"" SHA "\"}", bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(OTA_REQ_BAD_URL, ota_parse_request(APP_T, payload, APP_T, SYS_T, &r), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(OTA_REQ_OK, ota_parse_request(APP_T, "{\"url\":\"HTTPS://x/main\",\"sha256\":\"" SHA "\"}", APP_T, SYS_T, &r));
    TEST_ASSERT_EQUAL_STRING("invalid_url", ota_parse_error(OTA_REQ_BAD_URL));
}

static void test_long_presigned_url_not_truncated(void)
{
    /* Regression: url[512] silently cut longer S3 pre-signed URLs. */
    static char payload[6000], url[5000];
    memset(url, 'a', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[1099] = '\0';                                    /* 1099 characters: fits */
    snprintf(payload, sizeof(payload), "{\"url\":\"%s\",\"sha256\":\"" SHA "\"}", url);
    ota_request_t r;
    TEST_ASSERT_EQUAL_INT(OTA_REQ_OK, ota_parse_request(APP_T, payload, APP_T, SYS_T, &r));
    TEST_ASSERT_EQUAL_STRING(url, r.url);

    url[1099] = 'a';
    url[sizeof(r.url)] = '\0';                           /* one more than fits: rejected, not cut */
    snprintf(payload, sizeof(payload), "{\"url\":\"%s\",\"sha256\":\"" SHA "\"}", url);
    TEST_ASSERT_EQUAL_INT(OTA_REQ_URL_TOO_LONG, ota_parse_request(APP_T, payload, APP_T, SYS_T, &r));
}

static void test_escaped_slashes_in_url(void)
{
    /* Regression: JSON escapes such as \/ were not decoded. */
    ota_request_t r;
    ota_parse_request(APP_T, "{\"url\":\"https:\\/\\/s3.example\\/main\",\"sha256\":\"" SHA "\"}", APP_T, SYS_T, &r);
    TEST_ASSERT_EQUAL_STRING("https://s3.example/main", r.url);
}

static void test_app_request_needs_valid_sha256(void)
{
    /* Regression: an app command without sha256 was installed unverified. */
    ota_request_t r;
    const char *bad[] = {
        "{\"url\":\"https://x/m\"}",
        "{\"url\":\"https://x/m\",\"sha256\":\"\"}",
        "{\"url\":\"https://x/m\",\"sha256\":\"9f86d081\"}",
        "{\"url\":\"https://x/m\",\"sha256\":\"zz86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\"}",
        "{\"url\":\"https://x/m\",\"sha256\":\"" SHA "00\"}",
    };
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(OTA_REQ_BAD_SHA256, ota_parse_request(APP_T, bad[i], APP_T, SYS_T, &r), bad[i]);
    TEST_ASSERT_EQUAL_STRING("invalid_sha256", ota_parse_error(OTA_REQ_BAD_SHA256));
    TEST_ASSERT_NULL(ota_parse_error(OTA_REQ_MISSING_URL));
    /* a system update (swupdate checks its own image) needs none */
    TEST_ASSERT_EQUAL_INT(OTA_REQ_OK, ota_parse_request(SYS_T, "{\"url\":\"https://x/u.swu\"}", APP_T, SYS_T, &r));
}

/* --------------------------------------------------------- status message */

static void test_status_json(void)
{
    char buf[512];
    ota_status_json(buf, sizeof(buf), OTA_TYPE_APP, OTA_STATUS_FAILED, "1.2.0", "1.1.0", "download_failed", 1767225600);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"app\",\"status\":\"FAILED\",\"version\":\"1.2.0\","
                             "\"previous_version\":\"1.1.0\",\"error\":\"download_failed\",\"timestamp\":1767225600}", buf);
    ota_status_json(buf, sizeof(buf), OTA_TYPE_SYSTEM, OTA_STATUS_STARTED, NULL, "1.1.0", NULL, 0);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"system\",\"status\":\"STARTED\",\"version\":\"\","
                             "\"previous_version\":\"1.1.0\",\"error\":\"\",\"timestamp\":0}", buf);
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("SUCCEEDED", ota_status_name(OTA_STATUS_SUCCEEDED));
    TEST_ASSERT_EQUAL_STRING("system", ota_type_name(OTA_TYPE_SYSTEM));
}

/* ------------------------------------------------------------ digest check */

static void test_digest_matches_case_insensitive(void)
{
    unsigned char d[32];
    for (int i = 0; i < 32; i++)
        d[i] = (unsigned char)(i * 7 + 1);
    char hex[65];
    ota_digest_matches(d, "", hex);
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(hex));
    TEST_ASSERT_EQUAL_STRING_LEN("01080f16", hex, 8);
    char lower[65], upper[65], out[65];
    strcpy(lower, hex);
    for (int i = 0; i < 65; i++)
        upper[i] = (hex[i] >= 'a' && hex[i] <= 'f') ? (char)(hex[i] - 32) : hex[i];
    TEST_ASSERT_TRUE(ota_digest_matches(d, lower, out));
    TEST_ASSERT_TRUE(ota_digest_matches(d, upper, out));
    upper[63] = upper[63] == '0' ? '1' : '0';
    TEST_ASSERT_FALSE(ota_digest_matches(d, upper, out));
    TEST_ASSERT_FALSE(ota_digest_matches(d, "ab", out));
}

/* ------------------------------------------------------------ app update */

typedef struct {
    int  download_ok, verify_ok;
    int  rename_err[4];              /* result of the n-th rename call */
    int  renames, verifies, chmods;
    char log[16][128];               /* "report STATUS error", "rename a>b", "unlink p" */
    int  n;
} app_fake_t;

static app_fake_t af;

static void logf_(const char *s) { snprintf(af.log[af.n++], sizeof(af.log[0]), "%s", s); }

static int a_download(void *c, const char *u, const char *p, long t) { (void)c; (void)u; (void)p; (void)t; return af.download_ok; }
static int a_verify(void *c, const char *p, const char *h) { (void)c; (void)p; (void)h; af.verifies++; return af.verify_ok; }
static int a_rename(void *c, const char *from, const char *to)
{
    (void)c;
    char s[128];
    snprintf(s, sizeof(s), "rename %s>%s", from, to);
    logf_(s);
    return af.rename_err[af.renames++];
}
static void a_unlink(void *c, const char *p) { (void)c; char s[128]; snprintf(s, sizeof(s), "unlink %s", p); logf_(s); }
static void a_chmod(void *c, const char *p) { (void)c; (void)p; af.chmods++; }
static void a_report(void *c, ota_status_t st, const char *v, const char *e)
{
    (void)c; (void)v;
    char s[128];
    snprintf(s, sizeof(s), "report %s %s", ota_status_name(st), e);
    logf_(s);
}

static const ota_app_ops_t aops = { NULL, a_download, a_verify, a_rename, a_unlink, a_chmod, a_report };

static ota_request_t app_req(const char *sha)
{
    ota_request_t r;
    memset(&r, 0, sizeof(r));
    r.type = OTA_TYPE_APP;
    strcpy(r.url, "https://x/main");
    strcpy(r.version, "2.0");
    strcpy(r.sha256, sha);
    return r;
}

static void reset_app(void) { memset(&af, 0, sizeof(af)); af.download_ok = 1; af.verify_ok = 1; }

static void test_app_update_happy_path(void)
{
    reset_app();
    ota_request_t r = app_req("ab");
    TEST_ASSERT_EQUAL_INT(1, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
    const char *want[] = {
        "report DOWNLOADING ", "report VERIFYING ", "report APPLYING ",
        "rename /app/main>/app/main.bak", "rename /tmp/ota/main.new>/app/main", "report SUCCEEDED ",
    };
    TEST_ASSERT_EQUAL_INT(6, af.n);
    for (int i = 0; i < 6; i++)
        TEST_ASSERT_EQUAL_STRING(want[i], af.log[i]);
    TEST_ASSERT_EQUAL_INT(1, af.chmods);
}

static void test_download_failure(void)
{
    reset_app();
    af.download_ok = 0;
    ota_request_t r = app_req("ab");
    TEST_ASSERT_EQUAL_INT(0, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
    TEST_ASSERT_EQUAL_STRING("report FAILED download_failed", af.log[af.n - 1]);
    TEST_ASSERT_EQUAL_INT(0, af.renames);
}

static void test_sha_mismatch_deletes_download(void)
{
    reset_app();
    af.verify_ok = 0;
    ota_request_t r = app_req("ab");
    TEST_ASSERT_EQUAL_INT(0, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
    TEST_ASSERT_EQUAL_STRING("unlink /tmp/ota/main.new", af.log[af.n - 2]);
    TEST_ASSERT_EQUAL_STRING("report FAILED sha256_mismatch", af.log[af.n - 1]);
    TEST_ASSERT_EQUAL_INT(0, af.renames);
}

static void test_first_install_without_running_binary(void)
{
    reset_app();
    af.rename_err[0] = ENOENT;
    ota_request_t r = app_req("ab");
    TEST_ASSERT_EQUAL_INT(1, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
}

static void test_backup_failure_keeps_running_binary(void)
{
    reset_app();
    af.rename_err[0] = EACCES;
    ota_request_t r = app_req("ab");
    TEST_ASSERT_EQUAL_INT(0, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
    TEST_ASSERT_EQUAL_STRING("unlink /tmp/ota/main.new", af.log[af.n - 2]);
    TEST_ASSERT_EQUAL_STRING("report FAILED backup_failed", af.log[af.n - 1]);
    TEST_ASSERT_EQUAL_INT(1, af.renames);
}

static void test_replace_failure_restores_backup(void)
{
    reset_app();
    af.rename_err[1] = EXDEV;
    ota_request_t r = app_req("ab");
    TEST_ASSERT_EQUAL_INT(0, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
    TEST_ASSERT_EQUAL_STRING("rename /app/main.bak>/app/main", af.log[af.n - 2]);
    TEST_ASSERT_EQUAL_STRING("report FAILED replace_failed", af.log[af.n - 1]);
    TEST_ASSERT_EQUAL_INT(0, af.chmods);
}

static void test_update_without_sha256_not_installed(void)
{
    /* Regression: an empty sha256 skipped verification and installed the download. */
    reset_app();
    ota_request_t r = app_req("");
    TEST_ASSERT_EQUAL_INT(0, ota_apply_app(&aops, &r, "/tmp/ota", "/app/main"));
    TEST_ASSERT_EQUAL_INT(1, af.n);
    TEST_ASSERT_EQUAL_STRING("report FAILED sha256_missing", af.log[0]);
    TEST_ASSERT_EQUAL_INT(0, af.renames);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_app_request_parsed);
    RUN_TEST(test_system_request_ignores_sha256);
    RUN_TEST(test_other_topic_or_empty_payload_ignored);
    RUN_TEST(test_missing_url);
    RUN_TEST(test_non_https_url_rejected);
    RUN_TEST(test_long_presigned_url_not_truncated);
    RUN_TEST(test_escaped_slashes_in_url);
    RUN_TEST(test_app_request_needs_valid_sha256);
    RUN_TEST(test_status_json);
    RUN_TEST(test_names);
    RUN_TEST(test_digest_matches_case_insensitive);
    RUN_TEST(test_app_update_happy_path);
    RUN_TEST(test_download_failure);
    RUN_TEST(test_sha_mismatch_deletes_download);
    RUN_TEST(test_first_install_without_running_binary);
    RUN_TEST(test_backup_failure_keeps_running_binary);
    RUN_TEST(test_replace_failure_restores_backup);
    RUN_TEST(test_update_without_sha256_not_installed);
    return UNITY_END();
}

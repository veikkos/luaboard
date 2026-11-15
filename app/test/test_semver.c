#include "check.h"
#include "semver.h"

#include <string.h>

void test_semver(int *total, int *failed);

/* App versions (app.json "version"): what counts as newer when the
   marketplace offers an update. */
void test_semver(int *total, int *failed)
{
    CHECK(semver_cmp("1.0.1", "1.0.0") > 0);
    CHECK(semver_cmp("1.0.0", "1.0.1") < 0);
    CHECK(semver_cmp("1.10.0", "1.9.0") > 0);              /* numbers, not text */
    CHECK(semver_cmp("2.0.0", "1.99.99") > 0);
    CHECK(semver_cmp("1.0", "1.0.0") == 0);                /* missing parts are 0 */
    CHECK(semver_cmp("1", "1.0.0") == 0);
    CHECK(semver_cmp("1.0.0+a1b2c3d-dirty", "1.0.0") == 0); /* a dev build of 1.0.0 is 1.0.0 */
    CHECK(semver_cmp("1.0.0+abc", "1.0.1") < 0);
    CHECK(semver_cmp("1.0.0-beta.1", "1.0.0") < 0);        /* a pre-release comes before its release */
    CHECK(semver_cmp("1.0.0-beta.2", "1.0.0-beta.1") > 0);
    CHECK(semver_cmp("1.0.1-beta.1", "1.0.0") > 0);
    CHECK(semver_cmp("", "0.0.1") < 0);                    /* unreadable: older than anything */
    CHECK(semver_cmp("abc", "0.0.1") < 0);
    CHECK(semver_cmp("0.0.1", "abc") > 0);
    CHECK(semver_cmp("abc", "") == 0);
    CHECK(semver_cmp("v1.0.0", "0.0.1") < 0);
    CHECK(semver_cmp(NULL, "0.0.1") < 0);

    semver_t v;
    CHECK(semver_parse("3.2.1-rc.1+x", &v) && v.major == 3 && v.minor == 2 && v.patch == 1 && !strcmp(v.pre, "rc.1"));
    CHECK(!semver_parse("1.2.3.4", &v));
    CHECK(!semver_parse("1..2", &v));
    CHECK(!semver_parse("1.2.", &v));
    CHECK(!semver_parse("-1.0.0", &v));
    CHECK(!semver_parse("1.0.0-", &v));
    CHECK(!semver_parse("99999999999999999999.0.0", &v));  /* too big a number */

    *total += check_total;
    *failed += check_failed;
}

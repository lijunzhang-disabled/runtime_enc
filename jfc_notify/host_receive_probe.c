#define _GNU_SOURCE

/* Host-only resource probe. Build against the target's installed URMA headers.
 * No sender or remote endpoint is created. No notification is expected.
 */
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include <urma_api.h>

enum { QUEUE_DEPTH = 16, RECEIVE_BYTES = 64, EID_DISPLAY_LIMIT = 8 };

static void usage(const char *program)
{
    printf("Usage: %s --list\n"
           "       %s --device NAME --eid-index INDEX\n"
           "List mode enumerates devices/EIDs without creating queues.\n"
           "Resource mode posts one receive, polls once, then deletes its resources.\n"
           "No SEND, remote READ/WRITE, AICPU execution, or graph replay is tested.\n",
           program, program);
}

static bool check_status(const char *step, int rc)
{
    int saved_errno = errno;
    if (rc == 0) {
        printf("step=%s OK\n", step);
        return true;
    }
    printf("step=%s FAIL rc=%d errno=%d (%s)\n", step, rc, saved_errno,
           saved_errno ? strerror(saved_errno) : "not supplied by API");
    return false;
}

static bool check_pointer(const char *step, const void *pointer)
{
    int saved_errno = errno;
    if (pointer) {
        printf("step=%s OK\n", step);
        return true;
    }
    printf("step=%s FAIL null_result errno=%d (%s)\n", step, saved_errno,
           saved_errno ? strerror(saved_errno) : "not supplied by API");
    return false;
}

static void print_eid(const urma_eid_t *eid)
{
    for (size_t i = 0; i < sizeof(eid->raw); ++i) {
        printf("%02x", eid->raw[i]);
    }
}

static void print_library(void)
{
    Dl_info info = {0};
    if (dladdr((void *)urma_init, &info) && info.dli_fname) {
        char *resolved = realpath(info.dli_fname, NULL);
        printf("loaded_urma=%s\n", resolved ? resolved : info.dli_fname);
        free(resolved);
    } else {
        printf("loaded_urma=UNKNOWN\n");
    }
}

static bool print_devices(urma_device_t **devices, int count)
{
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        uint32_t eid_count = 0;
        errno = 0;
        urma_eid_info_t *eids = urma_get_eid_list(devices[i], &eid_count);
        int saved_errno = errno;
        printf("device=%s transport=%d eid_count=%" PRIu32 "\n",
               devices[i]->name, (int)devices[i]->type, eid_count);
        if (!eids) {
            printf("  eid_list=UNAVAILABLE errno=%d\n", saved_errno);
            ok = false;
            continue;
        }
        uint32_t shown = eid_count < EID_DISPLAY_LIMIT ? eid_count : EID_DISPLAY_LIMIT;
        for (uint32_t j = 0; j < shown; ++j) {
            printf("  eid_index=%" PRIu32 " eid=", eids[j].eid_index);
            print_eid(&eids[j].eid);
            putchar('\n');
        }
        if (eid_count > shown) {
            printf("  omitted_eids=%" PRIu32 "\n", eid_count - shown);
        }
        urma_free_eid_list(eids);
    }
    return ok;
}

static bool validate_eid(urma_device_t *device, uint32_t index)
{
    uint32_t count = 0;
    errno = 0;
    urma_eid_info_t *eids = urma_get_eid_list(device, &count);
    if (!check_pointer("get_eid_list", eids)) {
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (eids[i].eid_index == index) {
            printf("selected_device=%s eid_index=%" PRIu32 " eid=", device->name, index);
            print_eid(&eids[i].eid);
            putchar('\n');
            found = true;
            break;
        }
    }
    urma_free_eid_list(eids);
    if (!found) {
        printf("step=select_eid FAIL index_not_listed=%" PRIu32 "\n", index);
    }
    return found;
}

static bool random_token(urma_token_t *token)
{
    size_t done = 0;
    while (done < sizeof(token->token)) {
        ssize_t n = getrandom((char *)&token->token + done, sizeof(token->token) - done, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            printf("step=random_token FAIL errno=%d\n", errno);
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static bool receive_resources(urma_device_t *device, uint32_t index, bool *cleanup_ok)
{
    urma_context_t *context = NULL;
    urma_jfce_t *jfce = NULL;
    urma_jfc_t *jfc = NULL;
    urma_jfr_t *jfr = NULL;
    urma_target_seg_t *segment = NULL;
    void *buffer = NULL;
    bool ok = false;
    urma_token_t token = {0};
    urma_device_attr_t attr = {0};
    urma_jfc_cfg_t jfc_cfg = {0};
    urma_jfr_cfg_t jfr_cfg = {0};
    urma_seg_cfg_t seg_cfg = {0};
    urma_sge_t sge = {0};
    urma_jfr_wr_t wr = {0}, *bad_wr = NULL;
    urma_cr_t cr = {0};

    *cleanup_ok = true;
    errno = 0;
    if (!check_status("query_device", urma_query_device(device, &attr))) {
        return false;
    }
    printf("capabilities trans_mode=0x%x max_jfc_depth=%" PRIu32
           " max_jfr_depth=%" PRIu32 " max_jfr_sge=%" PRIu32 "\n",
           (unsigned)attr.dev_cap.trans_mode, attr.dev_cap.max_jfc_depth,
           attr.dev_cap.max_jfr_depth, attr.dev_cap.max_jfr_sge);
    if (!(attr.dev_cap.trans_mode & URMA_TM_RM) ||
        attr.dev_cap.max_jfc_depth < QUEUE_DEPTH ||
        attr.dev_cap.max_jfr_depth < QUEUE_DEPTH || attr.dev_cap.max_jfr_sge < 1) {
        printf("step=capabilities FAIL requested_RM_depth_16_sge_1_unavailable\n");
        return false;
    }
    if (!random_token(&token)) {
        return false;
    }

    errno = 0;
    context = urma_create_context(device, index);
    if (!check_pointer("create_context", context)) {
        goto cleanup;
    }
    errno = 0;
    jfce = urma_create_jfce(context);
    if (!check_pointer("create_jfce", jfce)) {
        goto cleanup;
    }
    jfc_cfg.depth = QUEUE_DEPTH;
    jfc_cfg.jfce = jfce;
    errno = 0;
    jfc = urma_create_jfc(context, &jfc_cfg);
    if (!check_pointer("create_jfc", jfc)) {
        goto cleanup;
    }
    jfr_cfg.depth = QUEUE_DEPTH;
    jfr_cfg.flag.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
    jfr_cfg.trans_mode = URMA_TM_RM;
    jfr_cfg.max_sge = 1;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = jfc;
    jfr_cfg.token_value = token;
    errno = 0;
    jfr = urma_create_jfr(context, &jfr_cfg);
    if (!check_pointer("create_jfr", jfr)) {
        goto cleanup;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < RECEIVE_BYTES) {
        printf("step=page_size FAIL value=%ld\n", page_size);
        goto cleanup;
    }
    int allocation_rc = posix_memalign(&buffer, (size_t)page_size, (size_t)page_size);
    if (allocation_rc != 0) {
        printf("step=allocate_buffer FAIL rc=%d\n", allocation_rc);
        goto cleanup;
    }
    memset(buffer, 0, (size_t)page_size);
    seg_cfg.va = (uint64_t)(uintptr_t)buffer;
    seg_cfg.len = (uint64_t)page_size;
    seg_cfg.token_value = token;
    seg_cfg.flag.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
    seg_cfg.flag.bs.access = URMA_ACCESS_LOCAL_ONLY;
    /* Zero flags select pinned pages and a provider-allocated token ID.
     * No remote READ/WRITE access to this segment is requested.
     */
    errno = 0;
    segment = urma_register_seg(context, &seg_cfg);
    if (!check_pointer("register_seg", segment)) {
        goto cleanup;
    }
    sge.addr = seg_cfg.va;
    sge.len = RECEIVE_BYTES;
    sge.tseg = segment;
    /* Despite the API member name 'src', this is a receive destination. */
    wr.src.sge = &sge;
    wr.src.num_sge = 1;
    wr.user_ctx = 1; /* Receive-slot identity, not an address. */
    errno = 0;
    if (!check_status("post_jfr_wr", urma_post_jfr_wr(jfr, &wr, &bad_wr))) {
        goto cleanup;
    }
    printf("receive_posted=1 buffer_bytes=%d queue_depth=%d\n", RECEIVE_BYTES, QUEUE_DEPTH);

    /* No rearm/wait: this stage has no sender and tests only an empty poll. */
    errno = 0;
    int completions = urma_poll_jfc(jfc, 1, &cr);
    int poll_errno = errno;
    if (completions != 0) {
        printf("step=poll_jfc FAIL expected=0 actual=%d errno=%d\n", completions, poll_errno);
        if (completions > 0) {
            printf("unexpected_cr status=%d user_ctx=%" PRIu64 " bytes=%" PRIu32 "\n",
                   (int)cr.status, cr.user_ctx, cr.completion_len);
        }
        goto cleanup;
    }
    printf("step=poll_jfc OK completions=0 (expected_without_sender)\n");
    ok = true;

cleanup:
    /* Destroy the queue before unregistering its posted buffer. A failure stops
     * dependent releases; ownership is retained until this process exits.
     * This probe has no peer and never exports the receive endpoint/token.
     */
    if (jfr) {
        errno = 0;
        if (!check_status("delete_jfr", urma_delete_jfr(jfr))) {
            goto cleanup_failed;
        }
    }
    if (segment) {
        errno = 0;
        if (!check_status("unregister_seg", urma_unregister_seg(segment))) {
            goto cleanup_failed;
        }
    }
    free(buffer);
    if (jfc) {
        errno = 0;
        if (!check_status("delete_jfc", urma_delete_jfc(jfc))) {
            goto cleanup_failed;
        }
    }
    if (jfce) {
        errno = 0;
        if (!check_status("delete_jfce", urma_delete_jfce(jfce))) {
            goto cleanup_failed;
        }
    }
    if (context) {
        errno = 0;
        if (!check_status("delete_context", urma_delete_context(context))) {
            goto cleanup_failed;
        }
    }
    return ok;

cleanup_failed:
    *cleanup_ok = false;
    printf("cleanup=FAIL dependent_resources_retained_until_process_exit\n");
    return false;
}

int main(int argc, char **argv)
{
    const char *device_name = NULL;
    uint32_t eid_index = 0;
    bool have_index = false, list = argc == 1;
    const struct option options[] = {
        {"list", no_argument, NULL, 'l'},
        {"device", required_argument, NULL, 'd'},
        {"eid-index", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "ld:e:h", options, NULL)) != -1) {
        switch (option) {
        case 'l': list = true; break;
        case 'd': device_name = optarg; break;
        case 'e': {
            char *end = NULL;
            errno = 0;
            unsigned long value = strtoul(optarg, &end, 10);
            if (errno || !*optarg || strspn(optarg, "0123456789") != strlen(optarg) ||
                *end || value > UINT32_MAX) {
                fprintf(stderr, "Invalid --eid-index: %s\n", optarg);
                return 2;
            }
            eid_index = (uint32_t)value;
            have_index = true;
            break;
        }
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind != argc || (list && (device_name || have_index)) ||
        (!list && (!device_name || !have_index))) {
        usage(argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("probe=host_receive_resources version=1 mode=%s\n", list ? "list" : "resources");
    print_library();
    errno = 0;
    if (!check_status("urma_init", urma_init(NULL))) {
        printf("RESULT %s=FAIL device_send=NOT_TESTED\n", list ? "enumeration" : "host_receive_resources");
        return 1;
    }
    bool ok = false, cleanup_ok = true;
    int count = 0;
    errno = 0;
    urma_device_t **devices = urma_get_device_list(&count);
    if (check_pointer("get_device_list", devices)) {
        printf("device_count=%d\n", count);
        if (list) {
            ok = count > 0 && print_devices(devices, count);
        } else {
            urma_device_t *selected = NULL;
            for (int i = 0; i < count; ++i) {
                if (strcmp(devices[i]->name, device_name) == 0) {
                    selected = devices[i];
                    break;
                }
            }
            if (!selected) {
                printf("step=select_device FAIL name_not_listed=%s\n", device_name);
            } else if (validate_eid(selected, eid_index)) {
                ok = receive_resources(selected, eid_index, &cleanup_ok);
            }
        }
    }
    /* Do not dismantle the library/device list beneath live resources. */
    if (cleanup_ok) {
        if (devices) {
            urma_free_device_list(devices);
        }
        errno = 0;
        if (!check_status("urma_uninit", urma_uninit())) {
            ok = false;
        }
    }
    printf("RESULT %s=%s device_send=NOT_TESTED graph_replay=NOT_TESTED\n",
           list ? "enumeration" : "host_receive_resources", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

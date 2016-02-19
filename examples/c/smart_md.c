
#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <librepo/librepo.h>
#include <fcntl.h>

#include "../../librepo/metalink.h"
#include "../../librepo/handle.h"
#include "../../librepo/rcodes.h"
#include "../../librepo/types.h"
#include "../../librepo/handle_internal.h"
#include "../../librepo/result_internal.h"
#include "../../librepo/yum.h"

#include "librepo/util.h"
#include <glib-2.0/glib/gerror.h>

gint hash_comp(gpointer p) {
    LrMetalinkHash *hash = (LrMetalinkHash*) p;
    return strcmp(hash->type, "sha256");
}

gint rec_comp(gpointer p, const char* type) {
    LrYumRepoMdRecord *file = (LrYumRepoMdRecord*) p;
    return strcmp(file->type, type);
}

gint path_comp(gpointer p, const char* type) {
    LrYumRepoPath *path = (LrYumRepoPath*) p;
    return strcmp(path->type, type);
}


int
main(int argc, char *argv[])
{
    const char path[] = "./updates/metalink.xml";
    GError *err = NULL;
    int fd;
    LrHandle *h_local = NULL;
    LrResult *r_local = NULL;
    LrHandle *h_remote = NULL;
    LrResult *r_remote = NULL;
    const char *urls[1];
    LrUrlVars *urlvars = NULL;
    gboolean ret;

    urls[0] = "./updates";
    urlvars = lr_urlvars_set(urlvars, "releasever", "22");
    urlvars = lr_urlvars_set(urlvars, "basearch", "x86_64");

    h_local = lr_handle_init();
    r_local = lr_result_init();

    char *full_yumdlist[] = {"primary", "filelists", "group_gz", NULL};
    lr_handle_setopt(h_local, NULL, LRO_DESTDIR, "./updates");
    lr_handle_setopt(h_local, NULL, LRO_VARSUB, urlvars);
    lr_handle_setopt(h_local, NULL, LRO_URLS, urls);
    lr_handle_setopt(h_local, NULL, LRO_LOCAL, 1);
    lr_handle_setopt(h_local, NULL, LRO_YUMDLIST, full_yumdlist);
    lr_handle_setopt(h_local, NULL, LRO_REPOTYPE, LR_YUMREPO);
    lr_handle_setopt(h_local, NULL, LRO_IGNOREMISSING, 1);

    lr_handle_perform(h_local, r_local, &err);
    if (err) {
        printf("%d: %s\n", err->code, err->message); g_error_free(err);
    }

    LrMetalink *ml = h_local->metalink;

    h_remote = lr_handle_init();
    r_remote = lr_result_init();
    lr_handle_setopt(h_remote, NULL, LRO_REPOTYPE, LR_YUMREPO);
    lr_handle_setopt(h_remote, NULL, LRO_FETCHMIRRORS, 1);
    lr_handle_setopt(h_remote, NULL, LRO_METALINKURL,
                     "https://mirrors.fedoraproject.org/metalink?repo=updates-released-f24&arch=x86_64");

    lr_handle_perform(h_remote, r_remote, &err);
    if (err) {
        printf("%d: %s\n", err->code, err->message);
        g_error_free(err);
    }

    GSList *l_hashes = NULL;
    LrMetalinkHash *l_hash;
    l_hashes = g_slist_find_custom(ml->hashes, NULL, (GCompareFunc)hash_comp);
    l_hash = l_hashes[0].data;

    GSList *r_hashes = NULL;
    LrMetalinkHash *r_hash;
    r_hashes = g_slist_find_custom(h_remote->metalink->hashes, NULL, (GCompareFunc)hash_comp);
    r_hash = r_hashes[0].data;

    if (!strcmp(l_hash->value, r_hash->value)) {
        printf("Remote matches local.\n");
    } else {
        printf("Remote does not match local.\n");

        fd = open("./updates/repodata/repomd.xml", O_RDONLY);
        LrYumRepoMd *l_repomd = lr_yum_repomd_init();
        lr_yum_repomd_parse_file(l_repomd, fd, NULL, NULL, NULL);

        char *dlist[] = LR_YUM_REPOMDONLY;
        lr_handle_setopt(h_remote, NULL, LRO_REPOTYPE, LR_YUMREPO);
        lr_handle_setopt(h_remote, NULL, LRO_YUMDLIST, dlist);
        lr_handle_setopt(h_remote, NULL, LRO_FETCHMIRRORS, 0);
        lr_handle_setopt(h_remote, NULL, LRO_METALINKURL,
                        "https://mirrors.fedoraproject.org/metalink?repo=updates-released-f24&arch=x86_64");

        lr_handle_perform(h_remote, r_remote, &err);
        if (err) {
            printf("%d: %s\n", err->code, err->message);
            g_error_free(err);
        }

        LrYumRepoMd *r_repomd;
        lr_result_getinfo(r_remote, err, LRR_YUM_REPOMD, &r_repomd);
        if (err) {
            printf("%d: %s\n", err->code, err->message);
            g_error_free(err);
        } else {
            char* final_dlist[4];
            final_dlist[0] = NULL;
            for (int i = 0; full_yumdlist[i]; i++) {
                LrYumRepoPath* l_path = g_slist_find_custom(r_local->yum_repo->paths, full_yumdlist[i], (GCompareFunc) path_comp)[0].data;
                LrYumRepoMdRecord* r_record = g_slist_find_custom(r_repomd->records, full_yumdlist[i], (GCompareFunc) rec_comp)[0].data;

                LrChecksumType sum_type = lr_checksum_type(r_record->checksum_type);
                int fd = open(l_path->path, O_RDONLY);

                gboolean matches;
                lr_checksum_fd_cmp(sum_type, fd, r_record->checksum, NULL, &matches, NULL);
                if (!matches) {
                    int j = 0;
                    for (j = 0; final_dlist[j]; j++);
                    final_dlist[j] = full_yumdlist[i];
                    final_dlist[j+1] = NULL;
                }
                close(fd);
            }
            lr_handle_setopt(h_remote, NULL, LRO_DESTDIR, "./final-repo");
            lr_handle_setopt(h_remote, NULL, LRO_YUMDLIST, final_dlist);
            lr_result_free(r_remote);
            r_remote = lr_result_init();
            lr_handle_perform(h_remote, r_remote, &err);
            if (err) {
                printf("%s\n", err->message);
                g_error_free(err);
            }
        }
    }

    // TODO free everything
    g_slist_free(l_hashes);
    g_slist_free(r_hashes);
    lr_urlvars_free(urlvars);
    return 0;
}

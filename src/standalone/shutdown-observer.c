/* SPDX-License-Identifier: MIT
 * Observe only complete exitrd messages. Arbitrary boot logs cause no action.
 * PMU cleanup and restart are each attempted at most once, after SSD unmount.
 */
#include "shutdown-observer.h"
#include <string.h>
static const char clean_prefix[]="M4_SSD_ROOT_CLEAN token=";
static const char uuid_suffix[]=" uuid=@ROOT_UUID@";
static const char reboot_prefix[]="M4_SSD_REBOOT_REQUEST token=";
static int valid_token(const char *s) {
    for(unsigned i=0;i<32;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int line(struct m4_shutdown_state *s,m4_shutdown_action clean,m4_shutdown_action reboot,void *opaque) {
    if(s->used==sizeof(clean_prefix)-1+32+sizeof(uuid_suffix)-1 &&
       !memcmp(s->line,clean_prefix,sizeof(clean_prefix)-1)) {
        const char *token=s->line+sizeof(clean_prefix)-1;
        if(!valid_token(token)||memcmp(token+32,uuid_suffix,sizeof(uuid_suffix)-1))return -1;
        if(s->attempted)return -1;
        s->attempted=1;memcpy(s->token,token,32);
        if(clean(opaque))return -1;
        s->clean=1;
    } else if(s->used==sizeof(reboot_prefix)-1+32 &&
              !memcmp(s->line,reboot_prefix,sizeof(reboot_prefix)-1)) {
        const char *token=s->line+sizeof(reboot_prefix)-1;
        if(!s->clean||s->reset||memcmp(token,s->token,32))return -1;
        s->reset=1;
        if(reboot(opaque))return -1;
        return 1;
    }
    return 0;
}
int m4_shutdown_feed(struct m4_shutdown_state *s,const char *data,size_t n,
                     m4_shutdown_action clean,m4_shutdown_action reboot,void *opaque) {
    if(!s||(!data&&n)||!clean||!reboot)return -1;
    for(size_t i=0;i<n;i++) {
        char c=data[i];
        if(c=='\n'||c=='\r') {
            int rc=s->overflow?0:line(s,clean,reboot,opaque);
            s->used=s->overflow=0;if(rc)return rc;
        } else if(!s->overflow) {
            if(s->used==sizeof(s->line))s->overflow=1;
            else s->line[s->used++]=c;
        }
    }
    return 0;
}

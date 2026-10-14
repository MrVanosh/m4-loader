/* SPDX-License-Identifier: MIT
 * Output policy only. Callers retain the existing validated framebuffer.
 * No framebuffer writes occur until diagnostic mode or a terminal error.
 */
#ifndef M4_BOOT_OUTPUT_H
#define M4_BOOT_OUTPUT_H
#define M4_BOOT_OUTPUT_CAPACITY 1024u
struct m4_boot_output {
    unsigned char tail[M4_BOOT_OUTPUT_CAPACITY];
    unsigned next, count;
    int visible, wrapped;
};
typedef void (*m4_boot_emit)(void *, unsigned char);
static inline void m4_boot_output_char(struct m4_boot_output *o, unsigned char c,
                                      m4_boot_emit emit, void *opaque)
{
    if (o->visible) { emit(opaque,c); return; }
    o->tail[o->next]=c;
    o->next=(o->next+1)%M4_BOOT_OUTPUT_CAPACITY;
    if (o->count<M4_BOOT_OUTPUT_CAPACITY) o->count++;
    else o->wrapped=1;
}
/* R41: `lead` (may be NULL) is printed right after the title, before the replay: the primary fault of
 * the failed step is the first thing on the page. `max_lines` complete trailing lines are replayed. */
static inline void m4_boot_output_reveal(struct m4_boot_output *o,
                                        m4_boot_emit emit, void *opaque,
                                        const char *lead, unsigned max_lines)
{
    if (o->visible) return;
    o->visible=1;
    /* Replay at most max_lines complete trailing lines and 1024 bytes. At the
     * validated minimum 100 columns this leaves room for terminal details. */
    unsigned first=(o->next+M4_BOOT_OUTPUT_CAPACITY-o->count)%M4_BOOT_OUTPUT_CAPACITY;
    unsigned skip=0, lines=0;
    for (unsigned n=0;n<o->count;n++) {
        unsigned at=(o->next+M4_BOOT_OUTPUT_CAPACITY-1-n)%M4_BOOT_OUTPUT_CAPACITY;
        if (o->tail[at]=='\n' && ++lines==max_lines+1) { skip=o->count-n; break; }
    }
    if (!skip && o->wrapped) {
        while(skip<o->count && o->tail[(first+skip)%M4_BOOT_OUTPUT_CAPACITY]!='\n') skip++;
        /* A single long line still has a useful bounded suffix. */
        if (skip==o->count) skip=0; else skip++;
    }
    const char *title="V16 BOOT STOP: recent diagnostics follow\n";
    while(*title) emit(opaque,(unsigned char)*title++);
    while(lead && *lead) emit(opaque,(unsigned char)*lead++);
    if (o->wrapped || skip) {
        const char *notice="[earlier diagnostic output omitted]\n";
        while(*notice) emit(opaque,(unsigned char)*notice++);
    }
    for(unsigned n=skip;n<o->count;n++)emit(opaque,o->tail[(first+n)%M4_BOOT_OUTPUT_CAPACITY]);
    o->count=0;
}
#endif

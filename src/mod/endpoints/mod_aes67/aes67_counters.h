typedef struct {
    int bufs;
    int caps;
    int objs;
    int errs;
    int structs;
    int samples;
    int memories;
    int events;
    int messages;
    int features;
    int gobjects;
} G_alloc_counts;

// -- need to define the following in c file --
extern G_alloc_counts g_alloc_counts;	
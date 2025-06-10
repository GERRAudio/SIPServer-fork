// Wrappers to track allocations/deallocations
// then you can display the counters from the CLI : use "aes67 allocs"
// also to wrap sensitive gstreamer calls with mutexes as recommended by gst documentation
//
// just prefix functions that are allocators with AL_  and deallocators with DA_. Most AL Da use mutexes as well
// MU_ just mutexes the call with an appropriate mutex (hardcoded here)
//
// NB - the dealloc/deref  wrappers always check if the passed ptr is NULL first
// other than the mutex protection, the only side effect of using them is a counter incr/decr, so they are a mostly harmless debug tool
//
// Copyright GERR Audio 2025
//
#ifndef G_ALLOC_WRAP
#define G_ALLOC_WRAP

#include "aes67_counters.h"
#include <glib.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio-channels.h>
#include <gst/gst.h>
#include <stdarg.h>

// specialized mutexes, must be declared and initialized in c module where MU_ functions are used
extern switch_mutex_t *alloc_buf_lock;
extern switch_mutex_t *alloc_clk_lock;
extern switch_mutex_t *alloc_elem_lock;
extern switch_mutex_t *alloc_obj_lock;
extern switch_mutex_t *alloc_pad_lock;
extern switch_mutex_t *alloc_pipl_lock;
extern switch_mutex_t *alloc_samp_lock;
extern switch_mutex_t *alloc_cap_lock;
extern switch_mutex_t *alloc_mcp_lock;
// extern switch_mutex_t *alloc_bus_lock;


// --- Macro for allocation wrappers ---
#define G_ALLOC_WRAP_ALLOC(ret_type, func, counter, tp1, p1)                                                           \
	inline ret_type AL_##func(tp1 p1)                                                                                  \
	{                                                                                                                  \
		ret_type _ret = func(p1);                                                                                      \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		return _ret;                                                                                                   \
	}
// with mutex
#define G_ALLOC_WRAP_ALLOC_M(ret_type, func, counter, tp1, p1, l)                                                      \
	inline ret_type AL_##func(tp1 p1)                                                                                  \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type _ret = func(p1);                                                                                      \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		switch_mutex_unlock(l);                                                                                        \
		return _ret;                                                                                                   \
	}

#define G_ALLOC_WRAP_ALLOC2(ret_type, func, counter, tp1, p1, tp2, p2)                                                 \
	inline ret_type AL_##func(tp1 p1, tp2 p2)                                                                          \
	{                                                                                                                  \
		ret_type _ret = func(p1, p2);                                                                                  \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		return _ret;                                                                                                   \
	}
//with mutex
#define G_ALLOC_WRAP_ALLOC2_M(ret_type, func, counter, tp1, p1, tp2, p2, l)                                            \
	inline ret_type AL_##func(tp1 p1, tp2 p2)                                                                          \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type _ret = func(p1, p2);                                                                                  \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		switch_mutex_unlock(l);                                                                                        \
		return _ret;                                                                                                   \
	}

#define G_ALLOC_WRAP_ALLOC3(ret_type, func, counter, tp1, p1, tp2, p2, tp3, p3)                                        \
	inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3)                                                                  \
	{                                                                                                                  \
		ret_type _ret = func(p1, p2, p3);                                                                              \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		return _ret;                                                                                                   \
	}
//with mutex
#define G_ALLOC_WRAP_ALLOC3_M(ret_type, func, counter, tp1, p1, tp2, p2, tp3, p3, l)                                   \
	inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3)                                                                  \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type _ret = func(p1, p2, p3);                                                                              \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		switch_mutex_unlock(l);                                                                                        \
		return _ret;                                                                                                   \
	}

#define G_ALLOC_WRAP_ALLOC4(ret_type, func, counter, tp1, p1, tp2, p2, tp3, p3, tp4, p4)                               \
	inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3, tp4 p4)                                                          \
	{                                                                                                                  \
		ret_type _ret = func(p1, p2, p3, p4);                                                                          \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		return _ret;                                                                                                   \
	}

#define G_ALLOC_WRAP_ALLOC7(ret_type, func, counter, tp1, p1, tp2, p2, tp3, p3, tp4, p4, tp5, p5, tp6, p6, tp7, p7)    \
	inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3, tp4 p4, tp5 p5, tp6 p6, tp7 p7)                                  \
	{                                                                                                                  \
		ret_type _ret = func(p1, p2, p3, p4, p5, p6, p7);                                                              \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		return _ret;                                                                                                   \
	}

// --- Macro for deallocation wrappers ---
#define G_ALLOC_WRAP_FREE(fname, counter, arg_type)                                                                     \
	inline void DA_##fname(arg_type p)                                                                                  \
	{                                                                                                                  \
		if (p != NULL) {                                                                                               \
			g_alloc_counts.counter--;                                                                                  \
			fname(p);                                                                                                   \
		}                                                                                                              \
	}
//with mutex
#define G_ALLOC_WRAP_FREE_M(fname, counter, arg_type, l)                                                                \
	inline void DA_##fname(arg_type p)                                                                                  \
	{                                                                                                                  \
		if (p != NULL) {                                                                                               \
			switch_mutex_lock(l);                                                                                      \
			g_alloc_counts.counter--;                                                                                  \
			fname(p);                                                                                                   \
			switch_mutex_unlock(l);                                                                                    \
		}                                                                                                              \
	}

/*
// --special test case ----
inline void DF_gst_object_unref(GstObject *p)
{
	if (p != NULL) {
		g_alloc_counts.objs--;
		g_alloc_counts.FDA++;
		gst_object_unref(p);
	}
}

inline GstElement *AF_gst_bin_get_by_name(GstBin *bin, gchar *name)
{
	GstElement *_ret = gst_bin_get_by_name(bin, name);
	if (_ret != NULL) {
		g_alloc_counts.objs++;
		g_alloc_counts.FAL++;
	}
	return _ret;
}
*/
// -----
/*
#define G_ALLOC_WRAP_REF(func, counter, arg_type)                                                                      \
	inline arg_type AL_##func(arg_type p)                                                                              \
	{                                                                                                                  \
		arg_type _ret = func(p);                                                                                       \
		if (_ret != NULL) g_alloc_counts.counter++;                                                                    \
		return _ret;                                                                                                   \
	}
*/

//
// ==== incr/decr to use when wrapping is undesired
// --- Macro for increment-only wrappers (for manual tracking) ---
#define G_ALLOC_WRAP_INC(counter, name, t, p)                                                                          \
	inline void AL_##name(t p)                                                                                         \
	{                                                                                                                  \
		if (p) g_alloc_counts.counter++;                                                                               \
	}

// --- Macro for decrement-only wrappers (for manual tracking) ---
#define G_ALLOC_WRAP_DEC(counter, name, t, p)                                                                          \
	inline void DA_##name(t p)                                                                                         \
	{                                                                                                                  \
		if (p) g_alloc_counts.counter--;                                                                               \
	}

//==== Mutex wrappers
//
#define MU_WRAP1(ret_type, fname, tp1, p1, l)                                                                          \
	inline ret_type MU_##fname(tp1 p1)                                                                                 \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type retval = fname(p1);                                                                                   \
		switch_mutex_unlock(l);                                                                                        \
		return retval;                                                                                                 \
	}

#define MU_WRAPV1c(fname, tp1, p1, l)                                                                                  \
	inline void MUc_##fname(tp1 p1)                                                                                    \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		fname(p1);                                                                                                     \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAPV1p(fname, tp1, p1, l)                                                                                  \
	inline void MUp_##fname(tp1 p1)                                                                                    \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		fname(p1);                                                                                                     \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAP2(ret_type, fname, tp1, p1, tp2, p2, l)                                                                 \
	inline ret_type MU_##fname(tp1 p1, tp2 p2)                                                                         \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type retval = fname(p1, p2);                                                                               \
		switch_mutex_unlock(l);                                                                                        \
		return retval;                                                                                                 \
	}

#define MU_WRAPV2(fname, tp1, p1, tp2, p2, l)                                                                          \
	inline void MU_##fname(tp1 p1, tp2 p2)                                                                             \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		fname(p1, p2);                                                                                                 \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAPV2p(fname, tp1, p1, tp2, p2, l)                                                                         \
	inline void MUp_##fname(tp1 p1, tp2, p2)                                                                           \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		fname(p1, p2);                                                                                                 \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAP3(ret_type, fname, tp1, p1, tp2, p2, tp3, p3, l)                                               \
	inline ret_type MU_##fname(tp1 p1, tp2 p2, tp3 p3)                                                         \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type retval = fname(p1, p2, p3);                                                                       \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAP3S(ret_type, fname, t1, p1, t2, p2, t3, p3, l)                                                          \
	inline ret_type MU3_##fname(t1 p1, t2 p2, t3 p3)                                                                   \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type _result = fname(p1, p2, p3);                                                                          \
		switch_mutex_unlock(l);                                                                                        \
		return _result;                                                                                                \
	}
#define MU_WRAPV3(fname, tp1, p1, tp2, p2, tp3, p3, l)                                                                 \
	inline void MU_##fname(tp1 p1, tp2 p2, tp3 p3)                                                                     \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		fname(p1, p2, p3);                                                                                             \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAP4(ret_type, fname, tp1, p1, tp2, p2, tp3, p3, tp4, p4, l)                                               \
	inline ret_type MU_##fname(tp1 p1, tp2 p2, tp3 p3, tp4 p4)                                                         \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type retval = fname(p1, p2, p3, p4);                                                                       \
		switch_mutex_unlock(l);                                                                                        \
	}

#define MU_WRAP7(ret_type, fname, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, l)                           \
	inline ret_type MU_##fname(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7)                                        \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type _result = fname(p1, p2, p3, p4, p5, p6, p7);                                                          \
		switch_mutex_unlock(l);                                                                                        \
		return _result;                                                                                                \
	}
#define MU_WRAP8S(ret_type, fname, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, l)                  \
	inline ret_type MU8_##fname(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8)                                \
	{                                                                                                                  \
		switch_mutex_lock(l);                                                                                          \
		ret_type _result = fname(p1, p2, p3, p4, p5, p6, p7, p8);                                                      \
		switch_mutex_unlock(l);                                                                                        \
		return _result;                                                                                                \
	}


// ===
// gst functions that require mutexes- so wrap
//
// // === memcpy lock
//void *memcpy(void *dest, const void *src, size_t n);
MU_WRAPV3(memcpy, void *,dest, const void *,src, size_t, n, alloc_mcp_lock)




// ===clock locks
// MUc_gst_object_unref(GST_OBJECT(stream->clock));
MU_WRAPV1c(gst_object_unref, gpointer, clock, alloc_clk_lock)
//gboolean gst_clock_add_observation( GstClock *clock, GstClockTime slave,GstClockTime master, gdouble*, r_squared);
MU_WRAP4(gboolean, gst_clock_add_observation, GstClock *, c, GstClockTime, s, GstClockTime, m, gdouble *, r_squared, alloc_clk_lock)


// ===element (obj) locks
// GstStateChangeReturn  gst_element_set_state (GstElement *element,GstState state);
MU_WRAP2(GstStateChangeReturn, gst_element_set_state, GstElement*, e, GstState, s, alloc_elem_lock)
// gboolean gst_bin_add (GstBin *bin, GstElement *element);
MU_WRAP2(gboolean, gst_bin_add, GstBin *, bin, GstElement *, element, alloc_elem_lock)
// gboolean gst_element_link(GstElement *src, GstElement *dest);
MU_WRAP2(gboolean, gst_element_link, GstElement *, src, GstElement *, dest, alloc_elem_lock)
// gboolean gst_element_sync_state_with_parent(GstElement *element);
MU_WRAP1(gboolean, gst_element_sync_state_with_parent, GstElement *, element, alloc_elem_lock)
// void gst_element_unlink (GstElement *src, GstElement *dest);
MU_WRAPV2(gst_element_unlink, GstElement *, src, GstElement *, dest, alloc_elem_lock)

// == pad locks :-)
// GstPadLinkReturn gst_pad_link (GstPad *srcpad, GstPad *sinkpad);
MU_WRAP2(GstPadLinkReturn, gst_pad_link, GstPad *, srcpad, GstPad *, sinkpad, alloc_pad_lock)
// void gst_element_release_request_pad (GstElement *element, GstPad *pad);
MU_WRAPV2(gst_element_release_request_pad, GstElement *, element, GstPad *, pad, alloc_pad_lock)
// gboolean gst_pad_unlink(GstPad *srcpad, GstPad *sinkpad);
MU_WRAP2(gboolean, gst_pad_unlink, GstPad *, srcpad, GstPad *, sinkpad, alloc_pad_lock)
// gboolean gst_element_link_pads( GstElement *src,const gchar *srcpadname, GstElement *dest, const gchar, *destpadname);
MU_WRAP4(gboolean, gst_element_link_pads, GstElement *,src, const gchar *, srcpn, GstElement *,d,const gchar *,dpn, alloc_pad_lock)

// == sample locks
// GstBuffer * gst_sample_get_buffer(GstSample *sample);
MU_WRAP1(GstBuffer *, gst_sample_get_buffer, GstSample *, sample, alloc_samp_lock)
//void gst_buffer_unmap(GstBuffer *buffer, GstMapInfo *info);
MU_WRAPV2(gst_buffer_unmap, GstBuffer *,buffer, GstMapInfo *,info, alloc_buf_lock)
// gboolean gst_buffer_map(   GstBuffer *buffer,    GstMapInfo *info,    GstMapFlags flags);
MU_WRAP3(gboolean, gst_buffer_map, GstBuffer *, buffer, GstMapInfo *, info, GstMapFlags, flags, alloc_buf_lock)

//guint gst_bus_add_watch( GstBus *bus,  GstBusFunc func,  gpointer user_data);
//MU_WRAP3(guint, gst_bus_add_watch, GstBus *,bus, GstBusFunc, func, gpointer ,user_data, alloc_bus_lock)

//gboolean gst_element_link_many(GstElement *element_1, GstElement *element_2, ..., NULL);

//MU_WRAP7(gboolean,gst_element_link_many,GstElement*,e1,GstElement*,e2,GstElement*,e3,GstElement*,e4,GstElement*,e5,GstElement*,e6, GstElement*,e7,alloc_pipl_lock) 
//gboolean gst_element_link_many(GstElement *element_1, GstElement *element_2, ..., NULL);
//void g_object_set (gpointer object, const gchar *first_property_name, ...);
#define MU_g_object_set(p1, ...)                                                                                       \
	do {                                                                                                               \
		switch_mutex_lock(alloc_elem_lock);                                                                            \
		g_object_set(p1, __VA_ARGS__);                                                                                 \
		switch_mutex_unlock(alloc_elem_lock);                                                                          \
	} while (0)

// void gst_bin_add_many (GstBin *bin, GstElement *element_1, ...);
#define MU_gst_bin_add_many(p1, ...)                                                                                   \
	do {                                                                                                               \
		switch_mutex_lock(alloc_elem_lock);                                                                            \
		gst_bin_add_many(p1, __VA_ARGS__);                                                                             \
		switch_mutex_unlock(alloc_elem_lock);                                                                          \
	} while (0)

// GstCaps * gst_caps_new_simple (const char *media_type, const char *fieldname, ...);
static GstCaps *gst_caps_new_simple_locked(const char *media_type, ...)
{
	GstCaps *caps;
	va_list args;

	switch_mutex_lock(alloc_cap_lock);
	va_start(args, media_type);
	caps = gst_caps_new_simple(media_type, args);
	va_end(args);
	switch_mutex_unlock(alloc_cap_lock);
	return caps;
}
// does not work - crashes - so reverted to original (the following is effectively no substitution)
#define MU_gst_caps_new_simple(...) gst_caps_new_simple(__VA_ARGS__)



// chars
G_ALLOC_WRAP_INC(chars, cnt_chars, gchar *, p)
G_ALLOC_WRAP_FREE(g_free, chars, gpointer)
G_ALLOC_WRAP_ALLOC(gpointer, g_malloc0, chars, gsize, c)
G_ALLOC_WRAP_ALLOC(gchar *, g_strdup, chars, gchar *, str)
G_ALLOC_WRAP_ALLOC(gchar *, gst_structure_to_string, chars, const GstStructure *, s)

// --- Buffer wrappers ---
G_ALLOC_WRAP_INC(bufs, cnt_bufs, GstStructure *, p)
G_ALLOC_WRAP_DEC(bufs, dec_bufs, GstBuffer *, p)
G_ALLOC_WRAP_FREE_M(gst_buffer_unref, bufs, GstBuffer *, alloc_buf_lock)
// G_ALLOC_WRAP_REF(gst_buffer_ref, bufs, GstBuffer *)
G_ALLOC_WRAP_ALLOC3_M(GstBuffer *, gst_buffer_new_allocate, bufs, GstAllocator *, allocator, gsize, size,
					  GstAllocationParams *, params, alloc_buf_lock)
// G_ALLOC_WRAP_ALLOC(GstBuffer*, gst_buffer_new, bufs, void)

// --- Structure wrappers ---
G_ALLOC_WRAP_INC(structs, cnt_structs, GstStructure *, p)
G_ALLOC_WRAP_FREE(gst_structure_free, structs, GstStructure *)
G_ALLOC_WRAP_ALLOC(GstStructure *, gst_structure_copy, structs, const GstStructure *, s)
G_ALLOC_WRAP_ALLOC(GstStructure *, gst_structure_new_empty, structs, const gchar *, name)
//
// no macro for variadic function below
//
inline GstStructure *AL_gst_structure_new(const gchar *name, const gchar *first, ...)
{
	va_list args;
	va_start(args, first);
	GstStructure *s = gst_structure_new_valist(name, first, args);
	va_end(args);
	if (s) g_alloc_counts.structs++;
	return s;
}

// --- Error wrappers ---
G_ALLOC_WRAP_INC(errs, cnt_errs, GError *, p)
G_ALLOC_WRAP_FREE(g_error_free, errs, GError *)


////////
// --- Object wrappers ---
G_ALLOC_WRAP_FREE_M(gst_object_unref, objs, GstObject *, alloc_elem_lock)

G_ALLOC_WRAP_DEC(objs, dec_objs, GstObject *, p)
G_ALLOC_WRAP_INC(objs, cnt_objs, GstObject *, p) G_ALLOC_WRAP_ALLOC_M(GstBus *, gst_pipeline_get_bus, objs,
																	  GstPipeline *, b, alloc_pipl_lock)
G_ALLOC_WRAP_ALLOC2_M(GstElement *, gst_bin_get_by_name, objs, GstBin *, bin, const gchar *, n, alloc_pipl_lock)
G_ALLOC_WRAP_ALLOC_M(GstElement *, gst_pipeline_new, objs, const gchar *, n, alloc_pipl_lock)
// ===pipeline locks
// gboolean gst_pipeline_set_clock(GstPipeline *pipeline, GstClock *clock);
MU_WRAP2(gboolean, gst_pipeline_set_clock, GstPipeline *, p, GstClock *, c, alloc_pipl_lock)
// void gst_pipeline_use_clock(GstPipeline *pipeline, GstClock *clock);
MU_WRAPV2(gst_pipeline_use_clock, GstPipeline *, pipeline, GstClock *, clock, alloc_pipl_lock)
// MUp_gst_object_unref(GST_OBJECT(stream->pipeline));
MU_WRAPV1p(gst_object_unref, gpointer, pipeline, alloc_pipl_lock)
// gboolean gst_bin_remove(GstBin *bin, GstElement *element);
MU_WRAP2(gboolean, gst_bin_remove, GstBin *, bin, GstElement *, element, alloc_pipl_lock)
// GstStateChangeReturn gst_element_get_state(GstElement *e, GstState *s,GstState *pending, GstClockTime timeout);
MU_WRAP4(GstStateChangeReturn, gst_element_get_state, GstElement *, e, GstState *, s, GstState *, p, GstClockTime, t, alloc_pipl_lock)
// gboolean gst_bus_remove_watch(GstBus *bus);
MU_WRAP1(gboolean, gst_bus_remove_watch, GstBus *, bus, alloc_pipl_lock)
// void gst_element_set_base_time(GstElement *element, GstClockTime time);
// MU_WRAPV2( gst_element_set_base_time,GstElement *,element, GstClockTime, time, alloc_pipl_lock)
// void gst_element_set_start_time(GstElement *element, GstClockTime time);
// MU_WRAPV2(gst_element_set_start_time, GstElement *, element, GstClockTime, time,alloc_pipl_lock)
// MU_WRAP8S(gboolean, gst_element_link_many, GstElement *, e1, GstElement *, e2, GstElement *, e3, GstElement *,
// e4,
//		  GstElement *, e5, GstElement *, e6, GstElement *, e7, GstElement *, e8, alloc_pipl_lock)
// gboolean gst_element_link_many(GstElement *element_1, GstElement *element_2, ..., NULL);
// MU_WRAP3S(gboolean,gst_element_link_many,GstElement*,e1,GstElement*,e2,GstElement*,e3,alloc_pipl_lock)






G_ALLOC_WRAP_ALLOC_M(GstPad *, gst_pad_get_peer, objs, GstPad *, pad, alloc_pad_lock)
G_ALLOC_WRAP_ALLOC2_M(GstElement *, gst_element_request_pad_simple, objs, GstBin *,bin, const gchar *, n, alloc_pad_lock) 
G_ALLOC_WRAP_ALLOC2_M(GstPad *, gst_element_get_static_pad,  objs, GstElement *, e, const gchar *, n,  alloc_pad_lock)

G_ALLOC_WRAP_ALLOC2_M(GstElement *, gst_element_factory_make, objs, const gchar *, factoryname, const gchar *, n, alloc_elem_lock)
G_ALLOC_WRAP_ALLOC_M(GstObject *, gst_element_get_parent, objs, GstElement *, e, alloc_elem_lock)
								
G_ALLOC_WRAP_ALLOC_M(GstClock *, gst_element_get_clock, objs, GstElement *, element, alloc_clk_lock)
// special for clock objects
#define DC_gst_object_unref(p1)                                                                                        \
	do {                                                                                                               \
		switch_mutex_lock(alloc_clk_lock);                                                                             \
		gst_object_unref(p1);                                                                                          \
		switch_mutex_unlock(alloc_clk_lock);                                                                           \
	} while (0)



// G_ALLOC_WRAP_ALLOC2(GstPad *, gst_element_get_pad, objs, GstElement *, element, const gchar *, name)
// G_ALLOC_WRAP_FREE(g_object_unref, gobjects, gpointer)
// G_ALLOC_WRAP_REF(g_object_ref, gobjects, gpointer)
// G_ALLOC_WRAP_REF(gst_object_ref, objs, GstObject *)
// G_ALLOC_WRAP_REF(gst_object_ref_sink, objs, GstObject *)

// --- Sample  wrappers ---
G_ALLOC_WRAP_INC(samples, cnt_samples, GstSample *, p)
G_ALLOC_WRAP_FREE_M(gst_sample_unref, samples, GstSample *, alloc_samp_lock)
G_ALLOC_WRAP_ALLOC2_M(GstSample *, gst_app_sink_try_pull_sample, samples, GstAppSink *, appsink, guint64, timeout, alloc_samp_lock)
// G_ALLOC_WRAP_ALLOC(GstSample*, gst_app_sink_pull_sample, samples, GstAppSink *,appsink)
// G_ALLOC_WRAP_ALLOC(GstSample*, gst_app_sink_pull_preroll, samples, GstAppSink *,appsink)
// G_ALLOC_WRAP_ALLOC4(GstSample*, gst_sample_new, samples, GstBuffer *,buffer, GstCaps *,caps, GstSegment
// *,segment, GstStructure *,info) G_ALLOC_WRAP_REF(gst_sample_ref, samples, GstSample *)

// --- caps wrappers ---
G_ALLOC_WRAP_INC(caps, cnt_caps, GstCaps *, p) G_ALLOC_WRAP_FREE_M(gst_caps_unref, caps, GstCaps *, alloc_cap_lock)
G_ALLOC_WRAP_ALLOC_M(GstCaps *, gst_caps_from_string, caps, const gchar *, string, alloc_cap_lock)
MU_WRAP1(GstCaps*, gst_caps_copy, const GstCaps*,c , alloc_cap_lock) 
//void gst_caps_set_simple(GstCaps *caps, const char *field, ...);
#define MU_gst_caps_set_simple(caps, field, ...)                                                                       \
	do {                                                                                                               \
		switch_mutex_lock(alloc_cap_lock);                                                                             \
		gst_caps_set_simple(caps, field, __VA_ARGS__);                                                                 \
		switch_mutex_unlock(alloc_cap_lock);                                                                           \
	} while (0)

// G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_new_empty, caps, void)
// G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_new_any, caps, void)
// G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_copy_nth, caps, const GstCaps *,caps, guint, nth)
// G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_caps_copy_and_set_caps_features, caps, const GstCaps *,caps, guint, index,
// const GstCapsFeatures *,features) G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_merge, caps, GstCaps *,caps1, GstCaps
// *,caps2) G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_merge_structure, caps, GstCaps *,caps, GstStructure *,structure)
// G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_normalize, caps, GstCaps *,caps)
// G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_caps_merge_structure_full, caps, GstCaps *,caps, GstStructure *,structure,
// GstCapsFeatures *,features) G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_type_find_helper_for_buffer, caps, GstObject *,obj,
// GstBuffer *,buf, GstCaps *,filter) G_ALLOC_WRAP_ALLOC4(GstCaps*, gst_type_find_helper_for_buffer_with_caps, caps,
// GstObject *,obj, GstBuffer *,buf, GstCaps *,caps, GstCaps *,filter) G_ALLOC_WRAP_ALLOC2(GstCaps*,
// gst_caps_intersect, caps, const GstCaps *,caps1, const GstCaps *,caps2) G_ALLOC_WRAP_ALLOC3(GstCaps*,
// gst_caps_intersect_full, caps, const GstCaps *,caps1, const GstCaps *,caps2, GstCapsIntersectMode, mode)
// G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_union, caps, const GstCaps *,caps1, const GstCaps *,caps2)
// G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_subtract, caps, const GstCaps *,minuend, const GstCaps *,subtrahend)
// G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_fixate, caps, const GstCaps *,caps)

// -- variadic --
/*
 inline GstCaps* AL_gst_caps_new_simple(const char *media_type, const char *fieldname, ...) {
	va_list args;
	va_start(args, fieldname);
	GstCaps *caps = gst_caps_new_simple_valist(media_type, fieldname, args);
	va_end(args);
	if (caps != NULL) g_alloc_counts.caps++;
	return caps;
}
*/

/* inline GstCaps *AL_gst_type_find_helper(GstObject *obj, GstCaps *filter, ...) {
	va_list args;
	va_start(args, filter);
	GstCaps *caps = gst_type_find_helper_valist(obj, filter, args);
	va_end(args);
	if (caps != NULL) g_alloc_counts.caps++;
	return caps;
}
*/

// --- features ---
// G_ALLOC_WRAP_FREE(gst_caps_features_unref, features, GstCapsFeatures *)
// G_ALLOC_WRAP_ALLOC(GstCapsFeatures*, gst_caps_features_new, features, const gchar *feature1, ...)         //weird
// one G_ALLOC_WRAP_REF(gst_caps_features_ref, features, GstCapsFeatures *)

// -- variadic --
/*
inline GstCapsFeatures *AL_gst_caps_features_new(const gchar *feature1, ...)
{
	va_list args;
	va_start(args, feature1);
	GstCapsFeatures *f = gst_caps_features_new_valist(feature1, args);
	va_end(args);
	if (f) g_alloc_counts.features++;
	return f;
}
*/

// --- memory ---
// G_ALLOC_WRAP_FREE(gst_memory_unref, memories, GstMemory *)
// G_ALLOC_WRAP_ALLOC3(GstMemory*, gst_allocator_alloc, memories, GstAllocator *,allocator, gsize, size,
// GstAllocationParams *,params) G_ALLOC_WRAP_ALLOC7(GstMemory*, gst_memory_new_wrapped, memories, GstMemoryFlags,
// flags, gpointer, data, gsize ,maxsize, gsize ,offset, gsize, size, GDestroyNotify ,notify, gpointer, user_data)
// G_ALLOC_WRAP_REF(gst_memory_ref, memories, GstMemory *)

// -- events --
// G_ALLOC_WRAP_FREE(gst_event_unref, events, GstEvent *)
// G_ALLOC_WRAP_REF(gst_event_ref, events, GstEvent *)
// G_ALLOC_WRAP_ALLOC(GstEvent*, gst_event_new_eos, events, void)

// -- messages --
// G_ALLOC_WRAP_FREE(gst_message_unref, messages, GstMessage *)
// G_ALLOC_WRAP_REF(gst_message_ref, messages, GstMessage *)

#endif

// wrappers to track allocations/deallocations
// just prefix functions that are allocators with AL_  and deallocators with DA_ 
// 
// then you can display the counters
// GERR Audio 2025
//
#ifndef G_ALLOC_WRAP
#define G_ALLOC_WRAP

#include <gst/gst.h>
#include <glib.h>
#include <stdarg.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio-channels.h>
#include "aes67_counters.h"
	

// --- Macro for allocation wrappers ---
#define G_ALLOC_WRAP_ALLOC(ret_type, func, counter, tp1, p1)\
    static inline ret_type AL_##func(tp1 p1) {                    \
        ret_type _ret = func(p1);                                 \
        if (_ret != NULL) g_alloc_counts.counter++;               \
        return _ret;                                              \
    }

#define G_ALLOC_WRAP_ALLOC2(ret_type, func, counter, tp1, p1, tp2, p2)  \
	static inline ret_type AL_##func(tp1 p1, tp2 p2)                    \
	{                                                                   \
		ret_type _ret = func(p1,p2);                                    \
		if (_ret != NULL) g_alloc_counts.counter++;                     \
		return _ret;                                                    \
	}

#define G_ALLOC_WRAP_ALLOC3(ret_type, func, counter, tp1,p1, tp2,p2, tp3,p3) \
	static inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3)                 \
	{                                                                        \
		ret_type _ret = func(p1, p2, p3);                                    \
		if (_ret != NULL) g_alloc_counts.counter++;                          \
		return _ret;                                                         \
	}

#define G_ALLOC_WRAP_ALLOC4(ret_type, func, counter, tp1, p1, tp2 ,p2, tp3, p3, tp4, p4)      \
	static inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3, tp4 p4)                  \
	{                                                                                 \
		ret_type _ret = func(p1, p2, p3, p4);                                         \
		if (_ret != NULL) g_alloc_counts.counter++;                                   \
		return _ret;                                                                  \
	}

#define G_ALLOC_WRAP_ALLOC7(ret_type, func, counter,tp1,p1,tp2,p2, tp3,p3,tp4,p4,tp5,p5,tp6,p6,tp7,p7)          \
	static inline ret_type AL_##func(tp1 p1, tp2 p2, tp3 p3, tp4 p4,tp5 p5,tp6 p6,tp7 p7)                       \
	{                                                                                                           \
		ret_type _ret = func(p1, p2, p3, p4, p5, p6, p7);                                                       \
		if (_ret != NULL) g_alloc_counts.counter++;                                                             \
		return _ret;                                                                                            \
	}


// --- Macro for deallocation wrappers ---
#define G_ALLOC_WRAP_FREE(func, counter, arg_type)               \
    static inline void DA_##func(arg_type p) {                   \
        if (p != NULL) {                                         \
            g_alloc_counts.counter--;                            \
            func(p);                                             \
        }                                                        \
    }

#define G_ALLOC_WRAP_REF(func, counter, arg_type)                \
    static inline arg_type AL_##func(arg_type p) {               \
        arg_type _ret = func(p);                                 \
        if (_ret != NULL) g_alloc_counts.counter++;              \
        return _ret;                                             \
    }
	
// --- Macro for increment-only wrappers (for manual tracking) ---
#define G_ALLOC_WRAP_INC(counter, name)                          \
    static inline void AL_##name(void) {                         \
        g_alloc_counts.counter++;                                \
    }

// --- Buffer wrappers ---
G_ALLOC_WRAP_FREE(g_free, bufs, gpointer)
G_ALLOC_WRAP_ALLOC(gpointer, g_malloc0, bufs, gsize,c)
G_ALLOC_WRAP_ALLOC(gchar*, g_strdup, bufs, const gchar *,str)
G_ALLOC_WRAP_ALLOC(gchar*, gst_structure_to_string, bufs, const GstStructure *,s)
G_ALLOC_WRAP_ALLOC(GstBuffer*, gst_buffer_new, bufs, void)
G_ALLOC_WRAP_REF(gst_buffer_ref, bufs, GstBuffer *)
G_ALLOC_WRAP_FREE(gst_buffer_unref, bufs, GstBuffer *)

// --- Structure wrappers ---
G_ALLOC_WRAP_FREE(gst_structure_free, structs, GstStructure *)
G_ALLOC_WRAP_ALLOC(GstStructure*, gst_structure_copy, structs, const GstStructure *,s)
G_ALLOC_WRAP_ALLOC(GstStructure*, gst_structure_new_empty, structs, const gchar *,name)

// no macro possible for variadic function below
//
static inline GstStructure* AL_gst_structure_new(const gchar *name, const gchar *first, ...) {
    va_list args;
    va_start(args, first);
    GstStructure *s = gst_structure_new_valist(name, first, args);
    va_end(args);
    if (s) g_alloc_counts.structs++;
    return s;
}

// --- Error wrappers ---
G_ALLOC_WRAP_FREE(g_error_free, errs, GError *)
// -- usage:  AL_errs( GErrror* err )
G_ALLOC_WRAP_INC(errs, cnt_err)

// --- Object wrappers ---
G_ALLOC_WRAP_FREE(gst_object_unref, objs, GstObject *)
G_ALLOC_WRAP_ALLOC(GstBus*, gst_pipeline_get_bus, objs, GstPipeline *,b)
G_ALLOC_WRAP_ALLOC2(GstPad*, gst_element_get_static_pad, objs, GstElement *,element, const gchar *,name)
G_ALLOC_WRAP_ALLOC(GstPad*, gst_pad_get_peer, objs, GstPad *,pad)
G_ALLOC_WRAP_ALLOC2(GstElement*, gst_bin_get_by_name, objs, GstBin *,bin, const gchar *,name)
G_ALLOC_WRAP_ALLOC2(GstElement*, gst_element_factory_make, objs, const gchar *,factoryname, const gchar *,name)
G_ALLOC_WRAP_ALLOC(GstClock*, gst_element_get_clock, objs, GstElement *,element)
G_ALLOC_WRAP_ALLOC(GstElement*, gst_pipeline_new, objs, const gchar *,name)

//G_ALLOC_WRAP_FREE(g_object_unref, gobjects, gpointer)
//G_ALLOC_WRAP_REF(g_object_ref, gobjects, gpointer)
G_ALLOC_WRAP_REF(gst_object_ref, objs, GstObject *)
G_ALLOC_WRAP_REF(gst_object_ref_sink, objs, GstObject *)

// --- Sample  wrappers ---
G_ALLOC_WRAP_FREE(gst_sample_unref, samples, GstSample *)
G_ALLOC_WRAP_ALLOC(GstSample*, gst_app_sink_pull_sample, samples, GstAppSink *,appsink)
G_ALLOC_WRAP_ALLOC2(GstSample*, gst_app_sink_try_pull_sample, samples, GstAppSink *,appsink, guint64, timeout)
G_ALLOC_WRAP_ALLOC(GstSample*, gst_app_sink_pull_preroll, samples, GstAppSink *,appsink)
G_ALLOC_WRAP_ALLOC4(GstSample*, gst_sample_new, samples, GstBuffer *,buffer, GstCaps *,caps, GstSegment *, segment, GstStructure *,info)
G_ALLOC_WRAP_REF(gst_sample_ref, samples, GstSample *)

// --- caps wrappers ---
G_ALLOC_WRAP_FREE(gst_caps_unref, caps, GstCaps *)
G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_new_empty, caps, void)
G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_new_any, caps, void)
G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_copy, caps, const GstCaps *,caps)
G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_copy_nth, caps, const GstCaps *,caps, guint, nth)
G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_caps_copy_and_set_caps_features, caps, const GstCaps *,caps, guint, index, const GstCapsFeatures *,features)
G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_from_string, caps, const gchar *,string)
G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_merge, caps, GstCaps *,caps1, GstCaps *,caps2)
G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_merge_structure, caps, GstCaps *,caps, GstStructure *,structure)
G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_normalize, caps, GstCaps *,caps)
G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_caps_merge_structure_full, caps, GstCaps *,caps, GstStructure *,structure, GstCapsFeatures *,features)
G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_type_find_helper_for_buffer, caps, GstObject *,obj, GstBuffer *,buf, GstCaps *,filter)
G_ALLOC_WRAP_ALLOC4(GstCaps*, gst_type_find_helper_for_buffer_with_caps, caps, GstObject *,obj, GstBuffer *,buf, GstCaps *,caps, GstCaps *,filter)
G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_intersect, caps, const GstCaps *,caps1, const GstCaps *,caps2)
G_ALLOC_WRAP_ALLOC3(GstCaps*, gst_caps_intersect_full, caps, const GstCaps *,caps1, const GstCaps *,caps2, GstCapsIntersectMode, mode)
G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_union, caps, const GstCaps *,caps1, const GstCaps *,caps2)
G_ALLOC_WRAP_ALLOC2(GstCaps*, gst_caps_subtract, caps, const GstCaps *,minuend, const GstCaps *,subtrahend)
G_ALLOC_WRAP_ALLOC(GstCaps*, gst_caps_fixate, caps, const GstCaps *,caps)

// -- variadic --
static inline GstCaps* AL_gst_caps_new_simple(const char *media_type, const char *fieldname, ...) {
    va_list args;
    va_start(args, fieldname);
    GstCaps *caps = gst_caps_new_simple_valist(media_type, fieldname, args);
    va_end(args);
    if (caps != NULL) g_alloc_counts.caps++;
    return caps;
}

static inline GstCaps* 
AL_gst_type_find_helper(GstObject *obj, GstCaps *filter, ...) {
    va_list args;
    va_start(args, filter);
    GstCaps *caps = gst_type_find_helper_valist(obj, filter, args);
    va_end(args);
    if (caps != NULL) g_alloc_counts.caps++;
    return caps;
}

// --- caps features ---
G_ALLOC_WRAP_FREE(gst_caps_features_unref, features, GstCapsFeatures *)
//G_ALLOC_WRAP_ALLOC(GstCapsFeatures*, gst_caps_features_new, features, const gchar *feature1, ...)         //weird one
G_ALLOC_WRAP_REF(gst_caps_features_ref, features, GstCapsFeatures *)

// -- variadic --
static inline GstCapsFeatures* AL_gst_caps_features_new(const gchar *feature1, ...) {
    va_list args;
    va_start(args, feature1);
    GstCapsFeatures *f = gst_caps_features_new_valist(feature1, args);
    va_end(args);
    if (f) g_alloc_counts.features++;
    return f;
}

// --- memory ---
G_ALLOC_WRAP_FREE(gst_memory_unref, memories, GstMemory *)
G_ALLOC_WRAP_ALLOC3(GstMemory*, gst_allocator_alloc, memories, GstAllocator *,allocator, gsize, size, GstAllocationParams *,params)
G_ALLOC_WRAP_ALLOC7(GstMemory*, gst_memory_new_wrapped, memories, GstMemoryFlags, flags, gpointer, data, gsize ,maxsize, gsize ,offset, gsize, size, GDestroyNotify ,notify, gpointer, user_data)
G_ALLOC_WRAP_REF(gst_memory_ref, memories, GstMemory *)

// -- events --
G_ALLOC_WRAP_FREE(gst_event_unref, events, GstEvent *)
G_ALLOC_WRAP_REF(gst_event_ref, events, GstEvent *)
G_ALLOC_WRAP_ALLOC(GstEvent*, gst_event_new_eos, events, void)

// -- messages --
G_ALLOC_WRAP_FREE(gst_message_unref, messages, GstMessage *)
G_ALLOC_WRAP_REF(gst_message_ref, messages, GstMessage *)

#endif



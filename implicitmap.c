//
// implicitmap.c
// a maxmsp and puredata external encapsulating the functionality of libmapper
// for performing implicit mapping
// http://www.idmil.org/software/libmapper
// Joseph Malloch, IDMIL 2010
//
// This software was written in the Input Devices and Music Interaction
// Laboratory at McGill University in Montreal, and is copyright those
// found in the AUTHORS file.  It is licensed under the GNU Lesser Public
// General License version 2.1 or later.  Please see COPYING for details.
//

// *********************************************************
// -(Includes)----------------------------------------------

#ifdef MAXMSP
    #include "ext.h"            // standard Max include, always required
    #include "ext_obex.h"       // required for new style Max object
    #include "ext_dictionary.h"
    #include "jpatcher_api.h"
#else
    #include "m_pd.h"
    #define A_SYM A_SYMBOL
#endif
#include "mapper/mapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <lo/lo.h>

#include <unistd.h>
#include <arpa/inet.h>

#define INTERVAL 1
#define MAX_LIST 256

// *********************************************************
// -(object struct)-----------------------------------------
typedef struct _signal_ref
{
    void *x;
    int offset;
} t_signal_ref;

typedef struct _snapshot
{
    int id;
    float *inputs;
    float *outputs;
    struct _snapshot *next;
} *t_snapshot;

typedef struct _impmap
{
    t_object ob;
    void *outlet1;
    void *outlet2;
    void *outlet3;
    void *clock;          // pointer to clock object
    void *timeout;
    char *name;
    mapper_device device;
    mapper_signal dummy_input;
    mapper_signal dummy_output;
    mapper_timetag_t tt;
    int ready;
    int mute;
    int new_in;
    int num_snapshots;
    t_snapshot snapshots;
    t_atom buffer_in[MAX_LIST];
    int size_in;
    t_atom buffer_out[MAX_LIST];
    int size_out;
    int query_count;
    t_atom msg_buffer;
    t_signal_ref signals_in[MAX_LIST];
    t_signal_ref signals_out[MAX_LIST];
} impmap;

static t_symbol *ps_list;
static int port = 9000;

// *********************************************************
// -(function prototypes)-----------------------------------
static void *impmap_new(t_symbol *s, int argc, t_atom *argv);
static void impmap_free(impmap *x);
static void impmap_list(impmap *x, t_symbol *s, int argc, t_atom *argv);
static void impmap_poll(impmap *x);
static void impmap_randomize(impmap *x);
static void impmap_on_input(mapper_signal sig, mapper_id instance,
                            const void *value, int count, mapper_timetag_t *tt);
static void impmap_on_query(mapper_signal sig, mapper_id instance,
                            const void *value, int count, mapper_timetag_t *tt);
static void impmap_on_map(mapper_device dev, mapper_map map,
                          mapper_record_event e);
static void impmap_print_properties(impmap *x);
static int impmap_setup_mapper(impmap *x, const char *iface);
static void impmap_snapshot(impmap *x);
static void impmap_output_snapshot(impmap *x);
static void impmap_clear_snapshots(impmap *x);
static void impmap_mute_output(impmap *x, t_symbol *s, int argc, t_atom *argv);
static void impmap_process(impmap *x);
static void impmap_save(impmap *x);
static void impmap_load(impmap *x);
#ifdef MAXMSP
    void impmap_assist(impmap *x, void *b, long m, long a, char *s);
#endif
static void impmap_update_input_vector_positions(impmap *x);
static void impmap_update_output_vector_positions(impmap *x);
static const char *maxpd_atom_get_string(t_atom *a);
static void maxpd_atom_set_string(t_atom *a, const char *string);
static void maxpd_atom_set_int(t_atom *a, int i);
static double maxpd_atom_get_float(t_atom *a);
static void maxpd_atom_set_float(t_atom *a, float d);
void maxpd_atom_set_float_array(t_atom *a, float *d, int length);

// *********************************************************
// -(global class pointer variable)-------------------------
static void *mapper_class;

// *********************************************************
// -(main)--------------------------------------------------
#ifdef MAXMSP
int main(void)
{
    t_class *c;
    c = class_new("implicitmap", (method)impmap_new, (method)impmap_free,
                  (long)sizeof(impmap), 0L, A_GIMME, 0);
    class_addmethod(c, (method)impmap_assist,           "assist",    A_CANT,  0);
    class_addmethod(c, (method)impmap_snapshot,         "snapshot",  A_GIMME, 0);
    class_addmethod(c, (method)impmap_randomize,        "randomize", A_GIMME, 0);
    class_addmethod(c, (method)impmap_list,             "list",      A_GIMME, 0);
    class_addmethod(c, (method)impmap_print_properties, "print",     A_GIMME, 0);
    class_addmethod(c, (method)impmap_clear_snapshots,  "clear",     A_GIMME, 0);
    class_addmethod(c, (method)impmap_mute_output,      "mute",      A_GIMME, 0);
    class_addmethod(c, (method)impmap_process,          "process",   A_GIMME, 0);
    class_addmethod(c, (method)impmap_save,             "export",    A_GIMME, 0);
    class_addmethod(c, (method)impmap_load,             "import",    A_GIMME, 0);
    class_register(CLASS_BOX, c); /* CLASS_NOBOX */
    mapper_class = c;
    ps_list = gensym("list");
    return 0;
}
#else
int implicitmap_setup(void)
{
    t_class *c;
    c = class_new(gensym("implicitmap"), (t_newmethod)impmap_new, (t_method)impmap_free,
                  (long)sizeof(impmap), 0L, A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_snapshot,         gensym("snapshot"),  A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_randomize,        gensym("randomize"), A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_list,             gensym("list"),      A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_print_properties, gensym("print"),     A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_clear_snapshots,  gensym("clear"),     A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_mute_output,      gensym("mute"),      A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_process,          gensym("process"),   A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_save,             gensym("export"),      A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_load,             gensym("import"),      A_GIMME, 0);
    mapper_class = c;
    ps_list = gensym("list");
    return 0;
}
#endif

// *********************************************************
// -(new)---------------------------------------------------
void *impmap_new(t_symbol *s, int argc, t_atom *argv)
{
    impmap *x = NULL;
    long i;
    const char *alias = NULL;
    const char *iface = NULL;

#ifdef MAXMSP
    if ((x = object_alloc(mapper_class))) {
        x->outlet3 = listout((t_object *)x);
        x->outlet2 = listout((t_object *)x);
        x->outlet1 = listout((t_object *)x);
#else
    if (x = (impmap *) pd_new(mapper_class) ) {
        x->outlet1 = outlet_new(&x->ob, gensym("list"));
        x->outlet2 = outlet_new(&x->ob, gensym("list"));
        x->outlet3 = outlet_new(&x->ob, gensym("list"));
#endif
        x->name = strdup("implicitmap");
        for (i = 0; i < argc; i++) {
            if ((argv + i)->a_type == A_SYM) {
                if(strcmp(maxpd_atom_get_string(argv+i), "@alias") == 0) {
                    if ((argv+i+1)->a_type == A_SYM) {
                        alias = maxpd_atom_get_string(argv+i+1);
                        i++;
                    }
                }
                else if(strcmp(maxpd_atom_get_string(argv+i), "@interface") == 0) {
                    if ((argv+i+1)->a_type == A_SYM) {
                        iface = maxpd_atom_get_string(argv+i+1);
                        i++;
                    }
                }
            }
        }

        if (alias) {
            free(x->name);
            x->name = *alias == '/' ? strdup(alias+1) : strdup(alias);
        }

        if (impmap_setup_mapper(x, iface)) {
            post("implicitmap: Error initializing.");
        }
        else {
            x->ready = 0;
            x->mute = 0;
            x->new_in = 0;
            x->query_count = 0;
            x->num_snapshots = 0;
            x->snapshots = 0;
            // initialize input and output buffers
            for (i = 0; i < MAX_LIST; i++) {
                maxpd_atom_set_float(x->buffer_in+i, 0);
                maxpd_atom_set_float(x->buffer_out+i, 0);
                x->signals_in[i].x = x;
                x->signals_out[i].x = x;
            }
            x->size_in = 0;
            x->size_out = 0;
#ifdef MAXMSP
            x->clock = clock_new(x, (method)impmap_poll);    // Create the timing clock
            x->timeout = clock_new(x, (method)impmap_output_snapshot);
#else
            x->clock = clock_new(x, (t_method)impmap_poll);
            x->timeout = clock_new(x, (t_method)impmap_output_snapshot);
#endif
            clock_delay(x->clock, INTERVAL);  // Set clock to go off after delay
        }
    }
    return (x);
}

// *********************************************************
// -(free)--------------------------------------------------
void impmap_free(impmap *x)
{
    if (x->clock) {
        clock_unset(x->clock);    // Remove clock routine from the scheduler
        clock_free(x->clock);     // Frees memory used by clock
    }
    if (x->device) {
        mapper_device_free(x->device);
    }
    if (x->name) {
        free(x->name);
    }
    impmap_clear_snapshots(x);
}

// *********************************************************
// -(print properties)--------------------------------------
void impmap_print_properties(impmap *x)
{
    if (x->ready) {
        //output name
        maxpd_atom_set_string(&x->msg_buffer, mapper_device_name(x->device));
        outlet_anything(x->outlet3, gensym("name"), 1, &x->msg_buffer);

        mapper_network net = mapper_device_network(x->device);
        //output interface
        maxpd_atom_set_string(&x->msg_buffer, (char *)mapper_network_interface(net));
        outlet_anything(x->outlet3, gensym("interface"), 1, &x->msg_buffer);

        //output IP
        const struct in_addr *ip = mapper_network_ip4(net);
        if (ip) {
            maxpd_atom_set_string(&x->msg_buffer, inet_ntoa(*ip));
            outlet_anything(x->outlet3, gensym("IP"), 1, &x->msg_buffer);
        }

        //output port
        maxpd_atom_set_int(&x->msg_buffer, mapper_device_port(x->device));
        outlet_anything(x->outlet3, gensym("port"), 1, &x->msg_buffer);

        //output numInputs
        maxpd_atom_set_int(&x->msg_buffer, mapper_device_num_signals(x->device, MAPPER_DIR_INCOMING) - 1);
        outlet_anything(x->outlet3, gensym("numInputs"), 1, &x->msg_buffer);

        //output numOutputs
        maxpd_atom_set_int(&x->msg_buffer, mapper_device_num_signals(x->device, MAPPER_DIR_OUTGOING) - 1);
        outlet_anything(x->outlet3, gensym("numOutputs"), 1, &x->msg_buffer);
    }
}

// *********************************************************
// -(inlet/outlet assist - maxmsp only)---------------------
#ifdef MAXMSP
void impmap_assist(impmap *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET) { // inlet
        sprintf(s, "OSC input");
    }
    else {    // outlet
        if (a == 0) {
            sprintf(s, "Mapped OSC inputs");
        }
        else if (a == 1) {
            sprintf(s, "Snapshot data");
        }
        else {
            sprintf(s, "Device information");
        }
    }
}
#endif

// *********************************************************
// -(snapshot)----------------------------------------------
void impmap_snapshot(impmap *x)
{
    // if previous snapshot still in progress, output current snapshot status
    if (x->query_count) {
        post("still waiting for last snapshot");
        return;
    }

    mapper_signal *psig;
    x->query_count = 0;

    // allocate a new snapshot
    if (x->ready) {
        t_snapshot new_snapshot = (t_snapshot)malloc(sizeof(t_snapshot));
        new_snapshot->id = x->num_snapshots++;
        new_snapshot->next = x->snapshots;
        new_snapshot->inputs = calloc(x->size_in, sizeof(float));
        new_snapshot->outputs = calloc(x->size_out, sizeof(float));
        x->snapshots = new_snapshot;
    }

    // iterate through input signals and store their current values
    psig = mapper_device_signals(x->device, MAPPER_DIR_INCOMING);
    while (psig) {
        if (*psig != x->dummy_input) {
            void *value = (void*)mapper_signal_value(*psig, 0);
            t_signal_ref *ref = mapper_signal_user_data(*psig);
            int siglength = mapper_signal_length(*psig);
            int length = ref->offset + siglength < MAX_LIST ? siglength : MAX_LIST - ref->offset;
            // we can simply use memcpy here since all our signals are type 'f'
            memcpy(&x->snapshots->inputs[ref->offset], value, length * sizeof(float));
        }
        psig = mapper_signal_query_next(psig);
    }

    mapper_timetag_now(&x->tt);
    mapper_device_start_queue(x->device, x->tt);

    // iterate through output signals and query the remote ends
    psig = mapper_device_signals(x->device, MAPPER_DIR_OUTGOING);
    while (psig) {
        if (*psig != x->dummy_output) {
            // query the remote value
            x->query_count += mapper_signal_query_remotes(*psig, MAPPER_NOW);
        }
        psig = mapper_signal_query_next(psig);
    }
    mapper_device_send_queue(x->device, x->tt);
    post("sent %i queries", x->query_count);

    if (x->query_count)
        clock_delay(x->timeout, 1000);  // Set clock to go off after delay
}

// *********************************************************
// -(snapshot)----------------------------------------------
void impmap_output_snapshot(impmap *x)
{
    if (x->query_count) {
        post("query timeout! setting query count to 0 and outputting current values.");
        x->query_count = 0;
    }
    maxpd_atom_set_int(x->buffer_in, x->snapshots->id+1);
    outlet_anything(x->outlet3, gensym("numSnapshots"), 1, x->buffer_in);
    maxpd_atom_set_float_array(x->buffer_in, x->snapshots->inputs, x->size_in);
    outlet_anything(x->outlet2, gensym("in"), x->size_in, x->buffer_in);
    maxpd_atom_set_float_array(x->buffer_out, x->snapshots->outputs, x->size_out);
    outlet_anything(x->outlet2, gensym("out"), x->size_out, x->buffer_out);
    maxpd_atom_set_int(x->buffer_in, x->snapshots->id);
    outlet_anything(x->outlet2, gensym("snapshot"), 1, x->buffer_in);
}

// *********************************************************
// -(mute output)-------------------------------------------
void impmap_mute_output(impmap *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc) {
        if (argv->a_type == A_FLOAT)
            x->mute = (int)atom_getfloat(argv);
#ifdef MAXMSP
        else if (argv->a_type == A_LONG)
            x->mute = atom_getlong(argv);
#endif
    }
}

// *********************************************************
// -(process)-----------------------------------------------
void impmap_process(impmap *x)
{
    outlet_anything(x->outlet2, gensym("process"), 0, 0);
}

// *********************************************************
// -(save)--------------------------------------------------
void impmap_save(impmap *x)
{
    outlet_anything(x->outlet2, gensym("export"), 0, 0);
}

// *********************************************************
// -(load)--------------------------------------------------
void impmap_load(impmap *x)
{
    outlet_anything(x->outlet2, gensym("import"), 0, 0);
}

// *********************************************************
// -(randomize)---------------------------------------------
void impmap_randomize(impmap *x)
{
    int i;
    float rand_val;

    if (x->ready) {
        mapper_timetag_now(&x->tt);
        mapper_device_start_queue(x->device, x->tt);
        mapper_signal *psig = mapper_device_signals(x->device, MAPPER_DIR_OUTGOING);
        while (psig) {
            if (*psig == x->dummy_output) {
                psig = mapper_signal_query_next(psig);
                continue;
            }
            t_signal_ref *ref = mapper_signal_user_data(*psig);
            if (mapper_signal_type(*psig) != 'f')
                continue;
            int length = mapper_signal_length(*psig);
            float v[length];
            float *min = (float*)mapper_signal_minimum(*psig);
            float *max = (float*)mapper_signal_maximum(*psig);
            for (i = 0; i < length; i++) {
                rand_val = (float)rand() / (float)RAND_MAX;
                if (min && max) {
                    v[i] = rand_val * (max[i] - min[i]) + min[i];
                }
                else {
                    // if ranges have not been declared, assume normalized between 0 and 1
                    v[i] = rand_val;
                }
                maxpd_atom_set_float(x->buffer_out + ref->offset + i, v[i]);
            }
            mapper_signal_update(*psig, v, 1, x->tt);
            psig = mapper_signal_query_next(psig);
        }
        mapper_device_send_queue(x->device, x->tt);
        outlet_anything(x->outlet2, gensym("out"), x->size_out, x->buffer_out);
    }
}

// *********************************************************
// -(anything)----------------------------------------------
void impmap_list(impmap *x, t_symbol *s, int argc, t_atom *argv)
{
    if (x->mute)
        return;

    if (argc != x->size_out) {
        post("vector size mismatch");
        return;
    }

    int i = 0;

    mapper_timetag_now(&x->tt);
    mapper_device_start_queue(x->device, x->tt);

    mapper_signal *psig = mapper_device_signals(x->device, MAPPER_DIR_OUTGOING);
    while (psig) {
        if (*psig == x->dummy_output) {
            psig = mapper_signal_query_next(psig);
            continue;
        }
        t_signal_ref *ref = mapper_signal_user_data(*psig);
        int len = mapper_signal_length(*psig);
        float v[len];
        for (i = 0; i < len; i++) {
            v[i] = atom_getfloat(argv + ref->offset + i);
        }
        mapper_signal_update(*psig, v, 1, x->tt);

        psig = mapper_signal_query_next(psig);
    }
    mapper_device_send_queue(x->device, x->tt);
    outlet_anything(x->outlet2, gensym("out"), argc, argv);
}

// *********************************************************
// -(input handler)-----------------------------------------
void impmap_on_input(mapper_signal sig, mapper_id instance, const void *value,
                     int count, mapper_timetag_t *time)
{
    t_signal_ref *ref = mapper_signal_user_data(sig);
    if (!ref) {
        post("implicitmap: no user data for signal '%s' in on_input()",
             mapper_signal_name(sig));
    }
    impmap *x = ref->x;

    int i, len = mapper_signal_length(sig);
    float *valf = (float*)value;
    for (i = 0; i < len; i++) {
        if (ref->offset + i >= MAX_LIST) {
            post("implicitmap: Maximum vector length exceeded!");
            break;
        }
        maxpd_atom_set_float(x->buffer_in + ref->offset + i, valf ? valf[i] : 0);
    }
    x->new_in = 1;
}

// *********************************************************
// -(query handler)-----------------------------------------
void impmap_on_query(mapper_signal sig, mapper_id instance, const void *value,
                     int count, mapper_timetag_t *time)
{
    t_signal_ref *ref = mapper_signal_user_data(sig);
    if (!ref) {
        post("implicitmap: no user data for signal '%s' in on_input()",
             mapper_signal_name(sig));
    }
    impmap *x = ref->x;

    int i, len = mapper_signal_length(sig);
    float *valf = (float*)value;
    for (i = 0; i < len; i++) {
        if (ref->offset + i >= MAX_LIST) {
            post("mapper: Maximum vector length exceeded!");
            break;
        }
        if (valf)
            x->snapshots->outputs[ref->offset + i] = valf[i];
    }

    x->query_count --;

    if (x->query_count == 0) {
        clock_unset(x->timeout);
        impmap_output_snapshot(x);
    }
}

// *********************************************************
// -(connection handler)------------------------------------
void impmap_on_map(mapper_device dev, mapper_map map, mapper_record_event e)
{
    // if connected involves current generic signal, create a new generic signal
    impmap *x = (void*)mapper_device_user_data(dev);
    if (!x) {
        post("error in connect handler: user_data is NULL");
        return;
    }
    if (!x->ready) {
        post("error in connect handler: device not ready");
        return;
    }

    // retrieve devices and signals
    mapper_slot slot = mapper_map_slot(map, MAPPER_LOC_SOURCE, 0);
    mapper_signal src_sig = mapper_slot_signal(slot);
    mapper_device src_dev = mapper_signal_device(src_sig);
    slot = mapper_map_slot(map, MAPPER_LOC_DESTINATION, 0);
    mapper_signal dst_sig = mapper_slot_signal(slot);
    mapper_device dst_dev = mapper_signal_device(dst_sig);

    // sanity check: don't allow self-connections
    if (src_dev == dst_dev) {
        mapper_map_release(map);
        return;
    }

    char full_name[256];

    if (e == MAPPER_ADDED) {
        if (src_dev == x->device) {
            snprintf(full_name, 256, "%s/%s", mapper_device_name(dst_dev),
                     mapper_signal_name(dst_sig));
            if (strcmp(mapper_signal_name(src_sig), full_name) == 0) {
                // <thisDev>:<dstDevName>/<dstSigName> -> <dstDev>:<dstSigName>
                return;
            }
            if (mapper_device_num_signals(x->device, MAPPER_DIR_OUTGOING) >= MAX_LIST) {
                post("Max outputs reached!");
                return;
            }
            // unmap the generic signal
            mapper_map_release(map);

            // add a matching output signal
            int i, length = mapper_signal_length(dst_sig);
            char type = mapper_signal_type(dst_sig);
            void *min = mapper_signal_minimum(dst_sig);
            void *max = mapper_signal_maximum(dst_sig);

            float *minf = 0, *maxf = 0;
            if (type == 'f') {
                minf = (float*)min;
                maxf = (float*)max;
            }
            else {
                if (min) {
                    minf = alloca(length * sizeof(float));
                    if (type == 'i') {
                        int *mini = (int*)min;
                        for (i = 0; i < length; i++)
                            minf[i] = (float)mini[i];
                    }
                    else if (type == 'd') {
                        double *mind = (double*)min;
                        for (i = 0; i < length; i++)
                            minf[i] = (float)mind[i];
                    }
                    else
                        minf = 0;
                }
                if (max) {
                    maxf = alloca(length * sizeof(float));
                    if (type == 'i') {
                        int *maxi = (int*)max;
                        for (i = 0; i < length; i++)
                            maxf[i] = (float)maxi[i];
                    }
                    else if (type == 'd') {
                        double *maxd = (double*)max;
                        for (i = 0; i < length; i++)
                            maxf[i] = (float)maxd[i];
                    }
                    else
                        maxf = 0;
                }
            }
            src_sig = mapper_device_add_output_signal(x->device, full_name,
                                                      length, 'f', 0, minf, maxf);
            if (!src_sig) {
                post("error creating new source signal!");
                return;
            }
            mapper_signal_set_callback(src_sig, impmap_on_query);

            // map the new signal
            map = mapper_map_new(1, &src_sig, 1, &dst_sig);
            mapper_map_set_mode(map, MAPPER_MODE_EXPRESSION);
            mapper_map_set_expression(map, "y=x");
            mapper_map_push(map);

            impmap_update_output_vector_positions(x);

            //output numOutputs
            maxpd_atom_set_int(&x->msg_buffer,
                               mapper_device_num_signals(x->device, MAPPER_DIR_OUTGOING) - 1);
            outlet_anything(x->outlet3, gensym("numOutputs"), 1, &x->msg_buffer);
        }
        else if (dst_dev == x->device) {
            snprintf(full_name, 256, "%s/%s", mapper_device_name(src_dev),
                     mapper_signal_name(src_sig));
            if (strcmp(mapper_signal_name(dst_sig), full_name) == 0) {
                // <srcDevName>:<srcSigName> -> <thisDev>:<srcDevName>/<srcSigName>
                return;
            }
            if (mapper_device_num_signals(x->device, MAPPER_DIR_INCOMING) >= MAX_LIST) {
                post("Max inputs reached!");
                return;
            }
            // unmap the generic signal
            mapper_map_release(map);

            // add a matching input signal
            int i, length = mapper_signal_length(src_sig);
            char type = mapper_signal_type(src_sig);
            void *min = mapper_signal_minimum(src_sig);
            void *max = mapper_signal_maximum(src_sig);

            float *minf = 0, *maxf = 0;
            if (type == 'f') {
                minf = (float*)min;
                maxf = (float*)max;
            }
            else {
                if (min) {
                    minf = alloca(length * sizeof(float));
                    if (type == 'i') {
                        int *mini = (int*)min;
                        for (i = 0; i < length; i++)
                        minf[i] = (float)mini[i];
                    }
                    else if (type == 'd') {
                        double *mind = (double*)min;
                        for (i = 0; i < length; i++)
                        minf[i] = (float)mind[i];
                    }
                    else
                        minf = 0;
                }
                if (max) {
                    maxf = alloca(length * sizeof(float));
                    if (type == 'i') {
                        int *maxi = (int*)max;
                        for (i = 0; i < length; i++)
                        maxf[i] = (float)maxi[i];
                    }
                    else if (type == 'd') {
                        double *maxd = (double*)max;
                        for (i = 0; i < length; i++)
                        maxf[i] = (float)maxd[i];
                    }
                    else
                        maxf = 0;
                }
            }
            dst_sig = mapper_device_add_input_signal(x->device, full_name,
                                                     length, 'f', 0, minf, maxf,
                                                     impmap_on_input, 0);
            if (!dst_sig) {
                post("error creating new destination signal!");
                return;
            }

            // map the new signal
            map = mapper_map_new(1, &src_sig, 1, &dst_sig);
            mapper_map_set_mode(map, MAPPER_MODE_EXPRESSION);
            mapper_map_set_expression(map, "y=x");
            mapper_map_push(map);

            impmap_update_input_vector_positions(x);

            //output numInputs
            maxpd_atom_set_int(&x->msg_buffer,
                               mapper_device_num_signals(x->device, MAPPER_DIR_INCOMING) - 1);
            outlet_anything(x->outlet3, gensym("numInputs"), 1, &x->msg_buffer);
        }
    }
    else if (e == MAPPER_REMOVED) {
        if (src_sig == x->dummy_input || src_sig == x->dummy_output
            || dst_sig == x->dummy_input || dst_sig == x->dummy_output)
            return;
        if (src_dev == x->device) {
            snprintf(full_name, 256, "%s/%s", mapper_device_name(dst_dev),
                     mapper_signal_name(dst_sig));
            if (strcmp(mapper_signal_name(src_sig), full_name) != 0)
                return;
            // remove signal
            mapper_device_remove_signal(x->device, src_sig);
            impmap_update_input_vector_positions(x);

            //output numOutputs
            maxpd_atom_set_int(&x->msg_buffer,
                               mapper_device_num_signals(x->device, MAPPER_DIR_OUTGOING) - 1);
            outlet_anything(x->outlet3, gensym("numOutputs"), 1, &x->msg_buffer);
        }
        else if (dst_dev == x->device) {
            snprintf(full_name, 256, "%s/%s", mapper_device_name(src_dev),
                     mapper_signal_name(src_sig));
            if (strcmp(mapper_signal_name(dst_sig), full_name) != 0)
                return;
            // remove signal
            mapper_device_remove_signal(x->device, dst_sig);
            impmap_update_input_vector_positions(x);

            //output numInputs
            maxpd_atom_set_int(&x->msg_buffer,
                               mapper_device_num_signals(x->device, MAPPER_DIR_INCOMING) - 1);
            outlet_anything(x->outlet3, gensym("numInputs"), 1, &x->msg_buffer);
        }
    }
}

// *********************************************************
// -(compare signal names for qsort)------------------------
int compare_signal_names(const void *l, const void *r)
{
    return strcmp(mapper_signal_name(*(mapper_signal*)l),
                  mapper_signal_name(*(mapper_signal*)r));
}

// *********************************************************
// -(set up new device and monitor)-------------------------
void impmap_update_input_vector_positions(impmap *x)
{
    int i, k=0, count;
    int num_inputs = mapper_device_num_signals(x->device, MAPPER_DIR_INCOMING) - 1;

    // store input signal pointers
    mapper_signal signals[num_inputs];
    mapper_signal *psig = mapper_device_signals(x->device, MAPPER_DIR_INCOMING);

    i = 0;
    while (psig) {
        if (*psig != x->dummy_input)
            signals[i++] = *psig;
        psig = mapper_signal_query_next(psig);
    }

    // sort input signal pointer array
    qsort(signals, num_inputs, sizeof(mapper_signal), compare_signal_names);

    // set offsets and user_data
    for (i = 0; i < num_inputs; i++) {
        x->signals_in[i].offset = k;
        mapper_signal_set_user_data(signals[i], &x->signals_in[i]);
        k += mapper_signal_length(signals[i]);
    }
    count = k < MAX_LIST ? k : MAX_LIST;
    if (count != x->size_in && x->num_snapshots) {
        post("implicitmap: input vector size has changed - resetting snapshots!");
        impmap_clear_snapshots(x);
    }
    x->size_in = count;
}

// *********************************************************
// -(set up new device and monitor)-------------------------
void impmap_update_output_vector_positions(impmap *x)
{
    int i, k=0, count;
    int num_outputs = mapper_device_num_signals(x->device, MAPPER_DIR_OUTGOING) - 1;

    // store output signal pointers
    mapper_signal signals[num_outputs];
    mapper_signal *psig = mapper_device_signals(x->device, MAPPER_DIR_OUTGOING);

    i = 0;
    while (psig) {
        if (*psig != x->dummy_output)
            signals[i++] = *psig;
        psig = mapper_signal_query_next(psig);
    }

    // sort output signal pointer array
    qsort(signals, num_outputs, sizeof(mapper_signal), compare_signal_names);

    // set offsets and user_data
    for (i = 0; i < num_outputs; i++) {
        x->signals_out[i].offset = k;
        mapper_signal_set_user_data(signals[i], &x->signals_out[i]);
        k += mapper_signal_length(signals[i]);
    }
    count = k < MAX_LIST ? k : MAX_LIST;
    if (count != x->size_out && x->num_snapshots) {
        post("implicitmap: output vector size has changed - resetting snapshots!");
        impmap_clear_snapshots(x);
    }
    x->size_out = count;
}

// *********************************************************
// -(set up new device and monitor)-------------------------
int impmap_setup_mapper(impmap *x, const char *iface)
{
    post("using name: %s", x->name);
    x->device = 0;

    x->device = mapper_device_new(x->name, port, 0);
    if (!x->device)
        return 1;

    mapper_device_set_user_data(x->device, x);
    mapper_device_set_map_callback(x->device, impmap_on_map);

    impmap_print_properties(x);

    return 0;
}

// *********************************************************
// -(poll libmapper)----------------------------------------
void impmap_poll(impmap *x)
{
    mapper_device_poll(x->device, 0);
    if (!x->ready) {
        if (mapper_device_ready(x->device)) {
            x->ready = 1;

            // create a new generic output signal
            x->dummy_output = mapper_device_add_output_signal(x->device,
                                                              "CONNECT_TO_DESTINATION",
                                                              1, 'f', 0, 0, 0);

            // create a new generic input signal
            x->dummy_input = mapper_device_add_input_signal(x->device,
                                                            "CONNECT_TO_SOURCE",
                                                            1, 'f', 0, 0, 0, 0, x);

            impmap_print_properties(x);
        }
    }
    if (x->new_in) {
        outlet_anything(x->outlet1, gensym("list"), x->size_in, x->buffer_in);
        x->new_in = 0;
    }
    clock_delay(x->clock, INTERVAL);  // Set clock to go off after delay
}

// *********************************************************
// -(poll libmapper)----------------------------------------
void impmap_clear_snapshots(impmap *x)
{
    while (x->snapshots) {
        t_snapshot temp = x->snapshots->next;
        free(x->snapshots->inputs);
        free(x->snapshots->outputs);
        x->snapshots = temp;
    }
    x->num_snapshots = 0;
    outlet_anything(x->outlet2, gensym("clear"), 0, 0);
    maxpd_atom_set_int(x->buffer_in, 0);
    outlet_anything(x->outlet3, gensym("numSnapshots"), 1, x->buffer_in);
}

// *********************************************************
// some helper functions for abtracting differences
// between maxmsp and puredata

const char *maxpd_atom_get_string(t_atom *a)
{
#ifdef MAXMSP
    return atom_getsym(a)->s_name;
#else
    return (a)->a_w.w_symbol->s_name;
#endif
}

void maxpd_atom_set_string(t_atom *a, const char *string)
{
#ifdef MAXMSP
    atom_setsym(a, gensym((char *)string));
#else
    SETSYMBOL(a, gensym(string));
#endif
}

void maxpd_atom_set_int(t_atom *a, int i)
{
#ifdef MAXMSP
    atom_setlong(a, (long)i);
#else
    SETFLOAT(a, (double)i);
#endif
}

double maxpd_atom_get_float(t_atom *a)
{
    return (double)atom_getfloat(a);
}

void maxpd_atom_set_float(t_atom *a, float d)
{
#ifdef MAXMSP
    atom_setfloat(a, d);
#else
    SETFLOAT(a, d);
#endif
}

void maxpd_atom_set_float_array(t_atom *a, float *d, int length)
{
#ifdef MAXMSP
    atom_setfloat_array(length, a, length, d);
#else
    int i;
    for (i=0; i<length; i++) {
        SETFLOAT(a+i, d[i]);
    }
#endif
}

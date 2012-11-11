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
#include "mapper/mapper_db.h"
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
    void                *outlet1;
    void                *outlet2;
    void                *outlet3;
    void                *clock;
    char                *name;
    mapper_admin        admin;
    mapper_device       device;
    mapper_monitor      monitor;
    mapper_db           db;
    mapper_timetag_t    tt;
    int                 ready;
    int                 mute;
    int                 new_in;
    int                 new_out;
    int                 num_snapshots;
    t_snapshot          snapshots;
    t_atom              buffer_in[MAX_LIST];
    int                 size_in;
    t_atom              buffer_out[MAX_LIST];
    int                 size_out;
    t_atom              msg_buffer;
    t_signal_ref        signals_in[MAX_LIST];
    t_signal_ref        signals_out[MAX_LIST];
} impmap;

static t_symbol *ps_list;
static int port = 9000;

// *********************************************************
// -(function prototypes)-----------------------------------
static void *impmap_new(t_symbol *s, int argc, t_atom *argv);
static void impmap_free(impmap *x);
static void impmap_list(impmap *x, t_symbol *s, int argc, t_atom *argv);
static void impmap_poll(impmap *x);
static void impmap_input_handler(mapper_signal sig, mapper_db_signal props,
                                 int instance_id, void *value, int count,
                                 mapper_timetag_t *tt);
static void impmap_output_handler(mapper_signal sig, mapper_db_signal props,
                                  int instance_id, void *value, int count,
                                  mapper_timetag_t *tt);
static void impmap_link_handler(mapper_db_link lnk, mapper_db_action_t a, void *user);
static void impmap_connect_handler(mapper_db_connection con, mapper_db_action_t a, void *user);
static void impmap_print_properties(impmap *x);
static int impmap_setup_mapper(impmap *x, const char *iface);
static void impmap_snapshot(impmap *x);
static void impmap_clear_snapshots(impmap *x);
static void impmap_process(impmap *x);
static void impmap_save(impmap *x);
static void impmap_load(impmap *x);
#ifdef MAXMSP
    void impmap_assist(impmap *x, void *b, long m, long a, char *s);
#endif
static void impmap_switch_modes(impmap *x, mapper_mode_type mode);
static void impmap_update_input_vector_positions(impmap *x);
static void impmap_update_output_vector_positions(impmap *x);
static const char *maxpd_atom_get_string(t_atom *a);
static void maxpd_atom_set_string(t_atom *a, const char *string);
static void maxpd_atom_set_int(t_atom *a, int i);
static double maxpd_atom_get_float(t_atom *a);
static void maxpd_atom_set_float(t_atom *a, float d);
void maxpd_atom_set_float_array(t_atom *a, float *d, int length);
static int osc_prefix_cmp(const char *str1, const char *str2, const char **rest);

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
    class_addmethod(c, (method)impmap_list,             "list",      A_GIMME, 0);
    class_addmethod(c, (method)impmap_print_properties, "print",     A_GIMME, 0);
    class_addmethod(c, (method)impmap_clear_snapshots,  "clear",     A_GIMME, 0);
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
    class_addmethod(c, (t_method)impmap_list,             gensym("list"),      A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_print_properties, gensym("print"),     A_GIMME, 0);
    class_addmethod(c, (t_method)impmap_clear_snapshots,  gensym("clear"),     A_GIMME, 0);
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
#else
            x->clock = clock_new(x, (t_method)impmap_poll);
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
    impmap_clear_snapshots(x);

    if (x->clock) {
        clock_unset(x->clock);    // Remove clock routine from the scheduler
        clock_free(x->clock);     // Frees memory used by clock
    }
    if (x->db) {
        mapper_db_remove_connection_callback(x->db, impmap_connect_handler, x);
    }
    if (x->monitor) {
        mapper_monitor_free(x->monitor);
    }
    if (x->device) {
        mdev_free(x->device);
    }
    if (x->admin) {
        mapper_admin_free(x->admin);
    }
    if (x->name) {
        free(x->name);
    }
}

// *********************************************************
// -(print properties)--------------------------------------
void impmap_print_properties(impmap *x)
{
    if (x->ready) {
        //output name
        maxpd_atom_set_string(&x->msg_buffer, mdev_name(x->device));
        outlet_anything(x->outlet3, gensym("name"), 1, &x->msg_buffer);

        //output interface
        maxpd_atom_set_string(&x->msg_buffer, (char *)mdev_interface(x->device));
        outlet_anything(x->outlet3, gensym("interface"), 1, &x->msg_buffer);

        //output IP
        const struct in_addr *ip = mdev_ip4(x->device);
        if (ip) {
            maxpd_atom_set_string(&x->msg_buffer, inet_ntoa(*ip));
            outlet_anything(x->outlet3, gensym("IP"), 1, &x->msg_buffer);
        }

        //output port
        maxpd_atom_set_int(&x->msg_buffer, mdev_port(x->device));
        outlet_anything(x->outlet3, gensym("port"), 1, &x->msg_buffer);

        //output numInputs
        maxpd_atom_set_int(&x->msg_buffer, mdev_num_inputs(x->device) - 1);
        outlet_anything(x->outlet3, gensym("numInputs"), 1, &x->msg_buffer);

        //output numOutputs
        maxpd_atom_set_int(&x->msg_buffer, mdev_num_outputs(x->device) - 1);
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
            sprintf(s, "Data from remote source");
        }
        else if (a == 1) {
            sprintf(s, "Data from remote destination");
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
    maxpd_atom_set_int(x->buffer_in, x->num_snapshots++);
    outlet_anything(x->outlet3, gensym("snapshot"), 1, x->buffer_in);
    maxpd_atom_set_int(x->buffer_in, x->num_snapshots);
    outlet_anything(x->outlet3, gensym("numSnapshots"), 1, x->buffer_in);
}

// *********************************************************
// -(process)-----------------------------------------------
void impmap_process(impmap *x)
{
    impmap_switch_modes(x, MO_BYPASS);
    outlet_anything(x->outlet3, gensym("process"), 0, 0);
}

// *********************************************************
// -(save)--------------------------------------------------
void impmap_save(impmap *x)
{
    outlet_anything(x->outlet3, gensym("export"), 0, 0);
}

// *********************************************************
// -(load)--------------------------------------------------
void impmap_load(impmap *x)
{
    impmap_switch_modes(x, MO_BYPASS);
    outlet_anything(x->outlet3, gensym("import"), 0, 0);
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

    int i=0, j=0;

    mapper_signal *psig = mdev_get_outputs(x->device);

    mdev_timetag_now(x->device, &x->tt);
    mdev_start_queue(x->device, x->tt);
    for (i=1; i < mdev_num_outputs(x->device); i++) {
        mapper_db_signal props = msig_properties(psig[i]);
        t_signal_ref *ref = props->user_data;
        float v[props->length];
        for (j = 0; j < props->length; j++) {
            v[j] = atom_getfloat(argv+ref->offset+j);
        }
        msig_update(psig[i], v, 1, x->tt);
    }
    mdev_send_queue(x->device, x->tt);
    outlet_anything(x->outlet2, ps_list, argc, argv);
}

// *********************************************************
// -(input handler)-----------------------------------------
void impmap_input_handler(mapper_signal sig, mapper_db_signal props,
                          int instance_id, void *value, int count,
                          mapper_timetag_t *time)
{
    t_signal_ref *ref = props->user_data;
    impmap *x = ref->x;

    int j;
    for (j=0; j < props->length; j++) {
        if (ref->offset+j >= MAX_LIST) {
            post("implicitmap: Maximum vector length exceeded!");
            break;
        }
        if (!value) {
            maxpd_atom_set_float(x->buffer_in+ref->offset+j, 0);
        }
        else if (props->type == 'f') {
            float *f = value;
            maxpd_atom_set_float(x->buffer_in+ref->offset+j, f[j]);
        }
        else if (props->type == 'i') {
            int *i = value;
            maxpd_atom_set_float(x->buffer_in+ref->offset+j, (float)i[j]);
        }
    }
    x->new_in = 1;
}

// *********************************************************
// -(output handler)-----------------------------------------
void impmap_output_handler(mapper_signal sig, mapper_db_signal props,
                           int instance_id, void *value, int count,
                           mapper_timetag_t *time)
{
    t_signal_ref *ref = props->user_data;
    impmap *x = ref->x;
    
    int j;
    for (j=0; j < props->length; j++) {
        if (ref->offset+j >= MAX_LIST) {
            post("implicitmap: Maximum vector length exceeded!");
            break;
        }
        if (!value) {
            maxpd_atom_set_float(x->buffer_out+ref->offset+j, 0);
        }
        else if (props->type == 'f') {
            float *f = value;
            maxpd_atom_set_float(x->buffer_out+ref->offset+j, f[j]);
        }
        else if (props->type == 'i') {
            int *i = value;
            maxpd_atom_set_float(x->buffer_out+ref->offset+j, (float)i[j]);
        }
    }
    x->new_out = 1;
}

// *********************************************************
// -(link handler)------------------------------------------
void impmap_link_handler(mapper_db_link lnk, mapper_db_action_t a, void *user_data)
{
    // do not allow self-links
    impmap *x = user_data;
    if (!x) {
        post("error in connect handler: user_data is NULL");
        return;
    }
    if (!x->ready) {
        post("error in connect handler: device not ready");
        return;
    }
    if (a == MDB_NEW) {
        if (strcmp(lnk->src_name, mdev_name(x->device)) == 0 &&
            strcmp(lnk->dest_name, mdev_name(x->device)) == 0) {
            mapper_monitor_unlink(x->monitor, lnk->src_name, lnk->dest_name);
        }
    }
}

// *********************************************************
// -(connection handler)------------------------------------
void impmap_connect_handler(mapper_db_connection con, mapper_db_action_t a, void *user)
{
    // if connected involves current generic signal, create a new generic signal
    impmap *x = user;
    if (!x) {
        post("error in connect handler: user_data is NULL");
        return;
    }
    if (!x->ready) {
        post("error in connect handler: device not ready");
        return;
    }
    const char *signal_name = 0;
    switch (a) {
        case MDB_NEW: {
            // check if applies to me
            if (!osc_prefix_cmp(con->src_name, mdev_name(x->device), &signal_name)) {
                if (!signal_name || strcmp(signal_name, con->dest_name) == 0)
                    return;
                if (mdev_num_outputs(x->device) >= MAX_LIST) {
                    post("Max outputs reached!");
                    return;
                }
                // disconnect the generic signal
                mapper_monitor_disconnect(x->monitor, con->src_name, con->dest_name);

                // add a matching output signal
                mapper_signal msig;
                char str[256];
                int length = con->dest_length ? : 1;
                msig = mdev_add_output(x->device, con->dest_name, length, 'f', 0,
                                       (con->range.known | CONNECTION_RANGE_DEST_MIN) ?
                                       &con->range.dest_min : 0,
                                       (con->range.known | CONNECTION_RANGE_DEST_MAX) ?
                                       &con->range.dest_max : 0);
                if (!msig) {
                    post("msig doesn't exist!");
                    return;
                }
                msig_set_callback(msig, impmap_output_handler, 0);
                // connect the new signal
                msig_full_name(msig, str, 256);
                mapper_db_connection_t props;
                props.mode = MO_REVERSE;
                mapper_monitor_connect(x->monitor, str, con->dest_name,
                                       &props, CONNECTION_MODE);

                impmap_update_output_vector_positions(x);

                //output numOutputs
                maxpd_atom_set_int(&x->msg_buffer, mdev_num_outputs(x->device) - 1);
                outlet_anything(x->outlet3, gensym("numOutputs"), 1, &x->msg_buffer);
            }
            else if (!osc_prefix_cmp(con->dest_name, mdev_name(x->device),
                                     &signal_name)) {
                if (!signal_name || strcmp(signal_name, con->src_name) == 0)
                    return;
                if (mdev_num_inputs(x->device) >= MAX_LIST) {
                    post("Max inputs reached!");
                    return;
                }
                // disconnect the generic signal
                mapper_monitor_disconnect(x->monitor, con->src_name, con->dest_name);

                // create a matching input signal
                mapper_signal msig;
                char str[256];
                int length = con->src_length ? : 1;
                msig = mdev_add_input(x->device, con->src_name, length, 'f', 0,
                                      (con->range.known | CONNECTION_RANGE_SRC_MIN) ?
                                      &con->range.src_min : 0,
                                      (con->range.known | CONNECTION_RANGE_SRC_MAX) ?
                                      &con->range.src_max : 0,
                                      impmap_input_handler, 0);
                if (!msig)
                    return;
                // connect the new signal
                mapper_db_connection_t props;
                props.mode = MO_BYPASS;
                msig_full_name(msig, str, 256);
                mapper_monitor_connect(x->monitor, con->src_name, str,
                                       &props, CONNECTION_MODE);

                impmap_update_input_vector_positions(x);

                //output numInputs
                maxpd_atom_set_int(&x->msg_buffer, mdev_num_inputs(x->device) - 1);
                outlet_anything(x->outlet3, gensym("numInputs"), 1, &x->msg_buffer);
            }
            break;
        }
        case MDB_MODIFY:
            break;
        case MDB_REMOVE: {
            mapper_signal msig;
            // check if applies to me
            if (!(osc_prefix_cmp(con->dest_name, mdev_name(x->device),
                                 &signal_name))) {
                if (!signal_name || strcmp(signal_name, "/CONNECT_HERE") == 0)
                    return;
                if (strcmp(signal_name, con->src_name) != 0)
                    return;
                // find corresponding signal
                if (!(msig = mdev_get_input_by_name(x->device, signal_name, 0))) {
                    post("error: input signal %s not found!", signal_name);
                    return;
                }
                // remove it
                mdev_remove_input(x->device, msig);
                impmap_update_input_vector_positions(x);

                //output numInputs
                maxpd_atom_set_int(&x->msg_buffer, mdev_num_inputs(x->device) - 1);
                outlet_anything(x->outlet3, gensym("numInputs"), 1, &x->msg_buffer);
            }
            else if (!(osc_prefix_cmp(con->src_name, mdev_name(x->device),
                                      &signal_name))) {
                if (!signal_name || strcmp(signal_name, "/CONNECT_HERE") == 0)
                    return;
                if (strcmp(signal_name, con->dest_name) != 0)
                    return;
                // find corresponding signal
                if (!(msig = mdev_get_output_by_name(x->device, signal_name, 0))) {
                    post("error: output signal %s not found", signal_name);
                    return;
                }                
                // remove it
                mdev_remove_output(x->device, msig);
                impmap_update_output_vector_positions(x);

                //output numOutputs
                maxpd_atom_set_int(&x->msg_buffer, mdev_num_outputs(x->device) - 1);
                outlet_anything(x->outlet3, gensym("numOutputs"), 1, &x->msg_buffer);
            }
            break;
        }
    }
}

// *********************************************************
// -(compare signal names for qsort)------------------------
int compare_signal_names(const void *l, const void *r)
{
    mapper_db_signal l_props = msig_properties(*(mapper_signal*)l);
    mapper_db_signal r_props = msig_properties(*(mapper_signal*)r);
    return strcmp(l_props->name, r_props->name);
}

// *********************************************************
// -(set up new device and monitor)-------------------------
void impmap_update_input_vector_positions(impmap *x)
{
    int i, k=0, count;

    // store input signal pointers
    mapper_signal signals[mdev_num_inputs(x->device) - 1];
    mapper_signal *psig = mdev_get_inputs(x->device);
    // start counting at index 1 to ignore signal "/CONNECT_HERE"
    for (i = 1; i < mdev_num_inputs(x->device); i++) {
        signals[i-1] = psig[i];
    }

    // sort input signal pointer array
    qsort(signals, mdev_num_inputs(x->device) - 1,
          sizeof(mapper_signal), compare_signal_names);

    // set offsets and user_data
    for (i = 0; i < mdev_num_inputs(x->device) - 1; i++) {
        mapper_db_signal props = msig_properties(signals[i]);
        x->signals_in[i].offset = k;
        props->user_data = &x->signals_in[i];
        k += props->length;
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

    // store output signal pointers
    mapper_signal signals[mdev_num_outputs(x->device) - 1];
    mapper_signal *psig = mdev_get_outputs(x->device);
    // start counting at index 1 to ignore signal "/CONNECT_HERE"
    for (i = 1; i < mdev_num_outputs(x->device); i++) {
        signals[i-1] = psig[i];
    }

    // sort output signal pointer array
    qsort(signals, mdev_num_outputs(x->device) - 1,
          sizeof(mapper_signal), compare_signal_names);

    // set offsets and user_data
    for (i = 0; i < mdev_num_outputs(x->device) - 1; i++) {
        mapper_db_signal props = msig_properties(signals[i]);
        x->signals_out[i].offset = k;
        props->user_data = &x->signals_out[i];
        k += props->length;
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
    x->admin = 0;
    x->device = 0;
    x->monitor = 0;
    x->db = 0;

    x->admin = mapper_admin_new(iface, 0, 0);
    if (!x->admin)
        return 1;

    x->device = mdev_new(x->name, port, x->admin);
    if (!x->device)
        return 1;

    x->monitor = mapper_monitor_new(x->admin, 0);
    if (!x->monitor)
        return 1;

    x->db = mapper_monitor_get_db(x->monitor);
    mapper_db_add_link_callback(x->db, impmap_link_handler, x);
    mapper_db_add_connection_callback(x->db, impmap_connect_handler, x);

    impmap_print_properties(x);

    return 0;
}

// *********************************************************
// -(poll libmapper)----------------------------------------
void impmap_poll(impmap *x)
{
    mdev_poll(x->device, 0);
    mapper_monitor_poll(x->monitor, 0);
    if (!x->ready) {
        if (mdev_ready(x->device)) {
            x->ready = 1;

            // create a new generic output signal
            mdev_add_output(x->device, "/CONNECT_HERE", 1, 'f', 0, 0, 0);

            // create a new generic input signal
            mdev_add_input(x->device, "/CONNECT_HERE", 1, 'f', 0, 0, 0, 0, x);

            impmap_print_properties(x);
        }
    }
    if (x->new_in) {
        outlet_anything(x->outlet1, ps_list, x->size_in, x->buffer_in);
        x->new_in = 0;
    }
    if (x->new_out) {
        outlet_anything(x->outlet2, ps_list, x->size_out, x->buffer_out);
        x->new_out = 0;
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
    outlet_anything(x->outlet3, gensym("clear"), 0, 0);
    maxpd_atom_set_int(x->buffer_in, 0);
    outlet_anything(x->outlet3, gensym("numSnapshots"), 1, x->buffer_in);

    impmap_switch_modes(x, MO_REVERSE);
}

// *********************************************************
// -(switch outgoing connection modes)----------------------
void impmap_switch_modes(impmap *x, mapper_mode_type mode)
{
    mapper_db_connection_t **con =
        mapper_db_get_connections_by_device_name(x->db, mdev_name(x->device));

    while (con) {
        (**con).mode = mode;
        mapper_monitor_connection_modify(x->monitor, *con, CONNECTION_MODE);
        con = mapper_db_connection_next(con);
    }
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

/* Helper function to check if the OSC prefix matches.  Like strcmp(),
 * returns 0 if they match (up to the second '/'), non-0 otherwise.
 * Also optionally returns a pointer to the remainder of str1 after
 * the prefix. */
static int osc_prefix_cmp(const char *str1, const char *str2,
                          const char **rest)
{
    if (str1[0]!='/') {
        return 0;
    }
    if (str2[0]!='/') {
        return 0;
    }

    // skip first slash
    const char *s1=str1+1, *s2=str2+1;

    while (*s1 && (*s1)!='/') s1++;
    while (*s2 && (*s2)!='/') s2++;

    int n1 = s1-str1, n2 = s2-str2;
    if (n1!=n2) return 1;

    if (rest)
        *rest = s1;

    return strncmp(str1, str2, n1);
}
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

// notes:
// declare one generic input, one generic output
// when receiving mapping command, should create new signal for mapping, with
// matching name and properties
// user input: snapshots, train, some training variables
// will have to listen for signal requests and declare an extra one (monitor functionality)
// 
// listen for signal request
//      if received, declare real signals and then one more generic
// listen for mapping request
//      if for generic signal OR existing signal
//          create new signal to match other end, map
//      if for output mapping, also create matching inputs
// listen for mapping destruction, destroy signal if necessary
// snapshot command


#ifndef PD
#define MAXMSP
#endif

// *********************************************************
// -(Includes)----------------------------------------------

#ifdef MAXMSP
    #include "ext.h"            // standard Max include, always required
    #include "ext_obex.h"       // required for new style Max object
    #include "ext_dictionary.h"
    #include "jpatcher_api.h"
#else
    #include "m_pd.h"
#endif
#include <mapper/mapper.h>
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

typedef struct _mapper 
{
    t_object ob;
    void *outlet1;
    void *outlet2;
    void *outlet3;
    void *clock;          // pointer to clock object
    void *timeout;
    char name[128];
    mapper_device device;
    mapper_monitor monitor;
    lo_address address;
    mapper_db db;
    int ready;
    int new_in;
    t_atom buffer_in[MAX_LIST];
    int size_in;
    t_atom buffer_out[MAX_LIST];
    int size_out;
    int query_count;
    t_atom buffer[MAX_LIST];
    t_signal_ref signals_in[MAX_LIST];
    t_signal_ref signals_out[MAX_LIST];
} t_mapper;

t_symbol *ps_list;
int port = 9000;

// *********************************************************
// -(function prototypes)-----------------------------------
void *mapper_new(t_symbol *s, int argc, t_atom *argv);
void mapper_free(t_mapper *x);
void mapper_list(t_mapper *x, t_symbol *s, int argc, t_atom *argv);
void mapper_poll(t_mapper *x);
void mapper_snapshot_timeout(t_mapper *x);
void mapper_randomize(t_mapper *x);
void mapper_input_handler(mapper_signal msig, mapper_db_signal props, mapper_timetag_t *time, void *value);
void mapper_query_handler(mapper_signal msig, mapper_db_signal props, mapper_timetag_t *time, void *value);
void mapper_connect_handler(mapper_db_mapping map, mapper_db_action_t a, void *user);
void mapper_print_properties(t_mapper *x);
int mapper_setup_mapper(t_mapper *x);
void mapper_snapshot(t_mapper *x);   
#ifdef MAXMSP
    void mapper_assist(t_mapper *x, void *b, long m, long a, char *s);
#endif
void add_input(t_mapper *x);
void add_output(t_mapper *x);
int mapper_pack_signal_value(mapper_signal sig, t_atom *buffer, int max_length);
int mapper_find_ordinal(const char *str);
void mapper_update_input_vector_positions(t_mapper *x);
void mapper_update_output_vector_positions(t_mapper *x);
void set_sym(t_atom *argv, char *sym);
void set_int(t_atom *argv, int i);
static int osc_prefix_cmp(const char *str1, const char *str2, const char **rest);

// *********************************************************
// -(global class pointer variable)-------------------------
void *mapper_class;

// *********************************************************
// -(main)--------------------------------------------------
#ifdef MAXMSP
int main(void)
    {    
        t_class *c;
        c = class_new("implicitmap", (method)mapper_new, (method)mapper_free, 
                      (long)sizeof(t_mapper), 0L, A_GIMME, 0);
        class_addmethod(c, (method)mapper_assist,           "assist",   A_CANT,     0);
        class_addmethod(c, (method)mapper_snapshot,         "snapshot", A_GIMME,    0);
        class_addmethod(c, (method)mapper_randomize,        "randomize",A_GIMME,    0);
        class_addmethod(c, (method)mapper_list,             "list",     A_GIMME,    0);
        class_addmethod(c, (method)mapper_print_properties, "print",    A_GIMME,    0);
        class_register(CLASS_BOX, c); /* CLASS_NOBOX */
        mapper_class = c;
        ps_list = gensym("list");
        return 0;
    }
#else
    int mapper_setup(void)
    {
        t_class *c;
        c = class_new(gensym("mapper"), (t_newmethod)mapper_new, (t_method)mapper_free, 
                      (long)sizeof(t_mapper), 0L, A_GIMME, 0);        
        class_addmethod(c,   (t_method)mapper_snapshot,         gensym("snapshot"),  A_GIMME, 0);
        class_addmethod(c,   (t_method)mapper_randomize,        gensym("randomize"), A_GIMME, 0);
        class_addmethod(c,   (t_method)mapper_list,             gensym("list"),      A_GIMME, 0);
        class_addmethod(c,   (t_method)mapper_print_properties, gensym("print"),     A_GIMME, 0);
        mapper_class = c;
        ps_list = gensym("list");
        return 0;
    }
#endif

// *********************************************************
// -(new)---------------------------------------------------
void *mapper_new(t_symbol *s, int argc, t_atom *argv)
{
    t_mapper *x = NULL;
    long i;
    char alias[128];
    
    strncpy(alias, "implicitmap", 128);
    
#ifdef MAXMSP
    if (x = object_alloc(mapper_class)) {
        x->outlet3 = listout((t_object *)x);
        x->outlet2 = listout((t_object *)x);
        x->outlet1 = listout((t_object *)x);
        
        for (i = 0; i < argc; i++) {
            if ((argv + i)->a_type == A_SYM) {
                if(strcmp(atom_getsym(argv+i)->s_name, "@alias") == 0) {
                    if ((argv+i+1)->a_type == A_SYM) {
                        strncpy(alias, atom_getsym(argv+i+1)->s_name, 128);
                    }
                }
            }
        }
#else
    if (x = (t_mapper *) pd_new(mapper_class) ) {
        x->outlet1 = outlet_new(&x->ob, gensym("list"));
        x->outlet2 = outlet_new(&x->ob, gensym("list"));
        x->outlet3 = outlet_new(&x->ob, gensym("list"));
        
        for (i = 0; i < argc; i++) {
            if ((argv+i)->a_type == A_SYMBOL) {
                if(strcmp((argv+i)->a_w.w_symbol->s_name, "@alias") == 0) {
                    if ((argv+i+1)->a_type == A_SYMBOL) {
                        strncpy(alias, (argv+i+1)->a_w.w_symbol->s_name, 128);
                    }
                }
            }
        }
#endif
        if (alias[0] == '/') {
            (*alias)++;
        }
        strncpy(x->name, alias, 128);
        
        if (mapper_setup_mapper(x)) {
            post("Error initializing.");
        }
        else {
            x->ready = 0;
            x->new_in = 0;
            x->query_count = 0;
            // initialize input and output buffers
            for (i = 0; i < MAX_LIST; i++) {
#ifdef MAXMSP
                atom_setfloat(x->buffer_in+i, 0);
                atom_setfloat(x->buffer_out+i, 0);
#else
                SETFLOAT(x->buffer_in+i, 0);
                SETFLOAT(x->buffer_out+i, 0);
#endif
                x->signals_in[i].x = x;
                x->signals_out[i].x = x;
            }
            
            x->size_in = 0;
            x->size_out = 0;
                        
#ifdef MAXMSP
            x->clock = clock_new(x, (method)mapper_poll);    // Create the timing clock
            x->timeout = clock_new(x, (method)mapper_snapshot_timeout);
#else
            x->clock = clock_new(x, (t_method)mapper_poll);
            x->timeout = clock_new(x, (t_method)mapper_snapshot_timeout);
#endif
            clock_delay(x->clock, INTERVAL);  // Set clock to go off after delay
        }
    }
    return (x);
}

// *********************************************************
// -(free)--------------------------------------------------
void mapper_free(t_mapper *x)
{
    clock_unset(x->clock);    // Remove clock routine from the scheduler
    clock_free(x->clock);     // Frees memeory used by clock
    
    if (x->device) {
        post("Freeing device %s...", mdev_name(x->device));
        mdev_free(x->device);
        post("ok");
    }
    if (x->db) {
        mapper_db_remove_mapping_callback(x->db, mapper_connect_handler, x);
    }
    if (x->monitor) {
        post("Freeing monitor...");
        mapper_monitor_free(x->monitor);
        post("ok");
    }
    if (x->address) {
        lo_address_free(x->address);
    }
}

// *********************************************************
// -(print properties)--------------------------------------
void mapper_print_properties(t_mapper *x)
{    
    if (x->ready) {        
        //output name
        set_sym(x->buffer, (char *)mdev_name(x->device));
        outlet_anything(x->outlet3, gensym("name"), 1, x->buffer);
        
        //output interface
        set_sym(x->buffer, (char *)mdev_interface(x->device));
        outlet_anything(x->outlet3, gensym("interface"), 1, x->buffer);
        
        //output IP
        const struct in_addr *ip = mdev_ip4(x->device);
        if (ip) {
            set_sym(x->buffer, inet_ntoa(*ip));
            outlet_anything(x->outlet2, gensym("IP"), 1, x->buffer);
        }
        
        //output port
        set_int(x->buffer, mdev_port(x->device));
        outlet_anything(x->outlet3, gensym("port"), 1, x->buffer);
        
        //output numInputs
        set_int(x->buffer, mdev_num_inputs(x->device));
        outlet_anything(x->outlet3, gensym("numInputs"), 1, x->buffer);
        
        //output numOutputs
        set_int(x->buffer, mdev_num_outputs(x->device));
        outlet_anything(x->outlet3, gensym("numOutputs"), 1, x->buffer);
    }
}

// *********************************************************
// -(inlet/outlet assist - maxmsp only)---------------------
#ifdef MAXMSP
void mapper_assist(t_mapper *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET) { // inlet
        sprintf(s, "OSC input");
    } 
    else {    // outlet
        if (a == 0) {
            sprintf(s, "Mapped OSC inputs");
        }
        else if (a == 1) {
            sprintf(s, "Snapshot data for outputs");
        }
        else {
            sprintf(s, "Device information");
        }
    }
}
#endif

// *********************************************************
// -(snapshot)----------------------------------------------
void mapper_snapshot(t_mapper *x)
{
    // if previous snapshot still in progress, output current snapshot status
    if (x->query_count) {
        post("still waiting for last snapshot");
        return;
    }
    
    int i, j;
    mapper_signal *psig;
    x->query_count = 0;
    
    // iterate through input signals: hidden inputs correspond to 
    // outputs/remote inputs - we need to query the remote end
    psig = mdev_get_inputs(x->device);
    for (i = 1; i < (mdev_num_inputs(x->device) + mdev_num_hidden_inputs(x->device)); i++) {
        mapper_db_signal props = msig_properties(psig[i]);
        if (props->hidden) {
            // find associated output
            mapper_signal output  = (mapper_signal) props->user_data;
            // query the remote value
            x->query_count += msig_query_remote(output, psig[i]);
        }
        else {
            mapper_signal_value_t *value = msig_value(psig[i], 0);
            t_signal_ref *ref = props->user_data;
            for (j=0; j < props->length; j++) {
                if (!value) {
#ifdef MAXMSP
                    atom_setfloat(x->buffer_in+ref->offset+j, 0);
#else
                    SETFLOAT(x->buffer_in+ref->offset+j, 0);
#endif
                }
                else if (props->type == 'f') {
#ifdef MAXMSP
                    atom_setfloat(x->buffer_in+ref->offset+j, (value+j)->f);
#else
                    SETFLOAT(x->buffer_in+ref->offset+j, (value+j)->f);
#endif
                }
                else if (props->type == 'i') {
#ifdef MAXMSP
                    atom_setfloat(x->buffer_in+ref->offset+j, (float)(value+j)->i32);
#else
                    SETFLOAT(x->buffer_in+ref->offset+j, (float)(value+j)->i32);
#endif
                }
            }
        }
    }
    if (x->query_count)
        clock_delay(x->timeout, 1000);  // Set clock to go off after delay
}

// *********************************************************
// -(snapshot)----------------------------------------------
void mapper_snapshot_timeout(t_mapper *x)
{
    if (x->query_count) {
        post("query timeout! setting query count to 0 and outputting current values.");
        // output snapshot as-is
        outlet_anything(x->outlet2, gensym("out"), x->size_out, x->buffer_out);
        x->query_count = 0;
    }
}

// *********************************************************
// -(randomize)---------------------------------------------
void mapper_randomize(t_mapper *x)
{
    int i, j, k = 0;
    float rand_val;
    mapper_db_signal props;
    if (x->ready) {
        mapper_signal *psig = mdev_get_outputs(x->device);
        for (i = 1; i < mdev_num_outputs(x->device); i ++) {
            props = msig_properties(psig[i]);
            if (props->type == 'f') {
                float v[props->length];
                for (j = 0; j < props->length; j++) {
                    rand_val = (float)rand() / (float)RAND_MAX;
                    if (props->minimum && props->maximum) {
                        v[j] = rand_val * (props->maximum->f - props->minimum->f) - props->minimum->f;
                    }
#ifdef MAXMSP
                    atom_setfloat(x->buffer_out+(k++), v[j]);
#else
                    SETFLOAT(x->buffer_out+(k++), v[j]);
#endif
                }
                msig_update(psig[i], v);
            }
            else if (props->type == 'i') {
                int v[props->length];
                for (j = 0; j < props->length; j++) {
                    rand_val = (float)rand() / (float)RAND_MAX;
                    if (props->minimum && props->maximum) {
                        v[j] = (int) (rand_val * (props->maximum->i32 - props->minimum->i32) - props->minimum->i32);
                    }
#ifdef MAXMSP
                    atom_setfloat(x->buffer_in+(k++), v[j]);
#else
                    SETFLOAT(x->buffer_in+(k++), v[j]);
#endif
                }
                msig_update(psig[i], v);
            }
        }
        outlet_anything(x->outlet2, gensym("out"), k, x->buffer_out);
    }
}

// *********************************************************
// -(anything)----------------------------------------------
void mapper_list(t_mapper *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc < x->size_out) {
        post("vector size mismatch");
        return;
    }
    
    int i=0, j=0, k=0;
    
    mapper_signal *psig = mdev_get_outputs(x->device);
    
    for (i=1; i < mdev_num_outputs(x->device); i++) {
        mapper_db_signal props = msig_properties(psig[i]);
        if (props->type == 'f') {
            float v[props->length];
            for (j = 0; j < props->length; j++) {
                v[j] = atom_getfloat(argv + k++);
            }
            msig_update(psig[i], v);
        }
        else if (props->type == 'i') {
            int v[props->length];
            for (j = 0; j < props->length; j++) {
                v[j] = (int)atom_getfloat(argv + k++);
            }
            msig_update(psig[i], v);
        }
    }
}

// *********************************************************
// -(input handler)-----------------------------------------
void mapper_input_handler(mapper_signal sig, mapper_db_signal props, mapper_timetag_t *time, void *value)
{
    t_signal_ref *ref = props->user_data;
    t_mapper *x = ref->x;
    if (!x)
        post("pointer problem! %i", x);
    
    int j;
    for (j=0; j < props->length; j++) {
        if (!value) {
#ifdef MAXMSP
            atom_setfloat(x->buffer_in+ref->offset+j, 0);
#else
            SETFLOAT(x->buffer_in+ref->offset+j, 0);
#endif
        }
        else if (props->type == 'f') {
            float *f = value;
#ifdef MAXMSP
            atom_setfloat(x->buffer_in+ref->offset+j, f[j]);
#else
            SETFLOAT(x->buffer_in+ref->offset+j, f[j]);
#endif
        }
        else if (props->type == 'i') {
            int *i = value;
#ifdef MAXMSP
            atom_setfloat(x->buffer_in + ref->offset + j, (float)i[j]);
#else
            SETFLOAT(x->buffer_in + ref->offset + j, (float)i[j]);
#endif
        }
    }
    x->new_in = 1;
}

// *********************************************************
// -(output the input vector)-------------------------------
void update_vector(t_mapper *x)
{
    outlet_anything(x->outlet1, gensym("in"), x->size_in, x->buffer_in);
    x->new_in = 0;
}
    
// *********************************************************
// -(query handler)-----------------------------------------
void mapper_query_handler(mapper_signal remote_sig, mapper_db_signal remote_props, mapper_timetag_t *time, void *value)
{
    post("query handler: %s", remote_props->name);
    mapper_signal local_sig = (mapper_signal) remote_props->user_data;

    mapper_db_signal local_props = msig_properties(local_sig);
    t_signal_ref *ref = local_props->user_data;
    t_mapper *x = ref->x;
    
    if (!local_props) {
        post("error in query_handler: user_data is NULL");
        return;
    }
    int j;
        
    for (j = 0; j < local_props->length; j++) {
        if (!value) {
#ifdef MAXMSP
            atom_setfloat(x->buffer_out+ref->offset+j, 0);
#else
            SETFLOAT(x->buffer_out+ref->offset+j, 0);
#endif
        }
        else if (local_props->type == 'f') {
            float *f = value;
#ifdef MAXMSP
            atom_setfloat(x->buffer_out+ref->offset+j, f[j]);
#else
            SETFLOAT(x->buffer_out+ref->offset+j, f[j]);
#endif
        }
        else if (local_props->type == 'i') {
            int *i = value;
#ifdef MAXMSP
            atom_setfloat(x->buffer_out+ref->offset+j, (float)i[j]);
#else
            SETFLOAT(x->buffer_out+ref->offset+j, (float)i[j]);
#endif
        }
    }
    
    x->query_count --;

    if (x->query_count == 0) {
        outlet_anything(x->outlet2, gensym("out"), x->size_out, x->buffer_out);
        outlet_anything(x->outlet3, gensym("snapshot"), 0, 0);
        x->query_count = 0;
    }
}
    
// *********************************************************
// -(link handler)------------------------------------------
void mapper_connect_handler(mapper_db_mapping map, mapper_db_action_t a, void *user)
{
    // if connected involves current generic signal, create a new generic signal
    t_mapper *x = user;
    if (!x) {
        post("error in connect handler: user_data is NULL");
        return;
    }
    if (!x->ready) {
        post("error in connect handler: device not ready");
        return;
    }
    switch (a) {
        case MDB_NEW: {
            // check if applies to me
            if (strcmp(map->src_name, x->name) == 0) {
                if (mdev_num_outputs(x->device) >= MAX_LIST) {
                    post("Max outputs reached!");
                    return;
                }
                // add a matching output signal
                mapper_signal msig;
                char str[256];
                msig = mdev_add_output(x->device, map->dest_name, map->dest_length, 'f', 0,
                                       (map->range.known | MAPPING_RANGE_DEST_MIN) ? &map->range.dest_min : 0,
                                       (map->range.known | MAPPING_RANGE_DEST_MAX) ? &map->range.dest_max : 0);
                // connect the new signal
                msig_full_name(msig, str, 256);
                mapper_db_mapping_t props;
                props.mode = MO_BYPASS;
                mapper_monitor_connect(x->monitor, str, map->dest_name, &props, MAPPING_MODE);
                // add a corresponding hidden input signal for querying
                snprintf(str, 256, "%s%s", "/~", map->dest_name);
                mdev_add_hidden_input(x->device, str, map->dest_length,
                                      map->dest_type, 0, 0, 0,
                                      mapper_query_handler, msig);
                // disconnect the generic signal
                mapper_monitor_disconnect(x->monitor, map->src_name, map->dest_name);
                
                mapper_update_output_vector_positions(x);

                //output numOutputs
                set_int(x->buffer, mdev_num_outputs(x->device));
                outlet_anything(x->outlet3, gensym("numOutputs"), 1, x->buffer);
            }
            else if (strcmp(map->dest_name, x->name) == 0) {
                if (mdev_num_inputs(x->device) >= MAX_LIST) {
                    post("Max inputs reached!");
                    return;
                }
                // create a matching input signal
                mapper_signal msig;
                char str[256];
                msig = mdev_add_input(x->device, map->src_name, map->src_length, 'f', 0,
                                      (map->range.known | MAPPING_RANGE_SRC_MIN) ? &map->range.src_min : 0,
                                      (map->range.known | MAPPING_RANGE_SRC_MAX) ? &map->range.src_max : 0,
                                      mapper_input_handler, x);
                // connect the new signal
                mapper_db_mapping_t props;
                props.mode = MO_BYPASS;
                msig_full_name(msig, str, 256);
                mapper_monitor_connect(x->monitor, map->src_name, str, &props, MAPPING_MODE);
                // disconnect the generic signal
                mapper_monitor_disconnect(x->monitor, map->src_name, map->dest_name);
                
                mapper_update_input_vector_positions(x);
                
                //output numInputs
                set_int(x->buffer, mdev_num_inputs(x->device));
                outlet_anything(x->outlet3, gensym("numInputs"), 1, x->buffer);
            }
            break;
        }
        case MDB_MODIFY:
            break;
        case MDB_REMOVE: {
            const char *signal_name;
            mapper_signal msig;
            // check if applies to me
            if (!(osc_prefix_cmp(map->dest_name, mdev_name(x->device), &signal_name))) {
                if (strcmp(signal_name, "/CONNECT_HERE") == 0)
                    return;
                // find corresponding signal
                if (!(msig = mdev_get_input_by_name(x->device, signal_name, 0))) {
                    post("error: input signal %s not found!", signal_name);
                    return;
                }
                // remove it
                mdev_remove_input(x->device, msig);
                mapper_update_input_vector_positions(x);
                
                //output numInputs
                set_int(x->buffer, mdev_num_inputs(x->device));
                outlet_anything(x->outlet3, gensym("numInputs"), 1, x->buffer);
            }
            else if (!(osc_prefix_cmp(map->src_name, mdev_name(x->device), &signal_name))) {
                if (strcmp(signal_name, "/CONNECT_HERE") == 0)
                    return;
                // find corresponding signal
                if (!(msig = mdev_get_output_by_name(x->device, signal_name, 0))) {
                    post("error: output signal %s not found", signal_name);
                    return;
                }
                // remove it
                mdev_remove_output(x->device, msig);
                mapper_update_output_vector_positions(x);
                
                //output numOutputs
                set_int(x->buffer, mdev_num_outputs(x->device));
                outlet_anything(x->outlet3, gensym("numOutputs"), 1, x->buffer);
            }
            break;
        }
    }
}

// *********************************************************
// -(set up new device and monitor)-------------------------
void mapper_update_input_vector_positions(t_mapper *x)
{
    int i, j=1, k=0;
    
    mapper_signal *psig = mdev_get_inputs(x->device);
    
    for (i = 1; i < mdev_num_inputs(x->device) + mdev_num_hidden_inputs(x->device); i++) {
        mapper_db_signal props = msig_properties(psig[i]);
        if (!props->hidden) {
            x->signals_in[j].offset = k;
            props->user_data = &x->signals_in[j];
            j++;
            k += props->length;
        }
    }
    x->size_in = k;
}

// *********************************************************
// -(set up new device and monitor)-------------------------
void mapper_update_output_vector_positions(t_mapper *x)
{
    int i, j=1, k=0;
    
    mapper_signal *psig = mdev_get_outputs(x->device);
    
    for (i = 1; i < mdev_num_outputs(x->device); i++) {
        mapper_db_signal props = msig_properties(psig[i]);
        x->signals_out[j].offset = k;
        props->user_data = &x->signals_out[j];
        j++;
        k += props->length;
    }
    x->size_out = k;
}

// *********************************************************
// -(set up new device and monitor)-------------------------
int mapper_setup_mapper(t_mapper *x)
{
    post("using name: %s", x->name);
    x->device = 0;
    x->monitor = 0;
    x->db = 0;
    
    x->device = mdev_new(x->name, port, 0);
    if (!x->device)
        return 1;
    
    x->monitor = mapper_monitor_new();
    if (!x->monitor)
        return 1;
    
    x->address = lo_address_new_from_url("osc.udp://224.0.1.3:7570");
    lo_address_set_ttl(x->address, 1);
    
    x->db = mapper_monitor_get_db(x->monitor);
    mapper_db_add_mapping_callback(x->db, mapper_connect_handler, x);
    
    mapper_print_properties(x);
    
    return 0;
}

// *********************************************************
// -(poll libmapper)----------------------------------------
void mapper_poll(t_mapper *x)
{    
    mdev_poll(x->device, 0);
    mapper_monitor_poll(x->monitor, 0);
    if (!x->ready) {
        if (mdev_ready(x->device)) {
            x->ready = 1;
            
            snprintf(x->name, 128, "%s%s", mdev_name(x->device), "/CONNECT_HERE");
            
            // create a new generic output signal
            mdev_add_output(x->device, "/CONNECT_HERE", 1, 'f', 0, 0, 0);
            
            // create a new generic input signal
            mdev_add_input(x->device, "/CONNECT_HERE", 1, 'f', 0, 0, 0, 0, x);
            
            mapper_print_properties(x);
        }
    }
    if (x->new_in) {
        update_vector(x);
    }
    clock_delay(x->clock, INTERVAL);  // Set clock to go off after delay
}

// *********************************************************
// -(helper function for set symbol to atom)----------------
void set_sym(t_atom *argv, char *sym)
{
#ifdef MAXMSP
    atom_setsym(argv, gensym(sym));
#else
    SETSYMBOL(argv, gensym(sym));
#endif
}

// *********************************************************
// -(helper function for set int to atom)-------------------
void set_int(t_atom *argv, int i)
{
#ifdef MAXMSP
    atom_setlong(argv, (long)i);
#else
    SETFLOAT(argv, (float)i);
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
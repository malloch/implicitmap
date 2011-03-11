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
typedef struct _mapper 
{
    t_object ob;
    void *outlet1;
    void *outlet2;
    void *clock;          // pointer to clock object
    char name[128];
    mapper_device device;
    mapper_monitor monitor;
    lo_address address;
    mapper_db db;
    mapper_signal inputs[MAX_LIST];
    mapper_signal outputs[MAX_LIST];
    mapper_signal hidden[MAX_LIST];
    int n_in;
    int n_out;
    int count_in;
    int count_out;
    int ready;
    int mute;
    t_atom snapshot_in[MAX_LIST];
    t_atom snapshot_out[MAX_LIST];
    int snapshot_ready;
    t_atom buffer[MAX_LIST];
} t_mapper;

t_symbol *ps_list;
int port = 9000;

// *********************************************************
// -(function prototypes)-----------------------------------
void *mapper_new(t_symbol *s, int argc, t_atom *argv);
void mapper_free(t_mapper *x);
void mapper_anything(t_mapper *x, t_symbol *s, int argc, t_atom *argv);
void mapper_poll(t_mapper *x);
void mapper_mute(t_mapper *x, t_symbol *s, int argc, t_atom *argv);
void mapper_randomize(t_mapper *x);
void mapper_input_handler(mapper_signal msig, int has_value);
void mapper_query_handler(mapper_signal msig, int has_value);
void mapper_link_handler(mapper_db_link lnk, mapper_db_action_t a, void *user);
void mapper_print_properties(t_mapper *x);
int mapper_setup_mapper(t_mapper *x);
void mapper_snapshot(t_mapper *x);   
#ifdef MAXMSP
    void mapper_assist(t_mapper *x, void *b, long m, long a, char *s);
#endif
void add_input(t_mapper *x);
void add_output(t_mapper *x);
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
        class_addmethod(c, (method)mapper_assist,         "assist",   A_CANT,     0);
        class_addmethod(c, (method)mapper_snapshot,       "snapshot", A_GIMME,    0);
        class_addmethod(c, (method)mapper_mute,           "mute",     A_GIMME,    0);
        class_addmethod(c, (method)mapper_randomize,     "randomize",A_GIMME,    0);
        class_addmethod(c, (method)mapper_anything,       "anything", A_GIMME,    0);
        class_addmethod(c, (method)mapper_print_properties, "print",  A_GIMME,    0);
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
        class_addmethod(c,   (t_method)mapper_snapshot, gensym("snapshot"), A_GIMME, 0);
        class_addmethod(c,   (t_method)mapper_mute,     gensym("mute"),     A_GIMME, 0);
        class_addmethod(c,   (t_method)mapper_print_properties, gensym("print"), A_GIMME, 0);
        class_addanything(c, (t_method)mapper_anything);
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
        x->outlet2 = listout((t_object *)x);
        x->outlet1 = listout((t_object *)x);
        
        for (i = 0; i < argc; i++) {
            if ((argv + i)->a_type == A_SYM) {
                if(strcmp(atom_getsym(argv+i)->s_name, "@alias") == 0) {
                    if ((argv + i + 1)->a_type == A_SYM) {
                        strncpy(alias, atom_getsym(argv+i+1)->s_name, 128);
                    }
                }
            }
        }
#else
    if (x = (t_mapper *) pd_new(mapper_class) ) {
        x->outlet1 = outlet_new(&x->ob, gensym("list"));
        x->outlet2 = outlet_new(&x->ob, gensym("list"));
        
        for (i = 0; i < argc; i++) {
            if ((argv + i)->a_type == A_SYMBOL) {
                if(strcmp((argv+i)->a_w.w_symbol->s_name, "@alias") == 0) {
                    if ((argv + i + 1)->a_type == A_SYMBOL) {
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
            x->mute = 0;
            x->snapshot_ready = 1;
            x->n_in = 0;
            x->n_out = 0;
#ifdef MAXMSP
            x->clock = clock_new(x, (method)mapper_poll);    // Create the timing clock
            atom_setsym(x->snapshot_in, gensym("in"));
            atom_setsym(x->snapshot_out, gensym("out"));
#else
            x->clock = clock_new(x, (t_method)mapper_poll);
            SETSYMBOL(x->snapshot_in, gensym("in"));
            SETSYMBOL(x->snapshot_out, gensym("out"));
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
    clock_free(x->clock);        // Frees memeory used by clock
    
    if (x->device) {
        post("Freeing device %s...", mdev_name(x->device));
        mdev_free(x->device);
        post("ok");
    }
    if (x->db) {
        mapper_db_remove_link_callback(x->db, mapper_link_handler, x);
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
    char *message;
    
    if (x->ready) {        
        //output name
        message = strdup(mdev_name(x->device));
#ifdef MAXMSP
        atom_setsym(x->buffer, gensym("name"));
        atom_setsym(x->buffer + 1, gensym(message));
        outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
        SETSYMBOL(x->buffer, gensym(message));
        outlet_anything(x->outlet2, gensym("name"), 1, x->buffer);
#endif
        
        //output interface
        message = strdup(mdev_interface(x->device));
#ifdef MAXMSP
        atom_setsym(x->buffer, gensym("interface"));
        atom_setsym(x->buffer + 1, gensym(message));
        outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
        SETSYMBOL(x->buffer, gensym(message));
        outlet_anything(x->outlet2, gensym("name"), 1, x->buffer);
#endif
        
        //output IP
        const struct in_addr *ip = mdev_ip4(x->device);
        message = strdup(inet_ntoa(*ip));
#ifdef MAXMSP
        atom_setsym(x->buffer, gensym("IP"));
        atom_setsym(x->buffer + 1, gensym(message));
        outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
        SETSYMBOL(x->buffer, gensym(message));
        outlet_anything(x->outlet2, gensym("IP"), 1, x->buffer);
#endif
        
        //output port
#ifdef MAXMSP
        atom_setsym(x->buffer, gensym("port"));
        atom_setlong(x->buffer + 1, mdev_port(x->device));
        outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
        SETFLOAT(x->buffer, (float)mdev_port(x->device));
        outlet_anything(x->outlet2, gensym("port"), 1, x->buffer);
#endif
        
        //output numInputs
#ifdef MAXMSP
        atom_setsym(x->buffer, gensym("numInputs"));
        atom_setlong(x->buffer + 1, mdev_num_inputs(x->device));
        outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
        SETFLOAT(x->buffer, (float)mdev_n_inputs(x->device));
        outlet_anything(x->outlet2, gensym("numInputs"), 1, x->buffer);
#endif
        
        //output numOutputs
#ifdef MAXMSP
        atom_setsym(x->buffer, gensym("numOutputs"));
        atom_setlong(x->buffer + 1, mdev_num_outputs(x->device));
        outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
        SETFLOAT(x->buffer, (float)mdev_num_outputs(x->device));
        outlet_anything(x->outlet2, gensym("numOutputs"), 1, x->buffer);
#endif
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
            sprintf(s, "Mapped OSC data");
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
void mapper_snapshot(t_mapper *x)
{
    int i;
    // if previous snapshot still in progress, output current snapshot status
    if (!x->snapshot_ready) {
        post("still waiting for last snapshot");
        return;
    }
    if (!x->n_in || !x->n_out) {
        post("no signals mapped!");
        return;
    }
    
    x->count_in = 0;
    x->count_out = 0;
    x->snapshot_ready = 0;
    
    // for each input, store the value. Assume scalars for now
    for (i = 0; i < x->n_in; i++) {
        mapper_signal_value_t *value = msig_value(x->inputs[i]);
        // check if signal has a value
        if (value) {
            mapper_db_signal props = msig_properties(x->inputs[i]);
#ifdef MAXMSP
            if (props->type == 'i')
                atom_setlong(x->snapshot_in + i + 1, (long)value->i32);
            else if (props->type == 'f')
                atom_setfloat(x->snapshot_in + i + 1, value->f);
#else
            if (props->type == 'i')
                SETFLOAT(x->snapshot_in + i + 1, (float)value->i32);
            else if (props->type == 'f')
                SETFLOAT(x->snapshot_in + i + 1, value->f);
#endif
        }
        else {
            // set to zero for now
#ifdef MAXMSP
            atom_setfloat(x->snapshot_in + i + 1, 0);
#else
            SETFLOAT(x->snapshot_in + i + 1, 0);
#endif
        }
    }
    
    // for each output, query the remote values
    for (i = 0; i < x->n_out; i++) {
        msig_query_remote(x->outputs[i], x->hidden[i]);
    }
}
    
// *********************************************************
// -(mute)--------------------------------------------------
void mapper_mute(t_mapper *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc) {
        if (argv->a_type ==  A_LONG) {
            x->mute = atom_getlong(argv);
            post("mute = %i", x->mute);
        }
    }
}

// *********************************************************
// -(randomize)---------------------------------------------
void mapper_randomize(t_mapper *x)
{
    int i;
    if (x->ready && x->n_out) {
        for (i = 0; i < x->n_out - 1; i ++) {
            msig_update_float(x->outputs[i], (float)rand() / (float)RAND_MAX);
        }
    }
}

// *********************************************************
// -(anything)----------------------------------------------
void mapper_anything(t_mapper *x, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    
    if (!x->mute && argc) {
        //find signal
        mapper_signal msig;
        if (!(msig=mdev_get_output_by_name(x->device, s->s_name, 0)))
            return;
        mapper_db_signal props = msig_properties(msig);
        if (props->length != argc) {
            post("Vector length does not match signal definition!");
            return;
        }
        if (props->type == 'i') {
            int payload[props->length];
            for (i = 0; i < argc; i++) {
                if ((argv + i)->a_type == A_FLOAT)
                    payload[i] = (int)atom_getfloat(argv + i);
#ifdef MAXMSP
                else if ((argv + i)->a_type == A_LONG)
                    payload[i] = (int)atom_getlong(argv + i);
#endif
                
            }
            //update signal
            msig_update(msig, payload);
        }
        else if (props->type == 'f') {
            float payload[props->length];
            for (i = 0; i < argc; i++) {
                if ((argv + i)->a_type == A_FLOAT)
                    payload[i] = atom_getfloat(argv + i);
#ifdef MAXMSP
                else if ((argv + i)->a_type == A_LONG)
                    payload[i] = (float)atom_getlong(argv + i);
#endif
                
            }
            //update signal
            msig_update(msig, payload);
        }
        else {
            return;
        }
    }
}

// *********************************************************
// -(int handler)-------------------------------------------
void mapper_input_handler(mapper_signal msig, int has_value)
{
    if (has_value) {
        mapper_db_signal props = msig_properties(msig);
        t_mapper *x = props->user_data;
        int i, length = props->length;
        mapper_signal_value_t *value = msig_value(msig);
        
        if (length > (MAX_LIST-1)) {
            post("Maximum list length is %i!", MAX_LIST-1);
            length = MAX_LIST-1;
        }
        
        if (props->type == 'f') {
            for (i = 0; i < length; i++) {
#ifdef MAXMSP
                atom_setfloat(x->buffer + i + 1, (value+i)->f);
#else
                SETFLOAT(x->buffer + i, (value+i)->f);
#endif
            }
        }
        else if (props->type == 'i') {
            for (i = 0; i < length; i++) {
#ifdef MAXMSP
                atom_setlong(x->buffer + i + 1, (long)(value+i)->i32);
#else
                SETFLOAT(x->buffer + i, (float)(value+i)->i32);
#endif
            }
        }

#ifdef MAXMSP
        atom_setsym(x->buffer, gensym((char *)props->name));
        outlet_list(x->outlet1, ps_list, length+1, x->buffer);
#else
        outlet_anything(x->outlet1, gensym((char *)props->name), length, x->buffer);
#endif
    }
}
    
// *********************************************************
// -(query handler)-----------------------------------------
void mapper_query_handler(mapper_signal remote_sig, int has_value)
{
    int i;
    mapper_db_signal temp_props;
    mapper_db_signal remote_props = msig_properties(remote_sig);
    mapper_signal local_sig = remote_props->user_data;

    mapper_db_signal local_props = msig_properties(local_sig);
    t_mapper *x = local_props->user_data;
    if (!local_props) {
        post("error in query_handler: user_data is NULL");
        return;
    }
    // find output signal in array
    for (i = 0; i < x->n_out; i++) {
        temp_props = msig_properties(x->outputs[i]);
        if (strcmp(temp_props->name, local_props->name) == 0)
            break;
    }
    if (i == x->n_out) {
        post("error in query handler: could not find output signal");
        return;
    }
    
    //int length = remote_props->length;
    if (has_value) {
        mapper_signal_value_t *value = msig_value(remote_sig);
#ifdef MAXMSP
        if (remote_props->type == 'i')
            atom_setlong(x->snapshot_out + i + 1, (long)value->i32);
        else if (remote_props->type == 'f')
            atom_setfloat(x->snapshot_out + i + 1, value->f);
#else
        if (remote_props->type == 'i')
            SETFLOAT(x->snapshot_out + i + 1, (float)value->i32);
        else if (remote_props->type == 'f')
            SETFLOAT(x->snapshot_out + i + 1, value->f);
#endif
    }
    else {
        // remote has no value, set to 0 for now
#ifdef MAXMSP
        atom_setfloat(x->snapshot_out + i + 1, 0);
#else
        SETFLOAT(x->snapshot_out + i + 1, 0);
#endif
    }

    x->count_out ++;
    
    if (x->count_out == x->n_out - 1) {
#ifdef MAXMSP
        outlet_list(x->outlet2, ps_list, x->n_in, x->snapshot_in);
        outlet_list(x->outlet2, ps_list, x->n_out, x->snapshot_out);
#else
        outlet_anything(x->outlet2, gensym("in"), x->n_out - 1, x->snapshot_in + 1);
        outlet_anything(x->outlet2, gensym("out"), x->n_out - 1, x->snapshot_out + 1);
#endif
        outlet_anything(x->outlet2, gensym("snapshot"), 0, 0);
        x->snapshot_ready = 1;
    }
}
    
// *********************************************************
// -(link handler)------------------------------------------
void mapper_link_handler(mapper_db_link lnk, mapper_db_action_t a, void *user)
{
    // if link involves me, learn remote namespace and duplicate it
    // if unlink involves me, remove matching namespace
    t_mapper *x = user;
    if (!x) {
        post("error in link handler: user_data is NULL");
        return;
    }
    if (!x->ready) {
        post("error in link handler: device not ready");
        return;
    }
    switch (a) {
        case MDB_NEW:
            // check if applies to me
            if (osc_prefix_cmp(lnk->src_name, mdev_name(x->device), 0) == 0) {
                mapper_db_signal *psig = mapper_db_get_inputs_by_device_name(x->db, 
                                                                             lnk->dest_name);
                char source_name[1024], dest_name[1024];
                while (psig) {
                    // add matching output
                    post("linked to signal %s%s", lnk->dest_name, (*psig)->name);
                    x->outputs[x->n_out] = mdev_add_output(x->device, (*psig)->name, (*psig)->length,
                                           (*psig)->type, (*psig)->unit, 0, 0);
                    msig_set_minimum(x->outputs[x->n_out], (*psig)->minimum);
                    msig_set_maximum(x->outputs[x->n_out], (*psig)->maximum);
                    // add extra properties?
                    
                    // send /connect message
                    msig_full_name(x->outputs[x->n_out], source_name, 1024);
                    strncpy(dest_name, (*psig)->device_name, 1024);
                    strncat(dest_name, (*psig)->name, 1024);
                    lo_send(x->address, "/connect", "ssss", source_name, dest_name, "@mode", "bypass");
                    psig = mapper_db_signal_next(psig);
                    x->n_out ++;
                }
                //output numInputs
#ifdef MAXMSP
                atom_setsym(x->buffer, gensym("numInputs"));
                atom_setlong(x->buffer + 1, mdev_num_inputs(x->device));
                outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
                SETFLOAT(x->buffer, (float)mdev_n_inputs(x->device));
                outlet_anything(x->outlet2, gensym("numInputs"), 1, x->buffer);
#endif
            }
            else if (osc_prefix_cmp(lnk->dest_name, mdev_name(x->device), 0) == 0) {
                mapper_db_signal *psig = mapper_db_get_outputs_by_device_name(x->db, 
                                                                              lnk->src_name);
                char source_name[1024], dest_name[1024];
                while (psig) {
                    // add matching input - send /connect as well?
                    post("linked to signal %s%s", lnk->src_name, (*psig)->name);
                    x->inputs[x->n_in] = mdev_add_input(x->device, (*psig)->name, (*psig)->length,
                                          (*psig)->type, (*psig)->unit, 0, 0, 
                                          mapper_input_handler, x);
                    msig_set_minimum(x->inputs[x->n_in], (*psig)->minimum);
                    msig_set_maximum(x->inputs[x->n_in], (*psig)->maximum);
                    // add extra properties?
                    
                    // send /connect message
                    strncpy(source_name, (*psig)->device_name, 1024);
                    strncat(source_name, (*psig)->name, 1024);
                    msig_full_name(x->inputs[x->n_in], dest_name, 1024);
                    lo_send(x->address, "/connect", "ssss", source_name, dest_name, "@mode", "bypass");
                    psig = mapper_db_signal_next(psig);
                    x->n_in ++;
                }
                //output numInputs
#ifdef MAXMSP
                atom_setsym(x->buffer, gensym("numInputs"));
                atom_setlong(x->buffer + 1, mdev_num_inputs(x->device));
                outlet_list(x->outlet2, ps_list, 2, x->buffer);
#else
                SETFLOAT(x->buffer, (float)mdev_n_inputs(x->device));
                outlet_anything(x->outlet2, gensym("numInputs"), 1, x->buffer);
#endif
            }
            break;
        case MDB_MODIFY:
            break;
        case MDB_REMOVE:
            break;
    }
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
    mapper_db_add_link_callback(x->db, mapper_link_handler, x);
    
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
            mapper_print_properties(x);
        }
    }
    clock_delay(x->clock, INTERVAL);  // Set clock to go off after delay
}

/* Helper function to check if the OSC prefix matches.  Like strcmp(),
 * returns 0 if they match (up to the second '/'), non-0 otherwise.
 * Also optionally returns a pointer to the remainder of str1 after
 * the prefix. */
static int osc_prefix_cmp(const char *str1, const char *str2,
                          const char **rest)
{
    if (str1[0]!='/') {
        //trace("OSC string '%s' does not start with '/'.\n", str1);
        return 0;
    }
    if (str2[0]!='/') {
        //trace("OSC string '%s' does not start with '/'.\n", str2);
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
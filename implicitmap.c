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
    mapper_db db;
    int input_index;
    int output_index;
    int ready;
    int mute;
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
void mapper_float_handler(mapper_signal msig, void *v);
void mapper_int_handler(mapper_signal msig, void *v);
void mapper_connection_handler(mapper_db_mapping map, mapper_db_action_t a, void *user);
void mapper_print_properties(t_mapper *x);
int mapper_setup_mapper(t_mapper *x);
void mapper_snapshot(t_mapper *x, t_symbol *s, int argc, t_atom *argv);   
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
        class_addmethod(c, (method)mapper_anything,       "anything", A_GIMME,    0);
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
            x->input_index = 0;
            x->output_index = 0;
            for (i = 0; i < 5; i++) {
                add_input(x);
                add_output(x);
            }
#ifdef MAXMSP
            x->clock = clock_new(x, (method)mapper_poll);    // Create the timing clock
#else
            x->clock = clock_new(x, (t_method)mapper_poll);
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
    }
    mapper_db_remove_mapping_callback(x->db, mapper_connection_handler, x);
    if (x->monitor) {
        post("Freeing monitor...");
        mapper_monitor_free(x->monitor);
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
        SETFLOAT(x->buffer, (float)mdev_num_inputs(x->device));
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
void mapper_snapshot(t_mapper *x, t_symbol *s, int argc, t_atom *argv)
{
    int snapshot_index;
    if (!argc)
        return;
    if (argv->a_type != A_LONG)
        return;
    
    snapshot_index = atom_getlong(argv);
    post("snapshot %i", snapshot_index);
    
    // if previous snapshot still in progress, output current snapshot status
    if (!x->snapshot_ready) {
        post("still waiting for last snapshot");
        return;
    }
    
    // for each input, store the value. Assume scalars for now
    
    // for each output, query the remote values
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
void mapper_int_handler(mapper_signal msig, void *v)
{
    mapper_db_signal props = msig_properties(msig);
    t_mapper *x = props->user_data;
    int i, length = props->length;
    int *pi = (int*)v;
    
    if (length > (MAX_LIST-1)) {
        post("Maximum list length is %i!", MAX_LIST-1);
        length = MAX_LIST-1;
    }

#ifdef MAXMSP
    atom_setsym(x->buffer, gensym((char *)props->name));
    for (i = 0; i < length; i++) {
        atom_setlong(x->buffer + i + 1, (long)*(pi+i));
    }
    outlet_list(x->outlet1, ps_list, length+1, x->buffer);
#else
    for (i = 0; i < length; i++) {
        SETFLOAT(x->buffer + i, (float)*(pi+i));
    }
    outlet_anything(x->outlet1, gensym((char *)props->name), length, x->buffer);
#endif
}

// *********************************************************
// -(float handler)-----------------------------------------
void mapper_float_handler(mapper_signal msig, void *v)
{
    mapper_db_signal props = msig_properties(msig);
    t_mapper *x = props->user_data;
    int i, length = props->length;
    float *pf = (float*)v;
    
    if (length > (MAX_LIST-1)) {
        post("Maximum list length is %i!", MAX_LIST-1);
        length = MAX_LIST-1;
    }
    
#ifdef MAXMSP
    atom_setsym(x->buffer, gensym((char *)props->name));
    for (i = 0; i < length; i++) {
        atom_setfloat(x->buffer + i + 1, *(pf+i));
    }
    outlet_list(x->outlet1, ps_list, length+1, x->buffer);
#else
    for (i = 0; i < length; i++) {
        SETFLOAT(x->buffer + i, *(pf+i));
    }
    outlet_anything(x->outlet1, gensym((char *)props->name), length, x->buffer);
#endif
}
    
// *********************************************************
// -(query handler)-----------------------------------------
void mapper_query_handler(mapper_signal remote_sig, void *v)
{
    //mapper_db_signal props = msig_properties(msig);
    //int index = (int) (props->user_data);
    //printf("--> source got query response: /in%i %f\n", index, (*(float*)v));
    
    mapper_db_signal props_remote = msig_properties(remote_sig);
    mapper_signal local_sig = props_remote->user_data;
    mapper_db_signal props_local = msig_properties(local_sig);
    t_mapper *x = props_local->user_data;
    int i, length = props_remote->length;
    float *pf = (float*)v;
    
    if (length > (MAX_LIST-1)) {
        post("Maximum list length is %i!", MAX_LIST-1);
        length = MAX_LIST-1;
    }
    
#ifdef MAXMSP
    atom_setsym(x->buffer, gensym((char *)props_local->name));
    for (i = 0; i < length; i++) {
        atom_setfloat(x->buffer + i + 1, *(pf+i));
    }
    outlet_list(x->outlet1, ps_list, length+1, x->buffer);
#else
    for (i = 0; i < length; i++) {
        SETFLOAT(x->buffer + i, *(pf+i));
    }
    outlet_anything(x->outlet1, gensym((char *)props_local->name), length, x->buffer);
#endif
}

// *********************************************************
// -(connection handler)---------------------------------------
void mapper_connection_handler(mapper_db_mapping map, mapper_db_action_t a, void *user)
{
    t_mapper *x = user;
    if (!x) {
        post("error in connection_handler: user_data is NULL");
        return;
    }
    post("Connection %s -> %s ", map->src_name, map->dest_name);
    switch (a) {
        case MDB_NEW:
            // check if applies to me
            if (osc_prefix_cmp(map->src_name, mdev_name(x->device), 0) == 0) {
                post("I am the source! Create a new output signal!");
                //add_output(x);
            }
            else if (osc_prefix_cmp(map->dest_name, mdev_name(x->device), 0) == 0) {
                post("I am the destination! Create a new input signal!");
                //add_input(x);
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
    
    x->device = mdev_new(x->name, port, 0);

    if (!x->device)
        return 1;
    else
        mapper_print_properties(x);
    
    x->monitor = mapper_monitor_new();
    if (!x->monitor) {
        return 1;
    }
    
    x->db = mapper_monitor_get_db(x->monitor);
    
    mapper_db_add_mapping_callback(x->db, mapper_connection_handler, x);
    
    post("initialization ok");
    
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
    
void add_input(t_mapper *x)
{
    int *user = (int *) x->input_index;
    char name[10];
    snprintf(name, 10, "%s%i", "/in", x->input_index);
    mapper_signal temp = mdev_add_input(x->device, name, 1, 'f', 0, 0, 0, mapper_float_handler, x);
    snprintf(name, 10, "%s%i", "/#in", x->input_index);
    mdev_add_hidden_input(x->device, name, 1, 'f', 0, 0, 0, mapper_query_handler, &temp);
    x->input_index++;
}

void add_output(t_mapper *x)
{
    char name[10];
    snprintf(name, 10, "%s%i", "/out", x->output_index++);
    mdev_add_output(x->device, name, 1, 'f', 0, 0, 0); 
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
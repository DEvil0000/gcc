/* Pipeline hazard description translator.
   Copyright (C) 2000, 2001, 2002, 2003 Free Software Foundation, Inc.

   Written by Vladimir Makarov <vmakarov@redhat.com>
   
This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU CC is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* References:
   
   1. Detecting pipeline structural hazards quickly. T. Proebsting,
      C. Fraser. Proceedings of ACM SIGPLAN-SIGACT Symposium on
      Principles of Programming Languages, pages 280--286, 1994.

      This article is a good start point to understand usage of finite
      state automata for pipeline hazard recognizers.  But I'd
      recommend the 2nd article for more deep understanding.

   2. Efficient Instruction Scheduling Using Finite State Automata:
      V. Bala and N. Rubin, Proceedings of MICRO-28.  This is the best
      article about usage of finite state automata for pipeline hazard
      recognizers.

   The current implementation is different from the 2nd article in the
   following:

   1. New operator `|' (alternative) is permitted in functional unit
      reservation which can be treated deterministically and
      non-deterministically.

   2. Possibility of usage of nondeterministic automata too.

   3. Possibility to query functional unit reservations for given
      automaton state.

   4. Several constructions to describe impossible reservations
      (`exclusion_set', `presence_set', `final_presence_set',
      `absence_set', and `final_absence_set').

   5. No reverse automata are generated.  Trace instruction scheduling
      requires this.  It can be easily added in the future if we
      really need this.

   6. Union of automaton states are not generated yet.  It is planned
      to be implemented.  Such feature is needed to make more accurate
      interlock insn scheduling to get state describing functional
      unit reservation in a joint CFG point.  */

/* This file code processes constructions of machine description file
   which describes automaton used for recognition of processor pipeline
   hazards by insn scheduler and can be used for other tasks (such as
   VLIW insn packing.

   The translator functions `gen_cpu_unit', `gen_query_cpu_unit',
   `gen_bypass', `gen_excl_set', `gen_presence_set',
   `gen_final_presence_set', `gen_absence_set',
   `gen_final_absence_set', `gen_automaton', `gen_automata_option',
   `gen_reserv', `gen_insn_reserv' are called from file
   `genattrtab.c'.  They transform RTL constructions describing
   automata in .md file into internal representation convenient for
   further processing.
 
   The translator major function `expand_automata' processes the
   description internal representation into finite state automaton.
   It can be divided on:

     o checking correctness of the automaton pipeline description
       (major function is `check_all_description').

     o generating automaton (automata) from the description (major
       function is `make_automaton').

     o optional transformation of nondeterministic finite state
       automata into deterministic ones if the alternative operator
       `|' is treated nondeterministically in the description (major
       function is NDFA_to_DFA).

     o optional minimization of the finite state automata by merging
       equivalent automaton states (major function is `minimize_DFA').

     o forming tables (some as comb vectors) and attributes
       representing the automata (functions output_..._table).

   Function `write_automata' outputs the created finite state
   automaton as different tables and functions which works with the
   automata to inquire automaton state and to change its state.  These
   function are used by gcc instruction scheduler and may be some
   other gcc code.  */

#include "bconfig.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "obstack.h"
#include "errors.h"

#include <math.h>
#include "hashtab.h"
#include "varray.h"

#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

#include "genattrtab.h"

/* Positions in machine description file.  Now they are not used.  But
   they could be used in the future for better diagnostic messages.  */
typedef int pos_t;

/* The following is element of vector of current (and planned in the
   future) functional unit reservations.  */
typedef unsigned HOST_WIDE_INT set_el_t;

/* Reservations of function units are represented by value of the following
   type.  */
typedef set_el_t *reserv_sets_t;

/* The following structure represents variable length array (vla) of
   pointers and HOST WIDE INTs.  We could be use only varray.  But we
   add new lay because we add elements very frequently and this could
   stress OS allocator when varray is used only.  */
typedef struct {
  size_t length;      /* current size of vla.  */
  varray_type varray; /* container for vla.  */
} vla_ptr_t;

typedef vla_ptr_t vla_hwint_t;

/* The following structure describes a ticker.  */
struct ticker
{
  /* The following member value is time of the ticker creation with
     taking into account time when the ticker is off.  Active time of
     the ticker is current time minus the value.  */
  int modified_creation_time;
  /* The following member value is time (incremented by one) when the
     ticker was off.  Zero value means that now the ticker is on.  */
  int incremented_off_time;
};

/* The ticker is represented by the following type.  */
typedef struct ticker ticker_t;

/* The following type describes elements of output vectors.  */
typedef HOST_WIDE_INT vect_el_t;

/* Forward declaration of structures of internal representation of
   pipeline description based on NDFA.  */

struct unit_decl;
struct bypass_decl;
struct result_decl;
struct automaton_decl;
struct unit_pattern_rel_decl;
struct reserv_decl;
struct insn_reserv_decl;
struct decl;
struct unit_regexp;
struct result_regexp;
struct reserv_regexp;
struct nothing_regexp;
struct sequence_regexp;
struct repeat_regexp;
struct allof_regexp;
struct oneof_regexp;
struct regexp;
struct description;
struct unit_set_el;
struct pattern_set_el;
struct pattern_reserv;
struct state;
struct alt_state;
struct arc;
struct ainsn;
struct automaton;
struct state_ainsn_table;

/* The following typedefs are for brevity.  */
typedef struct unit_decl *unit_decl_t;
typedef struct decl *decl_t;
typedef struct regexp *regexp_t;
typedef struct unit_set_el *unit_set_el_t;
typedef struct pattern_set_el *pattern_set_el_t;
typedef struct pattern_reserv *pattern_reserv_t;
typedef struct alt_state *alt_state_t;
typedef struct state *state_t;
typedef struct arc *arc_t;
typedef struct ainsn *ainsn_t;
typedef struct automaton *automaton_t;
typedef struct automata_list_el *automata_list_el_t;
typedef struct state_ainsn_table *state_ainsn_table_t;


/* Prototypes of functions gen_cpu_unit, gen_query_cpu_unit,
   gen_bypass, gen_excl_set, gen_presence_set, gen_final_presence_set,
   gen_absence_set, gen_final_absence_set, gen_automaton,
   gen_automata_option, gen_reserv, gen_insn_reserv,
   initiate_automaton_gen, expand_automata, write_automata are
   described on the file top because the functions are called from
   function `main'.  */

static void *create_node             PARAMS ((size_t));
static void *copy_node               PARAMS ((const void *, size_t));
static char *check_name              PARAMS ((char *, pos_t));
static char *next_sep_el             PARAMS ((char **, int, int));
static int n_sep_els                 PARAMS ((char *, int, int));
static char **get_str_vect           PARAMS ((char *, int *, int, int));
static void gen_presence_absence_set PARAMS ((rtx, int, int));
static regexp_t gen_regexp_el        PARAMS ((char *));
static regexp_t gen_regexp_repeat    PARAMS ((char *));
static regexp_t gen_regexp_allof     PARAMS ((char *));
static regexp_t gen_regexp_oneof     PARAMS ((char *));
static regexp_t gen_regexp_sequence  PARAMS ((char *));
static regexp_t gen_regexp           PARAMS ((char *));

static unsigned string_hash          PARAMS ((const char *));
static unsigned automaton_decl_hash  PARAMS ((const void *));
static int automaton_decl_eq_p       PARAMS ((const void *,
						   const void *));
static decl_t insert_automaton_decl       PARAMS ((decl_t));
static decl_t find_automaton_decl         PARAMS ((char *));
static void initiate_automaton_decl_table PARAMS ((void));
static void finish_automaton_decl_table   PARAMS ((void));

static hashval_t insn_decl_hash           PARAMS ((const void *));
static int insn_decl_eq_p                 PARAMS ((const void *,
						   const void *));
static decl_t insert_insn_decl            PARAMS ((decl_t));
static decl_t find_insn_decl              PARAMS ((char *));
static void initiate_insn_decl_table      PARAMS ((void));
static void finish_insn_decl_table        PARAMS ((void));

static hashval_t decl_hash                PARAMS ((const void *));
static int decl_eq_p                      PARAMS ((const void *,
						   const void *));
static decl_t insert_decl                 PARAMS ((decl_t));
static decl_t find_decl                   PARAMS ((char *));
static void initiate_decl_table           PARAMS ((void));
static void finish_decl_table             PARAMS ((void));

static unit_set_el_t process_excls       PARAMS ((char **, int, pos_t));
static void add_excls                    PARAMS ((unit_set_el_t, unit_set_el_t,
						  pos_t));
static unit_set_el_t process_presence_absence_names
					 PARAMS ((char **, int, pos_t,
						  int, int));
static pattern_set_el_t process_presence_absence_patterns
					 PARAMS ((char ***, int, pos_t,
						  int, int));
static void add_presence_absence	 PARAMS ((unit_set_el_t,
						  pattern_set_el_t,
						  pos_t, int, int));
static void process_decls                PARAMS ((void));
static struct bypass_decl *find_bypass   PARAMS ((struct bypass_decl *,
						  struct insn_reserv_decl *));
static void check_automaton_usage        PARAMS ((void));
static regexp_t process_regexp           PARAMS ((regexp_t));
static void process_regexp_decls         PARAMS ((void));
static void check_usage                  PARAMS ((void));
static int loop_in_regexp                PARAMS ((regexp_t, decl_t));
static void check_loops_in_regexps       PARAMS ((void));
static void process_regexp_cycles        PARAMS ((regexp_t, int, int,
						  int *, int *));
static void evaluate_max_reserv_cycles   PARAMS ((void));
static void check_all_description        PARAMS ((void));

static ticker_t create_ticker               PARAMS ((void));
static void ticker_off                      PARAMS ((ticker_t *));
static void ticker_on                       PARAMS ((ticker_t *));
static int active_time                      PARAMS ((ticker_t));
static void print_active_time               PARAMS ((FILE *, ticker_t));

static void add_advance_cycle_insn_decl     PARAMS ((void));

static alt_state_t get_free_alt_state PARAMS ((void));
static void free_alt_state              PARAMS ((alt_state_t));
static void free_alt_states             PARAMS ((alt_state_t));
static int alt_state_cmp                PARAMS ((const void *alt_state_ptr_1,
						 const void *alt_state_ptr_2));
static alt_state_t uniq_sort_alt_states PARAMS ((alt_state_t));
static int alt_states_eq                PARAMS ((alt_state_t, alt_state_t));
static void initiate_alt_states         PARAMS ((void));
static void finish_alt_states           PARAMS ((void));

static reserv_sets_t alloc_empty_reserv_sets PARAMS ((void));
static unsigned reserv_sets_hash_value PARAMS ((reserv_sets_t));
static int reserv_sets_cmp             PARAMS ((reserv_sets_t, reserv_sets_t));
static int reserv_sets_eq              PARAMS ((reserv_sets_t, reserv_sets_t));
static void set_unit_reserv            PARAMS ((reserv_sets_t, int, int));
static int test_unit_reserv            PARAMS ((reserv_sets_t, int, int));
static int it_is_empty_reserv_sets     PARAMS ((reserv_sets_t))
                                            ATTRIBUTE_UNUSED;
static int reserv_sets_are_intersected PARAMS ((reserv_sets_t, reserv_sets_t));
static void reserv_sets_shift          PARAMS ((reserv_sets_t, reserv_sets_t));
static void reserv_sets_or             PARAMS ((reserv_sets_t, reserv_sets_t,
						reserv_sets_t));
static void reserv_sets_and            PARAMS ((reserv_sets_t, reserv_sets_t,
						reserv_sets_t))
                                            ATTRIBUTE_UNUSED;
static void output_cycle_reservs       PARAMS ((FILE *, reserv_sets_t,
						int, int));
static void output_reserv_sets         PARAMS ((FILE *, reserv_sets_t));
static state_t get_free_state          PARAMS ((int, automaton_t));
static void free_state                 PARAMS ((state_t));
static hashval_t state_hash            PARAMS ((const void *));
static int state_eq_p                  PARAMS ((const void *, const void *));
static state_t insert_state            PARAMS ((state_t));
static void set_state_reserv           PARAMS ((state_t, int, int));
static int intersected_state_reservs_p PARAMS ((state_t, state_t));
static state_t states_union            PARAMS ((state_t, state_t, reserv_sets_t));
static state_t state_shift             PARAMS ((state_t, reserv_sets_t));
static void initiate_states            PARAMS ((void));
static void finish_states              PARAMS ((void));

static void free_arc           PARAMS ((arc_t));
static void remove_arc         PARAMS ((state_t, arc_t));
static arc_t find_arc          PARAMS ((state_t, state_t, ainsn_t));
static arc_t add_arc           PARAMS ((state_t, state_t, ainsn_t, int));
static arc_t first_out_arc     PARAMS ((state_t));
static arc_t next_out_arc      PARAMS ((arc_t));
static void initiate_arcs      PARAMS ((void));
static void finish_arcs        PARAMS ((void));

static automata_list_el_t get_free_automata_list_el PARAMS ((void));
static void free_automata_list_el PARAMS ((automata_list_el_t));
static void free_automata_list PARAMS ((automata_list_el_t));
static hashval_t automata_list_hash PARAMS ((const void *));
static int automata_list_eq_p PARAMS ((const void *, const void *));
static void initiate_automata_lists PARAMS ((void));
static void automata_list_start PARAMS ((void));
static void automata_list_add PARAMS ((automaton_t));
static automata_list_el_t automata_list_finish PARAMS ((void));
static void finish_automata_lists PARAMS ((void));

static void initiate_excl_sets             PARAMS ((void));
static reserv_sets_t get_excl_set          PARAMS ((reserv_sets_t));

static pattern_reserv_t form_reserv_sets_list PARAMS ((pattern_set_el_t));
static void initiate_presence_absence_pattern_sets     PARAMS ((void));
static int check_presence_pattern_sets     PARAMS ((reserv_sets_t,
						    reserv_sets_t, int));
static int check_absence_pattern_sets  PARAMS ((reserv_sets_t, reserv_sets_t,
						int));

static regexp_t copy_insn_regexp     PARAMS ((regexp_t));
static regexp_t transform_1          PARAMS ((regexp_t));
static regexp_t transform_2          PARAMS ((regexp_t));
static regexp_t transform_3          PARAMS ((regexp_t));
static regexp_t regexp_transform_func
                       PARAMS ((regexp_t, regexp_t (*) (regexp_t)));
static regexp_t transform_regexp            PARAMS ((regexp_t));
static void transform_insn_regexps          PARAMS ((void));

static void store_alt_unit_usage PARAMS ((regexp_t, regexp_t, int, int));
static void check_regexp_units_distribution   PARAMS ((const char *, regexp_t));
static void check_unit_distributions_to_automata PARAMS ((void));

static int process_seq_for_forming_states   PARAMS ((regexp_t, automaton_t,
						     int));
static void finish_forming_alt_state        PARAMS ((alt_state_t,
						     automaton_t));
static void process_alts_for_forming_states PARAMS ((regexp_t,
						     automaton_t, int));
static void create_alt_states               PARAMS ((automaton_t));

static void form_ainsn_with_same_reservs    PARAMS ((automaton_t));

static reserv_sets_t form_reservs_matter PARAMS ((automaton_t));
static void make_automaton           PARAMS ((automaton_t));
static void form_arcs_marked_by_insn PARAMS ((state_t));
static int create_composed_state     PARAMS ((state_t, arc_t, vla_ptr_t *));
static void NDFA_to_DFA              PARAMS ((automaton_t));
static void pass_state_graph         PARAMS ((state_t, void (*) (state_t)));
static void pass_states              PARAMS ((automaton_t,
					      void (*) (state_t)));
static void initiate_pass_states       PARAMS ((void));
static void add_achieved_state         PARAMS ((state_t));
static int set_out_arc_insns_equiv_num PARAMS ((state_t, int));
static void clear_arc_insns_equiv_num  PARAMS ((state_t));
static void copy_equiv_class           PARAMS ((vla_ptr_t *to,
						const vla_ptr_t *from));
static int first_cycle_unit_presence   PARAMS ((state_t, int));
static int state_is_differed           PARAMS ((state_t, state_t, int, int));
static state_t init_equiv_class        PARAMS ((state_t *states, int));
static int partition_equiv_class       PARAMS ((state_t *, int,
						vla_ptr_t *, int *));
static void evaluate_equiv_classes     PARAMS ((automaton_t, vla_ptr_t *));
static void merge_states               PARAMS ((automaton_t, vla_ptr_t *));
static void set_new_cycle_flags        PARAMS ((state_t));
static void minimize_DFA               PARAMS ((automaton_t));
static void incr_states_and_arcs_nums  PARAMS ((state_t));
static void count_states_and_arcs      PARAMS ((automaton_t, int *, int *));
static void build_automaton            PARAMS ((automaton_t));

static void set_order_state_num              PARAMS ((state_t));
static void enumerate_states                 PARAMS ((automaton_t));

static ainsn_t insert_ainsn_into_equiv_class       PARAMS ((ainsn_t, ainsn_t));
static void delete_ainsn_from_equiv_class          PARAMS ((ainsn_t));
static void process_insn_equiv_class               PARAMS ((ainsn_t, arc_t *));
static void process_state_for_insn_equiv_partition PARAMS ((state_t));
static void set_insn_equiv_classes                 PARAMS ((automaton_t));

static double estimate_one_automaton_bound     PARAMS ((void));
static int compare_max_occ_cycle_nums          PARAMS ((const void *,
							const void *));
static void units_to_automata_heuristic_distr  PARAMS ((void));
static ainsn_t create_ainsns                   PARAMS ((void));
static void units_to_automata_distr            PARAMS ((void));
static void create_automata                    PARAMS ((void));

static void form_regexp                      PARAMS ((regexp_t));
static const char *regexp_representation     PARAMS ((regexp_t));
static void finish_regexp_representation     PARAMS ((void));

static void output_range_type            PARAMS ((FILE *, long int, long int));
static int longest_path_length           PARAMS ((state_t));
static void process_state_longest_path_length PARAMS ((state_t));
static void output_dfa_max_issue_rate    PARAMS ((void));
static void output_vect                  PARAMS ((vect_el_t *, int));
static void output_chip_member_name      PARAMS ((FILE *, automaton_t));
static void output_temp_chip_member_name PARAMS ((FILE *, automaton_t));
static void output_translate_vect_name   PARAMS ((FILE *, automaton_t));
static void output_trans_full_vect_name  PARAMS ((FILE *, automaton_t));
static void output_trans_comb_vect_name  PARAMS ((FILE *, automaton_t));
static void output_trans_check_vect_name PARAMS ((FILE *, automaton_t));
static void output_trans_base_vect_name  PARAMS ((FILE *, automaton_t));
static void output_state_alts_full_vect_name    PARAMS ((FILE *, automaton_t));
static void output_state_alts_comb_vect_name    PARAMS ((FILE *, automaton_t));
static void output_state_alts_check_vect_name   PARAMS ((FILE *, automaton_t));
static void output_state_alts_base_vect_name    PARAMS ((FILE *, automaton_t));
static void output_min_issue_delay_vect_name    PARAMS ((FILE *, automaton_t));
static void output_dead_lock_vect_name   PARAMS ((FILE *, automaton_t));
static void output_reserved_units_table_name    PARAMS ((FILE *, automaton_t));
static void output_state_member_type     PARAMS ((FILE *, automaton_t));
static void output_chip_definitions      PARAMS ((void));
static void output_translate_vect        PARAMS ((automaton_t));
static int comb_vect_p                   PARAMS ((state_ainsn_table_t));
static state_ainsn_table_t create_state_ainsn_table PARAMS ((automaton_t));
static void output_state_ainsn_table
   PARAMS ((state_ainsn_table_t, char *, void (*) (FILE *, automaton_t),
	    void (*) (FILE *, automaton_t), void (*) (FILE *, automaton_t),
	    void (*) (FILE *, automaton_t)));
static void add_vect                     PARAMS ((state_ainsn_table_t,
						  int, vect_el_t *, int));
static int out_state_arcs_num            PARAMS ((state_t));
static int compare_transition_els_num    PARAMS ((const void *, const void *));
static void add_vect_el 	         PARAMS ((vla_hwint_t *,
						  ainsn_t, int));
static void add_states_vect_el           PARAMS ((state_t));
static void output_trans_table           PARAMS ((automaton_t));
static void output_state_alts_table      PARAMS ((automaton_t));
static int min_issue_delay_pass_states   PARAMS ((state_t, ainsn_t));
static int min_issue_delay               PARAMS ((state_t, ainsn_t));
static void initiate_min_issue_delay_pass_states PARAMS ((void));
static void output_min_issue_delay_table PARAMS ((automaton_t));
static void output_dead_lock_vect        PARAMS ((automaton_t));
static void output_reserved_units_table  PARAMS ((automaton_t));
static void output_tables                PARAMS ((void));
static void output_max_insn_queue_index_def PARAMS ((void));
static void output_insn_code_cases   PARAMS ((void (*) (automata_list_el_t)));
static void output_automata_list_min_issue_delay_code PARAMS ((automata_list_el_t));
static void output_internal_min_issue_delay_func PARAMS ((void));
static void output_automata_list_transition_code PARAMS ((automata_list_el_t));
static void output_internal_trans_func   PARAMS ((void));
static void output_internal_insn_code_evaluation PARAMS ((const char *,
							  const char *, int));
static void output_dfa_insn_code_func	        PARAMS ((void));
static void output_trans_func                   PARAMS ((void));
static void output_automata_list_state_alts_code PARAMS ((automata_list_el_t));
static void output_internal_state_alts_func     PARAMS ((void));
static void output_state_alts_func              PARAMS ((void));
static void output_min_issue_delay_func         PARAMS ((void));
static void output_internal_dead_lock_func      PARAMS ((void));
static void output_dead_lock_func               PARAMS ((void));
static void output_internal_reset_func          PARAMS ((void));
static void output_size_func		        PARAMS ((void));
static void output_reset_func                   PARAMS ((void));
static void output_min_insn_conflict_delay_func PARAMS ((void));
static void output_internal_insn_latency_func   PARAMS ((void));
static void output_insn_latency_func            PARAMS ((void));
static void output_print_reservation_func       PARAMS ((void));
static int units_cmp			        PARAMS ((const void *,
							 const void *));
static void output_get_cpu_unit_code_func       PARAMS ((void));
static void output_cpu_unit_reservation_p       PARAMS ((void));
static void output_dfa_clean_insn_cache_func    PARAMS ((void));
static void output_dfa_start_func	        PARAMS ((void));
static void output_dfa_finish_func	        PARAMS ((void));

static void output_regexp                  PARAMS ((regexp_t ));
static void output_unit_set_el_list	   PARAMS ((unit_set_el_t));
static void output_pattern_set_el_list	   PARAMS ((pattern_set_el_t));
static void output_description             PARAMS ((void));
static void output_automaton_name          PARAMS ((FILE *, automaton_t));
static void output_automaton_units         PARAMS ((automaton_t));
static void add_state_reservs              PARAMS ((state_t));
static void output_state_arcs              PARAMS ((state_t));
static int state_reservs_cmp               PARAMS ((const void *,
						    const void *));
static void remove_state_duplicate_reservs PARAMS ((void));
static void output_state                   PARAMS ((state_t));
static void output_automaton_descriptions  PARAMS ((void));
static void output_statistics              PARAMS ((FILE *));
static void output_time_statistics         PARAMS ((FILE *));
static void generate                       PARAMS ((void));

static void make_insn_alts_attr                PARAMS ((void));
static void make_internal_dfa_insn_code_attr   PARAMS ((void));
static void make_default_insn_latency_attr     PARAMS ((void));
static void make_bypass_attr                   PARAMS ((void));
static const char *file_name_suffix            PARAMS ((const char *));
static const char *base_file_name              PARAMS ((const char *));
static void check_automata_insn_issues	       PARAMS ((void));
static void add_automaton_state                PARAMS ((state_t));
static void form_important_insn_automata_lists PARAMS ((void));

/* Undefined position.  */
static pos_t no_pos = 0;

/* All IR is stored in the following obstack.  */
static struct obstack irp;



/* This page contains code for work with variable length array (vla)
   of pointers.  We could be use only varray.  But we add new lay
   because we add elements very frequently and this could stress OS
   allocator when varray is used only.  */

/* Start work with vla.  */
#define VLA_PTR_CREATE(vla, allocated_length, name)                   	 \
  do									 \
    {                                                                	 \
      vla_ptr_t *const _vla_ptr = &(vla);                                \
                                                                      	 \
      VARRAY_GENERIC_PTR_INIT (_vla_ptr->varray, allocated_length, name);\
      _vla_ptr->length = 0;                                              \
    }									 \
  while (0)

/* Finish work with the vla.  */
#define VLA_PTR_DELETE(vla) VARRAY_FREE ((vla).varray)

/* Return start address of the vla.  */
#define VLA_PTR_BEGIN(vla) ((void *) &VARRAY_GENERIC_PTR ((vla).varray, 0))

/* Address of the last element of the vla.  Do not use side effects in
   the macro argument.  */
#define VLA_PTR_LAST(vla) (&VARRAY_GENERIC_PTR ((vla).varray,         \
                                                (vla).length - 1))
/* Nullify the vla.  */
#define VLA_PTR_NULLIFY(vla)  ((vla).length = 0)

/* Shorten the vla on given number bytes.  */
#define VLA_PTR_SHORTEN(vla, n)  ((vla).length -= (n))

/* Expand the vla on N elements.  The values of new elements are
   undefined.  */
#define VLA_PTR_EXPAND(vla, n)                                        \
  do {                                                                \
    vla_ptr_t *const _expand_vla_ptr = &(vla);                        \
    const size_t _new_length = (n) + _expand_vla_ptr->length;         \
                                                                      \
    if (VARRAY_SIZE (_expand_vla_ptr->varray) < _new_length)          \
      VARRAY_GROW (_expand_vla_ptr->varray,                           \
                   (_new_length - _expand_vla_ptr->length < 128       \
                    ? _expand_vla_ptr->length + 128 : _new_length));  \
    _expand_vla_ptr->length = _new_length;                            \
  } while (0)

/* Add element to the end of the vla.  */
#define VLA_PTR_ADD(vla, ptr)                                           \
  do {                                                                  \
    vla_ptr_t *const _vla_ptr = &(vla);                                 \
                                                                        \
    VLA_PTR_EXPAND (*_vla_ptr, 1);                                      \
    VARRAY_GENERIC_PTR (_vla_ptr->varray, _vla_ptr->length - 1) = (ptr);\
  } while (0)

/* Length of the vla in elements.  */
#define VLA_PTR_LENGTH(vla) ((vla).length)

/* N-th element of the vla.  */
#define VLA_PTR(vla, n) VARRAY_GENERIC_PTR ((vla).varray, n)


/* The following macros are analogous to the previous ones but for
   VLAs of HOST WIDE INTs.  */

#define VLA_HWINT_CREATE(vla, allocated_length, name)                 \
  do {                                                                \
    vla_hwint_t *const _vla_ptr = &(vla);                             \
                                                                      \
    VARRAY_WIDE_INT_INIT (_vla_ptr->varray, allocated_length, name);  \
    _vla_ptr->length = 0;                                             \
  } while (0)

#define VLA_HWINT_DELETE(vla) VARRAY_FREE ((vla).varray)

#define VLA_HWINT_BEGIN(vla) (&VARRAY_WIDE_INT ((vla).varray, 0))

#define VLA_HWINT_NULLIFY(vla)  ((vla).length = 0)

#define VLA_HWINT_EXPAND(vla, n)                                      \
  do {                                                                \
    vla_hwint_t *const _expand_vla_ptr = &(vla);                      \
    const size_t _new_length = (n) + _expand_vla_ptr->length;         \
                                                                      \
    if (VARRAY_SIZE (_expand_vla_ptr->varray) < _new_length)          \
      VARRAY_GROW (_expand_vla_ptr->varray,                           \
                   (_new_length - _expand_vla_ptr->length < 128       \
                    ? _expand_vla_ptr->length + 128 : _new_length));  \
    _expand_vla_ptr->length = _new_length;                            \
  } while (0)

#define VLA_HWINT_ADD(vla, ptr)                                       \
  do {                                                                \
    vla_hwint_t *const _vla_ptr = &(vla);                             \
                                                                      \
    VLA_HWINT_EXPAND (*_vla_ptr, 1);                                  \
    VARRAY_WIDE_INT (_vla_ptr->varray, _vla_ptr->length - 1) = (ptr); \
  } while (0)

#define VLA_HWINT_LENGTH(vla) ((vla).length)

#define VLA_HWINT(vla, n) VARRAY_WIDE_INT ((vla).varray, n)



/* Options with the following names can be set up in automata_option
   construction.  Because the strings occur more one time we use the
   macros.  */

#define NO_MINIMIZATION_OPTION "-no-minimization"

#define TIME_OPTION "-time"

#define V_OPTION "-v"

#define W_OPTION "-w"

#define NDFA_OPTION "-ndfa"

/* The following flags are set up by function `initiate_automaton_gen'.  */

/* Make automata with nondeterministic reservation by insns (`-ndfa').  */
static int ndfa_flag;

/* Do not make minimization of DFA (`-no-minimization').  */
static int no_minimization_flag;

/* Value of this variable is number of automata being generated.  The
   actual number of automata may be less this value if there is not
   sufficient number of units.  This value is defined by argument of
   option `-split' or by constructions automaton if the value is zero
   (it is default value of the argument).  */
static int split_argument;

/* Flag of output time statistics (`-time').  */
static int time_flag;

/* Flag of creation of description file which contains description of
   result automaton and statistics information (`-v').  */
static int v_flag;

/* Flag of generating warning instead of error for non-critical errors
   (`-w').  */
static int w_flag;


/* Output file for pipeline hazard recognizer (PHR) being generated.
   The value is NULL if the file is not defined.  */
static FILE *output_file;

/* Description file of PHR.  The value is NULL if the file is not
   created.  */
static FILE *output_description_file;

/* PHR description file name.  */
static char *output_description_file_name;

/* Value of the following variable is node representing description
   being processed.  This is start point of IR.  */
static struct description *description;



/* This page contains description of IR structure (nodes).  */

enum decl_mode
{
  dm_unit,
  dm_bypass,
  dm_automaton,
  dm_excl,
  dm_presence,
  dm_absence,
  dm_reserv,
  dm_insn_reserv
};

/* This describes define_cpu_unit and define_query_cpu_unit (see file
   rtl.def).  */
struct unit_decl
{
  char *name;
  /* NULL if the automaton name is absent.  */
  char *automaton_name;
  /* If the following value is not zero, the cpu unit reservation is
     described in define_query_cpu_unit.  */
  char query_p;

  /* The following fields are defined by checker.  */

  /* The following field value is nonzero if the unit is used in an
     regexp.  */
  char unit_is_used;

  /* The following field value is order number (0, 1, ...) of given
     unit.  */
  int unit_num;
  /* The following field value is corresponding declaration of
     automaton which was given in description.  If the field value is
     NULL then automaton in the unit declaration was absent.  */
  struct automaton_decl *automaton_decl;
  /* The following field value is maximal cycle number (1, ...) on
     which given unit occurs in insns.  Zero value means that given
     unit is not used in insns.  */
  int max_occ_cycle_num;
  /* The following field value is minimal cycle number (0, ...) on
     which given unit occurs in insns.  -1 value means that given
     unit is not used in insns.  */
  int min_occ_cycle_num;
  /* The following list contains units which conflict with given
     unit.  */
  unit_set_el_t excl_list;
  /* The following list contains patterns which are required to
     reservation of given unit.  */
  pattern_set_el_t presence_list;
  pattern_set_el_t final_presence_list;
  /* The following list contains patterns which should be not present
     in reservation for given unit.  */
  pattern_set_el_t absence_list;
  pattern_set_el_t final_absence_list;
  /* The following is used only when `query_p' has nonzero value.
     This is query number for the unit.  */
  int query_num;
  /* The following is the last cycle on which the unit was checked for
     correct distributions of units to automata in a regexp.  */
  int last_distribution_check_cycle;

  /* The following fields are defined by automaton generator.  */

  /* The following field value is number of the automaton to which
     given unit belongs.  */
  int corresponding_automaton_num;
  /* If the following value is not zero, the cpu unit is present in a
     `exclusion_set' or in right part of a `presence_set',
     `final_presence_set', `absence_set', and
     `final_absence_set'define_query_cpu_unit.  */
  char in_set_p;
};

/* This describes define_bypass (see file rtl.def).  */
struct bypass_decl
{
  int latency;
  char *out_insn_name;
  char *in_insn_name;
  char *bypass_guard_name;

  /* The following fields are defined by checker.  */

  /* output and input insns of given bypass.  */
  struct insn_reserv_decl *out_insn_reserv;
  struct insn_reserv_decl *in_insn_reserv;
  /* The next bypass for given output insn.  */
  struct bypass_decl *next;
};

/* This describes define_automaton (see file rtl.def).  */
struct automaton_decl
{
  char *name;

  /* The following fields are defined by automaton generator.  */

  /* The following field value is nonzero if the automaton is used in
     an regexp definition.  */
  char automaton_is_used;

  /* The following fields are defined by checker.  */

  /* The following field value is the corresponding automaton.  This
     field is not NULL only if the automaton is present in unit
     declarations and the automatic partition on automata is not
     used.  */
  automaton_t corresponding_automaton;
};

/* This describes exclusion relations: exclusion_set (see file
   rtl.def).  */
struct excl_rel_decl
{
  int all_names_num;
  int first_list_length;
  char *names [1];
};

/* This describes unit relations: [final_]presence_set or
   [final_]absence_set (see file rtl.def).  */
struct unit_pattern_rel_decl
{
  int final_p;
  int names_num;
  int patterns_num;
  char **names;
  char ***patterns;
};

/* This describes define_reservation (see file rtl.def).  */
struct reserv_decl
{
  char *name;
  regexp_t regexp;

  /* The following fields are defined by checker.  */

  /* The following field value is nonzero if the unit is used in an
     regexp.  */
  char reserv_is_used;
  /* The following field is used to check up cycle in expression
     definition.  */
  int loop_pass_num;
};

/* This describes define_insn_reservation (see file rtl.def).  */
struct insn_reserv_decl
{
  rtx condexp;
  int default_latency;
  regexp_t regexp;
  char *name;

  /* The following fields are defined by checker.  */

  /* The following field value is order number (0, 1, ...) of given
     insn.  */
  int insn_num;
  /* The following field value is list of bypasses in which given insn
     is output insn.  */
  struct bypass_decl *bypass_list;

  /* The following fields are defined by automaton generator.  */

  /* The following field is the insn regexp transformed that
     the regexp has not optional regexp, repetition regexp, and an
     reservation name (i.e. reservation identifiers are changed by the
     corresponding regexp) and all alternations are the topest level
     of the regexp.  The value can be NULL only if it is special
     insn `cycle advancing'.  */
  regexp_t transformed_regexp;
  /* The following field value is list of arcs marked given
     insn.  The field is used in transformation NDFA -> DFA.  */
  arc_t arcs_marked_by_insn;
  /* The two following fields are used during minimization of a finite state
     automaton.  */
  /* The field value is number of equivalence class of state into
     which arc marked by given insn enters from a state (fixed during
     an automaton minimization).  */
  int equiv_class_num;
  /* The field value is state_alts of arc leaving a state (fixed
     during an automaton minimization) and marked by given insn
     enters.  */
  int state_alts;
  /* The following member value is the list to automata which can be
     changed by the insn issue.  */
  automata_list_el_t important_automata_list;
  /* The following member is used to process insn once for output.  */
  int processed_p;
};

/* This contains a declaration mentioned above.  */
struct decl
{
  /* What node in the union? */
  enum decl_mode mode;
  pos_t pos;
  union
  {
    struct unit_decl unit;
    struct bypass_decl bypass;
    struct automaton_decl automaton;
    struct excl_rel_decl excl;
    struct unit_pattern_rel_decl presence;
    struct unit_pattern_rel_decl absence;
    struct reserv_decl reserv;
    struct insn_reserv_decl insn_reserv;
  } decl;
};

/* The following structures represent parsed reservation strings.  */
enum regexp_mode
{
  rm_unit,
  rm_reserv,
  rm_nothing,
  rm_sequence,
  rm_repeat,
  rm_allof,
  rm_oneof
};

/* Cpu unit in reservation.  */
struct unit_regexp
{
  char *name;
  unit_decl_t unit_decl;
};

/* Define_reservation in a reservation.  */
struct reserv_regexp
{
  char *name;
  struct reserv_decl *reserv_decl;
};

/* Absence of reservation (represented by string `nothing').  */
struct nothing_regexp
{
  /* This used to be empty but ISO C doesn't allow that.  */
  char unused;
};

/* Representation of reservations separated by ',' (see file
   rtl.def).  */
struct sequence_regexp
{
  int regexps_num;
  regexp_t regexps [1];
};

/* Representation of construction `repeat' (see file rtl.def).  */
struct repeat_regexp
{
  int repeat_num;
  regexp_t regexp;
};

/* Representation of reservations separated by '+' (see file
   rtl.def).  */
struct allof_regexp
{
  int regexps_num;
  regexp_t regexps [1];
};

/* Representation of reservations separated by '|' (see file
   rtl.def).  */
struct oneof_regexp
{
  int regexps_num;
  regexp_t regexps [1];
};

/* Representation of a reservation string.  */
struct regexp
{
  /* What node in the union? */
  enum regexp_mode mode;
  pos_t pos;
  union
  {
    struct unit_regexp unit;
    struct reserv_regexp reserv;
    struct nothing_regexp nothing;
    struct sequence_regexp sequence;
    struct repeat_regexp repeat;
    struct allof_regexp allof;
    struct oneof_regexp oneof;
  } regexp;
};

/* Represents description of pipeline hazard description based on
   NDFA.  */
struct description
{
  int decls_num;

  /* The following fields are defined by checker.  */

  /* The following fields values are correspondingly number of all
     units, query units, and insns in the description.  */
  int units_num;
  int query_units_num;
  int insns_num;
  /* The following field value is max length (in cycles) of
     reservations of insns.  The field value is defined only for
     correct programs.  */
  int max_insn_reserv_cycles;

  /* The following fields are defined by automaton generator.  */

  /* The following field value is the first automaton.  */
  automaton_t first_automaton;

  /* The following field is created by pipeline hazard parser and
     contains all declarations.  We allocate additional entry for
     special insn "cycle advancing" which is added by the automaton
     generator.  */
  decl_t decls [1];
};


/* The following nodes are created in automaton checker.  */

/* The following nodes represent exclusion set for cpu units.  Each
   element is accessed through only one excl_list.  */
struct unit_set_el
{
  unit_decl_t unit_decl;
  unit_set_el_t next_unit_set_el;
};

/* The following nodes represent presence or absence pattern for cpu
   units.  Each element is accessed through only one presence_list or
   absence_list.  */
struct pattern_set_el
{
  /* The number of units in unit_decls.  */
  int units_num;
  /* The units forming the pattern.  */
  struct unit_decl **unit_decls;
  pattern_set_el_t next_pattern_set_el;
};


/* The following nodes are created in automaton generator.  */


/* The following nodes represent presence or absence pattern for cpu
   units.  Each element is accessed through only one element of
   unit_presence_set_table or unit_absence_set_table.  */
struct pattern_reserv
{
  reserv_sets_t reserv;
  pattern_reserv_t next_pattern_reserv;
};

/* The following node type describes state automaton.  The state may
   be deterministic or non-deterministic.  Non-deterministic state has
   several component states which represent alternative cpu units
   reservations.  The state also is used for describing a
   deterministic reservation of automaton insn.  */
struct state
{
  /* The following member value is nonzero if there is a transition by
     cycle advancing.  */
  int new_cycle_p;
  /* The following field is list of processor unit reservations on
     each cycle.  */
  reserv_sets_t reservs;
  /* The following field is unique number of given state between other
     states.  */
  int unique_num;
  /* The following field value is automaton to which given state
     belongs.  */
  automaton_t automaton;
  /* The following field value is the first arc output from given
     state.  */
  arc_t first_out_arc;
  /* The following field is used to form NDFA.  */
  char it_was_placed_in_stack_for_NDFA_forming;
  /* The following field is used to form DFA.  */
  char it_was_placed_in_stack_for_DFA_forming;
  /* The following field is used to transform NDFA to DFA and DFA
     minimization.  The field value is not NULL if the state is a
     compound state.  In this case the value of field `unit_sets_list'
     is NULL.  All states in the list are in the hash table.  The list
     is formed through field `next_sorted_alt_state'.  We should
     support only one level of nesting state.  */
  alt_state_t component_states;
  /* The following field is used for passing graph of states.  */
  int pass_num;
  /* The list of states belonging to one equivalence class is formed
     with the aid of the following field.  */
  state_t next_equiv_class_state;
  /* The two following fields are used during minimization of a finite
     state automaton.  */
  int equiv_class_num_1, equiv_class_num_2;
  /* The following field is used during minimization of a finite state
     automaton.  The field value is state corresponding to equivalence
     class to which given state belongs.  */
  state_t equiv_class_state;
  /* The following field value is the order number of given state.
     The states in final DFA is enumerated with the aid of the
     following field.  */
  int order_state_num;
  /* This member is used for passing states for searching minimal
     delay time.  */
  int state_pass_num;
  /* The following member is used to evaluate min issue delay of insn
     for a state.  */
  int min_insn_issue_delay;
  /* The following member is used to evaluate max issue rate of the
     processor.  The value of the member is maximal length of the path
     from given state no containing arcs marked by special insn `cycle
     advancing'.  */
  int longest_path_length;
};

/* The following macro is an initial value of member
   `longest_path_length' of a state.  */
#define UNDEFINED_LONGEST_PATH_LENGTH -1

/* Automaton arc.  */
struct arc
{
  /* The following field refers for the state into which given arc
     enters.  */
  state_t to_state;
  /* The following field describes that the insn issue (with cycle
     advancing for special insn `cycle advancing' and without cycle
     advancing for others) makes transition from given state to
     another given state.  */
  ainsn_t insn;
  /* The following field value is the next arc output from the same
     state.  */
  arc_t next_out_arc;
  /* List of arcs marked given insn is formed with the following
     field.  The field is used in transformation NDFA -> DFA.  */
  arc_t next_arc_marked_by_insn;
  /* The following field is defined if NDFA_FLAG is zero.  The member
     value is number of alternative reservations which can be used for
     transition for given state by given insn.  */
  int state_alts;
};

/* The following node type describes a deterministic alternative in
   non-deterministic state which characterizes cpu unit reservations
   of automaton insn or which is part of NDFA.  */
struct alt_state
{
  /* The following field is a deterministic state which characterizes
     unit reservations of the instruction.  */
  state_t state;
  /* The following field refers to the next state which characterizes
     unit reservations of the instruction.  */
  alt_state_t next_alt_state;
  /* The following field refers to the next state in sorted list.  */
  alt_state_t next_sorted_alt_state;
};

/* The following node type describes insn of automaton.  They are
   labels of FA arcs.  */
struct ainsn
{
  /* The following field value is the corresponding insn declaration
     of description.  */
  struct insn_reserv_decl *insn_reserv_decl;
  /* The following field value is the next insn declaration for an
     automaton.  */
  ainsn_t next_ainsn;
  /* The following field is states which characterize automaton unit
     reservations of the instruction.  The value can be NULL only if it
     is special insn `cycle advancing'.  */
  alt_state_t alt_states;
  /* The following field is sorted list of states which characterize
     automaton unit reservations of the instruction.  The value can be
     NULL only if it is special insn `cycle advancing'.  */
  alt_state_t sorted_alt_states;
  /* The following field refers the next automaton insn with
     the same reservations.  */
  ainsn_t next_same_reservs_insn;
  /* The following field is flag of the first automaton insn with the
     same reservations in the declaration list.  Only arcs marked such
     insn is present in the automaton.  This significantly decreases
     memory requirements especially when several automata are
     formed.  */
  char first_insn_with_same_reservs;
  /* The following member has nonzero value if there is arc from state of
     the automaton marked by the ainsn.  */
  char arc_exists_p;
  /* Cyclic list of insns of an equivalence class is formed with the
     aid of the following field.  */
  ainsn_t next_equiv_class_insn;
  /* The following field value is nonzero if the insn declaration is
     the first insn declaration with given equivalence number.  */
  char first_ainsn_with_given_equialence_num;
  /* The following field is number of class of equivalence of insns.
     It is necessary because many insns may be equivalent with the
     point of view of pipeline hazards.  */
  int insn_equiv_class_num;
  /* The following member value is TRUE if there is an arc in the
     automaton marked by the insn into another state.  In other
     words, the insn can change the state of the automaton.  */
  int important_p;
};

/* The following describes an automaton for PHR.  */
struct automaton
{
  /* The following field value is the list of insn declarations for
     given automaton.  */
  ainsn_t ainsn_list;
  /* The following field value is the corresponding automaton
     declaration.  This field is not NULL only if the automatic
     partition on automata is not used.  */
  struct automaton_decl *corresponding_automaton_decl;
  /* The following field value is the next automaton.  */
  automaton_t next_automaton;
  /* The following field is start state of FA.  There are not unit
     reservations in the state.  */
  state_t start_state;
  /* The following field value is number of equivalence classes of
     insns (see field `insn_equiv_class_num' in
     `insn_reserv_decl').  */
  int insn_equiv_classes_num;
  /* The following field value is number of states of final DFA.  */
  int achieved_states_num;
  /* The following field value is the order number (0, 1, ...) of
     given automaton.  */
  int automaton_order_num;
  /* The following fields contain statistics information about
     building automaton.  */
  int NDFA_states_num, DFA_states_num;
  /* The following field value is defined only if minimization of DFA
     is used.  */
  int minimal_DFA_states_num;
  int NDFA_arcs_num, DFA_arcs_num;
  /* The following field value is defined only if minimization of DFA
     is used.  */
  int minimal_DFA_arcs_num;
  /* The following two members refer for two table state x ainsn ->
     int.  */
  state_ainsn_table_t trans_table;
  state_ainsn_table_t state_alts_table;
  /* The following member value is maximal value of min issue delay
     for insns of the automaton.  */
  int max_min_delay;
  /* Usually min issue delay is small and we can place several (2, 4,
     8) elements in one vector element.  So the compression factor can
     be 1 (no compression), 2, 4, 8.  */
  int min_issue_delay_table_compression_factor;
};

/* The following is the element of the list of automata.  */
struct automata_list_el
{
  /* The automaton itself.  */
  automaton_t automaton;
  /* The next automata set element.  */
  automata_list_el_t next_automata_list_el;
};

/* The following structure describes a table state X ainsn -> int(>= 0).  */
struct state_ainsn_table
{
  /* Automaton to which given table belongs.  */
  automaton_t automaton;
  /* The following tree vectors for comb vector implementation of the
     table.  */
  vla_hwint_t comb_vect;
  vla_hwint_t check_vect;
  vla_hwint_t base_vect;
  /* This is simple implementation of the table.  */
  vla_hwint_t full_vect;
  /* Minimal and maximal values of the previous vectors.  */
  int min_comb_vect_el_value, max_comb_vect_el_value;
  int min_base_vect_el_value, max_base_vect_el_value;
};

/* Macros to access members of unions.  Use only them for access to
   union members of declarations and regexps.  */

#if defined ENABLE_CHECKING && (GCC_VERSION >= 2007)

#define DECL_UNIT(d) __extension__					\
(({ struct decl *const _decl = (d);					\
     if (_decl->mode != dm_unit)					\
       decl_mode_check_failed (_decl->mode, "dm_unit",			\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.unit; }))

#define DECL_BYPASS(d) __extension__					\
(({ struct decl *const _decl = (d);					\
     if (_decl->mode != dm_bypass)					\
       decl_mode_check_failed (_decl->mode, "dm_bypass",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.bypass; }))

#define DECL_AUTOMATON(d) __extension__					\
(({ struct decl *const _decl = (d);				       	\
     if (_decl->mode != dm_automaton)					\
       decl_mode_check_failed (_decl->mode, "dm_automaton",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.automaton; }))

#define DECL_EXCL(d) __extension__					\
(({ struct decl *const _decl = (d);				       	\
     if (_decl->mode != dm_excl)					\
       decl_mode_check_failed (_decl->mode, "dm_excl",			\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.excl; }))

#define DECL_PRESENCE(d) __extension__					\
(({ struct decl *const _decl = (d);					\
     if (_decl->mode != dm_presence)					\
       decl_mode_check_failed (_decl->mode, "dm_presence",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.presence; }))

#define DECL_ABSENCE(d) __extension__					\
(({ struct decl *const _decl = (d);				       	\
     if (_decl->mode != dm_absence)					\
       decl_mode_check_failed (_decl->mode, "dm_absence",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.absence; }))

#define DECL_RESERV(d) __extension__					\
(({ struct decl *const _decl = (d);					\
     if (_decl->mode != dm_reserv)					\
       decl_mode_check_failed (_decl->mode, "dm_reserv",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.reserv; }))

#define DECL_INSN_RESERV(d) __extension__				\
(({ struct decl *const _decl = (d);				       	\
     if (_decl->mode != dm_insn_reserv)					\
       decl_mode_check_failed (_decl->mode, "dm_insn_reserv",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_decl)->decl.insn_reserv; }))

static const char *decl_name PARAMS ((enum decl_mode));
static void decl_mode_check_failed PARAMS ((enum decl_mode, const char *,
					    const char *, int, const char *));

/* Return string representation of declaration mode MODE.  */
static const char *
decl_name (mode)
     enum decl_mode mode;
{
  static char str [100];

  if (mode == dm_unit)
    return "dm_unit";
  else if (mode == dm_bypass)
    return "dm_bypass";
  else if (mode == dm_automaton)
    return "dm_automaton";
  else if (mode == dm_excl)
    return "dm_excl";
  else if (mode == dm_presence)
    return "dm_presence";
  else if (mode == dm_absence)
    return "dm_absence";
  else if (mode == dm_reserv)
    return "dm_reserv";
  else if (mode == dm_insn_reserv)
    return "dm_insn_reserv";
  else
    sprintf (str, "unknown (%d)", (int) mode);
  return str;
}

/* The function prints message about unexpected declaration and finish
   the program.  */
static void
decl_mode_check_failed (mode, expected_mode_str, file, line, func)
     enum decl_mode mode;
     const char *expected_mode_str;
     const char *file;
     int line;
     const char *func;
{
  fprintf
    (stderr,
     "\n%s: %d: error in %s: DECL check: expected decl %s, have %s\n",
     file, line, func, expected_mode_str, decl_name (mode));
  exit (1);
}


#define REGEXP_UNIT(r) __extension__					\
(({ struct regexp *const _regexp = (r);					\
     if (_regexp->mode != rm_unit)					\
       regexp_mode_check_failed (_regexp->mode, "rm_unit",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_regexp)->regexp.unit; }))

#define REGEXP_RESERV(r) __extension__					\
(({ struct regexp *const _regexp = (r);					\
     if (_regexp->mode != rm_reserv)					\
       regexp_mode_check_failed (_regexp->mode, "rm_reserv",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_regexp)->regexp.reserv; }))

#define REGEXP_SEQUENCE(r) __extension__				\
(({ struct regexp *const _regexp = (r);					\
     if (_regexp->mode != rm_sequence)					\
       regexp_mode_check_failed (_regexp->mode, "rm_sequence",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_regexp)->regexp.sequence; }))

#define REGEXP_REPEAT(r) __extension__					\
(({ struct regexp *const _regexp = (r);					\
     if (_regexp->mode != rm_repeat)					\
       regexp_mode_check_failed (_regexp->mode, "rm_repeat",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_regexp)->regexp.repeat; }))

#define REGEXP_ALLOF(r) __extension__					\
(({ struct regexp *const _regexp = (r);					\
     if (_regexp->mode != rm_allof)					\
       regexp_mode_check_failed (_regexp->mode, "rm_allof",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_regexp)->regexp.allof; }))

#define REGEXP_ONEOF(r) __extension__					\
(({ struct regexp *const _regexp = (r);					\
     if (_regexp->mode != rm_oneof)					\
       regexp_mode_check_failed (_regexp->mode, "rm_oneof",		\
			       __FILE__, __LINE__, __FUNCTION__);	\
     &(_regexp)->regexp.oneof; }))

static const char *regexp_name PARAMS ((enum regexp_mode));
static void regexp_mode_check_failed PARAMS ((enum regexp_mode, const char *,
					      const char *, int,
					      const char *));


/* Return string representation of regexp mode MODE.  */
static const char *
regexp_name (mode)
     enum regexp_mode mode;
{
  static char str [100];

  if (mode == rm_unit)
    return "rm_unit";
  else if (mode == rm_reserv)
    return "rm_reserv";
  else if (mode == rm_nothing)
    return "rm_nothing";
  else if (mode == rm_sequence)
    return "rm_sequence";
  else if (mode == rm_repeat)
    return "rm_repeat";
  else if (mode == rm_allof)
    return "rm_allof";
  else if (mode == rm_oneof)
    return "rm_oneof";
  else
    sprintf (str, "unknown (%d)", (int) mode);
  return str;
}

/* The function prints message about unexpected regexp and finish the
   program.  */
static void
regexp_mode_check_failed (mode, expected_mode_str, file, line, func)
     enum regexp_mode mode;
     const char *expected_mode_str;
     const char *file;
     int line;
     const char *func;
{
  fprintf
    (stderr,
     "\n%s: %d: error in %s: REGEXP check: expected decl %s, have %s\n",
     file, line, func, expected_mode_str, regexp_name (mode));
  exit (1);
}

#else /* #if defined ENABLE_RTL_CHECKING && (GCC_VERSION >= 2007) */

#define DECL_UNIT(d) (&(d)->decl.unit)
#define DECL_BYPASS(d) (&(d)->decl.bypass)
#define DECL_AUTOMATON(d) (&(d)->decl.automaton)
#define DECL_EXCL(d) (&(d)->decl.excl)
#define DECL_PRESENCE(d) (&(d)->decl.presence)
#define DECL_ABSENCE(d) (&(d)->decl.absence)
#define DECL_RESERV(d) (&(d)->decl.reserv)
#define DECL_INSN_RESERV(d) (&(d)->decl.insn_reserv)

#define REGEXP_UNIT(r) (&(r)->regexp.unit)
#define REGEXP_RESERV(r) (&(r)->regexp.reserv)
#define REGEXP_SEQUENCE(r) (&(r)->regexp.sequence)
#define REGEXP_REPEAT(r) (&(r)->regexp.repeat)
#define REGEXP_ALLOF(r) (&(r)->regexp.allof)
#define REGEXP_ONEOF(r) (&(r)->regexp.oneof)

#endif /* #if defined ENABLE_RTL_CHECKING && (GCC_VERSION >= 2007) */

/* Create IR structure (node).  */
static void *
create_node (size)
     size_t size;
{
  void *result;

  obstack_blank (&irp, size);
  result = obstack_base (&irp);
  obstack_finish (&irp);
  /* Default values of members are NULL and zero.  */
  memset (result, 0, size);
  return result;
}

/* Copy IR structure (node).  */
static void *
copy_node (from, size)
     const void *from;
     size_t size;
{
  void *const result = create_node (size);
  memcpy (result, from, size);
  return result;
}

/* The function checks that NAME does not contain quotes (`"').  */
static char *
check_name (name, pos)
     char * name;
     pos_t pos ATTRIBUTE_UNUSED;
{
  const char *str;

  for (str = name; *str != '\0'; str++)
    if (*str == '\"')
      error ("Name `%s' contains quotes", name);
  return name;
}

/* Pointers to all declarations during IR generation are stored in the
   following.  */
static vla_ptr_t decls;

/* Given a pointer to a (char *) and a separator, return an alloc'ed
   string containing the next separated element, taking parentheses
   into account if PAR_FLAG has nonzero value.  Advance the pointer to
   after the string scanned, or the end-of-string.  Return NULL if at
   end of string.  */
static char *
next_sep_el (pstr, sep, par_flag)
     char **pstr;
     int sep;
     int par_flag;
{
  char *out_str;
  char *p;
  int pars_num;
  int n_spaces;

  /* Remove leading whitespaces.  */
  while (ISSPACE ((int) **pstr))
    (*pstr)++;

  if (**pstr == '\0')
    return NULL;

  n_spaces = 0;
  for (pars_num = 0, p = *pstr; *p != '\0'; p++)
    {
      if (par_flag && *p == '(')
	pars_num++;
      else if (par_flag && *p == ')')
	pars_num--;
      else if (pars_num == 0 && *p == sep)
	break;
      if (pars_num == 0 && ISSPACE ((int) *p))
	n_spaces++;
      else
	{
	  for (; n_spaces != 0; n_spaces--)
	    obstack_1grow (&irp, p [-n_spaces]);
	  obstack_1grow (&irp, *p);
	}
    }
  obstack_1grow (&irp, '\0');
  out_str = obstack_base (&irp);
  obstack_finish (&irp);

  *pstr = p;
  if (**pstr == sep)
    (*pstr)++;

  return out_str;
}

/* Given a string and a separator, return the number of separated
   elements in it, taking parentheses into account if PAR_FLAG has
   nonzero value.  Return 0 for the null string, -1 if parentheses is
   not balanced.  */
static int
n_sep_els (s, sep, par_flag)
     char *s;
     int sep;
     int par_flag;
{
  int n;
  int pars_num;

  if (*s == '\0')
    return 0;

  for (pars_num = 0, n = 1; *s; s++)
    if (par_flag && *s == '(')
      pars_num++;
    else if (par_flag && *s == ')')
      pars_num--;
    else if (pars_num == 0 && *s == sep)
      n++;

  return (pars_num != 0 ? -1 : n);
}

/* Given a string and a separator, return vector of strings which are
   elements in the string and number of elements through els_num.
   Take parentheses into account if PAREN_P has nonzero value.  The
   function also inserts the end marker NULL at the end of vector.
   Return 0 for the null string, -1 if parantheses are not balanced.  */
static char **
get_str_vect (str, els_num, sep, paren_p)
     char *str;
     int *els_num;
     int sep;
     int paren_p;
{
  int i;
  char **vect;
  char **pstr;

  *els_num = n_sep_els (str, sep, paren_p);
  if (*els_num <= 0)
    return NULL;
  obstack_blank (&irp, sizeof (char *) * (*els_num + 1));
  vect = (char **) obstack_base (&irp);
  obstack_finish (&irp);
  pstr = &str;
  for (i = 0; i < *els_num; i++)
    vect [i] = next_sep_el (pstr, sep, paren_p);
  if (next_sep_el (pstr, sep, paren_p) != NULL)
    abort ();
  vect [i] = NULL;
  return vect;
}

/* Process a DEFINE_CPU_UNIT.  

   This gives information about a unit contained in CPU.  We fill a
   struct unit_decl with information used later by `expand_automata'.  */
void
gen_cpu_unit (def)
     rtx def;
{
  decl_t decl;
  char **str_cpu_units;
  int vect_length;
  int i;

  str_cpu_units = get_str_vect ((char *) XSTR (def, 0), &vect_length, ',',
				FALSE);
  if (str_cpu_units == NULL)
    fatal ("invalid string `%s' in define_cpu_unit", XSTR (def, 0));
  for (i = 0; i < vect_length; i++)
    {
      decl = create_node (sizeof (struct decl));
      decl->mode = dm_unit;
      decl->pos = 0;
      DECL_UNIT (decl)->name = check_name (str_cpu_units [i], decl->pos);
      DECL_UNIT (decl)->automaton_name = (char *) XSTR (def, 1);
      DECL_UNIT (decl)->query_p = 0;
      DECL_UNIT (decl)->min_occ_cycle_num = -1;
      DECL_UNIT (decl)->in_set_p = 0;
      VLA_PTR_ADD (decls, decl);
      num_dfa_decls++;
    }
}

/* Process a DEFINE_QUERY_CPU_UNIT.  

   This gives information about a unit contained in CPU.  We fill a
   struct unit_decl with information used later by `expand_automata'.  */
void
gen_query_cpu_unit (def)
     rtx def;
{
  decl_t decl;
  char **str_cpu_units;
  int vect_length;
  int i;

  str_cpu_units = get_str_vect ((char *) XSTR (def, 0), &vect_length, ',',
				FALSE);
  if (str_cpu_units == NULL)
    fatal ("invalid string `%s' in define_query_cpu_unit", XSTR (def, 0));
  for (i = 0; i < vect_length; i++)
    {
      decl = create_node (sizeof (struct decl));
      decl->mode = dm_unit;
      decl->pos = 0;
      DECL_UNIT (decl)->name = check_name (str_cpu_units [i], decl->pos);
      DECL_UNIT (decl)->automaton_name = (char *) XSTR (def, 1);
      DECL_UNIT (decl)->query_p = 1;
      VLA_PTR_ADD (decls, decl);
      num_dfa_decls++;
    }
}

/* Process a DEFINE_BYPASS.  

   This gives information about a unit contained in the CPU.  We fill
   in a struct bypass_decl with information used later by
   `expand_automata'.  */
void
gen_bypass (def)
     rtx def;
{
  decl_t decl;
  char **out_insns;
  int out_length;
  char **in_insns;
  int in_length;
  int i, j;

  out_insns = get_str_vect ((char *) XSTR (def, 1), &out_length, ',', FALSE);
  if (out_insns == NULL)
    fatal ("invalid string `%s' in define_bypass", XSTR (def, 1));
  in_insns = get_str_vect ((char *) XSTR (def, 2), &in_length, ',', FALSE);
  if (in_insns == NULL)
    fatal ("invalid string `%s' in define_bypass", XSTR (def, 2));
  for (i = 0; i < out_length; i++)
    for (j = 0; j < in_length; j++)
      {
	decl = create_node (sizeof (struct decl));
	decl->mode = dm_bypass;
	decl->pos = 0;
	DECL_BYPASS (decl)->latency = XINT (def, 0);
	DECL_BYPASS (decl)->out_insn_name = out_insns [i];
	DECL_BYPASS (decl)->in_insn_name = in_insns [j];
	DECL_BYPASS (decl)->bypass_guard_name = (char *) XSTR (def, 3);
	VLA_PTR_ADD (decls, decl);
	num_dfa_decls++;
      }
}

/* Process an EXCLUSION_SET.  

   This gives information about a cpu unit conflicts.  We fill a
   struct excl_rel_decl (excl) with information used later by
   `expand_automata'.  */
void
gen_excl_set (def)
     rtx def;
{
  decl_t decl;
  char **first_str_cpu_units;
  char **second_str_cpu_units;
  int first_vect_length;
  int length;
  int i;

  first_str_cpu_units
    = get_str_vect ((char *) XSTR (def, 0), &first_vect_length, ',', FALSE);
  if (first_str_cpu_units == NULL)
    fatal ("invalid first string `%s' in exclusion_set", XSTR (def, 0));
  second_str_cpu_units = get_str_vect ((char *) XSTR (def, 1), &length, ',',
				       FALSE);
  if (second_str_cpu_units == NULL)
    fatal ("invalid second string `%s' in exclusion_set", XSTR (def, 1));
  length += first_vect_length;
  decl = create_node (sizeof (struct decl) + (length - 1) * sizeof (char *));
  decl->mode = dm_excl;
  decl->pos = 0;
  DECL_EXCL (decl)->all_names_num = length;
  DECL_EXCL (decl)->first_list_length = first_vect_length;
  for (i = 0; i < length; i++)
    if (i < first_vect_length)
      DECL_EXCL (decl)->names [i] = first_str_cpu_units [i];
    else
      DECL_EXCL (decl)->names [i]
	= second_str_cpu_units [i - first_vect_length];
  VLA_PTR_ADD (decls, decl);
  num_dfa_decls++;
}

/* Process a PRESENCE_SET, a FINAL_PRESENCE_SET, an ABSENCE_SET,
   FINAL_ABSENCE_SET (it is depended on PRESENCE_P and FINAL_P).

   This gives information about a cpu unit reservation requirements.
   We fill a struct unit_pattern_rel_decl with information used later
   by `expand_automata'.  */
static void
gen_presence_absence_set (def, presence_p, final_p)
     rtx def;
     int presence_p;
     int final_p;
{
  decl_t decl;
  char **str_cpu_units;
  char ***str_patterns;
  int cpu_units_length;
  int length;
  int patterns_length;
  int i;

  str_cpu_units = get_str_vect ((char *) XSTR (def, 0), &cpu_units_length, ',',
				FALSE);
  if (str_cpu_units == NULL)
    fatal ((presence_p
	    ? (final_p
	       ? "invalid first string `%s' in final_presence_set"
	       : "invalid first string `%s' in presence_set")
	    : (final_p
	       ? "invalid first string `%s' in final_absence_set"
	       : "invalid first string `%s' in absence_set")),
	   XSTR (def, 0));
  str_patterns = (char ***) get_str_vect ((char *) XSTR (def, 1),
					  &patterns_length, ',', FALSE);
  if (str_patterns == NULL)
    fatal ((presence_p
	    ? (final_p
	       ? "invalid second string `%s' in final_presence_set"
	       : "invalid second string `%s' in presence_set")
	    : (final_p
	       ? "invalid second string `%s' in final_absence_set"
	       : "invalid second string `%s' in absence_set")), XSTR (def, 1));
  for (i = 0; i < patterns_length; i++)
    {
      str_patterns [i] = get_str_vect ((char *) str_patterns [i], &length, ' ',
				       FALSE);
      if (str_patterns [i] == NULL)
	abort ();
    }
  decl = create_node (sizeof (struct decl));
  decl->pos = 0;
  if (presence_p)
    {
      decl->mode = dm_presence;
      DECL_PRESENCE (decl)->names_num = cpu_units_length;
      DECL_PRESENCE (decl)->names = str_cpu_units;
      DECL_PRESENCE (decl)->patterns = str_patterns;
      DECL_PRESENCE (decl)->patterns_num = patterns_length;
      DECL_PRESENCE (decl)->final_p = final_p;
    }
  else
    {
      decl->mode = dm_absence;
      DECL_ABSENCE (decl)->names_num = cpu_units_length;
      DECL_ABSENCE (decl)->names = str_cpu_units;
      DECL_ABSENCE (decl)->patterns = str_patterns;
      DECL_ABSENCE (decl)->patterns_num = patterns_length;
      DECL_ABSENCE (decl)->final_p = final_p;
    }
  VLA_PTR_ADD (decls, decl);
  num_dfa_decls++;
}

/* Process a PRESENCE_SET.  
 
    This gives information about a cpu unit reservation requirements.
   We fill a struct unit_pattern_rel_decl (presence) with information
   used later by `expand_automata'.  */
 void
gen_presence_set (def)
      rtx def;
{
  gen_presence_absence_set (def, TRUE, FALSE);
}
 
/* Process a FINAL_PRESENCE_SET.  

   This gives information about a cpu unit reservation requirements.
   We fill a struct unit_pattern_rel_decl (presence) with information
   used later by `expand_automata'.  */
void
gen_final_presence_set (def)
     rtx def;
{
  gen_presence_absence_set (def, TRUE, TRUE);
}
 
/* Process an ABSENCE_SET.

   This gives information about a cpu unit reservation requirements.
   We fill a struct unit_pattern_rel_decl (absence) with information
   used later by `expand_automata'.  */
void
gen_absence_set (def)
     rtx def;
{
  gen_presence_absence_set (def, FALSE, FALSE);
}
  
/* Process a FINAL_ABSENCE_SET.

   This gives information about a cpu unit reservation requirements.
   We fill a struct unit_pattern_rel_decl (absence) with information
   used later by `expand_automata'.  */
void
gen_final_absence_set (def)
     rtx def;
{
  gen_presence_absence_set (def, FALSE, TRUE);
}
  
/* Process a DEFINE_AUTOMATON.  

   This gives information about a finite state automaton used for
   recognizing pipeline hazards.  We fill a struct automaton_decl
   with information used later by `expand_automata'.  */
void
gen_automaton (def)
     rtx def;
{
  decl_t decl;
  char **str_automata;
  int vect_length;
  int i;

  str_automata = get_str_vect ((char *) XSTR (def, 0), &vect_length, ',',
			       FALSE);
  if (str_automata == NULL)
    fatal ("invalid string `%s' in define_automaton", XSTR (def, 0));
  for (i = 0; i < vect_length; i++)
    {
      decl = create_node (sizeof (struct decl));
      decl->mode = dm_automaton;
      decl->pos = 0;
      DECL_AUTOMATON (decl)->name = check_name (str_automata [i], decl->pos);
      VLA_PTR_ADD (decls, decl);
      num_dfa_decls++;
    }
}

/* Process an AUTOMATA_OPTION.  

   This gives information how to generate finite state automaton used
   for recognizing pipeline hazards.  */
void
gen_automata_option (def)
     rtx def;
{
  if (strcmp ((char *) XSTR (def, 0), NO_MINIMIZATION_OPTION + 1) == 0)
    no_minimization_flag = 1;
  else if (strcmp ((char *) XSTR (def, 0), TIME_OPTION + 1) == 0)
    time_flag = 1;
  else if (strcmp ((char *) XSTR (def, 0), V_OPTION + 1) == 0)
    v_flag = 1;
  else if (strcmp ((char *) XSTR (def, 0), W_OPTION + 1) == 0)
    w_flag = 1;
  else if (strcmp ((char *) XSTR (def, 0), NDFA_OPTION + 1) == 0)
    ndfa_flag = 1;
  else
    fatal ("invalid option `%s' in automata_option", XSTR (def, 0));
}

/* Name in reservation to denote absence reservation.  */
#define NOTHING_NAME "nothing"

/* The following string contains original reservation string being
   parsed.  */
static char *reserv_str;

/* Parse an element in STR.  */
static regexp_t
gen_regexp_el (str)
     char *str;
{
  regexp_t regexp;
  int len;

  if (*str == '(')
    {
      len = strlen (str);
      if (str [len - 1] != ')')
	fatal ("garbage after ) in reservation `%s'", reserv_str);
      str [len - 1] = '\0';
      regexp = gen_regexp_sequence (str + 1);
    }
  else if (strcmp (str, NOTHING_NAME) == 0)
    {
      regexp = create_node (sizeof (struct decl));
      regexp->mode = rm_nothing;
    }
  else
    {
      regexp = create_node (sizeof (struct decl));
      regexp->mode = rm_unit;
      REGEXP_UNIT (regexp)->name = str;
    }
  return regexp;
}

/* Parse construction `repeat' in STR.  */
static regexp_t
gen_regexp_repeat (str)
     char *str;
{
  regexp_t regexp;
  regexp_t repeat;
  char **repeat_vect;
  int els_num;
  int i;

  repeat_vect = get_str_vect (str, &els_num, '*', TRUE);
  if (repeat_vect == NULL)
    fatal ("invalid `%s' in reservation `%s'", str, reserv_str);
  if (els_num > 1)
    {
      regexp = gen_regexp_el (repeat_vect [0]);
      for (i = 1; i < els_num; i++)
	{
	  repeat = create_node (sizeof (struct regexp));
	  repeat->mode = rm_repeat;
	  REGEXP_REPEAT (repeat)->regexp = regexp;
	  REGEXP_REPEAT (repeat)->repeat_num = atoi (repeat_vect [i]);
          if (REGEXP_REPEAT (repeat)->repeat_num <= 1)
            fatal ("repetition `%s' <= 1 in reservation `%s'",
                   str, reserv_str);
          regexp = repeat;
	}
      return regexp;
    }
  else
    return gen_regexp_el (str);
}

/* Parse reservation STR which possibly contains separator '+'.  */
static regexp_t
gen_regexp_allof (str)
     char *str;
{
  regexp_t allof;
  char **allof_vect;
  int els_num;
  int i;

  allof_vect = get_str_vect (str, &els_num, '+', TRUE);
  if (allof_vect == NULL)
    fatal ("invalid `%s' in reservation `%s'", str, reserv_str);
  if (els_num > 1)
    {
      allof = create_node (sizeof (struct regexp)
			   + sizeof (regexp_t) * (els_num - 1));
      allof->mode = rm_allof;
      REGEXP_ALLOF (allof)->regexps_num = els_num;
      for (i = 0; i < els_num; i++)
	REGEXP_ALLOF (allof)->regexps [i] = gen_regexp_repeat (allof_vect [i]);
      return allof;
    }
  else
    return gen_regexp_repeat (str);
}

/* Parse reservation STR which possibly contains separator '|'.  */
static regexp_t
gen_regexp_oneof (str)
     char *str;
{
  regexp_t oneof;
  char **oneof_vect;
  int els_num;
  int i;

  oneof_vect = get_str_vect (str, &els_num, '|', TRUE);
  if (oneof_vect == NULL)
    fatal ("invalid `%s' in reservation `%s'", str, reserv_str);
  if (els_num > 1)
    {
      oneof = create_node (sizeof (struct regexp)
			   + sizeof (regexp_t) * (els_num - 1));
      oneof->mode = rm_oneof;
      REGEXP_ONEOF (oneof)->regexps_num = els_num;
      for (i = 0; i < els_num; i++)
	REGEXP_ONEOF (oneof)->regexps [i] = gen_regexp_allof (oneof_vect [i]);
      return oneof;
    }
  else
    return gen_regexp_allof (str);
}

/* Parse reservation STR which possibly contains separator ','.  */
static regexp_t
gen_regexp_sequence (str)
     char *str;
{
  regexp_t sequence;
  char **sequence_vect;
  int els_num;
  int i;

  sequence_vect = get_str_vect (str, &els_num, ',', TRUE);
  if (els_num > 1)
    {
      sequence = create_node (sizeof (struct regexp)
			      + sizeof (regexp_t) * (els_num - 1));
      sequence->mode = rm_sequence;
      REGEXP_SEQUENCE (sequence)->regexps_num = els_num;
      for (i = 0; i < els_num; i++)
	REGEXP_SEQUENCE (sequence)->regexps [i]
          = gen_regexp_oneof (sequence_vect [i]);
      return sequence;
    }
  else
    return gen_regexp_oneof (str);
}

/* Parse construction reservation STR.  */
static regexp_t
gen_regexp (str)
     char *str;
{
  reserv_str = str;
  return gen_regexp_sequence (str);;
}

/* Process a DEFINE_RESERVATION.

   This gives information about a reservation of cpu units.  We fill
   in a struct reserv_decl with information used later by
   `expand_automata'.  */
void
gen_reserv (def)
     rtx def;
{
  decl_t decl;

  decl = create_node (sizeof (struct decl));
  decl->mode = dm_reserv;
  decl->pos = 0;
  DECL_RESERV (decl)->name = check_name ((char *) XSTR (def, 0), decl->pos);
  DECL_RESERV (decl)->regexp = gen_regexp ((char *) XSTR (def, 1));
  VLA_PTR_ADD (decls, decl);
  num_dfa_decls++;
}

/* Process a DEFINE_INSN_RESERVATION.

   This gives information about the reservation of cpu units by an
   insn.  We fill a struct insn_reserv_decl with information used
   later by `expand_automata'.  */
void
gen_insn_reserv (def)
     rtx def;
{
  decl_t decl;

  decl = create_node (sizeof (struct decl));
  decl->mode = dm_insn_reserv;
  decl->pos = 0;
  DECL_INSN_RESERV (decl)->name
    = check_name ((char *) XSTR (def, 0), decl->pos);
  DECL_INSN_RESERV (decl)->default_latency = XINT (def, 1);
  DECL_INSN_RESERV (decl)->condexp = XEXP (def, 2);
  DECL_INSN_RESERV (decl)->regexp = gen_regexp ((char *) XSTR (def, 3));
  VLA_PTR_ADD (decls, decl);
  num_dfa_decls++;
}



/* The function evaluates hash value (0..UINT_MAX) of string.  */
static unsigned
string_hash (string)
     const char *string;
{
  unsigned result, i;

  for (result = i = 0;*string++ != '\0'; i++)
    result += ((unsigned char) *string << (i % CHAR_BIT));
  return result;
}



/* This page contains abstract data `table of automaton declarations'.
   Elements of the table is nodes representing automaton declarations.
   Key of the table elements is name of given automaton.  Remember
   that automaton names have own space.  */

/* The function evaluates hash value of an automaton declaration.  The
   function is used by abstract data `hashtab'.  The function returns
   hash value (0..UINT_MAX) of given automaton declaration.  */
static hashval_t
automaton_decl_hash (automaton_decl)
     const void *automaton_decl;
{
  const decl_t decl = (decl_t) automaton_decl;

  if (decl->mode == dm_automaton && DECL_AUTOMATON (decl)->name == NULL)
    abort ();
  return string_hash (DECL_AUTOMATON (decl)->name);
}

/* The function tests automaton declarations on equality of their
   keys.  The function is used by abstract data `hashtab'.  The
   function returns 1 if the declarations have the same key, 0
   otherwise.  */
static int
automaton_decl_eq_p (automaton_decl_1, automaton_decl_2)
     const void* automaton_decl_1;
     const void* automaton_decl_2;
{
  const decl_t decl1 = (decl_t) automaton_decl_1;
  const decl_t decl2 = (decl_t) automaton_decl_2;

  if (decl1->mode != dm_automaton || DECL_AUTOMATON (decl1)->name == NULL
      || decl2->mode != dm_automaton || DECL_AUTOMATON (decl2)->name == NULL)
    abort ();
  return strcmp (DECL_AUTOMATON (decl1)->name,
		 DECL_AUTOMATON (decl2)->name) == 0;
}

/* The automaton declaration table itself is represented by the
   following variable.  */
static htab_t automaton_decl_table;

/* The function inserts automaton declaration into the table.  The
   function does nothing if an automaton declaration with the same key
   exists already in the table.  The function returns automaton
   declaration node in the table with the same key as given automaton
   declaration node.  */
static decl_t
insert_automaton_decl (automaton_decl)
     decl_t automaton_decl;
{
  void **entry_ptr;

  entry_ptr = htab_find_slot (automaton_decl_table, automaton_decl, 1);
  if (*entry_ptr == NULL)
    *entry_ptr = (void *) automaton_decl;
  return (decl_t) *entry_ptr;
}

/* The following variable value is node representing automaton
   declaration.  The node used for searching automaton declaration
   with given name.  */
static struct decl work_automaton_decl;

/* The function searches for automaton declaration in the table with
   the same key as node representing name of the automaton
   declaration.  The function returns node found in the table, NULL if
   such node does not exist in the table.  */
static decl_t
find_automaton_decl (name)
     char *name;
{
  void *entry;

  work_automaton_decl.mode = dm_automaton;
  DECL_AUTOMATON (&work_automaton_decl)->name = name;
  entry = htab_find (automaton_decl_table, &work_automaton_decl);
  return (decl_t) entry;
}

/* The function creates empty automaton declaration table and node
   representing automaton declaration and used for searching automaton
   declaration with given name.  The function must be called only once
   before any work with the automaton declaration table.  */
static void
initiate_automaton_decl_table ()
{
  work_automaton_decl.mode = dm_automaton;
  automaton_decl_table = htab_create (10, automaton_decl_hash,
				      automaton_decl_eq_p, (htab_del) 0);
}

/* The function deletes the automaton declaration table.  Only call of
   function `initiate_automaton_decl_table' is possible immediately
   after this function call.  */
static void
finish_automaton_decl_table ()
{
  htab_delete (automaton_decl_table);
}



/* This page contains abstract data `table of insn declarations'.
   Elements of the table is nodes representing insn declarations.  Key
   of the table elements is name of given insn (in corresponding
   define_insn_reservation).  Remember that insn names have own
   space.  */

/* The function evaluates hash value of an insn declaration.  The
   function is used by abstract data `hashtab'.  The function returns
   hash value (0..UINT_MAX) of given insn declaration.  */
static hashval_t
insn_decl_hash (insn_decl)
     const void *insn_decl;
{
  const decl_t decl = (decl_t) insn_decl;

  if (decl->mode != dm_insn_reserv || DECL_INSN_RESERV (decl)->name == NULL)
    abort ();
  return string_hash (DECL_INSN_RESERV (decl)->name);
}

/* The function tests insn declarations on equality of their keys.
   The function is used by abstract data `hashtab'.  The function
   returns 1 if declarations have the same key, 0 otherwise.  */
static int
insn_decl_eq_p (insn_decl_1, insn_decl_2)
     const void *insn_decl_1;
     const void *insn_decl_2;
{
  const decl_t decl1 = (decl_t) insn_decl_1;
  const decl_t decl2 = (decl_t) insn_decl_2;

  if (decl1->mode != dm_insn_reserv || DECL_INSN_RESERV (decl1)->name == NULL
      || decl2->mode != dm_insn_reserv
      || DECL_INSN_RESERV (decl2)->name == NULL)
    abort ();
  return strcmp (DECL_INSN_RESERV (decl1)->name,
                 DECL_INSN_RESERV (decl2)->name) == 0;
}

/* The insn declaration table itself is represented by the following
   variable.  The table does not contain insn reservation
   declarations.  */
static htab_t insn_decl_table;

/* The function inserts insn declaration into the table.  The function
   does nothing if an insn declaration with the same key exists
   already in the table.  The function returns insn declaration node
   in the table with the same key as given insn declaration node.  */
static decl_t
insert_insn_decl (insn_decl)
     decl_t insn_decl;
{
  void **entry_ptr;

  entry_ptr = htab_find_slot (insn_decl_table, insn_decl, 1);
  if (*entry_ptr == NULL)
    *entry_ptr = (void *) insn_decl;
  return (decl_t) *entry_ptr;
}

/* The following variable value is node representing insn reservation
   declaration.  The node used for searching insn reservation
   declaration with given name.  */
static struct decl work_insn_decl;

/* The function searches for insn reservation declaration in the table
   with the same key as node representing name of the insn reservation
   declaration.  The function returns node found in the table, NULL if
   such node does not exist in the table.  */
static decl_t
find_insn_decl (name)
     char *name;
{
  void *entry;

  work_insn_decl.mode = dm_insn_reserv;
  DECL_INSN_RESERV (&work_insn_decl)->name = name;
  entry = htab_find (insn_decl_table, &work_insn_decl);
  return (decl_t) entry;
}

/* The function creates empty insn declaration table and node
   representing insn declaration and used for searching insn
   declaration with given name.  The function must be called only once
   before any work with the insn declaration table.  */
static void
initiate_insn_decl_table ()
{
  work_insn_decl.mode = dm_insn_reserv;
  insn_decl_table = htab_create (10, insn_decl_hash, insn_decl_eq_p,
				 (htab_del) 0);
}

/* The function deletes the insn declaration table.  Only call of
   function `initiate_insn_decl_table' is possible immediately after
   this function call.  */
static void
finish_insn_decl_table ()
{
  htab_delete (insn_decl_table);
}



/* This page contains abstract data `table of declarations'.  Elements
   of the table is nodes representing declarations (of units and
   reservations).  Key of the table elements is names of given
   declarations.  */

/* The function evaluates hash value of a declaration.  The function
   is used by abstract data `hashtab'.  The function returns hash
   value (0..UINT_MAX) of given declaration.  */
static hashval_t
decl_hash (decl)
     const void *decl;
{
  const decl_t d = (const decl_t) decl;

  if ((d->mode != dm_unit || DECL_UNIT (d)->name == NULL)
      && (d->mode != dm_reserv || DECL_RESERV (d)->name == NULL))
    abort ();
  return string_hash (d->mode == dm_unit
		      ? DECL_UNIT (d)->name : DECL_RESERV (d)->name);
}

/* The function tests declarations on equality of their keys.  The
   function is used by abstract data `hashtab'.  The function
   returns 1 if the declarations have the same key, 0 otherwise.  */
static int
decl_eq_p (decl_1, decl_2)
     const void *decl_1;
     const void *decl_2;
{
  const decl_t d1 = (const decl_t) decl_1;
  const decl_t d2 = (const decl_t) decl_2;

  if (((d1->mode != dm_unit || DECL_UNIT (d1)->name == NULL)
       && (d1->mode != dm_reserv || DECL_RESERV (d1)->name == NULL))
      || ((d2->mode != dm_unit || DECL_UNIT (d2)->name == NULL)
	  && (d2->mode != dm_reserv || DECL_RESERV (d2)->name == NULL)))
    abort ();
  return strcmp ((d1->mode == dm_unit
                  ? DECL_UNIT (d1)->name : DECL_RESERV (d1)->name),
                 (d2->mode == dm_unit
                  ? DECL_UNIT (d2)->name : DECL_RESERV (d2)->name)) == 0;
}

/* The declaration table itself is represented by the following
   variable.  */
static htab_t decl_table;

/* The function inserts declaration into the table.  The function does
   nothing if a declaration with the same key exists already in the
   table.  The function returns declaration node in the table with the
   same key as given declaration node.  */

static decl_t
insert_decl (decl)
     decl_t decl;
{
  void **entry_ptr;

  entry_ptr = htab_find_slot (decl_table, decl, 1);
  if (*entry_ptr == NULL)
    *entry_ptr = (void *) decl;
  return (decl_t) *entry_ptr;
}

/* The following variable value is node representing declaration.  The
   node used for searching declaration with given name.  */
static struct decl work_decl;

/* The function searches for declaration in the table with the same
   key as node representing name of the declaration.  The function
   returns node found in the table, NULL if such node does not exist
   in the table.  */
static decl_t
find_decl (name)
     char *name;
{
  void *entry;

  work_decl.mode = dm_unit;
  DECL_UNIT (&work_decl)->name = name;
  entry = htab_find (decl_table, &work_decl);
  return (decl_t) entry;
}

/* The function creates empty declaration table and node representing
   declaration and used for searching declaration with given name.
   The function must be called only once before any work with the
   declaration table.  */
static void
initiate_decl_table ()
{
  work_decl.mode = dm_unit;
  decl_table = htab_create (10, decl_hash, decl_eq_p, (htab_del) 0);
}

/* The function deletes the declaration table.  Only call of function
   `initiate_declaration_table' is possible immediately after this
   function call.  */
static void
finish_decl_table ()
{
  htab_delete (decl_table);
}



/* This page contains checker of pipeline hazard description.  */

/* Checking NAMES in an exclusion clause vector and returning formed
   unit_set_el_list.  */
static unit_set_el_t
process_excls (names, num, excl_pos)
     char **names;
     int num;
     pos_t excl_pos ATTRIBUTE_UNUSED;
{
  unit_set_el_t el_list;
  unit_set_el_t last_el;
  unit_set_el_t new_el;
  decl_t decl_in_table;
  int i;

  el_list = NULL;
  last_el = NULL;
  for (i = 0; i < num; i++)
    {
      decl_in_table = find_decl (names [i]);
      if (decl_in_table == NULL)
	error ("unit `%s' in exclusion is not declared", names [i]);
      else if (decl_in_table->mode != dm_unit)
	error ("`%s' in exclusion is not unit", names [i]);
      else
	{
	  new_el = create_node (sizeof (struct unit_set_el));
	  new_el->unit_decl = DECL_UNIT (decl_in_table);
	  new_el->next_unit_set_el = NULL;
	  if (last_el == NULL)
	    el_list = last_el = new_el;
	  else
	    {
	      last_el->next_unit_set_el = new_el;
	      last_el = last_el->next_unit_set_el;
	    }
	}
    }
  return el_list;
}

/* The function adds each element from SOURCE_LIST to the exclusion
   list of the each element from DEST_LIST.  Checking situation "unit
   excludes itself".  */
static void
add_excls (dest_list, source_list, excl_pos)
     unit_set_el_t dest_list;
     unit_set_el_t source_list;
     pos_t excl_pos ATTRIBUTE_UNUSED;
{
  unit_set_el_t dst;
  unit_set_el_t src;
  unit_set_el_t curr_el;
  unit_set_el_t prev_el;
  unit_set_el_t copy;

  for (dst = dest_list; dst != NULL; dst = dst->next_unit_set_el)
    for (src = source_list; src != NULL; src = src->next_unit_set_el)
      {
	if (dst->unit_decl == src->unit_decl)
	  {
	    error ("unit `%s' excludes itself", src->unit_decl->name);
	    continue;
	  }
	if (dst->unit_decl->automaton_name != NULL
	    && src->unit_decl->automaton_name != NULL
	    && strcmp (dst->unit_decl->automaton_name,
		       src->unit_decl->automaton_name) != 0)
	  {
	    error ("units `%s' and `%s' in exclusion set belong to different automata",
		   src->unit_decl->name, dst->unit_decl->name);
	    continue;
	  }
	for (curr_el = dst->unit_decl->excl_list, prev_el = NULL;
	     curr_el != NULL;
	     prev_el = curr_el, curr_el = curr_el->next_unit_set_el)
	  if (curr_el->unit_decl == src->unit_decl)
	    break;
	if (curr_el == NULL)
	  {
	    /* Element not found - insert.  */
	    copy = copy_node (src, sizeof (*src));
	    copy->next_unit_set_el = NULL;
	    if (prev_el == NULL)
	      dst->unit_decl->excl_list = copy;
	    else
	      prev_el->next_unit_set_el = copy;
	}
    }
}

/* Checking NAMES in presence/absence clause and returning the
   formed unit_set_el_list.  The function is called only after
   processing all exclusion sets.  */
static unit_set_el_t
process_presence_absence_names (names, num, req_pos, presence_p, final_p)
     char **names;
     int num;
     pos_t req_pos ATTRIBUTE_UNUSED;
     int presence_p;
     int final_p;
{
  unit_set_el_t el_list;
  unit_set_el_t last_el;
  unit_set_el_t new_el;
  decl_t decl_in_table;
  int i;

  el_list = NULL;
  last_el = NULL;
  for (i = 0; i < num; i++)
    {
      decl_in_table = find_decl (names [i]);
      if (decl_in_table == NULL)
	error ((presence_p
		? (final_p
		   ? "unit `%s' in final presence set is not declared"
		   : "unit `%s' in presence set is not declared")
		: (final_p
		   ? "unit `%s' in final absence set is not declared"
		   : "unit `%s' in absence set is not declared")), names [i]);
      else if (decl_in_table->mode != dm_unit)
	error ((presence_p
		? (final_p
		   ? "`%s' in final presence set is not unit"
		   : "`%s' in presence set is not unit")
		: (final_p
		   ? "`%s' in final absence set is not unit"
		   : "`%s' in absence set is not unit")), names [i]);
      else
	{
	  new_el = create_node (sizeof (struct unit_set_el));
	  new_el->unit_decl = DECL_UNIT (decl_in_table);
	  new_el->next_unit_set_el = NULL;
	  if (last_el == NULL)
	    el_list = last_el = new_el;
	  else
	    {
	      last_el->next_unit_set_el = new_el;
	      last_el = last_el->next_unit_set_el;
	    }
	}
    }
  return el_list;
}

/* Checking NAMES in patterns of a presence/absence clause and
   returning the formed pattern_set_el_list.  The function is called
   only after processing all exclusion sets.  */
static pattern_set_el_t
process_presence_absence_patterns (patterns, num, req_pos, presence_p, final_p)
     char ***patterns;
     int num;
     pos_t req_pos ATTRIBUTE_UNUSED;
     int presence_p;
     int final_p;
{
  pattern_set_el_t el_list;
  pattern_set_el_t last_el;
  pattern_set_el_t new_el;
  decl_t decl_in_table;
  int i, j;

  el_list = NULL;
  last_el = NULL;
  for (i = 0; i < num; i++)
    {
      for (j = 0; patterns [i] [j] != NULL; j++)
	;
      new_el = create_node (sizeof (struct pattern_set_el)
			    + sizeof (struct unit_decl *) * j);
      new_el->unit_decls
	= (struct unit_decl **) ((char *) new_el
				 + sizeof (struct pattern_set_el));
      new_el->next_pattern_set_el = NULL;
      if (last_el == NULL)
	el_list = last_el = new_el;
      else
	{
	  last_el->next_pattern_set_el = new_el;
	  last_el = last_el->next_pattern_set_el;
	}
      new_el->units_num = 0;
      for (j = 0; patterns [i] [j] != NULL; j++)
	{
	  decl_in_table = find_decl (patterns [i] [j]);
	  if (decl_in_table == NULL)
	    error ((presence_p
		    ? (final_p
		       ? "unit `%s' in final presence set is not declared"
		       : "unit `%s' in presence set is not declared")
		    : (final_p
		       ? "unit `%s' in final absence set is not declared"
		       : "unit `%s' in absence set is not declared")),
		   patterns [i] [j]);
	  else if (decl_in_table->mode != dm_unit)
	    error ((presence_p
		    ? (final_p
		       ? "`%s' in final presence set is not unit"
		       : "`%s' in presence set is not unit")
		    : (final_p
		       ? "`%s' in final absence set is not unit"
		       : "`%s' in absence set is not unit")),
		   patterns [i] [j]);
	  else
	    {
	      new_el->unit_decls [new_el->units_num]
		= DECL_UNIT (decl_in_table);
	      new_el->units_num++;
	    }
	}
    }
  return el_list;
}

/* The function adds each element from PATTERN_LIST to presence (if
   PRESENCE_P) or absence list of the each element from DEST_LIST.
   Checking situations "unit requires own absence", and "unit excludes
   and requires presence of ...", "unit requires absence and presence
   of ...", "units in (final) presence set belong to different
   automata", and "units in (final) absence set belong to different
   automata".  Remember that we process absence sets only after all
   presence sets.  */
static void
add_presence_absence (dest_list, pattern_list, req_pos, presence_p, final_p)
     unit_set_el_t dest_list;
     pattern_set_el_t pattern_list;
     pos_t req_pos ATTRIBUTE_UNUSED;
     int presence_p;
     int final_p;
{
  unit_set_el_t dst;
  pattern_set_el_t pat;
  struct unit_decl *unit;
  unit_set_el_t curr_excl_el;
  pattern_set_el_t curr_pat_el;
  pattern_set_el_t prev_el;
  pattern_set_el_t copy;
  int i;
  int no_error_flag;

  for (dst = dest_list; dst != NULL; dst = dst->next_unit_set_el)
    for (pat = pattern_list; pat != NULL; pat = pat->next_pattern_set_el)
      {
	for (i = 0; i < pat->units_num; i++)
	  {
	    unit = pat->unit_decls [i];
	    if (dst->unit_decl == unit && pat->units_num == 1 && !presence_p)
	      {
		error ("unit `%s' requires own absence", unit->name);
		continue;
	      }
	    if (dst->unit_decl->automaton_name != NULL
		&& unit->automaton_name != NULL
		&& strcmp (dst->unit_decl->automaton_name,
			   unit->automaton_name) != 0)
	      {
		error ((presence_p
			? (final_p
			   ? "units `%s' and `%s' in final presence set belong to different automata"
			   : "units `%s' and `%s' in presence set belong to different automata")
			: (final_p
			   ? "units `%s' and `%s' in final absence set belong to different automata"
			   : "units `%s' and `%s' in absence set belong to different automata")),
		       unit->name, dst->unit_decl->name);
		continue;
	      }
	    no_error_flag = 1;
	    if (presence_p)
	      for (curr_excl_el = dst->unit_decl->excl_list;
		   curr_excl_el != NULL;
		   curr_excl_el = curr_excl_el->next_unit_set_el)
		{
		  if (unit == curr_excl_el->unit_decl && pat->units_num == 1)
		    {
		      if (!w_flag)
			{
			  error ("unit `%s' excludes and requires presence of `%s'",
				 dst->unit_decl->name, unit->name);
			  no_error_flag = 0;
			}
		      else
			warning
			  ("unit `%s' excludes and requires presence of `%s'",
			   dst->unit_decl->name, unit->name);
		    }
		}
	    else if (pat->units_num == 1)
	      for (curr_pat_el = dst->unit_decl->presence_list;
		   curr_pat_el != NULL;
		   curr_pat_el = curr_pat_el->next_pattern_set_el)
		if (curr_pat_el->units_num == 1
		    && unit == curr_pat_el->unit_decls [0])
		  {
		    if (!w_flag)
		      {
			error
			  ("unit `%s' requires absence and presence of `%s'",
			   dst->unit_decl->name, unit->name);
			no_error_flag = 0;
		      }
		    else
		      warning
			("unit `%s' requires absence and presence of `%s'",
			 dst->unit_decl->name, unit->name);
		  }
	    if (no_error_flag)
	      {
		for (prev_el = (presence_p
				? (final_p
				   ? dst->unit_decl->final_presence_list
				   : dst->unit_decl->final_presence_list)
				: (final_p
				   ? dst->unit_decl->final_absence_list
				   : dst->unit_decl->absence_list));
		     prev_el != NULL && prev_el->next_pattern_set_el != NULL;
		     prev_el = prev_el->next_pattern_set_el)
		  ;
		copy = copy_node (pat, sizeof (*pat));
		copy->next_pattern_set_el = NULL;
		if (prev_el == NULL)
		  {
		    if (presence_p)
		      {
			if (final_p)
			  dst->unit_decl->final_presence_list = copy;
			else
			  dst->unit_decl->presence_list = copy;
		      }
		    else if (final_p)
		      dst->unit_decl->final_absence_list = copy;
		    else
		      dst->unit_decl->absence_list = copy;
		  }
		else
		  prev_el->next_pattern_set_el = copy;
	      }
	  }
      }
}


/* The function searches for bypass with given IN_INSN_RESERV in given
   BYPASS_LIST.  */
static struct bypass_decl *
find_bypass (bypass_list, in_insn_reserv)
     struct bypass_decl *bypass_list;
     struct insn_reserv_decl *in_insn_reserv;
{
  struct bypass_decl *bypass;

  for (bypass = bypass_list; bypass != NULL; bypass = bypass->next)
    if (bypass->in_insn_reserv == in_insn_reserv)
      break;
  return bypass;
}

/* The function processes pipeline description declarations, checks
   their correctness, and forms exclusion/presence/absence sets.  */
static void
process_decls ()
{
  decl_t decl;
  decl_t automaton_decl;
  decl_t decl_in_table;
  decl_t out_insn_reserv;
  decl_t in_insn_reserv;
  struct bypass_decl *bypass;
  int automaton_presence;
  int i;

  /* Checking repeated automata declarations.  */
  automaton_presence = 0;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_automaton)
	{
	  automaton_presence = 1;
	  decl_in_table = insert_automaton_decl (decl);
	  if (decl_in_table != decl)
	    {
	      if (!w_flag)
		error ("repeated declaration of automaton `%s'",
		       DECL_AUTOMATON (decl)->name);
	      else
		warning ("repeated declaration of automaton `%s'",
			 DECL_AUTOMATON (decl)->name);
	    }
	}
    }
  /* Checking undeclared automata, repeated declarations (except for
     automata) and correctness of their attributes (insn latency times
     etc.).  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
	{
          DECL_INSN_RESERV (decl)->condexp
	    = check_attr_test (DECL_INSN_RESERV (decl)->condexp, 0, 0);
	  if (DECL_INSN_RESERV (decl)->default_latency < 0)
	    error ("define_insn_reservation `%s' has negative latency time",
		   DECL_INSN_RESERV (decl)->name);
	  DECL_INSN_RESERV (decl)->insn_num = description->insns_num;
	  description->insns_num++;
	  decl_in_table = insert_insn_decl (decl);
	  if (decl_in_table != decl)
	    error ("`%s' is already used as insn reservation name",
		   DECL_INSN_RESERV (decl)->name);
	}
      else if (decl->mode == dm_bypass)
	{
	  if (DECL_BYPASS (decl)->latency < 0)
	    error ("define_bypass `%s - %s' has negative latency time",
		   DECL_BYPASS (decl)->out_insn_name,
		   DECL_BYPASS (decl)->in_insn_name);
	}
      else if (decl->mode == dm_unit || decl->mode == dm_reserv)
	{
	  if (decl->mode == dm_unit)
	    {
	      DECL_UNIT (decl)->automaton_decl = NULL;
	      if (DECL_UNIT (decl)->automaton_name != NULL)
		{
		  automaton_decl
                    = find_automaton_decl (DECL_UNIT (decl)->automaton_name);
		  if (automaton_decl == NULL)
		    error ("automaton `%s' is not declared",
			   DECL_UNIT (decl)->automaton_name);
		  else
		    {
		      DECL_AUTOMATON (automaton_decl)->automaton_is_used = 1;
		      DECL_UNIT (decl)->automaton_decl
			= DECL_AUTOMATON (automaton_decl);
		    }
		}
	      else if (automaton_presence)
		error ("define_unit `%s' without automaton when one defined",
		       DECL_UNIT (decl)->name);
	      DECL_UNIT (decl)->unit_num = description->units_num;
	      description->units_num++;
	      if (strcmp (DECL_UNIT (decl)->name, NOTHING_NAME) == 0)
		{
		  error ("`%s' is declared as cpu unit", NOTHING_NAME);
		  continue;
		}
	      decl_in_table = find_decl (DECL_UNIT (decl)->name);
	    }
	  else
	    {
	      if (strcmp (DECL_RESERV (decl)->name, NOTHING_NAME) == 0)
		{
		  error ("`%s' is declared as cpu reservation", NOTHING_NAME);
		  continue;
		}
	      decl_in_table = find_decl (DECL_RESERV (decl)->name);
	    }
	  if (decl_in_table == NULL)
	    decl_in_table = insert_decl (decl);
	  else
	    {
	      if (decl->mode == dm_unit)
		error ("repeated declaration of unit `%s'",
		       DECL_UNIT (decl)->name);
	      else
		error ("repeated declaration of reservation `%s'",
		       DECL_RESERV (decl)->name);
	    }
	}
    }
  /* Check bypasses and form list of bypasses for each (output)
     insn.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_bypass)
	{
	  out_insn_reserv = find_insn_decl (DECL_BYPASS (decl)->out_insn_name);
	  in_insn_reserv = find_insn_decl (DECL_BYPASS (decl)->in_insn_name);
	  if (out_insn_reserv == NULL)
	    error ("there is no insn reservation `%s'",
		   DECL_BYPASS (decl)->out_insn_name);
	  else if (in_insn_reserv == NULL)
	    error ("there is no insn reservation `%s'",
		   DECL_BYPASS (decl)->in_insn_name);
	  else
	    {
	      DECL_BYPASS (decl)->out_insn_reserv
		= DECL_INSN_RESERV (out_insn_reserv);
	      DECL_BYPASS (decl)->in_insn_reserv
		= DECL_INSN_RESERV (in_insn_reserv);
	      bypass
		= find_bypass (DECL_INSN_RESERV (out_insn_reserv)->bypass_list,
			       DECL_BYPASS (decl)->in_insn_reserv);
	      if (bypass != NULL)
		{
		  if (DECL_BYPASS (decl)->latency == bypass->latency)
		    {
		      if (!w_flag)
			error
			  ("the same bypass `%s - %s' is already defined",
			   DECL_BYPASS (decl)->out_insn_name,
			   DECL_BYPASS (decl)->in_insn_name);
		      else
			warning
			  ("the same bypass `%s - %s' is already defined",
			   DECL_BYPASS (decl)->out_insn_name,
			   DECL_BYPASS (decl)->in_insn_name);
		    }
		  else
		    error ("bypass `%s - %s' is already defined",
			   DECL_BYPASS (decl)->out_insn_name,
			   DECL_BYPASS (decl)->in_insn_name);
		}
	      else
		{
		  DECL_BYPASS (decl)->next
		    = DECL_INSN_RESERV (out_insn_reserv)->bypass_list;
		  DECL_INSN_RESERV (out_insn_reserv)->bypass_list
		    = DECL_BYPASS (decl);
		}
	    }
	}
    }

  /* Check exclusion set declarations and form exclusion sets.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_excl)
	{
	  unit_set_el_t unit_set_el_list;
	  unit_set_el_t unit_set_el_list_2;
	  
	  unit_set_el_list
            = process_excls (DECL_EXCL (decl)->names,
			     DECL_EXCL (decl)->first_list_length, decl->pos);
	  unit_set_el_list_2
	    = process_excls (&DECL_EXCL (decl)->names
			     [DECL_EXCL (decl)->first_list_length],
                             DECL_EXCL (decl)->all_names_num
                             - DECL_EXCL (decl)->first_list_length,
                             decl->pos);
	  add_excls (unit_set_el_list, unit_set_el_list_2, decl->pos);
	  add_excls (unit_set_el_list_2, unit_set_el_list, decl->pos);
	}
    }

  /* Check presence set declarations and form presence sets.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_presence)
	{
	  unit_set_el_t unit_set_el_list;
	  pattern_set_el_t pattern_set_el_list;
	  
	  unit_set_el_list
            = process_presence_absence_names
	      (DECL_PRESENCE (decl)->names, DECL_PRESENCE (decl)->names_num,
	       decl->pos, TRUE, DECL_PRESENCE (decl)->final_p);
	  pattern_set_el_list
	    = process_presence_absence_patterns
	      (DECL_PRESENCE (decl)->patterns,
	       DECL_PRESENCE (decl)->patterns_num,
	       decl->pos, TRUE, DECL_PRESENCE (decl)->final_p);
	  add_presence_absence (unit_set_el_list, pattern_set_el_list,
				decl->pos, TRUE,
				DECL_PRESENCE (decl)->final_p);
	}
    }

  /* Check absence set declarations and form absence sets.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_absence)
	{
	  unit_set_el_t unit_set_el_list;
	  pattern_set_el_t pattern_set_el_list;
	  
	  unit_set_el_list
            = process_presence_absence_names
	      (DECL_ABSENCE (decl)->names, DECL_ABSENCE (decl)->names_num,
	       decl->pos, FALSE, DECL_ABSENCE (decl)->final_p);
	  pattern_set_el_list
	    = process_presence_absence_patterns
	      (DECL_ABSENCE (decl)->patterns,
	       DECL_ABSENCE (decl)->patterns_num,
	       decl->pos, FALSE, DECL_ABSENCE (decl)->final_p);
	  add_presence_absence (unit_set_el_list, pattern_set_el_list,
				decl->pos, FALSE,
				DECL_ABSENCE (decl)->final_p);
	}
    }
}

/* The following function checks that declared automaton is used.  If
   the automaton is not used, the function fixes error/warning.  The
   following function must be called only after `process_decls'.  */
static void
check_automaton_usage ()
{
  decl_t decl;
  int i;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_automaton
	  && !DECL_AUTOMATON (decl)->automaton_is_used)
	{
	  if (!w_flag)
	    error ("automaton `%s' is not used", DECL_AUTOMATON (decl)->name);
	  else
	    warning ("automaton `%s' is not used",
		     DECL_AUTOMATON (decl)->name);
	}
    }
}

/* The following recursive function processes all regexp in order to
   fix usage of units or reservations and to fix errors of undeclared
   name.  The function may change unit_regexp onto reserv_regexp.
   Remember that reserv_regexp does not exist before the function
   call.  */
static regexp_t
process_regexp (regexp)
     regexp_t regexp;
{
  decl_t decl_in_table;
  regexp_t new_regexp;
  int i;
    
  if (regexp->mode == rm_unit)
    {
      decl_in_table = find_decl (REGEXP_UNIT (regexp)->name);
      if (decl_in_table == NULL)
        error ("undeclared unit or reservation `%s'",
	       REGEXP_UNIT (regexp)->name);
      else if (decl_in_table->mode == dm_unit)
	{
	  DECL_UNIT (decl_in_table)->unit_is_used = 1;
	  REGEXP_UNIT (regexp)->unit_decl = DECL_UNIT (decl_in_table);
	}
      else if (decl_in_table->mode == dm_reserv)
	{
	  DECL_RESERV (decl_in_table)->reserv_is_used = 1;
	  new_regexp = create_node (sizeof (struct regexp));
	  new_regexp->mode = rm_reserv;
	  new_regexp->pos = regexp->pos;
	  REGEXP_RESERV (new_regexp)->name = REGEXP_UNIT (regexp)->name;
	  REGEXP_RESERV (new_regexp)->reserv_decl
	    = DECL_RESERV (decl_in_table);
	  regexp = new_regexp;
	}
      else
	abort ();
    }
  else if (regexp->mode == rm_sequence)
    for (i = 0; i <REGEXP_SEQUENCE (regexp)->regexps_num; i++)
     REGEXP_SEQUENCE (regexp)->regexps [i]
	= process_regexp (REGEXP_SEQUENCE (regexp)->regexps [i]);
  else if (regexp->mode == rm_allof)
    for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
      REGEXP_ALLOF (regexp)->regexps [i]
        = process_regexp (REGEXP_ALLOF (regexp)->regexps [i]);
  else if (regexp->mode == rm_oneof)
    for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
      REGEXP_ONEOF (regexp)->regexps [i]
	= process_regexp (REGEXP_ONEOF (regexp)->regexps [i]);
  else if (regexp->mode == rm_repeat)
    REGEXP_REPEAT (regexp)->regexp
      = process_regexp (REGEXP_REPEAT (regexp)->regexp);
  else if (regexp->mode != rm_nothing)
    abort ();
  return regexp;
}

/* The following function processes regexp of define_reservation and
   define_insn_reservation with the aid of function
   `process_regexp'.  */
static void
process_regexp_decls ()
{
  decl_t decl;
  int i;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_reserv)
	DECL_RESERV (decl)->regexp
	  = process_regexp (DECL_RESERV (decl)->regexp);
      else if (decl->mode == dm_insn_reserv)
	DECL_INSN_RESERV (decl)->regexp
	  = process_regexp (DECL_INSN_RESERV (decl)->regexp);
    }
}

/* The following function checks that declared unit is used.  If the
   unit is not used, the function fixes errors/warnings.  The
   following function must be called only after `process_decls',
   `process_regexp_decls'.  */
static void
check_usage ()
{
  decl_t decl;
  int i;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit && !DECL_UNIT (decl)->unit_is_used)
	{
	  if (!w_flag)
	    error ("unit `%s' is not used", DECL_UNIT (decl)->name);
	  else
	    warning ("unit `%s' is not used", DECL_UNIT (decl)->name);
	}
      else if (decl->mode == dm_reserv && !DECL_RESERV (decl)->reserv_is_used)
	{
	  if (!w_flag)
	    error ("reservation `%s' is not used", DECL_RESERV (decl)->name);
	  else
	    warning ("reservation `%s' is not used", DECL_RESERV (decl)->name);
	}
    }
}

/* The following variable value is number of reservation being
   processed on loop recognition.  */
static int curr_loop_pass_num;

/* The following recursive function returns nonzero value if REGEXP
   contains given decl or reservations in given regexp refers for
   given decl.  */
static int
loop_in_regexp (regexp, start_decl)
     regexp_t regexp;
     decl_t start_decl;
{
  int i;

  if (regexp == NULL)
    return 0;
  if (regexp->mode == rm_unit)
    return 0;
  else if (regexp->mode == rm_reserv)
    {
      if (start_decl->mode == dm_reserv
          && REGEXP_RESERV (regexp)->reserv_decl == DECL_RESERV (start_decl))
        return 1;
      else if (REGEXP_RESERV (regexp)->reserv_decl->loop_pass_num
	       == curr_loop_pass_num)
        /* declaration has been processed.  */
        return 0;
      else
        {
	  REGEXP_RESERV (regexp)->reserv_decl->loop_pass_num
            = curr_loop_pass_num;
          return loop_in_regexp (REGEXP_RESERV (regexp)->reserv_decl->regexp,
                                 start_decl);
        }
    }
  else if (regexp->mode == rm_sequence)
    {
      for (i = 0; i <REGEXP_SEQUENCE (regexp)->regexps_num; i++)
	if (loop_in_regexp (REGEXP_SEQUENCE (regexp)->regexps [i], start_decl))
	  return 1;
      return 0;
    }
  else if (regexp->mode == rm_allof)
    {
      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	if (loop_in_regexp (REGEXP_ALLOF (regexp)->regexps [i], start_decl))
	  return 1;
      return 0;
    }
  else if (regexp->mode == rm_oneof)
    {
      for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
	if (loop_in_regexp (REGEXP_ONEOF (regexp)->regexps [i], start_decl))
	  return 1;
      return 0;
    }
  else if (regexp->mode == rm_repeat)
    return loop_in_regexp (REGEXP_REPEAT (regexp)->regexp, start_decl);
  else
    {
      if (regexp->mode != rm_nothing)
	abort ();
      return 0;
    }
}

/* The following function fixes errors "cycle in definition ...".  The
   function uses function `loop_in_regexp' for that.  */
static void
check_loops_in_regexps ()
{
  decl_t decl;
  int i;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_reserv)
	DECL_RESERV (decl)->loop_pass_num = 0;
    }
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      curr_loop_pass_num = i;
      
      if (decl->mode == dm_reserv)
	  {
	    DECL_RESERV (decl)->loop_pass_num = curr_loop_pass_num;
	    if (loop_in_regexp (DECL_RESERV (decl)->regexp, decl))
	      {
		if (DECL_RESERV (decl)->regexp == NULL)
		  abort ();
		error ("cycle in definition of reservation `%s'",
		       DECL_RESERV (decl)->name);
	      }
	  }
    }
}

/* The function recursively processes IR of reservation and defines
   max and min cycle for reservation of unit.  */
static void
process_regexp_cycles (regexp, max_start_cycle, min_start_cycle,
		       max_finish_cycle, min_finish_cycle)
     regexp_t regexp;
     int max_start_cycle, min_start_cycle;
     int *max_finish_cycle, *min_finish_cycle;
{
  int i;

  if (regexp->mode == rm_unit)
    {
      if (REGEXP_UNIT (regexp)->unit_decl->max_occ_cycle_num < max_start_cycle)
	REGEXP_UNIT (regexp)->unit_decl->max_occ_cycle_num = max_start_cycle;
      if (REGEXP_UNIT (regexp)->unit_decl->min_occ_cycle_num > min_start_cycle
	  || REGEXP_UNIT (regexp)->unit_decl->min_occ_cycle_num == -1)
	REGEXP_UNIT (regexp)->unit_decl->min_occ_cycle_num = min_start_cycle;
      *max_finish_cycle = max_start_cycle;
      *min_finish_cycle = min_start_cycle;
    }
  else if (regexp->mode == rm_reserv)
   process_regexp_cycles (REGEXP_RESERV (regexp)->reserv_decl->regexp,
			  max_start_cycle, min_start_cycle,
			  max_finish_cycle, min_finish_cycle);
  else if (regexp->mode == rm_repeat)
    {
      for (i = 0; i < REGEXP_REPEAT (regexp)->repeat_num; i++)
	{
	  process_regexp_cycles (REGEXP_REPEAT (regexp)->regexp,
				 max_start_cycle, min_start_cycle,
				 max_finish_cycle, min_finish_cycle);
	  max_start_cycle = *max_finish_cycle + 1;
	  min_start_cycle = *min_finish_cycle + 1;
	}
    }
  else if (regexp->mode == rm_sequence)
    {
      for (i = 0; i <REGEXP_SEQUENCE (regexp)->regexps_num; i++)
	{
	  process_regexp_cycles (REGEXP_SEQUENCE (regexp)->regexps [i],
				 max_start_cycle, min_start_cycle,
				 max_finish_cycle, min_finish_cycle);
	  max_start_cycle = *max_finish_cycle + 1;
	  min_start_cycle = *min_finish_cycle + 1;
	}
    }
  else if (regexp->mode == rm_allof)
    {
      int max_cycle = 0;
      int min_cycle = 0;

      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	{
	  process_regexp_cycles (REGEXP_ALLOF (regexp)->regexps [i],
				 max_start_cycle, min_start_cycle,
				 max_finish_cycle, min_finish_cycle);
	  if (max_cycle < *max_finish_cycle)
	    max_cycle = *max_finish_cycle;
	  if (i == 0 || min_cycle > *min_finish_cycle)
	    min_cycle = *min_finish_cycle;
	}
      *max_finish_cycle = max_cycle;
      *min_finish_cycle = min_cycle;
    }
  else if (regexp->mode == rm_oneof)
    {
      int max_cycle = 0;
      int min_cycle = 0;

      for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
	{
	  process_regexp_cycles (REGEXP_ONEOF (regexp)->regexps [i],
				 max_start_cycle, min_start_cycle,
				 max_finish_cycle, min_finish_cycle);
	  if (max_cycle < *max_finish_cycle)
	    max_cycle = *max_finish_cycle;
	  if (i == 0 || min_cycle > *min_finish_cycle)
	    min_cycle = *min_finish_cycle;
	}
      *max_finish_cycle = max_cycle;
      *min_finish_cycle = min_cycle;
    }
  else
    {
      if (regexp->mode != rm_nothing)
	abort ();
      *max_finish_cycle = max_start_cycle;
      *min_finish_cycle = min_start_cycle;
    }
}

/* The following function is called only for correct program.  The
   function defines max reservation of insns in cycles.  */
static void
evaluate_max_reserv_cycles ()
{
  int max_insn_cycles_num;
  int min_insn_cycles_num;
  decl_t decl;
  int i;

  description->max_insn_reserv_cycles = 0;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
      {
        process_regexp_cycles (DECL_INSN_RESERV (decl)->regexp, 0, 0,
			       &max_insn_cycles_num, &min_insn_cycles_num);
        if (description->max_insn_reserv_cycles < max_insn_cycles_num)
	  description->max_insn_reserv_cycles = max_insn_cycles_num;
      }
    }
  description->max_insn_reserv_cycles++;
}

/* The following function calls functions for checking all
   description.  */
static void
check_all_description ()
{
  process_decls ();
  check_automaton_usage ();
  process_regexp_decls ();
  check_usage ();
  check_loops_in_regexps ();
  if (!have_error)
    evaluate_max_reserv_cycles ();
}



/* The page contains abstract data `ticker'.  This data is used to
   report time of different phases of building automata.  It is
   possibly to write a description for which automata will be built
   during several minutes even on fast machine.  */

/* The following function creates ticker and makes it active.  */
static ticker_t
create_ticker ()
{
  ticker_t ticker;

  ticker.modified_creation_time = get_run_time ();
  ticker.incremented_off_time = 0;
  return ticker;
}

/* The following function switches off given ticker.  */
static void
ticker_off (ticker)
     ticker_t *ticker;
{
  if (ticker->incremented_off_time == 0)
    ticker->incremented_off_time = get_run_time () + 1;
}

/* The following function switches on given ticker.  */
static void
ticker_on (ticker)
     ticker_t *ticker;
{
  if (ticker->incremented_off_time != 0)
    {
      ticker->modified_creation_time
        += get_run_time () - ticker->incremented_off_time + 1;
      ticker->incremented_off_time = 0;
    }
}

/* The following function returns current time in milliseconds since
   the moment when given ticker was created.  */
static int
active_time (ticker)
     ticker_t ticker;
{
  if (ticker.incremented_off_time != 0)
    return ticker.incremented_off_time - 1 - ticker.modified_creation_time;
  else
    return get_run_time () - ticker.modified_creation_time;
}

/* The following function returns string representation of active time
   of given ticker.  The result is string representation of seconds
   with accuracy of 1/100 second.  Only result of the last call of the
   function exists.  Therefore the following code is not correct

      printf ("parser time: %s\ngeneration time: %s\n",
              active_time_string (parser_ticker),
              active_time_string (generation_ticker));

   Correct code has to be the following

      printf ("parser time: %s\n", active_time_string (parser_ticker));
      printf ("generation time: %s\n",
              active_time_string (generation_ticker));

*/
static void
print_active_time (f, ticker)
     FILE *f;
     ticker_t ticker;
{
  int msecs;

  msecs = active_time (ticker);
  fprintf (f, "%d.%06d", msecs / 1000000, msecs % 1000000);
}



/* The following variable value is number of automaton which are
   really being created.  This value is defined on the base of
   argument of option `-split'.  If the variable has zero value the
   number of automata is defined by the constructions `%automaton'.
   This case occurs when option `-split' is absent or has zero
   argument.  If constructions `define_automaton' is absent only one
   automaton is created.  */
static int automata_num;

/* The following variable values are times of
       o transformation of regular expressions
       o building NDFA (DFA if !ndfa_flag)
       o NDFA -> DFA   (simply the same automaton if !ndfa_flag)
       o DFA minimization
       o building insn equivalence classes
       o all previous ones
       o code output */
static ticker_t transform_time;
static ticker_t NDFA_time;
static ticker_t NDFA_to_DFA_time;
static ticker_t minimize_time;
static ticker_t equiv_time;
static ticker_t automaton_generation_time;
static ticker_t output_time;

/* The following variable values are times of
       all checking
       all generation
       all pipeline hazard translator work */
static ticker_t check_time;
static ticker_t generation_time;
static ticker_t all_time;



/* Pseudo insn decl which denotes advancing cycle.  */
static decl_t advance_cycle_insn_decl;
static void
add_advance_cycle_insn_decl ()
{
  advance_cycle_insn_decl = create_node (sizeof (struct decl));
  advance_cycle_insn_decl->mode = dm_insn_reserv;
  advance_cycle_insn_decl->pos = no_pos;
  DECL_INSN_RESERV (advance_cycle_insn_decl)->regexp = NULL;
  DECL_INSN_RESERV (advance_cycle_insn_decl)->name = (char *) "$advance_cycle";
  DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num
    = description->insns_num;
  description->decls [description->decls_num] = advance_cycle_insn_decl;
  description->decls_num++;
  description->insns_num++;
  num_dfa_decls++;
}


/* Abstract data `alternative states' which represents
   nondeterministic nature of the description (see comments for
   structures alt_state and state).  */

/* List of free states.  */
static alt_state_t first_free_alt_state;

#ifndef NDEBUG
/* The following variables is maximal number of allocated nodes
   alt_state.  */
static int allocated_alt_states_num = 0;
#endif

/* The following function returns free node alt_state.  It may be new
   allocated node or node freed earlier.  */
static alt_state_t 
get_free_alt_state ()
{
  alt_state_t result;

  if (first_free_alt_state != NULL)
    {
      result = first_free_alt_state;
      first_free_alt_state = first_free_alt_state->next_alt_state;
    }
  else
    {
#ifndef NDEBUG
      allocated_alt_states_num++;
#endif
      result = create_node (sizeof (struct alt_state));
    }
  result->state = NULL;
  result->next_alt_state = NULL;
  result->next_sorted_alt_state = NULL;
  return result;
}

/* The function frees node ALT_STATE.  */
static void
free_alt_state (alt_state)
     alt_state_t alt_state;
{
  if (alt_state == NULL)
    return;
  alt_state->next_alt_state = first_free_alt_state;
  first_free_alt_state = alt_state;
}

/* The function frees list started with node ALT_STATE_LIST.  */
static void
free_alt_states (alt_states_list)
     alt_state_t alt_states_list;
{
  alt_state_t curr_alt_state;
  alt_state_t next_alt_state;

  for (curr_alt_state = alt_states_list;
       curr_alt_state != NULL;
       curr_alt_state = next_alt_state)
    {
      next_alt_state = curr_alt_state->next_alt_state;
      free_alt_state (curr_alt_state);
    }
}

/* The function compares unique numbers of alt states.  */
static int
alt_state_cmp (alt_state_ptr_1, alt_state_ptr_2)
     const void *alt_state_ptr_1;
     const void *alt_state_ptr_2;
{
  if ((*(alt_state_t *) alt_state_ptr_1)->state->unique_num
      == (*(alt_state_t *) alt_state_ptr_2)->state->unique_num)
    return 0;
  else if ((*(alt_state_t *) alt_state_ptr_1)->state->unique_num
	   < (*(alt_state_t *) alt_state_ptr_2)->state->unique_num)
    return -1;
  else
    return 1;
}

/* The function sorts ALT_STATES_LIST and removes duplicated alt
   states from the list.  The comparison key is alt state unique
   number.  */
static alt_state_t 
uniq_sort_alt_states (alt_states_list)
     alt_state_t alt_states_list;
{
  alt_state_t curr_alt_state;
  vla_ptr_t alt_states;
  size_t i;
  size_t prev_unique_state_ind;
  alt_state_t result;
  alt_state_t *result_ptr;

  VLA_PTR_CREATE (alt_states, 150, "alt_states");
  for (curr_alt_state = alt_states_list;
       curr_alt_state != NULL;
       curr_alt_state = curr_alt_state->next_alt_state)
    VLA_PTR_ADD (alt_states, curr_alt_state);
  qsort (VLA_PTR_BEGIN (alt_states), VLA_PTR_LENGTH (alt_states),
	 sizeof (alt_state_t), alt_state_cmp);
  if (VLA_PTR_LENGTH (alt_states) == 0)
    result = NULL;
  else
    {
      result_ptr = VLA_PTR_BEGIN (alt_states);
      prev_unique_state_ind = 0;
      for (i = 1; i < VLA_PTR_LENGTH (alt_states); i++)
        if (result_ptr [prev_unique_state_ind]->state != result_ptr [i]->state)
          {
            prev_unique_state_ind++;
            result_ptr [prev_unique_state_ind] = result_ptr [i];
          }
#if 0
      for (i = prev_unique_state_ind + 1; i < VLA_PTR_LENGTH (alt_states); i++)
        free_alt_state (result_ptr [i]);
#endif
      VLA_PTR_SHORTEN (alt_states, i - prev_unique_state_ind - 1);
      result_ptr = VLA_PTR_BEGIN (alt_states);
      for (i = 1; i < VLA_PTR_LENGTH (alt_states); i++)
        result_ptr [i - 1]->next_sorted_alt_state = result_ptr [i];
      result_ptr [i - 1]->next_sorted_alt_state = NULL;
      result = *result_ptr;
    }
  VLA_PTR_DELETE (alt_states);
  return result;
}

/* The function checks equality of alt state lists.  Remember that the
   lists must be already sorted by the previous function.  */
static int
alt_states_eq (alt_states_1, alt_states_2)
     alt_state_t alt_states_1;
     alt_state_t alt_states_2;
{
  while (alt_states_1 != NULL && alt_states_2 != NULL
         && alt_state_cmp (&alt_states_1, &alt_states_2) == 0)
    {
      alt_states_1 = alt_states_1->next_sorted_alt_state;
      alt_states_2 = alt_states_2->next_sorted_alt_state;
    }
  return alt_states_1 == alt_states_2;
}

/* Initialization of the abstract data.  */
static void
initiate_alt_states ()
{
  first_free_alt_state = NULL;
}

/* Finishing work with the abstract data.  */
static void
finish_alt_states ()
{
}



/* The page contains macros for work with bits strings.  We could use
   standard gcc bitmap or sbitmap but it would result in difficulties
   of building canadian cross.  */

/* Set bit number bitno in the bit string.  The macro is not side
   effect proof.  */
#define SET_BIT(bitstring, bitno)					  \
  (((char *) (bitstring)) [(bitno) / CHAR_BIT] |= 1 << (bitno) % CHAR_BIT)

#define CLEAR_BIT(bitstring, bitno)					  \
  (((char *) (bitstring)) [(bitno) / CHAR_BIT] &= ~(1 << (bitno) % CHAR_BIT))

/* Test if bit number bitno in the bitstring is set.  The macro is not
   side effect proof.  */
#define TEST_BIT(bitstring, bitno)                                        \
  (((char *) (bitstring)) [(bitno) / CHAR_BIT] >> (bitno) % CHAR_BIT & 1)



/* This page contains abstract data `state'.  */

/* Maximal length of reservations in cycles (>= 1).  */
static int max_cycles_num;

/* Number of set elements (see type set_el_t) needed for
   representation of one cycle reservation.  It is depended on units
   number.  */
static int els_in_cycle_reserv;

/* Number of set elements (see type set_el_t) needed for
   representation of maximal length reservation.  Deterministic
   reservation is stored as set (bit string) of length equal to the
   variable value * number of bits in set_el_t.  */
static int els_in_reservs;

/* VLA for representation of array of pointers to unit
   declarations.  */
static vla_ptr_t units_container;

/* The start address of the array.  */
static unit_decl_t *units_array;

/* Temporary reservation of maximal length.  */
static reserv_sets_t temp_reserv;

/* The state table itself is represented by the following variable.  */
static htab_t state_table;

/* VLA for representation of array of pointers to free nodes
   `state'.  */
static vla_ptr_t free_states;

static int curr_unique_state_num;

#ifndef NDEBUG
/* The following variables is maximal number of allocated nodes
   `state'.  */
static int allocated_states_num = 0;
#endif

/* Allocate new reservation set.  */
static reserv_sets_t
alloc_empty_reserv_sets ()
{
  reserv_sets_t result;

  obstack_blank (&irp, els_in_reservs * sizeof (set_el_t));
  result = (reserv_sets_t) obstack_base (&irp);
  obstack_finish (&irp);
  memset (result, 0, els_in_reservs * sizeof (set_el_t));
  return result;
}

/* Hash value of reservation set.  */
static unsigned
reserv_sets_hash_value (reservs)
     reserv_sets_t reservs;
{
  set_el_t hash_value;
  unsigned result;
  int reservs_num, i;
  set_el_t *reserv_ptr;

  hash_value = 0;
  reservs_num = els_in_reservs;
  reserv_ptr = reservs;
  i = 0;
  while (reservs_num != 0)
    {
      reservs_num--;
      hash_value += ((*reserv_ptr >> i)
		     | (*reserv_ptr << (sizeof (set_el_t) * CHAR_BIT - i)));
      i++;
      if (i == sizeof (set_el_t) * CHAR_BIT)
	i = 0;
      reserv_ptr++;
    }
  if (sizeof (set_el_t) <= sizeof (unsigned))
    return hash_value;
  result = 0;
  for (i = sizeof (set_el_t); i > 0; i -= sizeof (unsigned) - 1)
    {
      result += (unsigned) hash_value;
      hash_value >>= (sizeof (unsigned) - 1) * CHAR_BIT;
    }
  return result;
}

/* Comparison of given reservation sets.  */
static int
reserv_sets_cmp (reservs_1, reservs_2)
     reserv_sets_t reservs_1;
     reserv_sets_t reservs_2;
{
  int reservs_num;
  set_el_t *reserv_ptr_1;
  set_el_t *reserv_ptr_2;

  if (reservs_1 == NULL || reservs_2 == NULL)
    abort ();
  reservs_num = els_in_reservs;
  reserv_ptr_1 = reservs_1;
  reserv_ptr_2 = reservs_2;
  while (reservs_num != 0 && *reserv_ptr_1 == *reserv_ptr_2)
    {
      reservs_num--;
      reserv_ptr_1++;
      reserv_ptr_2++;
    }
  if (reservs_num == 0)
    return 0;
  else if (*reserv_ptr_1 < *reserv_ptr_2)
    return -1;
  else
    return 1;
}

/* The function checks equality of the reservation sets.  */
static int
reserv_sets_eq (reservs_1, reservs_2)
     reserv_sets_t reservs_1;
     reserv_sets_t reservs_2;
{
  return reserv_sets_cmp (reservs_1, reservs_2) == 0;
}

/* Set up in the reservation set that unit with UNIT_NUM is used on
   CYCLE_NUM.  */
static void
set_unit_reserv (reservs, cycle_num, unit_num)
     reserv_sets_t reservs;
     int cycle_num;
     int unit_num;
{
  if (cycle_num >= max_cycles_num)
    abort ();
  SET_BIT (reservs, cycle_num * els_in_cycle_reserv
           * sizeof (set_el_t) * CHAR_BIT + unit_num);
}

/* Set up in the reservation set RESERVS that unit with UNIT_NUM is
   used on CYCLE_NUM.  */
static int
test_unit_reserv (reservs, cycle_num, unit_num)
     reserv_sets_t reservs;
     int cycle_num;
     int unit_num;
{
  if (cycle_num >= max_cycles_num)
    abort ();
  return TEST_BIT (reservs, cycle_num * els_in_cycle_reserv
		   * sizeof (set_el_t) * CHAR_BIT + unit_num);
}

/* The function checks that the reservation set represents no one unit
   reservation.  */
static int
it_is_empty_reserv_sets (operand)
     reserv_sets_t operand;
{
  set_el_t *reserv_ptr;
  int reservs_num;

  if (operand == NULL)
    abort ();
  for (reservs_num = els_in_reservs, reserv_ptr = operand;
       reservs_num != 0;
       reserv_ptr++, reservs_num--)
    if (*reserv_ptr != 0)
      return 0;
  return 1;
}

/* The function checks that the reservation sets are intersected,
   i.e. there is a unit reservation on a cycle in both reservation
   sets.  */
static int
reserv_sets_are_intersected (operand_1, operand_2)
     reserv_sets_t operand_1;
     reserv_sets_t operand_2;
{
  set_el_t *el_ptr_1;
  set_el_t *el_ptr_2;
  set_el_t *cycle_ptr_1;
  set_el_t *cycle_ptr_2;

  if (operand_1 == NULL || operand_2 == NULL)
    abort ();
  for (el_ptr_1 = operand_1, el_ptr_2 = operand_2;
       el_ptr_1 < operand_1 + els_in_reservs;
       el_ptr_1++, el_ptr_2++)
    if (*el_ptr_1 & *el_ptr_2)
      return 1;
  reserv_sets_or (temp_reserv, operand_1, operand_2);
  for (cycle_ptr_1 = operand_1, cycle_ptr_2 = operand_2;
       cycle_ptr_1 < operand_1 + els_in_reservs;
       cycle_ptr_1 += els_in_cycle_reserv, cycle_ptr_2 += els_in_cycle_reserv)
    {
      for (el_ptr_1 = cycle_ptr_1, el_ptr_2 = get_excl_set (cycle_ptr_2);
	   el_ptr_1 < cycle_ptr_1 + els_in_cycle_reserv;
	   el_ptr_1++, el_ptr_2++)
	if (*el_ptr_1 & *el_ptr_2)
	  return 1;
      if (!check_presence_pattern_sets (cycle_ptr_1, cycle_ptr_2, FALSE))
	return 1;
      if (!check_presence_pattern_sets (temp_reserv + (cycle_ptr_2
						       - operand_2),
					cycle_ptr_2, TRUE))
	return 1;
      if (!check_absence_pattern_sets (cycle_ptr_1, cycle_ptr_2, FALSE))
	return 1;
      if (!check_absence_pattern_sets (temp_reserv + (cycle_ptr_2 - operand_2),
				       cycle_ptr_2, TRUE))
	return 1;
    }
  return 0;
}

/* The function sets up RESULT bits by bits of OPERAND shifted on one
   cpu cycle.  The remaining bits of OPERAND (representing the last
   cycle unit reservations) are not changed.  */
static void
reserv_sets_shift (result, operand)
     reserv_sets_t result;
     reserv_sets_t operand;
{
  int i;

  if (result == NULL || operand == NULL || result == operand)
    abort ();
  for (i = els_in_cycle_reserv; i < els_in_reservs; i++)
    result [i - els_in_cycle_reserv] = operand [i];
}

/* OR of the reservation sets.  */
static void
reserv_sets_or (result, operand_1, operand_2)
     reserv_sets_t result;
     reserv_sets_t operand_1;
     reserv_sets_t operand_2;
{
  set_el_t *el_ptr_1;
  set_el_t *el_ptr_2;
  set_el_t *result_set_el_ptr;

  if (result == NULL || operand_1 == NULL || operand_2 == NULL)
    abort ();
  for (el_ptr_1 = operand_1, el_ptr_2 = operand_2, result_set_el_ptr = result;
       el_ptr_1 < operand_1 + els_in_reservs;
       el_ptr_1++, el_ptr_2++, result_set_el_ptr++)
    *result_set_el_ptr = *el_ptr_1 | *el_ptr_2;
}

/* AND of the reservation sets.  */
static void
reserv_sets_and (result, operand_1, operand_2)
     reserv_sets_t result;
     reserv_sets_t operand_1;
     reserv_sets_t operand_2;
{
  set_el_t *el_ptr_1;
  set_el_t *el_ptr_2;
  set_el_t *result_set_el_ptr;

  if (result == NULL || operand_1 == NULL || operand_2 == NULL)
    abort ();
  for (el_ptr_1 = operand_1, el_ptr_2 = operand_2, result_set_el_ptr = result;
       el_ptr_1 < operand_1 + els_in_reservs;
       el_ptr_1++, el_ptr_2++, result_set_el_ptr++)
    *result_set_el_ptr = *el_ptr_1 & *el_ptr_2;
}

/* The function outputs string representation of units reservation on
   cycle START_CYCLE in the reservation set.  The function uses repeat
   construction if REPETITION_NUM > 1.  */
static void
output_cycle_reservs (f, reservs, start_cycle, repetition_num)
     FILE *f;
     reserv_sets_t reservs;
     int start_cycle;
     int repetition_num;
{
  int unit_num;
  int reserved_units_num;

  reserved_units_num = 0;
  for (unit_num = 0; unit_num < description->units_num; unit_num++)
    if (TEST_BIT (reservs, start_cycle * els_in_cycle_reserv
                  * sizeof (set_el_t) * CHAR_BIT + unit_num))
      reserved_units_num++;
  if (repetition_num <= 0)
    abort ();
  if (repetition_num != 1 && reserved_units_num > 1)
    fprintf (f, "(");
  reserved_units_num = 0;
  for (unit_num = 0;
       unit_num < description->units_num;
       unit_num++)
    if (TEST_BIT (reservs, start_cycle * els_in_cycle_reserv
		  * sizeof (set_el_t) * CHAR_BIT + unit_num))
      {
        if (reserved_units_num != 0)
          fprintf (f, "+");
        reserved_units_num++;
        fprintf (f, "%s", units_array [unit_num]->name);
      }
  if (reserved_units_num == 0)
    fprintf (f, NOTHING_NAME);
  if (repetition_num <= 0)
    abort ();
  if (repetition_num != 1 && reserved_units_num > 1)
    fprintf (f, ")");
  if (repetition_num != 1)
    fprintf (f, "*%d", repetition_num);
}

/* The function outputs string representation of units reservation in
   the reservation set.  */
static void
output_reserv_sets (f, reservs)
     FILE *f;
     reserv_sets_t reservs;
{
  int start_cycle = 0;
  int cycle;
  int repetition_num;

  repetition_num = 0;
  for (cycle = 0; cycle < max_cycles_num; cycle++)
    if (repetition_num == 0)
      {
        repetition_num++;
        start_cycle = cycle;
      }
    else if (memcmp
             ((char *) reservs + start_cycle * els_in_cycle_reserv
	      * sizeof (set_el_t),
              (char *) reservs + cycle * els_in_cycle_reserv
	      * sizeof (set_el_t),
	      els_in_cycle_reserv * sizeof (set_el_t)) == 0)
      repetition_num++;
    else
      {
        if (start_cycle != 0)
          fprintf (f, ", ");
        output_cycle_reservs (f, reservs, start_cycle, repetition_num);
        repetition_num = 1;
        start_cycle = cycle;
      }
  if (start_cycle < max_cycles_num)
    {
      if (start_cycle != 0)
        fprintf (f, ", ");
      output_cycle_reservs (f, reservs, start_cycle, repetition_num);
    }
}

/* The following function returns free node state for AUTOMATON.  It
   may be new allocated node or node freed earlier.  The function also
   allocates reservation set if WITH_RESERVS has nonzero value.  */
static state_t
get_free_state (with_reservs, automaton)
     int with_reservs;
     automaton_t automaton;
{
  state_t result;

  if (max_cycles_num <= 0 || automaton == NULL)
    abort ();
  if (VLA_PTR_LENGTH (free_states) != 0)
    {
      result = VLA_PTR (free_states, VLA_PTR_LENGTH (free_states) - 1);
      VLA_PTR_SHORTEN (free_states, 1);
      result->automaton = automaton;
      result->first_out_arc = NULL;
      result->it_was_placed_in_stack_for_NDFA_forming = 0;
      result->it_was_placed_in_stack_for_DFA_forming = 0;
      result->component_states = NULL;
      result->longest_path_length = UNDEFINED_LONGEST_PATH_LENGTH;
    }
  else
    {
#ifndef NDEBUG
      allocated_states_num++;
#endif
      result = create_node (sizeof (struct state));
      result->automaton = automaton;
      result->first_out_arc = NULL;
      result->unique_num = curr_unique_state_num;
      result->longest_path_length = UNDEFINED_LONGEST_PATH_LENGTH;
      curr_unique_state_num++;
    }
  if (with_reservs)
    {
      if (result->reservs == NULL)
        result->reservs = alloc_empty_reserv_sets ();
      else
        memset (result->reservs, 0, els_in_reservs * sizeof (set_el_t));
    }
  return result;
}

/* The function frees node STATE.  */
static void
free_state (state)
     state_t state;
{
  free_alt_states (state->component_states);
  VLA_PTR_ADD (free_states, state);
}

/* Hash value of STATE.  If STATE represents deterministic state it is
   simply hash value of the corresponding reservation set.  Otherwise
   it is formed from hash values of the component deterministic
   states.  One more key is order number of state automaton.  */
static hashval_t
state_hash (state)
     const void *state;
{
  unsigned int hash_value;
  alt_state_t alt_state;

  if (((state_t) state)->component_states == NULL)
    hash_value = reserv_sets_hash_value (((state_t) state)->reservs);
  else
    {
      hash_value = 0;
      for (alt_state = ((state_t) state)->component_states;
           alt_state != NULL;
           alt_state = alt_state->next_sorted_alt_state)
        hash_value = (((hash_value >> (sizeof (unsigned) - 1) * CHAR_BIT)
                       | (hash_value << CHAR_BIT))
                      + alt_state->state->unique_num);
    }
  hash_value = (((hash_value >> (sizeof (unsigned) - 1) * CHAR_BIT)
                 | (hash_value << CHAR_BIT))
                + ((state_t) state)->automaton->automaton_order_num);
  return hash_value;
}

/* Return nonzero value if the states are the same.  */
static int
state_eq_p (state_1, state_2)
     const void *state_1;
     const void *state_2;
{
  alt_state_t alt_state_1;
  alt_state_t alt_state_2;

  if (((state_t) state_1)->automaton != ((state_t) state_2)->automaton)
    return 0;
  else if (((state_t) state_1)->component_states == NULL
           && ((state_t) state_2)->component_states == NULL)
    return reserv_sets_eq (((state_t) state_1)->reservs,
			   ((state_t) state_2)->reservs);
  else if (((state_t) state_1)->component_states != NULL
           && ((state_t) state_2)->component_states != NULL)
    {
      for (alt_state_1 = ((state_t) state_1)->component_states,
           alt_state_2 = ((state_t) state_2)->component_states;
           alt_state_1 != NULL && alt_state_2 != NULL;
           alt_state_1 = alt_state_1->next_sorted_alt_state,
	   alt_state_2 = alt_state_2->next_sorted_alt_state)
        /* All state in the list must be already in the hash table.
           Also the lists must be sorted.  */
        if (alt_state_1->state != alt_state_2->state)
          return 0;
      return alt_state_1 == alt_state_2;
    }
  else
    return 0;
}

/* Insert STATE into the state table.  */
static state_t
insert_state (state)
     state_t state;
{
  void **entry_ptr;

  entry_ptr = htab_find_slot (state_table, (void *) state, 1);
  if (*entry_ptr == NULL)
    *entry_ptr = (void *) state;
  return (state_t) *entry_ptr;
}

/* Add reservation of unit with UNIT_NUM on cycle CYCLE_NUM to
   deterministic STATE.  */
static void
set_state_reserv (state, cycle_num, unit_num)
     state_t state;
     int cycle_num;
     int unit_num;
{
  set_unit_reserv (state->reservs, cycle_num, unit_num);
}

/* Return nonzero value if the deterministic states contains a
   reservation of the same cpu unit on the same cpu cycle.  */
static int
intersected_state_reservs_p (state1, state2)
     state_t state1;
     state_t state2;
{
  if (state1->automaton != state2->automaton)
    abort ();
  return reserv_sets_are_intersected (state1->reservs, state2->reservs);
}

/* Return deterministic state (inserted into the table) which
   representing the automaton state which is union of reservations of
   the deterministic states masked by RESERVS.  */
static state_t
states_union (state1, state2, reservs)
     state_t state1;
     state_t state2;
     reserv_sets_t reservs;
{
  state_t result;
  state_t state_in_table;

  if (state1->automaton != state2->automaton)
    abort ();
  result = get_free_state (1, state1->automaton);
  reserv_sets_or (result->reservs, state1->reservs, state2->reservs);
  reserv_sets_and (result->reservs, result->reservs, reservs);
  state_in_table = insert_state (result);
  if (result != state_in_table)
    {
      free_state (result);
      result = state_in_table;
    }
  return result;
}

/* Return deterministic state (inserted into the table) which
   represent the automaton state is obtained from deterministic STATE
   by advancing cpu cycle and masking by RESERVS.  */
static state_t
state_shift (state, reservs)
     state_t state;
     reserv_sets_t reservs;
{
  state_t result;
  state_t state_in_table;

  result = get_free_state (1, state->automaton);
  reserv_sets_shift (result->reservs, state->reservs);
  reserv_sets_and (result->reservs, result->reservs, reservs);
  state_in_table = insert_state (result);
  if (result != state_in_table)
    {
      free_state (result);
      result = state_in_table;
    }
  return result;
}

/* Initialization of the abstract data.  */
static void
initiate_states ()
{
  decl_t decl;
  int i;

  VLA_PTR_CREATE (units_container, description->units_num, "units_container");
  units_array
    = (description->decls_num && description->units_num
       ? VLA_PTR_BEGIN (units_container) : NULL);
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	units_array [DECL_UNIT (decl)->unit_num] = DECL_UNIT (decl);
    }
  max_cycles_num = description->max_insn_reserv_cycles;
  els_in_cycle_reserv
    = ((description->units_num + sizeof (set_el_t) * CHAR_BIT - 1)
       / (sizeof (set_el_t) * CHAR_BIT));
  els_in_reservs = els_in_cycle_reserv * max_cycles_num;
  curr_unique_state_num = 0;
  initiate_alt_states ();
  VLA_PTR_CREATE (free_states, 1500, "free states");
  state_table = htab_create (1500, state_hash, state_eq_p, (htab_del) 0);
  temp_reserv = alloc_empty_reserv_sets ();
}

/* Finishing work with the abstract data.  */
static void
finish_states ()
{
  VLA_PTR_DELETE (units_container);
  htab_delete (state_table);
  VLA_PTR_DELETE (free_states);
  finish_alt_states ();
}



/* Abstract data `arcs'.  */

/* List of free arcs.  */
static arc_t first_free_arc;

#ifndef NDEBUG
/* The following variables is maximal number of allocated nodes
   `arc'.  */
static int allocated_arcs_num = 0;
#endif

/* The function frees node ARC.  */
static void
free_arc (arc)
     arc_t arc;
{
  arc->next_out_arc = first_free_arc;
  first_free_arc = arc;
}

/* The function removes and frees ARC staring from FROM_STATE.  */
static void
remove_arc (from_state, arc)
     state_t from_state;
     arc_t arc;
{
  arc_t prev_arc;
  arc_t curr_arc;

  if (arc == NULL)
    abort ();
  for (prev_arc = NULL, curr_arc = from_state->first_out_arc;
       curr_arc != NULL;
       prev_arc = curr_arc, curr_arc = curr_arc->next_out_arc)
    if (curr_arc == arc)
      break;
  if (curr_arc == NULL)
    abort ();
  if (prev_arc == NULL)
    from_state->first_out_arc = arc->next_out_arc;
  else
    prev_arc->next_out_arc = arc->next_out_arc;
  free_arc (arc);
}

/* The functions returns arc with given characteristics (or NULL if
   the arc does not exist).  */
static arc_t
find_arc (from_state, to_state, insn)
     state_t from_state;
     state_t to_state;
     ainsn_t insn;
{
  arc_t arc;

  for (arc = first_out_arc (from_state); arc != NULL; arc = next_out_arc (arc))
    if (arc->to_state == to_state && arc->insn == insn)
      return arc;
  return NULL;
}

/* The function adds arc from FROM_STATE to TO_STATE marked by AINSN
   and with given STATE_ALTS.  The function returns added arc (or
   already existing arc).  */
static arc_t
add_arc (from_state, to_state, ainsn, state_alts)
     state_t from_state;
     state_t to_state;
     ainsn_t ainsn;
     int state_alts;
{
  arc_t new_arc;

  new_arc = find_arc (from_state, to_state, ainsn);
  if (new_arc != NULL)
    return new_arc;
  if (first_free_arc == NULL)
    {
#ifndef NDEBUG
      allocated_arcs_num++;
#endif
      new_arc = create_node (sizeof (struct arc));
      new_arc->to_state = NULL;
      new_arc->insn = NULL;
      new_arc->next_out_arc = NULL;
    }
  else
    {
      new_arc = first_free_arc;
      first_free_arc =  first_free_arc->next_out_arc;
    }
  new_arc->to_state = to_state;
  new_arc->insn = ainsn;
  ainsn->arc_exists_p = 1;
  new_arc->next_out_arc = from_state->first_out_arc;
  from_state->first_out_arc = new_arc;
  new_arc->next_arc_marked_by_insn = NULL;
  new_arc->state_alts = state_alts;
  return new_arc;
}

/* The function returns the first arc starting from STATE.  */
static arc_t
first_out_arc (state)
     state_t state;
{
  return state->first_out_arc;
}

/* The function returns next out arc after ARC.  */
static arc_t
next_out_arc (arc)
     arc_t arc;
{
  return arc->next_out_arc;
}

/* Initialization of the abstract data.  */
static void
initiate_arcs ()
{
  first_free_arc = NULL;
}

/* Finishing work with the abstract data.  */
static void
finish_arcs ()
{
}



/* Abstract data `automata lists'.  */

/* List of free states.  */
static automata_list_el_t first_free_automata_list_el;

/* The list being formed.  */
static automata_list_el_t current_automata_list;

/* Hash table of automata lists.  */
static htab_t automata_list_table;

/* The following function returns free automata list el.  It may be
   new allocated node or node freed earlier.  */
static automata_list_el_t 
get_free_automata_list_el ()
{
  automata_list_el_t result;

  if (first_free_automata_list_el != NULL)
    {
      result = first_free_automata_list_el;
      first_free_automata_list_el
	= first_free_automata_list_el->next_automata_list_el;
    }
  else
    result = create_node (sizeof (struct automata_list_el));
  result->automaton = NULL;
  result->next_automata_list_el = NULL;
  return result;
}

/* The function frees node AUTOMATA_LIST_EL.  */
static void
free_automata_list_el (automata_list_el)
     automata_list_el_t automata_list_el;
{
  if (automata_list_el == NULL)
    return;
  automata_list_el->next_automata_list_el = first_free_automata_list_el;
  first_free_automata_list_el = automata_list_el;
}

/* The function frees list AUTOMATA_LIST.  */
static void
free_automata_list (automata_list)
     automata_list_el_t automata_list;
{
  automata_list_el_t curr_automata_list_el;
  automata_list_el_t next_automata_list_el;

  for (curr_automata_list_el = automata_list;
       curr_automata_list_el != NULL;
       curr_automata_list_el = next_automata_list_el)
    {
      next_automata_list_el = curr_automata_list_el->next_automata_list_el;
      free_automata_list_el (curr_automata_list_el);
    }
}

/* Hash value of AUTOMATA_LIST.  */
static hashval_t
automata_list_hash (automata_list)
     const void *automata_list;
{
  unsigned int hash_value;
  automata_list_el_t curr_automata_list_el;

  hash_value = 0;
  for (curr_automata_list_el = (automata_list_el_t) automata_list;
       curr_automata_list_el != NULL;
       curr_automata_list_el = curr_automata_list_el->next_automata_list_el)
    hash_value = (((hash_value >> (sizeof (unsigned) - 1) * CHAR_BIT)
		   | (hash_value << CHAR_BIT))
		  + curr_automata_list_el->automaton->automaton_order_num);
  return hash_value;
}

/* Return nonzero value if the automata_lists are the same.  */
static int
automata_list_eq_p (automata_list_1, automata_list_2)
     const void *automata_list_1;
     const void *automata_list_2;
{
  automata_list_el_t automata_list_el_1;
  automata_list_el_t automata_list_el_2;

  for (automata_list_el_1 = (automata_list_el_t) automata_list_1,
	 automata_list_el_2 = (automata_list_el_t) automata_list_2;
       automata_list_el_1 != NULL && automata_list_el_2 != NULL;
       automata_list_el_1 = automata_list_el_1->next_automata_list_el,
	 automata_list_el_2 = automata_list_el_2->next_automata_list_el)
    if (automata_list_el_1->automaton != automata_list_el_2->automaton)
      return 0;
  return automata_list_el_1 == automata_list_el_2;
}

/* Initialization of the abstract data.  */
static void
initiate_automata_lists ()
{
  first_free_automata_list_el = NULL;
  automata_list_table = htab_create (1500, automata_list_hash,
				     automata_list_eq_p, (htab_del) 0);
}

/* The following function starts new automata list and makes it the
   current one.  */
static void
automata_list_start ()
{
  current_automata_list = NULL;
}

/* The following function adds AUTOMATON to the current list.  */
static void
automata_list_add (automaton)
     automaton_t automaton;
{
  automata_list_el_t el;

  el = get_free_automata_list_el ();
  el->automaton = automaton;
  el->next_automata_list_el = current_automata_list;
  current_automata_list = el;
}

/* The following function finishes forming the current list, inserts
   it into the table and returns it.  */
static automata_list_el_t
automata_list_finish ()
{
  void **entry_ptr;

  if (current_automata_list == NULL)
    return NULL;
  entry_ptr = htab_find_slot (automata_list_table,
			      (void *) current_automata_list, 1);
  if (*entry_ptr == NULL)
    *entry_ptr = (void *) current_automata_list;
  else
    free_automata_list (current_automata_list);
  current_automata_list = NULL;
  return (automata_list_el_t) *entry_ptr;
}

/* Finishing work with the abstract data.  */
static void
finish_automata_lists ()
{
  htab_delete (automata_list_table);
}



/* The page contains abstract data for work with exclusion sets (see
   exclusion_set in file rtl.def).  */

/* The following variable refers to an exclusion set returned by
   get_excl_set.  This is bit string of length equal to cpu units
   number.  If exclusion set for given unit contains 1 for a unit,
   then simultaneous reservation of the units is prohibited.  */
static reserv_sets_t excl_set;

/* The array contains exclusion sets for each unit.  */
static reserv_sets_t *unit_excl_set_table;

/* The following function forms the array containing exclusion sets
   for each unit.  */
static void
initiate_excl_sets ()
{
  decl_t decl;
  reserv_sets_t unit_excl_set;
  unit_set_el_t el;
  int i;

  obstack_blank (&irp, els_in_cycle_reserv * sizeof (set_el_t));
  excl_set = (reserv_sets_t) obstack_base (&irp);
  obstack_finish (&irp);
  obstack_blank (&irp, description->units_num * sizeof (reserv_sets_t));
  unit_excl_set_table = (reserv_sets_t *) obstack_base (&irp);
  obstack_finish (&irp);
  /* Evaluate unit exclusion sets.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	{
	  obstack_blank (&irp, els_in_cycle_reserv * sizeof (set_el_t));
	  unit_excl_set = (reserv_sets_t) obstack_base (&irp);
	  obstack_finish (&irp);
	  memset (unit_excl_set, 0, els_in_cycle_reserv * sizeof (set_el_t));
	  for (el = DECL_UNIT (decl)->excl_list;
	       el != NULL;
	       el = el->next_unit_set_el)
	    {
	      SET_BIT (unit_excl_set, el->unit_decl->unit_num);
	      el->unit_decl->in_set_p = TRUE;
	    }
          unit_excl_set_table [DECL_UNIT (decl)->unit_num] = unit_excl_set;
        }
    }
}

/* The function sets up and return EXCL_SET which is union of
   exclusion sets for each unit in IN_SET.  */
static reserv_sets_t
get_excl_set (in_set)
     reserv_sets_t in_set;
{
  int excl_char_num;
  int chars_num;
  int i;
  int start_unit_num;
  int unit_num;

  chars_num = els_in_cycle_reserv * sizeof (set_el_t);
  memset (excl_set, 0, chars_num);
  for (excl_char_num = 0; excl_char_num < chars_num; excl_char_num++)
    if (((unsigned char *) in_set) [excl_char_num])
      for (i = CHAR_BIT - 1; i >= 0; i--)
	if ((((unsigned char *) in_set) [excl_char_num] >> i) & 1)
	  {
	    start_unit_num = excl_char_num * CHAR_BIT + i;
	    if (start_unit_num >= description->units_num)
	      return excl_set;
	    for (unit_num = 0; unit_num < els_in_cycle_reserv; unit_num++)
	      {
		excl_set [unit_num]
		  |= unit_excl_set_table [start_unit_num] [unit_num];
	      }
	  }
  return excl_set;
}



/* The page contains abstract data for work with presence/absence
   pattern sets (see presence_set/absence_set in file rtl.def).  */

/* The following arrays contain correspondingly presence, final
   presence, absence, and final absence patterns for each unit.  */
static pattern_reserv_t *unit_presence_set_table;
static pattern_reserv_t *unit_final_presence_set_table;
static pattern_reserv_t *unit_absence_set_table;
static pattern_reserv_t *unit_final_absence_set_table;

/* The following function forms list of reservation sets for given
   PATTERN_LIST.  */
static pattern_reserv_t
form_reserv_sets_list (pattern_list)
     pattern_set_el_t pattern_list;
{
  pattern_set_el_t el;
  pattern_reserv_t first, curr, prev;
  int i;

  prev = first = NULL;
  for (el = pattern_list; el != NULL; el = el->next_pattern_set_el)
    {
      curr = create_node (sizeof (struct pattern_reserv));
      curr->reserv = alloc_empty_reserv_sets ();
      curr->next_pattern_reserv = NULL;
      for (i = 0; i < el->units_num; i++)
	{
	  SET_BIT (curr->reserv, el->unit_decls [i]->unit_num);
	  el->unit_decls [i]->in_set_p = TRUE;
	}
      if (prev != NULL)
	prev->next_pattern_reserv = curr;
      else
	first = curr;
      prev = curr;
    }
  return first;
}

 /* The following function forms the array containing presence and
   absence pattern sets for each unit.  */
static void
initiate_presence_absence_pattern_sets ()
{
  decl_t decl;
  int i;

  obstack_blank (&irp, description->units_num * sizeof (pattern_reserv_t));
  unit_presence_set_table = (pattern_reserv_t *) obstack_base (&irp);
  obstack_finish (&irp);
  obstack_blank (&irp, description->units_num * sizeof (pattern_reserv_t));
  unit_final_presence_set_table = (pattern_reserv_t *) obstack_base (&irp);
  obstack_finish (&irp);
  obstack_blank (&irp, description->units_num * sizeof (pattern_reserv_t));
  unit_absence_set_table = (pattern_reserv_t *) obstack_base (&irp);
  obstack_finish (&irp);
  obstack_blank (&irp, description->units_num * sizeof (pattern_reserv_t));
  unit_final_absence_set_table = (pattern_reserv_t *) obstack_base (&irp);
  obstack_finish (&irp);
  /* Evaluate unit presence/absence sets.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	{
          unit_presence_set_table [DECL_UNIT (decl)->unit_num]
	    = form_reserv_sets_list (DECL_UNIT (decl)->presence_list);
          unit_final_presence_set_table [DECL_UNIT (decl)->unit_num]
	    = form_reserv_sets_list (DECL_UNIT (decl)->final_presence_list);
          unit_absence_set_table [DECL_UNIT (decl)->unit_num]
	    = form_reserv_sets_list (DECL_UNIT (decl)->absence_list);
          unit_final_absence_set_table [DECL_UNIT (decl)->unit_num]
	    = form_reserv_sets_list (DECL_UNIT (decl)->final_absence_list);
        }
    }
}

/* The function checks that CHECKED_SET satisfies all presence pattern
   sets for units in ORIGIONAL_SET.  The function returns TRUE if it
   is ok.  */
static int
check_presence_pattern_sets (checked_set, origional_set, final_p)
     reserv_sets_t checked_set, origional_set;
     int final_p;
{
  int char_num;
  int chars_num;
  int i;
  int start_unit_num;
  int unit_num;
  int presence_p;
  pattern_reserv_t pat_reserv;
  
  chars_num = els_in_cycle_reserv * sizeof (set_el_t);
  for (char_num = 0; char_num < chars_num; char_num++)
    if (((unsigned char *) origional_set) [char_num])
      for (i = CHAR_BIT - 1; i >= 0; i--)
	if ((((unsigned char *) origional_set) [char_num] >> i) & 1)
	  {
	    start_unit_num = char_num * CHAR_BIT + i;
	    if (start_unit_num >= description->units_num)
	      break;
	    if ((final_p
		 && unit_final_presence_set_table [start_unit_num] == NULL)
		|| (!final_p
		    && unit_presence_set_table [start_unit_num] == NULL))
	      continue;
	    presence_p = FALSE;
	    for (pat_reserv = (final_p
			       ? unit_final_presence_set_table [start_unit_num]
			       : unit_presence_set_table [start_unit_num]);
		 pat_reserv != NULL;
		 pat_reserv = pat_reserv->next_pattern_reserv)
	      {
		for (unit_num = 0; unit_num < els_in_cycle_reserv; unit_num++)
		  if ((checked_set [unit_num] & pat_reserv->reserv [unit_num])
		      != pat_reserv->reserv [unit_num])
		    break;
		presence_p = presence_p || unit_num >= els_in_cycle_reserv;
	      }
	    if (!presence_p)
	      return FALSE;
	  }
  return TRUE;
}

/* The function checks that CHECKED_SET satisfies all absence pattern
   sets for units in ORIGIONAL_SET.  The function returns TRUE if it
   is ok.  */
static int
check_absence_pattern_sets (checked_set, origional_set, final_p)
     reserv_sets_t checked_set, origional_set;
     int final_p;
{
  int char_num;
  int chars_num;
  int i;
  int start_unit_num;
  int unit_num;
  pattern_reserv_t pat_reserv;
  
  chars_num = els_in_cycle_reserv * sizeof (set_el_t);
  for (char_num = 0; char_num < chars_num; char_num++)
    if (((unsigned char *) origional_set) [char_num])
      for (i = CHAR_BIT - 1; i >= 0; i--)
	if ((((unsigned char *) origional_set) [char_num] >> i) & 1)
	  {
	    start_unit_num = char_num * CHAR_BIT + i;
	    if (start_unit_num >= description->units_num)
	      break;
	    for (pat_reserv = (final_p
			       ? unit_final_absence_set_table [start_unit_num]
			       : unit_absence_set_table [start_unit_num]);
		 pat_reserv != NULL;
		 pat_reserv = pat_reserv->next_pattern_reserv)
	      {
		for (unit_num = 0; unit_num < els_in_cycle_reserv; unit_num++)
		  if ((checked_set [unit_num] & pat_reserv->reserv [unit_num])
		      != pat_reserv->reserv [unit_num]
		      && pat_reserv->reserv [unit_num])
		    break;
		if (unit_num >= els_in_cycle_reserv)
		  return FALSE;
	      }
	  }
  return TRUE;
}



/* This page contains code for transformation of original reservations
   described in .md file.  The main goal of transformations is
   simplifying reservation and lifting up all `|' on the top of IR
   reservation representation.  */


/* The following function makes copy of IR representation of
   reservation.  The function also substitutes all reservations
   defined by define_reservation by corresponding value during making
   the copy.  */
static regexp_t
copy_insn_regexp (regexp)
     regexp_t regexp;
{
  regexp_t  result;
  int i;

  if (regexp->mode == rm_reserv)
    result = copy_insn_regexp (REGEXP_RESERV (regexp)->reserv_decl->regexp);
  else if (regexp->mode == rm_unit)
    result = copy_node (regexp, sizeof (struct regexp));
  else if (regexp->mode == rm_repeat)
    {
      result = copy_node (regexp, sizeof (struct regexp));
      REGEXP_REPEAT (result)->regexp
        = copy_insn_regexp (REGEXP_REPEAT (regexp)->regexp);
    }
  else if (regexp->mode == rm_sequence)
    {
      result = copy_node (regexp,
                          sizeof (struct regexp) + sizeof (regexp_t)
			  * (REGEXP_SEQUENCE (regexp)->regexps_num - 1));
      for (i = 0; i <REGEXP_SEQUENCE (regexp)->regexps_num; i++)
	REGEXP_SEQUENCE (result)->regexps [i]
	  = copy_insn_regexp (REGEXP_SEQUENCE (regexp)->regexps [i]);
    }
  else if (regexp->mode == rm_allof)
    {
      result = copy_node (regexp,
                          sizeof (struct regexp) + sizeof (regexp_t)
			  * (REGEXP_ALLOF (regexp)->regexps_num - 1));
      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	REGEXP_ALLOF (result)->regexps [i]
	  = copy_insn_regexp (REGEXP_ALLOF (regexp)->regexps [i]);
    }
  else if (regexp->mode == rm_oneof)
    {
      result = copy_node (regexp,
                          sizeof (struct regexp) + sizeof (regexp_t)
			  * (REGEXP_ONEOF (regexp)->regexps_num - 1));
      for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
	REGEXP_ONEOF (result)->regexps [i]
	  = copy_insn_regexp (REGEXP_ONEOF (regexp)->regexps [i]);
    }
  else
    {
      if (regexp->mode != rm_nothing)
	abort ();
      result = copy_node (regexp, sizeof (struct regexp));
    }
  return result;
}

/* The following variable is set up 1 if a transformation has been
   applied.  */
static int regexp_transformed_p;

/* The function makes transformation
   A*N -> A, A, ...  */
static regexp_t
transform_1 (regexp)
     regexp_t regexp;
{
  int i;
  int repeat_num;
  regexp_t operand;
  pos_t pos;

  if (regexp->mode == rm_repeat)
    {
      repeat_num = REGEXP_REPEAT (regexp)->repeat_num;
      if (repeat_num <= 1)
	abort ();
      operand = REGEXP_REPEAT (regexp)->regexp;
      pos = regexp->mode;
      regexp = create_node (sizeof (struct regexp) + sizeof (regexp_t)
			    * (repeat_num - 1));
      regexp->mode = rm_sequence;
      regexp->pos = pos;
      REGEXP_SEQUENCE (regexp)->regexps_num = repeat_num;
      for (i = 0; i < repeat_num; i++)
	REGEXP_SEQUENCE (regexp)->regexps [i] = copy_insn_regexp (operand);
      regexp_transformed_p = 1;
    }
  return regexp;
}

/* The function makes transformations
   ...,(A,B,...),C,... -> ...,A,B,...,C,...
   ...+(A+B+...)+C+... -> ...+A+B+...+C+...
   ...|(A|B|...)|C|... -> ...|A|B|...|C|...  */
static regexp_t
transform_2 (regexp)
     regexp_t regexp;
{
  if (regexp->mode == rm_sequence)
    {
      regexp_t sequence = NULL;
      regexp_t result;
      int sequence_index = 0;
      int i, j;

      for (i = 0; i < REGEXP_SEQUENCE (regexp)->regexps_num; i++)
	if (REGEXP_SEQUENCE (regexp)->regexps [i]->mode == rm_sequence)
	  {
	    sequence_index = i;
	    sequence = REGEXP_SEQUENCE (regexp)->regexps [i];
	    break;
	  }
      if (i < REGEXP_SEQUENCE (regexp)->regexps_num)
	{
	  if ( REGEXP_SEQUENCE (sequence)->regexps_num <= 1
	      || REGEXP_SEQUENCE (regexp)->regexps_num <= 1)
	    abort ();
	  result = create_node (sizeof (struct regexp)
                                + sizeof (regexp_t)
				* (REGEXP_SEQUENCE (regexp)->regexps_num
                                   + REGEXP_SEQUENCE (sequence)->regexps_num
                                   - 2));
	  result->mode = rm_sequence;
	  result->pos = regexp->pos;
	  REGEXP_SEQUENCE (result)->regexps_num
            = (REGEXP_SEQUENCE (regexp)->regexps_num
               + REGEXP_SEQUENCE (sequence)->regexps_num - 1);
	  for (i = 0; i < REGEXP_SEQUENCE (regexp)->regexps_num; i++)
            if (i < sequence_index)
              REGEXP_SEQUENCE (result)->regexps [i]
                = copy_insn_regexp (REGEXP_SEQUENCE (regexp)->regexps [i]);
            else if (i > sequence_index)
              REGEXP_SEQUENCE (result)->regexps
                [i + REGEXP_SEQUENCE (sequence)->regexps_num - 1]
                = copy_insn_regexp (REGEXP_SEQUENCE (regexp)->regexps [i]);
            else
              for (j = 0; j < REGEXP_SEQUENCE (sequence)->regexps_num; j++)
                REGEXP_SEQUENCE (result)->regexps [i + j]
                  = copy_insn_regexp (REGEXP_SEQUENCE (sequence)->regexps [j]);
	  regexp_transformed_p = 1;
	  regexp = result;
	}
    }
  else if (regexp->mode == rm_allof)
    {
      regexp_t allof = NULL;
      regexp_t result;
      int allof_index = 0;
      int i, j;

      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	if (REGEXP_ALLOF (regexp)->regexps [i]->mode == rm_allof)
	  {
	    allof_index = i;
	    allof = REGEXP_ALLOF (regexp)->regexps [i];
	    break;
	  }
      if (i < REGEXP_ALLOF (regexp)->regexps_num)
	{
	  if (REGEXP_ALLOF (allof)->regexps_num <= 1
	      || REGEXP_ALLOF (regexp)->regexps_num <= 1)
	    abort ();
	  result = create_node (sizeof (struct regexp)
                                + sizeof (regexp_t)
				* (REGEXP_ALLOF (regexp)->regexps_num
                                   + REGEXP_ALLOF (allof)->regexps_num - 2));
	  result->mode = rm_allof;
	  result->pos = regexp->pos;
	  REGEXP_ALLOF (result)->regexps_num
            = (REGEXP_ALLOF (regexp)->regexps_num
               + REGEXP_ALLOF (allof)->regexps_num - 1);
	  for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
            if (i < allof_index)
              REGEXP_ALLOF (result)->regexps [i]
                = copy_insn_regexp (REGEXP_ALLOF (regexp)->regexps [i]);
            else if (i > allof_index)
              REGEXP_ALLOF (result)->regexps
                [i + REGEXP_ALLOF (allof)->regexps_num - 1]
                = copy_insn_regexp (REGEXP_ALLOF (regexp)->regexps [i]);
            else
              for (j = 0; j < REGEXP_ALLOF (allof)->regexps_num; j++)
                REGEXP_ALLOF (result)->regexps [i + j]
                  = copy_insn_regexp (REGEXP_ALLOF (allof)->regexps [j]);
	  regexp_transformed_p = 1;
	  regexp = result;
	}
    }
  else if (regexp->mode == rm_oneof)
    {
      regexp_t oneof = NULL;
      regexp_t result;
      int oneof_index = 0;
      int i, j;

      for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
	if (REGEXP_ONEOF (regexp)->regexps [i]->mode == rm_oneof)
	  {
	    oneof_index = i;
	    oneof = REGEXP_ONEOF (regexp)->regexps [i];
	    break;
	  }
      if (i < REGEXP_ONEOF (regexp)->regexps_num)
	{
	  if (REGEXP_ONEOF (oneof)->regexps_num <= 1
	      || REGEXP_ONEOF (regexp)->regexps_num <= 1)
	    abort ();
	  result = create_node (sizeof (struct regexp)
				+ sizeof (regexp_t)
				* (REGEXP_ONEOF (regexp)->regexps_num
                                   + REGEXP_ONEOF (oneof)->regexps_num - 2));
	  result->mode = rm_oneof;
	  result->pos = regexp->pos;
	  REGEXP_ONEOF (result)->regexps_num
	    = (REGEXP_ONEOF (regexp)->regexps_num
               + REGEXP_ONEOF (oneof)->regexps_num - 1);
	  for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
            if (i < oneof_index)
              REGEXP_ONEOF (result)->regexps [i]
                = copy_insn_regexp (REGEXP_ONEOF (regexp)->regexps [i]);
            else if (i > oneof_index)
              REGEXP_ONEOF (result)->regexps
                [i + REGEXP_ONEOF (oneof)->regexps_num - 1]
                = copy_insn_regexp (REGEXP_ONEOF (regexp)->regexps [i]);
            else
              for (j = 0; j < REGEXP_ONEOF (oneof)->regexps_num; j++)
                REGEXP_ONEOF (result)->regexps [i + j]
                  = copy_insn_regexp (REGEXP_ONEOF (oneof)->regexps [j]);
	  regexp_transformed_p = 1;
	  regexp = result;
	}
    }
  return regexp;
}

/* The function makes transformations
   ...,A|B|...,C,... -> (...,A,C,...)|(...,B,C,...)|...
   ...+(A|B|...)+C+... -> (...+A+C+...)|(...+B+C+...)|...
   ...+(A,B,...)+C+... -> (...+A+C+...),B,...
   ...+(A,B,...)+(C,D,...) -> (A+C),(B+D),...  */
static regexp_t
transform_3 (regexp)
     regexp_t regexp;
{
  if (regexp->mode == rm_sequence)
    {
      regexp_t oneof = NULL;
      int oneof_index = 0;
      regexp_t result;
      regexp_t sequence;
      int i, j;

      for (i = 0; i <REGEXP_SEQUENCE (regexp)->regexps_num; i++)
	if (REGEXP_SEQUENCE (regexp)->regexps [i]->mode == rm_oneof)
	  {
	    oneof_index = i;
	    oneof = REGEXP_SEQUENCE (regexp)->regexps [i];
	    break;
	  }
      if (i < REGEXP_SEQUENCE (regexp)->regexps_num)
	{
	  if (REGEXP_ONEOF (oneof)->regexps_num <= 1
	      || REGEXP_SEQUENCE (regexp)->regexps_num <= 1)
	    abort ();
	  result = create_node (sizeof (struct regexp)
				+ sizeof (regexp_t)
				* (REGEXP_ONEOF (oneof)->regexps_num - 1));
	  result->mode = rm_oneof;
	  result->pos = regexp->pos;
	  REGEXP_ONEOF (result)->regexps_num
	    = REGEXP_ONEOF (oneof)->regexps_num;
	  for (i = 0; i < REGEXP_ONEOF (result)->regexps_num; i++)
	    {
	      sequence
                = create_node (sizeof (struct regexp)
                               + sizeof (regexp_t)
                               * (REGEXP_SEQUENCE (regexp)->regexps_num - 1));
	      sequence->mode = rm_sequence;
	      sequence->pos = regexp->pos;
	      REGEXP_SEQUENCE (sequence)->regexps_num
                = REGEXP_SEQUENCE (regexp)->regexps_num;
              REGEXP_ONEOF (result)->regexps [i] = sequence;
	      for (j = 0; j < REGEXP_SEQUENCE (sequence)->regexps_num; j++)
		if (j != oneof_index)
		  REGEXP_SEQUENCE (sequence)->regexps [j]
		    = copy_insn_regexp (REGEXP_SEQUENCE (regexp)->regexps [j]);
		else
		  REGEXP_SEQUENCE (sequence)->regexps [j]
		    = copy_insn_regexp (REGEXP_ONEOF (oneof)->regexps [i]);
	    }
	  regexp_transformed_p = 1;
	  regexp = result;
	}
    }
  else if (regexp->mode == rm_allof)
    {
      regexp_t oneof = NULL;
      regexp_t seq;
      int oneof_index = 0;
      int max_seq_length, allof_length;
      regexp_t result;
      regexp_t allof = NULL;
      regexp_t allof_op = NULL;
      int i, j;

      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	if (REGEXP_ALLOF (regexp)->regexps [i]->mode == rm_oneof)
	  {
	    oneof_index = i;
	    oneof = REGEXP_ALLOF (regexp)->regexps [i];
	    break;
	  }
      if (i < REGEXP_ALLOF (regexp)->regexps_num)
	{
	  if (REGEXP_ONEOF (oneof)->regexps_num <= 1
	      || REGEXP_ALLOF (regexp)->regexps_num <= 1)
	    abort ();
	  result = create_node (sizeof (struct regexp)
				+ sizeof (regexp_t)
				* (REGEXP_ONEOF (oneof)->regexps_num - 1));
	  result->mode = rm_oneof;
	  result->pos = regexp->pos;
	  REGEXP_ONEOF (result)->regexps_num
	    = REGEXP_ONEOF (oneof)->regexps_num;
	  for (i = 0; i < REGEXP_ONEOF (result)->regexps_num; i++)
	    {
	      allof
		= create_node (sizeof (struct regexp)
                               + sizeof (regexp_t)
			       * (REGEXP_ALLOF (regexp)->regexps_num - 1));
	      allof->mode = rm_allof;
	      allof->pos = regexp->pos;
	      REGEXP_ALLOF (allof)->regexps_num
                = REGEXP_ALLOF (regexp)->regexps_num;
              REGEXP_ONEOF (result)->regexps [i] = allof;
	      for (j = 0; j < REGEXP_ALLOF (allof)->regexps_num; j++)
		if (j != oneof_index)
		  REGEXP_ALLOF (allof)->regexps [j]
		    = copy_insn_regexp (REGEXP_ALLOF (regexp)->regexps [j]);
		else
		  REGEXP_ALLOF (allof)->regexps [j]
		    = copy_insn_regexp (REGEXP_ONEOF (oneof)->regexps [i]);
	    }
	  regexp_transformed_p = 1;
	  regexp = result;
	}
      max_seq_length = 0;
      if (regexp->mode == rm_allof)
	for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	  {
	    if (REGEXP_ALLOF (regexp)->regexps [i]->mode == rm_sequence)
	      {
		seq = REGEXP_ALLOF (regexp)->regexps [i];
		if (max_seq_length < REGEXP_SEQUENCE (seq)->regexps_num)
		  max_seq_length = REGEXP_SEQUENCE (seq)->regexps_num;
	      }
	    else if (REGEXP_ALLOF (regexp)->regexps [i]->mode != rm_unit
		     && REGEXP_ALLOF (regexp)->regexps [i]->mode != rm_nothing)
	      {
		max_seq_length = 0;
		break;
	      }
	  }
      if (max_seq_length != 0)
	{
	  if (max_seq_length == 1 || REGEXP_ALLOF (regexp)->regexps_num <= 1)
	    abort ();
	  result = create_node (sizeof (struct regexp)
				+ sizeof (regexp_t) * (max_seq_length - 1));
	  result->mode = rm_sequence;
	  result->pos = regexp->pos;
	  REGEXP_SEQUENCE (result)->regexps_num = max_seq_length;
	  for (i = 0; i < max_seq_length; i++)
	    {
	      allof_length = 0;
	      for (j = 0; j < REGEXP_ALLOF (regexp)->regexps_num; j++)
		if (REGEXP_ALLOF (regexp)->regexps [j]->mode == rm_sequence
		    && (i < (REGEXP_SEQUENCE (REGEXP_ALLOF (regexp)
					      ->regexps [j])->regexps_num)))
		  {
		    allof_op
		      = (REGEXP_SEQUENCE (REGEXP_ALLOF (regexp)->regexps [j])
			 ->regexps [i]);
		    allof_length++;
		  }
		else if (i == 0
			 && (REGEXP_ALLOF (regexp)->regexps [j]->mode
			     == rm_unit
			     || (REGEXP_ALLOF (regexp)->regexps [j]->mode
				 == rm_nothing)))
		  {
		    allof_op = REGEXP_ALLOF (regexp)->regexps [j];
		    allof_length++;
		  }
	      if (allof_length == 1)
		REGEXP_SEQUENCE (result)->regexps [i] = allof_op;
	      else
		{
		  allof = create_node (sizeof (struct regexp)
				       + sizeof (regexp_t)
				       * (allof_length - 1));
		  allof->mode = rm_allof;
		  allof->pos = regexp->pos;
		  REGEXP_ALLOF (allof)->regexps_num = allof_length;
		  REGEXP_SEQUENCE (result)->regexps [i] = allof;
		  allof_length = 0;
		  for (j = 0; j < REGEXP_ALLOF (regexp)->regexps_num; j++)
		    if (REGEXP_ALLOF (regexp)->regexps [j]->mode == rm_sequence
			&& (i <
			    (REGEXP_SEQUENCE (REGEXP_ALLOF (regexp)
					      ->regexps [j])->regexps_num)))
		      {
			allof_op = (REGEXP_SEQUENCE (REGEXP_ALLOF (regexp)
						     ->regexps [j])
				    ->regexps [i]);
			REGEXP_ALLOF (allof)->regexps [allof_length]
			  = allof_op;
			allof_length++;
		      }
		    else if (i == 0
			     && (REGEXP_ALLOF (regexp)->regexps [j]->mode
				 == rm_unit
				 || (REGEXP_ALLOF (regexp)->regexps [j]->mode
				     == rm_nothing)))
		      {
			allof_op = REGEXP_ALLOF (regexp)->regexps [j];
			REGEXP_ALLOF (allof)->regexps [allof_length]
			  = allof_op;
			allof_length++;
		      }
		}
	    }
	  regexp_transformed_p = 1;
	  regexp = result;
	}
    }
  return regexp;
}

/* The function traverses IR of reservation and applies transformations
   implemented by FUNC.  */
static regexp_t
regexp_transform_func (regexp, func)
     regexp_t regexp;
     regexp_t (*func) PARAMS ((regexp_t regexp));
{
  int i;

  if (regexp->mode == rm_sequence)
    for (i = 0; i < REGEXP_SEQUENCE (regexp)->regexps_num; i++)
      REGEXP_SEQUENCE (regexp)->regexps [i]
	= regexp_transform_func (REGEXP_SEQUENCE (regexp)->regexps [i], func);
  else if (regexp->mode == rm_allof)
    for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
      REGEXP_ALLOF (regexp)->regexps [i]
	= regexp_transform_func (REGEXP_ALLOF (regexp)->regexps [i], func);
  else if (regexp->mode == rm_oneof)
    for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
      REGEXP_ONEOF (regexp)->regexps [i]
	= regexp_transform_func (REGEXP_ONEOF (regexp)->regexps [i], func);
  else if (regexp->mode == rm_repeat)
    REGEXP_REPEAT (regexp)->regexp
      = regexp_transform_func (REGEXP_REPEAT (regexp)->regexp, func);
  else if (regexp->mode != rm_nothing && regexp->mode != rm_unit)
    abort ();
  return (*func) (regexp);
}

/* The function applies all transformations for IR representation of
   reservation REGEXP.  */
static regexp_t
transform_regexp (regexp)
     regexp_t regexp;
{
  regexp = regexp_transform_func (regexp, transform_1);  
  do
    {
      regexp_transformed_p = 0;
      regexp = regexp_transform_func (regexp, transform_2);
      regexp = regexp_transform_func (regexp, transform_3);
    }
  while (regexp_transformed_p);
  return regexp;
}

/* The function applies all transformations for reservations of all
   insn declarations.  */
static void
transform_insn_regexps ()
{
  decl_t decl;
  int i;

  transform_time = create_ticker ();
  add_advance_cycle_insn_decl ();
  fprintf (stderr, "Reservation transformation...");
  fflush (stderr);
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
	DECL_INSN_RESERV (decl)->transformed_regexp
	  = transform_regexp (copy_insn_regexp
			      (DECL_INSN_RESERV (decl)->regexp));
    }
  fprintf (stderr, "done\n");
  ticker_off (&transform_time);
  fflush (stderr);
}



/* The following variable value is TRUE if the first annotated message
   about units to automata distribution has been output.  */
static int annotation_message_reported_p;

/* The following structure describes usage of a unit in a reservation.  */
struct unit_usage
{
  unit_decl_t unit_decl;
  /* The following forms a list of units used on the same cycle in the
     same alternative.  */
  struct unit_usage *next;
};

/* Obstack for unit_usage structures.  */
static struct obstack unit_usages;

/* VLA for representation of array of pointers to unit usage
   structures.  There is an element for each combination of
   (alternative number, cycle).  Unit usages on given cycle in
   alternative with given number are referred through element with
   index equals to the cycle * number of all alternatives in the regexp
   + the alternative number.  */
static vla_ptr_t cycle_alt_unit_usages;

/* The following function creates the structure unit_usage for UNIT on
   CYCLE in REGEXP alternative with ALT_NUM.  The structure is made
   accessed through cycle_alt_unit_usages.  */
static void
store_alt_unit_usage (regexp, unit, cycle, alt_num)
     regexp_t regexp;
     regexp_t unit;
     int cycle;
     int alt_num;
{
  size_t i, length, old_length;
  unit_decl_t unit_decl;
  struct unit_usage *unit_usage_ptr;
  int index;

  if (regexp == NULL || regexp->mode != rm_oneof
      || alt_num >= REGEXP_ONEOF (regexp)->regexps_num)
    abort ();
  unit_decl = REGEXP_UNIT (unit)->unit_decl;
  old_length = VLA_PTR_LENGTH (cycle_alt_unit_usages);
  length = (cycle + 1) * REGEXP_ONEOF (regexp)->regexps_num;
  if (old_length < length)
    {
      VLA_PTR_EXPAND (cycle_alt_unit_usages, length - old_length);
      for (i = old_length; i < length; i++)
	VLA_PTR (cycle_alt_unit_usages, i) = NULL;
    }
  obstack_blank (&unit_usages, sizeof (struct unit_usage));
  unit_usage_ptr = (struct unit_usage *) obstack_base (&unit_usages);
  obstack_finish (&unit_usages);
  unit_usage_ptr->unit_decl = unit_decl;
  index = cycle * REGEXP_ONEOF (regexp)->regexps_num + alt_num;
  unit_usage_ptr->next = VLA_PTR (cycle_alt_unit_usages, index);
  VLA_PTR (cycle_alt_unit_usages, index) = unit_usage_ptr;
  unit_decl->last_distribution_check_cycle = -1; /* undefined */
}

/* The function processes given REGEXP to find units with the wrong
   distribution.  */
static void
check_regexp_units_distribution (insn_reserv_name, regexp)
     const char *insn_reserv_name;
     regexp_t regexp;
{
  int i, j, k, cycle;
  regexp_t seq, allof, unit;
  struct unit_usage *unit_usage_ptr, *other_unit_usage_ptr;

  if (regexp == NULL || regexp->mode != rm_oneof)
    return;
  /* Store all unit usages in the regexp:  */
  obstack_init (&unit_usages);
  VLA_PTR_CREATE (cycle_alt_unit_usages, 100, "unit usages on cycles");
  for (i = REGEXP_ONEOF (regexp)->regexps_num - 1; i >= 0; i--)
    {
      seq = REGEXP_ONEOF (regexp)->regexps [i];
      if (seq->mode == rm_sequence)
	for (j = 0; j < REGEXP_SEQUENCE (seq)->regexps_num; j++)
	  {
	    allof = REGEXP_SEQUENCE (seq)->regexps [j];
	    if (allof->mode == rm_allof)
	      for (k = 0; k < REGEXP_ALLOF (allof)->regexps_num; k++)
		{
		  unit = REGEXP_ALLOF (allof)->regexps [k];
		  if (unit->mode == rm_unit)
		    store_alt_unit_usage (regexp, unit, j, i);
		  else if (unit->mode != rm_nothing)
		    abort ();
		}
	    else if (allof->mode == rm_unit)
	      store_alt_unit_usage (regexp, allof, j, i);
	    else if (allof->mode != rm_nothing)
	      abort ();
	  }
      else if (seq->mode == rm_allof)
	for (k = 0; k < REGEXP_ALLOF (seq)->regexps_num; k++)
	  {
	    unit = REGEXP_ALLOF (seq)->regexps [k];
	    if (unit->mode == rm_unit)
	      store_alt_unit_usage (regexp, unit, 0, i);
	    else if (unit->mode != rm_nothing)
	      abort ();
	  }
      else if (seq->mode == rm_unit)
	store_alt_unit_usage (regexp, seq, 0, i);
      else if (seq->mode != rm_nothing)
	abort ();
    }
  /* Check distribution:  */
  for (i = 0; i < (int) VLA_PTR_LENGTH (cycle_alt_unit_usages); i++)
    {
      cycle = i / REGEXP_ONEOF (regexp)->regexps_num;
      for (unit_usage_ptr = VLA_PTR (cycle_alt_unit_usages, i);
	   unit_usage_ptr != NULL;
	   unit_usage_ptr = unit_usage_ptr->next)
	if (cycle != unit_usage_ptr->unit_decl->last_distribution_check_cycle)
	  {
	    unit_usage_ptr->unit_decl->last_distribution_check_cycle = cycle;
	    for (k = cycle * REGEXP_ONEOF (regexp)->regexps_num;
		 k < (int) VLA_PTR_LENGTH (cycle_alt_unit_usages)
		   && k == cycle * REGEXP_ONEOF (regexp)->regexps_num;
		 k++)
	      {
		for (other_unit_usage_ptr = VLA_PTR (cycle_alt_unit_usages, k);
		     other_unit_usage_ptr != NULL;
		     other_unit_usage_ptr = other_unit_usage_ptr->next)
		  if (unit_usage_ptr->unit_decl->automaton_decl
		      == other_unit_usage_ptr->unit_decl->automaton_decl)
		    break;
		if (other_unit_usage_ptr == NULL
		    && VLA_PTR (cycle_alt_unit_usages, k) != NULL)
		  break;
	      }
	    if (k < (int) VLA_PTR_LENGTH (cycle_alt_unit_usages)
		&& k == cycle * REGEXP_ONEOF (regexp)->regexps_num)
	      {
		if (!annotation_message_reported_p)
		  {
		    fprintf (stderr, "\n");
		    error ("The following units do not satisfy units-automata distribution rule");
		    error (" (A unit of given unit automaton should be on each reserv. altern.)");
		    annotation_message_reported_p = TRUE;
		  }
		error ("Unit %s, reserv. %s, cycle %d",
		       unit_usage_ptr->unit_decl->name, insn_reserv_name,
		       cycle);
	      }
	  }
    }
  VLA_PTR_DELETE (cycle_alt_unit_usages);
  obstack_free (&unit_usages, NULL);
}

/* The function finds units which violates units to automata
   distribution rule.  If the units exist, report about them.  */
static void
check_unit_distributions_to_automata ()
{
  decl_t decl;
  int i;

  fprintf (stderr, "Check unit distributions to automata...");
  annotation_message_reported_p = FALSE;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
	check_regexp_units_distribution
	  (DECL_INSN_RESERV (decl)->name,
	   DECL_INSN_RESERV (decl)->transformed_regexp);
    }
  fprintf (stderr, "done\n");
}



/* The page contains code for building alt_states (see comments for
   IR) describing all possible insns reservations of an automaton.  */

/* Current state being formed for which the current alt_state
   refers.  */
static state_t state_being_formed;

/* Current alt_state being formed.  */
static alt_state_t alt_state_being_formed;
 
/* This recursive function processes `,' and units in reservation
   REGEXP for forming alt_states of AUTOMATON.  It is believed that
   CURR_CYCLE is start cycle of all reservation REGEXP.  */
static int
process_seq_for_forming_states (regexp, automaton, curr_cycle)
     regexp_t regexp;
     automaton_t automaton;
     int curr_cycle;
{
  int i;

  if (regexp == NULL)
    return curr_cycle;
  else if (regexp->mode == rm_unit)
    {
      if (REGEXP_UNIT (regexp)->unit_decl->corresponding_automaton_num
          == automaton->automaton_order_num)
        set_state_reserv (state_being_formed, curr_cycle,
                          REGEXP_UNIT (regexp)->unit_decl->unit_num);
      return curr_cycle;
    }
  else if (regexp->mode == rm_sequence)
    {
      for (i = 0; i < REGEXP_SEQUENCE (regexp)->regexps_num; i++)
	curr_cycle
	  = process_seq_for_forming_states
	    (REGEXP_SEQUENCE (regexp)->regexps [i], automaton, curr_cycle) + 1;
      return curr_cycle;
    }
  else if (regexp->mode == rm_allof)
    {
      int finish_cycle = 0;
      int cycle;

      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	{
	  cycle = process_seq_for_forming_states (REGEXP_ALLOF (regexp)
						  ->regexps [i],
						  automaton, curr_cycle);
	  if (finish_cycle < cycle)
	    finish_cycle = cycle;
	}
      return finish_cycle;
    }
  else
    {
      if (regexp->mode != rm_nothing)
	abort ();
      return curr_cycle;
    }
}

/* This recursive function finishes forming ALT_STATE of AUTOMATON and
   inserts alt_state into the table.  */
static void
finish_forming_alt_state (alt_state, automaton)
     alt_state_t alt_state;
     automaton_t automaton ATTRIBUTE_UNUSED;
{
  state_t state_in_table;
  state_t corresponding_state;

  corresponding_state = alt_state->state;
  state_in_table = insert_state (corresponding_state);
  if (state_in_table != corresponding_state)
    {
      free_state (corresponding_state);
      alt_state->state = state_in_table;
    }
}

/* The following variable value is current automaton insn for whose
   reservation the alt states are created.  */
static ainsn_t curr_ainsn;

/* This recursive function processes `|' in reservation REGEXP for
   forming alt_states of AUTOMATON.  List of the alt states should
   have the same order as in the description.  */
static void
process_alts_for_forming_states (regexp, automaton, inside_oneof_p)
     regexp_t regexp;
     automaton_t automaton;
     int inside_oneof_p;
{
  int i;

  if (regexp->mode != rm_oneof)
    {
      alt_state_being_formed = get_free_alt_state ();
      state_being_formed = get_free_state (1, automaton);
      alt_state_being_formed->state = state_being_formed;
      /* We inserts in reverse order but we process alternatives also
         in reverse order.  So we have the same order of alternative
         as in the description.  */
      alt_state_being_formed->next_alt_state = curr_ainsn->alt_states;
      curr_ainsn->alt_states = alt_state_being_formed;
      (void) process_seq_for_forming_states (regexp, automaton, 0);
      finish_forming_alt_state (alt_state_being_formed, automaton);
    }
  else
    {
      if (inside_oneof_p)
	abort ();
      /* We processes it in reverse order to get list with the same
	 order as in the description.  See also the previous
	 commentary.  */
      for (i = REGEXP_ONEOF (regexp)->regexps_num - 1; i >= 0; i--)
	process_alts_for_forming_states (REGEXP_ONEOF (regexp)->regexps [i],
					 automaton, 1);
    }
}

/* Create nodes alt_state for all AUTOMATON insns.  */
static void
create_alt_states (automaton)
     automaton_t automaton;
{
  struct insn_reserv_decl *reserv_decl;

  for (curr_ainsn = automaton->ainsn_list;
       curr_ainsn != NULL;
       curr_ainsn = curr_ainsn->next_ainsn)
    {
      reserv_decl = curr_ainsn->insn_reserv_decl;
      if (reserv_decl != DECL_INSN_RESERV (advance_cycle_insn_decl))
        {
          curr_ainsn->alt_states = NULL;
          process_alts_for_forming_states (reserv_decl->transformed_regexp,
					   automaton, 0);
          curr_ainsn->sorted_alt_states
	    = uniq_sort_alt_states (curr_ainsn->alt_states);
        }
    }
}



/* The page contains major code for building DFA(s) for fast pipeline
   hazards recognition.  */

/* The function forms list of ainsns of AUTOMATON with the same
   reservation.  */
static void
form_ainsn_with_same_reservs (automaton)
     automaton_t automaton;
{
  ainsn_t curr_ainsn;
  size_t i;
  vla_ptr_t first_insns;
  vla_ptr_t last_insns;

  VLA_PTR_CREATE (first_insns, 150, "first insns with the same reservs");
  VLA_PTR_CREATE (last_insns, 150, "last insns with the same reservs");
  for (curr_ainsn = automaton->ainsn_list;
       curr_ainsn != NULL;
       curr_ainsn = curr_ainsn->next_ainsn)
    if (curr_ainsn->insn_reserv_decl
	== DECL_INSN_RESERV (advance_cycle_insn_decl))
      {
        curr_ainsn->next_same_reservs_insn = NULL;
        curr_ainsn->first_insn_with_same_reservs = 1;
      }
    else
      {
        for (i = 0; i < VLA_PTR_LENGTH (first_insns); i++)
          if (alt_states_eq
              (curr_ainsn->sorted_alt_states,
               ((ainsn_t) VLA_PTR (first_insns, i))->sorted_alt_states))
            break;
        curr_ainsn->next_same_reservs_insn = NULL;
        if (i < VLA_PTR_LENGTH (first_insns))
          {
            curr_ainsn->first_insn_with_same_reservs = 0;
	    ((ainsn_t) VLA_PTR (last_insns, i))->next_same_reservs_insn
	      = curr_ainsn;
            VLA_PTR (last_insns, i) = curr_ainsn;
          }
        else
          {
            VLA_PTR_ADD (first_insns, curr_ainsn);
            VLA_PTR_ADD (last_insns, curr_ainsn);
            curr_ainsn->first_insn_with_same_reservs = 1;
          }
      }
  VLA_PTR_DELETE (first_insns);
  VLA_PTR_DELETE (last_insns);
}

/* Forming unit reservations which can affect creating the automaton
   states achieved from a given state.  It permits to build smaller
   automata in many cases.  We would have the same automata after
   the minimization without such optimization, but the automaton
   right after the building could be huge.  So in other words, usage
   of reservs_matter means some minimization during building the
   automaton.  */
static reserv_sets_t
form_reservs_matter (automaton)
     automaton_t automaton;
{
  int cycle, unit;
  reserv_sets_t reservs_matter = alloc_empty_reserv_sets();

  for (cycle = 0; cycle < max_cycles_num; cycle++)
    for (unit = 0; unit < description->units_num; unit++)
      if (units_array [unit]->automaton_decl
	  == automaton->corresponding_automaton_decl
	  && (cycle >= units_array [unit]->min_occ_cycle_num
	      /* We can not remove queried unit from reservations.  */
	      || units_array [unit]->query_p
	      /* We can not remove units which are used
		 `exclusion_set', `presence_set',
		 `final_presence_set', `absence_set', and
		 `final_absence_set'.  */
	      || units_array [unit]->in_set_p))
	set_unit_reserv (reservs_matter, cycle, unit);
  return reservs_matter;
}

/* The following function creates all states of nondeterministic (if
   NDFA_FLAG has nonzero value) or deterministic AUTOMATON.  */
static void
make_automaton (automaton)
     automaton_t automaton;
{
  ainsn_t ainsn;
  struct insn_reserv_decl *insn_reserv_decl;
  alt_state_t alt_state;
  state_t state;
  state_t start_state;
  state_t state2;
  ainsn_t advance_cycle_ainsn;
  arc_t added_arc;
  vla_ptr_t state_stack;
  int states_n;
  reserv_sets_t reservs_matter = form_reservs_matter (automaton);

  VLA_PTR_CREATE (state_stack, 150, "state stack");
  /* Create the start state (empty state).  */
  start_state = insert_state (get_free_state (1, automaton));
  automaton->start_state = start_state;
  start_state->it_was_placed_in_stack_for_NDFA_forming = 1;
  VLA_PTR_ADD (state_stack, start_state);
  states_n = 1;
  while (VLA_PTR_LENGTH (state_stack) != 0)
    {
      state = VLA_PTR (state_stack, VLA_PTR_LENGTH (state_stack) - 1);
      VLA_PTR_SHORTEN (state_stack, 1);
      advance_cycle_ainsn = NULL;
      for (ainsn = automaton->ainsn_list;
	   ainsn != NULL;
	   ainsn = ainsn->next_ainsn)
        if (ainsn->first_insn_with_same_reservs)
          {
            insn_reserv_decl = ainsn->insn_reserv_decl;
            if (insn_reserv_decl != DECL_INSN_RESERV (advance_cycle_insn_decl))
              {
		/* We process alt_states in the same order as they are
                   present in the description.  */
		added_arc = NULL;
                for (alt_state = ainsn->alt_states;
                     alt_state != NULL;
                     alt_state = alt_state->next_alt_state)
                  {
                    state2 = alt_state->state;
                    if (!intersected_state_reservs_p (state, state2))
                      {
                        state2 = states_union (state, state2, reservs_matter);
                        if (!state2->it_was_placed_in_stack_for_NDFA_forming)
                          {
                            state2->it_was_placed_in_stack_for_NDFA_forming
			      = 1;
                            VLA_PTR_ADD (state_stack, state2);
			    states_n++;
			    if (states_n % 100 == 0)
			      fprintf (stderr, "*");
                          }
			added_arc = add_arc (state, state2, ainsn, 1);
			if (!ndfa_flag)
			  break;
                      }
                  }
		if (!ndfa_flag && added_arc != NULL)
		  {
		    added_arc->state_alts = 0;
		    for (alt_state = ainsn->alt_states;
			 alt_state != NULL;
			 alt_state = alt_state->next_alt_state)
		      {
			state2 = alt_state->state;
			if (!intersected_state_reservs_p (state, state2))
			  added_arc->state_alts++;
		      }
		  }
              }
            else
              advance_cycle_ainsn = ainsn;
          }
      /* Add transition to advance cycle.  */
      state2 = state_shift (state, reservs_matter);
      if (!state2->it_was_placed_in_stack_for_NDFA_forming)
        {
          state2->it_was_placed_in_stack_for_NDFA_forming = 1;
          VLA_PTR_ADD (state_stack, state2);
	  states_n++;
	  if (states_n % 100 == 0)
	    fprintf (stderr, "*");
        }
      if (advance_cycle_ainsn == NULL)
	abort ();
      add_arc (state, state2, advance_cycle_ainsn, 1);
    }
  VLA_PTR_DELETE (state_stack);
}

/* Foms lists of all arcs of STATE marked by the same ainsn.  */
static void
form_arcs_marked_by_insn (state)
     state_t state;
{
  decl_t decl;
  arc_t arc;
  int i;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
	DECL_INSN_RESERV (decl)->arcs_marked_by_insn = NULL;
    }
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    {
      if (arc->insn == NULL)
	abort ();
      arc->next_arc_marked_by_insn
	= arc->insn->insn_reserv_decl->arcs_marked_by_insn;
      arc->insn->insn_reserv_decl->arcs_marked_by_insn = arc;
    }
}

/* The function creates composed state (see comments for IR) from
   ORIGINAL_STATE and list of arcs ARCS_MARKED_BY_INSN marked by the
   same insn.  If the composed state is not in STATE_STACK yet, it is
   pushed into STATE_STACK.  */
static int
create_composed_state (original_state, arcs_marked_by_insn, state_stack)
     state_t original_state;
     arc_t arcs_marked_by_insn;
     vla_ptr_t *state_stack;
{
  state_t state;
  alt_state_t alt_state, curr_alt_state;
  alt_state_t new_alt_state;
  arc_t curr_arc;
  arc_t next_arc;
  state_t state_in_table;
  state_t temp_state;
  alt_state_t canonical_alt_states_list;
  int alts_number;
  int new_state_p = 0;

  if (arcs_marked_by_insn == NULL)
    return new_state_p;
  if (arcs_marked_by_insn->next_arc_marked_by_insn == NULL)
    state = arcs_marked_by_insn->to_state;
  else
    {
      if (!ndfa_flag)
	abort ();
      /* Create composed state.  */
      state = get_free_state (0, arcs_marked_by_insn->to_state->automaton);
      curr_alt_state = NULL;
      for (curr_arc = arcs_marked_by_insn;
           curr_arc != NULL;
           curr_arc = curr_arc->next_arc_marked_by_insn)
	if (curr_arc->to_state->component_states == NULL)
	  {
	    new_alt_state = get_free_alt_state ();
	    new_alt_state->next_alt_state = curr_alt_state;
	    new_alt_state->state = curr_arc->to_state;
	    curr_alt_state = new_alt_state;
	  }
	else
	  for (alt_state = curr_arc->to_state->component_states;
	       alt_state != NULL;
	       alt_state = alt_state->next_sorted_alt_state)
	    {
	      new_alt_state = get_free_alt_state ();
	      new_alt_state->next_alt_state = curr_alt_state;
	      new_alt_state->state = alt_state->state;
	      if (alt_state->state->component_states != NULL)
		abort ();
	      curr_alt_state = new_alt_state;
	    }
      /* There are not identical sets in the alt state list.  */
      canonical_alt_states_list = uniq_sort_alt_states (curr_alt_state);
      if (canonical_alt_states_list->next_sorted_alt_state == NULL)
        {
          temp_state = state;
          state = canonical_alt_states_list->state;
          free_state (temp_state);
        }
      else
        {
          state->component_states = canonical_alt_states_list;
          state_in_table = insert_state (state);
          if (state_in_table != state)
            {
              if (!state_in_table->it_was_placed_in_stack_for_DFA_forming)
		abort ();
              free_state (state);
              state = state_in_table;
            }
          else
            {
              if (state->it_was_placed_in_stack_for_DFA_forming)
		abort ();
	      new_state_p = 1;
              for (curr_alt_state = state->component_states;
                   curr_alt_state != NULL;
                   curr_alt_state = curr_alt_state->next_sorted_alt_state)
                for (curr_arc = first_out_arc (curr_alt_state->state);
                     curr_arc != NULL;
                     curr_arc = next_out_arc (curr_arc))
		  add_arc (state, curr_arc->to_state, curr_arc->insn, 1);
            }
          arcs_marked_by_insn->to_state = state;
          for (alts_number = 0,
	       curr_arc = arcs_marked_by_insn->next_arc_marked_by_insn;
               curr_arc != NULL;
               curr_arc = next_arc)
            {
              next_arc = curr_arc->next_arc_marked_by_insn;
              remove_arc (original_state, curr_arc);
	      alts_number++;
            }
	  arcs_marked_by_insn->state_alts = alts_number;
        }
    }
  if (!state->it_was_placed_in_stack_for_DFA_forming)
    {
      state->it_was_placed_in_stack_for_DFA_forming = 1;
      VLA_PTR_ADD (*state_stack, state);
    }
  return new_state_p;
}

/* The function transforms nondeterministic AUTOMATON into
   deterministic.  */
static void
NDFA_to_DFA (automaton)
     automaton_t automaton;
{
  state_t start_state;
  state_t state;
  decl_t decl;
  vla_ptr_t state_stack;
  int i;
  int states_n;

  VLA_PTR_CREATE (state_stack, 150, "state stack");
  /* Create the start state (empty state).  */
  start_state = automaton->start_state;
  start_state->it_was_placed_in_stack_for_DFA_forming = 1;
  VLA_PTR_ADD (state_stack, start_state);
  states_n = 1;
  while (VLA_PTR_LENGTH (state_stack) != 0)
    {
      state = VLA_PTR (state_stack, VLA_PTR_LENGTH (state_stack) - 1);
      VLA_PTR_SHORTEN (state_stack, 1);
      form_arcs_marked_by_insn (state);
      for (i = 0; i < description->decls_num; i++)
	{
	  decl = description->decls [i];
	  if (decl->mode == dm_insn_reserv
	      && create_composed_state
	         (state, DECL_INSN_RESERV (decl)->arcs_marked_by_insn,
		  &state_stack))
	    {
	      states_n++;
	      if (states_n % 100 == 0)
		fprintf (stderr, "*");
	    }
	}
    }
  VLA_PTR_DELETE (state_stack);
}

/* The following variable value is current number (1, 2, ...) of passing
   graph of states.  */
static int curr_state_graph_pass_num;

/* This recursive function passes all states achieved from START_STATE
   and applies APPLIED_FUNC to them.  */
static void
pass_state_graph (start_state, applied_func)
     state_t start_state;
     void (*applied_func) PARAMS ((state_t state));
{
  arc_t arc;

  if (start_state->pass_num == curr_state_graph_pass_num)
    return;
  start_state->pass_num = curr_state_graph_pass_num;
  (*applied_func) (start_state);
  for (arc = first_out_arc (start_state);
       arc != NULL;
       arc = next_out_arc (arc))
    pass_state_graph (arc->to_state, applied_func);
}

/* This recursive function passes all states of AUTOMATON and applies
   APPLIED_FUNC to them.  */
static void
pass_states (automaton, applied_func)
     automaton_t automaton;
     void (*applied_func) PARAMS ((state_t state));
{
  curr_state_graph_pass_num++;
  pass_state_graph (automaton->start_state, applied_func);
}

/* The function initializes code for passing of all states.  */
static void
initiate_pass_states ()
{
  curr_state_graph_pass_num = 0;
}

/* The following vla is used for storing pointers to all achieved
   states.  */
static vla_ptr_t all_achieved_states;

/* This function is called by function pass_states to add an achieved
   STATE.  */
static void
add_achieved_state (state)
     state_t state;
{
  VLA_PTR_ADD (all_achieved_states, state);
}

/* The function sets up equivalence numbers of insns which mark all
   out arcs of STATE by equiv_class_num_1 (if ODD_ITERATION_FLAG has
   nonzero value) or by equiv_class_num_2 of the destination state.
   The function returns number of out arcs of STATE.  */
static int
set_out_arc_insns_equiv_num (state, odd_iteration_flag)
     state_t state;
     int odd_iteration_flag;
{
  int state_out_arcs_num;
  arc_t arc;

  state_out_arcs_num = 0;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    {
      if (arc->insn->insn_reserv_decl->equiv_class_num != 0
	  || arc->insn->insn_reserv_decl->state_alts != 0)
	abort ();
      state_out_arcs_num++;
      arc->insn->insn_reserv_decl->equiv_class_num
	= (odd_iteration_flag
           ? arc->to_state->equiv_class_num_1
	   : arc->to_state->equiv_class_num_2);
      arc->insn->insn_reserv_decl->state_alts = arc->state_alts;
      if (arc->insn->insn_reserv_decl->equiv_class_num == 0
	  || arc->insn->insn_reserv_decl->state_alts <= 0)
	abort ();
    }
  return state_out_arcs_num;
}

/* The function clears equivalence numbers and alt_states in all insns
   which mark all out arcs of STATE.  */
static void
clear_arc_insns_equiv_num (state)
     state_t state;
{
  arc_t arc;

  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    {
      arc->insn->insn_reserv_decl->equiv_class_num = 0;
      arc->insn->insn_reserv_decl->state_alts = 0;
    }
}

/* The function copies pointers to equivalent states from vla FROM
   into vla TO.  */
static void
copy_equiv_class (to, from)
     vla_ptr_t *to;
     const vla_ptr_t *from;
{
  state_t *class_ptr;

  VLA_PTR_NULLIFY (*to);
  for (class_ptr = VLA_PTR_BEGIN (*from);
       class_ptr <= (state_t *) VLA_PTR_LAST (*from);
       class_ptr++)
    VLA_PTR_ADD (*to, *class_ptr);
}

/* The following function returns TRUE if STATE reserves the unit with
   UNIT_NUM on the first cycle.  */
static int
first_cycle_unit_presence (state, unit_num)
     state_t state;
     int unit_num;
{
  int presence_p;

  if (state->component_states == NULL)
    presence_p = test_unit_reserv (state->reservs, 0, unit_num);
  else
    presence_p
      = test_unit_reserv (state->component_states->state->reservs,
			  0, unit_num);
  return presence_p;
}

/* The function returns nonzero value if STATE is not equivalent to
   ANOTHER_STATE from the same current partition on equivalence
   classes.  Another state has ANOTHER_STATE_OUT_ARCS_NUM number of
   output arcs.  Iteration of making equivalence partition is defined
   by ODD_ITERATION_FLAG.  */
static int
state_is_differed (state, another_state, another_state_out_arcs_num,
		   odd_iteration_flag)
     state_t state, another_state;
     int another_state_out_arcs_num;
     int odd_iteration_flag;
{
  arc_t arc;
  int state_out_arcs_num;
  int i, presence1_p, presence2_p;

  state_out_arcs_num = 0;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    {
      state_out_arcs_num++;
      if ((odd_iteration_flag
           ? arc->to_state->equiv_class_num_1
	   : arc->to_state->equiv_class_num_2)
          != arc->insn->insn_reserv_decl->equiv_class_num
	  || (arc->insn->insn_reserv_decl->state_alts != arc->state_alts))
        return 1;
    }
  if (state_out_arcs_num != another_state_out_arcs_num)
    return 1;
  /* Now we are looking at the states with the point of view of query
     units.  */
  for (i = 0; i < description->units_num; i++)
    if (units_array [i]->query_p)
      {
	presence1_p = first_cycle_unit_presence (state, i);
	presence2_p = first_cycle_unit_presence (another_state, i);
	if ((presence1_p && !presence2_p) || (!presence1_p && presence2_p))
	  return 1;
      }
  return 0;
}

/* The function makes initial partition of STATES on equivalent
   classes.  */
static state_t
init_equiv_class (states, states_num)
     state_t *states;
     int states_num;
{
  state_t *state_ptr;
  state_t result_equiv_class;

  result_equiv_class = NULL;
  for (state_ptr = states; state_ptr < states + states_num; state_ptr++)
    {
      (*state_ptr)->equiv_class_num_1 = 1;
      (*state_ptr)->next_equiv_class_state = result_equiv_class;
      result_equiv_class = *state_ptr;
    }
  return result_equiv_class;
}

/* The function processes equivalence class given by its pointer
   EQUIV_CLASS_PTR on odd iteration if ODD_ITERATION_FLAG.  If there
   are not equivalent states, the function partitions the class
   removing nonequivalent states and placing them in
   *NEXT_ITERATION_CLASSES, increments *NEW_EQUIV_CLASS_NUM_PTR ans
   assigns it to the state equivalence number.  If the class has been
   partitioned, the function returns nonzero value.  */
static int
partition_equiv_class (equiv_class_ptr, odd_iteration_flag,
		       next_iteration_classes, new_equiv_class_num_ptr)
     state_t *equiv_class_ptr;
     int odd_iteration_flag;
     vla_ptr_t *next_iteration_classes;
     int *new_equiv_class_num_ptr;
{
  state_t new_equiv_class;
  int partition_p;
  state_t first_state;
  state_t curr_state;
  state_t prev_state;
  state_t next_state;
  int out_arcs_num;

  partition_p = 0;
  if (*equiv_class_ptr == NULL)
    abort ();
  for (first_state = *equiv_class_ptr;
       first_state != NULL;
       first_state = new_equiv_class)
    {
      new_equiv_class = NULL;
      if (first_state->next_equiv_class_state != NULL)
	{
	  /* There are more one states in the class equivalence.  */
	  out_arcs_num = set_out_arc_insns_equiv_num (first_state,
						      odd_iteration_flag);
	  for (prev_state = first_state,
		 curr_state = first_state->next_equiv_class_state;
	       curr_state != NULL;
	       curr_state = next_state)
	    {
	      next_state = curr_state->next_equiv_class_state;
	      if (state_is_differed (curr_state, first_state, out_arcs_num,
				     odd_iteration_flag))
		{
		  /* Remove curr state from the class equivalence.  */
		  prev_state->next_equiv_class_state = next_state;
		  /* Add curr state to the new class equivalence.  */
		  curr_state->next_equiv_class_state = new_equiv_class;
		  if (new_equiv_class == NULL)
		    (*new_equiv_class_num_ptr)++;
		  if (odd_iteration_flag)
		    curr_state->equiv_class_num_2 = *new_equiv_class_num_ptr;
		  else
		    curr_state->equiv_class_num_1 = *new_equiv_class_num_ptr;
		  new_equiv_class = curr_state;
		  partition_p = 1;
		}
	      else
		prev_state = curr_state;
	    }
	  clear_arc_insns_equiv_num (first_state);
	}
      if (new_equiv_class != NULL)
	VLA_PTR_ADD  (*next_iteration_classes, new_equiv_class);
    }
  return partition_p;
}

/* The function finds equivalent states of AUTOMATON.  */
static void
evaluate_equiv_classes (automaton, equiv_classes)
     automaton_t automaton;
     vla_ptr_t *equiv_classes;
{
  state_t new_equiv_class;
  int new_equiv_class_num;
  int odd_iteration_flag;
  int finish_flag;
  vla_ptr_t next_iteration_classes;
  state_t *equiv_class_ptr;
  state_t *state_ptr;
  
  VLA_PTR_CREATE (all_achieved_states, 1500, "all achieved states");
  pass_states (automaton, add_achieved_state);
  new_equiv_class = init_equiv_class (VLA_PTR_BEGIN (all_achieved_states),
                                      VLA_PTR_LENGTH (all_achieved_states));
  odd_iteration_flag = 0;
  new_equiv_class_num = 1;
  VLA_PTR_CREATE (next_iteration_classes, 150, "next iteration classes");
  VLA_PTR_ADD (next_iteration_classes, new_equiv_class);
  do
    {
      odd_iteration_flag = !odd_iteration_flag;
      finish_flag = 1;
      copy_equiv_class (equiv_classes, &next_iteration_classes);
      /* Transfer equiv numbers for the next iteration.  */
      for (state_ptr = VLA_PTR_BEGIN (all_achieved_states);
	   state_ptr <= (state_t *) VLA_PTR_LAST (all_achieved_states);
           state_ptr++)
	if (odd_iteration_flag)
	  (*state_ptr)->equiv_class_num_2 = (*state_ptr)->equiv_class_num_1;
	else
	  (*state_ptr)->equiv_class_num_1 = (*state_ptr)->equiv_class_num_2;
      for (equiv_class_ptr = VLA_PTR_BEGIN (*equiv_classes);
           equiv_class_ptr <= (state_t *) VLA_PTR_LAST (*equiv_classes);
           equiv_class_ptr++)
	if (partition_equiv_class (equiv_class_ptr, odd_iteration_flag,
				   &next_iteration_classes,
				   &new_equiv_class_num))
	  finish_flag = 0;
    }
  while (!finish_flag);
  VLA_PTR_DELETE (next_iteration_classes);
  VLA_PTR_DELETE (all_achieved_states);
}

/* The function merges equivalent states of AUTOMATON.  */
static void
merge_states (automaton, equiv_classes)
     automaton_t automaton;
     vla_ptr_t *equiv_classes;
{
  state_t *equiv_class_ptr;
  state_t curr_state;
  state_t new_state;
  state_t first_class_state;
  alt_state_t alt_states;
  alt_state_t alt_state, new_alt_state;
  arc_t curr_arc;
  arc_t next_arc;

  /* Create states corresponding to equivalence classes containing two
     or more states.  */
  for (equiv_class_ptr = VLA_PTR_BEGIN (*equiv_classes);
       equiv_class_ptr <= (state_t *) VLA_PTR_LAST (*equiv_classes);
       equiv_class_ptr++)
    if ((*equiv_class_ptr)->next_equiv_class_state != NULL)
      {
        /* There are more one states in the class equivalence.  */
        /* Create new compound state.  */
        new_state = get_free_state (0, automaton);
        alt_states = NULL;
        first_class_state = *equiv_class_ptr;
        for (curr_state = first_class_state;
             curr_state != NULL;
             curr_state = curr_state->next_equiv_class_state)
          {
            curr_state->equiv_class_state = new_state;
	    if (curr_state->component_states == NULL)
	      {
		new_alt_state = get_free_alt_state ();
		new_alt_state->state = curr_state;
		new_alt_state->next_alt_state = alt_states;
		alt_states = new_alt_state;
	      }
	    else
	      for (alt_state = curr_state->component_states;
		   alt_state != NULL;
		   alt_state = alt_state->next_sorted_alt_state)
		{
		  new_alt_state = get_free_alt_state ();
		  new_alt_state->state = alt_state->state;
		  new_alt_state->next_alt_state = alt_states;
		  alt_states = new_alt_state;
		}
          }
	/* Its is important that alt states were sorted before and
           after merging to have the same quering results.  */
        new_state->component_states = uniq_sort_alt_states (alt_states);
      }
    else
      (*equiv_class_ptr)->equiv_class_state = *equiv_class_ptr;
  for (equiv_class_ptr = VLA_PTR_BEGIN (*equiv_classes);
       equiv_class_ptr <= (state_t *) VLA_PTR_LAST (*equiv_classes);
       equiv_class_ptr++)
    if ((*equiv_class_ptr)->next_equiv_class_state != NULL)
      {
        first_class_state = *equiv_class_ptr;
        /* Create new arcs output from the state corresponding to
           equiv class.  */
        for (curr_arc = first_out_arc (first_class_state);
             curr_arc != NULL;
             curr_arc = next_out_arc (curr_arc))
          add_arc (first_class_state->equiv_class_state,
                   curr_arc->to_state->equiv_class_state,
		   curr_arc->insn, curr_arc->state_alts);
        /* Delete output arcs from states of given class equivalence.  */
        for (curr_state = first_class_state;
             curr_state != NULL;
             curr_state = curr_state->next_equiv_class_state)
          {
            if (automaton->start_state == curr_state)
              automaton->start_state = curr_state->equiv_class_state;
            /* Delete the state and its output arcs.  */
            for (curr_arc = first_out_arc (curr_state);
                 curr_arc != NULL;
                 curr_arc = next_arc)
              {
                next_arc = next_out_arc (curr_arc);
                free_arc (curr_arc);
              }
          }
      }
    else
      {
        /* Change `to_state' of arcs output from the state of given
           equivalence class.  */
        for (curr_arc = first_out_arc (*equiv_class_ptr);
             curr_arc != NULL;
             curr_arc = next_out_arc (curr_arc))
          curr_arc->to_state = curr_arc->to_state->equiv_class_state;
      }
}

/* The function sets up new_cycle_p for states if there is arc to the
   state marked by advance_cycle_insn_decl.  */
static void
set_new_cycle_flags (state)
     state_t state;
{
  arc_t arc;

  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    if (arc->insn->insn_reserv_decl
	== DECL_INSN_RESERV (advance_cycle_insn_decl))
      arc->to_state->new_cycle_p = 1;
}

/* The top level function for minimization of deterministic
   AUTOMATON.  */
static void
minimize_DFA (automaton)
     automaton_t automaton;
{
  vla_ptr_t equiv_classes;

  VLA_PTR_CREATE (equiv_classes, 1500, "equivalence classes");
  evaluate_equiv_classes (automaton, &equiv_classes);
  merge_states (automaton, &equiv_classes);
  pass_states (automaton, set_new_cycle_flags);
  VLA_PTR_DELETE (equiv_classes);
}

/* Values of two variables are counted number of states and arcs in an
   automaton.  */
static int curr_counted_states_num;
static int curr_counted_arcs_num;

/* The function is called by function `pass_states' to count states
   and arcs of an automaton.  */
static void
incr_states_and_arcs_nums (state)
     state_t state;
{
  arc_t arc;

  curr_counted_states_num++;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    curr_counted_arcs_num++;
}

/* The function counts states and arcs of AUTOMATON.  */
static void
count_states_and_arcs (automaton, states_num, arcs_num)
     automaton_t automaton;
     int *states_num;
     int *arcs_num;
{
  curr_counted_states_num = 0;
  curr_counted_arcs_num = 0;
  pass_states (automaton, incr_states_and_arcs_nums);
  *states_num = curr_counted_states_num;
  *arcs_num = curr_counted_arcs_num;
}

/* The function builds one DFA AUTOMATON for fast pipeline hazards
   recognition after checking and simplifying IR of the
   description.  */
static void
build_automaton (automaton)
     automaton_t automaton;
{
  int states_num;
  int arcs_num;

  ticker_on (&NDFA_time);
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (stderr, "Create anonymous automaton (1 star is 100 new states):");
  else
    fprintf (stderr, "Create automaton `%s' (1 star is 100 new states):",
	     automaton->corresponding_automaton_decl->name);
  make_automaton (automaton);
  fprintf (stderr, " done\n");
  ticker_off (&NDFA_time);
  count_states_and_arcs (automaton, &states_num, &arcs_num);
  automaton->NDFA_states_num = states_num;
  automaton->NDFA_arcs_num = arcs_num;
  ticker_on (&NDFA_to_DFA_time);
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (stderr, "Make anonymous DFA (1 star is 100 new states):");
  else
    fprintf (stderr, "Make DFA `%s' (1 star is 100 new states):",
	     automaton->corresponding_automaton_decl->name);
  NDFA_to_DFA (automaton);
  fprintf (stderr, " done\n");
  ticker_off (&NDFA_to_DFA_time);
  count_states_and_arcs (automaton, &states_num, &arcs_num);
  automaton->DFA_states_num = states_num;
  automaton->DFA_arcs_num = arcs_num;
  if (!no_minimization_flag)
    {
      ticker_on (&minimize_time);
      if (automaton->corresponding_automaton_decl == NULL)
	fprintf (stderr, "Minimize anonymous DFA...");
      else
	fprintf (stderr, "Minimize DFA `%s'...",
		 automaton->corresponding_automaton_decl->name);
      minimize_DFA (automaton);
      fprintf (stderr, "done\n");
      ticker_off (&minimize_time);
      count_states_and_arcs (automaton, &states_num, &arcs_num);
      automaton->minimal_DFA_states_num = states_num;
      automaton->minimal_DFA_arcs_num = arcs_num;
    }
}



/* The page contains code for enumeration  of all states of an automaton.  */

/* Variable used for enumeration of all states of an automaton.  Its
   value is current number of automaton states.  */
static int curr_state_order_num;

/* The function is called by function `pass_states' for enumerating
   states.  */
static void
set_order_state_num (state)
     state_t state;
{
  state->order_state_num = curr_state_order_num;
  curr_state_order_num++;
}

/* The function enumerates all states of AUTOMATON.  */
static void
enumerate_states (automaton)
     automaton_t automaton;
{
  curr_state_order_num = 0;
  pass_states (automaton, set_order_state_num);
  automaton->achieved_states_num = curr_state_order_num;
}



/* The page contains code for finding equivalent automaton insns
   (ainsns).  */

/* The function inserts AINSN into cyclic list
   CYCLIC_EQUIV_CLASS_INSN_LIST of ainsns.  */
static ainsn_t
insert_ainsn_into_equiv_class (ainsn, cyclic_equiv_class_insn_list)
     ainsn_t ainsn;
     ainsn_t cyclic_equiv_class_insn_list;
{
  if (cyclic_equiv_class_insn_list == NULL)
    ainsn->next_equiv_class_insn = ainsn;
  else
    {
      ainsn->next_equiv_class_insn
        = cyclic_equiv_class_insn_list->next_equiv_class_insn;
      cyclic_equiv_class_insn_list->next_equiv_class_insn = ainsn;
    }
  return ainsn;
}

/* The function deletes equiv_class_insn into cyclic list of
   equivalent ainsns.  */
static void
delete_ainsn_from_equiv_class (equiv_class_insn)
     ainsn_t equiv_class_insn;
{
  ainsn_t curr_equiv_class_insn;
  ainsn_t prev_equiv_class_insn;

  prev_equiv_class_insn = equiv_class_insn;
  for (curr_equiv_class_insn = equiv_class_insn->next_equiv_class_insn;
       curr_equiv_class_insn != equiv_class_insn;
       curr_equiv_class_insn = curr_equiv_class_insn->next_equiv_class_insn)
    prev_equiv_class_insn = curr_equiv_class_insn;
  if (prev_equiv_class_insn != equiv_class_insn)
    prev_equiv_class_insn->next_equiv_class_insn
      = equiv_class_insn->next_equiv_class_insn;
}

/* The function processes AINSN of a state in order to find equivalent
   ainsns.  INSN_ARCS_ARRAY is table: code of insn -> out arc of the
   state.  */
static void
process_insn_equiv_class (ainsn, insn_arcs_array)
     ainsn_t ainsn;
     arc_t *insn_arcs_array;
{
  ainsn_t next_insn;
  ainsn_t curr_insn;
  ainsn_t cyclic_insn_list;
  arc_t arc;

  if (insn_arcs_array [ainsn->insn_reserv_decl->insn_num] == NULL)
    abort ();
  curr_insn = ainsn;
  /* New class of ainsns which are not equivalent to given ainsn.  */
  cyclic_insn_list = NULL;
  do
    {
      next_insn = curr_insn->next_equiv_class_insn;
      arc = insn_arcs_array [curr_insn->insn_reserv_decl->insn_num];
      if (arc == NULL
          || (insn_arcs_array [ainsn->insn_reserv_decl->insn_num]->to_state
              != arc->to_state))
        {
          delete_ainsn_from_equiv_class (curr_insn);
          cyclic_insn_list = insert_ainsn_into_equiv_class (curr_insn,
							    cyclic_insn_list);
        }
      curr_insn = next_insn;
    }
  while (curr_insn != ainsn);
}

/* The function processes STATE in order to find equivalent ainsns.  */
static void
process_state_for_insn_equiv_partition (state)
     state_t state;
{
  arc_t arc;
  arc_t *insn_arcs_array;
  int i;
  vla_ptr_t insn_arcs_vect;

  VLA_PTR_CREATE (insn_arcs_vect, 500, "insn arcs vector");
  VLA_PTR_EXPAND (insn_arcs_vect, description->insns_num);
  insn_arcs_array = VLA_PTR_BEGIN (insn_arcs_vect);
  /* Process insns of the arcs.  */
  for (i = 0; i < description->insns_num; i++)
    insn_arcs_array [i] = NULL;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    insn_arcs_array [arc->insn->insn_reserv_decl->insn_num] = arc;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    process_insn_equiv_class (arc->insn, insn_arcs_array);
  VLA_PTR_DELETE (insn_arcs_vect);
}

/* The function searches for equivalent ainsns of AUTOMATON.  */
static void
set_insn_equiv_classes (automaton)
     automaton_t automaton;
{
  ainsn_t ainsn;
  ainsn_t first_insn;
  ainsn_t curr_insn;
  ainsn_t cyclic_insn_list;
  ainsn_t insn_with_same_reservs;
  int equiv_classes_num;

  /* All insns are included in one equivalence class.  */
  cyclic_insn_list = NULL;
  for (ainsn = automaton->ainsn_list; ainsn != NULL; ainsn = ainsn->next_ainsn)
    if (ainsn->first_insn_with_same_reservs)
      cyclic_insn_list = insert_ainsn_into_equiv_class (ainsn,
							cyclic_insn_list);
  /* Process insns in order to make equivalence partition.  */
  pass_states (automaton, process_state_for_insn_equiv_partition);
  /* Enumerate equiv classes.  */
  for (ainsn = automaton->ainsn_list; ainsn != NULL; ainsn = ainsn->next_ainsn)
    /* Set undefined value.  */
    ainsn->insn_equiv_class_num = -1;
  equiv_classes_num = 0;
  for (ainsn = automaton->ainsn_list; ainsn != NULL; ainsn = ainsn->next_ainsn)
    if (ainsn->insn_equiv_class_num < 0)
      {
        first_insn = ainsn;
        if (!first_insn->first_insn_with_same_reservs)
	  abort ();
        first_insn->first_ainsn_with_given_equialence_num = 1;
        curr_insn = first_insn;
        do
          {
            for (insn_with_same_reservs = curr_insn;
                 insn_with_same_reservs != NULL;
                 insn_with_same_reservs
		   = insn_with_same_reservs->next_same_reservs_insn)
              insn_with_same_reservs->insn_equiv_class_num = equiv_classes_num;
            curr_insn = curr_insn->next_equiv_class_insn;
          }
        while (curr_insn != first_insn);
        equiv_classes_num++;
      }
  automaton->insn_equiv_classes_num = equiv_classes_num;
}



/* This page contains code for creating DFA(s) and calls functions
   building them.  */


/* The following value is used to prevent floating point overflow for
   estimating an automaton bound.  The value should be less DBL_MAX on
   the host machine.  We use here approximate minimum of maximal
   double floating point value required by ANSI C standard.  It
   will work for non ANSI sun compiler too.  */

#define MAX_FLOATING_POINT_VALUE_FOR_AUTOMATON_BOUND  1.0E37

/* The function estimate size of the single DFA used by PHR (pipeline
   hazards recognizer).  */
static double
estimate_one_automaton_bound ()
{
  decl_t decl;
  double one_automaton_estimation_bound;
  double root_value;
  int i;

  one_automaton_estimation_bound = 1.0;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	{
	  root_value = exp (log (DECL_UNIT (decl)->max_occ_cycle_num
				 - DECL_UNIT (decl)->min_occ_cycle_num + 1.0)
                            / automata_num);
	  if (MAX_FLOATING_POINT_VALUE_FOR_AUTOMATON_BOUND / root_value
	      > one_automaton_estimation_bound)
	    one_automaton_estimation_bound *= root_value;
	}
    }
  return one_automaton_estimation_bound;
}

/* The function compares unit declarations according to their maximal
   cycle in reservations.  */
static int
compare_max_occ_cycle_nums (unit_decl_1, unit_decl_2)
     const void *unit_decl_1;
     const void *unit_decl_2;
{
  if ((DECL_UNIT (*(decl_t *) unit_decl_1)->max_occ_cycle_num)
      < (DECL_UNIT (*(decl_t *) unit_decl_2)->max_occ_cycle_num))
    return 1;
  else if ((DECL_UNIT (*(decl_t *) unit_decl_1)->max_occ_cycle_num)
	   == (DECL_UNIT (*(decl_t *) unit_decl_2)->max_occ_cycle_num))
    return 0;
  else
    return -1;
}

/* The function makes heuristic assigning automata to units.  Actually
   efficacy of the algorithm has been checked yet??? */
static void
units_to_automata_heuristic_distr ()
{
  double estimation_bound;
  decl_t decl;
  decl_t *unit_decl_ptr;
  int automaton_num;
  int rest_units_num;
  double bound_value;
  vla_ptr_t unit_decls;
  int i;

  if (description->units_num == 0)
    return;
  estimation_bound = estimate_one_automaton_bound ();
  VLA_PTR_CREATE (unit_decls, 150, "unit decls");
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	VLA_PTR_ADD (unit_decls, decl);
    }
  qsort (VLA_PTR_BEGIN (unit_decls), VLA_PTR_LENGTH (unit_decls),
         sizeof (decl_t), compare_max_occ_cycle_nums);
  automaton_num = 0;
  unit_decl_ptr = VLA_PTR_BEGIN (unit_decls);
  bound_value = DECL_UNIT (*unit_decl_ptr)->max_occ_cycle_num;
  DECL_UNIT (*unit_decl_ptr)->corresponding_automaton_num = automaton_num;
  for (unit_decl_ptr++;
       unit_decl_ptr <= (decl_t *) VLA_PTR_LAST (unit_decls);
       unit_decl_ptr++)
    {
      rest_units_num
	= ((decl_t *) VLA_PTR_LAST (unit_decls) - unit_decl_ptr + 1);
      if (automata_num - automaton_num - 1 > rest_units_num)
	abort ();
      if (automaton_num < automata_num - 1
          && ((automata_num - automaton_num - 1 == rest_units_num)
              || (bound_value
                  > (estimation_bound
		     / (DECL_UNIT (*unit_decl_ptr)->max_occ_cycle_num)))))
        {
          bound_value = DECL_UNIT (*unit_decl_ptr)->max_occ_cycle_num;
          automaton_num++;
        }
      else
        bound_value *= DECL_UNIT (*unit_decl_ptr)->max_occ_cycle_num;
      DECL_UNIT (*unit_decl_ptr)->corresponding_automaton_num = automaton_num;
    }
  if (automaton_num != automata_num - 1)
    abort ();
  VLA_PTR_DELETE (unit_decls);
}

/* The functions creates automaton insns for each automata.  Automaton
   insn is simply insn for given automaton which makes reservation
   only of units of the automaton.  */
static ainsn_t
create_ainsns ()
{
  decl_t decl;
  ainsn_t first_ainsn;
  ainsn_t curr_ainsn;
  ainsn_t prev_ainsn;
  int i;

  first_ainsn = NULL;
  prev_ainsn = NULL;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
	{
	  curr_ainsn = create_node (sizeof (struct ainsn));
	  curr_ainsn->insn_reserv_decl = DECL_INSN_RESERV (decl);
	  curr_ainsn->important_p = FALSE;
	  curr_ainsn->next_ainsn = NULL;
	  if (prev_ainsn == NULL)
	    first_ainsn = curr_ainsn;
	  else
	    prev_ainsn->next_ainsn = curr_ainsn;
	  prev_ainsn = curr_ainsn;
	}
    }
  return first_ainsn;
}

/* The function assigns automata to units according to constructions
   `define_automaton' in the description.  */
static void
units_to_automata_distr ()
{
  decl_t decl;
  int i;
  
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	{
	  if (DECL_UNIT (decl)->automaton_decl == NULL
	      || (DECL_UNIT (decl)->automaton_decl->corresponding_automaton
		  == NULL))
	    /* Distribute to the first automaton.  */
	    DECL_UNIT (decl)->corresponding_automaton_num = 0;
	  else
	    DECL_UNIT (decl)->corresponding_automaton_num
	      = (DECL_UNIT (decl)->automaton_decl
                 ->corresponding_automaton->automaton_order_num);
	}
    }
}

/* The function creates DFA(s) for fast pipeline hazards recognition
   after checking and simplifying IR of the description.  */
static void
create_automata ()
{
  automaton_t curr_automaton;
  automaton_t prev_automaton;
  decl_t decl;
  int curr_automaton_num;
  int i;

  if (automata_num != 0)
    {
      units_to_automata_heuristic_distr ();
      for (prev_automaton = NULL, curr_automaton_num = 0;
           curr_automaton_num < automata_num;
           curr_automaton_num++, prev_automaton = curr_automaton)
        {
	  curr_automaton = create_node (sizeof (struct automaton));
	  curr_automaton->ainsn_list = create_ainsns ();
	  curr_automaton->corresponding_automaton_decl = NULL;
	  curr_automaton->next_automaton = NULL;
          curr_automaton->automaton_order_num = curr_automaton_num;
          if (prev_automaton == NULL)
            description->first_automaton = curr_automaton;
          else
            prev_automaton->next_automaton = curr_automaton;
        }
    }
  else
    {
      curr_automaton_num = 0;
      prev_automaton = NULL;
      for (i = 0; i < description->decls_num; i++)
	{
	  decl = description->decls [i];
	  if (decl->mode == dm_automaton
	      && DECL_AUTOMATON (decl)->automaton_is_used)
	    {
	      curr_automaton = create_node (sizeof (struct automaton));
	      curr_automaton->ainsn_list = create_ainsns ();
	      curr_automaton->corresponding_automaton_decl
		= DECL_AUTOMATON (decl);
	      curr_automaton->next_automaton = NULL;
	      DECL_AUTOMATON (decl)->corresponding_automaton = curr_automaton;
	      curr_automaton->automaton_order_num = curr_automaton_num;
	      if (prev_automaton == NULL)
		description->first_automaton = curr_automaton;
	      else
		prev_automaton->next_automaton = curr_automaton;
	      curr_automaton_num++;
	      prev_automaton = curr_automaton;
	    }
	}
      if (curr_automaton_num == 0)
	{
	  curr_automaton = create_node (sizeof (struct automaton));
	  curr_automaton->ainsn_list = create_ainsns ();
	  curr_automaton->corresponding_automaton_decl = NULL;
	  curr_automaton->next_automaton = NULL;
	  description->first_automaton = curr_automaton;
	}
      units_to_automata_distr ();
    }
  NDFA_time = create_ticker ();
  ticker_off (&NDFA_time);
  NDFA_to_DFA_time = create_ticker ();
  ticker_off (&NDFA_to_DFA_time);
  minimize_time = create_ticker ();
  ticker_off (&minimize_time);
  equiv_time = create_ticker ();
  ticker_off (&equiv_time);
  for (curr_automaton = description->first_automaton;
       curr_automaton != NULL;
       curr_automaton = curr_automaton->next_automaton)
    {
      if (curr_automaton->corresponding_automaton_decl == NULL)
	fprintf (stderr, "Prepare anonymous automaton creation ... ");
      else
	fprintf (stderr, "Prepare automaton `%s' creation...",
		 curr_automaton->corresponding_automaton_decl->name);
      create_alt_states (curr_automaton);
      form_ainsn_with_same_reservs (curr_automaton);
      fprintf (stderr, "done\n");
      build_automaton (curr_automaton);
      enumerate_states (curr_automaton);
      ticker_on (&equiv_time);
      set_insn_equiv_classes (curr_automaton);
      ticker_off (&equiv_time);
    }
}



/* This page contains code for forming string representation of
   regexp.  The representation is formed on IR obstack.  So you should
   not work with IR obstack between regexp_representation and
   finish_regexp_representation calls.  */

/* This recursive function forms string representation of regexp
   (without tailing '\0').  */
static void
form_regexp (regexp)
     regexp_t regexp;
{
  int i;
    
  if (regexp->mode == rm_unit || regexp->mode == rm_reserv)
    {
      const char *name = (regexp->mode == rm_unit
                          ? REGEXP_UNIT (regexp)->name
			  : REGEXP_RESERV (regexp)->name);

      obstack_grow (&irp, name, strlen (name));
    }
  else if (regexp->mode == rm_sequence)
    for (i = 0; i < REGEXP_SEQUENCE (regexp)->regexps_num; i++)
      {
	if (i != 0)
          obstack_1grow (&irp, ',');
	form_regexp (REGEXP_SEQUENCE (regexp)->regexps [i]);
      }
  else if (regexp->mode == rm_allof)
    {
      obstack_1grow (&irp, '(');
      for (i = 0; i < REGEXP_ALLOF (regexp)->regexps_num; i++)
	{
	  if (i != 0)
            obstack_1grow (&irp, '+');
	  if (REGEXP_ALLOF (regexp)->regexps[i]->mode == rm_sequence
              || REGEXP_ALLOF (regexp)->regexps[i]->mode == rm_oneof)
            obstack_1grow (&irp, '(');
	  form_regexp (REGEXP_ALLOF (regexp)->regexps [i]);
	  if (REGEXP_ALLOF (regexp)->regexps[i]->mode == rm_sequence
              || REGEXP_ALLOF (regexp)->regexps[i]->mode == rm_oneof)
            obstack_1grow (&irp, ')');
        }
      obstack_1grow (&irp, ')');
    }
  else if (regexp->mode == rm_oneof)
    for (i = 0; i < REGEXP_ONEOF (regexp)->regexps_num; i++)
      {
	if (i != 0)
          obstack_1grow (&irp, '|');
	if (REGEXP_ONEOF (regexp)->regexps[i]->mode == rm_sequence)
          obstack_1grow (&irp, '(');
        form_regexp (REGEXP_ONEOF (regexp)->regexps [i]);
	if (REGEXP_ONEOF (regexp)->regexps[i]->mode == rm_sequence)
          obstack_1grow (&irp, ')');
      }
  else if (regexp->mode == rm_repeat)
    {
      char digits [30];

      if (REGEXP_REPEAT (regexp)->regexp->mode == rm_sequence
	  || REGEXP_REPEAT (regexp)->regexp->mode == rm_allof
	  || REGEXP_REPEAT (regexp)->regexp->mode == rm_oneof)
        obstack_1grow (&irp, '(');
      form_regexp (REGEXP_REPEAT (regexp)->regexp);
      if (REGEXP_REPEAT (regexp)->regexp->mode == rm_sequence
	  || REGEXP_REPEAT (regexp)->regexp->mode == rm_allof
	  || REGEXP_REPEAT (regexp)->regexp->mode == rm_oneof)
        obstack_1grow (&irp, ')');
      sprintf (digits, "*%d", REGEXP_REPEAT (regexp)->repeat_num);
      obstack_grow (&irp, digits, strlen (digits));
    }
  else if (regexp->mode == rm_nothing)
    obstack_grow (&irp, NOTHING_NAME, strlen (NOTHING_NAME));
  else
    abort ();
}

/* The function returns string representation of REGEXP on IR
   obstack.  */
static const char *
regexp_representation (regexp)
     regexp_t regexp;
{
  form_regexp (regexp);
  obstack_1grow (&irp, '\0');
  return obstack_base (&irp);
}

/* The function frees memory allocated for last formed string
   representation of regexp.  */
static void
finish_regexp_representation ()
{
  int length = obstack_object_size (&irp);
  
  obstack_blank_fast (&irp, -length);
}



/* This page contains code for output PHR (pipeline hazards recognizer).  */

/* The function outputs minimal C type which is sufficient for
   representation numbers in range min_range_value and
   max_range_value.  Because host machine and build machine may be
   different, we use here minimal values required by ANSI C standard
   instead of UCHAR_MAX, SHRT_MAX, SHRT_MIN, etc.  This is a good
   approximation.  */

static void
output_range_type (f, min_range_value, max_range_value)
     FILE *f;
     long int min_range_value;
     long int max_range_value;
{
  if (min_range_value >= 0 && max_range_value <= 255)
    fprintf (f, "unsigned char");
  else if (min_range_value >= -127 && max_range_value <= 127)
    fprintf (f, "signed char");
  else if (min_range_value >= 0 && max_range_value <= 65535)
    fprintf (f, "unsigned short");
  else if (min_range_value >= -32767 && max_range_value <= 32767)
    fprintf (f, "short");
  else
    fprintf (f, "int");
}

/* The following macro value is used as value of member
   `longest_path_length' of state when we are processing path and the
   state on the path.  */

#define ON_THE_PATH -2

/* The following recursive function searches for the length of the
   longest path starting from STATE which does not contain cycles and
   `cycle advance' arcs.  */

static int
longest_path_length (state)
     state_t state;
{
  arc_t arc;
  int length, result;
  
  if (state->longest_path_length == ON_THE_PATH)
    /* We don't expect the path cycle here.  Our graph may contain
       only cycles with one state on the path not containing `cycle
       advance' arcs -- see comment below.  */
    abort ();
  else if (state->longest_path_length != UNDEFINED_LONGEST_PATH_LENGTH)
    /* We already visited the state.  */
    return state->longest_path_length;

  result = 0;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    /* Ignore cycles containing one state and `cycle advance' arcs.  */
    if (arc->to_state != state
	&& (arc->insn->insn_reserv_decl
	    != DECL_INSN_RESERV (advance_cycle_insn_decl)))
    {
      length = longest_path_length (arc->to_state);
      if (length > result)
	result = length;
    }
  state->longest_path_length = result + 1;
  return result;
}

/* The following variable value is value of the corresponding global
   variable in the automaton based pipeline interface.  */

static int max_dfa_issue_rate;

/* The following function processes the longest path length staring
   from STATE to find MAX_DFA_ISSUE_RATE.  */

static void
process_state_longest_path_length (state)
     state_t state;
{
  int value;

  value = longest_path_length (state);
  if (value > max_dfa_issue_rate)
    max_dfa_issue_rate = value;
}

/* The following macro value is name of the corresponding global
   variable in the automaton based pipeline interface.  */

#define MAX_DFA_ISSUE_RATE_VAR_NAME "max_dfa_issue_rate"

/* The following function calculates value of the corresponding
   global variable and outputs its declaration.  */

static void
output_dfa_max_issue_rate ()
{
  automaton_t automaton;

  if (UNDEFINED_LONGEST_PATH_LENGTH == ON_THE_PATH || ON_THE_PATH >= 0)
    abort ();
  max_dfa_issue_rate = 0;
  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    pass_states (automaton, process_state_longest_path_length);
  fprintf (output_file, "\nint %s = %d;\n",
	   MAX_DFA_ISSUE_RATE_VAR_NAME, max_dfa_issue_rate);
}

/* The function outputs all initialization values of VECT with length
   vect_length.  */
static void
output_vect (vect, vect_length)
     vect_el_t *vect;
     int vect_length;
{
  int els_on_line;

  els_on_line = 1;
  if (vect_length == 0)
    fprintf (output_file,
             "0 /* This is dummy el because the vect is empty */");
  else
    {
      do
        {
          fprintf (output_file, "%5ld", (long) *vect);
          vect_length--;
          if (els_on_line == 10)
	    {
	      els_on_line = 0;
	      fprintf (output_file, ",\n");
	    }
          else if (vect_length != 0)
            fprintf (output_file, ", ");
          els_on_line++;
          vect++;
        }
      while (vect_length != 0);
    }
}

/* The following is name of the structure which represents DFA(s) for
   PHR.  */
#define CHIP_NAME "DFA_chip"

/* The following is name of member which represents state of a DFA for
   PHR.  */
static void
output_chip_member_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "automaton_state_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_automaton_state",
             automaton->corresponding_automaton_decl->name);
}

/* The following is name of temporary variable which stores state of a
   DFA for PHR.  */
static void
output_temp_chip_member_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  fprintf (f, "_");
  output_chip_member_name (f, automaton);
}

/* This is name of macro value which is code of pseudo_insn
   representing advancing cpu cycle.  Its value is used as internal
   code unknown insn.  */
#define ADVANCE_CYCLE_VALUE_NAME "DFA__ADVANCE_CYCLE"

/* Output name of translate vector for given automaton.  */
static void
output_translate_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "translate_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_translate", automaton->corresponding_automaton_decl->name);
}

/* Output name for simple transition table representation.  */
static void
output_trans_full_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "transitions_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_transitions",
	     automaton->corresponding_automaton_decl->name);
}

/* Output name of comb vector of the transition table for given
   automaton.  */
static void
output_trans_comb_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "transitions_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_transitions",
             automaton->corresponding_automaton_decl->name);
}

/* Output name of check vector of the transition table for given
   automaton.  */
static void
output_trans_check_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "check_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_check", automaton->corresponding_automaton_decl->name);
}

/* Output name of base vector of the transition table for given
   automaton.  */
static void
output_trans_base_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "base_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_base", automaton->corresponding_automaton_decl->name);
}

/* Output name for simple alternatives number representation.  */
static void
output_state_alts_full_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "state_alts_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_state_alts",
             automaton->corresponding_automaton_decl->name);
}

/* Output name of comb vector of the alternatives number table for given
   automaton.  */
static void
output_state_alts_comb_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "state_alts_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_state_alts",
             automaton->corresponding_automaton_decl->name);
}

/* Output name of check vector of the alternatives number table for given
   automaton.  */
static void
output_state_alts_check_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "check_state_alts_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_check_state_alts",
	     automaton->corresponding_automaton_decl->name);
}

/* Output name of base vector of the alternatives number table for given
   automaton.  */
static void
output_state_alts_base_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "base_state_alts_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_base_state_alts",
	     automaton->corresponding_automaton_decl->name);
}

/* Output name of simple min issue delay table representation.  */
static void
output_min_issue_delay_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "min_issue_delay_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_min_issue_delay",
             automaton->corresponding_automaton_decl->name);
}

/* Output name of deadlock vector for given automaton.  */
static void
output_dead_lock_vect_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "dead_lock_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_dead_lock", automaton->corresponding_automaton_decl->name);
}

/* Output name of reserved units table for AUTOMATON into file F.  */
static void
output_reserved_units_table_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "reserved_units_%d", automaton->automaton_order_num);
  else
    fprintf (f, "%s_reserved_units",
	     automaton->corresponding_automaton_decl->name);
}

/* Name of the PHR interface macro.  */
#define AUTOMATON_STATE_ALTS_MACRO_NAME "AUTOMATON_STATE_ALTS"

/* Name of the PHR interface macro.  */
#define CPU_UNITS_QUERY_MACRO_NAME "CPU_UNITS_QUERY"

/* Names of an internal functions: */
#define INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME "internal_min_issue_delay"

/* This is external type of DFA(s) state.  */
#define STATE_TYPE_NAME "state_t"

#define INTERNAL_TRANSITION_FUNC_NAME "internal_state_transition"

#define INTERNAL_STATE_ALTS_FUNC_NAME "internal_state_alts"

#define INTERNAL_RESET_FUNC_NAME "internal_reset"

#define INTERNAL_DEAD_LOCK_FUNC_NAME "internal_state_dead_lock_p"

#define INTERNAL_INSN_LATENCY_FUNC_NAME "internal_insn_latency"

/* Name of cache of insn dfa codes.  */
#define DFA_INSN_CODES_VARIABLE_NAME "dfa_insn_codes"

/* Name of length of cache of insn dfa codes.  */
#define DFA_INSN_CODES_LENGTH_VARIABLE_NAME "dfa_insn_codes_length"

/* Names of the PHR interface functions: */
#define SIZE_FUNC_NAME "state_size"

#define TRANSITION_FUNC_NAME "state_transition"

#define STATE_ALTS_FUNC_NAME "state_alts"

#define MIN_ISSUE_DELAY_FUNC_NAME "min_issue_delay"

#define MIN_INSN_CONFLICT_DELAY_FUNC_NAME "min_insn_conflict_delay"

#define DEAD_LOCK_FUNC_NAME "state_dead_lock_p"

#define RESET_FUNC_NAME "state_reset"

#define INSN_LATENCY_FUNC_NAME "insn_latency"

#define PRINT_RESERVATION_FUNC_NAME "print_reservation"

#define GET_CPU_UNIT_CODE_FUNC_NAME "get_cpu_unit_code"

#define CPU_UNIT_RESERVATION_P_FUNC_NAME "cpu_unit_reservation_p"

#define DFA_CLEAN_INSN_CACHE_FUNC_NAME  "dfa_clean_insn_cache"

#define DFA_START_FUNC_NAME  "dfa_start"

#define DFA_FINISH_FUNC_NAME "dfa_finish"

/* Names of parameters of the PHR interface functions.  */
#define STATE_NAME "state"

#define INSN_PARAMETER_NAME "insn"

#define INSN2_PARAMETER_NAME "insn2"

#define CHIP_PARAMETER_NAME "chip"

#define FILE_PARAMETER_NAME "f"

#define CPU_UNIT_NAME_PARAMETER_NAME "cpu_unit_name"

#define CPU_CODE_PARAMETER_NAME "cpu_unit_code"

/* Names of the variables whose values are internal insn code of rtx
   insn.  */
#define INTERNAL_INSN_CODE_NAME "insn_code"

#define INTERNAL_INSN2_CODE_NAME "insn2_code"

/* Names of temporary variables in some functions.  */
#define TEMPORARY_VARIABLE_NAME "temp"

#define I_VARIABLE_NAME "i"

/* Name of result variable in some functions.  */
#define RESULT_VARIABLE_NAME "res"

/* Name of function (attribute) to translate insn into internal insn
   code.  */
#define INTERNAL_DFA_INSN_CODE_FUNC_NAME "internal_dfa_insn_code"

/* Name of function (attribute) to translate insn into internal insn
   code with caching.  */
#define DFA_INSN_CODE_FUNC_NAME "dfa_insn_code"

/* Name of function (attribute) to translate insn into internal insn
   code.  */
#define INSN_DEFAULT_LATENCY_FUNC_NAME "insn_default_latency"

/* Name of function (attribute) to translate insn into internal insn
   code.  */
#define BYPASS_P_FUNC_NAME "bypass_p"

/* Output C type which is used for representation of codes of states
   of AUTOMATON.  */
static void
output_state_member_type (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  output_range_type (f, 0, automaton->achieved_states_num);
}

/* Output definition of the structure representing current DFA(s)
   state(s).  */
static void
output_chip_definitions ()
{
  automaton_t automaton;

  fprintf (output_file, "struct %s\n{\n", CHIP_NAME);
  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      fprintf (output_file, "  ");
      output_state_member_type (output_file, automaton);
      fprintf (output_file, " ");
      output_chip_member_name (output_file, automaton);
      fprintf (output_file, ";\n");
    }
  fprintf (output_file, "};\n\n");
#if 0
  fprintf (output_file, "static struct %s %s;\n\n", CHIP_NAME, CHIP_NAME);
#endif
}


/* The function outputs translate vector of internal insn code into
   insn equivalence class number.  The equivalence class number is
   used to access to table and vectors representing DFA(s).  */
static void
output_translate_vect (automaton)
     automaton_t automaton;
{
  ainsn_t ainsn;
  int insn_value;
  vla_hwint_t translate_vect;

  VLA_HWINT_CREATE (translate_vect, 250, "translate vector");
  VLA_HWINT_EXPAND (translate_vect, description->insns_num);
  for (insn_value = 0; insn_value <= description->insns_num; insn_value++)
    /* Undefined value */
    VLA_HWINT (translate_vect, insn_value) = automaton->insn_equiv_classes_num;
  for (ainsn = automaton->ainsn_list; ainsn != NULL; ainsn = ainsn->next_ainsn)
    VLA_HWINT (translate_vect, ainsn->insn_reserv_decl->insn_num)
      = ainsn->insn_equiv_class_num;
  fprintf (output_file,
           "/* Vector translating external insn codes to internal ones.*/\n");
  fprintf (output_file, "static const ");
  output_range_type (output_file, 0, automaton->insn_equiv_classes_num);
  fprintf (output_file, " ");
  output_translate_vect_name (output_file, automaton);
  fprintf (output_file, "[] ATTRIBUTE_UNUSED = {\n");
  output_vect (VLA_HWINT_BEGIN (translate_vect),
	       VLA_HWINT_LENGTH (translate_vect));
  fprintf (output_file, "};\n\n");
  VLA_HWINT_DELETE (translate_vect);
}

/* The value in a table state x ainsn -> something which represents
   undefined value.  */
static int undefined_vect_el_value;

/* The following function returns nonzero value if the best
   representation of the table is comb vector.  */
static int
comb_vect_p (tab)
     state_ainsn_table_t tab;
{
  return  (2 * VLA_HWINT_LENGTH (tab->full_vect)
           > 5 * VLA_HWINT_LENGTH (tab->comb_vect));
}

/* The following function creates new table for AUTOMATON.  */
static state_ainsn_table_t
create_state_ainsn_table (automaton)
     automaton_t automaton;
{
  state_ainsn_table_t tab;
  int full_vect_length;
  int i;

  tab = create_node (sizeof (struct state_ainsn_table));
  tab->automaton = automaton;
  VLA_HWINT_CREATE (tab->comb_vect, 10000, "comb vector");
  VLA_HWINT_CREATE (tab->check_vect, 10000, "check vector");
  VLA_HWINT_CREATE (tab->base_vect, 1000, "base vector");
  VLA_HWINT_EXPAND (tab->base_vect, automaton->achieved_states_num);
  VLA_HWINT_CREATE (tab->full_vect, 10000, "full vector");
  full_vect_length = (automaton->insn_equiv_classes_num
                      * automaton->achieved_states_num);
  VLA_HWINT_EXPAND (tab->full_vect, full_vect_length);
  for (i = 0; i < full_vect_length; i++)
    VLA_HWINT (tab->full_vect, i) = undefined_vect_el_value;
  tab->min_base_vect_el_value = 0;
  tab->max_base_vect_el_value = 0;
  tab->min_comb_vect_el_value = 0;
  tab->max_comb_vect_el_value = 0;
  return tab;
}

/* The following function outputs the best C representation of the
   table TAB of given TABLE_NAME.  */
static void
output_state_ainsn_table (tab, table_name, output_full_vect_name_func,
                          output_comb_vect_name_func,
                          output_check_vect_name_func,
                          output_base_vect_name_func)
     state_ainsn_table_t tab;
     char *table_name;
     void (*output_full_vect_name_func) PARAMS ((FILE *, automaton_t));
     void (*output_comb_vect_name_func) PARAMS ((FILE *, automaton_t));
     void (*output_check_vect_name_func) PARAMS ((FILE *, automaton_t));
     void (*output_base_vect_name_func) PARAMS ((FILE *, automaton_t));
{
  if (!comb_vect_p (tab))
    {
      fprintf (output_file, "/* Vector for %s.  */\n", table_name);
      fprintf (output_file, "static const ");
      output_range_type (output_file, tab->min_comb_vect_el_value,
                         tab->max_comb_vect_el_value);
      fprintf (output_file, " ");
      (*output_full_vect_name_func) (output_file, tab->automaton);
      fprintf (output_file, "[] ATTRIBUTE_UNUSED = {\n");
      output_vect (VLA_HWINT_BEGIN (tab->full_vect),
                   VLA_HWINT_LENGTH (tab->full_vect));
      fprintf (output_file, "};\n\n");
    }
  else
    {
      fprintf (output_file, "/* Comb vector for %s.  */\n", table_name);
      fprintf (output_file, "static const ");
      output_range_type (output_file, tab->min_comb_vect_el_value,
                         tab->max_comb_vect_el_value);
      fprintf (output_file, " ");
      (*output_comb_vect_name_func) (output_file, tab->automaton);
      fprintf (output_file, "[] ATTRIBUTE_UNUSED = {\n");
      output_vect (VLA_HWINT_BEGIN (tab->comb_vect),
                   VLA_HWINT_LENGTH (tab->comb_vect));
      fprintf (output_file, "};\n\n");
      fprintf (output_file, "/* Check vector for %s.  */\n", table_name);
      fprintf (output_file, "static const ");
      output_range_type (output_file, 0, tab->automaton->achieved_states_num);
      fprintf (output_file, " ");
      (*output_check_vect_name_func) (output_file, tab->automaton);
      fprintf (output_file, "[] = {\n");
      output_vect (VLA_HWINT_BEGIN (tab->check_vect),
                   VLA_HWINT_LENGTH (tab->check_vect));
      fprintf (output_file, "};\n\n");
      fprintf (output_file, "/* Base vector for %s.  */\n", table_name);
      fprintf (output_file, "static const ");
      output_range_type (output_file, tab->min_base_vect_el_value,
                         tab->max_base_vect_el_value);
      fprintf (output_file, " ");
      (*output_base_vect_name_func) (output_file, tab->automaton);
      fprintf (output_file, "[] = {\n");
      output_vect (VLA_HWINT_BEGIN (tab->base_vect),
                   VLA_HWINT_LENGTH (tab->base_vect));
      fprintf (output_file, "};\n\n");
    }
}

/* The following function adds vector with length VECT_LENGTH and
   elements pointed by VECT to table TAB as its line with number
   VECT_NUM.  */
static void
add_vect (tab, vect_num, vect, vect_length)
     state_ainsn_table_t tab;
     int vect_num;
     vect_el_t *vect;
     int vect_length;
{
  int real_vect_length;
  vect_el_t *comb_vect_start;
  vect_el_t *check_vect_start;
  int comb_vect_index;
  int comb_vect_els_num;
  int vect_index;
  int first_unempty_vect_index;
  int additional_els_num;
  int no_state_value;
  vect_el_t vect_el;
  int i;

  if (vect_length == 0)
    abort ();
  real_vect_length = tab->automaton->insn_equiv_classes_num;
  if (vect [vect_length - 1] == undefined_vect_el_value)
    abort ();
  /* Form full vector in the table: */
  for (i = 0; i < vect_length; i++)
    VLA_HWINT (tab->full_vect,
               i + tab->automaton->insn_equiv_classes_num * vect_num)
      = vect [i];
  /* Form comb vector in the table: */
  if (VLA_HWINT_LENGTH (tab->comb_vect) != VLA_HWINT_LENGTH (tab->check_vect))
    abort ();
  comb_vect_start = VLA_HWINT_BEGIN (tab->comb_vect);
  comb_vect_els_num = VLA_HWINT_LENGTH (tab->comb_vect);
  for (first_unempty_vect_index = 0;
       first_unempty_vect_index < vect_length;
       first_unempty_vect_index++)
    if (vect [first_unempty_vect_index] != undefined_vect_el_value)
      break;
  /* Search for the place in comb vect for the inserted vect.  */
  for (comb_vect_index = 0;
       comb_vect_index < comb_vect_els_num;
       comb_vect_index++)
    {
      for (vect_index = first_unempty_vect_index;
           vect_index < vect_length
             && vect_index + comb_vect_index < comb_vect_els_num;
           vect_index++)
        if (vect [vect_index] != undefined_vect_el_value
            && (comb_vect_start [vect_index + comb_vect_index]
		!= undefined_vect_el_value))
          break;
      if (vect_index >= vect_length
          || vect_index + comb_vect_index >= comb_vect_els_num)
        break;
    }
  /* Slot was found.  */
  additional_els_num = comb_vect_index + real_vect_length - comb_vect_els_num;
  if (additional_els_num < 0)
    additional_els_num = 0;
  /* Expand comb and check vectors.  */
  vect_el = undefined_vect_el_value;
  no_state_value = tab->automaton->achieved_states_num;
  while (additional_els_num > 0)
    {
      VLA_HWINT_ADD (tab->comb_vect, vect_el);
      VLA_HWINT_ADD (tab->check_vect, no_state_value);
      additional_els_num--;
    }
  comb_vect_start = VLA_HWINT_BEGIN (tab->comb_vect);
  check_vect_start = VLA_HWINT_BEGIN (tab->check_vect);
  if (VLA_HWINT_LENGTH (tab->comb_vect)
      < (size_t) (comb_vect_index + real_vect_length))
    abort ();
  /* Fill comb and check vectors.  */
  for (vect_index = 0; vect_index < vect_length; vect_index++)
    if (vect [vect_index] != undefined_vect_el_value)
      {
        if (comb_vect_start [comb_vect_index + vect_index]
	    != undefined_vect_el_value)
	  abort ();
        comb_vect_start [comb_vect_index + vect_index] = vect [vect_index];
        if (vect [vect_index] < 0)
	  abort ();
        if (tab->max_comb_vect_el_value < vect [vect_index])
          tab->max_comb_vect_el_value = vect [vect_index];
        if (tab->min_comb_vect_el_value > vect [vect_index])
          tab->min_comb_vect_el_value = vect [vect_index];
        check_vect_start [comb_vect_index + vect_index] = vect_num;
      }
  if (tab->max_base_vect_el_value < comb_vect_index)
    tab->max_base_vect_el_value = comb_vect_index;
  if (tab->min_base_vect_el_value > comb_vect_index)
    tab->min_base_vect_el_value = comb_vect_index;
  VLA_HWINT (tab->base_vect, vect_num) = comb_vect_index;
}

/* Return number of out arcs of STATE.  */
static int
out_state_arcs_num (state)
     state_t state;
{
  int result;
  arc_t arc;

  result = 0;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    {
      if (arc->insn == NULL)
	abort ();
      if (arc->insn->first_ainsn_with_given_equialence_num)
        result++;
    }
  return result;
}

/* Compare number of possible transitions from the states.  */
static int
compare_transition_els_num (state_ptr_1, state_ptr_2)
     const void *state_ptr_1;
     const void *state_ptr_2;
{
  int transition_els_num_1;
  int transition_els_num_2;

  transition_els_num_1 = out_state_arcs_num (*(state_t *) state_ptr_1);
  transition_els_num_2 = out_state_arcs_num (*(state_t *) state_ptr_2);
  if (transition_els_num_1 < transition_els_num_2)
    return 1;
  else if (transition_els_num_1 == transition_els_num_2)
    return 0;
  else
    return -1;
}

/* The function adds element EL_VALUE to vector VECT for a table state
   x AINSN.  */
static void
add_vect_el (vect, ainsn, el_value)
     vla_hwint_t *vect;
     ainsn_t ainsn;
     int el_value;
{
  int equiv_class_num;
  int vect_index;

  if (ainsn == NULL)
    abort ();
  equiv_class_num = ainsn->insn_equiv_class_num;
  for (vect_index = VLA_HWINT_LENGTH (*vect);
       vect_index <= equiv_class_num;
       vect_index++)
    VLA_HWINT_ADD (*vect, undefined_vect_el_value);
  VLA_HWINT (*vect, equiv_class_num) = el_value;
}

/* This is for forming vector of states of an automaton.  */
static vla_ptr_t output_states_vect;

/* The function is called by function pass_states.  The function adds
   STATE to `output_states_vect'.  */
static void
add_states_vect_el (state)
     state_t state;
{
  VLA_PTR_ADD (output_states_vect, state);
}

/* Form and output vectors (comb, check, base or full vector)
   representing transition table of AUTOMATON.  */
static void
output_trans_table (automaton)
     automaton_t automaton;
{
  state_t *state_ptr;
  arc_t arc;
  vla_hwint_t transition_vect;

  undefined_vect_el_value = automaton->achieved_states_num;
  automaton->trans_table = create_state_ainsn_table (automaton);
  /* Create vect of pointers to states ordered by num of transitions
     from the state (state with the maximum num is the first).  */
  VLA_PTR_CREATE (output_states_vect, 1500, "output states vector");
  pass_states (automaton, add_states_vect_el);
  qsort (VLA_PTR_BEGIN (output_states_vect),
         VLA_PTR_LENGTH (output_states_vect),
         sizeof (state_t), compare_transition_els_num);
  VLA_HWINT_CREATE (transition_vect, 500, "transition vector");
  for (state_ptr = VLA_PTR_BEGIN (output_states_vect);
       state_ptr <= (state_t *) VLA_PTR_LAST (output_states_vect);
       state_ptr++)
    {
      VLA_HWINT_NULLIFY (transition_vect);
      for (arc = first_out_arc (*state_ptr);
	   arc != NULL;
	   arc = next_out_arc (arc))
        {
          if (arc->insn == NULL)
	    abort ();
          if (arc->insn->first_ainsn_with_given_equialence_num)
            add_vect_el (&transition_vect, arc->insn,
                         arc->to_state->order_state_num);
        }
      add_vect (automaton->trans_table, (*state_ptr)->order_state_num,
                VLA_HWINT_BEGIN (transition_vect),
                VLA_HWINT_LENGTH (transition_vect));
    }
  output_state_ainsn_table
    (automaton->trans_table, (char *) "state transitions",
     output_trans_full_vect_name, output_trans_comb_vect_name,
     output_trans_check_vect_name, output_trans_base_vect_name);
  VLA_PTR_DELETE (output_states_vect);
  VLA_HWINT_DELETE (transition_vect);
}

/* Form and output vectors (comb, check, base or simple vect)
   representing alts number table of AUTOMATON.  The table is state x
   ainsn -> number of possible alternative reservations by the
   ainsn.  */
static void
output_state_alts_table (automaton)
     automaton_t automaton;
{
  state_t *state_ptr;
  arc_t arc;
  vla_hwint_t state_alts_vect;

  undefined_vect_el_value = 0; /* no alts when transition is not possible */
  automaton->state_alts_table = create_state_ainsn_table (automaton);
  /* Create vect of pointers to states ordered by num of transitions
     from the state (state with the maximum num is the first).  */
  VLA_PTR_CREATE (output_states_vect, 1500, "output states vector");
  pass_states (automaton, add_states_vect_el);
  qsort (VLA_PTR_BEGIN (output_states_vect),
         VLA_PTR_LENGTH (output_states_vect),
         sizeof (state_t), compare_transition_els_num);
  /* Create base, comb, and check vectors.  */
  VLA_HWINT_CREATE (state_alts_vect, 500, "state alts vector");
  for (state_ptr = VLA_PTR_BEGIN (output_states_vect);
       state_ptr <= (state_t *) VLA_PTR_LAST (output_states_vect);
       state_ptr++)
    {
      VLA_HWINT_NULLIFY (state_alts_vect);
      for (arc = first_out_arc (*state_ptr);
	   arc != NULL;
	   arc = next_out_arc (arc))
        {
          if (arc->insn == NULL)
	    abort ();
          if (arc->insn->first_ainsn_with_given_equialence_num)
            add_vect_el (&state_alts_vect, arc->insn, arc->state_alts);
        }
      add_vect (automaton->state_alts_table, (*state_ptr)->order_state_num,
                VLA_HWINT_BEGIN (state_alts_vect),
                VLA_HWINT_LENGTH (state_alts_vect));
    }
  output_state_ainsn_table
    (automaton->state_alts_table, (char *) "state insn alternatives",
     output_state_alts_full_vect_name, output_state_alts_comb_vect_name,
     output_state_alts_check_vect_name, output_state_alts_base_vect_name);
  VLA_PTR_DELETE (output_states_vect);
  VLA_HWINT_DELETE (state_alts_vect);
}

/* The current number of passing states to find minimal issue delay
   value for an ainsn and state.  */
static int curr_state_pass_num;

/* This recursive function passes states to find minimal issue delay
   value for AINSN.  The state being visited is STATE.  The function
   returns minimal issue delay value for AINSN in STATE or -1 if we
   enter into a loop.  */
static int
min_issue_delay_pass_states (state, ainsn)
     state_t state;
     ainsn_t ainsn;
{
  arc_t arc;
  int min_insn_issue_delay, insn_issue_delay;

  if (state->state_pass_num == curr_state_pass_num
      || state->min_insn_issue_delay != -1)
    /* We've entered into a loop or already have the correct value for
       given state and ainsn.  */
    return state->min_insn_issue_delay;
  state->state_pass_num = curr_state_pass_num;
  min_insn_issue_delay = -1;
  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    if (arc->insn == ainsn)
      {
	min_insn_issue_delay = 0;
	break;
      }
    else
      {
        insn_issue_delay = min_issue_delay_pass_states (arc->to_state, ainsn);
	if (insn_issue_delay != -1)
	  {
	    if (arc->insn->insn_reserv_decl
		== DECL_INSN_RESERV (advance_cycle_insn_decl))
	      insn_issue_delay++;
	    if (min_insn_issue_delay == -1
		|| min_insn_issue_delay > insn_issue_delay)
	      {
		min_insn_issue_delay = insn_issue_delay;
		if (insn_issue_delay == 0)
		  break;
	      }
	  }
      }
  return min_insn_issue_delay;
}

/* The function searches minimal issue delay value for AINSN in STATE.
   The function can return negative value if we can not issue AINSN.  We
   will report about it later.  */
static int
min_issue_delay (state, ainsn)
     state_t state;
     ainsn_t ainsn;
{
  curr_state_pass_num++;
  state->min_insn_issue_delay = min_issue_delay_pass_states (state, ainsn);
  return state->min_insn_issue_delay;
}

/* The function initiates code for finding minimal issue delay values.
   It should be called only once.  */
static void
initiate_min_issue_delay_pass_states ()
{
  curr_state_pass_num = 0;
}

/* Form and output vectors representing minimal issue delay table of
   AUTOMATON.  The table is state x ainsn -> minimal issue delay of
   the ainsn.  */
static void
output_min_issue_delay_table (automaton)
     automaton_t automaton;
{
  vla_hwint_t min_issue_delay_vect;
  vla_hwint_t compressed_min_issue_delay_vect;
  vect_el_t min_delay;
  ainsn_t ainsn;
  state_t *state_ptr;
  int i;

  /* Create vect of pointers to states ordered by num of transitions
     from the state (state with the maximum num is the first).  */
  VLA_PTR_CREATE (output_states_vect, 1500, "output states vector");
  pass_states (automaton, add_states_vect_el);
  VLA_HWINT_CREATE (min_issue_delay_vect, 1500, "min issue delay vector");
  VLA_HWINT_EXPAND (min_issue_delay_vect,
		    VLA_HWINT_LENGTH (output_states_vect)
		    * automaton->insn_equiv_classes_num);
  for (i = 0;
       i < ((int) VLA_HWINT_LENGTH (output_states_vect)
	    * automaton->insn_equiv_classes_num);
       i++)
    VLA_HWINT (min_issue_delay_vect, i) = 0;
  automaton->max_min_delay = 0;
  for (ainsn = automaton->ainsn_list; ainsn != NULL; ainsn = ainsn->next_ainsn)
    if (ainsn->first_ainsn_with_given_equialence_num)
      {
	for (state_ptr = VLA_PTR_BEGIN (output_states_vect);
	     state_ptr <= (state_t *) VLA_PTR_LAST (output_states_vect);
	     state_ptr++)
	  (*state_ptr)->min_insn_issue_delay = -1;
	for (state_ptr = VLA_PTR_BEGIN (output_states_vect);
	     state_ptr <= (state_t *) VLA_PTR_LAST (output_states_vect);
	     state_ptr++)
	  {
            min_delay = min_issue_delay (*state_ptr, ainsn);
	    if (automaton->max_min_delay < min_delay)
	      automaton->max_min_delay = min_delay;
	    VLA_HWINT (min_issue_delay_vect,
		       (*state_ptr)->order_state_num
		       * automaton->insn_equiv_classes_num
		       + ainsn->insn_equiv_class_num) = min_delay;
	  }
      }
  fprintf (output_file, "/* Vector of min issue delay of insns.  */\n");
  fprintf (output_file, "static const ");
  output_range_type (output_file, 0, automaton->max_min_delay);
  fprintf (output_file, " ");
  output_min_issue_delay_vect_name (output_file, automaton);
  fprintf (output_file, "[] ATTRIBUTE_UNUSED = {\n");
  /* Compress the vector */
  if (automaton->max_min_delay < 2)
    automaton->min_issue_delay_table_compression_factor = 8;
  else if (automaton->max_min_delay < 4)
    automaton->min_issue_delay_table_compression_factor = 4;
  else if (automaton->max_min_delay < 16)
    automaton->min_issue_delay_table_compression_factor = 2;
  else
    automaton->min_issue_delay_table_compression_factor = 1;
  VLA_HWINT_CREATE (compressed_min_issue_delay_vect, 1500,
		    "compressed min issue delay vector");
  VLA_HWINT_EXPAND (compressed_min_issue_delay_vect,
		    (VLA_HWINT_LENGTH (min_issue_delay_vect)
		     + automaton->min_issue_delay_table_compression_factor
		     - 1)
		    / automaton->min_issue_delay_table_compression_factor);
  for (i = 0;
       i < (int) VLA_HWINT_LENGTH (compressed_min_issue_delay_vect);
       i++)
    VLA_HWINT (compressed_min_issue_delay_vect, i) = 0;
  for (i = 0; i < (int) VLA_HWINT_LENGTH (min_issue_delay_vect); i++)
    VLA_HWINT (compressed_min_issue_delay_vect,
	       i / automaton->min_issue_delay_table_compression_factor)
      |= (VLA_HWINT (min_issue_delay_vect, i)
	  << (8 - (i % automaton->min_issue_delay_table_compression_factor
		   + 1)
	      * (8 / automaton->min_issue_delay_table_compression_factor)));
  output_vect (VLA_HWINT_BEGIN (compressed_min_issue_delay_vect),
               VLA_HWINT_LENGTH (compressed_min_issue_delay_vect));
  fprintf (output_file, "};\n\n");
  VLA_PTR_DELETE (output_states_vect);
  VLA_HWINT_DELETE (min_issue_delay_vect);
  VLA_HWINT_DELETE (compressed_min_issue_delay_vect);
}

#ifndef NDEBUG
/* Number of states which contains transition only by advancing cpu
   cycle.  */
static int locked_states_num;
#endif

/* Form and output vector representing the locked states of
   AUTOMATON.  */
static void
output_dead_lock_vect (automaton)
     automaton_t automaton;
{
  state_t *state_ptr;
  arc_t arc;
  vla_hwint_t dead_lock_vect;

  /* Create vect of pointers to states ordered by num of
     transitions from the state (state with the maximum num is the
     first).  */
  VLA_PTR_CREATE (output_states_vect, 1500, "output states vector");
  pass_states (automaton, add_states_vect_el);
  VLA_HWINT_CREATE (dead_lock_vect, 1500, "is dead locked vector");
  VLA_HWINT_EXPAND (dead_lock_vect, VLA_HWINT_LENGTH (output_states_vect));
  for (state_ptr = VLA_PTR_BEGIN (output_states_vect);
       state_ptr <= (state_t *) VLA_PTR_LAST (output_states_vect);
       state_ptr++)
    {
      arc = first_out_arc (*state_ptr);
      if (arc == NULL)
	abort ();
      VLA_HWINT (dead_lock_vect, (*state_ptr)->order_state_num)
        = (next_out_arc (arc) == NULL
           && (arc->insn->insn_reserv_decl
               == DECL_INSN_RESERV (advance_cycle_insn_decl)) ? 1 : 0);
#ifndef NDEBUG
      if (VLA_HWINT (dead_lock_vect, (*state_ptr)->order_state_num))
        locked_states_num++;
#endif
    }
  fprintf (output_file, "/* Vector for locked state flags.  */\n");
  fprintf (output_file, "static const ");
  output_range_type (output_file, 0, 1);
  fprintf (output_file, " ");
  output_dead_lock_vect_name (output_file, automaton);
  fprintf (output_file, "[] = {\n");
  output_vect (VLA_HWINT_BEGIN (dead_lock_vect),
	       VLA_HWINT_LENGTH (dead_lock_vect));
  fprintf (output_file, "};\n\n");
  VLA_HWINT_DELETE (dead_lock_vect);
  VLA_PTR_DELETE (output_states_vect);
}

/* Form and output vector representing reserved units of the states of
   AUTOMATON.  */
static void
output_reserved_units_table (automaton)
     automaton_t automaton;
{
  state_t *curr_state_ptr;
  vla_hwint_t reserved_units_table;
  size_t state_byte_size;
  int i;

  /* Create vect of pointers to states.  */
  VLA_PTR_CREATE (output_states_vect, 1500, "output states vector");
  pass_states (automaton, add_states_vect_el);
  /* Create vector.  */
  VLA_HWINT_CREATE (reserved_units_table, 1500, "reserved units vector");
  state_byte_size = (description->query_units_num + 7) / 8;
  VLA_HWINT_EXPAND (reserved_units_table,
		    VLA_HWINT_LENGTH (output_states_vect) * state_byte_size);
  for (i = 0;
       i < (int) (VLA_HWINT_LENGTH (output_states_vect) * state_byte_size);
       i++)
    VLA_HWINT (reserved_units_table, i) = 0;
  for (curr_state_ptr = VLA_PTR_BEGIN (output_states_vect);
       curr_state_ptr <= (state_t *) VLA_PTR_LAST (output_states_vect);
       curr_state_ptr++)
    {
      for (i = 0; i < description->units_num; i++)
	if (units_array [i]->query_p
	    && first_cycle_unit_presence (*curr_state_ptr, i))
	  VLA_HWINT (reserved_units_table,
		     (*curr_state_ptr)->order_state_num * state_byte_size
		     + units_array [i]->query_num / 8)
	    += (1 << (units_array [i]->query_num % 8));
    }
  fprintf (output_file, "/* Vector for reserved units of states.  */\n");
  fprintf (output_file, "static const ");
  output_range_type (output_file, 0, 255);
  fprintf (output_file, " ");
  output_reserved_units_table_name (output_file, automaton);
  fprintf (output_file, "[] = {\n");
  output_vect (VLA_HWINT_BEGIN (reserved_units_table),
               VLA_HWINT_LENGTH (reserved_units_table));
  fprintf (output_file, "};\n\n");
  VLA_HWINT_DELETE (reserved_units_table);
  VLA_PTR_DELETE (output_states_vect);
}

/* The function outputs all tables representing DFA(s) used for fast
   pipeline hazards recognition.  */
static void
output_tables ()
{
  automaton_t automaton;

#ifndef NDEBUG
  locked_states_num = 0;
#endif
  initiate_min_issue_delay_pass_states ();
  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      output_translate_vect (automaton);
      output_trans_table (automaton);
      fprintf (output_file, "\n#if %s\n", AUTOMATON_STATE_ALTS_MACRO_NAME);
      output_state_alts_table (automaton);
      fprintf (output_file, "\n#endif /* #if %s */\n\n",
	       AUTOMATON_STATE_ALTS_MACRO_NAME);
      output_min_issue_delay_table (automaton);
      output_dead_lock_vect (automaton);
      fprintf (output_file, "\n#if %s\n\n", CPU_UNITS_QUERY_MACRO_NAME);
      output_reserved_units_table (automaton);
      fprintf (output_file, "\n#endif /* #if %s */\n\n",
	       CPU_UNITS_QUERY_MACRO_NAME);
    }
  fprintf (output_file, "\n#define %s %d\n\n", ADVANCE_CYCLE_VALUE_NAME,
           DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num);
}

/* The function outputs definition and value of PHR interface variable
   `max_insn_queue_index'.  Its value is not less than maximal queue
   length needed for the insn scheduler.  */
static void
output_max_insn_queue_index_def ()
{
  int i, max, latency;
  decl_t decl;

  max = description->max_insn_reserv_cycles;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
	{
	  latency = DECL_INSN_RESERV (decl)->default_latency;
	  if (latency > max)
	    max = latency;
	}
      else if (decl->mode == dm_bypass)
	{
	  latency = DECL_BYPASS (decl)->latency;
	  if (latency > max)
	    max = latency;
	}
    }
  for (i = 0; (1 << i) <= max; i++)
    ;
  if (i < 0)
    abort ();
  fprintf (output_file, "\nint max_insn_queue_index = %d;\n\n", (1 << i) - 1);
}


/* The function outputs switch cases for insn reservations using
   function *output_automata_list_code.  */
static void
output_insn_code_cases (output_automata_list_code)
     void (*output_automata_list_code) PARAMS ((automata_list_el_t));
{
  decl_t decl, decl2;
  int i, j;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
	DECL_INSN_RESERV (decl)->processed_p = FALSE;
    }
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv
	  && !DECL_INSN_RESERV (decl)->processed_p)
	{
	  for (j = i; j < description->decls_num; j++)
	    {
	      decl2 = description->decls [j];
	      if (decl2->mode == dm_insn_reserv
		  && (DECL_INSN_RESERV (decl2)->important_automata_list
		      == DECL_INSN_RESERV (decl)->important_automata_list))
		{
		  DECL_INSN_RESERV (decl2)->processed_p = TRUE;
		  fprintf (output_file, "    case %d: /* %s */\n",
			   DECL_INSN_RESERV (decl2)->insn_num,
			   DECL_INSN_RESERV (decl2)->name);
		}
	    }
	  (*output_automata_list_code)
	    (DECL_INSN_RESERV (decl)->important_automata_list);
	}
    }
}


/* The function outputs a code for evaluation of a minimal delay of
   issue of insns which have reservations in given AUTOMATA_LIST.  */
static void
output_automata_list_min_issue_delay_code (automata_list)
     automata_list_el_t automata_list;
{
  automata_list_el_t el;
  automaton_t automaton;

  for (el = automata_list; el != NULL; el = el->next_automata_list_el)
    {
      automaton = el->automaton;
      fprintf (output_file, "\n      %s = ", TEMPORARY_VARIABLE_NAME);
      output_min_issue_delay_vect_name (output_file, automaton);
      fprintf (output_file,
	       (automaton->min_issue_delay_table_compression_factor != 1
		? " [(" : " ["));
      output_translate_vect_name (output_file, automaton);
      fprintf (output_file, " [%s] + ", INTERNAL_INSN_CODE_NAME);
      fprintf (output_file, "%s->", CHIP_PARAMETER_NAME);
      output_chip_member_name (output_file, automaton);
      fprintf (output_file, " * %d", automaton->insn_equiv_classes_num);
      if (automaton->min_issue_delay_table_compression_factor == 1)
	fprintf (output_file, "];\n");
      else
	{
	  fprintf (output_file, ") / %d];\n",
		   automaton->min_issue_delay_table_compression_factor);
	  fprintf (output_file, "      %s = (%s >> (8 - (",
		   TEMPORARY_VARIABLE_NAME, TEMPORARY_VARIABLE_NAME);
	  output_translate_vect_name (output_file, automaton);
	  fprintf
	    (output_file, " [%s] %% %d + 1) * %d)) & %d;\n",
	     INTERNAL_INSN_CODE_NAME,
	     automaton->min_issue_delay_table_compression_factor,
	     8 / automaton->min_issue_delay_table_compression_factor,
	     (1 << (8 / automaton->min_issue_delay_table_compression_factor))
	     - 1);
	}
      if (el == automata_list)
	fprintf (output_file, "      %s = %s;\n",
		 RESULT_VARIABLE_NAME, TEMPORARY_VARIABLE_NAME);
      else
	{
	  fprintf (output_file, "      if (%s > %s)\n",
		   TEMPORARY_VARIABLE_NAME, RESULT_VARIABLE_NAME);
	  fprintf (output_file, "        %s = %s;\n",
		   RESULT_VARIABLE_NAME, TEMPORARY_VARIABLE_NAME);
	}
    }
  fprintf (output_file, "      break;\n\n");
}

/* Output function `internal_min_issue_delay'.  */
static void
output_internal_min_issue_delay_func ()
{
  fprintf (output_file, "static int %s PARAMS ((int, struct %s *));\n",
	   INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME, CHIP_NAME);
  fprintf (output_file,
	   "static int\n%s (%s, %s)\n\tint %s;\n\tstruct %s *%s  ATTRIBUTE_UNUSED;\n",
	   INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
	   CHIP_PARAMETER_NAME, INTERNAL_INSN_CODE_NAME, CHIP_NAME,
	   CHIP_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s ATTRIBUTE_UNUSED;\n  int %s = -1;\n",
	   TEMPORARY_VARIABLE_NAME, RESULT_VARIABLE_NAME);
  fprintf (output_file, "\n  switch (%s)\n    {\n", INTERNAL_INSN_CODE_NAME);
  output_insn_code_cases (output_automata_list_min_issue_delay_code);
  fprintf (output_file,
	   "\n    default:\n      %s = -1;\n      break;\n    }\n",
	   RESULT_VARIABLE_NAME);
  fprintf (output_file, "  return %s;\n", RESULT_VARIABLE_NAME);
  fprintf (output_file, "}\n\n");
}

/* The function outputs a code changing state after issue of insns
   which have reservations in given AUTOMATA_LIST.  */
static void
output_automata_list_transition_code (automata_list)
     automata_list_el_t automata_list;
{
  automata_list_el_t el, next_el;

  fprintf (output_file, "      {\n");
  if (automata_list != NULL && automata_list->next_automata_list_el != NULL)
    for (el = automata_list;; el = next_el)
      {
        next_el = el->next_automata_list_el;
        if (next_el == NULL)
          break;
        fprintf (output_file, "        ");
        output_state_member_type (output_file, el->automaton);
	fprintf (output_file, " ");
        output_temp_chip_member_name (output_file, el->automaton);
        fprintf (output_file, ";\n");
      }
  for (el = automata_list; el != NULL; el = el->next_automata_list_el)
    if (comb_vect_p (el->automaton->trans_table))
      {
	fprintf (output_file, "\n        %s = ", TEMPORARY_VARIABLE_NAME);
	output_trans_base_vect_name (output_file, el->automaton);
	fprintf (output_file, " [%s->", CHIP_PARAMETER_NAME);
	output_chip_member_name (output_file, el->automaton);
	fprintf (output_file, "] + ");
	output_translate_vect_name (output_file, el->automaton);
	fprintf (output_file, " [%s];\n", INTERNAL_INSN_CODE_NAME);
	fprintf (output_file, "        if (");
	output_trans_check_vect_name (output_file, el->automaton);
	fprintf (output_file, " [%s] != %s->",
		 TEMPORARY_VARIABLE_NAME, CHIP_PARAMETER_NAME);
	output_chip_member_name (output_file, el->automaton);
	fprintf (output_file, ")\n");
	fprintf (output_file, "          return %s (%s, %s);\n",
		 INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
		 CHIP_PARAMETER_NAME);
	fprintf (output_file, "        else\n");
	fprintf (output_file, "          ");
	if (el->next_automata_list_el != NULL)
	  output_temp_chip_member_name (output_file, el->automaton);
	else
	  {
	    fprintf (output_file, "%s->", CHIP_PARAMETER_NAME);
	    output_chip_member_name (output_file, el->automaton);
	  }
	fprintf (output_file, " = ");
	output_trans_comb_vect_name (output_file, el->automaton);
	fprintf (output_file, " [%s];\n", TEMPORARY_VARIABLE_NAME);
      }
    else
      {
	fprintf (output_file, "\n        %s = ", TEMPORARY_VARIABLE_NAME);
	output_trans_full_vect_name (output_file, el->automaton);
	fprintf (output_file, " [");
	output_translate_vect_name (output_file, el->automaton);
	fprintf (output_file, " [%s] + ", INTERNAL_INSN_CODE_NAME);
	fprintf (output_file, "%s->", CHIP_PARAMETER_NAME);
	output_chip_member_name (output_file, el->automaton);
	fprintf (output_file, " * %d];\n",
		 el->automaton->insn_equiv_classes_num);
	fprintf (output_file, "        if (%s >= %d)\n",
		 TEMPORARY_VARIABLE_NAME, el->automaton->achieved_states_num);
	fprintf (output_file, "          return %s (%s, %s);\n",
		 INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
		 CHIP_PARAMETER_NAME);
	fprintf (output_file, "        else\n          ");
	if (el->next_automata_list_el != NULL)
	  output_temp_chip_member_name (output_file, el->automaton);
	else
	  {
	    fprintf (output_file, "%s->", CHIP_PARAMETER_NAME);
	    output_chip_member_name (output_file, el->automaton);
	  }
	fprintf (output_file, " = %s;\n", TEMPORARY_VARIABLE_NAME);
      }
  if (automata_list != NULL && automata_list->next_automata_list_el != NULL)
    for (el = automata_list;; el = next_el)
      {
        next_el = el->next_automata_list_el;
        if (next_el == NULL)
          break;
        fprintf (output_file, "        %s->", CHIP_PARAMETER_NAME);
        output_chip_member_name (output_file, el->automaton);
        fprintf (output_file, " = ");
        output_temp_chip_member_name (output_file, el->automaton);
        fprintf (output_file, ";\n");
      }
  fprintf (output_file, "        return -1;\n");
  fprintf (output_file, "      }\n");
}

/* Output function `internal_state_transition'.  */
static void
output_internal_trans_func ()
{
  fprintf (output_file, "static int %s PARAMS ((int, struct %s *));\n",
	   INTERNAL_TRANSITION_FUNC_NAME, CHIP_NAME);
  fprintf (output_file,
	   "static int\n%s (%s, %s)\n\tint %s;\n\tstruct %s *%s  ATTRIBUTE_UNUSED;\n",
	   INTERNAL_TRANSITION_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
	   CHIP_PARAMETER_NAME, INTERNAL_INSN_CODE_NAME,
	   CHIP_NAME, CHIP_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s ATTRIBUTE_UNUSED;\n", TEMPORARY_VARIABLE_NAME);
  fprintf (output_file, "\n  switch (%s)\n    {\n", INTERNAL_INSN_CODE_NAME);
  output_insn_code_cases (output_automata_list_transition_code);
  fprintf (output_file, "\n    default:\n      return -1;\n    }\n");
  fprintf (output_file, "}\n\n");
}

/* Output code

  if (insn != 0)
    {
      insn_code = dfa_insn_code (insn);
      if (insn_code > DFA__ADVANCE_CYCLE)
        return code;
    }
  else
    insn_code = DFA__ADVANCE_CYCLE;

  where insn denotes INSN_NAME, insn_code denotes INSN_CODE_NAME, and
  code denotes CODE.  */
static void
output_internal_insn_code_evaluation (insn_name, insn_code_name, code)
     const char *insn_name;
     const char *insn_code_name;
     int code;
{
  fprintf (output_file, "\n  if (%s != 0)\n    {\n", insn_name);
  fprintf (output_file, "      %s = %s (%s);\n", insn_code_name,
	   DFA_INSN_CODE_FUNC_NAME, insn_name);
  fprintf (output_file, "      if (%s > %s)\n        return %d;\n",
	   insn_code_name, ADVANCE_CYCLE_VALUE_NAME, code);
  fprintf (output_file, "    }\n  else\n    %s = %s;\n\n",
	   insn_code_name, ADVANCE_CYCLE_VALUE_NAME);
}


/* This function outputs `dfa_insn_code' and its helper function
   `dfa_insn_code_enlarge'.  */
static void
output_dfa_insn_code_func ()
{
  /* Emacs c-mode gets really confused if there's a { or } in column 0
     inside a string, so don't do that.  */
  fprintf (output_file, "\
static void dfa_insn_code_enlarge PARAMS ((int));\n\
static void\n\
dfa_insn_code_enlarge (uid)\n\
     int uid;\n{\n\
  int i = %s;\n\
  %s = 2 * uid;\n\
  %s = xrealloc (%s,\n\
                 %s * sizeof(int));\n\
  for (; i < %s; i++)\n\
    %s[i] = -1;\n}\n\n",
 	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME,
 	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME,
 	   DFA_INSN_CODES_VARIABLE_NAME, DFA_INSN_CODES_VARIABLE_NAME,
 	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME,
 	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME,
 	   DFA_INSN_CODES_VARIABLE_NAME);
  fprintf (output_file, "\
static inline int %s PARAMS ((rtx));\n\
static inline int\n%s (%s)\n\
    rtx %s;\n{\n\
 int uid = INSN_UID (%s);\n\
 int %s;\n\n",
	   DFA_INSN_CODE_FUNC_NAME, DFA_INSN_CODE_FUNC_NAME,
	   INSN_PARAMETER_NAME, INSN_PARAMETER_NAME,
	   INSN_PARAMETER_NAME,
	   INTERNAL_INSN_CODE_NAME);

  fprintf (output_file,
	   "  if (uid >= %s)\n    dfa_insn_code_enlarge (uid);\n\n",
	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME);
  fprintf (output_file, "  %s = %s[uid];\n",
	   INTERNAL_INSN_CODE_NAME, DFA_INSN_CODES_VARIABLE_NAME);
  fprintf (output_file, "\
  if (%s < 0)\n\
    {\n\
      %s = %s (%s);\n\
      %s[uid] = %s;\n\
    }\n",
	   INTERNAL_INSN_CODE_NAME,
	   INTERNAL_INSN_CODE_NAME,
	   INTERNAL_DFA_INSN_CODE_FUNC_NAME, INSN_PARAMETER_NAME,
	   DFA_INSN_CODES_VARIABLE_NAME, INTERNAL_INSN_CODE_NAME);
  fprintf (output_file, "  return %s;\n}\n\n", INTERNAL_INSN_CODE_NAME);
}

/* The function outputs PHR interface function `state_transition'.  */
static void
output_trans_func ()
{
  fprintf (output_file, "int\n%s (%s, %s)\n\t%s %s;\n\trtx %s;\n",
	   TRANSITION_FUNC_NAME, STATE_NAME, INSN_PARAMETER_NAME,
	   STATE_TYPE_NAME, STATE_NAME, INSN_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s;\n", INTERNAL_INSN_CODE_NAME);
  output_internal_insn_code_evaluation (INSN_PARAMETER_NAME,
					INTERNAL_INSN_CODE_NAME, -1);
  fprintf (output_file, "  return %s (%s, %s);\n}\n\n",
	   INTERNAL_TRANSITION_FUNC_NAME, INTERNAL_INSN_CODE_NAME, STATE_NAME);
}

/* The function outputs a code for evaluation of alternative states
   number for insns which have reservations in given AUTOMATA_LIST.  */
static void
output_automata_list_state_alts_code (automata_list)
     automata_list_el_t automata_list;
{
  automata_list_el_t el;
  automaton_t automaton;

  fprintf (output_file, "      {\n");
  for (el = automata_list; el != NULL; el = el->next_automata_list_el)
    if (comb_vect_p (el->automaton->state_alts_table))
      {
	fprintf (output_file, "        int %s;\n", TEMPORARY_VARIABLE_NAME);
	break;
      }
  for (el = automata_list; el != NULL; el = el->next_automata_list_el)
    {
      automaton = el->automaton;
      if (comb_vect_p (automaton->state_alts_table))
	{
	  fprintf (output_file, "\n        %s = ", TEMPORARY_VARIABLE_NAME);
	  output_state_alts_base_vect_name (output_file, automaton);
	  fprintf (output_file, " [%s->", CHIP_PARAMETER_NAME);
	  output_chip_member_name (output_file, automaton);
	  fprintf (output_file, "] + ");
	  output_translate_vect_name (output_file, automaton);
	  fprintf (output_file, " [%s];\n", INTERNAL_INSN_CODE_NAME);
	  fprintf (output_file, "        if (");
	  output_state_alts_check_vect_name (output_file, automaton);
	  fprintf (output_file, " [%s] != %s->",
		   TEMPORARY_VARIABLE_NAME, CHIP_PARAMETER_NAME);
	  output_chip_member_name (output_file, automaton);
	  fprintf (output_file, ")\n");
	  fprintf (output_file, "          return 0;\n");
	  fprintf (output_file, "        else\n");
	  fprintf (output_file,
		   (el == automata_list
		    ? "          %s = " : "          %s += "),
		   RESULT_VARIABLE_NAME);
	  output_state_alts_comb_vect_name (output_file, automaton);
	  fprintf (output_file, " [%s];\n", TEMPORARY_VARIABLE_NAME);
	}
      else
	{
	  fprintf (output_file,
		   (el == automata_list
		    ? "\n        %s = " : "        %s += "),
		   RESULT_VARIABLE_NAME);
	  output_state_alts_full_vect_name (output_file, automaton);
	  fprintf (output_file, " [");
	  output_translate_vect_name (output_file, automaton);
	  fprintf (output_file, " [%s] + ", INTERNAL_INSN_CODE_NAME);
	  fprintf (output_file, "%s->", CHIP_PARAMETER_NAME);
	  output_chip_member_name (output_file, automaton);
	  fprintf (output_file, " * %d];\n",
		   automaton->insn_equiv_classes_num);
	}
    }
  fprintf (output_file, "        break;\n      }\n\n");
}

/* Output function `internal_state_alts'.  */
static void
output_internal_state_alts_func ()
{
  fprintf (output_file, "static int %s PARAMS ((int, struct %s *));\n",
	   INTERNAL_STATE_ALTS_FUNC_NAME, CHIP_NAME);
  fprintf (output_file,
	   "static int\n%s (%s, %s)\n\tint %s;\n\tstruct %s *%s;\n",
	   INTERNAL_STATE_ALTS_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
	   CHIP_PARAMETER_NAME, INTERNAL_INSN_CODE_NAME, CHIP_NAME,
	   CHIP_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s;\n", RESULT_VARIABLE_NAME);
  fprintf (output_file, "\n  switch (%s)\n    {\n", INTERNAL_INSN_CODE_NAME);
  output_insn_code_cases (output_automata_list_state_alts_code);
  fprintf (output_file,
	   "\n    default:\n      %s = 0;\n      break;\n    }\n",
	   RESULT_VARIABLE_NAME);
  fprintf (output_file, "  return %s;\n", RESULT_VARIABLE_NAME);
  fprintf (output_file, "}\n\n");
}

/* The function outputs PHR interface function `state_alts'.  */
static void
output_state_alts_func ()
{
  fprintf (output_file, "int\n%s (%s, %s)\n\t%s %s;\n\trtx %s;\n",
	   STATE_ALTS_FUNC_NAME, STATE_NAME, INSN_PARAMETER_NAME,
	   STATE_TYPE_NAME, STATE_NAME, INSN_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s;\n", INTERNAL_INSN_CODE_NAME);
  output_internal_insn_code_evaluation (INSN_PARAMETER_NAME,
					INTERNAL_INSN_CODE_NAME, 0);
  fprintf (output_file, "  return %s (%s, %s);\n}\n\n",
	   INTERNAL_STATE_ALTS_FUNC_NAME, INTERNAL_INSN_CODE_NAME, STATE_NAME);
}

/* Output function `min_issue_delay'.  */
static void
output_min_issue_delay_func ()
{
  fprintf (output_file, "int\n%s (%s, %s)\n\t%s %s;\n\trtx %s;\n",
	   MIN_ISSUE_DELAY_FUNC_NAME, STATE_NAME, INSN_PARAMETER_NAME,
	   STATE_TYPE_NAME, STATE_NAME, INSN_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s;\n", INTERNAL_INSN_CODE_NAME);
  fprintf (output_file, "\n  if (%s != 0)\n    {\n", INSN_PARAMETER_NAME);
  fprintf (output_file, "      %s = %s (%s);\n", INTERNAL_INSN_CODE_NAME,
	   DFA_INSN_CODE_FUNC_NAME, INSN_PARAMETER_NAME);
  fprintf (output_file, "      if (%s > %s)\n        return 0;\n",
	   INTERNAL_INSN_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME);
  fprintf (output_file, "    }\n  else\n    %s = %s;\n",
	   INTERNAL_INSN_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME);
  fprintf (output_file, "\n  return %s (%s, %s);\n",
 	   INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
	   STATE_NAME);
  fprintf (output_file, "}\n\n");
}

/* Output function `internal_dead_lock'.  */
static void
output_internal_dead_lock_func ()
{
  automaton_t automaton;

  fprintf (output_file, "static int %s PARAMS ((struct %s *));\n",
	   INTERNAL_DEAD_LOCK_FUNC_NAME, CHIP_NAME);
  fprintf (output_file, "static int\n%s (%s)\n\tstruct %s *%s;\n",
	   INTERNAL_DEAD_LOCK_FUNC_NAME, CHIP_PARAMETER_NAME, CHIP_NAME,
	   CHIP_PARAMETER_NAME);
  fprintf (output_file, "{\n");
  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      fprintf (output_file, "  if (");
      output_dead_lock_vect_name (output_file, automaton);
      fprintf (output_file, " [%s->", CHIP_PARAMETER_NAME);
      output_chip_member_name (output_file, automaton);
      fprintf (output_file, "])\n    return 1/* TRUE */;\n");
    }
  fprintf (output_file, "  return 0/* FALSE */;\n}\n\n");
}

/* The function outputs PHR interface function `state_dead_lock_p'.  */
static void
output_dead_lock_func ()
{
  fprintf (output_file, "int\n%s (%s)\n\t%s %s;\n",
	   DEAD_LOCK_FUNC_NAME, STATE_NAME, STATE_TYPE_NAME, STATE_NAME);
  fprintf (output_file, "{\n  return %s (%s);\n}\n\n",
	   INTERNAL_DEAD_LOCK_FUNC_NAME, STATE_NAME);
}

/* Output function `internal_reset'.  */
static void
output_internal_reset_func ()
{
  fprintf (output_file, "static void %s PARAMS ((struct %s *));\n",
	   INTERNAL_RESET_FUNC_NAME, CHIP_NAME);
  fprintf (output_file, "static void\n%s (%s)\n\tstruct %s *%s;\n",
	   INTERNAL_RESET_FUNC_NAME, CHIP_PARAMETER_NAME,
	   CHIP_NAME, CHIP_PARAMETER_NAME);
  fprintf (output_file, "{\n  memset (%s, 0, sizeof (struct %s));\n}\n\n",
	   CHIP_PARAMETER_NAME, CHIP_NAME);
}

/* The function outputs PHR interface function `state_size'.  */
static void
output_size_func ()
{
  fprintf (output_file, "int\n%s ()\n", SIZE_FUNC_NAME);
  fprintf (output_file, "{\n  return sizeof (struct %s);\n}\n\n", CHIP_NAME);
}

/* The function outputs PHR interface function `state_reset'.  */
static void
output_reset_func ()
{
  fprintf (output_file, "void\n%s (%s)\n\t %s %s;\n",
	   RESET_FUNC_NAME, STATE_NAME, STATE_TYPE_NAME, STATE_NAME);
  fprintf (output_file, "{\n  %s (%s);\n}\n\n", INTERNAL_RESET_FUNC_NAME,
	   STATE_NAME);
}

/* Output function `min_insn_conflict_delay'.  */
static void
output_min_insn_conflict_delay_func ()
{
  fprintf (output_file,
	   "int\n%s (%s, %s, %s)\n\t%s %s;\n\trtx %s;\n\trtx %s;\n",
	   MIN_INSN_CONFLICT_DELAY_FUNC_NAME,
	   STATE_NAME, INSN_PARAMETER_NAME, INSN2_PARAMETER_NAME,
 	   STATE_TYPE_NAME, STATE_NAME,
	   INSN_PARAMETER_NAME, INSN2_PARAMETER_NAME);
  fprintf (output_file, "{\n  struct %s %s;\n  int %s, %s;\n",
	   CHIP_NAME, CHIP_NAME, INTERNAL_INSN_CODE_NAME,
	   INTERNAL_INSN2_CODE_NAME);
  output_internal_insn_code_evaluation (INSN_PARAMETER_NAME,
					INTERNAL_INSN_CODE_NAME, 0);
  output_internal_insn_code_evaluation (INSN2_PARAMETER_NAME,
					INTERNAL_INSN2_CODE_NAME, 0);
  fprintf (output_file, "  memcpy (&%s, %s, sizeof (%s));\n",
	   CHIP_NAME, STATE_NAME, CHIP_NAME);
  fprintf (output_file, "  %s (&%s);\n", INTERNAL_RESET_FUNC_NAME, CHIP_NAME);
  fprintf (output_file, "  if (%s (%s, &%s) > 0)\n    abort ();\n",
	   INTERNAL_TRANSITION_FUNC_NAME, INTERNAL_INSN_CODE_NAME, CHIP_NAME);
  fprintf (output_file, "  return %s (%s, &%s);\n",
	   INTERNAL_MIN_ISSUE_DELAY_FUNC_NAME, INTERNAL_INSN2_CODE_NAME,
	   CHIP_NAME);
  fprintf (output_file, "}\n\n");
}

/* Output function `internal_insn_latency'.  */
static void
output_internal_insn_latency_func ()
{
  decl_t decl;
  struct bypass_decl *bypass;
  int i, j, col;
  const char *tabletype = "unsigned char";

  /* Find the smallest integer type that can hold all the default
     latency values.  */
  for (i = 0; i < description->decls_num; i++)
    if (description->decls[i]->mode == dm_insn_reserv)
      {
	decl = description->decls[i];
	if (DECL_INSN_RESERV (decl)->default_latency > UCHAR_MAX
	    && tabletype[0] != 'i')  /* don't shrink it */
	  tabletype = "unsigned short";
	if (DECL_INSN_RESERV (decl)->default_latency > USHRT_MAX)
	  tabletype = "int";
      }
    
  fprintf (output_file, "static int %s PARAMS ((int, int, rtx, rtx));\n",
	   INTERNAL_INSN_LATENCY_FUNC_NAME);
  fprintf (output_file, "static int\n%s (%s, %s, %s, %s)",
	   INTERNAL_INSN_LATENCY_FUNC_NAME, INTERNAL_INSN_CODE_NAME,
	   INTERNAL_INSN2_CODE_NAME, INSN_PARAMETER_NAME,
	   INSN2_PARAMETER_NAME);
  fprintf (output_file,
	   "\n\tint %s ATTRIBUTE_UNUSED;\n\tint %s ATTRIBUTE_UNUSED;\n",
	   INTERNAL_INSN_CODE_NAME, INTERNAL_INSN2_CODE_NAME);
  fprintf (output_file,
	   "\trtx %s ATTRIBUTE_UNUSED;\n\trtx %s ATTRIBUTE_UNUSED;\n{\n",
	   INSN_PARAMETER_NAME, INSN2_PARAMETER_NAME);

  if (DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num == 0)
    {
      fputs ("  return 0;\n}\n\n", output_file);
      return;
    }

  fprintf (output_file, "  static const %s default_latencies[] =\n    {",
	   tabletype);

  for (i = 0, j = 0, col = 7; i < description->decls_num; i++)
    if (description->decls[i]->mode == dm_insn_reserv
	&& description->decls[i] != advance_cycle_insn_decl)
      {
	if ((col = (col+1) % 8) == 0)
	  fputs ("\n     ", output_file);
	decl = description->decls[i];
	if (j++ != DECL_INSN_RESERV (decl)->insn_num)
	  abort ();
	fprintf (output_file, "% 4d,",
		 DECL_INSN_RESERV (decl)->default_latency);
      }
  if (j != DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num)
    abort ();
  fputs ("\n    };\n", output_file);

  fprintf (output_file, "  if (%s >= %s || %s >= %s)\n    return 0;\n",
	   INTERNAL_INSN_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME,
	   INTERNAL_INSN2_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME);

  fprintf (output_file, "  switch (%s)\n    {\n", INTERNAL_INSN_CODE_NAME);
  for (i = 0; i < description->decls_num; i++)
    if (description->decls[i]->mode == dm_insn_reserv
	&& DECL_INSN_RESERV (description->decls[i])->bypass_list)
      {
	decl = description->decls [i];
	fprintf (output_file,
		 "    case %d:\n      switch (%s)\n        {\n",
		 DECL_INSN_RESERV (decl)->insn_num,
		 INTERNAL_INSN2_CODE_NAME);
	for (bypass = DECL_INSN_RESERV (decl)->bypass_list;
	     bypass != NULL;
	     bypass = bypass->next)
	  {
	    if (bypass->in_insn_reserv->insn_num
		== DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num)
	      abort ();
	    fprintf (output_file, "        case %d:\n",
		     bypass->in_insn_reserv->insn_num);
	    if (bypass->bypass_guard_name == NULL)
	      fprintf (output_file, "          return %d;\n",
		       bypass->latency);
	    else
	      {
		fprintf (output_file,
			 "          if (%s (%s, %s))\n",
			 bypass->bypass_guard_name, INSN_PARAMETER_NAME,
			 INSN2_PARAMETER_NAME);
		fprintf (output_file,
			 "            return %d;\n          break;\n",
			 bypass->latency);
	      }
	  }
	fputs ("        }\n      break;\n", output_file);
      }

  fprintf (output_file, "    }\n  return default_latencies[%s];\n}\n\n",
	   INTERNAL_INSN_CODE_NAME);
}

/* The function outputs PHR interface function `insn_latency'.  */
static void
output_insn_latency_func ()
{
  fprintf (output_file, "int\n%s (%s, %s)\n\trtx %s;\n\trtx %s;\n",
	   INSN_LATENCY_FUNC_NAME, INSN_PARAMETER_NAME, INSN2_PARAMETER_NAME,
	   INSN_PARAMETER_NAME, INSN2_PARAMETER_NAME);
  fprintf (output_file, "{\n  int %s, %s;\n",
	   INTERNAL_INSN_CODE_NAME, INTERNAL_INSN2_CODE_NAME);
  output_internal_insn_code_evaluation (INSN_PARAMETER_NAME,
					INTERNAL_INSN_CODE_NAME, 0);
  output_internal_insn_code_evaluation (INSN2_PARAMETER_NAME,
					INTERNAL_INSN2_CODE_NAME, 0);
  fprintf (output_file, "  return %s (%s, %s, %s, %s);\n}\n\n",
	   INTERNAL_INSN_LATENCY_FUNC_NAME,
	   INTERNAL_INSN_CODE_NAME, INTERNAL_INSN2_CODE_NAME,
	   INSN_PARAMETER_NAME, INSN2_PARAMETER_NAME);
}

/* The function outputs PHR interface function `print_reservation'.  */
static void
output_print_reservation_func ()
{
  decl_t decl;
  int i, j;

  fprintf (output_file,
	   "void\n%s (%s, %s)\n\tFILE *%s;\n\trtx %s ATTRIBUTE_UNUSED;\n{\n",
           PRINT_RESERVATION_FUNC_NAME, FILE_PARAMETER_NAME,
           INSN_PARAMETER_NAME, FILE_PARAMETER_NAME,
           INSN_PARAMETER_NAME);

  if (DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num == 0)
    {
      fprintf (output_file, "  fputs (\"%s\", %s);\n}\n\n",
	       NOTHING_NAME, FILE_PARAMETER_NAME);
      return;
    }


  fputs ("  static const char *const reservation_names[] =\n    {",
	 output_file);

  for (i = 0, j = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
	{
	  if (j++ != DECL_INSN_RESERV (decl)->insn_num)
	    abort ();
	  fprintf (output_file, "\n      \"%s\",",
		   regexp_representation (DECL_INSN_RESERV (decl)->regexp));
	  finish_regexp_representation ();
	}
    }
  if (j != DECL_INSN_RESERV (advance_cycle_insn_decl)->insn_num)
    abort ();
	      
  fprintf (output_file, "\n      \"%s\"\n    };\n  int %s;\n\n",
	   NOTHING_NAME, INTERNAL_INSN_CODE_NAME);

  fprintf (output_file, "  if (%s == 0)\n    %s = %s;\n",
	   INSN_PARAMETER_NAME,
	   INTERNAL_INSN_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME);
  fprintf (output_file, "  else\n\
    {\n\
      %s = %s (%s);\n\
      if (%s > %s)\n\
        %s = %s;\n\
    }\n",
	   INTERNAL_INSN_CODE_NAME, DFA_INSN_CODE_FUNC_NAME,
	       INSN_PARAMETER_NAME,
	   INTERNAL_INSN_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME,
	   INTERNAL_INSN_CODE_NAME, ADVANCE_CYCLE_VALUE_NAME);

  fprintf (output_file, "  fputs (reservation_names[%s], %s);\n}\n\n",
	   INTERNAL_INSN_CODE_NAME, FILE_PARAMETER_NAME);
}

/* The following function is used to sort unit declaration by their
   names.  */
static int
units_cmp (unit1, unit2)
     const void *unit1, *unit2;
{
  const unit_decl_t u1 = *(unit_decl_t *) unit1;
  const unit_decl_t u2 = *(unit_decl_t *) unit2;

  return strcmp (u1->name, u2->name);
}

/* The following macro value is name of struct containing unit name
   and unit code.  */
#define NAME_CODE_STRUCT_NAME  "name_code"

/* The following macro value is name of table of struct name_code.  */
#define NAME_CODE_TABLE_NAME   "name_code_table"

/* The following macro values are member names for struct name_code.  */
#define NAME_MEMBER_NAME       "name"
#define CODE_MEMBER_NAME       "code"

/* The following macro values are local variable names for function
   `get_cpu_unit_code'.  */
#define CMP_VARIABLE_NAME      "cmp"
#define LOW_VARIABLE_NAME      "l"
#define MIDDLE_VARIABLE_NAME   "m"
#define HIGH_VARIABLE_NAME     "h"

/* The following function outputs function to obtain internal cpu unit
   code by the cpu unit name.  */
static void
output_get_cpu_unit_code_func ()
{
  int i;
  unit_decl_t *units;
  
  fprintf (output_file, "int\n%s (%s)\n\tconst char *%s;\n",
	   GET_CPU_UNIT_CODE_FUNC_NAME, CPU_UNIT_NAME_PARAMETER_NAME,
	   CPU_UNIT_NAME_PARAMETER_NAME);
  fprintf (output_file, "{\n  struct %s {const char *%s; int %s;};\n",
	   NAME_CODE_STRUCT_NAME, NAME_MEMBER_NAME, CODE_MEMBER_NAME);
  fprintf (output_file, "  int %s, %s, %s, %s;\n", CMP_VARIABLE_NAME,
	   LOW_VARIABLE_NAME, MIDDLE_VARIABLE_NAME, HIGH_VARIABLE_NAME);
  fprintf (output_file, "  static struct %s %s [] =\n    {\n",
	   NAME_CODE_STRUCT_NAME, NAME_CODE_TABLE_NAME);
  units = (unit_decl_t *) xmalloc (sizeof (unit_decl_t)
				   * description->units_num);
  memcpy (units, units_array, sizeof (unit_decl_t) * description->units_num);
  qsort (units, description->units_num, sizeof (unit_decl_t), units_cmp);
  for (i = 0; i < description->units_num; i++)
    if (units [i]->query_p)
      fprintf (output_file, "      {\"%s\", %d},\n",
	       units[i]->name, units[i]->query_num);
  fprintf (output_file, "    };\n\n");
  fprintf (output_file, "  /* The following is binary search: */\n");
  fprintf (output_file, "  %s = 0;\n", LOW_VARIABLE_NAME);
  fprintf (output_file, "  %s = sizeof (%s) / sizeof (struct %s) - 1;\n",
	   HIGH_VARIABLE_NAME, NAME_CODE_TABLE_NAME, NAME_CODE_STRUCT_NAME);
  fprintf (output_file, "  while (%s <= %s)\n    {\n",
	   LOW_VARIABLE_NAME, HIGH_VARIABLE_NAME);
  fprintf (output_file, "      %s = (%s + %s) / 2;\n",
	   MIDDLE_VARIABLE_NAME, LOW_VARIABLE_NAME, HIGH_VARIABLE_NAME);
  fprintf (output_file, "      %s = strcmp (%s, %s [%s].%s);\n",
	   CMP_VARIABLE_NAME, CPU_UNIT_NAME_PARAMETER_NAME,
	   NAME_CODE_TABLE_NAME, MIDDLE_VARIABLE_NAME, NAME_MEMBER_NAME);
  fprintf (output_file, "      if (%s < 0)\n", CMP_VARIABLE_NAME);
  fprintf (output_file, "        %s = %s - 1;\n",
	   HIGH_VARIABLE_NAME, MIDDLE_VARIABLE_NAME);
  fprintf (output_file, "      else if (%s > 0)\n", CMP_VARIABLE_NAME);
  fprintf (output_file, "        %s = %s + 1;\n",
	   LOW_VARIABLE_NAME, MIDDLE_VARIABLE_NAME);
  fprintf (output_file, "      else\n");
  fprintf (output_file, "        return %s [%s].%s;\n    }\n",
	   NAME_CODE_TABLE_NAME, MIDDLE_VARIABLE_NAME, CODE_MEMBER_NAME);
  fprintf (output_file, "  return -1;\n}\n\n");
  free (units);
}

/* The following function outputs function to check reservation of cpu
   unit (its internal code will be passed as the function argument) in
   given cpu state.  */
static void
output_cpu_unit_reservation_p ()
{
  automaton_t automaton;

  fprintf (output_file, "int\n%s (%s, %s)\n\t%s %s;\n\tint %s;\n",
	   CPU_UNIT_RESERVATION_P_FUNC_NAME, STATE_NAME,
	   CPU_CODE_PARAMETER_NAME, STATE_TYPE_NAME, STATE_NAME,
	   CPU_CODE_PARAMETER_NAME);
  fprintf (output_file, "{\n  if (%s < 0 || %s >= %d)\n    abort ();\n",
	   CPU_CODE_PARAMETER_NAME, CPU_CODE_PARAMETER_NAME,
	   description->query_units_num);
  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      fprintf (output_file, "  if ((");
      output_reserved_units_table_name (output_file, automaton);
      fprintf (output_file, " [((struct %s *) %s)->", CHIP_NAME, STATE_NAME);
      output_chip_member_name (output_file, automaton);
      fprintf (output_file, " * %d + %s / 8] >> (%s %% 8)) & 1)\n",
	       (description->query_units_num + 7) / 8,
	       CPU_CODE_PARAMETER_NAME, CPU_CODE_PARAMETER_NAME);
      fprintf (output_file, "    return 1;\n");
    }
  fprintf (output_file, "  return 0;\n}\n\n");
}

/* The function outputs PHR interface function `dfa_clean_insn_cache'.  */
static void
output_dfa_clean_insn_cache_func ()
{
  fprintf (output_file,
	   "void\n%s ()\n{\n  int %s;\n\n",
	   DFA_CLEAN_INSN_CACHE_FUNC_NAME, I_VARIABLE_NAME);
  fprintf (output_file,
	   "  for (%s = 0; %s < %s; %s++)\n    %s [%s] = -1;\n}\n\n",
	   I_VARIABLE_NAME, I_VARIABLE_NAME,
	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME, I_VARIABLE_NAME,
	   DFA_INSN_CODES_VARIABLE_NAME, I_VARIABLE_NAME);
}

/* The function outputs PHR interface function `dfa_start'.  */
static void
output_dfa_start_func ()
{
  fprintf (output_file,
	   "void\n%s ()\n{\n  %s = get_max_uid ();\n",
	   DFA_START_FUNC_NAME, DFA_INSN_CODES_LENGTH_VARIABLE_NAME);
  fprintf (output_file, "  %s = (int *) xmalloc (%s * sizeof (int));\n",
	   DFA_INSN_CODES_VARIABLE_NAME, DFA_INSN_CODES_LENGTH_VARIABLE_NAME);
  fprintf (output_file, "  %s ();\n}\n\n", DFA_CLEAN_INSN_CACHE_FUNC_NAME);
}

/* The function outputs PHR interface function `dfa_finish'.  */
static void
output_dfa_finish_func ()
{
  fprintf (output_file, "void\n%s ()\n{\n  free (%s);\n}\n\n",
	   DFA_FINISH_FUNC_NAME, DFA_INSN_CODES_VARIABLE_NAME);
}



/* The page contains code for output description file (readable
   representation of original description and generated DFA(s).  */

/* The function outputs string representation of IR reservation.  */
static void
output_regexp (regexp)
     regexp_t regexp;
{
  fprintf (output_description_file, "%s", regexp_representation (regexp));
  finish_regexp_representation ();
}

/* Output names of units in LIST separated by comma.  */
static void
output_unit_set_el_list (list)
     unit_set_el_t list;
{
  unit_set_el_t el;

  for (el = list; el != NULL; el = el->next_unit_set_el)
    {
      if (el != list)
	fprintf (output_description_file, ", ");
      fprintf (output_description_file, "%s", el->unit_decl->name);
    }
}

/* Output patterns in LIST separated by comma.  */
static void
output_pattern_set_el_list (list)
     pattern_set_el_t list;
{
  pattern_set_el_t el;
  int i;

  for (el = list; el != NULL; el = el->next_pattern_set_el)
    {
      if (el != list)
	fprintf (output_description_file, ", ");
      for (i = 0; i < el->units_num; i++)
	fprintf (output_description_file, (i == 0 ? "%s" : " %s"),
		 el->unit_decls [i]->name);
    }
}

/* The function outputs string representation of IR define_reservation
   and define_insn_reservation.  */
static void
output_description ()
{
  decl_t decl;
  int i;

  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit)
	{
	  if (DECL_UNIT (decl)->excl_list != NULL)
	    {
	      fprintf (output_description_file, "unit %s exlusion_set: ",
		       DECL_UNIT (decl)->name);
	      output_unit_set_el_list (DECL_UNIT (decl)->excl_list);
	      fprintf (output_description_file, "\n");
	    }
	  if (DECL_UNIT (decl)->presence_list != NULL)
	    {
	      fprintf (output_description_file, "unit %s presence_set: ",
		       DECL_UNIT (decl)->name);
	      output_pattern_set_el_list (DECL_UNIT (decl)->presence_list);
	      fprintf (output_description_file, "\n");
	    }
	  if (DECL_UNIT (decl)->final_presence_list != NULL)
	    {
	      fprintf (output_description_file, "unit %s final_presence_set: ",
		       DECL_UNIT (decl)->name);
	      output_pattern_set_el_list
		(DECL_UNIT (decl)->final_presence_list);
 	      fprintf (output_description_file, "\n");
 	    }
	  if (DECL_UNIT (decl)->absence_list != NULL)
	    {
	      fprintf (output_description_file, "unit %s absence_set: ",
		       DECL_UNIT (decl)->name);
	      output_pattern_set_el_list (DECL_UNIT (decl)->absence_list);
	      fprintf (output_description_file, "\n");
	    }
	  if (DECL_UNIT (decl)->final_absence_list != NULL)
	    {
	      fprintf (output_description_file, "unit %s final_absence_set: ",
		       DECL_UNIT (decl)->name);
	      output_pattern_set_el_list
		(DECL_UNIT (decl)->final_absence_list);
 	      fprintf (output_description_file, "\n");
 	    }
	}
    }
  fprintf (output_description_file, "\n");
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_reserv)
	{
          fprintf (output_description_file, "reservation ");
          fprintf (output_description_file, DECL_RESERV (decl)->name);
          fprintf (output_description_file, ": ");
          output_regexp (DECL_RESERV (decl)->regexp);
          fprintf (output_description_file, "\n");
        }
      else if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
        {
          fprintf (output_description_file, "insn reservation %s ",
		   DECL_INSN_RESERV (decl)->name);
          print_rtl (output_description_file,
		     DECL_INSN_RESERV (decl)->condexp);
          fprintf (output_description_file, ": ");
          output_regexp (DECL_INSN_RESERV (decl)->regexp);
          fprintf (output_description_file, "\n");
        }
      else if (decl->mode == dm_bypass)
	fprintf (output_description_file, "bypass %d %s %s\n",
		 DECL_BYPASS (decl)->latency,
		 DECL_BYPASS (decl)->out_insn_name,
		 DECL_BYPASS (decl)->in_insn_name);
    }
  fprintf (output_description_file, "\n\f\n");
}

/* The function outputs name of AUTOMATON.  */
static void
output_automaton_name (f, automaton)
     FILE *f;
     automaton_t automaton;
{
  if (automaton->corresponding_automaton_decl == NULL)
    fprintf (f, "#%d", automaton->automaton_order_num);
  else
    fprintf (f, "`%s'", automaton->corresponding_automaton_decl->name);
}

/* Maximal length of line for pretty printing into description
   file.  */
#define MAX_LINE_LENGTH 70

/* The function outputs units name belonging to AUTOMATON.  */
static void
output_automaton_units (automaton)
     automaton_t automaton;
{
  decl_t decl;
  char *name;
  int curr_line_length;
  int there_is_an_automaton_unit;
  int i;

  fprintf (output_description_file, "\n  Coresponding units:\n");
  fprintf (output_description_file, "    ");
  curr_line_length = 4;
  there_is_an_automaton_unit = 0;
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_unit
          && (DECL_UNIT (decl)->corresponding_automaton_num
	      == automaton->automaton_order_num))
	{
	  there_is_an_automaton_unit = 1;
	  name = DECL_UNIT (decl)->name;
	  if (curr_line_length + strlen (name) + 1 > MAX_LINE_LENGTH )
	    {
	      curr_line_length = strlen (name) + 4;
	      fprintf (output_description_file, "\n    ");
	    }
	  else
	    {
	      curr_line_length += strlen (name) + 1;
	      fprintf (output_description_file, " ");
	    }
	  fprintf (output_description_file, name);
	}
    }
  if (!there_is_an_automaton_unit)
    fprintf (output_description_file, "<None>");
  fprintf (output_description_file, "\n\n");
}

/* The following variable is used for forming array of all possible cpu unit
   reservations described by the current DFA state.  */
static vla_ptr_t state_reservs;

/* The function forms `state_reservs' for STATE.  */
static void
add_state_reservs (state)
     state_t state;
{
  alt_state_t curr_alt_state;
  reserv_sets_t reservs;

  if (state->component_states != NULL)
    for (curr_alt_state = state->component_states;
         curr_alt_state != NULL;
         curr_alt_state = curr_alt_state->next_sorted_alt_state)
      add_state_reservs (curr_alt_state->state);
  else
    {
      reservs = state->reservs;
      VLA_PTR_ADD (state_reservs, reservs);
    }
}

/* The function outputs readable representation of all out arcs of
   STATE.  */
static void
output_state_arcs (state)
     state_t state;
{
  arc_t arc;
  ainsn_t ainsn;
  char *insn_name;
  int curr_line_length;

  for (arc = first_out_arc (state); arc != NULL; arc = next_out_arc (arc))
    {
      ainsn = arc->insn;
      if (!ainsn->first_insn_with_same_reservs)
	abort ();
      fprintf (output_description_file, "    ");
      curr_line_length = 7;
      fprintf (output_description_file, "%2d: ", ainsn->insn_equiv_class_num);
      do
        {
          insn_name = ainsn->insn_reserv_decl->name;
          if (curr_line_length + strlen (insn_name) > MAX_LINE_LENGTH)
            {
              if (ainsn != arc->insn)
                {
                  fprintf (output_description_file, ",\n      ");
                  curr_line_length = strlen (insn_name) + 6;
                }
              else
                curr_line_length += strlen (insn_name);
            }
          else
            {
              curr_line_length += strlen (insn_name);
              if (ainsn != arc->insn)
                {
                  curr_line_length += 2;
                  fprintf (output_description_file, ", ");
                }
            }
          fprintf (output_description_file, insn_name);
          ainsn = ainsn->next_same_reservs_insn;
        }
      while (ainsn != NULL);
      fprintf (output_description_file, "    %d (%d)\n",
	       arc->to_state->order_state_num, arc->state_alts);
    }
  fprintf (output_description_file, "\n");
}

/* The following function is used for sorting possible cpu unit
   reservation of a DFA state.  */
static int
state_reservs_cmp (reservs_ptr_1, reservs_ptr_2)
     const void *reservs_ptr_1;
     const void *reservs_ptr_2;
{
  return reserv_sets_cmp (*(reserv_sets_t *) reservs_ptr_1,
                          *(reserv_sets_t *) reservs_ptr_2);
}

/* The following function is used for sorting possible cpu unit
   reservation of a DFA state.  */
static void
remove_state_duplicate_reservs ()
{
  reserv_sets_t *reservs_ptr;
  reserv_sets_t *last_formed_reservs_ptr;

  last_formed_reservs_ptr = NULL;
  for (reservs_ptr = VLA_PTR_BEGIN (state_reservs);
       reservs_ptr <= (reserv_sets_t *) VLA_PTR_LAST (state_reservs);
       reservs_ptr++)
    if (last_formed_reservs_ptr == NULL)
      last_formed_reservs_ptr = reservs_ptr;
    else if (reserv_sets_cmp (*last_formed_reservs_ptr, *reservs_ptr) != 0)
      {
        ++last_formed_reservs_ptr;
        *last_formed_reservs_ptr = *reservs_ptr;
      }
  VLA_PTR_SHORTEN (state_reservs, reservs_ptr - last_formed_reservs_ptr - 1);
}

/* The following function output readable representation of DFA(s)
   state used for fast recognition of pipeline hazards.  State is
   described by possible (current and scheduled) cpu unit
   reservations.  */
static void
output_state (state)
     state_t state;
{
  reserv_sets_t *reservs_ptr;

  VLA_PTR_CREATE (state_reservs, 150, "state reservations");
  fprintf (output_description_file, "  State #%d", state->order_state_num);
  fprintf (output_description_file,
	   state->new_cycle_p ? " (new cycle)\n" : "\n");
  add_state_reservs (state);
  qsort (VLA_PTR_BEGIN (state_reservs), VLA_PTR_LENGTH (state_reservs),
         sizeof (reserv_sets_t), state_reservs_cmp);
  remove_state_duplicate_reservs ();
  for (reservs_ptr = VLA_PTR_BEGIN (state_reservs);
       reservs_ptr <= (reserv_sets_t *) VLA_PTR_LAST (state_reservs);
       reservs_ptr++)
    {
      fprintf (output_description_file, "    ");
      output_reserv_sets (output_description_file, *reservs_ptr);
      fprintf (output_description_file, "\n");
    }
  fprintf (output_description_file, "\n");
  output_state_arcs (state);
  VLA_PTR_DELETE (state_reservs);
}

/* The following function output readable representation of
   DFAs used for fast recognition of pipeline hazards.  */
static void
output_automaton_descriptions ()
{
  automaton_t automaton;

  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      fprintf (output_description_file, "\nAutomaton ");
      output_automaton_name (output_description_file, automaton);
      fprintf (output_description_file, "\n");
      output_automaton_units (automaton);
      pass_states (automaton, output_state);
    }
}



/* The page contains top level function for generation DFA(s) used for
   PHR.  */

/* The function outputs statistics about work of different phases of
   DFA generator.  */
static void
output_statistics (f)
     FILE *f;
{
  automaton_t automaton;
  int states_num;
#ifndef NDEBUG
  int transition_comb_vect_els = 0;
  int transition_full_vect_els = 0;
  int state_alts_comb_vect_els = 0;
  int state_alts_full_vect_els = 0;
  int min_issue_delay_vect_els = 0;
#endif

  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      fprintf (f, "\nAutomaton ");
      output_automaton_name (f, automaton);
      fprintf (f, "\n    %5d NDFA states,          %5d NDFA arcs\n",
	       automaton->NDFA_states_num, automaton->NDFA_arcs_num);
      fprintf (f, "    %5d DFA states,           %5d DFA arcs\n",
	       automaton->DFA_states_num, automaton->DFA_arcs_num);
      states_num = automaton->DFA_states_num;
      if (!no_minimization_flag)
	{
	  fprintf (f, "    %5d minimal DFA states,   %5d minimal DFA arcs\n",
		   automaton->minimal_DFA_states_num,
		   automaton->minimal_DFA_arcs_num);
	  states_num = automaton->minimal_DFA_states_num;
	}
      fprintf (f, "    %5d all insns      %5d insn equivalence classes\n",
	       description->insns_num, automaton->insn_equiv_classes_num);
#ifndef NDEBUG
      fprintf
	(f, "%5ld transition comb vector els, %5ld trans table els: %s\n",
	 (long) VLA_HWINT_LENGTH (automaton->trans_table->comb_vect),
	 (long) VLA_HWINT_LENGTH (automaton->trans_table->full_vect),
	 (comb_vect_p (automaton->trans_table)
	  ? "use comb vect" : "use simple vect"));
      fprintf
        (f, "%5ld state alts comb vector els, %5ld state alts table els: %s\n",
         (long) VLA_HWINT_LENGTH (automaton->state_alts_table->comb_vect),
         (long) VLA_HWINT_LENGTH (automaton->state_alts_table->full_vect),
         (comb_vect_p (automaton->state_alts_table)
          ? "use comb vect" : "use simple vect"));
      fprintf
        (f, "%5ld min delay table els, compression factor %d\n",
         (long) states_num * automaton->insn_equiv_classes_num,
	 automaton->min_issue_delay_table_compression_factor);
      transition_comb_vect_els
	+= VLA_HWINT_LENGTH (automaton->trans_table->comb_vect);
      transition_full_vect_els 
        += VLA_HWINT_LENGTH (automaton->trans_table->full_vect);
      state_alts_comb_vect_els
        += VLA_HWINT_LENGTH (automaton->state_alts_table->comb_vect);
      state_alts_full_vect_els
        += VLA_HWINT_LENGTH (automaton->state_alts_table->full_vect);
      min_issue_delay_vect_els
	+= states_num * automaton->insn_equiv_classes_num;
#endif
    }
#ifndef NDEBUG
  fprintf (f, "\n%5d all allocated states,     %5d all allocated arcs\n",
	   allocated_states_num, allocated_arcs_num);
  fprintf (f, "%5d all allocated alternative states\n",
	   allocated_alt_states_num);
  fprintf (f, "%5d all transition comb vector els, %5d all trans table els\n",
	   transition_comb_vect_els, transition_full_vect_els);
  fprintf
    (f, "%5d all state alts comb vector els, %5d all state alts table els\n",
     state_alts_comb_vect_els, state_alts_full_vect_els);
  fprintf (f, "%5d all min delay table els\n", min_issue_delay_vect_els);
  fprintf (f, "%5d locked states num\n", locked_states_num);
#endif
}

/* The function output times of work of different phases of DFA
   generator.  */
static void
output_time_statistics (f)
     FILE *f;
{
  fprintf (f, "\n  transformation: ");
  print_active_time (f, transform_time);
  fprintf (f, (!ndfa_flag ? ", building DFA: " : ", building NDFA: "));
  print_active_time (f, NDFA_time);
  if (ndfa_flag)
    {
      fprintf (f, ", NDFA -> DFA: ");
      print_active_time (f, NDFA_to_DFA_time);
    }
  fprintf (f, "\n  DFA minimization: ");
  print_active_time (f, minimize_time);
  fprintf (f, ", making insn equivalence: ");
  print_active_time (f, equiv_time);
  fprintf (f, "\n all automaton generation: ");
  print_active_time (f, automaton_generation_time);
  fprintf (f, ", output: ");
  print_active_time (f, output_time);
  fprintf (f, "\n");
}

/* The function generates DFA (deterministic finite state automaton)
   for fast recognition of pipeline hazards.  No errors during
   checking must be fixed before this function call.  */
static void
generate ()
{
  automata_num = split_argument;
  if (description->units_num < automata_num)
    automata_num = description->units_num;
  initiate_states ();
  initiate_arcs ();
  initiate_automata_lists ();
  initiate_pass_states ();
  initiate_excl_sets ();
  initiate_presence_absence_pattern_sets ();
  automaton_generation_time = create_ticker ();
  create_automata ();
  ticker_off (&automaton_generation_time);
}



/* The following function creates insn attribute whose values are
   number alternatives in insn reservations.  */
static void
make_insn_alts_attr ()
{
  int i, insn_num;
  decl_t decl;
  rtx condexp;

  condexp = rtx_alloc (COND);
  XVEC (condexp, 0) = rtvec_alloc ((description->insns_num - 1) * 2);
  XEXP (condexp, 1) = make_numeric_value (0);
  for (i = insn_num = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
	{
          XVECEXP (condexp, 0, 2 * insn_num)
	    = DECL_INSN_RESERV (decl)->condexp;
          XVECEXP (condexp, 0, 2 * insn_num + 1)
            = make_numeric_value
	      (DECL_INSN_RESERV (decl)->transformed_regexp->mode != rm_oneof
	       ? 1 : REGEXP_ONEOF (DECL_INSN_RESERV (decl)
				   ->transformed_regexp)->regexps_num);
          insn_num++;
        }
    }
  if (description->insns_num != insn_num + 1)
    abort ();
  make_internal_attr (attr_printf (sizeof ("*")
				   + strlen (INSN_ALTS_FUNC_NAME) + 1,
				   "*%s", INSN_ALTS_FUNC_NAME),
		      condexp, 0);
}



/* The following function creates attribute which is order number of
   insn in pipeline hazard description translator.  */
static void
make_internal_dfa_insn_code_attr ()
{
  int i, insn_num;
  decl_t decl;
  rtx condexp;

  condexp = rtx_alloc (COND);
  XVEC (condexp, 0) = rtvec_alloc ((description->insns_num - 1) * 2);
  XEXP (condexp, 1)
    = make_numeric_value (DECL_INSN_RESERV (advance_cycle_insn_decl)
			  ->insn_num + 1);
  for (i = insn_num = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
	{
          XVECEXP (condexp, 0, 2 * insn_num)
	    = DECL_INSN_RESERV (decl)->condexp;
          XVECEXP (condexp, 0, 2 * insn_num + 1)
            = make_numeric_value (DECL_INSN_RESERV (decl)->insn_num);
          insn_num++;
        }
    }
  if (description->insns_num != insn_num + 1)
    abort ();
  make_internal_attr
    (attr_printf (sizeof ("*")
		  + strlen (INTERNAL_DFA_INSN_CODE_FUNC_NAME) + 1,
		  "*%s", INTERNAL_DFA_INSN_CODE_FUNC_NAME),
     condexp, 0);
}



/* The following function creates attribute which order number of insn
   in pipeline hazard description translator.  */
static void
make_default_insn_latency_attr ()
{
  int i, insn_num;
  decl_t decl;
  rtx condexp;

  condexp = rtx_alloc (COND);
  XVEC (condexp, 0) = rtvec_alloc ((description->insns_num - 1) * 2);
  XEXP (condexp, 1) = make_numeric_value (0);
  for (i = insn_num = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv && decl != advance_cycle_insn_decl)
	{
          XVECEXP (condexp, 0, 2 * insn_num)
	    = DECL_INSN_RESERV (decl)->condexp;
          XVECEXP (condexp, 0, 2 * insn_num + 1)
            = make_numeric_value (DECL_INSN_RESERV (decl)->default_latency);
          insn_num++;
        }
    }
  if (description->insns_num != insn_num + 1)
    abort ();
  make_internal_attr (attr_printf (sizeof ("*")
				   + strlen (INSN_DEFAULT_LATENCY_FUNC_NAME)
				   + 1, "*%s", INSN_DEFAULT_LATENCY_FUNC_NAME),
		      condexp, 0);
}



/* The following function creates attribute which returns 1 if given
   output insn has bypassing and 0 otherwise.  */
static void
make_bypass_attr ()
{
  int i, bypass_insn;
  int bypass_insns_num = 0;
  decl_t decl;
  rtx result_rtx;
  
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv
	  && DECL_INSN_RESERV (decl)->condexp != NULL
	  && DECL_INSN_RESERV (decl)->bypass_list != NULL)
	bypass_insns_num++;
    }
  if (bypass_insns_num == 0)
    result_rtx = make_numeric_value (0);
  else
    {
      result_rtx = rtx_alloc (COND);
      XVEC (result_rtx, 0) = rtvec_alloc (bypass_insns_num * 2);
      XEXP (result_rtx, 1) = make_numeric_value (0);

      for (i = bypass_insn = 0; i < description->decls_num; i++)
        {
          decl = description->decls [i];
          if (decl->mode == dm_insn_reserv
	      && DECL_INSN_RESERV (decl)->condexp != NULL
	      && DECL_INSN_RESERV (decl)->bypass_list != NULL)
	    {
              XVECEXP (result_rtx, 0, 2 * bypass_insn)
		= DECL_INSN_RESERV (decl)->condexp;
              XVECEXP (result_rtx, 0, 2 * bypass_insn + 1)
	        = make_numeric_value (1);
              bypass_insn++;
            }
        }
    }
  make_internal_attr (attr_printf (sizeof ("*")
				   + strlen (BYPASS_P_FUNC_NAME) + 1,
				   "*%s", BYPASS_P_FUNC_NAME),
		      result_rtx, 0);
}



/* This page mainly contains top level functions of pipeline hazards
   description translator.  */

/* The following macro value is suffix of name of description file of
   pipeline hazards description translator.  */
#define STANDARD_OUTPUT_DESCRIPTION_FILE_SUFFIX ".dfa"

/* The function returns suffix of given file name.  The returned
   string can not be changed.  */
static const char *
file_name_suffix (file_name)
     const char *file_name;
{
  const char *last_period;

  for (last_period = NULL; *file_name != '\0'; file_name++)
    if (*file_name == '.')
      last_period = file_name;
  return (last_period == NULL ? file_name : last_period);
}

/* The function returns base name of given file name, i.e. pointer to
   first char after last `/' (or `\' for WIN32) in given file name,
   given file name itself if the directory name is absent.  The
   returned string can not be changed.  */
static const char *
base_file_name (file_name)
     const char *file_name;
{
  int directory_name_length;

  directory_name_length = strlen (file_name);
#ifdef WIN32
  while (directory_name_length >= 0 && file_name[directory_name_length] != '/'
         && file_name[directory_name_length] != '\\')
#else
  while (directory_name_length >= 0 && file_name[directory_name_length] != '/')
#endif
    directory_name_length--;
  return file_name + directory_name_length + 1;
}

/* The following is top level function to initialize the work of
   pipeline hazards description translator.  */
void
initiate_automaton_gen (argc, argv)
     int argc;
     char **argv;
{
  const char *base_name;
  int i;

  ndfa_flag = 0;
  split_argument = 0;  /* default value */
  no_minimization_flag = 0;
  time_flag = 0;
  v_flag = 0;
  w_flag = 0;
  for (i = 2; i < argc; i++)
    if (strcmp (argv [i], NO_MINIMIZATION_OPTION) == 0)
      no_minimization_flag = 1;
    else if (strcmp (argv [i], TIME_OPTION) == 0)
      time_flag = 1;
    else if (strcmp (argv [i], V_OPTION) == 0)
      v_flag = 1;
    else if (strcmp (argv [i], W_OPTION) == 0)
      w_flag = 1;
    else if (strcmp (argv [i], NDFA_OPTION) == 0)
      ndfa_flag = 1;
    else if (strcmp (argv [i], "-split") == 0)
      {
	if (i + 1 >= argc)
	  fatal ("-split has no argument.");
	fatal ("option `-split' has not been implemented yet\n");
	/* split_argument = atoi (argument_vect [i + 1]); */
      }
  VLA_PTR_CREATE (decls, 150, "decls");
  /* Initialize IR storage.  */
  obstack_init (&irp);
  initiate_automaton_decl_table ();
  initiate_insn_decl_table ();
  initiate_decl_table ();
  output_file = stdout;
  output_description_file = NULL;
  base_name = base_file_name (argv[1]);
  obstack_grow (&irp, base_name,
		strlen (base_name) - strlen (file_name_suffix (base_name)));
  obstack_grow (&irp, STANDARD_OUTPUT_DESCRIPTION_FILE_SUFFIX,
		strlen (STANDARD_OUTPUT_DESCRIPTION_FILE_SUFFIX) + 1);
  obstack_1grow (&irp, '\0');
  output_description_file_name = obstack_base (&irp);
  obstack_finish (&irp);
}

/* The following function checks existence at least one arc marked by
   each insn.  */
static void
check_automata_insn_issues ()
{
  automaton_t automaton;
  ainsn_t ainsn, reserv_ainsn;

  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      for (ainsn = automaton->ainsn_list;
	   ainsn != NULL;
	   ainsn = ainsn->next_ainsn)
	if (ainsn->first_insn_with_same_reservs && !ainsn->arc_exists_p)
	  {
	    for (reserv_ainsn = ainsn;
		 reserv_ainsn != NULL;
		 reserv_ainsn = reserv_ainsn->next_same_reservs_insn)
	      if (automaton->corresponding_automaton_decl != NULL)
		{
		  if (!w_flag)
		    error ("Automaton `%s': Insn `%s' will never be issued",
			   automaton->corresponding_automaton_decl->name,
			   reserv_ainsn->insn_reserv_decl->name);
		  else
		    warning
		      ("Automaton `%s': Insn `%s' will never be issued",
		       automaton->corresponding_automaton_decl->name,
		       reserv_ainsn->insn_reserv_decl->name);
		}
	      else
		{
		  if (!w_flag)
		    error ("Insn `%s' will never be issued",
			   reserv_ainsn->insn_reserv_decl->name);
		  else
		    warning ("Insn `%s' will never be issued",
			     reserv_ainsn->insn_reserv_decl->name);
		}
	  }
    }
}

/* The following vla is used for storing pointers to all achieved
   states.  */
static vla_ptr_t automaton_states;

/* This function is called by function pass_states to add an achieved
   STATE.  */
static void
add_automaton_state (state)
     state_t state;
{
  VLA_PTR_ADD (automaton_states, state);
}

/* The following function forms list of important automata (whose
   states may be changed after the insn issue) for each insn.  */
static void
form_important_insn_automata_lists ()
{
  automaton_t automaton;
  state_t *state_ptr;
  decl_t decl;
  ainsn_t ainsn;
  arc_t arc;
  int i;

  VLA_PTR_CREATE (automaton_states, 1500,
		  "automaton states for forming important insn automata sets");
  /* Mark important ainsns.  */
  for (automaton = description->first_automaton;
       automaton != NULL;
       automaton = automaton->next_automaton)
    {
      VLA_PTR_NULLIFY (automaton_states);
      pass_states (automaton, add_automaton_state);
      for (state_ptr = VLA_PTR_BEGIN (automaton_states);
	   state_ptr <= (state_t *) VLA_PTR_LAST (automaton_states);
	   state_ptr++)
	{
	  for (arc = first_out_arc (*state_ptr);
	       arc != NULL;
	       arc = next_out_arc (arc))
	    if (arc->to_state != *state_ptr)
	      {
		if (!arc->insn->first_insn_with_same_reservs)
		  abort ();
		for (ainsn = arc->insn;
		     ainsn != NULL;
		     ainsn = ainsn->next_same_reservs_insn)
		  ainsn->important_p = TRUE;
	      }
	}
    }
  VLA_PTR_DELETE (automaton_states);
  /* Create automata sets for the insns.  */
  for (i = 0; i < description->decls_num; i++)
    {
      decl = description->decls [i];
      if (decl->mode == dm_insn_reserv)
	{
	  automata_list_start ();
	  for (automaton = description->first_automaton;
	       automaton != NULL;
	       automaton = automaton->next_automaton)
	    for (ainsn = automaton->ainsn_list;
		 ainsn != NULL;
		 ainsn = ainsn->next_ainsn)
	      if (ainsn->important_p
		  && ainsn->insn_reserv_decl == DECL_INSN_RESERV (decl))
		{
		  automata_list_add (automaton);
		  break;
		}
	  DECL_INSN_RESERV (decl)->important_automata_list
	    = automata_list_finish ();
	}
    }
}


/* The following is top level function to generate automat(a,on) for
   fast recognition of pipeline hazards.  */
void
expand_automata ()
{
  int i;

  description = create_node (sizeof (struct description)
			     /* One entry for cycle advancing insn.  */
			     + sizeof (decl_t) * VLA_PTR_LENGTH (decls));
  description->decls_num = VLA_PTR_LENGTH (decls);
  description->query_units_num = 0;
  for (i = 0; i < description->decls_num; i++)
    {
      description->decls [i] = VLA_PTR (decls, i);
      if (description->decls [i]->mode == dm_unit
	  && DECL_UNIT (description->decls [i])->query_p)
        DECL_UNIT (description->decls [i])->query_num
	  = description->query_units_num++;
    }
  all_time = create_ticker ();
  check_time = create_ticker ();
  fprintf (stderr, "Check description...");
  fflush (stderr);
  check_all_description ();
  fprintf (stderr, "done\n");
  ticker_off (&check_time);
  generation_time = create_ticker ();
  if (!have_error)
    {
      transform_insn_regexps ();
      check_unit_distributions_to_automata ();
    }
  if (!have_error)
    {
      generate ();
      check_automata_insn_issues ();
    }
  if (!have_error)
    {
      form_important_insn_automata_lists ();
      fprintf (stderr, "Generation of attributes...");
      fflush (stderr);
      make_internal_dfa_insn_code_attr ();
      make_insn_alts_attr ();
      make_default_insn_latency_attr ();
      make_bypass_attr ();
      fprintf (stderr, "done\n");
    }
  ticker_off (&generation_time);
  ticker_off (&all_time);
  fprintf (stderr, "All other genattrtab stuff...");
  fflush (stderr);
}

/* The following is top level function to output PHR and to finish
   work with pipeline description translator.  */
void
write_automata ()
{
  fprintf (stderr, "done\n");
  if (have_error)
    fatal ("Errors in DFA description");
  ticker_on (&all_time);
  output_time = create_ticker ();
  fprintf (stderr, "Forming and outputing automata tables...");
  fflush (stderr);
  output_dfa_max_issue_rate ();
  output_tables ();
  fprintf (stderr, "done\n");
  fprintf (stderr, "Output functions to work with automata...");
  fflush (stderr);
  output_chip_definitions ();
  output_max_insn_queue_index_def ();
  output_internal_min_issue_delay_func ();
  output_internal_trans_func ();
  /* Cache of insn dfa codes: */
  fprintf (output_file, "\nstatic int *%s;\n", DFA_INSN_CODES_VARIABLE_NAME);
  fprintf (output_file, "\nstatic int %s;\n\n",
	   DFA_INSN_CODES_LENGTH_VARIABLE_NAME);
  output_dfa_insn_code_func ();
  output_trans_func ();
  fprintf (output_file, "\n#if %s\n\n", AUTOMATON_STATE_ALTS_MACRO_NAME);
  output_internal_state_alts_func ();
  output_state_alts_func ();
  fprintf (output_file, "\n#endif /* #if %s */\n\n",
	   AUTOMATON_STATE_ALTS_MACRO_NAME);
  output_min_issue_delay_func ();
  output_internal_dead_lock_func ();
  output_dead_lock_func ();
  output_size_func ();
  output_internal_reset_func ();
  output_reset_func ();
  output_min_insn_conflict_delay_func ();
  output_internal_insn_latency_func ();
  output_insn_latency_func ();
  output_print_reservation_func ();
  /* Output function get_cpu_unit_code.  */
  fprintf (output_file, "\n#if %s\n\n", CPU_UNITS_QUERY_MACRO_NAME);
  output_get_cpu_unit_code_func ();
  output_cpu_unit_reservation_p ();
  fprintf (output_file, "\n#endif /* #if %s */\n\n",
	   CPU_UNITS_QUERY_MACRO_NAME);
  output_dfa_clean_insn_cache_func ();
  output_dfa_start_func ();
  output_dfa_finish_func ();
  fprintf (stderr, "done\n");
  if (v_flag)
    {
      output_description_file = fopen (output_description_file_name, "w");
      if (output_description_file == NULL)
	{
	  perror (output_description_file_name);
	  exit (FATAL_EXIT_CODE);
	}
      fprintf (stderr, "Output automata description...");
      fflush (stderr);
      output_description ();
      output_automaton_descriptions ();
      fprintf (stderr, "done\n");
      output_statistics (output_description_file);
    }
  output_statistics (stderr);
  ticker_off (&output_time);
  output_time_statistics (stderr);
  finish_states ();
  finish_arcs ();
  finish_automata_lists ();
  if (time_flag)
    {
      fprintf (stderr, "Summary:\n");
      fprintf (stderr, "  check time ");
      print_active_time (stderr, check_time);
      fprintf (stderr, ", generation time ");
      print_active_time (stderr, generation_time);
      fprintf (stderr, ", all time ");
      print_active_time (stderr, all_time);
      fprintf (stderr, "\n");
    }
  /* Finish all work.  */
  if (output_description_file != NULL)
    {
      fflush (output_description_file);
      if (ferror (stdout) != 0)
	fatal ("Error in writing DFA description file %s",
               output_description_file_name);
      fclose (output_description_file);
    }
  finish_automaton_decl_table ();
  finish_insn_decl_table ();
  finish_decl_table ();
  obstack_free (&irp, NULL);
  if (have_error && output_description_file != NULL)
    remove (output_description_file_name);
}

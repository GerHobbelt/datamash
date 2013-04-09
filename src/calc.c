/* calc - perform simple calculation on input data
   Copyright (C) 2013 Assaf Gordon.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* Written by Assaf Gordon */
#include <config.h>

#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>

#include "system.h"

#include "closeout.h"
#include "error.h"
#include "linebuffer.h"
#include "minmax.h"
#include "quote.h"
#include "size_max.h"
#include "version-etc.h"
#include "version.h"
#include "xalloc.h"

#include "key-compare.h"

/* The official name of this program (e.g., no 'g' prefix).  */
#define PROGRAM_NAME "calc"

#define AUTHORS proper_name ("Assaf Gordon")

/* Until someone better comes along */
const char version_etc_copyright[] = "Copyright %s %d Assaf Gordon" ;

/* enable debugging */
static bool debug = false;

/* The character marking end of line. Default to \n. */
static char eolchar = '\n';

/* The character used to separate collapsed/uniqued strings */
static char collapse_separator = ',';

/* Line number in the input file */
static size_t line_number = 0 ;

/* Lines in the current group */
static size_t lines_in_group = 0 ;

/* If true, print the entire input line. Otherwise, print only the key fields */
static bool print_full_line = false;

enum operation
{
  OP_COUNT = 0,
  OP_SUM,
  OP_MIN,
  OP_MAX,
  OP_ABSMIN,
  OP_ABSMAX,
  OP_MEAN,
  OP_MEDIAN,
  OP_PSTDEV,    /* Population Standard Deviation */
  OP_SSTDEV,    /* Sample Standard Deviation */
  OP_PVARIANCE, /* Population Variance */
  OP_SVARIANCE, /* Sample Variance */
  OP_MODE,
  OP_ANTIMODE,
  OP_UNIQUE,        /* Collapse Unique string into comma separated values */
  OP_UNIQUE_NOCASE, /* Collapse Unique strings, ignoring-case */
  OP_COLLAPSE       /* Collapse strings into comma separated values */
};

enum operation_type
{
  NUMERIC_SCALAR = 0,
  NUMERIC_VECTOR,
  STRING_VECTOR
};

enum operation_first_value
{
  AUTO_SET_FIRST = true,
  IGNORE_FIRST = false
};

struct operation_data
{
  const char* name;
  enum operation_type type;
  enum operation_first_value auto_first;
};

struct operation_data operations[] =
{
  {"count",   NUMERIC_SCALAR,  IGNORE_FIRST},   /* OP_COUNT */
  {"sum",     NUMERIC_SCALAR,  IGNORE_FIRST},   /* OP_SUM */
  {"min",     NUMERIC_SCALAR,  AUTO_SET_FIRST}, /* OP_MIN */
  {"max",     NUMERIC_SCALAR,  AUTO_SET_FIRST}, /* OP_MAX */
  {"absmin",  NUMERIC_SCALAR,  AUTO_SET_FIRST}, /* OP_ABSMIN */
  {"absmax",  NUMERIC_SCALAR,  AUTO_SET_FIRST}, /* OP_ABSMAX */
  {"mean",    NUMERIC_SCALAR,  IGNORE_FIRST},   /* OP_MEAN */
  {"median",  NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_MEDIAN */
  {"pstdev",  NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_PSTDEV */
  {"sstdev",  NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_SSTDEV */
  {"pvar",    NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_PVARIANCE */
  {"svar",    NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_SVARIANCE */
  {"mode",    NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_MODE */
  {"antimode",NUMERIC_VECTOR,  IGNORE_FIRST},   /* OP_ANTIMODE */
  {"unique",  STRING_VECTOR,   IGNORE_FIRST},   /* OP_UNIQUE */
  {"uniquenc",STRING_VECTOR,   IGNORE_FIRST},   /* OP_UNIQUE_NOCASE */
  {"collapse",STRING_VECTOR,   IGNORE_FIRST},   /* OP_COLLAPSE */
  {NULL, 0, 0}
};

/* Operation on a field */
struct fieldop
{
    /* operation 'class' information */
  enum operation op;
  enum operation_type type;
  const char* name;
  bool numeric;
  bool auto_first; /* if true, automatically set 'value' if 'first' */

  /* Instance information */
  size_t field; /* field number.  1 = first field in input file. */

  /* Collected Data */
  bool first;   /* true if this is the first item in a new group */

  /* NUMERIC_SCALAR operations */
  size_t count; /* number of items collected so far in a group */
  long double value; /* for single-value operations (sum, min, max, absmin,
                        absmax, mean) - this is the accumulated value */

  /* NUMERIC_VECTOR operations */
  long double *values;     /* array for multi-valued ops (median,mode,stdev) */
  size_t      num_values;  /* number of used values */
  size_t      alloc_values;/* number of allocated values */

  /* String buffer for STRING_VECTOR operations */
  char *str_buf;   /* points to the beginning of the buffer */
  size_t str_buf_used; /* number of bytes used in the buffer */
  size_t str_buf_alloc; /* number of bytes allocated in the buffer */
  char **str_ptr;  /* array of string pointers, into 'str_buf' */
  size_t str_ptr_used; /* number of strings pointers */
  size_t str_ptr_alloc; /* number of string pointers allocated */

  struct fieldop *next;
};

static struct fieldop* field_ops = NULL;

enum { VALUES_BATCH_INCREMENT = 1024 };

/* Add a numeric value to the values vector, allocating memory as needed */
static void
field_op_add_value (struct fieldop *op, long double val)
{
  if (op->num_values >= op->alloc_values)
    {
      op->alloc_values += VALUES_BATCH_INCREMENT;
      op->values = xnrealloc (op->values, op->alloc_values, sizeof (long double));
    }
  op->values[op->num_values] = val;
  op->num_values++;
}

/* Add a string to the strings vector, allocating memory as needed */
static void
field_op_add_string (struct fieldop *op, const char* str, size_t slen)
{
  if (op->str_buf_used + slen+1 >= op->str_buf_alloc)
    {
      op->str_buf_alloc += MAX(VALUES_BATCH_INCREMENT,slen+1);
      op->str_buf = xrealloc (op->str_buf, op->str_buf_alloc);
    }
  if (op->str_ptr_used + 1 >= op->str_ptr_alloc)
    {
      op->str_ptr_alloc += VALUES_BATCH_INCREMENT;
      op->str_ptr = xrealloc (op->str_ptr, op->str_ptr_alloc);
    }

  /* Set the string pointer */
  op->str_ptr[op->str_ptr_used] = op->str_buf + op->str_buf_used;
  op->str_ptr_used++;

  /* Copy the string to the buffer */
  memcpy (op->str_buf + op->str_buf_used, str, slen);
  op->str_buf_used += slen;
  *(op->str_buf + op->str_buf_used) = 0;
  op->str_buf_used++;
}

/* Compare two flowting-point variables, while avoiding '==' .
see:
http://www.gnu.org/software/libc/manual/html_node/Comparison-Functions.html */
static int
cmp_long_double (const void *p1, const void *p2)
{
  const long double *a = (const long double *)p1;
  const long double *b = (const long double *)p2;
  return ( *a > *b ) - (*a < *b);
}

/* Sort the numeric values vector in a fieldop structure */
static void
field_op_sort_values (struct fieldop *op)
{
  qsort (op->values, op->num_values, sizeof (long double), cmp_long_double);
}

/* Allocate a new fieldop, initialize it based on 'oper',
   and add it to the linked-list of operations */
static void
new_field_op (enum operation oper, size_t field)
{
  struct fieldop *op = XZALLOC(struct fieldop);

  op->op = oper;
  op->type = operations[oper].type;
  op->name = operations[oper].name;
  op->numeric = (op->type == NUMERIC_SCALAR || op->type == NUMERIC_VECTOR);
  op->auto_first = operations[oper].auto_first;

  op->field = field;
  op->first = true;
  op->value = 0;
  op->count = 0;

  op->next = NULL;

  if (field_ops != NULL)
    {
      struct fieldop *p = field_ops;
      while (p->next != NULL)
        p = p->next;
      p->next = op;
    }
  else
    field_ops = op;
}

/* Add a value (from input) to the current field operation.
   If the operation is numeric, num_value should be used.
   If the operation is textual, str +slen should be used
     (str is not guarenteed to be null terminated).

   Return value (boolean, keep_line) isn't used at the moment. */
static bool
field_op_collect (struct fieldop *op,
                  const char* str, size_t slen,
                  const long double num_value)
{
  bool keep_line = false;

  if (debug)
    {
      fprintf (stderr, "-- collect for %s(%zu) val='", op->name, op->field);
      fwrite (str, sizeof(char), slen, stderr);
      fprintf (stderr, "'\n");
    }

  op->count++;

  if (op->first && op->auto_first)
      op->value = num_value;

  switch (op->op)
    {
    case OP_SUM:
    case OP_MEAN:
      op->value += num_value;
      keep_line = op->first;
      break;

    case OP_COUNT:
      op->value++;
      keep_line = op->first;
      break;

    case OP_MIN:
      if (num_value < op->value)
        {
          keep_line = true;
          op->value = num_value;
        }
      break;

    case OP_MAX:
      if (num_value > op->value)
        {
          keep_line = true;
          op->value = num_value;
        }
      break;

    case OP_ABSMIN:
      if (fabsl(num_value) < fabsl(op->value))
        {
          keep_line = true;
          op->value = num_value;
        }
      break;

    case OP_ABSMAX:
      if (fabsl(num_value) > fabsl(op->value))
        {
          keep_line = true;
          op->value = num_value;
        }
      break;

    case OP_MEDIAN:
    case OP_PSTDEV:
    case OP_SSTDEV:
    case OP_PVARIANCE:
    case OP_SVARIANCE:
    case OP_MODE:
    case OP_ANTIMODE:
      field_op_add_value (op, num_value);
      break;

    case OP_UNIQUE:
    case OP_UNIQUE_NOCASE:
    case OP_COLLAPSE:
      field_op_add_string (op, str, slen);
      break;

    default:
      break;
    }

  if (op->first)
    op->first = false;

  return keep_line;
}

inline static long double
median_value ( const long double * const values, size_t n )
{
  /* Assumes 'values' are already sorted, returns the median value */
  return (n&0x01)
    ?values[n/2]
    :( (values[n/2-1] + values[n/2]) / 2.0 );
}

enum degrees_of_freedom
{
  DF_POPULATION = 0,
  DF_SAMPLE = 1
};

inline static long double
variance_value ( const long double * const values, size_t n, int df )
{
  long double sum=0;
  long double mean;
  long double variance;

  for (size_t i = 0; i < n; i++)
    sum += values[i];
  mean = sum / n ;

  sum = 0 ;
  for (size_t i = 0; i < n; i++)
    sum += (values[i] - mean) * (values[i] - mean);

  variance = sum / ( n - df );

  return variance;
}

inline static long double
stdev_value ( const long double * const values, size_t n, int df )
{
  return sqrtl ( variance_value ( values, n, df ) );
}


enum MODETYPE
{
  MODE=1,
  ANTIMODE
};

inline static long double
mode_value ( const long double * const values, size_t n, enum MODETYPE type)
{
  /* not ideal implementation but simple enough */
  /* Assumes 'values' are already sorted, find the longest sequence */
  long double last_value = values[0];
  size_t seq_size=1;
  size_t best_seq_size= (type==MODE)?1:SIZE_MAX;
  size_t best_value = values[0];

  for (size_t i=1; i<n; i++)
    {
      bool eq = (cmp_long_double(&values[i],&last_value)==0);

      if (eq)
        seq_size++;

      if ( ((type==MODE) && (seq_size > best_seq_size))
           ||
           ((type==ANTIMODE) && (seq_size < best_seq_size)))
        {
          best_seq_size = seq_size;
          best_value = last_value;
        }

      if (!eq)
          seq_size = 1;

      last_value = values[i];
    }
  return best_value;
}

static int
cmpstringp(const void *p1, const void *p2)
{
  /* The actual arguments to this function are "pointers to
   * pointers to char", but strcmp(3) arguments are "pointers
   * to char", hence the following cast plus dereference */
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

static int
cmpstringp_nocase(const void *p1, const void *p2)
{
  /* The actual arguments to this function are "pointers to
   * pointers to char", but strcmp(3) arguments are "pointers
   * to char", hence the following cast plus dereference */
  return strcasecmp(* (char * const *) p1, * (char * const *) p2);
}



/* Returns a nul-terimated string, composed of the unique values
   of the input strings. The return string must be free()'d. */
static char *
unique_value ( struct fieldop *op, bool case_sensitive )
{
  const char *last_str;
  char *buf, *pos;

  /* Sort the string pointers */
  qsort ( op->str_ptr, op->str_ptr_used, sizeof(char*), case_sensitive
                                                          ?cmpstringp
                                                          :cmpstringp_nocase);

  /* Uniquify them */
  pos = buf = xzalloc ( op->str_buf_used );

  /* Copy the first string */
  last_str = op->str_ptr[0];
  strcpy (buf, op->str_ptr[0]);
  pos += strlen(op->str_ptr[0]);

  /* Copy the following strings, if they are different from the previous one */
  for (size_t i = 1; i < op->str_ptr_used ; ++i)
    {
      const char *newstr = op->str_ptr[i];

      if ((case_sensitive && (strcmp(newstr, last_str)!=0))
          ||
          (!case_sensitive && (strcasecmp(newstr, last_str)!=0)))
        {
          *pos++ = collapse_separator ;
          strcpy (pos, newstr);
          pos += strlen(newstr);
        }
      last_str = newstr;
    }

  return buf;
}

/* Returns a nul-terimated string, composed of all the values
   of the input strings. The return string must be free()'d. */
static char *
collapse_value ( struct fieldop *op )
{
  /* Copy the string buffer as-is */
  char *buf = xzalloc ( op->str_buf_used );
  memcpy (buf, op->str_buf, op->str_buf_used);

  /* convert every NUL to comma, except for the last one */
  for (size_t i=0; i < op->str_buf_used-1 ; i++)
      if (buf[i] == 0)
        buf[i] = collapse_separator ;

  return buf;
}

/* Prints to stdout the result of the field operation,
   based on collected values */
static void
field_op_summarize (struct fieldop *op)
{
  long double numeric_result = 0 ;
  char *string_result = NULL;

  if (debug)
    fprintf (stderr, "-- summarize for %s(%zu)\n", op->name, op->field);

  switch (op->op)
    {
    case OP_MEAN:
      numeric_result = op->value / op->count;
      break;

    case OP_SUM:
    case OP_COUNT:
    case OP_MIN:
    case OP_MAX:
    case OP_ABSMIN:
    case OP_ABSMAX:
      /* no summarization for these operations, just print the value */
      numeric_result = op->value;
      break;

    case OP_MEDIAN:
      field_op_sort_values (op);
      numeric_result = median_value ( op->values, op->num_values );
      break;

    case OP_PSTDEV:
      numeric_result = stdev_value ( op->values, op->num_values, DF_POPULATION);
      break;

    case OP_SSTDEV:
      numeric_result = stdev_value ( op->values, op->num_values, DF_SAMPLE);
      break;

    case OP_PVARIANCE:
      numeric_result = variance_value ( op->values, op->num_values,
                                        DF_POPULATION);
      break;

    case OP_SVARIANCE:
      numeric_result = variance_value ( op->values, op->num_values,
                                        DF_SAMPLE);
      break;

    case OP_MODE:
    case OP_ANTIMODE:
      field_op_sort_values (op);
      numeric_result = mode_value ( op->values, op->num_values,
                                    (op->op==OP_MODE)?MODE:ANTIMODE);
      break;

    case OP_UNIQUE:
    case OP_UNIQUE_NOCASE:
      string_result = unique_value (op, (op->op==OP_UNIQUE));
      break;

    case OP_COLLAPSE:
      string_result = collapse_value (op);
      break;

    default:
      error (EXIT_FAILURE, 0, _("internal error 2"));
      break;
    }


  if (debug)
    {
      if (op->numeric)
        fprintf (stderr, "%s(%zu) = %Lg\n", op->name, op->field, numeric_result);
      else
        fprintf (stderr, "%s(%zu) = '%s'\n", op->name, op->field, string_result);
    }

  if (op->numeric)
    printf ("%Lg", numeric_result);
  else
    printf ("%s", string_result);

  free (string_result);
}

static void
summarize_field_ops ()
{
  for (struct fieldop *p = field_ops; p ; p=p->next)
    {
      field_op_summarize (p);

      /* print field separator */
      if (p->next)
        putchar( (tab==TAB_DEFAULT)?' ':tab );
    }

    /* print end-of-line */
    putchar(eolchar);
}

/* reset operation values for next group */
static void
reset_field_op (struct fieldop *op)
{
  op->first = true;
  op->count = 0 ;
  op->value = 0;
  op->num_values = 0 ;
  op->str_ptr_used = 0;
  op->str_buf_used = 0;
}

/* reset all field operations, for next group */
static void
reset_field_ops ()
{
  for (struct fieldop *p = field_ops; p ; p = p->next)
    reset_field_op (p);
}

/* Frees all memory associated with a field operation struct.
   returns the 'next' field operation, or NULL */
static struct fieldop *
free_field_op (struct fieldop *op)
{
  struct fieldop *next;

  if (!op)
    return NULL;

  next = op->next;

  if (op->values)
      free (op->values);
  op->num_values = 0 ;
  op->alloc_values = 0;

  free(op->str_buf);
  op->str_buf = NULL;
  op->str_buf_alloc = 0;
  op->str_buf_used = 0;

  free (op->str_ptr);
  op->str_ptr = NULL;
  op->str_ptr_used = 0;
  op->str_ptr_alloc = 0;

  free(op);

  return next;
}

static void
free_field_ops ()
{
  struct fieldop *p = field_ops;
  while (p)
    p = free_field_op(p);
}

enum
{
  DEBUG_OPTION = CHAR_MAX + 1,
};

static char const short_options[] = "fzg:k:t:";

static struct option const long_options[] =
{
  {"zero-terminated", no_argument, NULL, 'z'},
  {"field-separator", required_argument, NULL, 't'},
  {"groups", required_argument, NULL, 'g'},
  {"key", required_argument, NULL, 'k'},
  {"full", no_argument, NULL, 'f'},
  {"debug", no_argument, NULL, DEBUG_OPTION},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0},
};

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPTION] op col [op col ...]\n\
"),
              program_name);
      fputs (_("\
Performs numeric/string operations on input from stdin.\n\
"), stdout);
      fputs("\
\n\
'op' is the operation to perform on field 'col'.\n\
\n\
Numeric operations:\n\
  count      count number of elements in the input group\n\
  sum        print the sum the of values\n\
  min        print the minimum value\n\
  max        print the maximum value\n\
  absmin     print the minimum of abs(values)\n\
  absmax     print the maximum of abs(values)\n\
  mean       print the mean of the values\n\
  median     print the median value\n\
  mode       print the mode value (most common value)\n\
  antimode   print the anti-mode value (least common value)\n\
  pstdev     print the population standard deviation\n\
  sstdev     print the sample standard deviation\n\
  pvar       print the population variance\n\
  svar       print the sample variance\n\
\n\
String operations:\n\
  unique     print comma-separated sorted list of unique values\n\
  uniquenc   Same as above, while ignoring upper/lower case letters.\n\
  collapse   print comma-separed list of all input values\n\
\n\
",stdout);
      fputs (_("\
\n\
General options:\n\
  -f, --full                Print entire input line before op results\n\
                            (default: print only the groupped keys)\n\
  -g, --groups=X[,Y,Z,]     Group via fields X,[Y,X]\n\
                            This is a short-cut for --key:\n\
                            '-g5,6' is equivalent to '-k5,5 -k6,6'\n\
  -k, --key=KEYDEF          Group via a key; KEYDEF gives location and type\n\
  -t, --field-separator=X    use X instead of whitespace for field delimiter\n\
  -z, --zero-terminated     end lines with 0 byte, not newline\n\
"), stdout);

      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);

      printf (_("\
\n\
Examples:\n\
  Print the mean and the median of values from column 1\n\
  $ seq 10 | %s mean 1 median 1\n\
\n\
  Group input based on field 1, and sum values (per group) on field 2:\n\
  $ printf '%%s %%d\n' A 10 A 5 B 9 | %s -g1 sum 2\n\
\n\
  Unsorted input must be sorted:\n\
  $ cat INPUT.TXT | sort -k1,1 | %s -k1,1 mean 2\n\
\n\
"), program_name, program_name, program_name);

    }
  exit (status);
}

/* Given a string with operation name, returns the operation enum.
   exits with an error message if the string is not a valid/known operation. */
static enum operation
get_operation (const char* op)
{
  for (size_t i = 0; operations[i].name ; i++)
      if ( STREQ(operations[i].name, op) )
        return (enum operation)i;

  error (EXIT_FAILURE, 0, _("invalid operation '%s'"), op);
  return 0; /* never reached */
}

/* Converts a string to number (field number).
   Exits with an error message (using 'op') on invalid field number. */
static size_t
get_field_number(enum operation op, const char* field_str)
{
  size_t val;
  char *endptr;
  errno = 0 ;
  val = strtoul (field_str, &endptr, 10);
  /* NOTE: can't use xstrtol_fatal - it's too tightly-coupled
     with getopt command-line processing */
  if (errno != 0 || endptr == field_str)
    error (EXIT_FAILURE, 0, _("invalid column '%s' for operation " \
                               "'%s'"), field_str,
                               operations[op].name);
  return val;
}

/* Extract the operation patterns from args START through ARGC - 1 of ARGV. */
static void
parse_operations (int argc, int start, char **argv)
{
  int i = start;	/* Index into ARGV. */
  size_t field;
  enum operation op;

  while ( i < argc )
    {
      op = get_operation (argv[i]);
      i++;
      if ( i >= argc )
        error (EXIT_FAILURE, 0, _("missing field number after " \
                                  "operation '%s'"), argv[i-1] );
      field = get_field_number (op, argv[i]);
      i++;

      new_field_op (op, field);
    }
}

/* Force NUL-termination of the string in the linebuffer struct.
   NOTE 1: The buffer is assumed to contain NUL later on in the program,
           and is used in 'strtoul()'.
   NOTE 2: The buffer can not be simply chomp'd (by derementing length),
           because sort's "keycompare()" function assume the last valid index
           is one PAST the last character of the line (i.e. there is an EOL
           charcter in the buffer). */
inline static void
linebuffer_nullify (struct linebuffer *line)
{
  if (line->buffer[line->length-1]==eolchar)
    {
      line->buffer[line->length-1] = 0; /* make it NUL terminated */
    }
  else
    {
      /* TODO: verify this is safe, and the allocated buffer is large enough */
      line->buffer[line->length] = 0;
    }
}

static void
get_field (const struct linebuffer *line, size_t field,
               const char** /* OUT*/ _ptr, size_t /*OUT*/ *_len)
{
  size_t pos = 0;
  size_t flen = 0;
  const size_t buflen = line->length;
  char* fptr = line->buffer;
  /* Move 'fptr' to point to the beginning of 'field' */
  if (tab != TAB_DEFAULT)
    {
      /* delimiter is explicit character */
      while ((pos<buflen) && --field)
        {
          while ( (pos<buflen) && (*fptr != tab))
            {
              ++fptr;
              ++pos;
            }
          if ( (pos<buflen) && (*fptr == tab))
            {
              ++fptr;
              ++pos;
            }
        }
    }
  else
    {
      /* delimiter is white-space transition
         (multiple whitespaces are one delimiter) */
      while ((pos<buflen) && --field)
        {
          while ( (pos<buflen) && !isblank (*fptr))
            {
              ++fptr;
              ++pos;
            }
          while ( (pos<buflen) && isblank (*fptr))
            {
              ++fptr;
              ++pos;
            }
        }
    }

  /* Find the length of the field (until the next delimiter/eol) */
  if (tab != TAB_DEFAULT)
    {
      while ( (pos+flen<buflen) && (*(fptr+flen) != tab) )
        flen++;
    }
  else
    {
      while ( (pos+flen<buflen) && !isblank (*(fptr+flen)) )
        flen++;
    }

  /* Chomp field if needed */
  if ( (flen>1) && ((*(fptr + flen -1) == 0) || (*(fptr+flen-1)==eolchar)) )
    flen--;

  *_len = flen;
  *_ptr = fptr;
}

inline static long double
safe_strtold ( const char *str, size_t len, size_t field )
{
  char *endptr=NULL;

  errno = 0;
  long double val = strtold (str, &endptr);
  if (errno==ERANGE || endptr==str)
    {
      char *tmp = strdup(str);
      tmp[len] = 0 ;
      /* TODO: make invalid input error or warning or silent */
      error (EXIT_FAILURE, 0,
          _("invalid numeric input in line %zu field %zu: '%s'"),
          line_number, field, tmp);
    }
  return val;
}

/* Given a "struct linebuffer", initializes a sort-compatible "struct line"
 * (and finds the first key field) */
/* copied from coreutils's src/uniq.c (in the key-spec branch) */
static void
prepare_line (const struct linebuffer *linebuf, struct line* /*output*/ line,
              char eol_delimiter)
{
  size_t len = linebuf->length;

  line->text = linebuf->buffer;
  line->length = linebuf->length;
  line->keybeg = NULL; /* TODO: are these the right initializers? */
  line->keylim = NULL;

  if (line->text[line->length-1] == eol_delimiter)
    len--;

  line->keybeg = begfield (line, keylist);
  line->keylim = limfield (line, keylist);

  if (line->keybeg >= line->keylim)
    {
      /* TODO: is this the correct way to detect 'no limit' ?
       * (ie compare keys until the end of the line)*/
      line->keylim = line->keybeg + len - (line->keybeg - line->text);
    }
}

/* returns TRUE if the lines are different, false if identical.
 * comparison is based on the specified keys */
/* copied from coreutils's src/uniq.c (in the key-spec branch) */
static bool
different (const struct line* l1, const struct line* l2)
{
  int diff = keycompare (l1,l2);
  return (diff != 0);
}

/* For a given line, extract all requested fields and process the associated
   operations on them */
static void
process_line (const struct linebuffer *line)
{
  long double val=0;
  const char *str;
  size_t len;

  struct fieldop *op = field_ops;
  while (op)
    {
      get_field (line, op->field, &str, &len);
      if (debug)
        {
          fprintf(stderr,"getfield(%zu) = len %zu: '", op->field,len);
          fwrite(str,sizeof(char),len,stderr);
          fprintf(stderr,"'\n");
        }

      if (op->numeric)
        val = safe_strtold ( str, len, op->field );

      field_op_collect (op, str, len, val);

      op = op->next;
    }
}

/* Print the input line representing the summarized group.
   if '--full' - print the entire line.
   if not full, print only the keys used for grouping.
 */
static void
print_input_line (const struct linebuffer* lb)
{
  if (print_full_line)
    {
      size_t len = lb->length;
      const char *buf = lb->buffer;
      if (buf[len-1]==eolchar || buf[len-1]==0)
        len--;
      fwrite (buf, sizeof(char), len, stdout);
      putchar( (tab==TAB_DEFAULT)?' ':tab );
    }
  else
    {
      struct keyfield *key = keylist;
      struct line li ;
      li.text = lb->buffer;
      li.length = lb->length;
      while (key)
        {
          const char *keybeg = begfield (&li, key);
          const char *keylim = limfield (&li, key);
          while (isblank(*keybeg))
            keybeg++;
          size_t len = keylim - keybeg;
          fwrite (keybeg, sizeof(char), len, stdout);
          putchar( (tab==TAB_DEFAULT)?' ':tab );
          key = key->next;
        }
    }
}

#define SWAP_LINES(A, B)			\
  do						\
    {						\
      struct linebuffer *_tmp;			\
      _tmp = (A);				\
      (A) = (B);				\
      (B) = _tmp;				\
    }						\
  while (0)

#define SWAP_SORT_LINES(A, B)			\
  do						\
    {						\
      struct line *_tmp;			\
      _tmp = (A);				\
      (A) = (B);				\
      (B) = _tmp;				\
    }						\
  while (0)


/*
    Process each line in the input.

    If the key fo the current line is different from the prevopis one,
    summarize the previous group and start a new one.
 */
static void
process_file ()
{
  struct linebuffer lb1, lb2;
  struct linebuffer *thisline, *prevline;

   /* line structure used for sort's key comparison*/
  struct line sortlb1, sortlb2;
  struct line *thislinesort, *prevlinesort;

  thisline = &lb1;
  prevline = &lb2;

  thislinesort = &sortlb1;
  prevlinesort = &sortlb2;

  initbuffer (thisline);
  initbuffer (prevline);

  while (!feof (stdin))
    {
      bool new_group = false;

      if (readlinebuffer_delim (thisline, stdin, eolchar) == 0)
        break;
      linebuffer_nullify (thisline);
      line_number++;

      /* If no keys are given, the entire input is considered one group */
      if (keylist)
        {
          prepare_line (thisline, thislinesort, eolchar);
          new_group = (prevline->length == 0
                       || different (thislinesort, prevlinesort));

          if (debug)
            {
              fprintf(stderr,"thisline = '");
              fwrite(thisline->buffer,sizeof(char),thisline->length,stderr);
              fprintf(stderr,"'\n");
              fprintf(stderr,"prevline = '");
              fwrite(prevline->buffer,sizeof(char),prevline->length,stderr);
              fprintf(stderr,"'\n");
              fprintf(stderr, "newgroup = %d\n", new_group);
            }

          if (new_group && lines_in_group>0)
            {
              print_input_line (prevline);
              summarize_field_ops ();
              reset_field_ops ();
              lines_in_group = 0 ;
            }
        }

      lines_in_group++;
      process_line (thisline);

      if (new_group)
        {
          SWAP_LINES (prevline, thisline);
          SWAP_SORT_LINES (prevlinesort, thislinesort);
        }
    }

  /* summarize last group */
  if (lines_in_group)
    {
      print_input_line (print_full_line ? thisline : prevline);
      summarize_field_ops ();
      reset_field_ops ();
    }

  if (ferror (stdin) || fclose (stdin) != 0)
    error (EXIT_FAILURE, 0, _("read error"));

  free (lb1.buffer);
  free (lb2.buffer);
}

/* Parse the "--group=X[,Y,Z]" parameter, populating 'keylist' */
static void
parse_group_spec ( const char* spec )
{
  size_t val;
  char *endptr;
  struct keyfield *key;
  struct keyfield key_buf;


  errno = 0 ;
  while (1)
    {
      val = strtoul (spec, &endptr, 10);
      if (errno != 0 || endptr == spec)
        error (EXIT_FAILURE, 0, _("invalid field value for grouping '%s'"),
                                        spec);
      if (val==0)
        error (EXIT_FAILURE, 0, _("invalid field value (zero) for grouping"));

      /* Emulate a '-key Xb,X' parameter */
      key = key_init (&key_buf);
      key->sword = val-1;
      key->eword = val-1;
      insertkey (key);

      if (endptr==NULL || *endptr==0)
        break;
      if (*endptr != ',')
        error (EXIT_FAILURE, 0, _("invalid grouping parameter '%s'"), endptr);
      endptr++;
      spec = endptr;
    }
}

/* Parse the "--key POS1,POS2" parameter, adding the key to 'keylist'.
   This code is copied from coreutils' "sort". */
static void
parse_key_spec ( const char *spec )
{
  struct keyfield *key;
  struct keyfield key_buf;
  char const *s;

  key = key_init (&key_buf);

  /* Get POS1. */
  s = parse_field_count (spec, &key->sword,
                         N_("invalid number at field start"));
  if (! key->sword--)
    {
      /* Provoke with 'sort -k0' */
      badfieldspec (spec, N_("field number is zero"));
    }
  if (*s == '.')
    {
      s = parse_field_count (s + 1, &key->schar,
                             N_("invalid number after '.'"));
      if (! key->schar--)
        {
          /* Provoke with 'sort -k1.0' */
          badfieldspec (spec, N_("character offset is zero"));
        }
    }
  if (! (key->sword || key->schar))
    key->sword = SIZE_MAX;
  s = set_ordering (s, key, bl_start);
  if (*s != ',')
    {
      key->eword = SIZE_MAX;
      key->echar = 0;
    }
  else
    {
      /* Get POS2. */
      s = parse_field_count (s + 1, &key->eword,
                             N_("invalid number after ','"));
      if (! key->eword--)
        {
          /* Provoke with 'sort -k1,0' */
          badfieldspec (spec, N_("field number is zero"));
        }
      if (*s == '.')
        {
          s = parse_field_count (s + 1, &key->echar,
                                 N_("invalid number after '.'"));
        }
      s = set_ordering (s, key, bl_end);
    }
  if (*s)
    badfieldspec (spec, N_("stray character in field spec"));
  /* TODO: strange, sort's original code sets sword=SIZE_MAX for "-k1".
   * force override??? */
  if (key->sword==SIZE_MAX)
    key->sword=0;
  insertkey (key);
}

int main(int argc, char* argv[])
{
  int optc;

  set_program_name (argv[0]);

  setlocale (LC_ALL, "");

  init_key_spec ();

  atexit (close_stdout);

  while ((optc = getopt_long (argc, argv, short_options, long_options, NULL))
         != -1)
    {
      switch (optc)
        {
        case 'f':
          print_full_line = true;
          break;

        case 'g':
          parse_group_spec (optarg);
          break;

        case 'k':
          parse_key_spec (optarg);
          break;

        case 'z':
          eolchar = 0;
          break;

        case DEBUG_OPTION:
          debug = true;
          break;

        case 't':
          /* Interpret -t '' to mean 'use the NUL byte as the delimiter.'  */
          if (optarg[0] != '\0' && optarg[1] != '\0')
            error (EXIT_FAILURE, 0,
                   _("the delimiter must be a single character"));
          tab = optarg[0];
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (argc <= optind)
    {
      error (0, 0, _("missing operation specifiers"));
      usage (EXIT_FAILURE);
    }

  parse_operations (argc, optind, argv);
  process_file ();
  free_field_ops ();

  return EXIT_SUCCESS;
}

/* vim: set cinoptions=>4,n-2,{2,^-2,:2,=2,g0,h2,p5,t0,+2,(0,u0,w1,m1: */
/* vim: set shiftwidth=2: */
/* vim: set tabstop=8: */
/* vim: set expandtab: */

/*
 * genutils.h
 *
 * General utility functions
 * 
 * Joe Conway <joe@crunchydata.com>
 *
 * This code is released under the PostgreSQL license.
 *
 * Copyright 2020 Crunchy Data Solutions, Inc.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice
 * and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL CRUNCHY DATA SOLUTIONS, INC. BE LIABLE TO ANY PARTY
 * FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
 * INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE CRUNCHY DATA SOLUTIONS, INC. HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE CRUNCHY DATA SOLUTIONS, INC. SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE CRUNCHY DATA SOLUTIONS, INC. HAS NO
 * OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
 * MODIFICATIONS.
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"

#include "genutils.h"

static int guc_var_compare(const void *a, const void *b);
static int guc_name_compare(const char *namea, const char *nameb);

/*
 * Convert a 2D array of strings into a tuplestore and return it
 * as an SRF result.
 * 
 * fcinfo is the called SQL facing function call info
 * values is the 2D array of strings to convert
 * nrow and ncol provide the array dimensions
 * dtypes is an array of data type oids for the output tuple
 */
Datum
form_srf(FunctionCallInfo fcinfo, char ***values, int nrow, int ncol, Oid *dtypes)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate	   *tupstore;
	HeapTuple			tuple;
	TupleDesc			tupdesc;
	AttInMetadata	   *attinmeta;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	int					i;

	/* check to see if caller supports us returning a tuplestore */
	if (!rsinfo || !(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/*
	 * Check to make sure we have a reasonable tuple descriptor
	 */
	if (tupdesc->natts != ncol)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query-specified return tuple and "
						"function return type are not compatible"),
				 errdetail("Number of columns mismatch")));
	}
	else
	{
		for (i = 0; i < ncol; ++i)
		{
			Oid		tdtyp = TupleDescAttr(tupdesc, i)->atttypid;

			if (tdtyp != dtypes[i])
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("query-specified return tuple and "
							"function return type are not compatible"),
					 errdetail("Expected %s, got %s", format_type_be(dtypes[i]), format_type_be(tdtyp))));
		}
	}

	/* OK to use it */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* let the caller know we're sending back a tuplestore */
	rsinfo->returnMode = SFRM_Materialize;

	/* initialize our tuplestore */
	tupstore = tuplestore_begin_heap(true, false, work_mem);

	for (i = 0; i < nrow; ++i)
	{
		char	   **rowvals = values[i];

		tuple = BuildTupleFromCStrings(attinmeta, rowvals);
		tuplestore_puttuple(tupstore, tuple);
	}

	/*
	 * no longer need the tuple descriptor reference created by
	 * TupleDescGetAttInMetadata()
	 */
	ReleaseTupleDesc(tupdesc);

	tuplestore_donestoring(tupstore);
	rsinfo->setResult = tupstore;

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 */
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	return (Datum) 0;
}

/* qsort comparison function for int64 */
int
int64_cmp(const void *p1, const void *p2)
{
	int64	v1 = *((const int64 *) p1);
	int64	v2 = *((const int64 *) p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}

/*
 * Functions for obtaining the context within which we are operating
 */
/*
 * Look up GUC option NAME. If it exists, return a pointer to its record,
 * else return NULL. This is cribbed from guc.c -- unfortunately there
 * seems to be no exported functionality to get the entire record by name.
 */
struct config_generic *
find_option(const char *name)
{
	const char			  **key = &name;
	struct config_generic **res;
	struct config_generic **guc_vars;
	int                     numOpts;

	Assert(name);

	guc_vars = get_guc_variables();
	numOpts = GetNumConfigOptions();

	/*
	 * By equating const char ** with struct config_generic *, we are assuming
	 * the name field is first in config_generic.
	 */
	res = (struct config_generic **) bsearch((void *) &key,
											 (void *) guc_vars,
											 numOpts,
											 sizeof(struct config_generic *),
											 guc_var_compare);
	if (res)
		return *res;

	/* Unknown name */
	return NULL;
}

/*
 * Additional utility functions cribbed from guc.c
 */

/*
 * comparator for qsorting and bsearching guc_variables array
 */
static int
guc_var_compare(const void *a, const void *b)
{
	const struct config_generic *confa = *(struct config_generic *const *) a;
	const struct config_generic *confb = *(struct config_generic *const *) b;

	return guc_name_compare(confa->name, confb->name);
}

/*
 * the bare comparison function for GUC names
 */
static int
guc_name_compare(const char *namea, const char *nameb)
{
	/*
	 * The temptation to use strcasecmp() here must be resisted, because the
	 * array ordering has to remain stable across setlocale() calls. So, build
	 * our own with a simple ASCII-only downcasing.
	 */
	while (*namea && *nameb)
	{
		char		cha = *namea++;
		char		chb = *nameb++;

		if (cha >= 'A' && cha <= 'Z')
			cha += 'a' - 'A';
		if (chb >= 'A' && chb <= 'Z')
			chb += 'a' - 'A';
		if (cha != chb)
			return cha - chb;
	}
	if (*namea)
		return 1;				/* a is longer */
	if (*nameb)
		return -1;				/* b is longer */
	return 0;
}

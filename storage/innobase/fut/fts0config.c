/*****************************************************************************

Copyright (c) 1995, 2009, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/******************************************************************//**
@file fut/fts0config.c
Full Text Search configuration table.

Created 9/5/2007 Sunny Bains
***********************************************************************/


#include "trx0roll.h"
#include "row0sel.h"

#include "fts0priv.h"

#ifndef UNIV_NONINL
#include "fts0types.ic"
#include "fts0vlc.ic"
#endif

/********************************************************************
Callback function for fetching the config value. */
static
ibool
fts_config_fetch_value(
/*===================*/
						/* out: always returns
						non-NULL */
	void*		row,			/* in: sel_node_t* */
	void*		user_arg)		/* in: pointer to
						 ib_vector_t */
{
	sel_node_t*	node = row;
	fts_string_t*	value = user_arg;

	dfield_t*	dfield = que_node_get_val(node->select_list);
	dtype_t*	type = dfield_get_type(dfield);
	ulint		len = dfield_get_len(dfield);
	void*		data = dfield_get_data(dfield);

	ut_a(dtype_get_mtype(type) == DATA_VARCHAR);

	if (len != UNIV_SQL_NULL) {
		ulint	max_len = ut_min(value->len - 1, len);

		memcpy(value->utf8, data, max_len);
		value->len = max_len;
		value->utf8[value->len] = '\0';
	}

	return(TRUE);
}

/********************************************************************
Get value from the config table. The caller must ensure that enough
space is allocated for value to hold the column contents. */

ulint
fts_config_get_value(
/*=================*/
						/* out: DB_SUCCESS or error
						code */
	trx_t*		trx,			/* transaction */
	fts_table_t*	fts_table,		/* in: the indexed FTS table */
	const char*	name,			/* in: get config value for
						this parameter name */
	fts_string_t*	value)			/* out: value read from
						config table */
{
	pars_info_t*	info;
	que_t*		graph;
	ulint		error;
	ulint		name_len = strlen(name);

	info = pars_info_create();

	*value->utf8 = '\0';
	ut_a(value->len > 0);

	pars_info_bind_function(info, "my_func", fts_config_fetch_value,
				value);

	/* The len field of value must be set to the max bytes that
	it can hold. On a successful read, the len field will be set
	to the actual number of bytes copied to value. */
	pars_info_bind_varchar_literal(info, "name", (byte*) name, name_len);

	fts_table->suffix = "CONFIG";

	graph = fts_parse_sql(
		fts_table,
		info,
		"DECLARE FUNCTION my_func;\n"
		"DECLARE CURSOR c IS SELECT value FROM %s"
		" WHERE key = :name;\n"
		"BEGIN\n"
		""
		"OPEN c;\n"
		"WHILE 1 = 1 LOOP\n"
		"  FETCH c INTO my_func();\n"
		"  IF c % NOTFOUND THEN\n"
		"    EXIT;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE c;");

	trx->op_info = "getting FTS config value";

	error = fts_eval_sql(trx, graph);

	mutex_enter(&dict_sys->mutex);
	que_graph_free(graph);
	mutex_exit(&dict_sys->mutex);

	return(error);
}

/********************************************************************
Create the config table name for retrieving index specific value. */
static
char*
fts_config_create_index_param_name(
/*===============================*/
						/* out,own: index config
						parameter name */
	const char*		param,		/* in: base name of param */
	const dict_index_t*	index)		/* in: index for config */
{
	ulint		len;
	char*		name;

	/* The format of the config name is: name_<index_id>. */
	len = strlen(param);

	/* Caller is responsible for deleting name. */
	name = ut_malloc(len + FTS_AUX_MIN_TABLE_ID_LENGTH + 2);
	strcpy(name, param);
	name[len] = '_';

	fts_write_object_id(index->id, name + len + 1);

	return(name);
}

/********************************************************************
Get value specific to an FTS index from the config table. The caller
must ensure that enough space is allocated for value to hold the
column contents. */

ulint
fts_config_get_index_value(
/*=======================*/
						/* out: DB_SUCCESS or error
						code */
	trx_t*		trx,			/* transaction */
	dict_index_t*	index,			/* in: index */
	const char*	param,			/* in: get config value for
						this parameter name */
	fts_string_t*	value)			/* out: value read from
						config table */
{
	char*		name;
	ulint		error;
	fts_table_t	fts_table;

	fts_table.suffix = "CONFIG";
	fts_table.type = FTS_COMMON_TABLE;
	fts_table.table_id = index->table->id;
	fts_table.parent = index->table->name;

	/* We are responsible for free'ing name. */
	name = fts_config_create_index_param_name(param, index);

	error = fts_config_get_value(trx, &fts_table, name, value);

	ut_free(name);

	return(error);
}

/********************************************************************
Set the value in the config table for name. */

ulint
fts_config_set_value(
/*=================*/
						/* out: DB_SUCCESS or error
						code */
	trx_t*		trx,			/* transaction */
	fts_table_t*	fts_table,		/* in: the indexed FTS table */
	const char*	name,			/* in: get config value for
						this parameter name */
	const fts_string_t*
			value)			/* in: value to update */
{
	pars_info_t*	info;
	que_t*		graph;
	ulint		error;
	undo_no_t	undo_no;
	undo_no_t	n_rows_updated;
	ulint		name_len = strlen(name);

	info = pars_info_create();

	pars_info_bind_varchar_literal(info, "name", (byte*) name, name_len);
	pars_info_bind_varchar_literal(info, "value", value->utf8, value->len);

	fts_table->suffix = "CONFIG";

	row_mysql_lock_data_dictionary(trx);

	graph = fts_parse_sql_no_dict_lock(
		fts_table, info,
		"BEGIN UPDATE %s SET value = :value WHERE key = :name;");

	trx->op_info = "setting FTS config value";

	undo_no = trx->undo_no;

	error = fts_eval_sql(trx, graph);

	que_graph_free(graph);

	n_rows_updated = trx->undo_no - undo_no;

	/* Check if we need to do an insert. */
	if (n_rows_updated == 0) {
		info = pars_info_create();

		pars_info_bind_varchar_literal(
			info, "name", (byte*) name, name_len);

		pars_info_bind_varchar_literal(
			info, "value", value->utf8, value->len);

		graph = fts_parse_sql_no_dict_lock(
			fts_table, info,
			"BEGIN\n"
			"INSERT INTO %s VALUES(:name, :value);");

		trx->op_info = "inserting FTS config value";

		error = fts_eval_sql(trx, graph);

		que_graph_free(graph);
	}

	row_mysql_unlock_data_dictionary(trx);

	return(error);
}

/********************************************************************
Set the value specific to an FTS index in the config table. */

ulint
fts_config_set_index_value(
/*=======================*/
						/* out: DB_SUCCESS or error
						code */
	trx_t*		trx,			/* transaction */
	dict_index_t*	index,			/* in: index */
	const char*	param,			/* in: get config value for
						this parameter name */
	fts_string_t*	value)			/* out: value read from
						config table */
{
	char*		name;
	ulint		error;
	fts_table_t	fts_table;

	fts_table.suffix = "CONFIG";
	fts_table.type = FTS_COMMON_TABLE;
	fts_table.table_id = index->table->id;
	fts_table.parent = index->table->name;

	/* We are responsible for free'ing name. */
	name = fts_config_create_index_param_name(param, index);

	error = fts_config_set_value(trx, &fts_table, name, value);

	ut_free(name);

	return(error);
}

/********************************************************************
Get an ulint value from the config table. */

ulint
fts_config_get_index_ulint(
/*=======================*/
						/* out: DB_SUCCESS if all OK
						else error code */
	trx_t*		trx,			/* in: transaction */
	dict_index_t*	index,			/* in: FTS index */
	const char*	name,			/* in: param name */
	ulint*		int_value)		/* out: value */
{
	ulint		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.len = FTS_MAX_CONFIG_VALUE_LEN;
	value.utf8 = ut_malloc(value.len + 1);

	error = fts_config_get_index_value(trx, index, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ut_print_timestamp(stderr);

		fprintf(stderr, "  InnoDB: Error: (%lu) reading `%s'\n",
			error, name);
	} else {
		*int_value = strtoul((char*) value.utf8, NULL, 10);
	}

	ut_free(value.utf8);

	return(error);
}

/********************************************************************
Set an ulint value in the config table. */

ulint
fts_config_set_index_ulint(
/*=======================*/
						/* out: DB_SUCCESS if all OK
						else error code */
	trx_t*		trx,			/* in: transaction */
	dict_index_t*	index,			/* in: FTS index */
	const char*	name,			/* in: param name */
	ulint		int_value)		/* in: value */
{
	ulint		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.len = FTS_MAX_CONFIG_VALUE_LEN;
	value.utf8 = ut_malloc(value.len + 1);

	// FIXME: Get rid of snprintf
	ut_a(FTS_MAX_INT_LEN < FTS_MAX_CONFIG_VALUE_LEN);

	value.len = snprintf(
		(char*) value.utf8, FTS_MAX_INT_LEN, "%lu", int_value);

	error = fts_config_set_index_value(trx, index, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ut_print_timestamp(stderr);

		fprintf(stderr, "  InnoDB: Error: (%lu) writing `%s'\n",
			error, name);
	}

	ut_free(value.utf8);

	return(error);
}

/********************************************************************
Get an ulint value from the config table. */

ulint
fts_config_get_ulint(
/*=================*/
						/* out: DB_SUCCESS if all OK
						else error code */
	trx_t*		trx,			/* in: transaction */
	fts_table_t*	fts_table,		/* in: the indexed FTS table */
	const char*	name,			/* in: param name */
	ulint*		int_value)		/* out: value */
{
	ulint		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.len = FTS_MAX_CONFIG_VALUE_LEN;
	value.utf8 = ut_malloc(value.len + 1);

	error = fts_config_get_value(trx, fts_table, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ut_print_timestamp(stderr);

		fprintf(stderr, "  InnoDB: Error: (%lu) reading `%s'\n",
			error, name);
	} else {
		*int_value = strtoul((char*) value.utf8, NULL, 10);
	}

	ut_free(value.utf8);

	return(error);
}

/********************************************************************
Set an ulint value in the config table. */

ulint
fts_config_set_ulint(
/*=================*/
						/* out: DB_SUCCESS if all OK
						else error code */
	trx_t*		trx,			/* in: transaction */
	fts_table_t*	fts_table,		/* in: the indexed FTS table */
	const char*	name,			/* in: param name */
	ulint		int_value)		/* in: value */
{
	ulint		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.len = FTS_MAX_CONFIG_VALUE_LEN;
	value.utf8 = ut_malloc(value.len + 1);

	// FIXME: Get rid of snprintf
	ut_a(FTS_MAX_INT_LEN < FTS_MAX_CONFIG_VALUE_LEN);

	value.len = snprintf(
		(char*) value.utf8, FTS_MAX_INT_LEN, "%lu", int_value);

	error = fts_config_set_value(trx, fts_table, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ut_print_timestamp(stderr);

		fprintf(stderr, "  InnoDB: Error: (%lu) writing `%s'\n",
			error, name);
	}

	ut_free(value.utf8);

	return(error);
}

/********************************************************************
Increment the value in the config table for column name. */

ulint
fts_config_increment_value(
/*=======================*/
						/* out: DB_SUCCESS or error
						code */
	trx_t*		trx,			/* transaction */
	fts_table_t*	fts_table,		/* in: the indexed FTS table */
	const char*	name,			/* in: increment config value
						for this parameter name */
	ulint		delta)			/* in: increment by this much */
{
	ulint		error;
	fts_string_t	value;
	que_t*		graph = NULL;
	ulint		name_len = strlen(name);
	pars_info_t*	info = pars_info_create();

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.len = FTS_MAX_CONFIG_VALUE_LEN;
	value.utf8 = ut_malloc(value.len + 1);

	*value.utf8 = '\0';

	pars_info_bind_varchar_literal(info, "name", (byte*) name, name_len);

	pars_info_bind_function(
		info, "my_func", fts_config_fetch_value, &value);

	fts_table->suffix = "CONFIG";

	graph = fts_parse_sql(
		fts_table, info,
		"DECLARE FUNCTION my_func;\n"
		"DECLARE CURSOR c IS SELECT value FROM %s"
		" WHERE key = :name FOR UPDATE;\n"
		"BEGIN\n"
		""
		"OPEN c;\n"
		"WHILE 1 = 1 LOOP\n"
		"  FETCH c INTO my_func();\n"
		"  IF c % NOTFOUND THEN\n"
		"    EXIT;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE c;");

	trx->op_info = "read  FTS config value";

	error = fts_eval_sql(trx, graph);

	mutex_enter(&dict_sys->mutex);
	que_graph_free(graph);
	mutex_exit(&dict_sys->mutex);

	if (UNIV_UNLIKELY(error == DB_SUCCESS)) {
		ulint		int_value;

		int_value = strtoul((char*) value.utf8, NULL, 10);

		int_value += delta;

		ut_a(FTS_MAX_CONFIG_VALUE_LEN > FTS_MAX_INT_LEN);

		// FIXME: Get rid of snprintf
		value.len = snprintf(
			(char*) value.utf8, FTS_MAX_INT_LEN, "%lu", int_value);

		fts_config_set_value(trx, fts_table, name, &value);
	}

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {

		ut_print_timestamp(stderr);

		fprintf(stderr, "  InnoDB: Error: (%lu) "
			"while incrementing %s.\n", error, name);
	}

	ut_free(value.utf8);

	return(error);
}

/********************************************************************
Increment the per index value in the config table for column name. */

ulint
fts_config_increment_index_value(
/*=============================*/
						/* out: DB_SUCCESS or error
						code */
	trx_t*		trx,			/* transaction */
	dict_index_t*	index,			/* in: FTS index */
	const char*	param,			/* in: increment config value
						for this parameter name */
	ulint		delta)			/* in: increment by this much */
{
	char*		name;
	ulint		error;
	fts_table_t	fts_table;

	fts_table.suffix = "CONFIG";
	fts_table.type = FTS_COMMON_TABLE;
	fts_table.table_id = index->table->id;
	fts_table.parent = index->table->name;

	/* We are responsible for free'ing name. */
	name = fts_config_create_index_param_name(param, index);

	error = fts_config_increment_value(trx, &fts_table, name, delta);

	ut_free(name);

	return(error);
}


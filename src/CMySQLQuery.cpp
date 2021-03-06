#pragma once

#include "CMySQLHandle.h"
#include "CMySQLResult.h"
#include "CMySQLQuery.h"
#include "CMySQLConnection.h"
#include "CLog.h"

#include "misc.h"


CMySQLQuery CMySQLQuery::CreateThreaded(
	string query, CMySQLConnection *connection,
	string cb_name, stack< boost::variant<cell, string> > cb_params)
{
	CMySQLQuery QueryObj;

	QueryObj.Connection = connection;
	QueryObj.Query = boost::move(query);
	QueryObj.Callback.Name = boost::move(cb_name);
	QueryObj.Callback.Params = boost::move(cb_params);


	QueryObj.Execute();


	return QueryObj;

}

CMySQLQuery CMySQLQuery::CreateUnthreaded(string query, CMySQLConnection *connection)
{
	CMySQLQuery QueryObj;

	QueryObj.Connection = connection;
	QueryObj.Query = boost::move(query);


	QueryObj.Execute(true);


	return QueryObj;
}

CMySQLQuery CMySQLQuery::CreateOrm(
	string query, CMySQLConnection *connection,
	string cbname, stack< boost::variant<cell, string> > cbparams,
	COrm *orm_object, unsigned short orm_querytype)
{
	CMySQLQuery QueryObj;

	QueryObj.Connection = connection;
	QueryObj.Query = boost::move(query);
	QueryObj.Callback.Name = boost::move(cbname);
	QueryObj.Callback.Params = boost::move(cbparams);
	QueryObj.Orm.Object = orm_object;
	QueryObj.Orm.Type = orm_querytype;


	QueryObj.Execute();


	return QueryObj;
}


void CMySQLQuery::Execute(bool unthreaded)
{
	MYSQL *mysql_connection = Connection->GetMySQLPointer();
	if (mysql_connection != NULL)
	{
		mysql_thread_init();


		char log_funcname[64];
		if (unthreaded)
			sprintf(log_funcname, "CMySQLQuery::Execute");
		else
			sprintf(log_funcname, "CMySQLQuery::Execute[%s]", Callback.Name.c_str());

		CLog::Get()->LogFunction(LOG_DEBUG, log_funcname, "starting query execution");


		if (mysql_real_query(mysql_connection, Query.c_str(), Query.length()) == 0)
		{
			CLog::Get()->LogFunction(LOG_DEBUG, log_funcname, "query was successful");

			MYSQL_RES *mysql_result = mysql_store_result(mysql_connection); //this has to be here

			//why should we process the result if it won't and can't be used?
			if (unthreaded || Callback.Name.length() > 0)
			{
				if (StoreResult(mysql_connection, mysql_result) == false)
					CLog::Get()->LogFunction(LOG_ERROR, log_funcname, "an error occured while storing the result: (error #%d) \"%s\"", mysql_errno(mysql_connection), mysql_error(mysql_connection));
			}
			else  //no callback was specified
			{
				CLog::Get()->LogFunction(LOG_DEBUG, log_funcname, "no callback specified, skipping result saving");
			}

			if (mysql_result != NULL)
				mysql_free_result(mysql_result);
		}
		else  //mysql_real_query failed
		{
			int error_id = mysql_errno(mysql_connection);
			string error_str(mysql_error(mysql_connection));

			CLog::Get()->LogFunction(LOG_ERROR, log_funcname, "(error #%d) %s", error_id, error_str.c_str());


			if (Connection->GetAutoReconnect() && error_id == 2006)
			{
				CLog::Get()->LogFunction(LOG_WARNING, log_funcname, "lost connection, reconnecting...");

				MYSQL_RES *mysql_result;
				if ((mysql_result = mysql_store_result(mysql_connection)) != NULL)
					mysql_free_result(mysql_result);

				Connection->Disconnect();
				Connection->Connect();
			}

			if (Callback.Name.size() > 0)
			{
				//forward OnQueryError(error_id, error[], callback[], query[], connectionHandle);
				//recycle these structures, change some data

				Orm.Object = NULL;
				Orm.Type = 0;

				while (Callback.Params.size() > 0)
					Callback.Params.pop();


				Callback.Params.push(static_cast<cell>(error_id));
				Callback.Params.push(error_str);
				Callback.Params.push(Callback.Name);
				Callback.Params.push(Query);
				Callback.Params.push(static_cast<cell>(Connection->GetConnectionID()));

				Callback.Name = "OnQueryError";

				CLog::Get()->LogFunction(LOG_DEBUG, log_funcname, "error will be triggered in OnQueryError");
			}
		}
		mysql_thread_end();
	}
	if (!unthreaded)
	{
		Connection->DecreaseQueryCounter();
		Connection->IsInUse = false;
	}
}


bool CMySQLQuery::StoreResult(MYSQL *mysql_connection, MYSQL_RES *mysql_result)
{
	if (mysql_result != NULL)
	{
		MYSQL_FIELD *mysql_field;
		MYSQL_ROW mysql_row;

		CMySQLResult *result_ptr = Result = new CMySQLResult;

		Result->m_WarningCount = mysql_warning_count(mysql_connection);

		const my_ulonglong num_rows = Result->m_Rows = mysql_num_rows(mysql_result);
		const unsigned int num_fields = Result->m_Fields = mysql_num_fields(mysql_result);

		Result->m_FieldNames.reserve(Result->m_Fields + 1);


		size_t row_data_size = 0;
		while (mysql_field = mysql_fetch_field(mysql_result))
		{
			Result->m_FieldNames.push_back(mysql_field->name);
			row_data_size += mysql_field->max_length + 1;
		}


		size_t
			mem_head_size = sizeof(char**)* static_cast<size_t>(num_rows),
			mem_row_size = (sizeof(char*)* (num_fields + 1)) + ((row_data_size)* sizeof(char));
		//+1 because there is another value in memory pointing to somewhere
		//mem_row_size has to be a multiple of 8
		while (mem_row_size % 8 != 0)
			mem_row_size++;

		const size_t mem_size = mem_head_size + static_cast<size_t>(num_rows)* mem_row_size;
		char ***mem_data = result_ptr->m_Data = static_cast<char***>(malloc(mem_size));
		char **mem_offset = reinterpret_cast<char**>(&mem_data[num_rows]);

		for (size_t r = 0; r != num_rows; ++r)
		{
			mysql_row = mysql_fetch_row(mysql_result);
			unsigned long *mysql_lengths = mysql_fetch_lengths(mysql_result);

			//copy mysql result data to our location
			mem_data[r] = mem_offset;
			mem_offset += mem_row_size / sizeof(char**);
			memcpy(mem_data[r], mysql_row, mem_row_size);

			char *mem_row_offset = reinterpret_cast<char*>(mem_data[r] + static_cast<size_t>(num_fields + 1));
			for (size_t f = 0; f != num_fields; ++f)
			{
				//correct the pointers of the copied mysql result data
				if (mem_data[r][f] != NULL) //don't touch NULL values
				{
					if (f != 0)
					{
						mem_row_offset += static_cast<size_t>(mysql_lengths[f - 1]);
						if (mem_data[r][f - 1] != NULL) //if the row value isn't NULL
							mem_row_offset += 1; //add length for '\0' delimiter
					}

					mem_data[r][f] = mem_row_offset;
				}
			}
		}
		return true;
	}
	else if (mysql_field_count(mysql_connection) == 0) //query is non-SELECT query
	{
		Result = new CMySQLResult;
		Result->m_WarningCount = mysql_warning_count(mysql_connection);
		Result->m_AffectedRows = mysql_affected_rows(mysql_connection);
		Result->m_InsertID = mysql_insert_id(mysql_connection);
		return true;
	}
	else //error
	{
		//we clear the callback name and forward it to the callback handler
		//the callback handler free's all memory but doesn't call the callback because there's no callback name
		Callback.Name.clear();
		return false;
	}

}
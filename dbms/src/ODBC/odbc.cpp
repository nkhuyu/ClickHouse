#include <sql.h>
#include <sqlext.h>

#include <stdio.h>
#include <malloc.h>
#include <string.h>

#include <iostream>
#include <sstream>
#include <stdexcept>

#include <Poco/NumberParser.h>
#include <Poco/Base64Encoder.h>
#include <Poco/UTF8Encoding.h>
#include <Poco/UTF16Encoding.h>
#include <Poco/TextConverter.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>


static void mylog(const char * message)
{
	static struct Once
	{
		Once()
		{
			std::string stderr_path = "/tmp/clickhouse-odbc-stderr";
			if (!freopen(stderr_path.c_str(), "a+", stderr))
				throw std::logic_error("Cannot freopen stderr.");
		}
	} once;

	std::cerr << message << "\n";
}


struct StringRef
{
	const char * data = nullptr;
	size_t size = 0;

	StringRef() {}
	StringRef(const char * c_str) { *this = c_str; }
	StringRef & operator= (const char * c_str) { data = c_str; size = strlen(c_str); return *this; }

	std::string toString() const { return {data, size}; }

	bool operator== (const char * rhs) const
	{
		return size == strlen(rhs) && 0 == memcmp(data, rhs, strlen(rhs));
	}

	operator bool() const { return data != nullptr; }
};


/// Парсит строку вида key1=value1;key2=value2... TODO Парсинг значений в фигурных скобках.
static const char * nextKeyValuePair(const char * data, const char * end, StringRef & out_key, StringRef & out_value)
{
	if (data >= end)
		return nullptr;

	const char * key_begin = data;
	const char * key_end = reinterpret_cast<const char *>(memchr(key_begin, '=', end - key_begin));
	if (!key_end)
		return nullptr;

	const char * value_begin = key_end + 1;
	const char * value_end;
	if (value_begin >= end)
		value_end = value_begin;
	else
	{
		value_end = reinterpret_cast<const char *>(memchr(value_begin, ';', end - value_begin));
		if (!value_end)
			value_end = end;
	}

	out_key.data = key_begin;
	out_key.size = key_end - key_begin;

	out_value.data = value_begin;
	out_value.size = value_end - value_begin;

	if (value_end < end && *value_end == ';')
		return value_end + 1;
	return value_end;
}


template <typename PTR, typename LENGTH>
RETCODE fillOutputString(const char * value, size_t size_without_zero,
	PTR out_value, LENGTH out_value_max_length, LENGTH * out_value_length)
{
	if (out_value_length)
		*out_value_length = size_without_zero;

	if (out_value_max_length < 0)
		return SQL_ERROR;

	bool res = SQL_SUCCESS;

	if (out_value)
	{
		if (out_value_max_length >= static_cast<LENGTH>(size_without_zero + 1))
		{
			memcpy(out_value, value, size_without_zero + 1);
		}
		else
		{
			if (out_value_max_length > 0)
			{
				memcpy(out_value, value, out_value_max_length - 1);
				reinterpret_cast<char *>(out_value)[out_value_max_length - 1] = 0;

				std::cerr << (char*)(out_value) << "\n";
			}
			res = SQL_SUCCESS_WITH_INFO;
		}
	}

	return res;
}


struct DiagnosticRecord
{
	SQLINTEGER native_error_code = 0;
	std::string sql_state = "-----";
	std::string message;
};


struct Environment
{
	struct TypeInfo
	{
		std::string sql_type_name;
		size_t display_size;
		bool is_unsigned;
	};

	const std::map<std::string, TypeInfo> types_info =
	{
		{"UInt8", 		{ .sql_type_name = "TINYINT", 	.display_size = 3,			.is_unsigned = true, }},
		{"UInt16", 		{ .sql_type_name = "SMALLINT", 	.display_size = 5,			.is_unsigned = true,  }},
		{"UInt32", 		{ .sql_type_name = "INT", 		.display_size = 11,			.is_unsigned = true,  }},
		{"UInt64", 		{ .sql_type_name = "BIGINT", 	.display_size = 20,			.is_unsigned = true,  }},
		{"Int8",		{ .sql_type_name = "TINYINT", 	.display_size = 3,			.is_unsigned = false,  }},
		{"Int16", 		{ .sql_type_name = "SMALLINT", 	.display_size = 5,			.is_unsigned = false,  }},
		{"Int32", 		{ .sql_type_name = "INT", 		.display_size = 11,			.is_unsigned = false,  }},
		{"Int64", 		{ .sql_type_name = "BIGINT", 	.display_size = 20,			.is_unsigned = false,  }},
		{"Float32", 	{ .sql_type_name = "FLOAT", 	.display_size = 1024,		.is_unsigned = false,  }},
		{"Float64", 	{ .sql_type_name = "DOUBLE", 	.display_size = 1024,		.is_unsigned = false,  }},
		{"String", 		{ .sql_type_name = "TEXT", 		.display_size = 16777216,	.is_unsigned = true,  }},
		{"FixedString", { .sql_type_name = "TEXT", 		.display_size = 256,		.is_unsigned = true,  }},
		{"Date", 		{ .sql_type_name = "DATE", 		.display_size = 20,			.is_unsigned = true,  }},
		{"DateTime", 	{ .sql_type_name = "DATETIME", 	.display_size = 20,			.is_unsigned = true,  }},
		{"Array", 		{ .sql_type_name = "TEXT", 		.display_size = 16777216,	.is_unsigned = true,  }},
	};

/*	Poco::UTF8Encoding utf8;
	Poco::UTF16Encoding utf16;
	Poco::TextConverter converter_utf8_to_utf16 {utf8, utf16};*/

	int odbc_version = SQL_OV_ODBC3;
	DiagnosticRecord diagnostic_record;
};


struct Connection
{
	Connection(Environment & env_) : environment(env_) {}

	Environment & environment;
	std::string host = "localhost";
	uint16_t port = 8123;
	std::string user = "default";
	std::string password;
	std::string database = "default";

	Poco::Net::HTTPClientSession session;

	DiagnosticRecord diagnostic_record;
};


struct Statement
{
	Statement(Connection & conn_) : connection(conn_) {}

	Connection & connection;
	std::string query;
	Poco::Net::HTTPRequest request;
	Poco::Net::HTTPResponse response;
	std::istream * in;

	DiagnosticRecord diagnostic_record;

	struct ColumnInfo
	{
		std::string name;
		std::string type;
	};

	std::vector<ColumnInfo> columns_info;

	void initializeResultSet()
	{
		/// TODO Обработка исключений, отправленных сервером.
		/// TODO Случай отсутствия данных.
		while (true)
		{
			std::string name;
			*in >> name;	/// TODO Поддержка эскейпленных строк.

			std::cerr << "name: " << name << "\n";

			if (!in->good())
				throw std::runtime_error("Incomplete header received.");

			ColumnInfo column;
			column.name = name;
			columns_info.push_back(std::move(column));

			auto c = in->get();
			if (c == '\n')
				break;	/// TODO Более корректный код.
		}

		size_t i = 0;
		size_t size = columns_info.size();
		for (; i < size; ++i)
		{
			std::string type;
			*in >> type;

			std::cerr << "type: " << type << "\n";

			if (!in->good())
				throw std::runtime_error("Incomplete header received.");

			columns_info[i].type = type;

			auto c = in->get();
			if (c == '\n')
				break;
		}

		std::cerr << i << ", " << size << "\n";

		if (i + 1 != size)
			throw std::runtime_error("Number of types doesn't equal to number of columns.");
	}


	std::vector<std::string> current_row;
	size_t row_count = 0;

	bool fetchRow()
	{
		size_t size = columns_info.size();
		if (!size)
			return false;

		if (current_row.empty())
			current_row.resize(size);

		size_t i = 0;
		for (; i < size; ++i)
		{
			std::string value;
			*in >> value;		/// TODO Здесь всё неправильно.

			std::cerr << "value: " << value << "\n";

			if (!in->good())
			{
				if (i == 0)
					return false;
				else
					throw std::runtime_error("Incomplete row received.");
			}

			current_row[i] = std::move(value);

			auto c = in->get();
			if (c == '\n')
				break;
		}

		if (i + 1 != size)
			throw std::runtime_error("Number of values in row doesn't equal to number of columns.");

		++row_count;
		return true;
	}

	static uint64_t getUInt(const std::string s)
	{
		return Poco::NumberParser::parseUnsigned64(s);
	}

	static int64_t getInt(const std::string s)
	{
		return Poco::NumberParser::parse64(s);
	}

	static float getFloat(const std::string s)
	{
		return Poco::NumberParser::parseFloat(s);
	}

	static double getDouble(const std::string s)
	{
		return Poco::NumberParser::parseFloat(s);
	}
};


RETCODE allocEnv(SQLHENV * out_environment)
{
	if (nullptr == out_environment)
		return SQL_INVALID_HANDLE;

	*out_environment = new Environment;

	return SQL_SUCCESS;
}

RETCODE allocConnect(SQLHENV environment, SQLHDBC * out_connection)
{
	if (nullptr == out_connection)
		return SQL_INVALID_HANDLE;

	*out_connection = new Connection(*reinterpret_cast<Environment *>(environment));

	return SQL_SUCCESS;
}

RETCODE allocStmt(SQLHDBC connection, SQLHSTMT * out_statement)
{
	if (nullptr == out_statement || nullptr == connection)
		return SQL_INVALID_HANDLE;

	*out_statement = new Statement(*reinterpret_cast<Connection *>(connection));

	return SQL_SUCCESS;
}

RETCODE freeEnv(SQLHENV environment)
{
	delete reinterpret_cast<Environment *>(environment);
	return SQL_SUCCESS;
}

RETCODE freeConnect(SQLHDBC connection)
{
	delete reinterpret_cast<Connection *>(connection);
	return SQL_SUCCESS;
}

RETCODE freeStmt(SQLHSTMT statement)
{
	delete reinterpret_cast<Statement *>(statement);
	return SQL_SUCCESS;
}


/// Интерфейс библиотеки.
extern "C"
{


RETCODE SQL_API
SQLAllocHandle(SQLSMALLINT handle_type,
               SQLHANDLE input_handle,
               SQLHANDLE * output_handle)
{
	mylog(__FUNCTION__);

	switch (handle_type)
	{
		case SQL_HANDLE_ENV:
			return allocEnv((SQLHENV *)output_handle);
		case SQL_HANDLE_DBC:
			return allocConnect((SQLHENV)input_handle, (SQLHDBC *)output_handle);
		case SQL_HANDLE_STMT:
			return allocStmt((SQLHDBC)input_handle, (SQLHSTMT *)output_handle);
		default:
			return SQL_ERROR;
	}
}

RETCODE SQL_API
SQLAllocEnv(SQLHDBC * output_handle)
{
	mylog(__FUNCTION__);
	return allocEnv(output_handle);
}

RETCODE SQL_API
SQLAllocConnect(SQLHENV input_handle, SQLHDBC * output_handle)
{
	mylog(__FUNCTION__);
	return allocConnect(input_handle, output_handle);
}

RETCODE SQL_API
SQLAllocStmt(SQLHDBC input_handle, SQLHSTMT * output_handle)
{
	mylog(__FUNCTION__);
	return allocStmt(input_handle, output_handle);
}


RETCODE SQL_API
SQLFreeHandle(SQLSMALLINT handleType, SQLHANDLE handle)
{
	mylog(__FUNCTION__);

	switch (handleType)
	{
		case SQL_HANDLE_ENV:
			return freeEnv((SQLHENV)handle);
		case SQL_HANDLE_DBC:
			return freeConnect((SQLHDBC)handle);
		case SQL_HANDLE_STMT:
			return freeStmt((SQLHDBC)handle);
		default:
			return SQL_ERROR;
	}
}


RETCODE SQL_API
SQLFreeEnv(HENV handle)
{
	mylog(__FUNCTION__);
	return freeEnv(handle);
}

RETCODE SQL_API
SQLFreeConnect(HDBC handle)
{
	mylog(__FUNCTION__);
	return freeConnect(handle);
}

RETCODE SQL_API
SQLFreeStmt(HSTMT statement_handle,
			SQLUSMALLINT option)
{
	mylog(__FUNCTION__);

	switch (option)
	{
		case SQL_DROP:
			return freeStmt(statement_handle);

		case SQL_CLOSE:				/// Закрыть курсор, проигнорировать оставшиеся результаты. Если курсора нет, то noop.
		case SQL_UNBIND:
		case SQL_RESET_PARAMS:
			return SQL_SUCCESS;

		default:
			return SQL_ERROR;
	}
}


RETCODE SQL_API
SQLConnect(HDBC connection_handle,
		   SQLCHAR * server_name, SQLSMALLINT server_name_size,
		   SQLCHAR * user, SQLSMALLINT user_size,
		   SQLCHAR * password, SQLSMALLINT password_size)
{
	mylog(__FUNCTION__);

	if (nullptr == connection_handle)
		return SQL_INVALID_HANDLE;

	Connection & connection = *reinterpret_cast<Connection *>(connection_handle);

	if (connection.session.connected())
		return SQL_ERROR;

	if (user)
	{
		if (user_size < 0)
			user_size = strlen(reinterpret_cast<const char *>(user));
		connection.user.assign(reinterpret_cast<const char *>(user), static_cast<size_t>(user_size));
	}

	if (password)
	{
		if (password_size < 0)
			password_size = strlen(reinterpret_cast<const char *>(password));
		connection.password.assign(reinterpret_cast<const char *>(password), static_cast<size_t>(password_size));
	}

	connection.session.setHost(connection.host);
	connection.session.setPort(connection.port);
	connection.session.setKeepAlive(true);

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLDriverConnect(HDBC connection_handle,
				 HWND unused_window,
				 SQLCHAR FAR * connection_str_in,
				 SQLSMALLINT connection_str_in_size,
				 SQLCHAR FAR * connection_str_out,
				 SQLSMALLINT connection_str_out_max_size,
				 SQLSMALLINT FAR * connection_str_out_size,
				 SQLUSMALLINT driver_completion)
{
	mylog(__FUNCTION__);

	if (nullptr == connection_handle)
		return SQL_INVALID_HANDLE;

	Connection & connection = *reinterpret_cast<Connection *>(connection_handle);

	if (connection.session.connected())
		return SQL_ERROR;

	if (nullptr == connection_str_in)
		return SQL_ERROR;

	/// Почему-то при использовании isql, сюда передаётся -3. TODO С чего бы это?
	if (connection_str_in_size < 0)
		connection_str_in_size = strlen(reinterpret_cast<const char *>(connection_str_in));

	/// connection_str_in - строка вида DSN=ClickHouse;UID=default;PWD=password

	const char * data = reinterpret_cast<const char *>(connection_str_in);
	const char * end = reinterpret_cast<const char *>(connection_str_in) + connection_str_in_size;

	StringRef current_key;
	StringRef current_value;

	while ((data = nextKeyValuePair(data, end, current_key, current_value)))
	{
		if (current_key == "UID")
			connection.user = current_value.toString();
		else if (current_key == "PWD")
			connection.password = current_value.toString();
		else if (current_key == "HOST")
			connection.host = current_value.toString();
		else if (current_key == "PORT")
		{
			int int_port = 0;
			if (Poco::NumberParser::tryParse(current_value.toString(), int_port))
				connection.port = int_port;
			else
				return SQL_ERROR;
		}
		else if (current_key == "DATABASE")
			connection.database = current_value.toString();
	}

	connection.session.setHost(connection.host);
	connection.session.setPort(connection.port);
	connection.session.setKeepAlive(true);

	/// TODO Таймаут.
	/// TODO Ловля исключений.

	std::cerr << connection_str_in << "\n";

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLGetInfo(HDBC connection_handle,
		   SQLUSMALLINT info_type,
		   PTR out_info_value, SQLSMALLINT out_info_value_max_length, SQLSMALLINT * out_info_value_length)
{
	mylog(__FUNCTION__);

	std::cerr << "GetInfo with info_type: " << info_type << ", out_info_value_max_length: " << out_info_value_max_length << ", out_info_value: " << (void*)out_info_value << "\n";

	std::string res;

	switch (info_type)
	{
		case SQL_DRIVER_VER:
			res = "1.0";
			break;
		case SQL_DRIVER_ODBC_VER:
			res = "03.80";
			break;
		case SQL_DRIVER_NAME:
			res = "ClickHouse ODBC";
			break;
		case SQL_DBMS_NAME:
			res = "ClickHouse";
			break;
		case SQL_SERVER_NAME:
			res = "ClickHouse";
			break;
		case SQL_DATA_SOURCE_NAME:
			res = "ClickHouse";
			break;

		case SQL_MAX_COLUMNS_IN_SELECT:
		case SQL_MAX_DRIVER_CONNECTIONS:
		case SQL_MAX_CONCURRENT_ACTIVITIES:
		case SQL_MAX_COLUMN_NAME_LEN:
		case SQL_MAX_CURSOR_NAME_LEN:
		case SQL_MAX_SCHEMA_NAME_LEN:
		case SQL_MAX_CATALOG_NAME_LEN:
		case SQL_MAX_TABLE_NAME_LEN:
		case SQL_MAX_COLUMNS_IN_GROUP_BY:
		case SQL_MAX_COLUMNS_IN_INDEX:
		case SQL_MAX_COLUMNS_IN_ORDER_BY:
		case SQL_MAX_COLUMNS_IN_TABLE:
		case SQL_MAX_INDEX_SIZE:
		case SQL_MAX_ROW_SIZE:
		case SQL_MAX_STATEMENT_LEN:
		case SQL_MAX_TABLES_IN_SELECT:
		case SQL_MAX_USER_NAME_LEN:
			res.assign("\0\0\0\0");
			break;

		case SQL_DATA_SOURCE_READ_ONLY: /// TODO Libreoffice
		default:
			std::cerr << "Unsupported info type: " << info_type << "\n";	/// TODO Унификация трассировки.
			return SQL_ERROR;
	}

	return fillOutputString(res.data(), res.size(), out_info_value, out_info_value_max_length, out_info_value_length);
}


RETCODE SQL_API
SQLPrepare(HSTMT statement_handle,
		   SQLCHAR * statement_text, SQLINTEGER statement_text_size)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	if (nullptr == statement_text)
		return SQL_ERROR;

	Statement & statement = *reinterpret_cast<Statement *>(statement_handle);

	if (!statement.query.empty())
		return SQL_ERROR;

	if (statement_text_size < 0)	/// TODO И снова сюда передаётся -3. С чего бы это?
		statement_text_size = strlen(reinterpret_cast<const char *>(statement_text));

	statement.query.assign(reinterpret_cast<const char *>(statement_text), static_cast<size_t>(statement_text_size));

	std::cerr << statement.query << "\n";

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLExecute(HSTMT statement_handle)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	Statement & statement = *reinterpret_cast<Statement *>(statement_handle);

	if (statement.query.empty())
		return SQL_ERROR;

	/// Отправляем запрос на сервер.

	std::ostringstream user_password_base64;
	Poco::Base64Encoder base64_encoder(user_password_base64);
	base64_encoder << statement.connection.user << ":" << statement.connection.password; /// TODO Проверка, что user не содержит символа :.
	base64_encoder.close();

	statement.request.setMethod(Poco::Net::HTTPRequest::HTTP_POST);
	statement.request.setCredentials("Basic", user_password_base64.str());
	statement.request.setURI("/?default_format=TabSeparatedWithNamesAndTypes");	/// TODO Возможность передать настройки.

	statement.connection.session.sendRequest(statement.request) << statement.query;
	statement.in = &statement.connection.session.receiveResponse(statement.response);

	statement.initializeResultSet();

	for (const auto & info : statement.columns_info)
		std::cerr << info.name << ", " << info.type << "\n";

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLExecDirect(HSTMT statement_handle,
			  SQLCHAR * statement_text, SQLINTEGER statement_text_size)
{
	mylog(__FUNCTION__);

	RETCODE ret = SQLPrepare(statement_handle, statement_text, statement_text_size);
	if (ret != SQL_SUCCESS)
		return ret;

	return SQLExecute(statement_handle);
}


RETCODE SQL_API
SQLNumResultCols(HSTMT statement_handle,
				 SQLSMALLINT * column_count)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	if (nullptr == column_count)
		return SQL_ERROR;

	*column_count = reinterpret_cast<Statement *>(statement_handle)->columns_info.size();
	std::cerr << *column_count << "\n";

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLColAttribute(HSTMT statement_handle, SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
	SQLPOINTER out_string_value, SQLSMALLINT out_string_value_max_size, SQLSMALLINT * out_string_value_size,
	SQLLEN * out_num_value)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	Statement & statement = *reinterpret_cast<Statement *>(statement_handle);

	if (column_number < 1 || column_number > statement.columns_info.size())
		return SQL_ERROR;

	size_t column_idx = column_number - 1;

	SQLLEN num_value = 0;
	std::string str_value;

	switch (field_identifier)
	{
		case SQL_DESC_AUTO_UNIQUE_VALUE:
			break;
		case SQL_DESC_BASE_COLUMN_NAME:
			break;
		case SQL_DESC_BASE_TABLE_NAME:
			break;
		case SQL_DESC_CASE_SENSITIVE:
			num_value = SQL_TRUE;
			break;
		case SQL_DESC_CATALOG_NAME:
			break;
		case SQL_DESC_CONCISE_TYPE:
			// TODO
			break;
		case SQL_DESC_COUNT:
			num_value = statement.columns_info.size();
			break;
		case SQL_DESC_DISPLAY_SIZE:
			num_value = 0; //statement.connection.environment.types_info.at(statement.columns_info[column_idx].type).display_size;
			break;
		case SQL_DESC_FIXED_PREC_SCALE:
			break;
		case SQL_DESC_LABEL:
			str_value = statement.columns_info[column_idx].name;
			break;
		case SQL_DESC_LENGTH:
			break;
		case SQL_DESC_LITERAL_PREFIX:
			break;
		case SQL_DESC_LITERAL_SUFFIX:
			break;
		case SQL_DESC_LOCAL_TYPE_NAME:
			break;
		case SQL_DESC_NAME:
			str_value = statement.columns_info[column_idx].name;
			break;
		case SQL_DESC_NULLABLE:
			num_value = SQL_FALSE;
			break;
		case SQL_DESC_NUM_PREC_RADIX:
			break;
		case SQL_DESC_OCTET_LENGTH:
			break;
		case SQL_DESC_PRECISION:
			break;
		case SQL_DESC_SCALE:
			break;
		case SQL_DESC_SCHEMA_NAME:
			break;
		case SQL_DESC_SEARCHABLE:
			break;
		case SQL_DESC_TABLE_NAME:
			break;
		case SQL_DESC_TYPE:
			break;
		case SQL_DESC_TYPE_NAME:
			break;
		case SQL_DESC_UNNAMED:
			num_value = SQL_NAMED;
			break;
		case SQL_DESC_UNSIGNED:
			num_value = statement.connection.environment.types_info.at(statement.columns_info[column_idx].type).is_unsigned;
			break;
		case SQL_DESC_UPDATABLE:
			num_value = SQL_FALSE;
			break;
		default:
			return SQL_ERROR;
	}

	if (out_num_value)
		*out_num_value = num_value;

	return fillOutputString(str_value.data(), str_value.size(), out_string_value, out_string_value_max_size, out_string_value_size);
}


RETCODE SQL_API
SQLFetch(HSTMT statement_handle)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	Statement & statement = *reinterpret_cast<Statement *>(statement_handle);

	bool res = statement.fetchRow();

	return res ? SQL_SUCCESS : SQL_NO_DATA;
}


RETCODE SQL_API
SQLGetData(HSTMT statement_handle,
		   SQLUSMALLINT column_or_param_number, SQLSMALLINT target_type,
		   PTR out_value, SQLLEN out_value_max_size,
		   SQLLEN * out_value_size_or_indicator)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	Statement & statement = *reinterpret_cast<Statement *>(statement_handle);

	if (column_or_param_number < 1 || column_or_param_number > statement.columns_info.size())
		return SQL_ERROR;

	size_t column_idx = column_or_param_number - 1;

	std::cerr << "column: " << column_idx << ", target_type: " << target_type << "\n";

	const std::string & value = statement.current_row[column_idx];

	union
	{
		char bytes[8];
		uint64_t uint_data;
		int64_t int_data;
		float float_data;
		double double_data;
	} num;
	size_t num_size = 0;

	switch (target_type)
	{
		case SQL_ARD_TYPE:
		case SQL_C_DEFAULT:
			return SQL_ERROR;

		case SQL_C_WCHAR:
		case SQL_C_CHAR:
			break;

		case SQL_C_TINYINT:
		case SQL_C_STINYINT:
			num_size = 1;
			num.int_data = Statement::getInt(value);
			break;
		case SQL_C_UTINYINT:
			num_size = 1;
			num.uint_data = Statement::getUInt(value);
			break;

		case SQL_C_SHORT:
		case SQL_C_SSHORT:
			num_size = 2;
			num.int_data = Statement::getInt(value);
			break;
		case SQL_C_USHORT:
			num_size = 2;
			num.uint_data = Statement::getUInt(value);
			break;

		case SQL_C_LONG:
		case SQL_C_SLONG:
			num_size = 4;
			num.int_data = Statement::getInt(value);
			break;
		case SQL_C_ULONG:
			num_size = 4;
			num.uint_data = Statement::getUInt(value);
			break;

		case SQL_C_SBIGINT:
			num_size = 8;
			num.int_data = Statement::getInt(value);
			break;
		case SQL_C_UBIGINT:
			num_size = 8;
			num.uint_data = Statement::getUInt(value);
			break;

		case SQL_C_FLOAT:
			num_size = 4;
			num.float_data = Statement::getFloat(value);
			break;

		case SQL_C_DOUBLE:
			num_size = 8;
			num.double_data = Statement::getDouble(value);
			break;

		default:
			return SQL_ERROR;
	}

	if (num_size)
	{
		if (out_value_max_size < static_cast<SQLLEN>(num_size))
			return SQL_ERROR;

		memcpy(out_value, num.bytes, num_size);

		if (out_value_size_or_indicator)
			*out_value_size_or_indicator = num_size;
	}
	else
	{
		if (target_type == SQL_C_CHAR)
		{
			return fillOutputString(value.data(), value.size(), out_value, out_value_max_size, out_value_size_or_indicator);
		}
		else
		{
			std::string converted;
			//statement.connection.environment.converter_utf8_to_utf16.convert(value.data(), converted);

			converted.resize(value.size() * 2 + 1, '\xFF');
			converted[value.size() * 2] = '\0';
			for (size_t i = 0, size = value.size(); i < size; ++i)
				converted[i * 2] = value[i];

			return fillOutputString(converted.data(), converted.size(), out_value, out_value_max_size, out_value_size_or_indicator);
		}
	}

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLRowCount(HSTMT statement_handle,
			SQLLEN * out_row_count)
{
	mylog(__FUNCTION__);

	if (nullptr == statement_handle)
		return SQL_INVALID_HANDLE;

	Statement & statement = *reinterpret_cast<Statement *>(statement_handle);

	if (out_row_count)
		*out_row_count = statement.row_count;

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLMoreResults(HSTMT hstmt)
{
	mylog(__FUNCTION__);

	return SQL_NO_DATA;
}


RETCODE SQL_API
SQLDisconnect(HDBC connection_handle)
{
	mylog(__FUNCTION__);

	if (nullptr == connection_handle)
		return SQL_INVALID_HANDLE;

	Connection & connection = *reinterpret_cast<Connection *>(connection_handle);

	connection.session.reset();

	return SQL_SUCCESS;
}


RETCODE SQL_API
SQLSetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute,
    SQLPOINTER value, SQLINTEGER value_length)
{
	mylog(__FUNCTION__);

	if (nullptr == environment_handle)
		return SQL_INVALID_HANDLE;

	Environment & environment = *reinterpret_cast<Environment *>(environment_handle);

	std::cerr << "attr: " << attribute << "\n";

	switch (attribute)
	{
		case SQL_ATTR_CONNECTION_POOLING:
		case SQL_ATTR_CP_MATCH:
		case SQL_ATTR_OUTPUT_NTS:
		default:
			return SQL_ERROR;

		case SQL_ATTR_ODBC_VERSION:
			intptr_t int_value = reinterpret_cast<intptr_t>(value);
			if (int_value != SQL_OV_ODBC2 && int_value != SQL_OV_ODBC3)
				return SQL_ERROR;

			environment.odbc_version = int_value;
			std::cerr << "Set ODBC version to " << int_value << "\n";

			return SQL_SUCCESS;
	}
}


RETCODE SQL_API
SQLGetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute,
    SQLPOINTER out_value, SQLINTEGER out_value_max_length, SQLINTEGER * out_value_length)
{
	mylog(__FUNCTION__);

	if (nullptr == environment_handle)
		return SQL_INVALID_HANDLE;

	Environment & environment = *reinterpret_cast<Environment *>(environment_handle);

	std::cerr << "attr: " << attribute << "\n";

	switch (attribute)
	{
		case SQL_ATTR_CONNECTION_POOLING:
		case SQL_ATTR_CP_MATCH:
		case SQL_ATTR_OUTPUT_NTS:
		default:
			return SQL_ERROR;

		case SQL_ATTR_ODBC_VERSION:
			*reinterpret_cast<intptr_t*>(out_value) = environment.odbc_version;
			if (out_value_length)
				*out_value_length = sizeof(SQLUINTEGER);

			return SQL_SUCCESS;
	}

	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetConnectAttr(SQLHDBC connection_handle, SQLINTEGER attribute,
        SQLPOINTER value, SQLINTEGER value_length)
{
	mylog(__FUNCTION__);

	if (nullptr == connection_handle)
		return SQL_INVALID_HANDLE;

	std::cerr << "attr: " << attribute << "\n";

	switch (attribute)
	{
		case SQL_ATTR_ACCESS_MODE:
		case SQL_ATTR_ASYNC_ENABLE:
		case SQL_ATTR_AUTO_IPD:
		case SQL_ATTR_AUTOCOMMIT:
		case SQL_ATTR_CONNECTION_DEAD:
		case SQL_ATTR_CONNECTION_TIMEOUT:
		case SQL_ATTR_CURRENT_CATALOG:
		case SQL_ATTR_LOGIN_TIMEOUT: /// TODO
		case SQL_ATTR_METADATA_ID:
		case SQL_ATTR_ODBC_CURSORS:
		case SQL_ATTR_PACKET_SIZE:
		case SQL_ATTR_QUIET_MODE:
		case SQL_ATTR_TRACE:
		case SQL_ATTR_TRACEFILE:
		case SQL_ATTR_TRANSLATE_LIB:
		case SQL_ATTR_TRANSLATE_OPTION:
		case SQL_ATTR_TXN_ISOLATION:
		default:
			return SQL_ERROR;
	}
}


RETCODE SQL_API
SQLGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE handle,
    SQLSMALLINT record_number,
	SQLCHAR * out_sqlstate,
	SQLINTEGER * out_native_error_code,
	SQLCHAR * out_mesage, SQLSMALLINT out_message_max_size, SQLSMALLINT * out_message_size)
{
	mylog(__FUNCTION__);

	std::cerr << "handle_type: " << handle_type << ", record_number: " << record_number << ", out_message_max_size: " << out_message_max_size << "\n";

	if (nullptr == handle)
		return SQL_INVALID_HANDLE;

	if (record_number <= 0 || out_message_max_size < 0)
		return SQL_ERROR;

	if (record_number > 1)
		return SQL_NO_DATA;

	DiagnosticRecord * diagnostic_record = nullptr;
	switch (handle_type)
	{
		case SQL_HANDLE_ENV:
			diagnostic_record = &reinterpret_cast<Environment *>(handle)->diagnostic_record;
			break;
		case SQL_HANDLE_DBC:
			diagnostic_record = &reinterpret_cast<Connection *>(handle)->diagnostic_record;
			break;
		case SQL_HANDLE_STMT:
			diagnostic_record = &reinterpret_cast<Statement *>(handle)->diagnostic_record;
			break;
		default:
			return SQL_ERROR;
	}

	if (diagnostic_record->native_error_code == 0)
		return SQL_NO_DATA;

	/// Пятибуквенный SQLSTATE и завершающий ноль.
	if (out_sqlstate)
		strncpy(reinterpret_cast<char *>(out_sqlstate), diagnostic_record->sql_state.data(), 6);

	if (out_native_error_code)
		*out_native_error_code = diagnostic_record->native_error_code;

	return fillOutputString(diagnostic_record->message.data(), diagnostic_record->message.size(), out_mesage, out_message_max_size, out_message_size);
}


RETCODE SQL_API
SQLGetDiagField(SQLSMALLINT handle_type, SQLHANDLE handle,
    SQLSMALLINT record_number,
	SQLSMALLINT field_id,
	SQLPOINTER out_mesage, SQLSMALLINT out_message_max_size, SQLSMALLINT * out_message_size)
{
	mylog(__FUNCTION__);

	return SQLGetDiagRec(
		handle_type,
		handle,
		record_number,
		nullptr,
		nullptr,
		reinterpret_cast<SQLCHAR *>(out_mesage),
		out_message_max_size,
		out_message_size);
}


RETCODE SQL_API
SQLTables(HSTMT StatementHandle,
		  SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
		  SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
		  SQLCHAR *TableName, SQLSMALLINT NameLength3,
		  SQLCHAR *TableType, SQLSMALLINT NameLength4)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLBrowseConnect(HDBC connection_handle,
				 SQLCHAR *szConnStrIn,
				 SQLSMALLINT cbConnStrIn,
				 SQLCHAR *szConnStrOut,
				 SQLSMALLINT cbConnStrOutMax,
				 SQLSMALLINT *pcbConnStrOut)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLBindCol(HSTMT StatementHandle,
		   SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
		   PTR TargetValue, SQLLEN BufferLength,
		   SQLLEN *StrLen_or_Ind)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLCancel(HSTMT StatementHandle)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLColumns(HSTMT StatementHandle,
		   SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
		   SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
		   SQLCHAR *TableName, SQLSMALLINT NameLength3,
		   SQLCHAR *ColumnName, SQLSMALLINT NameLength4)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLDataSources(HENV EnvironmentHandle,
			   SQLUSMALLINT Direction, SQLCHAR *ServerName,
			   SQLSMALLINT BufferLength1, SQLSMALLINT *NameLength1,
			   SQLCHAR *Description, SQLSMALLINT BufferLength2,
			   SQLSMALLINT *NameLength2)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLDescribeCol(HSTMT StatementHandle,
			   SQLUSMALLINT ColumnNumber, SQLCHAR *ColumnName,
			   SQLSMALLINT BufferLength, SQLSMALLINT *NameLength,
			   SQLSMALLINT *DataType, SQLULEN *ColumnSize,
			   SQLSMALLINT *DecimalDigits, SQLSMALLINT *Nullable)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetCursorName(HSTMT StatementHandle,
				 SQLCHAR *CursorName, SQLSMALLINT BufferLength,
				 SQLSMALLINT *NameLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


/*
/// Эта функция может быть реализована в driver manager-е.
RETCODE SQL_API
SQLGetFunctions(HDBC ConnectionHandle,
				SQLUSMALLINT FunctionId, SQLUSMALLINT *Supported)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}*/


RETCODE SQL_API
SQLGetTypeInfo(HSTMT StatementHandle,
			   SQLSMALLINT DataType)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLParamData(HSTMT StatementHandle,
			 PTR *Value)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLPutData(HSTMT StatementHandle,
		   PTR Data, SQLLEN StrLen_or_Ind)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetCursorName(HSTMT StatementHandle,
				 SQLCHAR *CursorName, SQLSMALLINT NameLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetParam(HSTMT StatementHandle,
			SQLUSMALLINT ParameterNumber, SQLSMALLINT ValueType,
			SQLSMALLINT ParameterType, SQLULEN LengthPrecision,
			SQLSMALLINT ParameterScale, PTR ParameterValue,
			SQLLEN *StrLen_or_Ind)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSpecialColumns(HSTMT StatementHandle,
				  SQLUSMALLINT IdentifierType, SQLCHAR *CatalogName,
				  SQLSMALLINT NameLength1, SQLCHAR *SchemaName,
				  SQLSMALLINT NameLength2, SQLCHAR *TableName,
				  SQLSMALLINT NameLength3, SQLUSMALLINT Scope,
				  SQLUSMALLINT Nullable)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLStatistics(HSTMT StatementHandle,
			  SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
			  SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
			  SQLCHAR *TableName, SQLSMALLINT NameLength3,
			  SQLUSMALLINT Unique, SQLUSMALLINT Reserved)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLColumnPrivileges(HSTMT hstmt,
					SQLCHAR *szCatalogName,
					SQLSMALLINT cbCatalogName,
					SQLCHAR *szSchemaName,
					SQLSMALLINT cbSchemaName,
					SQLCHAR *szTableName,
					SQLSMALLINT cbTableName,
					SQLCHAR *szColumnName,
					SQLSMALLINT cbColumnName)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLDescribeParam(HSTMT hstmt,
				 SQLUSMALLINT ipar,
				 SQLSMALLINT *pfSqlType,
				 SQLULEN *pcbParamDef,
				 SQLSMALLINT *pibScale,
				 SQLSMALLINT *pfNullable)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLExtendedFetch(HSTMT hstmt,
				 SQLUSMALLINT fFetchType,
				 SQLLEN irow,
#if defined(WITH_UNIXODBC) && (SIZEOF_LONG != 8)
				 SQLROWSETSIZE *pcrow,
#else
				 SQLULEN *pcrow,
#endif /* WITH_UNIXODBC */
				 SQLUSMALLINT *rgfRowStatus)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLForeignKeys(HSTMT hstmt,
			   SQLCHAR *szPkCatalogName,
			   SQLSMALLINT cbPkCatalogName,
			   SQLCHAR *szPkSchemaName,
			   SQLSMALLINT cbPkSchemaName,
			   SQLCHAR *szPkTableName,
			   SQLSMALLINT cbPkTableName,
			   SQLCHAR *szFkCatalogName,
			   SQLSMALLINT cbFkCatalogName,
			   SQLCHAR *szFkSchemaName,
			   SQLSMALLINT cbFkSchemaName,
			   SQLCHAR *szFkTableName,
			   SQLSMALLINT cbFkTableName)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLNativeSql(HDBC hdbc,
			 SQLCHAR *szSqlStrIn,
			 SQLINTEGER cbSqlStrIn,
			 SQLCHAR *szSqlStr,
			 SQLINTEGER cbSqlStrMax,
			 SQLINTEGER *pcbSqlStr)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLNumParams(HSTMT hstmt,
			 SQLSMALLINT *pcpar)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLPrimaryKeys(HSTMT hstmt,
			   SQLCHAR *szCatalogName,
			   SQLSMALLINT cbCatalogName,
			   SQLCHAR *szSchemaName,
			   SQLSMALLINT cbSchemaName,
			   SQLCHAR *szTableName,
			   SQLSMALLINT cbTableName)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLProcedureColumns(HSTMT hstmt,
					SQLCHAR *szCatalogName,
					SQLSMALLINT cbCatalogName,
					SQLCHAR *szSchemaName,
					SQLSMALLINT cbSchemaName,
					SQLCHAR *szProcName,
					SQLSMALLINT cbProcName,
					SQLCHAR *szColumnName,
					SQLSMALLINT cbColumnName)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLProcedures(HSTMT hstmt,
			  SQLCHAR *szCatalogName,
			  SQLSMALLINT cbCatalogName,
			  SQLCHAR *szSchemaName,
			  SQLSMALLINT cbSchemaName,
			  SQLCHAR *szProcName,
			  SQLSMALLINT cbProcName)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetPos(HSTMT hstmt,
		  SQLSETPOSIROW irow,
		  SQLUSMALLINT fOption,
		  SQLUSMALLINT fLock)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLTablePrivileges(HSTMT hstmt,
				   SQLCHAR *szCatalogName,
				   SQLSMALLINT cbCatalogName,
				   SQLCHAR *szSchemaName,
				   SQLSMALLINT cbSchemaName,
				   SQLCHAR *szTableName,
				   SQLSMALLINT cbTableName)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLBindParameter(HSTMT hstmt,
				 SQLUSMALLINT ipar,
				 SQLSMALLINT fParamType,
				 SQLSMALLINT fCType,
				 SQLSMALLINT fSqlType,
				 SQLULEN cbColDef,
				 SQLSMALLINT ibScale,
				 PTR rgbValue,
				 SQLLEN cbValueMax,
				 SQLLEN *pcbValue)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}

/*
RETCODE SQL_API
SQLBulkOperations(
     SQLHSTMT       StatementHandle,
     SQLUSMALLINT   Operation)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}*/


RETCODE SQL_API
SQLCancelHandle(
      SQLSMALLINT  HandleType,
      SQLHANDLE    Handle)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLCloseCursor(
	SQLHSTMT     StatementHandle)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLCompleteAsync(
      SQLSMALLINT HandleType,
      SQLHANDLE   Handle,
      RETCODE *   AsyncRetCodePtr)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLCopyDesc(
     SQLHDESC     SourceDescHandle,
     SQLHDESC     TargetDescHandle)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLEndTran(
     SQLSMALLINT   HandleType,
     SQLHANDLE     Handle,
     SQLSMALLINT   CompletionType)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLError(SQLHENV hDrvEnv, SQLHDBC hDrvDbc, SQLHSTMT hDrvStmt,
    SQLCHAR *szSqlState, SQLINTEGER *pfNativeError, SQLCHAR *szErrorMsg,
    SQLSMALLINT nErrorMsgMax, SQLSMALLINT *pcbErrorMsg)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLFetchScroll(SQLHSTMT hDrvStmt, SQLSMALLINT nOrientation, SQLLEN nOffset)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetConnectAttr(SQLHDBC hDrvDbc, SQLINTEGER Attribute, SQLPOINTER Value,
    SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetConnectOption(SQLHDBC hDrvDbc, UWORD fOption, PTR pvParam)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetDescField(SQLHDESC DescriptorHandle, SQLSMALLINT RecordNumber,
    SQLSMALLINT FieldIdentifier, SQLPOINTER Value, SQLINTEGER BufferLength,
    SQLINTEGER *StringLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetDescRec(SQLHDESC DescriptorHandle, SQLSMALLINT RecordNumber,
    SQLCHAR *Name, SQLSMALLINT BufferLength, SQLSMALLINT *StringLength,
    SQLSMALLINT *Type, SQLSMALLINT *SubType, SQLLEN *Length,
    SQLSMALLINT *Precision, SQLSMALLINT *Scale, SQLSMALLINT *Nullable)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetStmtAttr(SQLHSTMT hDrvStmt, SQLINTEGER Attribute, SQLPOINTER Value,
    SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLGetStmtOption(SQLHSTMT hDrvStmt, UWORD fOption, PTR pvParam)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLParamOptions(SQLHSTMT hDrvStmt, SQLULEN nRow, SQLULEN *pnRow)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetConnectOption(SQLHDBC hDrvDbc, UWORD nOption, SQLULEN vParam)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetDescField(SQLHDESC DescriptorHandle, SQLSMALLINT RecordNumber,
    SQLSMALLINT FieldIdentifier, SQLPOINTER Value, SQLINTEGER BufferLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetDescRec(SQLHDESC hDescriptorHandle, SQLSMALLINT nRecordNumber,
    SQLSMALLINT nType, SQLSMALLINT nSubType, SQLLEN nLength,
    SQLSMALLINT nPrecision, SQLSMALLINT nScale, SQLPOINTER pData,
    SQLLEN *pnStringLength, SQLLEN *pnIndicator)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetScrollOptions(
	SQLHSTMT hDrvStmt, SQLUSMALLINT fConcurrency, SQLLEN crowKeyset,
    SQLUSMALLINT crowRowset)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetStmtAttr(SQLHSTMT hDrvStmt, SQLINTEGER Attribute, SQLPOINTER Value,
    SQLINTEGER StringLength)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLSetStmtOption(SQLHSTMT hDrvStmt, UWORD fOption, SQLULEN vParam)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


RETCODE SQL_API
SQLTransact(SQLHENV hDrvEnv, SQLHDBC hDrvDbc, UWORD nType)
{
	mylog(__FUNCTION__);
	return SQL_ERROR;
}


}
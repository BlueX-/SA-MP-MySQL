#pragma once
#ifndef INC_CCALLBACK_H
#define INC_CCALLBACK_H


#include <list>
#include <stack>
#include <string>
//#include <queue>
#include <tuple>
#include <future>
#include <mutex>
#include <boost/variant.hpp>

using std::list;
using std::stack;
using std::string;
//using std::queue;
using std::tuple;
using std::future;
using std::mutex;

#include "main.h"
#include "CMySQLQuery.h"


class CMySQLHandle;


class CCallback 
{
private:
	static list<tuple<future<CMySQLQuery>, CMySQLHandle*>> m_CallbackQueue;
	static mutex m_QueueMtx;

	static list<AMX *> m_AmxList;

public:

	static void FillCallbackParams(stack<boost::variant<cell, string>> &dest, const char *format, AMX* amx, cell* params, const int ConstParamCount);

	
	static void ProcessCallbacks();
	
	static inline void AddQueryToQueue(future<CMySQLQuery> &&fut, CMySQLHandle *handle)
	{
		std::lock_guard<mutex> LockGuard(m_QueueMtx);
		m_CallbackQueue.push_front(std::make_tuple(std::move(fut), handle));
	}

	static void AddAmx(AMX *amx);
	static void EraseAmx(AMX *amx);

	static void ClearAll();

};


#endif // INC_CCALLBACK_H

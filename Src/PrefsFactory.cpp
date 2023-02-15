// Copyright (c) 2010-2023 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "PrefsFactory.h"

#include <glib.h>

#include <memory>
#include <fstream>
#include <cstring>
#include <iterator>
#include <algorithm>
#include <map>
#include <luna-service2++/error.hpp>

#include "ErrorException.h"
#include "LocalePrefsHandler.h"
#include "Logging.h"
#include "PrefsDb.h"
#include "PrefsHandler.h"
#include "TimePrefsHandler.h"
#include "WallpaperPrefsHandler.h"
#include "BuildInfoHandler.h"
#include "RingtonePrefsHandler.h"

#include "UrlRep.h"
#include "JSONUtils.h"

using namespace pbnjson;

static const char* s_logChannel = "PrefsFactory";

static bool cbSetPreferences(LSHandle* lsHandle, LSMessage* message,
							 void* user_data);
static bool cbGetPreferences(LSHandle* lsHandle, LSMessage* message,
							 void* user_data);
static bool cbGetPreferenceValues(LSHandle* lsHandle, LSMessage* message,
								  void* user_data);
static bool cbSwInfo(LSHandle* lsHandle, LSMessage* message, void* user_data);

/*!
 * \page com_palm_systemservice Service API com.webos.service.systemservice/
 *
 * Public methods:
 * - \ref com_palm_systemservice_set_preferences
 * - \ref com_palm_systemservice_get_preferences
 * - \ref com_palm_systemservice_get_preference_values
 */

static LSMethod s_methods[] = {
	{ "setPreferences", cbSetPreferences },
	{ "getPreferences", cbGetPreferences },
	{ "getPreferenceValues", cbGetPreferenceValues },
	{ 0, 0 }
};

static LSMethod q_methods[] = {
	{ "query", cbSwInfo},
	{ 0,0}
};

PrefsFactory::PrefsFactory()
	: m_serviceHandle(nullptr)
{
	PrefsDb::instance();
}

std::string exec(std::string command)
{
	char buffer[128];
	std::string result = "";

	FILE* pipe = popen(command.c_str(), "r");
	if (!pipe)
	{
	return "";
	}
	while (!feof(pipe)) {

	if (fgets(buffer, 128, pipe) != NULL)
		result += buffer;
	}

	pclose(pipe);
	return result;
}

void PrefsFactory::setServiceHandle(LSHandle* serviceHandle)
{
	m_serviceHandle = serviceHandle;

	LS::Error error;
	if (!LSRegisterCategory(serviceHandle, "/", s_methods, nullptr, nullptr, error))
	{
		qCritical() << "Failed to register methods:" << error.what();
		return;
	}

	if (!LSRegisterCategory(serviceHandle, "/softwareInfo", q_methods, nullptr, nullptr, error))
	{
		qCritical() << "Failed to register methods:" << error.what();
		return;
	}

	// Now we can create all the prefs handlers
	registerPrefHandler(std::make_shared<LocalePrefsHandler>(serviceHandle));
	registerPrefHandler(std::make_shared<TimePrefsHandler>(serviceHandle));
	registerPrefHandler(std::make_shared<WallpaperPrefsHandler>(serviceHandle));
	registerPrefHandler(std::make_shared<BuildInfoHandler>(serviceHandle));
	registerPrefHandler(std::make_shared<RingtonePrefsHandler>(serviceHandle));
}

std::shared_ptr<PrefsHandler> PrefsFactory::getPrefsHandler(const std::string& key) const
{
	auto it = m_handlersMaps.find(key);
	if (it == m_handlersMaps.end())
		return nullptr;

	return (*it).second;
}

void PrefsFactory::registerPrefHandler(const PrefsHandlerPtr& handler)
{
	assert(handler);

	std::list<std::string> keys = handler->keys();
	for (const auto& key : keys)
		m_handlersMaps[key] = handler;
}

void PrefsFactory::postPrefChange(const std::string& keyStr,const std::string& valueStr)
{
	LSSubscriptionIter *iter=NULL;
	LSError lserror;
	LSHandle * lsHandle;

	LSErrorInit(&lserror);

	std::string reply = std::string("{ \"")+keyStr+std::string("\":")+valueStr+std::string("}");

	bool retVal = LSSubscriptionAcquire(m_serviceHandle, keyStr.c_str(), &iter, &lserror);
	if (retVal) {
		lsHandle = m_serviceHandle;
		while (LSSubscriptionHasNext(iter)) {

			LSMessage *message = LSSubscriptionNext(iter);
			if (!LSMessageReply(lsHandle,message,reply.c_str(),&lserror)) {
				LSErrorPrint(&lserror,stderr);
				LSErrorFree(&lserror);
			}
		}

		LSSubscriptionRelease(iter);
	}
	else {
		LSErrorFree(&lserror);
	}


}

void PrefsFactory::postPrefChangeValueIsCompleteString(const std::string& keyStr,const std::string& json_string)
{
	LSSubscriptionIter *iter=NULL;
	LSError lserror;
	LSHandle * lsHandle;

	LSErrorInit(&lserror);
	//std::string reply = std::string("{ \"")+keyStr+std::string("\":")+valueStr+std::string("}");
	const std::string reply = json_string;
	//**DEBUG validate for correct UTF-8 output
	if (!g_utf8_validate (reply.c_str(), -1, NULL))
	{
		qWarning() << "bus reply fails UTF-8 validity check! [" << reply.c_str() << "]";
	}

	bool retVal = LSSubscriptionAcquire(m_serviceHandle, keyStr.c_str(), &iter, &lserror);
	if (retVal) {
		lsHandle = m_serviceHandle;
		while (LSSubscriptionHasNext(iter)) {

			LSMessage *message = LSSubscriptionNext(iter);
			if (!LSMessageReply(lsHandle,message,reply.c_str(),&lserror)) {
				LSErrorPrint(&lserror,stderr);
				LSErrorFree(&lserror);
			}
		}

		LSSubscriptionRelease(iter);

	}
	else  {
		LSErrorFree(&lserror);
	}

}

void PrefsFactory::refreshAllKeys()
{

	//get all the keys from the db
	std::map<std::string,std::string> allPrefs = PrefsDb::instance()->getAllPrefs();

	for (std::map<std::string,std::string>::const_iterator it = allPrefs.begin();
			it != allPrefs.end(); ++it)
	{
		//iterate over all the keys in the database
		std::string key = it->first;
		std::string val = it->second;
		auto handler = PrefsFactory::instance()->getPrefsHandler(key);
		// Inform the handler about the change
		if (handler)
		{
			handler->valueChanged(key, val);
		}

		//post change about it
		postPrefChange(key,val);
	}

}

void PrefsFactory::runConsistencyChecksOnAllHandlers()
{
	//go through all the handlers

	for (PrefsHandlerMap::iterator it = m_handlersMaps.begin();it != m_handlersMaps.end();++it) {
		std::string key = it->first;
		auto handler = it->second;
		if (handler) {
			//run the verifier on this key to make sure the pref is correct
			if (handler->isPrefConsistent() == false) {
				qWarning() << "reports inconsistency with key [" << key.c_str() << "]. Restoring default...";
				handler->restoreToDefault();		//something is wrong with this...try and restore it
				std::string restoreVal = PrefsDb::instance()->getPref(key);
				qWarning() << "key [" << key.c_str() << "] restored to value [" << restoreVal.c_str() << "]";
				PrefsFactory::instance()->postPrefChange(key,restoreVal);
			}
		}
	}
}

/*!
\page com_palm_systemservice
\n
\section com_palm_systemservice_set_preferences setPreferences

\e Public.

com.webos.service.systemservice/setPreferences

Sets preference keys to specified values.

\subsection com_palm_systemservice_set_preferences_syntax Syntax:
\code
{
	"params" : object
}
\endcode

\param params An object containing one or more key-value pairs or other objects.

\subsection com_palm_systemservice_set_preferences_returns Returns:
\code
{
	"returnValue": boolean,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorText Description of the error if call was not succesful.

\subsection com_palm_systemservice_set_preferences_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/setPreferences '{ "params": {"food":"pizza"} }'
\endcode

Example response for a succesful call:
\code
{
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "couldn't parse json"
}
\endcode
*/
static bool cbSetPreferences(LSHandle* lsHandle, LSMessage* message,
							 void* user_data)
{
	bool success = true;
	std::string errorText;
	int savecount=0;
	int errcount=0;
	std::string callerId;

	do {
        auto payload = LSMessageGetPayload(message);

        if (!payload) {
            success = false;
            errorText = std::string("Payload get failed, null payload");
            break;
        }

		JValue root = JDomParser::fromString(payload);
		if (!root.isObject())
		{
			success = false;
			errorText = std::string("invalid payload (should be an object)");
			break;
		}

        auto tmpAppId = LSMessageGetApplicationID(message);
        if (tmpAppId) {
            callerId = tmpAppId;
        } else {
            callerId = "";
        }

		for (JValue::KeyValue pref: root.children()) {
			// Is there a preferences handler for this?
			bool savedPref = false;

			std::string key = pref.first.asString();
			std::string value = pref.second.stringify();

			auto handler = PrefsFactory::instance()->getPrefsHandler(key);
			if (handler) {
				PMLOG_TRACE("found handler for %s", key.c_str());
				if (handler->validate(key, pref.second, callerId)) {
					qDebug("handler validated value for key [%s]",key.c_str());
					savedPref = PrefsDb::instance()->setPref(key, value);
				}
				else {
					qWarning() << "handler DID NOT validate value for key:" << key.c_str();
				}
			}
			else {
				qWarning() << "setPref did NOT find handler for:" << key.c_str();

				//filter out
				savedPref = PrefsDb::instance()->setPref(key, value);
			}
			qDebug("setPref saved? %s",(savedPref ? "true" : "false"));

			if (savedPref) {
				++savecount;

				// successfully set the preference. post a notification about it
				JObject json {{key, pref.second}};

				PrefsFactory::instance()->postPrefChangeValueIsCompleteString(key, json.stringify());

				// Inform the handler about the change
				if (handler)
					handler->valueChanged(key, pref.second);

				success=true;
			}
			else {
				++errcount;
			}
		}

		if (errcount) {
			success=false;
			errorText=std::string("Some settings could not be saved");
		}
	} while (false);

	JObject result {{"returnValue", success}};
	if (!success) {
		result.put("errorText", errorText);
		qWarning() << errorText.c_str();
	}

	LS::Error error;
	(void) LSMessageReply(lsHandle, message, result.stringify().c_str(), error);

	return true;
}

static bool cbSwInfo(LSHandle *lsHandle, LSMessage *message, void *)
{
	pbnjson::JValue response_json;
	LS::Message request(message);
	JObject reply;
	std::string node = "nodejs_versions";
	std::vector<std::string> allver;
	std::map<std::string, std::string> versions;
	versions.insert(std::pair<std::string, std::string>(node, ""));
	do
	{
		LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_1(PROPERTY(parameters, array))));
		if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
			return true;
		JValue root = parser.get();
		JValue parameters = root["parameters"];
		for (JValue parameters : parameters.items())
		{
			auto query = versions.find(parameters.asString());
			if (query == versions.end())
			{
				qWarning() << "reached invalid parameter";
				response_json =
					pbnjson::JObject{{"returnValue", false}, {"errorText", "Invalid parameter: " + parameters.stringify()}};
				request.respond(response_json.stringify().c_str());
				break;
			}
		}
		std::string nodejsversion = exec("node -v");
		if (nodejsversion.empty())
		{
			reply = JObject{{"returnValue", false}, {"errorText", "Failed to get nodejs version"}};
			break;
		}
		else
		{
			nodejsversion.erase(std::remove(nodejsversion.begin(), nodejsversion.end(), '\n'), nodejsversion.end());
			allver.push_back(nodejsversion);
		}
		std::string nodejs6version = exec("node6 -v");
		if (nodejs6version.empty())
		{
			nodejs6version = "";
		}
		else
		{
			nodejs6version.erase(std::remove(nodejs6version.begin(), nodejs6version.end(), '\n'), nodejs6version.end());
			allver.push_back(nodejs6version);
		}
		pbnjson::JValue version_array = pbnjson::JArray();
		for (std::string name : allver)
		{
			version_array << pbnjson::JValue(name);
		}
		reply.put("nodejs_versions", version_array);
		reply.put("returnValue", true);
	} while (false);

	LS::Error error;
	if (!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
	{
		qWarning() << "Failed to send LS reply: " << error.what();
	}

	return true;
}

static bool quotesRequired(const std::string& value)
{
	bool isQuotes(true);

	const char* val_str = value.c_str();
	char* pEnd;
	double result = strtod(val_str, &pEnd);
	if (!(abs(result - 0.0) < 0.1)) {
		isQuotes = false;			// maybe number, will continue check
		while (*pEnd != '\0') {
			if (!isspace(*pEnd)) {		// if we have not spaces symbols after number => we have string
				isQuotes = true;
				break;
			}
			pEnd++;
		}
	}
	else if (val_str != pEnd) {	// check if value == 0.0
		isQuotes = false;		// number detected
	}

	if (isQuotes) {
		switch(value[0]) {
		case '"':
			isQuotes = false;
			break;
		case 'f':
			if ("false" == value) {
				isQuotes = false;
			}
			break;
		case 't':
			if ("true" == value) {
				isQuotes = false;
			}
			break;
		case 'n':
			if ("null" == value) {
				isQuotes = false;
			}
			break;
		}
	}

	return isQuotes;
}

/*!
\page com_palm_systemservice
\n
\section com_palm_systemservice_get_preferences getPreferences

\e Public.

com.webos.service.systemservice/getPreferences

Retrieves the values for keys specified in a passed array. If subscribe is set to true, then getPreferences sends an update if the key values change.

\subsection com_palm_systemservice_get_preferences_syntax Syntax:
\code
{
	"subscribe" : boolean,
	"keys"      : string array
}
\endcode

\param subscribe If true, getPreferences sends an update whenever the value of one of the keys changes.
\param keys An array of key names. Required.

\subsection com_palm_systemservice_get_preferences_returns Returns:
\code
{
   "[no name]"   : object,
   "returnValue" : boolean
}
\endcode

\param "[no name]" Key-value pairs containing the values for the requested preferences. If the requested preferences key or keys do not exist, the object is empty.
\param returnValue Indicates if the call was succesful.

\subsection com_palm_systemservice_get_preferences_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/getPreferences '{"subscribe": false, "keys":["wallpaper", "ringtone"]}'
\endcode

Example response for a succesful call:
\code
{
	"ringtone": {
		"fullPath": "\/usr\/lib\/luna\/customization\/copy_binaries\/media\/internal\/ringtones\/Pre.mp3",
		"name": "Prē"
	},
	"wallpaper": {
		"wallpaperName": "flowers.png",
		"wallpaperFile": "\/usr\/lib\/luna\/system\/luna-systemui\/images\/flowers.png",
		"wallpaperThumbFile": ""
	},
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"subscribed": false,
	"errorCode": "no keys specified"
}
\endcode
*/
static bool cbGetPreferences(LSHandle* lsHandle, LSMessage* message, void*)
{
	// {"subscribe": boolean, "keys": array of strings}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(subscribe, boolean),
															  R"("keys":{"type": "array", "minItems": 1, "items": {"type":"string"}})")
													  REQUIRED_1(keys)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();

	bool subscription = false;

	JValue label = root["keys"];
	std::list<std::string> keyList;
	for (const JValue &key: label.items()) {
		std::string key_str = key.asString();
		auto handler = PrefsFactory::instance()->getPrefsHandler(key_str);
		if (handler) {
			//run the verifier on this key to make sure the pref is correct
			if (handler->isPrefConsistent() == false) {
				handler->restoreToDefault();		//something is wrong with this...try and restore it
				std::string restoreVal = PrefsDb::instance()->getPref(key_str);
				PrefsFactory::instance()->postPrefChange(key_str,restoreVal);
			}
		}
		keyList.push_back(key_str);
	}

	std::map<std::string, std::string> resultMap = PrefsDb::instance()->getPrefs(keyList);

	if (LSMessageIsSubscription(message)) {

		LS::Error tmp_error;
		for (std::list<std::string>::const_iterator it = keyList.begin();
			 it != keyList.end(); ++it) {
			(void) LSSubscriptionAdd(lsHandle, (*it).c_str(),
									 message, tmp_error);
		}
		subscription = true;
	}
	else
		subscription = false;

	JObject reply;
	std::string errorCode;
	for (std::map<std::string, std::string>::const_iterator it = resultMap.begin();
		 it != resultMap.end(); ++it) {
		JValue value = JDomParser::fromString((*it).second);
		if (value.isValid()) {
			qDebug("resultMap: [%s] -> [---, length %zu]",(*it).first.c_str(),(*it).second.size());
			reply.put((*it).first, value);
		}
		else {
			// not JSON, try to work with json primitive (ex. string, number)
			std::string primitive;
			if (quotesRequired((*it).second)) {
				primitive = "[\"" + (*it).second + "\"]";
			}
			else {
				primitive = "[" + (*it).second + "]";
			}

			JValue arr = JDomParser::fromString(primitive);
			if (arr.isValid()) {
				reply.put((*it).first, arr[0]);
			}
			else {
				errorCode = arr.errorString();
				break;
			}
		}
	}

	if (errorCode.empty()) {
		reply.put("subscribed", subscription);
		reply.put("returnValue", true);
	}
	else {
		reply = JObject {{"returnValue", false},
						 {"subscribed", false},
						 {"errorCode", errorCode}};

		qWarning() << errorCode.c_str();
	}

	LS::Error error;
	(void) LSMessageReply(lsHandle, message, reply.stringify().c_str(), error);

	return true;
}

/*!
\page com_palm_systemservice
\n
\section com_palm_systemservice_get_preference_values getPreferenceValues

\e Public.

com.webos.service.systemservice/getPreferenceValues

Retrieve the list of valid values for the specified key. If the key is of a type that takes one of a discrete set of valid values, getPreferenceValues returns that set. Otherwise, getPreferenceValues returns nothing for the key.

\subsection com_palm_systemservice_get_preference_values_syntax Syntax:
\code
{
	"key": string
}
\endcode

\param key Key name.

\subsection com_palm_systemservice_get_preference_value_returns Returns:
\code
{
	"[no name]"   : object,
	"returnValue" : boolean
}
\endcode

\param "[no name]" The key and the valid values.
\param returnValue Indicates if the call was succesful.

\subsection com_palm_systemservice_get_preference_value_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/getPreferenceValues '{"key": "wallpaper" }'
\endcode

Example responses for succesful calls:
\code
{
	"wallpaper": [
		{
			"wallpaperName": "flowers.png",
			"wallpaperFile": "\/media\/internal\/.wallpapers\/flowers.png",
			"wallpaperThumbFile": "\/media\/internal\/.wallpapers\/thumbs\/flowers.png"
		}
	],
	"returnValue": true
}
\endcode
\code
{
	"timeFormat": [
		"HH12",
		"HH24"
	],
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false
}
\endcode
*/
static bool cbGetPreferenceValues(LSHandle* lsHandle, LSMessage* message, void* user_data)
{
	// {"key": string}
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(PROPS_1(PROPERTY(key, string))
													  REQUIRED_1(key)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();
	JValue reply;
	try
	{
		std::string key = root["key"].asString();
		auto handler = PrefsFactory::instance()->getPrefsHandler(key);
		if (!handler)
		{
			throw ErrorException(PrefsFactory::ErrorPrefDoesntExist, "Can't find handler for key: "+ key);
		}

		if ("timeZone" == key) {
			std::string countryCode = root["countryCode"].asString();
			std::string locale = root["locale"].asString();
			reply = std::static_pointer_cast<TimePrefsHandler>(handler)->timeZoneListAsJson(countryCode, locale);
		} else {
			reply = handler->valuesForKey(key);
		}

		if (!reply.isValid())
		{
			throw ErrorException(PrefsFactory::ErrorValuesDontExist, "Handler doesn't have values for key: " + key);
		}

		reply.put("returnValue", true);
	}
	catch (const ErrorException& e)
	{
		reply = JObject {{"returnValue", false},
						 {"errorText", e.errorText()},
						 {"errorCode", e.erroCode()}};
	}

	LS::Error error;
	if (!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
	{
		qWarning() << error.what();
	}

	return true;
}

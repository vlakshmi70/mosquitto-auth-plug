/*
 * Copyright (c) 2013 Jan-Piet Mens <jp@mens.de> wendal
 * <wendal1985()gmai.com> All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer. 2. Redistributions
 * in binary form must reproduce the above copyright notice, this list of
 * conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution. 3. Neither the name of mosquitto
 * nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written
 * permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef BE_JWT
#include "backends.h"
#include "be-jwt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash.h"
#include "log.h"
#include "envs.h"
#include <curl/curl.h>
#include <jansson.h>
#include <jwt.h>
#include <uthash.h>
#include <time.h>


/*
 * user authentication info data types 
 */
#define MAX_USERNAME_LEN 60
#define MAX_TOPIC_LEN 60
#define MAX_ROLE_LEN 50
typedef struct User_Info_t {
        char username[MAX_USERNAME_LEN];
        char topic[MAX_TOPIC_LEN];
	char role[MAX_ROLE_LEN];
        long exp_time;
} user_info_t;

typedef struct User_List_t {
        struct User_List_t *next;
        user_info_t *user;
} user_list_t;

/* 
 * global variable for user info list
 */
user_list_t *head;

#define ERR_USER_NOT_FOUND -1
#define ERR_MEM_FAILURE -2
#define SUCCESS_USER 1
void init_user_list();
int add_user_info(user_info_t *user);
user_info_t *get_user_info(char *username);
int del_user_info(char *username);
void del_user_list();
void print_user(user_info_t *user);
void print_user_list();

/*
 * User Role to access mapping data type
 */
typedef struct RoleAccessMap_t {
	char role[MAX_ROLE_LEN];
	int access;
} role_access_t;

/* 
 * global variable for role to access mapping
 */
#define MAX_ROLES 25
role_access_t role_access_map[MAX_ROLES];

void init_role_access_map(const char *required_env);
int get_access_from_role(char *role);
#define MAX_BITS 4
int check_access(const char *username, const char *reqTopic, int reqAccess, char *allowedTopic, int allowedAccess);
int user_role_check (int request, int allowed);


static int http_post(void *handle, char *uri, const char *username, const char *password, const char *topic, int acc, int method);

static int get_string_envs(CURL *curl, const char *required_env, char *querystring)
{
	char *data = NULL;
	char *escaped_key = NULL;
	char *escaped_val = NULL;
	char *env_string = NULL;

	char *params_key[MAXPARAMSNUM];
	char *env_names[MAXPARAMSNUM];
	char *env_value[MAXPARAMSNUM];
	int i, num = 0;

	//_log(LOG_DEBUG, "sys_envs=%s", sys_envs);

	env_string = (char *)malloc(strlen(required_env) + 20);
	if (env_string == NULL)
	{
		_fatal("ENOMEM");
		return (-1);
	}
	sprintf(env_string, "%s", required_env);

	//_log(LOG_DEBUG, "env_string=%s", env_string);

	num = get_sys_envs(env_string, ",", "=", params_key, env_names, env_value);
	//sprintf(querystring, "");
	for (i = 0; i < num; i++)
	{
		escaped_key = curl_easy_escape(curl, params_key[i], 0);
		escaped_val = curl_easy_escape(curl, env_names[i], 0);

		//_log(LOG_DEBUG, "key=%s", params_key[i]);
		//_log(LOG_DEBUG, "escaped_key=%s", escaped_key);
		//_log(LOG_DEBUG, "escaped_val=%s", escaped_envvalue);

		data = (char *)malloc(strlen(escaped_key) + strlen(escaped_val) + 1);
		if (data == NULL)
		{
			_fatal("ENOMEM");
			return (-1);
		}
		sprintf(data, "%s=%s&", escaped_key, escaped_val);
		if (i == 0)
		{
			sprintf(querystring, "%s", data);
		}
		else
		{
			strcat(querystring, data);
		}
	}

	if (data)
		free(data);
	if (escaped_key)
		free(escaped_key);
	if (escaped_val)
		free(escaped_val);
	free(env_string);
	return (num);
}


void *be_jwt_init()
{
	struct jwt_backend *conf;
	char *ip;
	char *getuser_uri;
	char *superuser_uri;
	char *aclcheck_uri;

	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
	{
		_fatal("init curl fail");
		return (NULL);
	}
	if ((ip = p_stab("http_ip")) == NULL)
	{
		_fatal("Mandatory parameter `http_ip' missing");
		return (NULL);
	}
	if ((getuser_uri = p_stab("http_getuser_uri")) == NULL)
	{
		_fatal("Mandatory parameter `http_getuser_uri' missing");
		return (NULL);
	}
	if ((superuser_uri = p_stab("http_superuser_uri")) == NULL)
	{
		_fatal("Mandatory parameter `http_superuser_uri' missing");
		return (NULL);
	}
	if ((aclcheck_uri = p_stab("http_aclcheck_uri")) == NULL)
	{
		_fatal("Mandatory parameter `http_aclcheck_uri' missing");
		return (NULL);
	}
	conf = (struct jwt_backend *)malloc(sizeof(struct jwt_backend));
	conf->ip = ip;
	conf->hostname = NULL;
	conf->hostheader = NULL;
	conf->port = p_stab("http_port") == NULL ? 80 : atoi(p_stab("http_port"));
	if (p_stab("http_hostname") != NULL)
	{
		conf->hostheader = (char *)malloc(128);
		conf->hostname = p_stab("http_hostname");
		sprintf(conf->hostheader, "Host: %s", p_stab("http_hostname"));
	}
	conf->getuser_uri = getuser_uri;
	conf->superuser_uri = superuser_uri;
	conf->aclcheck_uri = aclcheck_uri;

	conf->getuser_envs = p_stab("http_getuser_params");
	conf->superuser_envs = p_stab("http_superuser_params");
	conf->aclcheck_envs = p_stab("http_aclcheck_params");

	if (p_stab("http_with_tls") != NULL)
	{
		conf->with_tls = p_stab("http_with_tls");
	}
	else
	{
		conf->with_tls = "false";
	}

	_log(LOG_DEBUG, "with_tls=%s", conf->with_tls);
	_log(LOG_DEBUG, "getuser_uri=%s", getuser_uri);
	_log(LOG_DEBUG, "superuser_uri=%s", superuser_uri);
	_log(LOG_DEBUG, "aclcheck_uri=%s", aclcheck_uri);
	_log(LOG_DEBUG, "getuser_params=%s", conf->getuser_envs);
	_log(LOG_DEBUG, "superuser_params=%s", conf->superuser_envs);
	_log(LOG_DEBUG, "aclcheck_params=%s", conf->aclcheck_envs);

	init_user_list();
	init_role_access_map(conf->aclcheck_envs);
	return (conf);
};

void be_jwt_destroy(void *handle)
{
	struct jwt_backend *conf = (struct jwt_backend *)handle;

	if (conf)
	{
		if (conf->hostname)
			free(conf->hostname);
		if (conf->hostheader)
			free(conf->hostheader);
		curl_global_cleanup();
		free(conf);
	}
	del_user_list();
};

int be_jwt_getuser(void *handle, const char *username, const char *pass, char **phash, const char *clientid)
{
	struct jwt_backend *conf = (struct jwt_backend *)handle;
	int re;
	if (username == NULL)
	{
		return BACKEND_ERROR;
	}
	printf("client-id = %s\n", clientid);
	re = http_post(handle, conf->getuser_uri, username, pass, NULL, -1, METHOD_GETUSER);
	return re;
};

int be_jwt_superuser(void *handle, const char *username)
{
	//struct jwt_backend *conf = (struct jwt_backend *)handle;
	//int re;

	//re = http_post(handle, conf->superuser_uri, username, NULL, NULL, -1, METHOD_SUPERUSER);
	/*
	 * superuser not supported
	 */
	return BACKEND_ERROR;
};

int be_jwt_aclcheck(void *handle, const char *clientid, const char *username, const char *topic, int acc)
{
	user_info_t *pUser;
	int allowedAccess=0;
	int result=BACKEND_ERROR;

	_log(LOG_DEBUG, "aclcheck: username = %s, requested topic = %s, requested access = %x", username, topic, acc);
	pUser = get_user_info((char *)username);
	if (pUser) {
		allowedAccess = get_access_from_role(pUser->role);
		result = check_access(username, topic, acc, pUser->topic, allowedAccess);
		if (result != BACKEND_ALLOW) {
			del_user_info((char *)username);
		}
		return result;
	}
	return result;
};

/*
 * check requested topic/access against allowed topic/access
 * returns BACKEND_ALLOW or DENY
 */
int check_access(const char *username, const char *reqTopic, int reqAccess, char *allowedTopic, int allowedAccess)
{

	int result=BACKEND_ALLOW;

	_log(LOG_DEBUG, "username = %s, requested topic = %s, requested access = %x, allowed topic = %s, allowed access = %x", username, reqTopic, reqAccess, allowedTopic, allowedAccess);
	if (allowedAccess <= 0) {
		_log(LOG_NOTICE, "<%s>: Access Denied, request-access=%d, allowedAccess=%d", username, reqAccess, allowedAccess);
		result = BACKEND_ERROR;
	}
	else {
		if (!strncmp("#", allowedTopic, 1)) {
			result = user_role_check(reqAccess, allowedAccess);
		}
		else {
			if (reqTopic) {
				if (!strncmp(allowedTopic, reqTopic, strlen(allowedTopic))) {
					result = user_role_check(reqAccess, allowedAccess);
				}
				else {
					_log(LOG_NOTICE, "<%s>: Access Denied, Topic Not allowed", username);
					result = BACKEND_ERROR;
				}
			}
			else {
				_log(LOG_NOTICE, "<%s>: Access deferred, Topic not specified", username);
				result = BACKEND_DEFER;	
			}
		}
	}
	return result;
}

/*
 * check if user role is allowed access
 */
int user_role_check (int request, int allowed) 
{
	int ok = BACKEND_ERROR;
	switch (request) {
		case 1:
			if (allowed >= request)
				ok=BACKEND_ALLOW;
			break;
		case 2:
			if (allowed & request)
				ok=BACKEND_ALLOW;
			break;
		case 3:
			if (allowed & request)
				ok=BACKEND_ALLOW;
			break;
		case 4:
			ok=BACKEND_ALLOW;
			break;
	}
	if (ok==BACKEND_ALLOW)
		_log(LOG_DEBUG, "user_role_check: request-access=%d, allowed-access=%d, Access Granted", request, allowed);
	else if (ok == BACKEND_ERROR)
		_log(LOG_DEBUG, "user_role_check: request-access=%d, allowed-access=%d, Access Denied", request, allowed);
	return ok;
}
/*
 * role access map - manipulation - init, get
 */
void init_role_access_map(const char *required_env)
{
	char *env_string = NULL;

	char *params_key[MAXPARAMSNUM];
	char *env_names[MAXPARAMSNUM];
	char *env_value[MAXPARAMSNUM];
	int i, num = 0;

	env_string = (char *)malloc(strlen(required_env) + 20);
	if (env_string == NULL)
	{
		_fatal("ENOMEM");
		return;
	}
	sprintf(env_string, "%s", required_env);

	num = get_sys_envs(env_string, ",", "=", params_key, env_names, env_value);
	for (i = 0; i < num; i++)
	{
		_log(LOG_DEBUG, "role=%s, value=%d", params_key[i], atoi(env_names[i]));
		sprintf(role_access_map[i].role,"%s", params_key[i]);
		role_access_map[i].access = atoi(env_names[i]);

	}
}
int get_access_from_role(char *role) 
{
	for (int i=0;i<MAX_ROLES;i++) {
		if (!strcmp(role, role_access_map[i].role)) {
			return role_access_map[i].access;
		}
	}
	return -1;
}

/*
 * user info manipulation - init, add, del, print
 */
void init_user_list()
{
	head = NULL;
}
int add_user_info(user_info_t *user)
{
        user_list_t *p;
	user_info_t *pUser;

        if (user) {
		_log(LOG_DEBUG, "add_user_info called with <%s>:Allowed topic=%s, Allowed Role=%s, exp_time=%ld\n", user->username, user->topic, user->role, user->exp_time);
		pUser = get_user_info(user->username);
		if (pUser) {
                	_log(LOG_DEBUG, "User <%s> already exists, updating info ...\n", user->username);
                	sprintf(pUser->username, "%s", user->username);
                	sprintf(pUser->topic, "%s", user->topic);
                	sprintf(pUser->role, "%s", user->role);
                	pUser->exp_time = user->exp_time;
		}
		else {
			_log(LOG_DEBUG, "Adding user: <%s>:Allowed topic=%s, Allowed Role=%s, exp_time=%ld\n", user->username, user->topic, user->role, user->exp_time);
                	p = (user_list_t *) malloc(sizeof(user_list_t));
                	if (!p) {
                        	_log(LOG_DEBUG, "%s: add user failure, malloc error\n", user->username);
                        	return ERR_MEM_FAILURE;
                	}
                	p->user=(user_info_t *)malloc(sizeof(user_info_t));
                	if (!p->user) {
                        	_log(LOG_DEBUG, "%s: add user failure, malloc error\n", user->username);
                        	return ERR_MEM_FAILURE;
                	}

			memset(p->user, 0, sizeof(user_info_t));
                	sprintf(p->user->username, "%s", user->username);
                	sprintf(p->user->topic, "%s", user->topic);
                	sprintf(p->user->role, "%s", user->role);
                	p->user->exp_time = user->exp_time;
                	p->next = NULL;
                	if (!head) {
                        	head = p;
                	}
                	else {
				user_list_t *curr, *prev;
				for (curr=head;curr;prev=curr, curr=curr->next);
                        	prev->next = p;
                	}
		}
        }
        return SUCCESS_USER;
}
user_info_t *get_user_info(char *username)
{
        user_list_t *p;
        _log(LOG_DEBUG, "Searching user <%s> ...\n", username);
        for (p=head; p; p=p->next) {
                if (strncmp(p->user->username, username, strlen(username))) {
                        _log(LOG_DEBUG, "User <%s> not matching, checking next ...\n", username);
                        continue;
                }
                else {
                        _log(LOG_DEBUG, "User <%s> found\n", username);
                        return p->user;
                }

        }
        _log(LOG_DEBUG, "User <%s> not found...\n", username);
        return NULL;
}
int del_user_info(char *username)
{
        user_list_t *p, *prev;
	_log(LOG_DEBUG, "Deleting user <%s> ...", username);
        for (p=head; p; prev=p, p=p->next) {
                if (strncmp(p->user->username, username, strlen(username))) {
                        _log(LOG_DEBUG, "User <%s> not matching, checking next ...\n", username);
                        continue;
                }
                else {
                        _log(LOG_DEBUG, "User <%s> found, deleting ...\n", username);
                        if (p == head) {
                                if (p->next) {
                                        head = p->next;
                                }
                                else {
                                        init_user_list();
                                }
                        }
                        else {
                                prev->next=p->next;
                        }
                        free(p->user);
                        free(p);
                        return SUCCESS_USER;
                }
        }
        return ERR_USER_NOT_FOUND;
}
void del_user_list()
{
	user_list_t *i;
	_log(LOG_DEBUG, "Deleting all users ...");
	for (i=head;i;) {
		head = i->next;
                free(i->user);
                free(i);
                i=head;
	}
}
void print_user(user_info_t *p)
{
	_log(LOG_DEBUG, "%s:%s:%s:%ld\n", p->username, p->topic, p->role, p->exp_time);
}
void print_user_list()
{
        user_list_t *i;
        _log(LOG_DEBUG, "Printing users ...\n");
        for (i=head;i;i=i->next) {
                print_user(i->user);
        }
}
/*
 * get the expiry time from JWT
 * its assumed a user will have one role defined in keycloak
 */
int get_jwt_exp_time(jwt_t *jwtToken, long *time)
{
	*time = jwt_get_grant_int(jwtToken, "exp");
	_log(LOG_DEBUG, "JWT: exp_time = %ld\n", *time);
	return 1;
}
/*
 * return the topic from JWT
 */
int get_jwt_topic(jwt_t *jwtToken, char *topic) 
{
	const char *tmp;
	tmp = jwt_get_grant(jwtToken, "topic");
	if (!tmp)
		return -1;
	sprintf(topic, "%s", tmp);
	return 1;
}
/*
 * return the first role from JWT
 * its assumed a user will have one role defined in keycloak
 */
int get_jwt_role(jwt_t *jwtToken, char *role) 
{
	json_t *realmRoot, *rolesArray, *roleJson;
	char *roleStr="";
	json_error_t error;
	char *realmRoleJson;

	realmRoleJson = jwt_get_grants_json(jwtToken, "realm_access");
	if (!realmRoleJson) {
		return -1;
	}
	_log(LOG_DEBUG, "getUserRole:realmRoleJson = <%s>\n", realmRoleJson);

	realmRoot = json_loads((char *)realmRoleJson, 0, &error);
	if (!realmRoot)
		return -1;
	rolesArray = json_object_get(realmRoot, "roles");
	if (!rolesArray) 
		return -1;
	roleJson = json_array_get(rolesArray, 0);
	if (!roleJson)
		return -1;
	roleStr = (char *) json_string_value(roleJson);
	if (!role)
		return -1;
	sprintf(role, "%s", roleStr);

	json_decref(rolesArray);
	json_decref(roleJson);
	json_decref(realmRoot);

	return 1;
}
jwt_t *get_jwt_token(char *respData) 
{
	json_t *respRoot=NULL;
	json_t *accessToken=NULL;
	json_t *errResp=NULL;
	json_error_t error;
	const char *token=NULL;
	
	respRoot = json_loads((char *)respData, 0, &error);
	if (!respRoot) {
		_log(LOG_NOTICE, "get_jwt_token: JSON processing error on line <%d>:<%s>", error.line, error.text);
		return NULL;
	}
	accessToken = json_object_get(respRoot, "access_token");
	if (!json_is_string(accessToken)) {
		errResp = json_object_get(respRoot, "error");
		if (json_is_string(errResp)) 
			_log(LOG_NOTICE, "get_jwt_token: JSON processing, no access_token, error <%s>", json_string_value(errResp));
		return NULL;
	}	
	token = json_string_value(accessToken);
	if (!token) {
		_log(LOG_NOTICE, "get_jwt_token: JSON processing error, no token present");
		return NULL;
	}
	jwt_t *jwtToken=NULL;
	_log(LOG_DEBUG, "get_jwt_token: encoded token=%s\n", token);
	jwt_decode(&jwtToken, token, NULL, 0);
	if (jwtToken == NULL) {
		_log(LOG_NOTICE, "get_jwt_token: Token decode error");
		return NULL;
	}
	json_decref(respRoot);
	json_decref(accessToken);
	return jwtToken;
}
/*
 * Process the response received from keycloak
 */
size_t process_auth_resp(void *respData, size_t size, size_t count, void *userData) 
{

	if ((count <= 0) || (!respData)) {
		_log(LOG_NOTICE, "process_auth_resp: Received nothing from keycloak for user = %s\n", (char *)userData);
		return BACKEND_ERROR;
	}
	_log(LOG_DEBUG, "process_auth_resp: Received response from keycloak: User = %s, Response = %s\n", (char *)userData, (char *)respData);

	jwt_t *jwtToken;
	jwtToken = get_jwt_token(respData);
	if (jwtToken == NULL) {
		_log(LOG_NOTICE, "process_auth_resp:user=<%s>, Auth Failure, JWT decode error\n", (char *)userData);
		return BACKEND_ERROR;
	}
	user_info_t userinfo;
	int ret;
	ret = get_jwt_topic(jwtToken, userinfo.topic);
	if (ret < 0) {
		_log(LOG_NOTICE, "process_auth_resp:user=<%s>, Auth Failure, no topic in JWT\n", (char *)userData);
		return BACKEND_ERROR;
	}
	ret = get_jwt_exp_time(jwtToken, &(userinfo.exp_time));
	if (ret < 0) {
		_log(LOG_NOTICE, "process_auth_resp:user=<%s>, no expiry time in JWT\n", (char *)userData);
	}
	ret = get_jwt_role(jwtToken, userinfo.role);
	if (ret < 0) {
		_log(LOG_NOTICE, "process_auth_resp:user=<%s>, Auth Failure, no role in JWT\n", (char *)userData);
		return BACKEND_ERROR;
	}
	_log(LOG_DEBUG, "process_auth_resp: Allowed topic=%s, Allowed Role=%s, exp_time=%ld\n", userinfo.topic, userinfo.role, userinfo.exp_time);
	sprintf(userinfo.username, "%s", (char *)userData);
	add_user_info(&userinfo);
	print_user_list();
	return BACKEND_DEFER;
}
/*
 * Send authentication request to keycloak
 */
static int http_post(void *handle, char *uri, const char *username, const char *password, const char *topic, int acc, int method)
{
	struct jwt_backend *conf = (struct jwt_backend *)handle;
	CURL *curl;
	struct curl_slist *headerlist = NULL;
	int re;
	//int urllen = 0;
	int respCode = 0;
	int ok = BACKEND_DEFER;
	char url[BUFSIZ];
	char *data;

	if (username == NULL)
	{
		return BACKEND_DEFER;
	}

	password = (password && *password) ? password : "";
	topic = (topic && *topic) ? topic : "";

	if ((curl = curl_easy_init()) == NULL)
	{
		_fatal("create curl_easy_handle fails");
		return BACKEND_ERROR;
	}
	//if (conf->hostheader != NULL)
	//	headerlist = curl_slist_append(headerlist, conf->hostheader);
	//headerlist = curl_slist_append(headerlist, "Expect:");

	//_log(LOG_NOTICE, "u=%s p=%s t=%s acc=%d", username, password, topic, acc);


	// uri begins with a slash
	snprintf(url, sizeof(url), "%s://%s:%d%s",
			 strcmp(conf->with_tls, "true") == 0 ? "https" : "http",
			 conf->hostname ? conf->hostname : "127.0.0.1",
			 conf->port,
			 uri);

	char *escaped_username = curl_easy_escape(curl, username, 0);
	char *escaped_password = curl_easy_escape(curl, password, 0);
	char *escaped_topic = curl_easy_escape(curl, topic, 0);
	// char* escaped_clientid = curl_easy_escape(curl, clientid, 0);

	char string_acc[20];
	snprintf(string_acc, 20, "%d", acc);

	char *string_envs = (char *)malloc(MAXPARAMSLEN);
	if (string_envs == NULL)
	{
		_fatal("ENOMEM");
		return BACKEND_ERROR;
	}

	memset(string_envs, 0, MAXPARAMSLEN);

	//get the sys_env from here
	int env_num = 0;
	if (method == METHOD_GETUSER && conf->getuser_envs != NULL)
	{
		env_num = get_string_envs(curl, conf->getuser_envs, string_envs);
	}
	else if (method == METHOD_SUPERUSER && conf->superuser_envs != NULL)
	{
		env_num = get_string_envs(curl, conf->superuser_envs, string_envs);
	}
	else if (method == METHOD_ACLCHECK && conf->aclcheck_envs != NULL)
	{
		env_num = get_string_envs(curl, conf->aclcheck_envs, string_envs);
	}
	if (env_num == -1)
	{
		return BACKEND_ERROR;
	}
	//---- over ----

	//data = (char *)malloc(strlen(string_envs) + strlen(escaped_username) + strlen(escaped_password) + strlen(escaped_topic) + strlen(string_acc) + strlen(escaped_clientid) + 50);
	int data_bytes = strlen(string_envs)+strlen(escaped_username)+strlen(escaped_password)+strlen(escaped_topic)+strlen(string_acc)+50;
	data = (char *)malloc(data_bytes);
	if (data == NULL)
	{
		_fatal("ENOMEM");
		return BACKEND_ERROR;
	}
	//	sprintf(data, "%susername=%s&password=%s&topic=%s&acc=%s&clientid=%s",
	//		string_envs,
	//		escaped_username,
	//		escaped_password,
	//		escaped_topic,
	//		string_acc,
	//		clientid);
	memset(data, 0, data_bytes);
	sprintf(data, "%susername=%s&password=%s",
			string_envs,
			escaped_username,
			escaped_password);

	_log(LOG_DEBUG, "url=%s", url);
	_log(LOG_DEBUG, "data=%s", data);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(curl, CURLOPT_USERNAME, username);
	curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, username);
	if (strcmp(conf->with_tls, "true") == 0)
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, process_auth_resp);
	re = curl_easy_perform(curl);
	if (re == CURLE_OK)
	{
		re = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &respCode);
		if (re == CURLE_OK && respCode >= 200 && respCode < 300)
		{
			ok = BACKEND_ALLOW;
		}
		else if (re == CURLE_OK && respCode >= 500)
		{
			ok = BACKEND_ERROR;
		}
		else
		{
			//_log(LOG_NOTICE, "http auth fail re=%d respCode=%d", re, respCode);
		}
	}
	else
	{
		re = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &respCode);
		if (respCode >= 200 && respCode < 300)
			ok = BACKEND_ALLOW;
		else {
			_log(LOG_NOTICE, "Error: Authentication failed for user=%s", username);
			_log(LOG_DEBUG, "HTTP Request URL=%s::HTTP response code=%d", url, respCode);
			ok = BACKEND_ERROR;
		}
	}

	curl_easy_cleanup(curl);
	curl_slist_free_all(headerlist);
	free(data);
	free(string_envs);
	free(escaped_username);
	free(escaped_password);
	free(escaped_topic);
	//free(escaped_clientid);
	return (ok);
}

#endif /* BE_JWT */

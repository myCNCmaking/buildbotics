/******************************************************************************\

                 This file is part of the BuildBotics Webserver.

                Copyright (c) 2014-2015, Cauldron Development LLC
                               All rights reserved.

        The BuildBotics Webserver is free software: you can redistribute
        it and/or modify it under the terms of the GNU General Public
        License as published by the Free Software Foundation, either
        version 2 of the License, or (at your option) any later version.

        The BuildBotics Webserver is distributed in the hope that it will
        be useful, but WITHOUT ANY WARRANTY; without even the implied
        warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
        PURPOSE.  See the GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this software.  If not, see
        <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
        You may request written permission by emailing the authors.

                For information regarding this software email:
                               Joseph Coffland
                        joseph@cauldrondevelopment.com

\******************************************************************************/

#include "Transaction.h"
#include "App.h"
#include "AWS4Post.h"

#include <cbang/event/Client.h>
#include <cbang/event/Buffer.h>
#include <cbang/event/BufferDevice.h>
#include <cbang/event/Event.h>

#include <cbang/json/JSON.h>
#include <cbang/log/Logger.h>
#include <cbang/util/DefaultCatch.h>
#include <cbang/db/maria/EventDB.h>
#include <cbang/time/Timer.h>
#include <cbang/security/Digest.h>
#include <cbang/net/URI.h>

#include <mysql/mysqld_error.h>

using namespace std;
using namespace cb;
using namespace BuildBotics;


Transaction::Transaction(App &app, evhttp_request *req) :
  Request(req), Event::OAuth2Login(app.getEventClient()), app(app),
  jsonFields(0) {
  LOG_DEBUG(5, "Transaction()");
}


Transaction::~Transaction() {
  LOG_DEBUG(5, "~Transaction()");
}


SmartPointer<JSON::Dict> Transaction::parseArgsPtr() {
  return SmartPointer<JSON::Dict>::Null(&parseArgs());
}


bool Transaction::lookupUser(bool skipAuthCheck) {
  if (!user.isNull()) return true;

  // Get session
  string session = findCookie(app.getSessionCookieName());
  if (session.empty() && inHas("Authorization"))
    session = inGet("Authorization").substr(6, 32);
  if (session.empty()) return false;

  // Check Authorization header matches first 32 bytes of session
  if (!skipAuthCheck) {
    if (!inHas("Authorization")) return false;

    string auth = inGet("Authorization");
    unsigned len = auth.length();

    if (len < 38 || auth.compare(0, 6, "Token ") ||
        session.compare(0, len - 6, auth.c_str() + 6)) return false;
  }

  // Get user
  user = app.getUserManager().get(session);

  // Check if we have a user and it's not expired
  if (user.isNull() || user->hasExpired()) {
    user.release();
    setCookie(app.getSessionCookieName(), "", "", "/");
    return false;
  }

  // Check if the user auth is expiring soon
  if (user->isExpiring()) {
    app.getUserManager().updateSession(user);
    user->setCookie(*this);
  }

  LOG_DEBUG(3, "User: " << user->getName());

  return true;
}


void Transaction::requireUser() {
  lookupUser();
  if (user.isNull() || !user->isAuthenticated())
    THROWX("Not authorized, please login", HTTP_UNAUTHORIZED);
}


void Transaction::requireUser(const string &name) {
  requireUser();
  if (user->getName() != name) THROWX("Not authorized", HTTP_UNAUTHORIZED);
}


bool Transaction::isUser(const string &name) {
  return !user.isNull() && user->getName() == name;
}


void Transaction::query(event_db_member_functor_t member, const string &s,
                        const SmartPointer<JSON::Value> &dict) {
  if (db.isNull()) db = app.getDBConnection();
  db->query(this, member, s, dict);
}


bool Transaction::apiError(int status, const string &msg) {
  LOG_ERROR(msg);

  // Reset output
  if (!writer.isNull()) {
    //writer->reset();
    writer.release();
  }
  getOutputBuffer().clear();

  // Drop DB connection
  if (!db.isNull()) db->close();

  // Send error message
  setContentType("text/plain");
  reply(status, msg);

  return true;
}


bool Transaction::pleaseLogin() {
  apiError(HTTP_UNAUTHORIZED, "Not authorized, please login");
  return true;
}


void Transaction::processProfile(const SmartPointer<JSON::Value> &profile) {
  if (!profile.isNull())
    try {
      // Authenticate user
      user->authenticate(profile->getString("provider"),
                         profile->getString("id"));
      app.getUserManager().updateSession(user);

      // Fix up Facebook avatar
      if (profile->getString("provider") == "facebook")
        profile->insert("avatar", "http://graph.facebook.com/" +
                        profile->getString("id") + "/picture?type=small");

      // Fix up for GitHub name
      if ((!profile->has("name") ||
           String::trim(profile->getString("name")).empty()) &&
          profile->hasString("login"))
        profile->insert("name", profile->getString("login"));

      LOG_DEBUG(3, "Profile: " << *profile);

      query(&Transaction::login, "CALL Login(%(provider)s, %(id)s, %(name)s, "
            "%(email)s, %(avatar)s);", profile);

      return;
    } CATCH_ERROR;

  redirect("/");
}


bool Transaction::apiAuthUser() {
  lookupUser();

  if (!user.isNull() && user->isAuthenticated()) {
    SmartPointer<JSON::Dict> dict = new JSON::Dict;
    dict->insert("provider", user->getProvider());
    dict->insert("id", user->getID());

    jsonFields = "*profile things followers following starred badges events";

    query(&Transaction::authUser,
          "CALL GetUser(%(provider)s, %(id)s)", dict);

  } else pleaseLogin();

  return true;
}


bool Transaction::apiAuthLogin() {
  const URI &uri = getURI();

  // Get user
  lookupUser(true);
  if (user.isNull()) user = app.getUserManager().create();
  if (user->isAuthenticated()) {
    redirect("/");
    return true;
  }

  // Set session cookie
  if (!uri.has("state")) user->setCookie(*this);

  OAuth2 *auth;
  string path = getURI().getPath();
  if (String::endsWith(path, "/callback"))
    path = path.substr(0, path.length() - 9);

  if (String::endsWith(path, "/google")) auth = &app.getGoogleAuth();
  else if (String::endsWith(path, "/github")) auth = &app.getGitHubAuth();
  else if (String::endsWith(path, "/facebook")) auth = &app.getFacebookAuth();
  else THROWC("Unsupported login provider", HTTP_BAD_REQUEST);

  return OAuth2Login::authorize(*this, *auth, user->getToken());
}


bool Transaction::apiAuthLogout() {
  setCookie(app.getSessionCookieName(), "", "", "/", 1);
  getJSONWriter()->write("ok");
  setContentType("application/json");
  reply();
  return true;
}


bool Transaction::apiGetProfiles() {
  JSON::ValuePtr args = parseArgsPtr();
  query(&Transaction::returnList,
        "CALL FindProfiles(%(query)s, %(order)s, %(limit)u, %(offset)u)", args);
  return true;
}


bool Transaction::apiProfileRegister() {
  lookupUser();
  if (user.isNull()) return pleaseLogin();

  SmartPointer<JSON::Dict> dict = new JSON::Dict;
  dict->insert("profile", getArg("profile"));
  dict->insert("provider", user->getProvider());
  dict->insert("id", user->getID());

  query(&Transaction::registration,
        "CALL Register(%(profile)s, %(provider)s, %(id)s)", dict);
  return true;
}


bool Transaction::apiProfileAvailable() {
  query(&Transaction::returnBool, "CALL Available(%(profile)s)",
        parseArgsPtr());
  return true;
}


bool Transaction::apiProfileSuggest() {
  lookupUser();
  if (user.isNull()) return pleaseLogin();

  SmartPointer<JSON::Dict> dict = new JSON::Dict;
  dict->insert("provider", user->getProvider());
  dict->insert("id", user->getID());

  query(&Transaction::returnList, "CALL Suggest(%(provider)s, %(id)s, 5)",
        dict);

  return true;
}


bool Transaction::apiPutProfile() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  query(&Transaction::returnOK,
        "CALL PutProfile(%(profile)s, %(fullname)s, %(location)s, %(url)s, "
        "%(bio)s)", args);

  return true;
}


bool Transaction::apiGetProfile() {
  lookupUser();
  JSON::ValuePtr args = parseArgsPtr();
  args->insertBoolean("unpublished", isUser(args->getString("profile")));

  jsonFields = "*profile things followers following starred badges events";

  query(&Transaction::returnJSONFields,
        "CALL GetProfile(%(profile)s, %(unpublished)b)", args);

  return true;
}


bool Transaction::apiGetProfileAvatar() {
  JSON::ValuePtr args = parseArgsPtr();
  query(&Transaction::download, "CALL GetProfileAvatar(%(profile)s)", args);
  return true;
}


bool Transaction::apiFollow() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("user", user->getName());

  query(&Transaction::returnOK, "CALL Follow(%(user)s, %(profile)s)", args);

  return true;
}


bool Transaction::apiUnfollow() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("user", user->getName());

  query(&Transaction::returnOK, "CALL Unfollow(%(user)s, %(profile)s)", args);

  return true;
}


bool Transaction::apiGetThings() {
  JSON::ValuePtr args = parseArgsPtr();

  query(&Transaction::returnList,
        "CALL FindThings(%(query)s, %(license)s, %(order)s, %(limit)u, "
        "%(offset)u)", args);
  return true;
}


bool Transaction::apiThingAvailable() {
  query(&Transaction::returnBool, "CALL ThingAvailable(%(profile)s, %(thing)s)",
        parseArgsPtr());
  return true;
}


bool Transaction::apiGetThing() {
  lookupUser();
  JSON::ValuePtr args = parseArgsPtr();

  args->insertBoolean("unpublished", isUser(args->getString("profile")));

  string userID;
  if (user.isNull()) {
    if (inHas("X-Real-IP")) userID = inGet("X-Real-IP");
    else userID = getClientIP().toString();

  } else userID = user->getName();

  args->insert("user", userID);

  jsonFields = "*thing files comments stars";

  query(&Transaction::returnJSONFields,
        "CALL GetThing(%(profile)s, %(thing)s, %(user)s, %(unpublished)b)",
        args);

  return true;
}


bool Transaction::apiPublishThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  query(&Transaction::returnOK,
        "CALL PublishThing(%(profile)s, %(thing)s)", args);

  return true;
}


bool Transaction::apiPutThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  if (!args->hasString("type")) args->insert("type", "project");

  query(&Transaction::returnOK,
        "CALL PutThing(%(profile)s, %(thing)s, %(type)s, %(title)s, "
        "%(url)s, %(instructions)s, %(license)s, %(publish)b)", args);

  return true;
}


bool Transaction::apiRenameThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  query(&Transaction::returnOK,
        "CALL RenameThing(%(profile)s, %(thing)s, %(name)s)", args);

  return true;
}


bool Transaction::apiDeleteThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  query(&Transaction::returnOK, "CALL DeleteThing(%(profile)s, %(thing)s)",
        args);

  return true;
}


bool Transaction::apiStarThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("user", user->getName());

  query(&Transaction::returnOK,
        "CALL StarThing(%(user)s, %(profile)s, %(thing)s)", args);

  return true;
}


bool Transaction::apiUnstarThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("user", user->getName());

  query(&Transaction::returnOK,
        "CALL UnstarThing(%(user)s, %(profile)s, %(thing)s)", args);

  return true;
}


bool Transaction::apiTagThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();

  query(&Transaction::returnOK,
        "CALL MultiTagThing(%(profile)s, %(thing)s, %(tags)s)", args);

  return true;
}


bool Transaction::apiUntagThing() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  query(&Transaction::returnOK,
        "CALL MultiUntagThing(%(profile)s, %(thing)s, %(tags)s)", args);

  return true;
}


bool Transaction::apiPostComment() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("owner", user->getName());

  query(&Transaction::returnU64,
        "CALL PostComment(%(owner)s, %(profile)s, %(thing)s, %(ref)u, "
        "%(text)s)", args);

  return true;
}


bool Transaction::apiUpdateComment() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("owner", user->getName());

  query(&Transaction::returnOK,
        "CALL UpdateComment(%(owner)s, %(comment)u, %(text)s)", args);

  return true;
}


bool Transaction::apiDeleteComment() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser();
  args->insert("owner", user->getName());

  query(&Transaction::returnOK, "CALL DeleteComment(%(owner)s, %(comment)u)",
        args);

  return true;
}


bool Transaction::apiDownloadFile() {
  JSON::ValuePtr args = parseArgsPtr();

  query(&Transaction::download,
        "CALL DownloadFile(%(profile)s, %(thing)s, %(file)s, %(count)b)", args);

  return true;
}


bool Transaction::apiGetFile() {
  THROW("Not yet implemented");
}


bool Transaction::apiPutFile() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  // Create GUID
  Digest hash("sha256");
  hash.update(args->getString("profile"));
  hash.update(args->getString("thing"));
  hash.update(args->getString("file"));
  hash.updateWith(Timer::now());
  string guid = hash.toBase64();

  // Create key
  string key = guid + "/" + args->getString("file");

  // Create URLs
  string uploadURL = "https://" + app.getAWSBucket() + ".s3.amazonaws.com/";
  string fileURL = "/" + args->getString("profile") + "/" +
    args->getString("thing") + "/" + args->getString("file");
  args->insert("url", uploadURL + URI::encode(key));

  // Build POST
  AWS4Post post(app.getAWSBucket(), key, app.getAWSUploadExpires(),
                Time::now(), "s3", app.getAWSRegion());

  uint32_t size = args->getU32("size");
  post.setLengthRange(size, size);
  post.insert("Content-Type", args->getString("type"));
  post.insert("acl", "public-read");
  post.insert("success_action_status", "201");
  post.addCondition("name", args->getString("file"));
  post.sign(app.getAWSID(), app.getAWSSecret());

  // Write JSON
  setContentType("application/json");
  writer = getJSONWriter();
  writer->beginDict();
  writer->insert("upload_url", uploadURL);
  writer->insert("file_url", URI::encode(fileURL));
  writer->insert("guid", guid);
  writer->beginInsert("post");
  post.write(*writer);
  writer->endDict();
  writer.release();

  // Write to DB
  query(&Transaction::returnReply,
        "CALL PutFile(%(profile)s, %(thing)s, %(file)s, %(type)s, %(size)u, "
        "%(url)s, %(caption)s, %(display)b)", args);

  return true;
}


bool Transaction::apiDeleteFile() {
  JSON::ValuePtr args = parseArgsPtr();
  requireUser(args->getString("profile"));

  query(&Transaction::returnOK,
        "CALL DeleteFile(%(profile)s, %(thing)s, %(file)s)", args);

  return true;
}


bool Transaction::apiGetTags() {
  JSON::ValuePtr args = parseArgsPtr();
  query(&Transaction::returnList, "CALL GetTags(%(limit)u)", args);
  return true;
}


bool Transaction::apiGetTagThings() {
  JSON::ValuePtr args = parseArgsPtr();
  query(&Transaction::returnList,
        "CALL FindThingsByTag(%(tag)s, %(order)s, %(limit)u, %(offset)u)",
        args);
  return true;
}


bool Transaction::apiGetLicenses() {
  query(&Transaction::returnList, "CALL GetLicenses()");
  return true;
}


bool Transaction::apiGetEvents() {
  JSON::ValuePtr args = parseArgsPtr();
  query(&Transaction::returnList, "CALL GetEvents(%(subject)s, %(action)s, "
        "%(object_type)s, %(object)s, %(owner)s, %(since)s, %(limit)u)", args);
  return true;
}


bool Transaction::apiNotFound() {
  apiError(HTTP_NOT_FOUND, "Invalid API method " + getURI().getPath());
  return true;
}


string Transaction::nextJSONField() {
  if (!jsonFields) return "";

  const char *start = jsonFields;
  const char *end = jsonFields;
  while (*end && *end != ' ') end++;

  jsonFields = *end ? end + 1 : 0;

  return string(start, end - start);
}


void Transaction::download(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_BEGIN_RESULT:
  case MariaDB::EventDBCallback::EVENTDB_END_RESULT:
    break;

  case MariaDB::EventDBCallback::EVENTDB_DONE:
    redirect(redirectTo);
    break;

  case MariaDB::EventDBCallback::EVENTDB_ROW:
    redirectTo = db->getString(0);
    break;

  default: returnReply(state); return;
  }
}


void Transaction::authUser(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_ERROR:
    // Not really logged in, clear cookie
    setCookie(app.getSessionCookieName(), "", "", "/");

    // Fall through

  default: return returnJSONFields(state);
  }
}


void Transaction::login(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_BEGIN_RESULT:
  case MariaDB::EventDBCallback::EVENTDB_END_RESULT:
    break;

  case MariaDB::EventDBCallback::EVENTDB_ROW:
    user->setName(db->getString(0));
    user->setAuth(db->getU64(1));
    break;

  case MariaDB::EventDBCallback::EVENTDB_DONE:
    app.getUserManager().updateSession(user);
    user->setCookie(*this);

    getJSONWriter()->write("ok");
    setContentType("application/json");
    redirect("/");
    break;

  default: returnReply(state); return;
  }
}


void Transaction::registration(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_DONE:
    user->setName(getArg("profile"));
    app.getUserManager().updateSession(user);
    user->setCookie(*this);
    // Fall through

  default: returnOK(state); return;
  }
}


void Transaction::returnOK(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_DONE:
    getJSONWriter()->write("ok");
    setContentType("application/json");
    reply();
    break;

  default: returnReply(state);
  }

}


void Transaction::returnList(MariaDB::EventDBCallback::state_t state) {
  if (state != MariaDB::EventDBCallback::EVENTDB_ROW) returnJSON(state);

  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_ROW:
    writer->beginAppend();

    if (db->getFieldCount() == 1) db->writeField(*writer, 0);
    else db->writeRowDict(*writer);
    break;

  case MariaDB::EventDBCallback::EVENTDB_BEGIN_RESULT:
    writer->beginList();
    break;

  case MariaDB::EventDBCallback::EVENTDB_END_RESULT:
    writer->endList();
    break;

  default: break;
  }
}


void Transaction::returnBool(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_ROW:
    writer->writeBoolean(db->getBoolean(0));
    break;

  default: return returnJSON(state);
  }
}



void Transaction::returnU64(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_ROW:
    writer->write(db->getU64(0));
    break;

  default: return returnJSON(state);
  }
}


void Transaction::returnJSON(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_ROW:
    if (db->getFieldCount() == 1) db->writeField(*writer, 0);
    else db->writeRowDict(*writer);
    break;

  case MariaDB::EventDBCallback::EVENTDB_BEGIN_RESULT:
    setContentType("application/json");
    if (writer.isNull()) writer = getJSONWriter();
    break;

  case MariaDB::EventDBCallback::EVENTDB_END_RESULT: break;

  default: return returnReply(state);
  }
}


void Transaction::returnJSONFields(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_ROW:
    if (writer->inDict()) db->insertRow(*writer, 0, -1, false);
    else {
      writer->appendDict();
      db->insertRow(*writer, 0, -1, false);
      writer->endDict();
    }
    break;

  case MariaDB::EventDBCallback::EVENTDB_BEGIN_RESULT: {
    setContentType("application/json");
    if (writer.isNull()) {
      writer = getJSONWriter();
      writer->beginDict();
    }

    string field = nextJSONField();
    if (field.empty()) THROW("Unexpected result set");
    if (field[0] == '*') writer->insertDict(field.substr(1));
    else writer->insertList(field);
    break;
  }

  case MariaDB::EventDBCallback::EVENTDB_END_RESULT:
    if (writer->inList()) writer->endList();
    else writer->endDict();
    break;

  case MariaDB::EventDBCallback::EVENTDB_DONE:
    if (!writer.isNull()) writer->endDict();
    // Fall through

  default: return returnReply(state);
  }
}


void Transaction::returnReply(MariaDB::EventDBCallback::state_t state) {
  switch (state) {
  case MariaDB::EventDBCallback::EVENTDB_DONE: {
    writer.release();
    reply();
    break;
  }

  case MariaDB::EventDBCallback::EVENTDB_ERROR: {
    int error = HTTP_INTERNAL_SERVER_ERROR;

    switch (db->getErrorNumber()) {
    case ER_SIGNAL_NOT_FOUND: error = HTTP_NOT_FOUND; break;
    default: break;
    }

    apiError(error,
             SSTR("DB:" << db->getErrorNumber() << ": " << db->getError()));
    break;
  }

  default:
    apiError(HTTP_INTERNAL_SERVER_ERROR, "Unexpected DB response");
    return;
  }
}

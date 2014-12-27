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

#include "HTTPRE2Matcher.h"

#include <cbang/Exception.h>
#include <cbang/event/Request.h>
#include <cbang/log/Logger.h>

#include <vector>

using namespace std;
using namespace cb;
using namespace BuildBotics;


HTTPRE2Matcher::HTTPRE2Matcher(unsigned methods, const string &pattern,
                               const SmartPointer<Event::HTTPHandler> &child) :
  methods(methods), matchAll(pattern.empty()), regex(pattern), child(child) {
  if (regex.error_code()) THROWS("Failed to compile RE2: " << regex.error());
}


bool HTTPRE2Matcher::operator()(Event::Request &req) {
  if (!(methods & req.getMethod())) return false;

  if (!matchAll) {
    int n = regex.NumberOfCapturingGroups();
    vector<RE2::Arg> args(n);
    vector<RE2::Arg *> argPtrs(n);
    vector<string> results(n);

    // Connect args
    for (int i = 0; i < n; i++) {
      args[i] = &results[i];
      argPtrs[i] = &args[i];
    }

    // Attempt match
    string path = req.getURI().getPath();
    if (!RE2::FullMatchN(path, regex, argPtrs.data(), n))
      return false;

    LOG_DEBUG(5, path << " matched " << regex.pattern());

    // Store results
    const map<int, string> &names = regex.CapturingGroupNames();
    for (int i = 0; i < n; i++)
      if (names.find(i + 1) != names.end())
        req.insertArg(names.at(i + 1), results[i]);
      else req.insertArg(results[i]);
  }

  return (*child)(req);
}

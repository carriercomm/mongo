/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/tools/mongodump_options.h"

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoDumpGlobalParams mongoDumpGlobalParams;

    Status addMongoDumpOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("out", "out,o", moe::String,
                "output directory or \"-\" for stdout")
                                  .setDefault(moe::Value(std::string("dump")));

        options->addOptionChaining("query", "query,q", moe::String, "json query");

        options->addOptionChaining("oplog", "oplog", moe::Switch,
                "Use oplog for point-in-time snapshotting");

        options->addOptionChaining("repair", "repair", moe::Switch,
                "try to recover a crashed database");

        options->addOptionChaining("forceTableScan", "forceTableScan", moe::Switch,
                "force a table scan (do not use $snapshot)");


        return Status::OK();
    }

    void printMongoDumpHelp(std::ostream* out) {
        *out << "Export MongoDB data to BSON files.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoDumpOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printMongoDumpHelp(&std::cout);
            ::_exit(0);
        }
        return Status::OK();
    }

    Status storeMongoDumpOptions(const moe::Environment& params,
                                 const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoDumpGlobalParams.repair = hasParam("repair");
        if (mongoDumpGlobalParams.repair){
            if (!hasParam("dbpath")) {
                log() << "repair mode only works with --dbpath" << std::endl;
                ::_exit(-1);
            }

            if (!hasParam("db")) {
                log() << "repair mode only works on 1 db at a time right now" << std::endl;
                ::_exit(-1);
            }
        }
        mongoDumpGlobalParams.query = getParam("query");
        mongoDumpGlobalParams.useOplog = hasParam("oplog");
        if (mongoDumpGlobalParams.useOplog) {
            if (hasParam("query") || hasParam("db") || hasParam("collection")) {
                log() << "oplog mode is only supported on full dumps" << std::endl;
                ::_exit(-1);
            }
        }
        mongoDumpGlobalParams.outputFile = getParam("out");
        mongoDumpGlobalParams.snapShotQuery = false;
        if (!hasParam("query") && !hasParam("dbpath") && !hasParam("forceTableScan")) {
            mongoDumpGlobalParams.snapShotQuery = true;
        }

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoDumpOptions)(InitializerContext* context) {
        return addMongoDumpOptions(&moe::startupOptions);
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(MongoDumpOptions)(InitializerContext* context) {
        Status ret = handlePreValidationMongoDumpOptions(moe::startupOptionsParsed);
        if (!ret.isOK()) {
            return ret;
        }
        ret = moe::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(MongoDumpOptions)(InitializerContext* context) {
        return storeMongoDumpOptions(moe::startupOptionsParsed, context->args());
    }
}

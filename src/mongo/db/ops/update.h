//@file update.h

/**
 *    Copyright (C) 2008 10gen Inc.
 *    Copyright (C) 2013 Tokutek Inc.
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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"

namespace mongo {

    class UpdateDriver;

    // ---------- public -------------

    struct UpdateResult {
        const bool existing; // if existing objects were modified
        const bool mod;      // was this a $ mod
        const long long num; // how many objects touched
        OID upserted;        // if something was upserted, the new _id of the object

        UpdateResult(const bool e, const bool m,
                     const unsigned long long n, const BSONObj &upsertedObj) :
            existing(e), mod(m), num(n) {
            upserted.clear();
            const BSONElement id = upsertedObj["_id"];
            if (!e && n == 1 && id.type() == jstOID) {
                upserted = id.OID();
            }
        }
    };

    BSONObj invertUpdateMods(const BSONObj &updateobj);

    /* returns true if an existing object was updated, false if no existing object was found.
       multi - update multiple objects - mostly useful with things like $set
       su - allow access to system namespaces (super user)
    */
    UpdateResult updateObjects(const char* ns,
                               const BSONObj& updateobj,
                               const BSONObj& pattern,
                               bool upsert,
                               bool multi,
                               bool logop,
                               OpDebug& debug,
                               bool fromMigrate = false,
                               const QueryPlanSelectionPolicy& planPolicy = QueryPlanSelectionPolicy::any());

    /** A variant of updateObjects that is only useable if the new update framework is enabled.
     *  It assumes that the UpdateDriver has already been initialized outside the lock.
     */
    UpdateResult updateObjects(UpdateDriver* driver,
                               const char* ns,
                               const BSONObj& updateobj,
                               const BSONObj& pattern,
                               bool upsert,
                               bool multi,
                               bool logop,
                               OpDebug& debug,
                               bool fromMigrate = false,
                               const QueryPlanSelectionPolicy& planPolicy = QueryPlanSelectionPolicy::any());

    /*
     * Similar to updateObjects but not strict about applying mods that can fail during initial
     * replication.
     *
     * Reference ticket: SERVER-4781
     */
    UpdateResult updateObjectsForReplication(const char* ns,
                                             const BSONObj& updateobj,
                                             const BSONObj& pattern,
                                             bool upsert,
                                             bool multi,
                                             bool logop,
                                             OpDebug& debug,
                                             bool fromMigrate = false,
                                             const QueryPlanSelectionPolicy& planPolicy =
                                                 QueryPlanSelectionPolicy::any());

    UpdateResult _updateObjects(bool su,
                                const char* ns,
                                const BSONObj& updateobj,
                                const BSONObj& pattern,
                                bool upsert,
                                bool multi,
                                bool logop,
                                OpDebug& debug,
                                RemoveSaver* rs = 0,
                                bool fromMigrate = false,
                                const QueryPlanSelectionPolicy& planPolicy
                                    = QueryPlanSelectionPolicy::any(),
                                bool forReplication = false);

    UpdateResult _updateObjects(UpdateDriver* driver,
                                bool su,
                                const char* ns,
                                const BSONObj& updateobj,
                                const BSONObj& pattern,
                                bool upsert,
                                bool multi,
                                bool logop,
                                OpDebug& debug,
                                RemoveSaver* rs = 0,
                                bool fromMigrate = false,
                                const QueryPlanSelectionPolicy& planPolicy
                                    = QueryPlanSelectionPolicy::any(),
                                bool forReplication = false);


    /**
     * takes the from document and returns a new document
     * after apply all the operators 
     * e.g. 
     *   applyUpdateOperators( BSON( "x" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
     *   returns: { x : 2 }
     */
    BSONObj applyUpdateOperators( const BSONObj& from, const BSONObj& operators );
    
}  // namespace mongo

// delete.cpp

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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/oplog.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/collection.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/query.h"
#include "mongo/util/stacktrace.h"
#include "mongo/db/oplog_helpers.h"

namespace mongo {

    void deleteOneObject(Collection *cl, const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        cl->deleteObject(pk, obj, flags);
        cl->notifyOfWriteOp();
    }
    
    // Special-cased helper for deleting ranges out of an index.
    long long deleteIndexRange(const string &ns,
                               const BSONObj &min,
                               const BSONObj &max,
                               const BSONObj &keyPattern,
                               const bool maxInclusive,
                               const bool fromMigrate,
                               uint64_t flags) {
        Collection *cl = getCollection(ns);
        if (cl == NULL) {
            return 0;
        }

        IndexDetails &i = cl->idx(cl->findIndexByKeyPattern(keyPattern));
        // Extend min to get (min, MinKey, MinKey, ....)
        KeyPattern kp(keyPattern);
        BSONObj newMin = KeyPattern::toKeyFormat(kp.extendRangeBound(min, false));
        // If upper bound is included, extend max to get (max, MaxKey, MaxKey, ...)
        // If not included, extend max to get (max, MinKey, MinKey, ....)
        BSONObj newMax = KeyPattern::toKeyFormat(kp.extendRangeBound(max, maxInclusive));

        long long nDeleted = 0;
        for (shared_ptr<Cursor> c(Cursor::make(cl, i, newMin, newMax, maxInclusive, 1));
             c->ok(); c->advance()) {
            const BSONObj pk = c->currPK();
            const BSONObj obj = c->current();
            OplogHelpers::logDelete(ns.c_str(), obj, fromMigrate);
            deleteOneObject(cl, pk, obj, flags);
            nDeleted++;
        }
        return nDeleted;
    }

    long long _deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop) {
        Collection *cl = getCollection(ns);
        if (cl == NULL) {
            return 0;
        }

        uassert(10101, "can't remove from a capped collection", !cl->isCapped());

        BSONObj obj;
        BSONObj pk = cl->getSimplePKFromQuery(pattern);

        // Fast-path for simple primary key deletes.
        if (!pk.isEmpty()) {
            if (queryByPKHack(cl, pk, pattern, obj)) {
                if (logop) {
                    OplogHelpers::logDelete(ns, obj, false);
                }
                deleteOneObject(cl, pk, obj);
                return 1;
            }
            return 0;
        }

        long long nDeleted = 0;
        for (shared_ptr<Cursor> c = getOptimizedCursor(ns, pattern); c->ok(); ) {
            pk = c->currPK();
            if (c->getsetdup(pk)) {
                c->advance();
                continue;
            }
            if (!c->currentMatches()) {
                c->advance();
                continue;
            }

            obj = c->current();

            // justOne deletes do not intend to advance, so there's
            // no reason to do so here and potentially overlock rows.
            if (!justOne) {
                // There may be interleaved query plans that utilize multiple
                // cursors, some of which point to the same PK. We advance
                // here while those cursors point the row to be deleted.
                // 
                // Make sure to get local copies of pk/obj before advancing.
                pk = pk.getOwned();
                obj = obj.getOwned();
                while (c->ok() && c->currPK() == pk) {
                    c->advance();
                }
            }

            if (logop) {
                OplogHelpers::logDelete(ns, obj, false);
            }
            deleteOneObject(cl, pk, obj);
            nDeleted++;

            if (justOne) {
                break;
            }
        }
        return nDeleted;
    }

    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
    */
    long long deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop) {
        if (NamespaceString::isSystem(ns)) {
            uassert(12050, "cannot delete from system namespace",
                    legalClientSystemNS(ns, true));
        }

        if (!NamespaceString::normal(ns)) {
            log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
            uasserted(10100, "cannot delete from collection with reserved $ in name");
        }

        return _deleteObjects(ns, pattern, justOne, logop);
    }
}

/** @file oplogreader.cpp */
#include "pch.h"

#include "pcrecpp.h"
#include <boost/thread/thread.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl.h"
#include "mongo/util/net/message.h"
#include "mongo/util/background.h"
#include "mongo/client/connpool.h"
#include "mongo/db/commands.h"
#include "mongo/db/security.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/instance.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/relock.h"

namespace mongo {
    BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    // copied (only the) comment from mongodb 2.6
    /* Generally replAuthenticate will only be called within system threads to fully authenticate
     * connections to other nodes in the cluster that will be used as part of internal operations.
     * If a user-initiated action results in needing to call replAuthenticate, you can call it
     * with skipAuthCheck set to false. Only do this if you are certain that the proper auth
     * checks have already run to ensure that the user is authorized to do everything that this
     * connection will be used for!
     */
    bool replAuthenticate(DBClientBase *conn) {
        if( noauth ) {
            return true;
        }
        if( ! cc().isAdmin() ) {
            log() << "replauthenticate: requires admin permissions, failing\n";
            return false;
        }

        string u;
        string p;
        if (internalSecurity.pwd.length() > 0) {
            u = internalSecurity.user;
            p = internalSecurity.pwd;
        }
        else {
            BSONObj user;
            {
                Client::ReadContext ctxt("local.");
                if( !Helpers::findOne("local.system.users", userReplQuery, user) ||
                        // try the first user in local
                        !Helpers::getSingleton("local.system.users", user) ) {
                    log() << "replauthenticate: no user in local.system.users to use for authentication\n";
                    return false;
                }
            }
            u = user.getStringField("user");
            p = user.getStringField("pwd");
            massert( 10392 , "bad user object? [1]", !u.empty());
            massert( 10393 , "bad user object? [2]", !p.empty());
        }

        string err;
        if( !conn->auth("local", u.c_str(), p.c_str(), err, false) ) {
            log() << "replauthenticate: can't authenticate to master server, user:" << u << endl;
            return false;
        }
        if ( internalSecurity.pwd.length() > 0 ) {
            conn->setAuthenticationTable(
                    AuthenticationTable::getInternalSecurityAuthenticationTable() );
        }
        return true;
    }

    void getMe(BSONObj& me) {
        string myname = getHostName();
        Client::Transaction transaction(0);            
        // local.me is an identifier for a server for getLastError w:2+
        if ( ! Helpers::getSingleton( "local.me" , me ) ||
             ! me.hasField("host") ||
             me["host"].String() != myname ) {
        
            // clean out local.me
            Helpers::emptyCollection("local.me");
        
            // repopulate
            BSONObjBuilder b;
            b.appendOID( "_id" , 0 , true );
            b.append( "host", myname );
            me = b.obj();
            Helpers::putSingleton( "local.me" , me );
        }
        transaction.commit(0);
    }

    bool replHandshake(DBClientConnection *conn) {
        BSONObj me;
        try {
            Lock::DBRead lk("local");
            getMe(me);
        }
        catch (RetryWithWriteLock &e) {
            Lock::DBWrite lk("local");
            getMe(me);
        }

        BSONObjBuilder cmd;
        cmd.appendAs( me["_id"] , "handshake" );
        if (theReplSet) {
            cmd.append("member", theReplSet->selfId());
        }

        BSONObj res;
        bool ok = conn->runCommand( "admin" , cmd.obj() , res );
        // ignoring for now on purpose for older versions
        LOG(ok ? 1 : 0) << "replHandshake res not: " << ok << " res: " << res << endl;
        return true;
    }

    OplogReader::OplogReader( bool doHandshake ) : 
        _doHandshake( doHandshake ) { 
        
        _tailingQueryOptions = QueryOption_SlaveOk;
        _tailingQueryOptions |= QueryOption_CursorTailable | QueryOption_OplogReplay;
        
        /* TODO: slaveOk maybe shouldn't use? */
        _tailingQueryOptions |= QueryOption_AwaitData;
    }

    bool OplogReader::commonConnect(const string& hostName) {
        if( conn() == 0 ) {
            _conn = shared_ptr<DBClientConnection>(new DBClientConnection(false,
                                                                          0,
                                                                          30 /* tcp timeout */));
            string errmsg;
            if ( !_conn->connect(hostName.c_str(), errmsg) ||
                 (!noauth && !replAuthenticate(_conn.get())) ) {
                resetConnection();
                log() << "repl: " << errmsg << endl;
                return false;
            }
        }
        return true;
    }
    
    bool OplogReader::connect(string hostName) {
        if (conn() != 0) {
            return true;
        }

        if ( ! commonConnect(hostName) ) {
            return false;
        }
        
        
        if ( _doHandshake && ! replHandshake(_conn.get() ) ) {
            return false;
        }

        return true;
    }

    bool OplogReader::connect(const BSONObj& rid, const int from, const string& to) {
        if (conn() != 0) {
            return true;
        }
        if (commonConnect(to)) {
            log() << "handshake between " << from << " and " << to << endl;
            return passthroughHandshake(rid, from);
        }
        return false;
    }

    bool OplogReader::passthroughHandshake(const BSONObj& rid, const int f) {
        BSONObjBuilder cmd;
        cmd.appendAs( rid["_id"], "handshake" );
        cmd.append( "member" , f );

        BSONObj res;
        return conn()->runCommand( "admin" , cmd.obj() , res );
    }

    void OplogReader::tailingQuery(const char *ns, Query& query, const BSONObj* fields ) {
        verify( !haveCursor() );
        LOG(2) << "repl: " << ns << ".find(" << query.toString() << ')' << endl;
        cursor.reset( _conn->query( ns, query, 0, 0, fields, _tailingQueryOptions ).release() );
    }
    
    void OplogReader::tailingQueryGTE(const char *ns, GTID gtid, const BSONObj* fields ) {
        BSONObjBuilder q;
        addGTIDToBSON("$gte", gtid, q);
        BSONObjBuilder query;
        query.append("_id", q.done());
        tailingQuery(ns, Query(query.done()).hint(BSON("_id" << 1)), fields);
    }

    shared_ptr<DBClientCursor> OplogReader::getRollbackCursor(GTID lastGTID) {
        shared_ptr<DBClientCursor> retCursor;
        BSONObjBuilder q;
        addGTIDToBSON("$lte", lastGTID, q);
        BSONObjBuilder query;
        query.append("_id", q.done());
        retCursor.reset(
            _conn->query(rsoplog, Query(query.done()).sort(reverseNaturalObj), 0, 0, NULL, QueryOption_SlaveOk).release()
            );
        return retCursor;
    }

    bool OplogReader::propogateSlaveLocation(GTID lastGTID){
        BSONObjBuilder cmd;
        cmd.append("updateSlave", 1);
        addGTIDToBSON("gtid", lastGTID, cmd);
        BSONObj ret;
        return _conn->runCommand(
            "local",
            cmd.done(),
            ret
            );
    }

    shared_ptr<DBClientCursor> OplogReader::getOplogRefsCursor(OID &oid) {
        shared_ptr<DBClientCursor> retCursor;
        // this maps to {_id : {$gt : { oid : oid , seq : 0 }}}
        retCursor.reset(_conn->query(rsOplogRefs, QUERY("_id" << BSON("$gt" << BSON("oid" << oid << "seq" << 0)) ).hint(BSON("_id" << 1))).release());
        return retCursor;
    }
}

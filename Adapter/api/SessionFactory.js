/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

/*global unified_debug */

"use strict";

var session     = require("./Session.js"),
    udebug      = unified_debug.getLogger("SessionFactory.js"),  
    userContext = require('../impl/common/UserContext.js');

var SessionFactory = function(key, dbConnectionPool, properties, annotations, delete_callback) {
  this.key = key;
  this.dbConnectionPool = dbConnectionPool;
  this.properties = properties;
  this.annotations = annotations;
  this.delete_callback = delete_callback;
  this.sessions = [];
  this.tableHandlers = {};
  this.tableMetadatas = {};
};


//openSession(Annotations annotations, Function(Object error, Session session, ...) callback, ...);
// Open new session or get one from a pool
SessionFactory.prototype.openSession = function() {
  var context = new userContext.UserContext(arguments, 2, 2, null, this);
  // delegate to context for execution
  context.openSession();
};


/** Get metadata for a table.
 * @param tableName the name of the table
 * @param callback
 */
SessionFactory.prototype.getTableMetadata = function() {
  var context = new userContext.UserContext(arguments, 2, 2, null, this);
  // delegate to context for execution
  context.getTableMetadata();
};


SessionFactory.prototype.close = function() {
  // TODO: close all sessions first
  this.dbConnectionPool.closeSync();
  this.delete_callback(this.key, this.properties.database);
};


SessionFactory.prototype.closeSession = function(index, session) {
  // TODO put session back into pool
  this.sessions[index] = null;
};


exports.SessionFactory = SessionFactory;

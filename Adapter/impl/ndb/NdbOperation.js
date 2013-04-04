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

/*global unified_debug, path, build_dir, api_dir, spi_doc_dir, assert */

"use strict";

var adapter       = require(path.join(build_dir, "ndb_adapter.node")).ndb,
    doc           = require(path.join(spi_doc_dir, "DBOperation")),
    stats_module  = require(path.join(api_dir,"stats.js")),
    stats         = stats_module.getWriter(["spi","ndb","DBOperation"]),
    index_stats   = stats_module.getWriter(["spi","ndb","key_access"]),
    COMMIT        = adapter.ndbapi.Commit,
    NOCOMMIT      = adapter.ndbapi.NoCommit,
    ROLLBACK      = adapter.ndbapi.Rollback,
    constants     = adapter.impl,
    OpHelper      = constants.OpHelper,
    opcodes       = doc.OperationCodes,
    udebug        = unified_debug.getLogger("NdbOperation.js");


/* Constructors.
   All of these use prototypes directly from the documentation.
*/
var DBResult = function() {};
DBResult.prototype = doc.DBResult;

// DBOperationError
var errorClassificationMap = {
  "ConstraintViolation" : "23000",
  "NoDataFound"         : "02000",
  "UnknownResultError"  : "08000"
};

function DBOperationError(ndb_error) {
  var mappedCode = errorClassificationMap[ndb_error.classification];
  this.message = ndb_error.message + " [" + ndb_error.code + "]";
  this.sqlstate = mappedCode || "NDB00";
  this.ndb_error = ndb_error;
  this.cause = null;
}

DBOperationError.prototype = doc.DBOperationError;
exports.DBOperationError = DBOperationError;

function IndirectError(dbOperationErr) {
  this.message = "Error";
  this.sqlstate = dbOperationErr.sqlstate;
  this.ndb_error = null;
  this.cause = dbOperationErr;
}
IndirectError.prototype = doc.DBOperationError;


var DBOperation = function(opcode, tx, indexHandler, tableHandler) {
  assert(tx);
 
  this.opcode         = opcode;
  this.userCallback   = null;
  this.transaction    = tx;
  this.keys           = {}; 
  this.values         = {};
  this.lockMode       = "";
  this.state          = doc.OperationStates[0];  // DEFINED
  this.result         = new DBResult();
  this.indexHandler   = indexHandler;   
  if(indexHandler) { 
    this.tableHandler = indexHandler.tableHandler; 
    this.index        = indexHandler.dbIndex;
  }      
  else {
    this.tableHandler = tableHandler;
    this.index        = {};  
  }
  
  /* NDB Impl-specific properties */
  this.ndbop        = null;
  this.autoinc      = null;
  this.buffers      = { 'row' : null, 'key' : null  };
  this.columnMask   = [];

  stats.incr(["created",opcode]);
};


function allocateKeyBuffer(op) {
  assert(op.buffers.key === null);
  op.buffers.key = new Buffer(op.index.record.getBufferSize());

  index_stats.incr([ op.tableHandler.dbTable.database,
                     op.tableHandler.dbTable.name,
                    (op.index.isPrimaryKey ? "PrimaryKey" : op.index.name)
                   ]);
}

function releaseKeyBuffer(op) {
  if(op.opcode !== 2) {  /* all but insert use a key */
    op.buffers.key = null;
  }
}

function encodeKeyBuffer(op) {
  udebug.log("encodeKeyBuffer with keys", op.keys);
  var i, offset, value, nfields, col;
  var record = op.index.record;

  nfields = op.indexHandler.getMappedFieldCount();
  col = op.indexHandler.getColumnMetadata();
  for(i = 0 ; i < nfields ; i++) {
    value = op.keys[i];
    if(value !== null) {
      record.setNotNull(i, op.buffers.key);
      offset = record.getColumnOffset(i);
      if(col.typeConverter) {
        value = col.typeConverter.toDB(value);
      }
      adapter.impl.encoderWrite(col[i], value, op.buffers.key, offset);
    }
    else {
      udebug.log("encodeKeyBuffer ", i, "NULL.");
      record.setNull(i, op.buffers.key);
    }
  }
}


function allocateRowBuffer(op) {
  assert(op.buffers.row === null);
  op.buffers.row = new Buffer(op.tableHandler.dbTable.record.getBufferSize());
}  

function releaseRowBuffer(op) {
  op.buffers.row = null;
}

function encodeRowBuffer(op) {
  udebug.log("encodeRowBuffer");
  var i, offset, value, err;
  var record = op.tableHandler.dbTable.record;
  var nfields = op.tableHandler.getMappedFieldCount();
  udebug.log("encodeRowBuffer nfields", nfields);
  var col = op.tableHandler.getColumnMetadata();
  
  for(i = 0 ; i < nfields ; i++) {  
    value = op.tableHandler.get(op.values, i);
    if(value === null) {
      record.setNull(i, op.buffers.row);
    }
    else if(typeof value !== 'undefined') {
      record.setNotNull(i, op.buffers.row);
      op.columnMask.push(col[i].columnNumber);
      offset = record.getColumnOffset(i);
      if(col[i].typeConverter) {
        value = col[i].typeConverter.toDB(value);
      }
      err = adapter.impl.encoderWrite(col[i], value, op.buffers.row, offset);
      if(err) { udebug.log("encoderWrite: ", err); }
      // FIXME: What to do with this error?
    }
  }
}


function HelperSpec() {
  this.clear();
}

HelperSpec.prototype.clear = function() {
  this[OpHelper.row_buffer]  = null;
  this[OpHelper.key_buffer]  = null;
  this[OpHelper.row_record]  = null;
  this[OpHelper.key_record]  = null;
  this[OpHelper.lock_mode]   = null;
  this[OpHelper.column_mask] = null;
};

var helperSpec = new HelperSpec();

DBOperation.prototype.prepare = function(ndbTransaction) {
  udebug.log("prepare", opcodes[this.opcode], this.values);
  stats.incr("prepared");
  var code = this.opcode;
  var helper;

  /* There is one global helperSpec */
  helperSpec.clear();
  
  /* All operations get a row record */
  helperSpec[OpHelper.row_record] = this.tableHandler.dbTable.record;
  
  /* All but insert use a key.  
  */
  if(code !== 2) {
    allocateKeyBuffer(this);
    encodeKeyBuffer(this);
    helperSpec[OpHelper.key_record]  = this.index.record;
    helperSpec[OpHelper.key_buffer]  = this.buffers.key;
  }

  /* All but delete get an allocated row buffer, and column mask */
  if(code !== 16) {
    allocateRowBuffer(this);
    helperSpec[OpHelper.row_buffer]  = this.buffers.row;
    helperSpec[OpHelper.column_mask] = this.columnMask;

    /* Read gets a lock mode; writes get the data encoded into the row buffer. */
    if(code === 1) {
      helperSpec[OpHelper.lock_mode]  = constants.LockModes[this.lockMode];
    }
    else { 
      encodeRowBuffer(this);
    }
  }
  
  /* Use the HelperSpec and opcode to build the NdbOperation */
  this.ndbop = adapter.impl.DBOperationHelper(helperSpec, code, ndbTransaction);

  this.state = doc.OperationStates[1];  // PREPARED
};


function readResultRow(op) {
  udebug.log("readResultRow");
  var i, offset, value;
  var dbt             = op.tableHandler;
  var record          = dbt.dbTable.record;
  var nfields         = dbt.getMappedFieldCount();
  var col             = dbt.getColumnMetadata();
  var resultRow       = dbt.newResultObject();
  
  for(i = 0 ; i < nfields ; i++) {
    offset  = record.getColumnOffset(i);
  if(record.isNull(i, op.buffers.row)) {
      value = col[i].defaultValue;
    }
    else {
      value = adapter.impl.encoderRead(col[i], op.buffers.row, offset);
      if(col[i].typeConverter) {
        value = col[i].typeConverter.fromDB(value);
      }
    }

    dbt.set(resultRow, i, value);
  }
  op.result.value = resultRow;
}


function buildValueObject(op) {
  udebug.log("buildValueObject");
  var VOC = op.tableHandler.ValueObject; // NDB Value Object Constructor
  var DOC = op.tableHandler.newObjectConstructor;  // User's Domain Object Ctor
  
  if(VOC) {
    /* Turn the buffer into a Value Object */
    op.result.value = new VOC(op.buffers.row);

    /* DBT may have some fieldConverters for this object */
    op.tableHandler.applyFieldConverters(op.result.value);

    /* Finally the user's constructor is called on the new value: */
    if(DOC) {
      DOC.call(op.result.value);
    }
  }
  else {
    /* If there is a good reason to have no VOC, just call readResultRow()... */
    console.log("NO VOC!");
    process.exit();
  }
}


function buildOperationResult(transactionHandler, op, execMode) {
  udebug.log("buildOperationResult");
  var op_ndb_error, result_code;
  
  if(! op.ndbop) {
    op.result.success = false;
    op.result.error = new IndirectError(transactionHandler.error);
    return;
  }
  
  op_ndb_error = op.ndbop.getNdbError();
  result_code = op_ndb_error.code;

  if(execMode !== ROLLBACK) {
    /* Error Handling */
    if(result_code === 0) {
      if(transactionHandler.error) {
        /* This operation has no error, but the transaction failed. */
        udebug.log("Case txErr + opOK", transactionHandler.moniker);
        op.result.success = false;
        op.result.error = new IndirectError(transactionHandler.error);      
      }
      else {
        udebug.log("Case txOK + opOK", transactionHandler.moniker);
        op.result.success = true;
      }
    }
    else {
      /* This operation has an error. */
      op.result.success = false;
      op.result.error = new DBOperationError(op_ndb_error);
      if(transactionHandler.error) {
        udebug.log("Case txErr + OpErr", transactionHandler.moniker);
        if(! transactionHandler.error.cause) {
          transactionHandler.error.cause = op.result.error;
        }
      }
      else {
        if(op.opcode === opcodes.OP_READ || execMode === NOCOMMIT) {
          udebug.log("Case txOK + OpErr [READ | NOCOMMIT]", transactionHandler.moniker);
        }
        else {
          udebug.log("Case txOK + OpErr", transactionHandler.moniker);
          transactionHandler.error = new IndirectError(op.result.error);        
        }
      }
    }

    if(op.result.success && op.opcode === opcodes.OP_READ) {
      // readResultRow(op);
      buildValueObject(op);
    } 
  }
  stats.incr( [ "result_code", result_code ] );
  udebug.log_detail("buildOperationResult finished:", op.result);
}

function completeExecutedOps(dbTxHandler, execMode, operationsList) {
  udebug.log("completeExecutedOps mode:", execMode, 
             "operations: ", operationsList.length);
  var op;
  while(op = operationsList.shift()) {
    assert(op.state === "PREPARED");

    releaseKeyBuffer(op);
    buildOperationResult(dbTxHandler, op, execMode);
    releaseRowBuffer(op);

    dbTxHandler.executedOperations.push(op);
    op.state = doc.OperationStates[2];  // COMPLETED
    if(typeof op.userCallback === 'function') {
      op.userCallback(op.result.error, op);
    }
  }
  udebug.log("completeExecutedOps done");
}


function storeNativeConstructorInMapping(dbTableHandler) {
  var i, nfields, record, fieldNames, typeConverters;
  var VOC, DOC;  // Value Object Constructor, Domain Object Constructor
  if(dbTableHandler.ValueObject) { 
    return;
  }
  /* Step 1: Create Record
     getRecordForMapping(table, ndb, nColumns, columns array)
  */
  nfields = dbTableHandler.fieldNumberToColumnMap.length;
  record = adapter.impl.DBDictionary.getRecordForMapping(
    dbTableHandler.dbTable,
    dbTableHandler.dbTable.per_table_ndb,
    nfields,
    dbTableHandler.fieldNumberToColumnMap
  );

  /* Step 2: Get NdbRecordObject Constructor
    getValueObjectConstructor(record, fieldNames, typeConverters)
  */
  fieldNames = {};
  typeConverters = {};
  for(i = 0 ; i < nfields ; i++) {
    fieldNames[i] = dbTableHandler.resolvedMapping.fields[i].fieldName;
    typeConverters[i] = dbTableHandler.fieldNumberToColumnMap[i].typeConverter;
  }

  VOC = adapter.impl.getValueObjectConstructor(record, fieldNames, typeConverters);

  /* Apply the user's prototype */
  DOC = dbTableHandler.newObjectConstructor;
  if(DOC && DOC.prototype) {
    VOC.prototype = DOC.prototype;
  }

  /* Store the VOC in the mapping */
  dbTableHandler.ValueObject = VOC;
}

function verifyIndexHandler(dbIndexHandler) {
  if(! dbIndexHandler.tableHandler) { throw ("Invalid dbIndexHandler"); }
}

function newReadOperation(tx, dbIndexHandler, keys, lockMode) {
  udebug.log("newReadOperation", keys);
  verifyIndexHandler(dbIndexHandler);
  var op = new DBOperation(opcodes.OP_READ, tx, dbIndexHandler, null);
  op.keys = keys;

  if(! dbIndexHandler.tableHandler.NativeConstructor) {
    storeNativeConstructorInMapping(dbIndexHandler.tableHandler);
  }

  assert(doc.LockModes.indexOf(lockMode) !== -1);
  if(op.index.isPrimaryKey || lockMode === "EXCLUSIVE") {
    op.lockMode = lockMode;
  }
  else {
    op.lockMode = "SHARED";
  }
  return op;
}


function newInsertOperation(tx, tableHandler, row) {
  udebug.log("newInsertOperation");
  var op = new DBOperation(opcodes.OP_INSERT, tx, null, tableHandler);
  op.values = row;
  return op;
}


function newDeleteOperation(tx, dbIndexHandler, keys) {
  udebug.log("newDeleteOperation");
  verifyIndexHandler(dbIndexHandler);
  var op = new DBOperation(opcodes.OP_DELETE, tx, dbIndexHandler, null);
  op.keys = keys;
  return op;
}


function newWriteOperation(tx, dbIndexHandler, row) {
  udebug.log("newWriteOperation");
  verifyIndexHandler(dbIndexHandler);
  var op = new DBOperation(opcodes.OP_WRITE, tx, dbIndexHandler, null);
  op.keys = dbIndexHandler.getFields(row);
  op.values = row;
  return op;
}


function newUpdateOperation(tx, dbIndexHandler, keys, row) {
  udebug.log("newUpdateOperation");
  verifyIndexHandler(dbIndexHandler);
  var op = new DBOperation(opcodes.OP_UPDATE, tx, dbIndexHandler, null);
  op.keys = keys;
  op.values = row;
  return op;
}


exports.DBOperation         = DBOperation;
exports.newReadOperation    = newReadOperation;
exports.newInsertOperation  = newInsertOperation;
exports.newDeleteOperation  = newDeleteOperation;
exports.newUpdateOperation  = newUpdateOperation;
exports.newWriteOperation   = newWriteOperation;
exports.completeExecutedOps = completeExecutedOps;
